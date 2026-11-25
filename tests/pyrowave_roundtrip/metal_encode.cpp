#include <pyrowave_metal.h>
#include <cstdio>
#include <vector>
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
        fprintf(stderr, "usage: %s output-file\n", argv[0]);
        return 1;
    }
    constexpr int W = 64, H = 64;
    pyrowave_device device = nullptr;
    CHECK(pyrowave_create_default_device(&device));
    pyrowave_encoder_create_info info = {};
    info.device = device;
    info.width = W;
    info.height = H;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder enc = nullptr;
    CHECK(pyrowave_encoder_create(&info, &enc));
    std::vector<unsigned char> y(W * H), u(W * H / 4), v(W * H / 4);
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++)
            y[j * W + i] = (i * 3 + j * 2) % 256;
    for (int j = 0; j < H / 2; j++)
        for (int i = 0; i < W / 2; i++) {
            u[j * W / 2 + i] = 64 + i * 2;
            v[j * W / 2 + i] = 192 - j * 2;
        }
    pyrowave_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = u.data();
    buf.data[2] = v.data();
    buf.row_stride_in_bytes[0] = W;
    buf.row_stride_in_bytes[1] = buf.row_stride_in_bytes[2] = W / 2;
    buf.plane_size_in_bytes[0] = y.size();
    buf.plane_size_in_bytes[1] = u.size();
    buf.plane_size_in_bytes[2] = v.size();
    buf.width = W;
    buf.height = H;
    buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    pyrowave_rate_control rc = { 65536 };
    CHECK(pyrowave_encoder_encode_cpu(enc, &buf, &rc));
    size_t count = 0;
    CHECK(pyrowave_encoder_compute_num_packets(enc, 65536, &count));
    if (count != 1) {
        fprintf(stderr, "unexpected packet count %zu\n", count);
        return 1;
    }
    pyrowave_packet p = {};
    std::vector<unsigned char> out(65536);
    CHECK(pyrowave_encoder_packetize(enc, &p, 65536, &count, out.data(), out.size()));
    FILE* f = fopen(argv[1], "wb");
    if (!f || fwrite(out.data() + p.offset, 1, p.size, f) != p.size)
        return 2;
    fclose(f);
    printf("generated %zu bytes\n", p.size);
    pyrowave_encoder_destroy(enc);
    pyrowave_device_destroy(device);
    return 0;
}
