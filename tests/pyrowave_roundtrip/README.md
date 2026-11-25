# PyroWave pixel roundtrip on macOS

This optional test uses the public PyroWave Metal encoder API to create a YUV420P frame, wraps its encoded payload in common-c transport packets, reassembles them in reverse order, and decodes the result twice through the public PyroWave Vulkan API. It checks the Y, U and V planes against the original pattern with a maximum error of 3 per sample and uses padded output row strides. It does not exercise Moonlight's renderer, actual network, or decoder class.

Build the PyroWave Vulkan shared library with its `pyrowave-shared.pc` installed and build its separate `metal` CMake project. A working Vulkan implementation (for example MoltenVK), Vulkan headers, and an Apple Silicon GPU supported by the Metal backend are required. Then run:

```sh
PKG_CONFIG_PATH=/path/to/pyrowave/lib/pkgconfig \
PYROWAVE_METAL_INCLUDE_DIR=/path/to/pyrowave/metal \
PYROWAVE_METAL_LIB_DIR=/path/to/metal/build \
VULKAN_INCLUDE_DIR=/path/to/vulkan/include \
GRANITE_VULKAN_LIBRARY=/path/to/libMoltenVK.dylib \
./tests/pyrowave_roundtrip/run-macos.sh
```

`GRANITE_VULKAN_LIBRARY` is only needed when the Vulkan loader cannot otherwise be found. The two implementations export the same symbols, so the test executes encoding and decoding in separate processes.
