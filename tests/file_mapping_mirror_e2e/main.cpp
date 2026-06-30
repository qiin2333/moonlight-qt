#include "mount/macos_finder_mirror_provider.h"
#include "mount/mount_coordinator.h"
#include "vfs/remote_vfs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTextStream>

using namespace FileMapping;

namespace {
class FakeRemoteVfs : public RemoteVfs
{
public:
    FakeRemoteVfs()
    {
        addDirectory(VfsItemId::root(), QStringLiteral("mapping-docs"), QStringLiteral("Documents"));
        addDirectory(id(QStringLiteral("mapping-docs")), QStringLiteral("nested"), QStringLiteral("nested"));
        addFile(id(QStringLiteral("mapping-docs")), QStringLiteral("hello"), QStringLiteral("hello.txt"), "hello from host\n");
        addFile(id(QStringLiteral("nested")), QStringLiteral("deep"), QStringLiteral("deep.txt"), "deep contents\n");
        addFile(id(QStringLiteral("mapping-docs")), QStringLiteral("bad-name"), QStringLiteral("bad:name?.txt"), "safe name\n");

        VfsItem large;
        large.id = id(QStringLiteral("large"));
        large.parentId = id(QStringLiteral("mapping-docs"));
        large.displayName = QStringLiteral("large.bin");
        large.size = 2ULL * 1024ULL * 1024ULL;
        m_Items.insert(large.id.value, large);
        m_Children[large.parentId.value].append(large);
    }

    ChildrenResult children(const VfsItemId& parentId) override
    {
        ChildrenResult result;
        result.items = m_Children.value(parentId.value);
        return result;
    }

    ItemResult item(const VfsItemId& itemId) override
    {
        ItemResult result;
        if (!m_Items.contains(itemId.value)) {
            result.error = Error::make(ErrorKind::NotFound, QStringLiteral("missing fake item"));
            return result;
        }
        result.item = m_Items.value(itemId.value);
        return result;
    }

    OpenResult open(const VfsItemId& itemId) override
    {
        OpenResult result;
        if (!m_Items.contains(itemId.value) || m_Items.value(itemId.value).directory) {
            result.error = Error::make(ErrorKind::NotFound, QStringLiteral("missing fake file"));
            return result;
        }
        result.handle.id = ++m_NextHandleId;
        result.handle.item = m_Items.value(itemId.value);
        return result;
    }

    ReadResult read(const ReadHandle& handle, quint64 offset, quint32 length) override
    {
        ReadResult result;
        const QByteArray data = m_Data.value(handle.item.id.value);
        if (offset >= static_cast<quint64>(data.size())) {
            return result;
        }
        result.data = data.mid(static_cast<int>(offset), static_cast<int>(length));
        return result;
    }

    void close(const ReadHandle&) override
    {
    }

private:
    static VfsItemId id(const QString& value)
    {
        return { value };
    }

    void addDirectory(const VfsItemId& parentId, const QString& itemId, const QString& name)
    {
        VfsItem item;
        item.id = id(itemId);
        item.parentId = parentId;
        item.displayName = name;
        item.directory = true;
        m_Items.insert(item.id.value, item);
        m_Children[parentId.value].append(item);
    }

    void addFile(const VfsItemId& parentId, const QString& itemId, const QString& name, const QByteArray& data)
    {
        VfsItem item;
        item.id = id(itemId);
        item.parentId = parentId;
        item.displayName = name;
        item.size = static_cast<quint64>(data.size());
        m_Items.insert(item.id.value, item);
        m_Children[parentId.value].append(item);
        m_Data.insert(item.id.value, data);
    }

    QHash<QString, VfsItem> m_Items;
    QHash<QString, QList<VfsItem>> m_Children;
    QHash<QString, QByteArray> m_Data;
    quint64 m_NextHandleId = 0;
};

bool readAll(const QString& path, QByteArray& out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    out = file.readAll();
    return true;
}

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
    QCoreApplication::setOrganizationName(QStringLiteral("Moonlight Test"));
    QCoreApplication::setApplicationName(QStringLiteral("file-mapping-mirror-e2e"));
    qputenv("MOONLIGHT_FILE_MAPPING_MIRROR_MAX_FILE_MB", "1");

    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList args = QCoreApplication::arguments();
    const bool keepSnapshot = args.contains(QStringLiteral("--keep"));
    const bool revealSnapshot = args.contains(QStringLiteral("--reveal"));

    auto vfs = std::make_shared<FakeRemoteVfs>();
    MountRequest request;
    request.hostUuid = QStringLiteral("fake-host");
    request.hostName = QStringLiteral("Fake Host");
    request.sessionId = QStringLiteral("e2e-session");
    request.vfs = vfs;

    auto provider = std::make_shared<MacOSFinderMirrorProvider>();
    MountCoordinator coordinator({ provider });
    MountResult result = coordinator.ensureMounted(request);
    if (!require(result.ok(), QStringLiteral("mount failed: %1").arg(result.error.message), err)) {
        return 1;
    }
    if (!require(result.status.state == MountState::Mounted, QStringLiteral("mount did not report Mounted"), err)) {
        return 1;
    }

    const QString rootPath = result.status.displayPath;
    QByteArray data;
    const QString docs = QDir(rootPath).filePath(QStringLiteral("Documents"));
    bool ok = true;
    ok &= require(QFileInfo::exists(QDir(rootPath).filePath(QStringLiteral("README.txt"))), QStringLiteral("README missing"), err);
    ok &= require(QFileInfo(docs).isDir(), QStringLiteral("Documents folder missing"), err);
    ok &= require(readAll(QDir(docs).filePath(QStringLiteral("hello.txt")), data) && data == QByteArray("hello from host\n"), QStringLiteral("hello.txt contents mismatch"), err);
    ok &= require(readAll(QDir(docs).filePath(QStringLiteral("nested/deep.txt")), data) && data == QByteArray("deep contents\n"), QStringLiteral("nested/deep.txt contents mismatch"), err);
    ok &= require(readAll(QDir(docs).filePath(QStringLiteral("bad_name_.txt")), data) && data == QByteArray("safe name\n"), QStringLiteral("sanitized filename missing"), err);
    ok &= require(QFileInfo::exists(QDir(docs).filePath(QStringLiteral("large.bin.moonlight-skipped.txt"))), QStringLiteral("large file skip marker missing"), err);
    ok &= require(QFileInfo::exists(QDir(rootPath).filePath(QStringLiteral("Moonlight skipped files.txt"))), QStringLiteral("warnings file missing"), err);

    if (revealSnapshot) {
        MountError reveal = provider->reveal(result.id);
        ok &= require(reveal.ok(), QStringLiteral("reveal failed: %1").arg(reveal.message), err);
        out << "reveal=" << (reveal.ok() ? QStringLiteral("passed") : QStringLiteral("failed")) << '\n';
    }

    if (!keepSnapshot) {
        coordinator.unmount(request.hostUuid, request.sessionId);
        ok &= require(!QFileInfo::exists(rootPath), QStringLiteral("snapshot folder was not removed on unmount"), err);
    }

    if (!ok) {
        return 1;
    }

    out << "file_mapping_mirror_e2e=passed\n";
    out << "snapshot_path=" << rootPath << '\n';
    out << "kept=" << (keepSnapshot ? QStringLiteral("true") : QStringLiteral("false")) << '\n';
    return 0;
}
