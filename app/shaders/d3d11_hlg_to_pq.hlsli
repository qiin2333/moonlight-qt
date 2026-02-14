// HLG to PQ conversion for D3D11VA pixel shaders
// Reference: ITU-R BT.2100, SMPTE ST 2084
//
// When the swap chain is set to DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (PQ),
// but the decoded video content uses HLG transfer characteristics, we must
// convert the shader output from HLG gamma to PQ gamma.
//
// NOTE: Sunshine encodes HLG as display-referred content (scRGB linear → ÷1000 nits →
// HLG OETF) without applying the inverse OOTF. Therefore, the client must NOT apply
// the forward OOTF during conversion. We simply do:
//   HLG inverse OETF → display-referred linear [0,1] (1.0 = 1000 nits) → PQ OETF.

float3 hlgToPQ(float3 hlg)
{
    // === Step 1: HLG inverse OETF → display-referred linear light ===
    // BT.2100 Table 5 (inverse OETF only, no OOTF)
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;

    float3 E;
    [unroll]
    for (int i = 0; i < 3; i++)
    {
        float e = saturate(hlg[i]);
        if (e <= 0.5)
            E[i] = e * e / 3.0;
        else
            E[i] = (exp((e - c) / a) + b) / 12.0;
    }

    // === Step 2: Scale to PQ absolute luminance range ===
    // E is in [0,1] where 1.0 = 1000 cd/m² (Sunshine's HLG peak luminance).
    // PQ reference is 10000 cd/m², so scale by 1000/10000 = 0.1.
    float3 pqNorm = saturate(E * 0.1);

    // === Step 3: PQ OETF (ST 2084 inverse EOTF) ===
    const float m1 = 0.1593017578125;  // 2610/16384
    const float m2 = 78.84375;         // 2523/4096 * 128
    const float c1 = 0.8359375;        // 3424/4096
    const float c2 = 18.8515625;       // 2413/4096 * 32
    const float c3 = 18.6875;          // 2392/4096 * 32

    float3 Lm1 = pow(pqNorm, m1);
    return pow((c1 + c2 * Lm1) / (1.0 + c3 * Lm1), m2);
}
