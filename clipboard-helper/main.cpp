#include "streaming/clipboardsync.h"
#include "streaming/clipboardipc.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslKey>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef Q_OS_MACOS
extern "C" void ClipboardHelperSetBackgroundActivationPolicy();
#endif

class StdinReaderThread : public QThread
{
    Q_OBJECT

signals:
    void lineReceived(QByteArray line);

protected:
    void run() override
    {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            emit lineReceived(QByteArray(line.data(), static_cast<int>(line.size())));
        }
        emit lineReceived(ClipboardIpc::encodeStop(0));
    }
};

// Single owner of stdout. Every protocol line goes through this thread so a
// wedged GUI thread (a clipboard write blocked on pasteboard contention)
// cannot stall host-bound traffic: outbound frames are enqueued from the
// snapshot thread directly, never crossing the GUI event loop. The queue is
// bounded by total bytes, symmetric with the session's stdin backlog cap.
class ProtocolWriter
{
public:
    explicit ProtocolWriter()
    {
        m_Thread = std::thread([this] { run(); });
    }
    ~ProtocolWriter() { stopAndDrain(); }

    // Thread-safe. Returns false when the line was dropped: the backlog cap
    // was hit, or the writer already failed (queued lines would never be
    // flushed, so callers must not count on delivery).
    bool write(QByteArray line)
    {
        line += '\n';
        bool ok = true;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_Failed) {
                ok = false;
            } else if (m_TotalBytes + line.size() > MAX_PENDING_BYTES) {
                m_Failed = true;
                ok = false;
            } else {
                m_Queue.push_back(std::move(line));
                m_TotalBytes += m_Queue.back().size();
            }
        }
        m_Condition.notify_one();
        return ok;
    }

    bool failed() const { return m_Failed; }

    void stopAndDrain()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_Stopping) {
                return;
            }
            m_Stopping = true;
        }
        m_Condition.notify_all();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
    }

private:
    static constexpr size_t MAX_PENDING_BYTES = 4 * 1024 * 1024;

    void run()
    {
        for (;;) {
            QByteArray line;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Condition.wait(lock, [this] { return m_Stopping || !m_Queue.empty(); });
                if (!m_Queue.empty()) {
                    line = std::move(m_Queue.front());
                    m_Queue.pop_front();
                    m_TotalBytes -= line.size();
                } else if (m_Stopping) {
                    return;
                }
            }
            size_t written = fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
            // One flush, checked: a first unchecked flush could discard the
            // buffered line while a second flush reports success (UCRT clears
            // the buffer even when the underlying write failed).
            const bool flushed = fflush(stdout) == 0;
            if (written != static_cast<size_t>(line.size()) || !flushed) {
                // Reader is gone; nothing else can be delivered.
                m_Failed = true;
                QCoreApplication::quit();
                return;
            }
        }
    }

    std::thread m_Thread;
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    std::deque<QByteArray> m_Queue;
    size_t m_TotalBytes = 0;
    bool m_Stopping = false;
    std::atomic<bool> m_Failed{ false };
};

class ClipboardHelperController : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardHelperController(QObject* parent = nullptr)
        : QObject(parent)
    {
        // The engine's dedicated thread. The GUI main thread keeps only the
        // event loop and never touches the clipboard again.
        m_WorkerThread = new QThread(this);
        m_WorkerThread->setObjectName("clipboard-engine");
        m_WorkerThread->start();
    }
    ~ClipboardHelperController() override
    {
        m_WorkerThread->quit();
        m_WorkerThread->wait(2000);
    }

    QThread* workerThread() const { return m_WorkerThread; }

    void setWriter(ProtocolWriter* writer) { m_Writer = writer; }

public slots:
    void handleLine(const QByteArray& line)
    {
        ClipboardIpc::Message message;
        QString error;
        if (!ClipboardIpc::decodeLine(line, message, error)) {
            writeProtocolLine(ClipboardIpc::encodeError(0, QStringLiteral("bad-message"), error));
            return;
        }

        if (message.type == ClipboardIpc::MessageType::Stop) {
            QCoreApplication::quit();
            return;
        }
        if (message.type == ClipboardIpc::MessageType::Ping) {
            writeProtocolLine(ClipboardIpc::encodePong(message.sequence));
            return;
        }

        if (message.type == ClipboardIpc::MessageType::Configure) {
            handleConfig(message.sequence, message.config);
            return;
        }

        if (message.type == ClipboardIpc::MessageType::HostFrame) {
            if (m_ClipboardEngine == nullptr) {
                writeProtocolLine(ClipboardIpc::encodeError(message.sequence,
                                                            QStringLiteral("not-ready"),
                                                            QStringLiteral("clipboard sync is not configured")));
                return;
            }
            m_ClipboardEngine->handleIncomingFrame(message.frame.constData(), message.frame.size());
            return;
        }

        writeProtocolLine(ClipboardIpc::encodeError(message.sequence,
                                                    QStringLiteral("unexpected-message"),
                                                    QStringLiteral("message type is not valid for helper input")));
    }

private:
    void handleConfig(quint32 sequence, const ClipboardIpc::HostConfig& config)
    {
        if (config.address.isEmpty() || config.httpsPort == 0 ||
                config.serverCertPem.isEmpty() ||
                config.clientCertPem.isEmpty() ||
                config.clientKeyPem.isEmpty()) {
            writeProtocolLine(ClipboardIpc::encodeError(sequence,
                                                        QStringLiteral("bad-config"),
                                                        QStringLiteral("missing host address, port, or certificate data")));
            return;
        }

        ClipboardSyncHostContext context;
        context.address = config.address;
        context.httpsPort = config.httpsPort;
        context.serverCertificate = QSslCertificate(config.serverCertPem);
        context.clientCertificate = QSslCertificate(config.clientCertPem);
        context.clientPrivateKey = QSslKey(config.clientKeyPem, QSsl::Rsa);
        if (context.serverCertificate.isNull() ||
                context.clientCertificate.isNull() ||
                context.clientPrivateKey.isNull()) {
            writeProtocolLine(ClipboardIpc::encodeError(sequence,
                                                        QStringLiteral("bad-config"),
                                                        QStringLiteral("certificate or private key data is unreadable")));
            return;
        }

        if (m_ClipboardEngine == nullptr) {
            // The engine lives on its own thread for its whole life: inbound
            // clipboard writes happen there, so pasteboard contention can
            // never wedge the GUI thread (which only runs the event loop).
            auto* engine = new ClipboardSync(context);
            adoptEngine(engine, m_WorkerThread);
            connect(m_WorkerThread, &QThread::finished, engine, &QObject::deleteLater);
            // Outbound frames are produced on the engine thread and enqueued
            // onto the writer directly (the writer owns stdout).
            connect(engine, &ClipboardSync::outboundFrame, this,
                    &ClipboardHelperController::sendLocalFrame, Qt::DirectConnection);
            postToEngine([engine] { engine->start(); });
        }
        else {
            postToEngine(
                [engine = m_ClipboardEngine, context] { engine->setHostContext(context); });
        }

        writeProtocolLine(ClipboardIpc::encodeReady(sequence));
    }

    void sendLocalFrame(const QByteArray& frame)
    {
        writeProtocolLine(ClipboardIpc::encodeLocalFrame(m_NextSequence++, frame));
    }

    void writeProtocolLine(const QByteArray& line)
    {
        if (m_Writer != nullptr && !m_Writer->write(line)) {
            QCoreApplication::quit();
        }
    }

    // Move the engine onto its own thread (no parent, or moveToThread
    // refuses). All engine methods are then invoked through queued functors
    // targeting the engine, so they execute on that thread — including the
    // clipboard writes whose blocking is what this refactor defends against.
    void adoptEngine(ClipboardSync* engine, QThread* worker)
    {
        engine->moveToThread(worker);
        m_ClipboardEngine = engine;
    }

    template <typename Fn> void postToEngine(Fn&& fn)
    {
        if (m_ClipboardEngine == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            m_ClipboardEngine,
            [engine = QPointer<ClipboardSync>(m_ClipboardEngine),
             fn = std::forward<Fn>(fn)]() mutable {
                if (engine != nullptr) {
                    fn();
                }
            },
            Qt::QueuedConnection);
    }

    ProtocolWriter* m_Writer = nullptr;
    QThread* m_WorkerThread = nullptr;
    ClipboardSync* m_ClipboardEngine = nullptr;
    // Touched from snapshot threads (inline frames) and the GUI thread (REF
    // callbacks), so the increment must be atomic.
    std::atomic<quint32> m_NextSequence{ 1 };
};

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
#ifdef Q_OS_MACOS
    ClipboardHelperSetBackgroundActivationPolicy();
#endif
    QCoreApplication::setApplicationName(QStringLiteral("Moonlight Clipboard Helper"));
    QCoreApplication::setOrganizationName(QStringLiteral("Moonlight Game Streaming Project"));

    ProtocolWriter writer;
    ClipboardHelperController controller;
    controller.setWriter(&writer);
    StdinReaderThread stdinThread;
    // Bound the reader to one GUI delivery at a time. Otherwise a blocked
    // clipboard provider lets the worker enqueue an unbounded number of frames.
    QObject::connect(&stdinThread, &StdinReaderThread::lineReceived, &controller,
                     &ClipboardHelperController::handleLine, Qt::BlockingQueuedConnection);

    stdinThread.start();
    int rc = app.exec();
    // Flush any protocol lines still queued, stop the writer, then wind the
    // engine thread down (the queued engine stop is best-effort at exit).
    writer.stopAndDrain();
    if (controller.workerThread() != nullptr) {
        controller.workerThread()->quit();
        if (!controller.workerThread()->wait(2000)) {
            // The engine thread is stuck in a clipboard call (the stalled-
            // pasteboard case). Unwinding would destroy a running QThread
            // and abort via qFatal; exit immediately instead — the OS
            // reclaims the stuck thread and the writer has already drained.
            std::fflush(nullptr);
            std::exit(rc);
        }
    }
    if (!stdinThread.wait(1000)) {
        stdinThread.terminate();
        stdinThread.wait();
    }
    // A writer failure means protocol output was lost; surface it in the
    // exit status so the session log shows an abnormal helper termination.
    if (rc == 0 && writer.failed()) {
        rc = 3;
    }
    return rc;
}

#include "main.moc"
