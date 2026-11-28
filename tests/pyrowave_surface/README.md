# PyroWave Vulkan surface A/B

Build `pyrowave_surface.pro` with qmake in a release build directory, using the
same PyroWave, FFmpeg and libplacebo dependencies as the client. On Windows,
add the dependency DLL directory to `PATH`. Run the resulting executable with
width, height, HDR mode (0 SDR, 1 PQ, 2 HLG), measured frames per pass, target
FPS and bitstream budget in Mbit/s.

```console
pyrowave_surface 1920 1080 0 480 120 100
pyrowave_surface 3840 2160 1 480 60 150
```

The test encodes a deterministic YUV420 pattern once and replays the packet in
CPU/GPU/GPU/CPU order. Each pass warms up for 24 frames. Both paths check the
packet metadata and render into the same offscreen target format (RGB8 for SDR,
RGB10 for HDR). The CPU path uses synchronous readback and libplacebo upload.
The GPU path uses the client's surface pool and semaphore handoff.

* `submit_ms` measures packet ingestion through delivery of a decoded frame.
* `complete_ms` includes rendering and a GPU completion wait on every frame.
* Both timings report P50/P95/P99. `effective_fps` reports the observed input
  cadence. `over_budget` counts completed frames taking longer than one target
  frame period.
* Outside the timed loop, three distinct patterns exercise a two-slot pool.
  The test holds the first frame, drops the second and reuses its slot for the
  third. It compares both surviving frames with CPU readback of their respective
  packets while the pool is full. After releasing both slots, it decodes the
  second pattern on the same GPU decoder and checks it against CPU readback.

The measured interval excludes capture, network, client pacing, presentation
and display scanout. The completion wait serializes frames so deferred GPU
work is included in the comparison.

## Radeon 780M results

Windows x64 Release, AMD driver 32.0.32029.2010, PyroWave `cc91cbf` and
libplacebo API 374. Each range covers the two measured passes for that path.
Times are milliseconds from packet ingestion through offscreen GPU completion.

| Input | CPU P50 | Surface P50 | CPU P95 | Surface P95 | CPU P99 | Surface P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p120 SDR, 100 Mbit/s | 2.51–2.53 | 1.32–1.33 | 3.31–3.47 | 1.84–2.08 | 3.76–3.95 | 2.71–2.85 |
| 1080p120 HLG, 100 Mbit/s | 2.98–2.99 | 1.22–1.34 | 4.26–4.27 | 2.14–2.54 | 4.89–5.03 | 2.68–2.71 |
| 2160p60 SDR, 150 Mbit/s | 6.33–6.46 | 3.16–4.33 | 7.29–7.85 | 4.65–4.79 | 8.23–9.03 | 5.04–5.32 |
| 2160p60 PQ, 150 Mbit/s | 8.78–8.80 | 3.64–3.69 | 10.16–10.17 | 4.87–4.89 | 11.00–11.92 | 5.45–5.51 |

All passes met the target input cadence with 480 measured frames each. In the
2160p60 PQ case, CPU-side delivery P50 fell from 6.12–6.15 ms to 0.29 ms.
Rendered SDR pixels matched exactly. PQ and HLG differed by at most two RGB10
code values on this pattern.
