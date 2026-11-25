#include <vulkan/vulkan.h>
#include <pyrowave.h>
#include "PyrowaveReassembly.h"
#include <cstring>
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        auto r = (x);                                                                              \
        if (r != PYROWAVE_SUCCESS) {                                                               \
            fprintf(stderr, "%s => %d\n", #x, int(r));                                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s input-file\n", argv[0]);
        return 1;
    }
    constexpr int W = 64, H = 64, YS = 96, CS = 64;
    FILE* f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 16 * 1024 * 1024) {
        fclose(f);
        return 1;
    }
    std::vector<unsigned char> in(static_cast<size_t>(n));
    if (fread(in.data(), 1, n, f) != (size_t)n)
        return 1;
    fclose(f);
    // Exercise the actual common-c framing and reassembly with the encoded frame.
    constexpr size_t blockSize = 512;
    const size_t count = (in.size() + blockSize - 1) / blockSize;
    if (count == 0 || count > UINT16_MAX || in.size() > UINT32_MAX)
        return 1;
    LI_PYROWAVE_REASSEMBLY_STATE state;
    LiPyrowaveReassemblyInitialize(&state, 16 * 1024 * 1024);
    for (size_t i = count; i-- > 0;) {
        const size_t offset = i * blockSize;
        const size_t size = std::min(blockSize, in.size() - offset);
        LI_PYROWAVE_PACKET_HEADER h = {};
        h.version = LI_PYROWAVE_PROTOCOL_VERSION;
        h.packetKind = LI_PYROWAVE_PACKET_DATA;
        h.headerLength = LI_PYROWAVE_WIRE_HEADER_SIZE;
        h.frameId = 1;
        h.rtpTimestamp = 90;
        h.codecPayloadLength = h.protectedPayloadLength = static_cast<uint32_t>(in.size());
        h.fecScheme = LI_PYROWAVE_FEC_SCHEME_NONE;
        h.dataBlockCount = h.blockCount = static_cast<uint16_t>(count);
        h.fecBlockPayloadSize = blockSize;
        h.blockIndex = static_cast<uint16_t>(i);
        h.payloadLength = static_cast<uint16_t>(size);
        if (i == 0)
            h.flags |= LI_PYROWAVE_FLAG_START_OF_FRAME;
        if (i + 1 == count)
            h.flags |= LI_PYROWAVE_FLAG_END_OF_FRAME;
        std::vector<uint8_t> packet(LI_PYROWAVE_WIRE_HEADER_SIZE + size);
        size_t packetSize = 0;
        if (LiPyrowaveBuildPacket(&h, in.data() + offset, size, packet.data(), packet.size(),
                                  &packetSize) != LI_PYROWAVE_PACKET_OK)
            return 1;
        auto result = LiPyrowaveReassemblyPushPacket(&state, packet.data(), packetSize, 100, 1000);
        if (result != (i == 0 ? LI_PYROWAVE_REASSEMBLY_COMPLETE : LI_PYROWAVE_REASSEMBLY_ACCEPTED))
            return 1;
    }
    std::vector<uint8_t> reassembled(in.size());
    size_t size = 0;
    if (LiPyrowaveReassemblyCopyFrame(&state, reassembled.data(), reassembled.size(), &size,
                                      nullptr) != LI_PYROWAVE_REASSEMBLY_COMPLETE ||
        size != in.size() || memcmp(in.data(), reassembled.data(), size) != 0)
        return 1;
    LiPyrowaveReassemblyDestroy(&state);
    in.swap(reassembled);
    printf("reassembled %zu bytes across %zu transport packets\n", size, count);
    pyrowave_device device = nullptr;
    CHECK(pyrowave_create_default_device(&device));
    pyrowave_decoder_create_info info = {};
    info.device = device;
    info.width = W;
    info.height = H;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    info.fragment_path = pyrowave_decoder_device_prefers_fragment_path(device);
    printf("fragment path %d\n", int(info.fragment_path));
    pyrowave_decoder dec = nullptr;
    CHECK(pyrowave_decoder_create(&info, &dec));
    std::vector<unsigned char> y(YS * H, 0xCC), u(CS * H / 2, 0xCC), v(CS * H / 2, 0xCC);
    pyrowave_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = u.data();
    buf.data[2] = v.data();
    buf.row_stride_in_bytes[0] = YS;
    buf.row_stride_in_bytes[1] = buf.row_stride_in_bytes[2] = CS;
    buf.plane_size_in_bytes[0] = y.size();
    buf.plane_size_in_bytes[1] = u.size();
    buf.plane_size_in_bytes[2] = v.size();
    buf.width = W;
    buf.height = H;
    buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    int maxY = 0, maxU = 0, maxV = 0;
    long long sum = 0;
    for (int frame = 0; frame < 2; frame++) {
        std::fill(y.begin(), y.end(), 0xCC);
        std::fill(u.begin(), u.end(), 0xCC);
        std::fill(v.begin(), v.end(), 0xCC);
        pyrowave_decoder_clear(dec);
        CHECK(pyrowave_decoder_push_packet(dec, in.data(), in.size()));
        if (!pyrowave_decoder_decode_is_ready(dec, false)) {
            fprintf(stderr, "not ready\n");
            return 1;
        }
        CHECK(pyrowave_decoder_decode_cpu_buffer_synchronous(dec, &buf));
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++) {
                int d = abs(int(y[j * YS + i]) - ((i * 3 + j * 2) % 256));
                if (d > maxY)
                    maxY = d;
                sum += d;
            }
        for (int j = 0; j < H / 2; j++)
            for (int i = 0; i < W / 2; i++) {
                int du = abs(int(u[j * CS + i]) - (64 + i * 2));
                int dv = abs(int(v[j * CS + i]) - (192 - j * 2));
                if (du > maxU)
                    maxU = du;
                if (dv > maxV)
                    maxV = dv;
                sum += du + dv;
            }
    }
    printf("decoded 2 frames %dx%d YUV420P, max abs error Y/U/V %d/%d/%d, average %.4f\n", W, H,
           maxY, maxU, maxV, double(sum) / (2 * W * H * 3 / 2));
    pyrowave_decoder_destroy(dec);
    pyrowave_device_destroy(device);
    return (maxY > 3 || maxU > 3 || maxV > 3) ? 1 : 0;
}
