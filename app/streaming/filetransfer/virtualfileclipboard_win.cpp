#include "virtualfileclipboard_win.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

// IDataObjectAsyncCapability from shobjidl_core.h, declared here with its IID
// because MinGW's headers lack it. The method order is the interface's.
const IID IID_AsyncCapability = {
    0x3D8B0590, 0xF691, 0x11d2, { 0x8E, 0xA9, 0x00, 0x60, 0x97, 0xDF, 0x5B, 0xD4 }
};

struct IAsyncCapability : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL doOpAsync) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* isOpAsync) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx* reserved) = 0;
    virtual HRESULT STDMETHODCALLTYPE InOperation(BOOL* inAsyncOp) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx* reserved,
                                                   DWORD effects) = 0;
};

CLIPFORMAT descriptorFormat()
{
    static const CLIPFORMAT cf =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    return cf;
}

CLIPFORMAT contentsFormat()
{
    static const CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    return cf;
}

CLIPFORMAT dropEffectFormat()
{
    static const CLIPFORMAT cf =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect"));
    return cf;
}

FILETIME unixToFileTime(std::int64_t seconds)
{
    // FILETIME counts 100 ns intervals since 1601-01-01.
    const ULONGLONG ticks = static_cast<ULONGLONG>(seconds + 11644473600LL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
}

std::wstring toWide(const std::u16string& s)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t is UTF-16");
    return std::wstring(reinterpret_cast<const wchar_t*>(s.data()), s.size());
}

// One file's contents, read from the source when the paste target reads it.
class FileStream final : public IStream
{
public:
    FileStream(std::shared_ptr<VirtualFiles::Source> source, std::size_t index,
               const VirtualFiles::Entry& entry)
        : m_Source(std::move(source)), m_Index(index), m_Size(entry.size)
    {
        const std::wstring path = toWide(entry.path);
        const auto slash = path.find_last_of(L'\\');
        m_Name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_Refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG size, ULONG* read) override
    {
        if (read != nullptr) {
            *read = 0;
        }
        if (buffer == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        // A short read means the end of the stream to IStream callers, so
        // fill the whole request unless the file ends or the transfer fails.
        ULONG total = 0;
        while (total < size && m_Position < m_Size) {
            if (!m_Reader || m_ReaderPosition != m_Position) {
                m_Reader = m_Source->open(m_Index, m_Position);
                m_ReaderPosition = m_Position;
            }
            const std::uint64_t remaining = m_Size - m_Position;
            const ULONG want = static_cast<ULONG>(std::min<std::uint64_t>(size - total, remaining));
            const std::int64_t got = m_Reader->read(static_cast<char*>(buffer) + total, want);
            if (got < 0) {
                m_Reader.reset();
                if (read != nullptr) {
                    *read = total;
                }
                return STG_E_READFAULT;
            }
            if (got == 0) {
                // The source ended before the announced size: the file is incomplete.
                m_Reader.reset();
                if (read != nullptr) {
                    *read = total;
                }
                return STG_E_READFAULT;
            }
            total += static_cast<ULONG>(got);
            m_Position += static_cast<std::uint64_t>(got);
            m_ReaderPosition = m_Position;
        }
        if (read != nullptr) {
            *read = total;
        }
        return total == size ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* written) override
    {
        if (written != nullptr) {
            *written = 0;
        }
        return STG_E_ACCESSDENIED;
    }

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* newPosition) override
    {
        LONGLONG base = 0;
        switch (origin) {
        case STREAM_SEEK_SET:
            base = 0;
            break;
        case STREAM_SEEK_CUR:
            base = static_cast<LONGLONG>(m_Position);
            break;
        case STREAM_SEEK_END:
            base = static_cast<LONGLONG>(m_Size);
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        const LONGLONG target = base + move.QuadPart;
        if (target < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        // Positions past the end are legal; reads there return nothing.
        m_Position = std::min<std::uint64_t>(static_cast<std::uint64_t>(target), m_Size);
        if (newPosition != nullptr) {
            newPosition->QuadPart = m_Position;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER size, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override
    {
        if (target == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        static constexpr ULONG CHUNK = 256 * 1024;
        std::unique_ptr<char[]> buffer(new char[CHUNK]);
        ULONGLONG left = size.QuadPart;
        ULONGLONG totalRead = 0;
        ULONGLONG totalWritten = 0;
        HRESULT hr = S_OK;
        while (left > 0) {
            ULONG got = 0;
            hr = Read(buffer.get(), static_cast<ULONG>(std::min<ULONGLONG>(left, CHUNK)), &got);
            if (FAILED(hr) || got == 0) {
                break;
            }
            totalRead += got;
            ULONG put = 0;
            hr = target->Write(buffer.get(), got, &put);
            totalWritten += put;
            if (FAILED(hr) || put != got) {
                break;
            }
            left -= got;
            if (hr == S_FALSE) {
                break;
            }
        }
        if (read != nullptr) {
            read->QuadPart = totalRead;
        }
        if (written != nullptr) {
            written->QuadPart = totalWritten;
        }
        return FAILED(hr) ? hr : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override
    {
        if (stat == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = m_Size;
        stat->grfMode = STGM_READ;
        if ((flags & STATFLAG_NONAME) == 0) {
            const size_t bytes = (m_Name.size() + 1) * sizeof(wchar_t);
            auto* name = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
            if (name == nullptr) {
                return E_OUTOFMEMORY;
            }
            std::memcpy(name, m_Name.c_str(), bytes);
            stat->pwcsName = name;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override
    {
        if (out != nullptr) {
            *out = nullptr;
        }
        return E_NOTIMPL;
    }

private:
    ~FileStream() = default;

    std::atomic<ULONG> m_Refs{ 1 };
    std::shared_ptr<VirtualFiles::Source> m_Source;
    std::size_t m_Index;
    std::uint64_t m_Size;
    std::wstring m_Name;
    std::uint64_t m_Position = 0;
    std::unique_ptr<VirtualFiles::Reader> m_Reader;
    std::uint64_t m_ReaderPosition = 0;
};

class FileDataObject final : public IDataObject, public IAsyncCapability
{
public:
    FileDataObject(std::vector<VirtualFiles::Entry> entries,
                   std::shared_ptr<VirtualFiles::Source> source)
        : m_Entries(std::move(entries)), m_Source(std::move(source))
    {
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
        } else if (riid == IID_AsyncCapability) {
            *out = static_cast<IAsyncCapability*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_Refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (format == nullptr || medium == nullptr) {
            return E_INVALIDARG;
        }
        std::memset(medium, 0, sizeof(*medium));
        const HRESULT supported = QueryGetData(format);
        if (FAILED(supported)) {
            return supported;
        }

        if (format->cfFormat == descriptorFormat()) {
            HGLOBAL global = makeDescriptor();
            if (global == nullptr) {
                return E_OUTOFMEMORY;
            }
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = global;
            return S_OK;
        }
        if (format->cfFormat == dropEffectFormat()) {
            HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (global == nullptr) {
                return E_OUTOFMEMORY;
            }
            *static_cast<DWORD*>(GlobalLock(global)) = DROPEFFECT_COPY;
            GlobalUnlock(global);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = global;
            return S_OK;
        }
        // FileContents, already checked by QueryGetData.
        const auto index = static_cast<std::size_t>(format->lindex);
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = new FileStream(m_Source, index, m_Entries[index]);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (format == nullptr) {
            return E_INVALIDARG;
        }
        if (format->dwAspect != DVASPECT_CONTENT) {
            return DV_E_DVASPECT;
        }
        if (format->cfFormat == descriptorFormat() || format->cfFormat == dropEffectFormat()) {
            return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
        }
        if (format->cfFormat == contentsFormat()) {
            if ((format->tymed & TYMED_ISTREAM) == 0) {
                return DV_E_TYMED;
            }
            // lindex -1 (whole clipboard, as a clipboard viewer asks) has no contents to give.
            if (format->lindex < 0 ||
                static_cast<std::size_t>(format->lindex) >= m_Entries.size() ||
                m_Entries[static_cast<std::size_t>(format->lindex)].dir) {
                return DV_E_LINDEX;
            }
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        if (out != nullptr) {
            out->ptd = nullptr;
        }
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM* medium, BOOL release) override
    {
        // Explorer reports "Performed DropEffect" / "Paste Succeeded" here; nothing to keep.
        if (release && medium != nullptr) {
            ReleaseStgMedium(medium);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        FORMATETC formats[] = {
            { descriptorFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
            { contentsFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM },
            { dropEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
        };
        return SHCreateStdEnumFmtEtc(ARRAYSIZE(formats), formats, out);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    // IDataObjectAsyncCapability: lets Explorer copy on its own worker thread.
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL doOpAsync) override
    {
        m_Async = doOpAsync != FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* isOpAsync) override
    {
        if (isOpAsync == nullptr) {
            return E_INVALIDARG;
        }
        *isOpAsync = m_Async ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override
    {
        m_InOperation = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* inAsyncOp) override
    {
        if (inAsyncOp == nullptr) {
            return E_INVALIDARG;
        }
        *inAsyncOp = m_InOperation ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx*, DWORD) override
    {
        m_InOperation = false;
        return S_OK;
    }

private:
    ~FileDataObject() = default;

    HGLOBAL makeDescriptor() const
    {
        const std::size_t count = m_Entries.size();
        const std::size_t bytes =
            sizeof(FILEGROUPDESCRIPTORW) + (count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (global == nullptr) {
            return nullptr;
        }
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(global));
        group->cItems = static_cast<UINT>(count);
        for (std::size_t i = 0; i < count; ++i) {
            const VirtualFiles::Entry& entry = m_Entries[i];
            FILEDESCRIPTORW& fd = group->fgd[i];
            fd.dwFlags =
                FD_ATTRIBUTES | FD_FILESIZE | FD_PROGRESSUI | static_cast<DWORD>(FD_UNICODE);
            fd.dwFileAttributes = entry.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
            fd.nFileSizeLow = static_cast<DWORD>(entry.size & 0xFFFFFFFFULL);
            if (entry.mtime > 0) {
                fd.dwFlags |= FD_WRITESTIME;
                fd.ftLastWriteTime = unixToFileTime(entry.mtime);
            }
            const std::wstring path = toWide(entry.path);
            const std::size_t units = std::min<std::size_t>(path.size(), MAX_PATH - 1);
            std::wmemcpy(fd.cFileName, path.c_str(), units);
            fd.cFileName[units] = L'\0';
        }
        GlobalUnlock(global);
        return global;
    }

    std::atomic<ULONG> m_Refs{ 1 };
    std::vector<VirtualFiles::Entry> m_Entries;
    std::shared_ptr<VirtualFiles::Source> m_Source;
    bool m_Async = true;
    bool m_InOperation = false;
};

// The object last put on the clipboard; one reference held here.
IDataObject* g_Current = nullptr;

void releaseCurrent()
{
    if (g_Current != nullptr) {
        g_Current->Release();
        g_Current = nullptr;
    }
}

} // namespace

namespace VirtualFileClipboard {

bool set(std::vector<VirtualFiles::Entry> entries, std::shared_ptr<VirtualFiles::Source> source)
{
    auto* object = new FileDataObject(std::move(entries), std::move(source));
    HRESULT hr = E_FAIL;
    // Another process may hold the clipboard open for a moment.
    for (int attempt = 0; attempt < 10; ++attempt) {
        hr = OleSetClipboard(object);
        if (hr != CLIPBRD_E_CANT_OPEN) {
            break;
        }
        Sleep(20);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr,
                     "ClipboardSync WARN: VirtualFileClipboard: OleSetClipboard failed (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        object->Release();
        return false;
    }
    releaseCurrent();
    g_Current = object; // keeps the creation reference
    return true;
}

bool ownsClipboard()
{
    if (g_Current == nullptr) {
        return false;
    }
    if (OleIsCurrentClipboard(g_Current) == S_OK) {
        return true;
    }
    // Someone else owns the clipboard now; streams a paste still reads hold their own references.
    releaseCurrent();
    return false;
}

void clearIfOwned()
{
    if (ownsClipboard()) {
        OleSetClipboard(nullptr);
    }
    releaseCurrent();
}

} // namespace VirtualFileClipboard
