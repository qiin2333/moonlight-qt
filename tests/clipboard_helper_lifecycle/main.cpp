#define SDL_MAIN_HANDLED
#include "streaming/clipboardhelperclient.h"
#include "streaming/clipboardipc.h"
#include "backend/identitymanager.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTextStream>
#include <QThread>
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

// A stuck process API must fail the regression instead of occupying a CI runner
// indefinitely. This watchdog does not depend on the Qt event loop under test.
class TestDeadline
{
public:
    TestDeadline()
        : worker([this] {
              std::unique_lock<std::mutex> lock(mutex);
              if (!finished.wait_for(lock, std::chrono::seconds(60), [this] { return done; })) {
                  std::fputs("FAIL: clipboard helper lifecycle exceeded 60 seconds\n", stderr);
                  std::fflush(stderr);
                  std::_Exit(EXIT_FAILURE);
              }
          })
    {
    }
    ~TestDeadline()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        finished.notify_one();
        worker.join();
    }

private:
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    std::thread worker;
};

// The test exercises process supervision, never the user's paired identity or
// a real streaming connection. Keep those external dependencies inert.
IdentityManager* IdentityManager::get()
{
    return nullptr;
}
QByteArray IdentityManager::getCertificate()
{
    return QByteArray();
}
QByteArray IdentityManager::getPrivateKey()
{
    return QByteArray();
}
extern "C" int LiSendClipboardData(const void*, int)
{
    return 0;
}

class ClipboardHelperClientTest
{
public:
    static bool start(ClipboardHelperClient& client, const QString& mode)
    {
        std::fprintf(stderr, "Starting lifecycle case: %s\n", qPrintable(mode));
        std::fflush(stderr);
        client.m_Enabled = true;
        client.m_Process = new QProcess(&client);
        client.m_Process->start(QCoreApplication::applicationFilePath(), { mode });
        std::fputs("Helper process launched; initializing timer\n", stderr);
        std::fflush(stderr);
        client.m_LastPingTicks = client.m_LastResponseTicks = SDL_GetTicks();
        return client.m_Process->waitForStarted(3000);
    }
    static bool run(QTextStream& err)
    {
        bool ok = true;
        const auto check = [&](bool value, const char* message) {
            if (!value) {
                err << "FAIL: " << message << '\n';
                ok = false;
            }
        };
        {
            ClipboardHelperClient client(nullptr);
            if (!start(client, "--invalid-output"))
                return false;
            check(client.m_Process->waitForFinished(3000), "fake helper exits");
            client.processPendingMessages();
            check(client.m_Process == nullptr && client.m_RestartAttempts == 1,
                  "invalid trailing output schedules one restart without null dereference");
        }
        {
            ClipboardHelperClient client(nullptr);
            if (!start(client, "--hung"))
                return false;
            client.m_LastResponseTicks = SDL_GetTicks() - 30001;
            client.processPendingMessages();
            check(client.m_Process == nullptr && client.m_RestartAttempts == 1,
                  "living but unresponsive helper restarts");
        }
        {
            ClipboardHelperClient client(nullptr);
            if (!start(client, "--hung"))
                return false;
            client.m_ConfigSentTicks = SDL_GetTicks() - 30001;
            client.processProtocolLine(ClipboardIpc::encodePong(1));
            client.processPendingMessages();
            check(client.m_Process == nullptr, "pong cannot hide missing READY timeout");
        }
        {
            ClipboardHelperClient client(nullptr);
            if (!start(client, "--hung"))
                return false;
            client.m_Process->write(
                QByteArray(ClipboardHelperClient::MAX_PENDING_STDIN_BYTES, 'x'));
            check(!client.writeLine(QByteArray(65536, 'y')) && client.m_Process == nullptr,
                  "QProcess internal write backlog counts toward limit");
        }
        {
            ClipboardHelperClient client(nullptr);
            if (!start(client, "--hung"))
                return false;
            const auto frame = ClipboardIpc::encodeLocalFrame(1, QByteArray(60000, 'x'));
            for (int i = 0; i < 20; ++i)
                client.m_StdoutBuffer += frame + '\n';
            client.readHelperOutput();
            check(client.m_Process != nullptr && client.m_StdoutBuffer.isEmpty(),
                  "legal aggregated stdout does not restart helper");
        }
        return ok;
    }
};
int main(int argc, char** argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--invalid-output")) {
        std::puts("invalid JSON");
        return 0;
    }
    if (argc == 2 && !std::strcmp(argv[1], "--hung")) {
        QThread::sleep(15);
        return 0;
    }
    std::fputs("Starting clipboard helper lifecycle regression\n", stderr);
    std::fflush(stderr);
    TestDeadline deadline;
    QCoreApplication app(argc, argv);
    QTextStream err(stderr), out(stdout);
    if (!ClipboardHelperClientTest::run(err))
        return 1;
    out << "clipboard_helper_lifecycle=passed\n";
    return 0;
}
