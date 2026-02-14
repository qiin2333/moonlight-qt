// HLG to PQ conversion for D3D11VA pixel shaders
// Reference: ITU-R BT.2100, SMPTE ST 2084
//
// When the swap chain is set to DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (PQ),
// but the decoded video content uses HLG transfer characteristics, we must
// convert the shader output from HLG gamma to PQ gamma.

float3 hlgToPQ(float3 hlg)
{
    // === Step 1: HLG inverse OETF → scene-referred linear light ===
    // BT.2100 Table 5
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;

    float3 scene;
    [unroll]
    for (int i = 0; i < 3; i++)
    {
        float e = saturate(hlg[i]);
        if (e <= 0.5)
            scene[i] = e * e / 3.0;
        else
            scene[i] = (exp((e - c) / a) + b) / 12.0;
    }

    // === Step 2: HLG OOTF → display-referred linear light ===
    // BT.2100: L_d = alpha * Y_s^(gamma-1) * E_s
    // For 1000 cd/m² nominal peak luminance: gamma ≈ 1.2
    const float gamma_sys = 1.2;
    const float peakLuminance = 1000.0;
    float Ys = dot(scene, float3(0.2627, 0.6780, 0.0593)); // BT.2020 luminance
    float ootfScale = peakLuminance * pow(max(Ys, 1e-6), gamma_sys - 1.0);
    float3 display = scene * ootfScale;

    // === Step 3: Normalize for PQ (10000 cd/m² reference) ===
    display = saturate(display / 10000.0);

    // === Step 4: PQ OETF (ST 2084 inverse EOTF) ===
    const float m1 = 0.1593017578125;  // 2610/16384
    const float m2 = 78.84375;         // 2523/4096 * 128
    const float c1 = 0.8359375;        // 3424/4096
    const float c2 = 18.8515625;       // 2413/4096 * 32
    const float c3 = 18.6875;          // 2392/4096 * 32

    float3 Lm1 = pow(display, m1);
    return pow((c1 + c2 * Lm1) / (1.0 + c3 * Lm1), m2);
}
