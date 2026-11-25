# PyroWave color contract

This test exercises the production color mapping and static HDR metadata helpers.
It covers SDR, HDR10/PQ and HLG, 8/10-bit AVFrame formats, the HDR compute-path
requirement, mismatched bitstream colors, chroma siting, metadata units and allocation
failures. It does not require a GPU or link the PyroWave library.

The `pyrowave-tests` CI job runs this test with PyroWave headers pinned to
`cc91cbf6363721d19d135d05ca8e3d8665ea6afd` and also checks preferences in builds
with and without PyroWave support.

Install libavutil and Vulkan development headers, then use an empty build directory:

```sh
qmake /path/to/moonlight-qt/tests/pyrowave_color_contract/pyrowave_color_contract.pro \
    PYROWAVE_INCLUDEPATH=/path/to/AlkaidLab/pyrowave
make && ./pyrowave_color_contract
```

The include-path override is unnecessary when `pyrowave-shared` is installed and
visible through pkg-config. Headers must provide the AlkaidLab color metadata API.

This test does not validate GPU decoding or HDR display output. The separate Metal
roundtrip test remains an SDR test. Validate HDR10/PQ and HLG with a real HDR source,
the intended GPU and display, and conventional-codec fallback before release.
