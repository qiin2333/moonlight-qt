# macOS File Provider Integration

This directory contains the staged macOS File Provider implementation for Host Files.

The goal is to make Moonlight Host Files appear in Finder without requiring macFUSE or any other user-installed filesystem driver.

## Phase 1 Shape

The first implementation phase is intentionally split:

- Qt provider selection lives in `file-mapping/mount/mac_file_provider_mount_provider.*`.
- The native File Provider extension lives under `MoonlightFileProviderExtension/`.
- qmake packaging is not wired yet. The extension files are scaffolded here so the macOS target can be added without touching the platform-neutral VFS and mount coordinator again.

Provider order on macOS should be:

1. Apple File Provider.
2. macFUSE runtime provider.
3. Finder mirror fallback.

## Domain Model

Use one active domain per host session:

```text
identifier: moonlight.<hostUuid>.<sessionId>
display:    Moonlight Host Files - <hostName>
```

The app registers the domain when Host Files are ready and removes it when the stream ends.

## Extension Data Flow

The extension must not link Qt. It should use an App Group container or local IPC bridge to reach Moonlight state:

```text
Finder
  -> File Provider extension
  -> Moonlight app-group session store or local IPC
  -> protocol-backed RemoteVfs
  -> Sunshine file-mapping WebSocket
```

## Phase Exit Criteria

M1 is complete when a macOS build can:

- bundle and sign the `.appex`;
- register a mock `Moonlight Host Files` domain;
- show static folders in Finder;
- reveal the Finder location from the overlay.

M2 replaces the mock enumerator with real `RemoteVfs::children()` data.
M3 implements file content materialization through `RemoteVfs::open/read/close`.
