#include <pyrowave.h>

int main()
{
    // Verify the AlkaidLab color API at compile and link time. A package with
    // the same name is not sufficient, and this probe never needs a GPU.
    pyrowave_color_metadata metadata = {};
    metadata.transfer = PYROWAVE_TRANSFER_HLG;
    pyrowave_cpu_buffer buffer = {};
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10;
    bool (*volatile readMetadata)(pyrowave_decoder, pyrowave_color_metadata*) =
        &pyrowave_decoder_get_color_metadata;
    return readMetadata != nullptr && metadata.transfer == PYROWAVE_TRANSFER_HLG &&
                   buffer.format == PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10
               ? 0
               : 1;
}
