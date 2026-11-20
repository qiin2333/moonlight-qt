// Windows: the virtual-file clipboard object, read back through the OLE
// clipboard the way Explorer reads a paste (descriptor, then one stream per
// file). Plain Win32: no Qt, no network; the file contents come from memory.

#include "streaming/filetransfer/virtualfileclipboard_win.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_Failures = 0;

void check(bool value, const char* name)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++g_Failures;
    }
}

// Serves files from memory, a few bytes per read, recording each open.
class MemorySource : public VirtualFiles::Source
{
public:
    std::vector<std::string> files;
    std::vector<std::pair<std::size_t, std::uint64_t>> opens;
    bool fail = false;

    std::unique_ptr<VirtualFiles::Reader> open(std::size_t index, std::uint64_t offset) override
    {
        opens.emplace_back(index, offset);
        return std::make_unique<Reader>(fail ? nullptr : &files.at(index), offset);
    }

private:
    class Reader : public VirtualFiles::Reader
    {
    public:
        Reader(const std::string* data, std::uint64_t offset) : m_Data(data), m_Position(offset) {}
        std::int64_t read(void* buffer, std::uint32_t size) override
        {
            if (m_Data == nullptr) {
                return -1;
            }
            // Short reads: the stream must keep asking until the request is filled.
            const std::uint64_t left =
                m_Data->size() - std::min<std::uint64_t>(m_Position, m_Data->size());
            const std::uint32_t n =
                static_cast<std::uint32_t>(std::min<std::uint64_t>({ left, size, 7 }));
            std::memcpy(buffer, m_Data->data() + m_Position, n);
            m_Position += n;
            return n;
        }

    private:
        const std::string* m_Data;
        std::uint64_t m_Position;
    };
};

CLIPFORMAT format(const wchar_t* name)
{
    return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name));
}

VirtualFiles::Entry entry(const char16_t* path, std::uint64_t size, bool dir = false)
{
    VirtualFiles::Entry e;
    e.path = path;
    e.size = size;
    e.dir = dir;
    e.mtime = 1700000000;
    return e;
}

std::string readStream(IStream* stream, ULONG chunk)
{
    std::string out;
    std::vector<char> buffer(chunk);
    while (true) {
        ULONG got = 0;
        const HRESULT hr = stream->Read(buffer.data(), chunk, &got);
        out.append(buffer.data(), got);
        if (FAILED(hr) || got < chunk) {
            break;
        }
    }
    return out;
}

} // namespace

int main()
{
    if (FAILED(OleInitialize(nullptr))) {
        std::fprintf(stderr, "FAIL: OleInitialize\n");
        return 1;
    }

    auto source = std::make_shared<MemorySource>();
    std::string big(100000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>((i * 31) & 0xFF);
    }
    source->files = { "hello world", "", big };

    check(VirtualFileClipboard::set(
              { entry(u"a.txt", 11), entry(u"dir", 0, true), entry(u"dir\\b.bin", big.size()) },
              source),
          "set puts the files on the clipboard");
    check(VirtualFileClipboard::ownsClipboard(), "owns the clipboard after set");
    check(source->opens.empty(), "nothing is read before a paste");

    IDataObject* clip = nullptr;
    check(SUCCEEDED(OleGetClipboard(&clip)) && clip != nullptr, "OleGetClipboard");
    if (clip == nullptr) {
        return 1;
    }

    // Descriptor
    FORMATETC fd = { format(L"FileGroupDescriptorW"), nullptr, DVASPECT_CONTENT, -1,
                     TYMED_HGLOBAL };
    STGMEDIUM medium = {};
    check(SUCCEEDED(clip->GetData(&fd, &medium)) && medium.tymed == TYMED_HGLOBAL,
          "descriptor available");
    if (medium.hGlobal != nullptr) {
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
        check(group->cItems == 3, "three descriptors");
        check(std::wcscmp(group->fgd[0].cFileName, L"a.txt") == 0 &&
                  group->fgd[0].nFileSizeLow == 11,
              "file name and size");
        check((group->fgd[1].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, "folder attribute");
        check(std::wcscmp(group->fgd[2].cFileName, L"dir\\b.bin") == 0 &&
                  group->fgd[2].nFileSizeLow == big.size() && group->fgd[2].nFileSizeHigh == 0,
              "nested file path and size");
        check((group->fgd[0].dwFlags & FD_PROGRESSUI) && (group->fgd[0].dwFlags & FD_FILESIZE) &&
                  (group->fgd[0].dwFlags & FD_WRITESTIME),
              "descriptor flags");
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }

    // Preferred drop effect
    FORMATETC effect = { format(L"Preferred DropEffect"), nullptr, DVASPECT_CONTENT, -1,
                         TYMED_HGLOBAL };
    medium = {};
    if (SUCCEEDED(clip->GetData(&effect, &medium))) {
        check(*static_cast<DWORD*>(GlobalLock(medium.hGlobal)) == DROPEFFECT_COPY, "paste copies");
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    } else {
        check(false, "preferred drop effect available");
    }

    // Contents
    FORMATETC contents = { format(L"FileContents"), nullptr, DVASPECT_CONTENT, 2, TYMED_ISTREAM };
    medium = {};
    check(SUCCEEDED(clip->GetData(&contents, &medium)) && medium.tymed == TYMED_ISTREAM,
          "stream for a file");
    if (medium.pstm != nullptr) {
        STATSTG stat = {};
        check(SUCCEEDED(medium.pstm->Stat(&stat, STATFLAG_NONAME)) &&
                  stat.cbSize.QuadPart == big.size(),
              "stream size");
        check(readStream(medium.pstm, 4096) == big, "stream bytes");
        ULONG got = 1;
        char byte = 0;
        check(medium.pstm->Read(&byte, 1, &got) == S_FALSE && got == 0,
              "read at the end returns nothing");

        LARGE_INTEGER to;
        to.QuadPart = 50000;
        ULARGE_INTEGER now = {};
        check(SUCCEEDED(medium.pstm->Seek(to, STREAM_SEEK_SET, &now)) && now.QuadPart == 50000,
              "seek");
        check(readStream(medium.pstm, 65536) == big.substr(50000), "read after a seek");
        check(!source->opens.empty() &&
                  source->opens.back() == std::make_pair(std::size_t(2), std::uint64_t(50000)),
              "a seek reopens at the new offset");
        ReleaseStgMedium(&medium);
    }

    contents.lindex = 0;
    medium = {};
    if (SUCCEEDED(clip->GetData(&contents, &medium))) {
        check(readStream(medium.pstm, 3) == "hello world", "small reads");
        ReleaseStgMedium(&medium);
    } else {
        check(false, "stream for the first file");
    }

    contents.lindex = 1;
    medium = {};
    check(FAILED(clip->GetData(&contents, &medium)), "a folder has no contents");
    contents.lindex = -1;
    check(FAILED(clip->GetData(&contents, &medium)), "lindex -1 has no contents");
    contents.lindex = 9;
    check(FAILED(clip->GetData(&contents, &medium)), "an index outside the offer has no contents");

    // A failing transfer is a read error, not a short file.
    source->fail = true;
    contents.lindex = 0;
    medium = {};
    if (SUCCEEDED(clip->GetData(&contents, &medium))) {
        char buffer[16];
        ULONG got = 0;
        check(FAILED(medium.pstm->Read(buffer, sizeof(buffer), &got)) && got == 0,
              "failed source fails the read");
        ReleaseStgMedium(&medium);
    }
    source->fail = false;
    clip->Release();

    // Another program taking the clipboard ends our ownership.
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
    check(!VirtualFileClipboard::ownsClipboard(), "ownership ends when another program copies");

    check(VirtualFileClipboard::set({ entry(u"a.txt", 11) }, source), "set again");
    VirtualFileClipboard::clearIfOwned();
    check(!VirtualFileClipboard::ownsClipboard(), "clear empties the clipboard");
    check(!IsClipboardFormatAvailable(format(L"FileGroupDescriptorW")),
          "no descriptor after clear");

    OleUninitialize();
    if (g_Failures != 0) {
        return 1;
    }
    std::printf("virtual_file_clipboard=passed\n");
    return 0;
}
