# File Mapping Platform Mount Plan

## Goal

Make host shared folders appear in the user's native file surface on every supported client platform, while keeping protocol, VFS, cache, and mount-provider logic cleanly separated.

macOS is the first implementation target because it is the easiest environment for near-term validation. The design itself is platform-wide.

The user-facing target is:

1. The user starts a Moonlight stream.
2. The overlay shows `Host Files: Ready`.
3. Clicking `Host Files` opens the platform-native file surface:
   - Finder on macOS.
   - Explorer on Windows.
   - File manager or mount point on Linux.
   - Native document/file UI on mobile platforms.
4. Shared host folders appear as read-only folders.
5. Files are fetched on demand and opened by normal local apps.
6. The location disappears or becomes unavailable when the session ends.

This is a native file-surface experience, not a Moonlight-only file browser.

## Recommendation

Use a provider-based architecture with platform-specific mount providers.

Initial provider:

- macOS: Apple File Provider.

Planned providers:

- Windows: WinFsp.
- Linux: FUSE.
- Mobile: platform document/file provider surfaces.

Fallback provider:

- Moonlight overlay/download UI for platforms where native mounting is unavailable or disabled.

## Implementation Status

M0 has started on the Qt branch.

Current landed skeleton:

- `file-mapping/protocol`: platform-neutral protocol facade and message/error types.
- `file-mapping/vfs`: platform-neutral VFS item, handle, and read interface.
- `file-mapping/mount`: provider-neutral mount session and provider interface.
- `file-mapping/file-mapping.pri`: qmake integration for the Qt app build.

Current non-goals:

- No behavior change to the existing overlay Host Files readiness probe.
- No macOS File Provider extension yet.
- No Windows/Linux mount helpers yet.
- No migration of `app/streaming/filemappingclient.*` yet.

## First Implementation Target

Start with macOS using Apple File Provider.

Do not start with macFUSE for the first user-testable build.

Rationale:

- File Provider is Apple's native model for Finder-integrated remote files.
- It does not require users to install a kernel extension, system extension, or third-party driver.
- It gives us Finder sidebar/location integration, on-demand materialization, offline state, and system-managed UI affordances.
- It is closer to a future iOS/iPadOS path than a FUSE mount.
- It lets the first Mac build validate product UX before we take on the heavier POSIX mount surface.

Keep macFUSE as a later optional macOS provider for users who need true POSIX mount behavior, offset reads, or workloads where Finder materialization is too limiting.

## Platform Providers

### macOS: Apple File Provider

Use for the first macOS milestone and the first full native-file-surface validation.

Strengths:

- Native Finder integration.
- No extra driver installation.
- Good for read-only shared folders, browsing, preview, and opening files.
- Natural fit for on-demand file content.
- Can map Moonlight host/session as a provider domain.

Tradeoffs:

- It is not a raw filesystem mount.
- File contents are materialized through the File Provider contract, so large-file random access may become full-file or chunk-managed download depending on implementation.
- Requires an app extension, entitlements, codesigning, and macOS packaging work.
- The provider must carefully model item identifiers, parent relationships, versioning, and invalidation.

### macOS: macFUSE

Defer to a second macOS provider.

Strengths:

- Closest to true mounted filesystem behavior.
- Natural fit for `read(path, offset, length)`.
- Better for large media files and POSIX-style tools.

Tradeoffs:

- Requires third-party installation and system approval.
- Higher support burden.
- Packaging and notarization are more sensitive.
- Not a good first test path if the goal is broad user acceptance.

### Windows: WinFsp

Use for the Windows Explorer mount provider.

Strengths:

- Native Explorer drive or folder mount.
- Good match for random `read(path, offset, length)`.
- User-mode filesystem implementation.
- Better large-file behavior than File Provider materialization.

Tradeoffs:

- Requires WinFsp installation and support flow.
- Needs a helper process so Explorer callbacks do not affect streaming.
- Installer, cleanup, and lifecycle handling are more involved.

### Linux: FUSE

Use for Linux file manager and CLI mount integration.

Strengths:

- Native user-space filesystem model.
- Good match for random reads.
- Works with common Linux file managers and CLI tools.

Tradeoffs:

- Packaging varies by distro.
- Permission and mount namespace behavior must be tested carefully.
- Needs a helper process and clear unmount behavior.

### Mobile: Native Document Surfaces

Use platform-specific file/document provider APIs instead of exposing a POSIX mount.

Targets:

- iOS/iPadOS: File Provider-style integration when feasible.
- Android: DocumentsProvider-style integration.

Tradeoffs:

- Not a full mounted filesystem.
- Stronger app lifecycle constraints.
- Requires mobile-specific UX and background execution handling.

### WebDAV

Do not use for the primary experience.

Strengths:

- Easy to prototype.
- Common desktop file managers can connect to WebDAV.

Tradeoffs:

- Desktop WebDAV behavior and caching are not reliable enough for this feature.
- Error handling, authentication, and reconnect UX are weaker.
- It adds another protocol surface instead of reusing the existing file-mapping RPC cleanly.

## Architecture

Use a provider-based architecture:

```text
Sunshine file-mapping WSS protocol
        |
        v
file-mapping protocol client
        |
        v
platform-neutral VFS core
        |
        v
mount provider interface
        |
        +-- macOS File Provider extension
        +-- Windows WinFsp provider
        +-- Linux FUSE provider
        +-- fallback overlay/download provider
```

Qt should not contain Finder, Explorer, FUSE, File Provider, or WinFsp-specific logic. Qt owns session UX and calls a mount coordinator.

## Module Layout

Proposed layout in `moonlight-qt`:

```text
app/streaming/
  filemappingclient.*              # Current Qt protocol client, kept as adapter until core exists

file-mapping/
  protocol/
    file_mapping_client.h
    file_mapping_client.cpp
    file_mapping_messages.h
    file_mapping_errors.h

  vfs/
    remote_vfs.h
    remote_vfs.cpp
    vfs_cache.h
    vfs_item.h
    vfs_handle.h

  mount/
    mount_provider.h
    mount_session.h
    mount_errors.h

  mount/macos-fileprovider/
    MoonlightFileProviderExtension/
    file_provider_bridge.mm
    file_provider_domain_manager.mm
    file_provider_item_model.mm

  mount/windows-winfsp/
    README.md

  mount/linux-fuse/
    README.md
```

The exact build-system shape can be adjusted, but the boundary should remain:

- protocol knows WSS/RPC
- VFS knows folders/files/handles/cache
- provider knows platform integration
- Qt knows user actions and session lifecycle

## Core Interfaces

### Protocol Client

```cpp
struct FileMappingCapability {
    bool available;
    QString endpoint;
    quint16 port;
    QString token;
    QString clientUuid;
    QString error;
};

class FileMappingProtocolClient {
public:
    FileMappingCapability fetchCapability();
    void connectSession(const FileMappingCapability& capability);
    QList<RemoteEntry> list(QString mappingId, QString path);
    RemoteStat stat(QString mappingId, QString path);
    QByteArray read(QString mappingId, QString path, quint64 offset, quint32 length);
};
```

### VFS Core

```cpp
class RemoteFileSystem {
public:
    QList<VfsItem> children(VfsItemId parent);
    VfsItem item(VfsItemId id);
    ReadHandle open(VfsItemId id);
    QByteArray read(ReadHandle handle, quint64 offset, quint32 length);
    void close(ReadHandle handle);
};
```

The VFS core must be platform-neutral:

- stable item IDs
- path normalization
- read-only access
- negative cache with short TTL
- per-file metadata cache
- reconnect-aware error mapping
- no Finder, WinFsp, Qt, or SDL dependencies

### Mount Provider

```cpp
class MountProvider {
public:
    MountResult mount(MountRequest request);
    void unmount(MountId id);
    MountStatus status(MountId id);
    void reveal(MountId id);
};
```

Each platform implementation maps this interface to its native surface:

- macOS maps to File Provider domains and Finder reveal operations.
- Windows maps to WinFsp mounts and Explorer reveal operations.
- Linux maps to FUSE mounts and file-manager reveal operations.
- Mobile maps to native document/file provider surfaces.

## Provider Selection Policy

Runtime provider selection should be explicit and predictable:

1. Prefer the platform-native provider if installed, supported, and enabled.
2. Fall back to overlay/download UI if native mounting is unavailable.
3. Never silently switch to a less secure or less reliable provider without telling the user.

Provider status should be visible in Qt:

- `Available`
- `Setup Required`
- `Mounting`
- `Mounted`
- `Unavailable`
- `Error`

## macOS File Provider Design

### Domain Model

Use one File Provider domain per active Moonlight host/session:

```text
Moonlight Host Files
  <Host Name>
    <Shared Folder A>
    <Shared Folder B>
```

Domain identifier:

```text
moonlight.<hostUuid>.<sessionId>
```

Display name:

```text
Moonlight Host Files - <Host Name>
```

This avoids collisions when multiple hosts or sessions exist.

### Item IDs

Use opaque stable IDs, not raw paths:

```text
root
mapping:<mappingId>
node:<mappingId>:<base64url(normalizedPath)>
```

Reasons:

- Avoid leaking host paths into provider internals.
- Keep path normalization centralized.
- Allow future aliasing or display-name changes.
- Avoid invalid Finder identifiers for special path characters.

### Read-Only Policy

First macOS milestone is read-only.

Supported:

- enumerate
- get metadata
- fetch file contents
- open in apps
- Quick Look where supported
- copy out to local disk

Rejected:

- create
- write
- rename
- delete
- move
- set attributes

All rejected operations should return a clear read-only error.

### Content Fetch

For File Provider, implement on-demand materialization:

1. Finder asks for file contents.
2. Provider opens a VFS read handle.
3. Provider reads from host into a local temporary/materialized file.
4. Provider reports completion to File Provider.
5. System opens the materialized file normally.

For the first build, prefer correctness over clever streaming:

- sequentially fetch file content in chunks
- cap concurrent file downloads
- support cancellation
- surface disconnect errors
- avoid unbounded local cache growth

Later, optimize:

- sparse/chunk cache
- resume partial materialization
- predictive folder metadata prefetch
- large-file streaming provider fallback via macFUSE

## Qt UX

Overlay behavior:

- `Host Files: Checking`
- `Host Files: Ready`
- `Host Files: Mounting`
- `Host Files: Open`
- `Host Files: Error`

Click behavior:

1. If checking, show toast.
2. If unavailable, show actionable error and retry capability.
3. If ready but native mount/provider is not active, create or reconnect it.
4. Reveal the platform-native location.
5. If the platform provider is missing or disabled, show setup guidance.

Settings:

- `Host Files native integration`: on/off
- `Auto-open host files when stream starts`: off by default
- `Auto-unmount when stream ends`: on by default
- `Cache limit`: automatic for first build

Session end:

- mark provider domain unavailable immediately
- optionally remove active session domain
- clean temporary materialized files according to cache policy

## Helper vs In-Process

For macOS File Provider, the provider code naturally lives in an app extension process. The Qt app should coordinate domains and pass session context through a small local bridge.

Use a local app-group container or local IPC for:

- active host/session metadata
- capability/session token refresh
- provider logs
- cancellation and shutdown

Do not put long-running Finder provider work directly in the SDL streaming thread or overlay UI path.

## Security

Security defaults:

- read-only
- paired-client certificate remains required on Sunshine capability endpoint
- WSS session token is short-lived and one-time
- token must never be stored persistently
- host absolute paths are not exposed as item IDs
- no writes in first milestone
- materialized files live in a Moonlight-controlled cache directory
- cache is cleared on logout/stream end according to policy

Risk controls:

- reject path traversal in VFS core
- normalize separators and Unicode path forms
- cap file size fetched in one operation only if UX needs a warning, not as a silent failure
- rate-limit concurrent reads
- map network failures to retryable native-provider errors where possible

## Milestones

### M0: Platform Architecture Skeleton

- Add module directories and interfaces.
- Move current Qt file-mapping client behind protocol-client facade.
- Add mock VFS provider with local test data.
- Add documentation and compile-only stubs.

Exit criteria:

- Qt still builds.
- Existing Host Files readiness behavior remains intact.
- VFS mock tests pass.

### M1: macOS File Provider Proof of Concept

- Add File Provider extension target.
- Register a test domain.
- Show static mock folders in Finder.
- Reveal Finder location from Qt.

Exit criteria:

- Test build shows `Moonlight Host Files` in Finder.
- Clicking overlay opens Finder.
- No Sunshine connection required.

### M2: Platform VFS Remote Enumeration

- Connect provider enumeration to Sunshine file-mapping `list`.
- Show real shared folders.
- Show nested directories.
- Handle disconnect as unavailable state.

Exit criteria:

- Shared folders appear in the active native provider.
- Folder navigation works.
- Read-only operations are enforced.

### M3: File Content Fetch

- Implement file content fetch through VFS `read`.
- Support cancellation.
- Support basic cache cleanup.
- Add user-visible errors for timeout/disconnect.

Exit criteria:

- Double-clicking a small file opens it.
- Copying a file to local disk works.
- Network interruption does not hang the native file surface indefinitely.

### M4: macOS Product Polish

- Settings.
- Better status toasts.
- Logs and diagnostics.
- Packaging/notarization.
- Test release for macOS.

Exit criteria:

- User can install the test DMG, start a stream, click Host Files, browse Finder, and open/copy files.

### M5: Windows Provider

- Add WinFsp helper.
- Support drive or folder mount.
- Reveal Explorer location from Qt.
- Reuse the same protocol client, VFS core, and mount coordinator.

Exit criteria:

- User can install the Windows test build, start a stream, click Host Files, browse Explorer, and open/copy files.

### M6: Linux Provider

- Add FUSE helper.
- Support XDG-friendly mount location.
- Reveal file-manager location from Qt where available.
- Reuse the same protocol client, VFS core, and mount coordinator.

Exit criteria:

- User can start a stream, click Host Files, browse the mounted location, and open/copy files on a supported Linux desktop.

## Windows Provider Design

Use WinFsp provider:

- true Explorer mount
- drive letter or folder mount
- direct offset reads
- helper process preferred

Reuse:

- protocol client
- VFS core
- cache/error model
- mount provider interface

First Windows milestone should come after macOS M2 or M3, once the platform-neutral VFS interface has proven stable.

## Linux Provider Design

Use FUSE provider:

- user-space mount
- direct offset reads
- distro packaging needed

Linux should reuse the same helper-process pattern as Windows.

## Mobile Provider Design

Use VFS core with native document/file UI:

- iOS/iPadOS can reuse File Provider concepts later.
- Android can expose a DocumentsProvider-style surface.

## Open Questions

- Should each active stream create a temporary File Provider domain, or should each host have a persistent domain that becomes online/offline?
- What local cache size is acceptable for first test builds?
- Do we need a macFUSE advanced mode for large video editing workloads?
- Should Finder auto-open on stream start or only on explicit click?
- Should shared folders be grouped by host display name or direct root folders?
- Should Windows default to a drive letter, a folder mount, or user choice?
- Should Linux default to an XDG-visible mount location under the user's runtime directory?
- How much behavior must be shared between desktop and mobile providers?

## First Implementation Choice

Build M0 and M1 first.

That gives us the fastest Mac-verifiable loop while keeping the architecture platform-wide:

- no dependency on Sunshine changes
- no third-party driver install
- clear Finder UX validation
- clean abstraction for later Windows/Linux providers

After M1, connect real Sunshine enumeration and content fetch incrementally.

## References

- Apple File Provider documentation: https://developer.apple.com/documentation/fileprovider
- macFUSE project: https://macfuse.github.io/
