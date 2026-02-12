#include "micstream.h"
#include "macpermissions.h"

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

// Internal moonlight-common-c functions for microphone streaming
int initializeMicrophoneStream(void);
void destroyMicrophoneStream(void);
int sendMicrophoneOpusData(const unsigned char* opusData, int opusLength);
}

static const int PCM_FRAME_SAMPLES = 960; // 20 ms at 48 kHz
static const int PCM_FRAME_SIZE = PCM_FRAME_SAMPLES * 2; // mono 16-bit
static const int MAX_OPUS_SIZE = 4000;

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

bool MicStream::start()
{
    if (m_audioInput)
        return false;

    // On macOS, request microphone permission once via AVCaptureDevice
    // *before* touching QMediaDevices / CoreAudio to avoid multiple
    // TCC dialogs (especially with universal binaries).
    if (!checkAndRequestMicrophonePermission()) {
        qWarning() << "[MicStream] Microphone permission denied";
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

    // Obtain the default input device once and reuse it for format
    // checks, logging, and the actual QAudioSource to avoid redundant
    // QMediaDevices calls that can each trigger a macOS TCC prompt.
    QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        qWarning() << "[MicStream] No default audio input device available";
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
        return false;
    }

    if (!device.isFormatSupported(fmt)) {
        qWarning() << "[MicStream] Requested audio format not supported by default device, attempting fallbacks";
        // try common fallbacks
        QAudioFormat fmt2 = fmt;
        fmt2.setSampleRate(44100);
        if (device.isFormatSupported(fmt2)) {
            fmt = fmt2;
            qInfo() << "[MicStream] Falling back to 44100 Hz";
        } else {
            // try stereo 48000 then
            QAudioFormat fmt3 = fmt;
            fmt3.setChannelCount(2);
            if (device.isFormatSupported(fmt3)) {
                fmt = fmt3;
                qInfo() << "[MicStream] Falling back to stereo 48000";
            } else {
                qWarning() << "[MicStream] No compatible fallback format found; will still attempt start and log errors";
            }
        }
    }

    qInfo() << "[MicStream] Using audio input device:" << device.description();

    m_audioInput = new QAudioSource(device, fmt, this);
    // make buffer at least one PCM frame or a few frames to smooth reads
    m_audioInput->setBufferSize(PCM_FRAME_SIZE * 4);
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
        // sendMicrophoneOpusData handles RTP header and encryption internally
        int rc = sendMicrophoneOpusData(reinterpret_cast<const unsigned char*>(opus.constData()), opus.size());
        if (rc < 0) {
            qWarning() << "[MicStream] sendMicrophoneOpusData failed rc=" << rc;
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
