#pragma once

// Files put on the local clipboard whose contents are fetched only when a
// paste target reads them. Plain C++ so the Windows clipboard object
// (virtualfileclipboard_win.cpp) builds without Qt, and the Qt downloader
// (fileoffer.cpp) can be tested on every platform.

#include <cstdint>
#include <memory>
#include <string>

namespace VirtualFiles {

struct Entry
{
    // Relative to the paste target, '\\'-separated, at most MAX_PATH - 1 units.
    std::u16string path;
    std::uint64_t size = 0;
    bool dir = false;
    // Seconds since the Unix epoch; 0 when unknown.
    std::int64_t mtime = 0;
};

// Why a read failed, so the paste target can say so instead of a disk error.
enum class Failure
{
    Other,
    // The host's file is no longer the one that was copied (changed or replaced).
    Changed,
    // The host's file, or its offer, is gone.
    Gone,
    // No data arrived in time.
    Timeout,
    // The connection to the host failed.
    Network,
};

class Reader
{
public:
    virtual ~Reader() = default;

    // Blocks until data, the end of the file or a failure. Returns the bytes
    // copied into `buffer` (at least one), 0 at the end, -1 on failure.
    virtual std::int64_t read(void* buffer, std::uint32_t size) = 0;

    // Why the last read returned -1.
    virtual Failure failure() const { return Failure::Other; }
};

class Source
{
public:
    virtual ~Source() = default;

    // Starts reading entry `index` (a file) from `offset`. Never null.
    virtual std::unique_ptr<Reader> open(std::size_t index, std::uint64_t offset) = 0;
};

} // namespace VirtualFiles
