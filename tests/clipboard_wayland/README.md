# Native Wayland clipboard smoke test

Run only in a disposable, isolated Wayland session: this test changes its clipboard.
It requires Qt with `QT_WAYLAND_USE_DATA_CONTROL` support, a compositor exposing
`wl_seat` and `zwlr_data_control_manager_v1`, and `wl-copy` / `wl-paste`.

Build the production `clipboard-helper/clipboard-helper.pro` with qmake, then run:

```sh
python3 tests/clipboard_wayland/smoke.py /absolute/path/to/moonlight-clipboard-helper
```

The caller supplies `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` for an isolated Sway
session. The script forces the helper onto Wayland and checks protocol binding,
then compares fresh UTF-8 text in both directions with data-control `0 → 1 → 0`.
Each helper is restarted; the disabled cases must retain the local clipboard and
remain responsive. Checked-in public TLS fixtures only satisfy helper configuration;
the short inline text does not require a Sunshine host or HTTPS server.

This covers the helper's native clipboard path. The separate lifecycle test covers
the production launcher passing default and explicit environment values. It does
not establish niri/AppImage end-to-end streaming or PNG compatibility.
