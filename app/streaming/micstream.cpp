#include "micstream.h"

#include <opus.h>
#include <QtEndian>
#include <QRandomGenerator>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QAudio>
#include <QList>
#include <QDebug>

extern "C" {
#include <Input.h>
}

static const int PCM_FRAME_SAMPLES = 960; // 20 ms at 48 kHz
static const int PCM_FRAME_SIZE = PCM_FRAME_SAMPLES * 2; // mono 16-bit
static const int MAX_OPUS_SIZE = 4000;

// Initialize static variable
bool MicStream::s_MicrophonePermissionGranted = false;

MicStream::MicStream(QObject *parent)
    : QObject(parent),
    m_audioInput(nullptr),
    m_audioDevice(nullptr),
    m_encoder(nullptr),
    m_seq(0),
    m_timestamp(0),
    m_ssrc(0),
    m_pcmBytes(0),
    m_opusBytes(0),
    m_sentBytes(0),
    m_sentPackets(0),
    m_idleLoops(0)
{
    connect(&m_sendTimer, &QTimer::timeout, this, &MicStream::sendLoop);
    m_logTimer.setInterval(5000);
    connect(&m_logTimer, &QTimer::timeout, this, &MicStream::logSummary);
}

MicStream::~MicStream()
{
    stop();
}

bool MicStream::checkMicrophonePermission()
{
    // If we've already successfully granted permission in this session, don't check again
    if (s_MicrophonePermissionGranted) {
        return true;
    }

    // On macOS, we can't directly check microphone permission status with Qt APIs
    // We'll attempt to request permission and handle the result
#ifdef Q_OS_DARWIN
    return requestMicrophonePermission();
#else
    // On other platforms, assume permission is granted if we can create audio devices
    return true;
#endif
}

bool MicStream::requestMicrophonePermission()
{
    // If permission was already granted, return true
    if (s_MicrophonePermissionGranted) {
        return true;
    }

    qInfo() << "[MicStream] Requesting microphone permission";

    // Try to create a temporary audio source to trigger permission request
    QAudioFormat testFormat;
    testFormat.setSampleRate(48000);
    testFormat.setChannelCount(1);
    testFormat.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!device.isNull()) {
        // Create a temporary QAudioSource to trigger permission request
        QAudioSource *tempAudioInput = new QAudioSource(device, testFormat, nullptr);

        // On macOS, we need to be careful about multiple permission requests
        // Try to start the audio source and immediately stop it to test permission
        QIODevice *tempDevice = tempAudioInput->start();

        if (tempDevice && tempAudioInput->error() == QAudio::NoError) {
            // Permission granted successfully
            s_MicrophonePermissionGranted = true;
            qInfo() << "[MicStream] Microphone permission granted";

            // Clean up temporary resources immediately
            tempAudioInput->stop();
            delete tempAudioInput;

            return true;
        } else {
            // Permission denied or error
            qWarning() << "[MicStream] Microphone permission denied or error:" << tempAudioInput->error();
            delete tempAudioInput;
            return false;
        }
    }

    qWarning() << "[MicStream] No audio input device available";
    return false;
}

bool MicStream::start()
{
    if (m_audioInput)
        return false;

    // Check microphone permission before proceeding
    if (!checkMicrophonePermission()) {
        qWarning() << "[MicStream] Cannot start microphone stream: permission not granted";
        return false;
    }

    int err;
    m_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK)
        return false;
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(64000));

    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    // macOS/Qt sometimes returns devices that do not support requested format.
    // Query supported formats and fall back if needed to avoid silent failures.
    QAudioDevice deviceTest = QMediaDevices::defaultAudioInput();
    if (!deviceTest.isFormatSupported(fmt)) {
        qWarning() << "[MicStream] Requested audio format not supported by default device, attempting fallbacks";
        // try common fallbacks
        QAudioFormat fmt2 = fmt;
        fmt2.setSampleRate(44100);
        if (deviceTest.isFormatSupported(fmt2)) {
            fmt = fmt2;
            qInfo() << "[MicStream] Falling back to 44100 Hz";
        } else {
            // try stereo 48000 then
            QAudioFormat fmt3 = fmt;
            fmt3.setChannelCount(2);
            if (deviceTest.isFormatSupported(fmt3)) {
                fmt = fmt3;
                qInfo() << "[MicStream] Falling back to stereo 48000";
            } else {
                qWarning() << "[MicStream] No compatible fallback format found; will still attempt start and log errors";
            }
        }
    }

    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        qWarning() << "[MicStream] No audio input devices available";
    } else {
        qInfo() << "[MicStream] Available audio input devices:";
        for (const QAudioDevice &dev : devices) {
            qInfo() << "  " << dev.description();
        }
    }

    QAudioDevice device = QMediaDevices::defaultAudioInput();
    qInfo() << "[MicStream] Using audio input device:" << device.description();

    // Since we've already checked permission, create the actual audio source
    m_audioInput = new QAudioSource(device, fmt, this);
    // make buffer at least one PCM frame or a few frames to smooth reads
    m_audioInput->setBufferSize(PCM_FRAME_SIZE * 4);

    // Since permission is already granted, this should not trigger another permission request
    m_audioDevice = m_audioInput->start();
    if (!m_audioDevice || m_audioInput->error() != QAudio::NoError) {
        qWarning() << "[MicStream] Failed to start audio device error=" << m_audioInput->error();
        delete m_audioInput;
        m_audioInput = nullptr;
        return false;
    }

    qInfo() << "[MicStream] Audio device initialized successfully";

    connect(m_audioDevice, &QIODevice::readyRead, this, &MicStream::onAudio);

    if (initializeMicrophoneStream() != 0) {
        qWarning() << "[MicStream] initializeMicrophoneStream failed";
        m_audioInput->stop();
        delete m_audioInput;
        m_audioInput = nullptr;
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
        return false;
    }

    m_seq = 0;
    m_timestamp = 0;
    m_ssrc = QRandomGenerator::global()->generate();

    qInfo() << "[MicStream] start";

    m_sendTimer.start(20);
    m_logTimer.start();
    m_pcmBytes = 0;
    m_opusBytes = 0;
    m_sentBytes = 0;
    m_sentPackets = 0;
    m_idleLoops = 0;
    return true;
}

void MicStream::stop()
{
    m_sendTimer.stop();
    m_logTimer.stop();
    logSummary();
    if (m_audioInput) {
        m_audioInput->stop();
        delete m_audioInput;
        m_audioInput = nullptr;
        m_audioDevice = nullptr;
    }
    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }

    destroyMicrophoneStream();
    m_queue.clear();

    qInfo() << "[MicStream] stop";
}

void MicStream::onAudio()
{
    // accumulate partial reads to form complete PCM frames
    if (!m_audioDevice)
        return;

    QByteArray chunk = m_audioDevice->readAll();
    if (chunk.isEmpty()) {
        return;
    }
    // append to partial buffer
    m_partialBuffer.append(chunk);
    while (m_partialBuffer.size() >= PCM_FRAME_SIZE) {
        QByteArray pcm = m_partialBuffer.left(PCM_FRAME_SIZE);
        m_partialBuffer.remove(0, PCM_FRAME_SIZE);
        if (pcm.size() < PCM_FRAME_SIZE) {
            qWarning() << "[MicStream] PCM underrun after assembly read=" << pcm.size() << " expected=" << PCM_FRAME_SIZE;
            break;
        }
        m_pcmBytes += pcm.size();

        unsigned char encoded[MAX_OPUS_SIZE];
        int len = opus_encode(m_encoder,
                              reinterpret_cast<const opus_int16*>(pcm.constData()),
                              PCM_FRAME_SAMPLES,
                              encoded,
                              MAX_OPUS_SIZE);
    if (len > 0) {
            m_queue.enqueue(QByteArray(reinterpret_cast<char*>(encoded), len));
            m_opusBytes += len;
        } else {
            qWarning() << "[MicStream] opus_encode failed len=" << len;
        }
    }
}

void MicStream::sendLoop()
{
    if (m_queue.isEmpty()) {
        m_idleLoops++;
        return;
    }

    while (!m_queue.isEmpty()) {
        QByteArray opus = m_queue.dequeue();
        QByteArray pkt;
        pkt.resize(12 + opus.size());
        pkt[0] = 0x00;
        pkt[1] = 0x61;
        quint16 seqle = qToLittleEndian(m_seq++);
        quint32 tsle = qToLittleEndian(m_timestamp);
        quint32 ssrcle = qToLittleEndian(m_ssrc);
        memcpy(pkt.data() + 2, &seqle, 2);
        memcpy(pkt.data() + 4, &tsle, 4);
        memcpy(pkt.data() + 8, &ssrcle, 4);
        memcpy(pkt.data() + 12, opus.constData(), opus.size());
        int rc = sendMicrophoneData(pkt.constData(), pkt.size());
        if (rc < 0) {
            qWarning() << "[MicStream] sendMicrophoneData failed rc=" << rc;
            continue;
        }
        m_sentPackets++;
        m_sentBytes += opus.size();
        m_timestamp += PCM_FRAME_SAMPLES;
    }
}

void MicStream::logSummary()
{
    qInfo() << "[MicStream] 5s summary pcm=" << m_pcmBytes
            << "B opus=" << m_opusBytes
            << "B sent=" << m_sentPackets << "/" << m_sentBytes
            << "B idle=" << m_idleLoops
            << "queue=" << m_queue.size();
    m_pcmBytes = 0;
    m_opusBytes = 0;
    m_sentBytes = 0;
    m_sentPackets = 0;
    m_idleLoops = 0;
}
