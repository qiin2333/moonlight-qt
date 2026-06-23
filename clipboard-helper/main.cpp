#include "streaming/clipboardsync.h"
#include "streaming/clipboardipc.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QThread>

#include <cstdio>
#include <iostream>
#include <string>

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

class ClipboardHelperController : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardHelperController(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

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
            m_ClipboardEngine = new ClipboardSync(context, this);
            connect(m_ClipboardEngine, &ClipboardSync::outboundFrame,
                    this, &ClipboardHelperController::sendLocalFrame);
            m_ClipboardEngine->start();
        }
        else {
            m_ClipboardEngine->setHostContext(context);
        }

        writeProtocolLine(ClipboardIpc::encodeReady(sequence));
    }

    void sendLocalFrame(const QByteArray& frame)
    {
        writeProtocolLine(ClipboardIpc::encodeLocalFrame(m_NextSequence++, frame));
    }

    void writeProtocolLine(const QByteArray& line)
    {
        QByteArray out = line;
        out.append('\n');
        size_t written = fwrite(out.constData(), 1, static_cast<size_t>(out.size()), stdout);
        fflush(stdout);
        if (written != static_cast<size_t>(out.size())) {
            QCoreApplication::quit();
        }
    }

    ClipboardSync* m_ClipboardEngine = nullptr;
    quint32 m_NextSequence = 1;
};

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Moonlight Clipboard Helper"));
    QCoreApplication::setOrganizationName(QStringLiteral("Moonlight Game Streaming Project"));

    ClipboardHelperController controller;
    StdinReaderThread stdinThread;
    QObject::connect(&stdinThread, &StdinReaderThread::lineReceived,
                     &controller, &ClipboardHelperController::handleLine,
                     Qt::QueuedConnection);

    stdinThread.start();
    int rc = app.exec();
    if (!stdinThread.wait(1000)) {
        stdinThread.terminate();
        stdinThread.wait();
    }
    return rc;
}

#include "main.moc"
