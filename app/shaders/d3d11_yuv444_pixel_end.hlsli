#include "d3d11_hlg_to_pq.hlsli"

min16float4 main(ShaderInput input) : SV_TARGET
{
    min16float3 yuv = swizzle(videoTex.Sample(theSampler, input.tex));

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    // Apply HLG→PQ EOTF conversion if content is HLG-encoded
    if (hlgMode > 0.5)
        yuv = (min16float3)hlgToPQ((float3)yuv);

    return min16float4(yuv, 1.0);
}