#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>

class NvComputer;
class QProcess;

class ClipboardHelperClient : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardHelperClient(NvComputer* computer, QObject* parent = nullptr);
    ~ClipboardHelperClient() override;

    void start();
    void stop();
    void updateHostContext();

    // Thread-safe. Called from the Limelight control receive thread.
    void handleIncomingFrame(const char* data, int length);

    // Called from the SDL streaming loop on the session/main thread.
    void processPendingMessages();

    bool isRunning() const;

private:
    static constexpr int MAX_QUEUED_HOST_FRAMES = 32;
    static constexpr int MAX_PENDING_STDIN_BYTES = 4 * 1024 * 1024;
    static constexpr int MAX_RESTART_ATTEMPTS = 3;
    static constexpr quint32 RESTART_DELAY_MS = 1000;

    QString findHelperExecutable() const;
    bool sendCurrentConfig();
    bool startProcess();
    void scheduleRestart(const char* reason);
    void maybeRestart();
    void flushQueuedHostFrames();
    void readHelperOutput();
    void readHelperErrors();
    void logHelperStderrLine(const QByteArray& line);
    void processProtocolLine(const QByteArray& line);
    void handleProtocolError(const QString& error);
    void restartHelper(const char* reason);
    bool flushProcessInput();
    bool writeLine(const QByteArray& line);
    quint32 nextSequence();

    NvComputer* m_Computer;
    QProcess* m_Process;
    QString m_HelperPath;
    QMutex m_InboundMutex;
    QQueue<QByteArray> m_InboundFrames;
    QByteArray m_StdoutBuffer;
    QByteArray m_StderrBuffer;
    QByteArray m_StdinBuffer;
    bool m_Enabled;
    bool m_Disabled;
    bool m_StopRequested;
    bool m_HelperReady;
    quint32 m_ConfigSequence;
    quint32 m_NextSequence;
    quint32 m_NextRestartTicks;
    int m_RestartAttempts;
    int m_DroppedInboundFrames;
};
