#pragma once

#include <QObject>
#include <QAudioSource>
#include <QTimer>
#include <QQueue>

struct OpusEncoder;

class MicStream : public QObject
{
    Q_OBJECT

public:
    explicit MicStream(QObject *parent = nullptr);
    ~MicStream();

    bool start();
    void stop();

private slots:
    void onAudio();
    void sendLoop();
    void logSummary();

private:
    QAudioSource *m_audioInput;
    QIODevice *m_audioDevice;
    OpusEncoder *m_encoder;
    QTimer m_sendTimer;
    QTimer m_logTimer;
    QQueue<QByteArray> m_queue;
    quint16 m_seq;
    quint32 m_timestamp;
    quint32 m_ssrc;

    quint64 m_pcmBytes;
    quint64 m_opusBytes;
    quint64 m_sentBytes;
    int m_sentPackets;
    int m_idleLoops;
    QByteArray m_partialBuffer;
};
