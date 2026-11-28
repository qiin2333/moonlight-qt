#include "../../app/streaming/video/pyrowavecolor.h"
#include "../../app/streaming/video/pyrowavesurfaces.h"

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #expr);                      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
using Clock = std::chrono::steady_clock;

static pyrowave_cpu_buffer cpuBuffer(AVFrame* frame, pyrowave_cpu_buffer_format format)
{
    pyrowave_cpu_buffer buffer = {};
    buffer.width = frame->width;
    buffer.height = frame->height;
    buffer.format = format;
    for (int p = 0; p < 3; ++p) {
        buffer.data[p] = frame->data[p];
        buffer.row_stride_in_bytes[p] = frame->linesize[p];
        buffer.plane_size_in_bytes[p] =
            size_t(frame->linesize[p]) * (p ? frame->height / 2 : frame->height);
    }
    return buffer;
}

static std::vector<uint8_t> encode(int width, int height, const PyrowaveColor::Contract& color,
                                   size_t byteBudget, int pattern)
{
    pyrowave_device device = nullptr;
    CHECK(pyrowave_create_default_device(&device) == PYROWAVE_SUCCESS);
    pyrowave_encoder_create_info info = { device, width, height, PYROWAVE_CHROMA_SUBSAMPLING_420 };
    pyrowave_encoder encoder = nullptr;
    CHECK(pyrowave_encoder_create(&info, &encoder) == PYROWAVE_SUCCESS);
    CHECK(pyrowave_encoder_set_color_metadata(encoder, &color.metadata) == PYROWAVE_SUCCESS);
    AVFrame* frame = av_frame_alloc();
    CHECK(frame);
    frame->width = width;
    frame->height = height;
    frame->format = color.pixelFormat;
    CHECK(av_frame_get_buffer(frame, 32) == 0);
    for (int p = 0; p < 3; ++p) {
        for (int y = 0; y < (p ? height / 2 : height); ++y) {
            auto row = frame->data[p] + y * frame->linesize[p];
            for (int x = 0; x < (p ? width / 2 : width); ++x) {
                int value = x + (p + 1) * y + p * 89 + pattern * (color.hdrMode ? 341 : 85);
                if (color.hdrMode) {
                    reinterpret_cast<uint16_t*>(row)[x] = value % 1024;
                } else {
                    row[x] = uint8_t(value);
                }
            }
        }
    }
    auto buffer = cpuBuffer(frame, color.cpuFormat);
    pyrowave_rate_control rate = {};
    rate.maximum_bitstream_size = byteBudget;
    CHECK(pyrowave_encoder_encode_cpu(encoder, &buffer, &rate) == PYROWAVE_SUCCESS);
    size_t count = 0;
    CHECK(pyrowave_encoder_compute_num_packets(encoder, rate.maximum_bitstream_size, &count) ==
              PYROWAVE_SUCCESS &&
          count == 1);
    pyrowave_packet packet = {};
    std::vector<uint8_t> bytes(rate.maximum_bitstream_size);
    CHECK(pyrowave_encoder_packetize(encoder, &packet, bytes.size(), &count, bytes.data(),
                                     bytes.size()) == PYROWAVE_SUCCESS);
    CHECK(packet.offset == 0 && packet.size > 0);
    bytes.resize(packet.size);
    av_frame_free(&frame);
    pyrowave_encoder_destroy(encoder);
    pyrowave_device_destroy(device);
    return bytes;
}

static void report(const char* metric, std::vector<double>& samples)
{
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](size_t p) {
        return samples[(p * samples.size() + 99) / 100 - 1];
    };
    std::printf(" %s_ms=%.3f/%.3f/%.3f", metric, percentile(50), percentile(95), percentile(99));
}

static void checkPixels(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& pixels,
                        int bits)
{
    CHECK(reference.size() == pixels.size());
    int maxError = 0;
    uint64_t totalError = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        uint32_t a, b;
        std::memcpy(&a, reference.data() + i, sizeof(a));
        std::memcpy(&b, pixels.data() + i, sizeof(b));
        int mask = (1 << bits) - 1;
        for (int c = 0; c < 3; ++c) {
            int error = std::abs(int(a & mask) - int(b & mask));
            totalError += error;
            maxError = std::max(error, maxError);
            a >>= bits;
            b >>= bits;
        }
    }
    std::printf("RGB%d max_error=%d mean_abs_error=%.5f\n", bits, maxError,
                double(totalError) / (pixels.size() / 4 * 3));
    CHECK(maxError <= 3);
}

static std::vector<uint8_t> run(bool direct, const std::array<std::vector<uint8_t>, 3>& packets,
                                int w, int h, const PyrowaveColor::Contract& color, int samples,
                                int fps)
{
    pl_vk_inst_params instanceParams = pl_vk_inst_default_params;
#ifdef _WIN32
    static HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    CHECK(loader);
    instanceParams.get_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
#endif
    pl_vk_inst instance = pl_vk_inst_create(nullptr, &instanceParams);
    CHECK(instance);
    pl_vulkan_params params = pl_vulkan_default_params;
    params.instance = instance->instance;
    params.get_proc_addr = instance->get_proc_addr;
    if (direct) {
        PyrowaveSurfaces::configure(params);
    }
    pl_vulkan vk = pl_vulkan_create(nullptr, &params);
    CHECK(vk);
    pl_gpu gpu = vk->gpu;
    pl_renderer renderer = pl_renderer_create(nullptr, gpu);
    CHECK(renderer);
    pl_tex_params target = {};
    target.w = w;
    target.h = h;
    target.format = pl_find_named_fmt(gpu, color.hdrMode ? "rgb10a2" : "rgba8");
    CHECK(target.format);
    target.renderable = target.host_readable = true;
    pl_tex output = pl_tex_create(gpu, &target);
    CHECK(output);
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    {
        std::optional<PyrowaveSurfaces> surfaces;
        pyrowave_device device = nullptr;
        if (direct) {
            surfaces.emplace(instance, vk, 2);
            CHECK(surfaces->createDevice(&device));
        } else {
            CHECK(pyrowave_create_default_device(&device) == PYROWAVE_SUCCESS);
        }
        pyrowave_decoder_create_info info = { device, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420,
                                              false };
        pyrowave_decoder decoder = nullptr;
        CHECK(pyrowave_decoder_create(&info, &decoder) == PYROWAVE_SUCCESS);
        pl_tex uploads[4] = {};
        auto prepare = [&](const std::vector<uint8_t>& packet) {
            pyrowave_decoder_clear(decoder);
            CHECK(pyrowave_decoder_push_packet(decoder, packet.data(), packet.size()) ==
                  PYROWAVE_SUCCESS);
            CHECK(pyrowave_decoder_decode_is_ready(decoder, false));
            pyrowave_color_metadata meta = {};
            CHECK(pyrowave_decoder_get_color_metadata(decoder, &meta) &&
                  PyrowaveColor::matches(color, meta));
            AVFrame* frame = av_frame_alloc();
            CHECK(frame);
            PyrowaveColor::applyToFrame(color, meta, frame);
            frame->width = w;
            frame->height = h;
            return frame;
        };
        auto readCpu = [&](AVFrame* frame) {
            CHECK(av_frame_get_buffer(frame, 32) == 0);
            auto buffer = cpuBuffer(frame, color.cpuFormat);
            CHECK(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) ==
                  PYROWAVE_SUCCESS);
        };
        auto render = [&](AVFrame* frame) {
            pl_frame src = {}, dst = {};
            const bool isSurface = surfaces && surfaces->owns(frame);
            if (isSurface) {
                surfaces->map(frame, &src);
            } else {
                pl_avframe_params map = {};
                map.frame = frame;
                map.tex = uploads;
                CHECK(pl_map_avframe_ex(gpu, &src, &map));
            }
            dst.num_planes = 1;
            dst.planes[0].texture = output;
            dst.planes[0].components = 4;
            for (int c = 0; c < 4; ++c) {
                dst.planes[0].component_mapping[c] = c;
            }
            dst.color = src.color;
            dst.repr = pl_color_repr_rgb;
            CHECK(pl_render_image(renderer, &src, &dst, &pl_render_fast_params));
            if (!isSurface) {
                pl_unmap_avframe(gpu, &src);
            }
            pl_gpu_finish(gpu);
        };
        auto download = [&](std::vector<uint8_t>& result) {
            pl_tex_transfer_params transfer = {};
            transfer.tex = output;
            transfer.ptr = result.data();
            CHECK(pl_tex_download(gpu, &transfer));
        };
        std::vector<double> submit, complete;
        auto next = Clock::now(), first = next, last = next;
        const auto period = std::chrono::nanoseconds(1000000000 / fps);
        int overBudget = 0;
        for (int i = -24; i < samples; ++i) {
            std::this_thread::sleep_until(next);
            auto start = Clock::now();
            last = start;
            if (i == 0) {
                first = start;
            }
            next += period;
            if (next < start) {
                next = start + period;
            }
            AVFrame* frame = prepare(packets[0]);
            if (direct) {
                CHECK(surfaces->decode(decoder, frame) == PyrowaveSurfaces::Decoded);
            } else {
                readCpu(frame);
            }
            auto submitted = Clock::now();
            render(frame);
            auto end = Clock::now();
            if (i >= 0) {
                submit.push_back(
                    std::chrono::duration<double, std::milli>(submitted - start).count());
                complete.push_back(std::chrono::duration<double, std::milli>(end - start).count());
                overBudget += end - start > period;
            }
            av_frame_free(&frame);
        }
        std::printf("%s %dx%d %s frames=%d fps_target=%d", direct ? "GPU" : "CPU", w, h,
                    color.label, samples, fps);
        report("submit", submit);
        report("complete", complete);
        std::printf(" effective_fps=%.1f over_budget=%d",
                    (samples - 1) / std::chrono::duration<double>(last - first).count(),
                    overBudget);
        std::puts("");
        download(pixels);
        if (direct) {
            // Hold both pacer references, drop one frame without rendering, and
            // reuse that slot while its previous decode may still be in flight.
            AVFrame* held = prepare(packets[0]);
            CHECK(surfaces->decode(decoder, held) == PyrowaveSurfaces::Decoded);
            AVFrame* dropped = prepare(packets[1]);
            CHECK(surfaces->decode(decoder, dropped) == PyrowaveSurfaces::Decoded);
            AVFrame* reused = prepare(packets[2]);
            CHECK(surfaces->decode(decoder, reused) == PyrowaveSurfaces::PoolBusy);
            av_frame_free(&dropped);
            CHECK(surfaces->decode(decoder, reused) == PyrowaveSurfaces::Decoded);

            std::vector<uint8_t> actual(pixels.size()), expected(pixels.size());
            auto verifySurface = [&](AVFrame* frame, const std::vector<uint8_t>& packet) {
                render(frame);
                download(actual);
                AVFrame* fallback = prepare(packet);
                CHECK(surfaces->decode(decoder, fallback) == PyrowaveSurfaces::PoolBusy);
                readCpu(fallback);
                render(fallback);
                download(expected);
                av_frame_free(&fallback);
                checkPixels(expected, actual, color.hdrMode ? 10 : 8);
            };
            verifySurface(held, packets[0]);
            verifySurface(reused, packets[2]);
            av_frame_free(&held);
            av_frame_free(&reused);
        }
        for (auto& tex : uploads) {
            pl_tex_destroy(gpu, &tex);
        }
        pyrowave_decoder_destroy(decoder);
        pyrowave_device_destroy(device);
    }
    pl_tex_destroy(gpu, &output);
    pl_renderer_destroy(&renderer);
    pl_vulkan_destroy(&vk);
    pl_vk_inst_destroy(&instance);
    return pixels;
}

int main(int argc, char** argv)
{
    if (argc != 7) {
        std::fprintf(
            stderr,
            "Usage: pyrowave_surface width height hdr_mode samples target_fps target_mbps\n");
        return 2;
    }
    int w = std::atoi(argv[1]), h = std::atoi(argv[2]), samples = std::atoi(argv[4]),
        fps = std::atoi(argv[5]);
    PyrowaveColor::Contract color;
    CHECK(w > 0 && h > 0 && w <= 16384 && h <= 16384 && !(w % 2) && !(h % 2) && samples > 1 &&
          fps > 0);
    CHECK(PyrowaveColor::describe(std::atoi(argv[3]), color));
    int mbps = std::atoi(argv[6]);
    CHECK(mbps > 0 && mbps <= 10000);
#ifdef _WIN32
    CHECK(timeBeginPeriod(1) == TIMERR_NOERROR);
#endif
    std::array<std::vector<uint8_t>, 3> packets;
    for (int i = 0; i < 3; ++i) {
        packets[i] = encode(w, h, color, size_t(mbps) * 125000 / fps, i);
    }
    std::printf("packet_bytes=%zu; percentiles=P50/P95/P99\n", packets[0].size());
    std::vector<uint8_t> reference;
    for (bool gpu : { false, true, true, false }) {
        auto pixels = run(gpu, packets, w, h, color, samples, fps);
        if (reference.empty()) {
            reference = std::move(pixels);
        } else {
            checkPixels(reference, pixels, color.hdrMode ? 10 : 8);
        }
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}
