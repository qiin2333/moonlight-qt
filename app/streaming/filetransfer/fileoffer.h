#pragma once

// Files the host copied, offered through the clipboard (wire kind 4).
//
// The host agent registers copied paths with Sunshine and sends the offer it
// gets back, a UTF-8 JSON object:
//
//   {"version": 2, "id": "<64 hex>", "type": "file" | "files",
//    "source": "clipboard" | "send", "total_size": N, "expires_in": S,
//    "files": [{"path": "dir/a.txt", "size": N, "dir": false, "mtime": T}, ...]}
//
// The first, single-file version of the protocol sent only
//   {"id": "<64 hex>", "type": "file", "name", "size", "mime", "download_url"}
// with the file at GET /api/v1/file-transfer/<id>; such an offer is taken as
// a list of that one file, fetched from there.
//
// Paths are relative and '/'-separated, folders before what they contain.
// Each file is fetched from GET /api/v1/file-transfer/<id>/<index> on the
// paired HTTPS port, only when a paste target reads it.

#include "streaming/clipboardsync.h"
#include "virtualfilesource.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <deque>
#include <memory>
#include <vector>

struct FileOffer
{
    // Bounds shared with Sunshine (src/file_transfer.h).
    static constexpr int MAX_ENTRIES = 10000;
    static constexpr int MAX_PATH_UNITS = 259;

    QString id;
    QString source; // "clipboard" (a copy) or "send" ("send to client")
    // A single-file offer of the first version: its file is at /api/v1/file-transfer/<id>.
    bool singleFileRoute = false;
    quint64 totalSize = 0;
    std::vector<VirtualFiles::Entry> entries;

    // Validates everything a paste target would trust: ids, sizes and every
    // path component (no traversal, no names Windows reserves or rejects, no
    // entry whose folder was not listed before it, no duplicates).
    static bool parse(const QByteArray& json, FileOffer& out, QString* error = nullptr);
};

// Fetches the files of one offer from Sunshine on a thread of its own, so a
// paste target can block in Reader::read() on any thread, the GUI thread
// included (Windows delivers a paste's stream reads there).
class FileOfferDownloader : public VirtualFiles::Source
{
public:
    FileOfferDownloader(const ClipboardSyncHostContext& host, const FileOffer& offer);
    ~FileOfferDownloader() override;

    std::unique_ptr<VirtualFiles::Reader> open(std::size_t index, std::uint64_t offset) override;

    // Shared with readers so a stream outlives the clipboard object that made it.
    struct Core;

private:
    std::shared_ptr<Core> m_Core;
};

class QFileInfo;

// Files copied on this client, offered to the host the other way round: the
// offer above goes to the host (wire kind 4) and the host's agent pastes it by
// asking for one chunk at a time (wire kind 5, a FilePull). The client answers
// each with POST /api/v1/file-transfer/chunk/<ticket> on the paired HTTPS port.
// Nothing is read before the host pastes.
class LocalFileOffers
{
public:
    // Offers kept at once; adding another forgets the oldest.
    static constexpr int MAX_OFFERS = 4;
    // How long an offer can still be pasted on the host.
    static constexpr qint64 TTL_MS = 60LL * 60 * 1000;
    // Largest chunk the host may ask for (Sunshine's file_transfer_pull::kMaxChunk).
    static constexpr quint64 MAX_CHUNK = 4ULL * 1024 * 1024;

    LocalFileOffers();

    // Lists `paths` (absolute local paths; a folder with everything below it,
    // parents first, symbolic links and junctions left out) and remembers the
    // result under a new id. On success `json` is the offer to send.
    bool add(const QStringList& paths, QByteArray& json, QString* error = nullptr);

    struct Chunk
    {
        QByteArray bytes;
        // Empty on success, otherwise the word sent to the host:
        // "not_found" (no such offer or entry), "gone" (cannot open or read),
        // "changed" (its size or modification time differs from the offer, so it
        // may be another file), "range" (offset past the end).
        QByteArray error;
    };
    // Reads at most `length` bytes of entry `index` from `offset`.
    Chunk read(const QString& id, qint64 index, quint64 offset, quint64 length);

    void clear();

private:
    struct Item
    {
        QString localPath;
        quint64 size = 0;
        qint64 mtimeMs = 0; // with the size, what tells a replaced or edited file
        bool dir = false;
    };
    struct Offer
    {
        QString id;
        std::vector<Item> items;
        qint64 createdMs = 0;
    };
    void expire();
    static bool isLink(const QFileInfo& info);
    // False when the folder cannot be listed (an empty folder can).
    static bool canList(const QString& folder);

    std::deque<Offer> m_Offers;
    QElapsedTimer m_Clock;
};

// The host's request for a chunk of a local offer (wire kind 5):
// {"ticket": "<32 hex>", "id": "<uuid>", "index": N, "offset": N, "length": N}
struct FilePull
{
    QString ticket;
    QString id;
    qint64 index = 0;
    quint64 offset = 0;
    quint64 length = 0;

    static bool parse(const QByteArray& json, FilePull& out);
};
