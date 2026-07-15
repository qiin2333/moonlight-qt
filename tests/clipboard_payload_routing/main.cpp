#include "streaming/clipboardsync.h"

#include <QCoreApplication>
#include <QTextStream>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= require(!ClipboardSync::shouldTransferOutOfBand(ClipboardSync::INLINE_THRESHOLD - 1),
                  QStringLiteral("payload below the inline threshold used blob transfer"), err);
    ok &= require(ClipboardSync::shouldTransferOutOfBand(ClipboardSync::INLINE_THRESHOLD),
                  QStringLiteral("payload at the inline threshold did not use blob transfer"), err);
    ok &= require(ClipboardSync::shouldTransferOutOfBand(70 * 1024),
                  QStringLiteral("70 KiB payload did not use blob transfer"), err);
    ok &= require(ClipboardSync::MAX_BLOB_BYTES == 64LL * 1024 * 1024,
                  QStringLiteral("blob size cap changed unexpectedly"), err);

    if (!ok) {
        return 1;
    }

    out << "clipboard_payload_routing=passed\n";
    return 0;
}
