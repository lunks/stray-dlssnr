// mvec_selftest.cpp - native replay of every piece of arithmetic src/mvec_decode.hpp runs on the
// GPU, checked against INDEPENDENTLY COMPUTED ground truth rather than against itself.
//
// This is deliberately NOT in src/: build.sh compiles src/*.cpp into the add-on, and this is a
// host-side test with a main(). It builds and runs natively on the build machine:
//
//     c++ -std=c++17 -O2 -Wall -o /tmp/mvec_selftest tools/mvec_selftest.cpp && /tmp/mvec_selftest
//
// It shares no code with the shader - it RESTATES the shader's arithmetic, so a disagreement is a
// real disagreement. The camera-reprojection case builds two genuine view-projection matrices,
// forms ClipToPrevClip = InvViewProj_cur * ViewProj_prev the way SceneView.cpp:2496-2499 does,
// and checks the reconstruction against the difference of the two INDEPENDENT projections of the
// same world point.
//
// The one thing it CANNOT settle is the DLSS sign convention (previous-minus-current), which is
// [WEB]-sourced. It pins everything the host can actually decide, including that the shipped
// matrix convention is right AND that the wrong one is detectable.
//
// Expected: 24 passed, 0 failed.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>

static int g_fail = 0, g_pass = 0;
static void ck(bool c, const char *what, const char *detail = "")
{
    if (c) { ++g_pass; std::printf("  ok   %s %s\n", what, detail); }
    else   { ++g_fail; std::printf("  FAIL %s %s\n", what, detail); }
}
static uint32_t bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// ---------------------------------------------------------------------------- UE 4.27 codec
// Common.ush:1542 / :1558-1561, written as the same expressions UE writes.
static float ue_encode_x(float v) { return v * (0.499f * 0.5f) + 32767.0f / 65535.0f; }
static const float kInvDiv = 1.0f / (0.499f * 0.5f);
static const float kBias   = 32767.0f / 65535.0f;
static float ue_decode_x(float e) { return e * kInvDiv - kBias * kInvDiv; }

// unorm16 quantisation, exactly what an R16G16B16A16_UNORM texel does on write+read.
static float unorm16_rt(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return float(uint32_t(v * 65535.0f + 0.5f)) / 65535.0f;
}

// ---------------------------------------------------------------------------- row-vector 4x4
// UE/FMatrix convention: row-major storage, ROW vectors. m[4*r+c] == M[r][c].
// mul(v, M): out[c] = sum_r v[r]*M[r][c].   This is byte-for-byte mvec_decode::clip_transform
// and the shader's mvMulClipToPrevClip.
struct M4 { float m[16]; };
static void xform(const M4 &A, const float v[4], float o[4])
{
    for (int c = 0; c < 4; ++c)
        o[c] = v[0]*A.m[c] + v[1]*A.m[4+c] + v[2]*A.m[8+c] + v[3]*A.m[12+c];
}
static M4 mul(const M4 &A, const M4 &B)   // (v*A)*B == v*(A*B)
{
    M4 R{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += A.m[4*r+k] * B.m[4*k+c];
            R.m[4*r+c] = s;
        }
    return R;
}
static bool invert(const M4 &A, M4 &out)   // Gauss-Jordan
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c) a[r][c] = A.m[4*r+c];
        for (int c = 0; c < 4; ++c) a[r][4+c] = (r == c) ? 1.0 : 0.0;
    }
    for (int i = 0; i < 4; ++i)
    {
        int piv = i;
        for (int r = i+1; r < 4; ++r) if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        if (std::fabs(a[piv][i]) < 1e-12) return false;
        if (piv != i) for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[piv][c]);
        const double d = a[i][i];
        for (int c = 0; c < 8; ++c) a[i][c] /= d;
        for (int r = 0; r < 4; ++r)
        {
            if (r == i) continue;
            const double f = a[r][i];
            if (f == 0.0) continue;
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out.m[4*r+c] = float(a[r][4+c]);
    return true;
}

// UE reversed-Z perspective, row-vector. Maps view-space z=near -> DeviceZ 1, z=inf -> 0.
static M4 persp_revz(float fov_y_rad, float aspect, float znear)
{
    const float t = 1.0f / std::tan(fov_y_rad * 0.5f);
    M4 P{};
    P.m[0]  = t / aspect;
    P.m[5]  = t;
    P.m[11] = 1.0f;          // w = z   (UE is left-handed, +z forward)
    P.m[14] = znear;         // z' = znear  -> DeviceZ = znear/z  (reversed-Z, infinite far)
    return P;
}
// Row-vector look-at (left-handed, +z forward), world -> view.
static M4 look_at(const float eye[3], const float fwd[3], const float up[3])
{
    auto norm = [](float v[3]) { float l = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); v[0]/=l; v[1]/=l; v[2]/=l; };
    auto cross = [](const float a[3], const float b[3], float o[3]) {
        o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; };
    float z[3] = { fwd[0], fwd[1], fwd[2] }; norm(z);
    float x[3]; cross(up, z, x); norm(x);
    float y[3]; cross(z, x, y);
    M4 V{};
    // Row-vector: world_row * V. Columns of the rotation part are the basis vectors.
    V.m[0]=x[0]; V.m[1]=y[0]; V.m[2]=z[0]; V.m[3]=0.0f;
    V.m[4]=x[1]; V.m[5]=y[1]; V.m[6]=z[1]; V.m[7]=0.0f;
    V.m[8]=x[2]; V.m[9]=y[2]; V.m[10]=z[2]; V.m[11]=0.0f;
    V.m[12]=-(eye[0]*x[0]+eye[1]*x[1]+eye[2]*x[2]);
    V.m[13]=-(eye[0]*y[0]+eye[1]*y[1]+eye[2]*y[2]);
    V.m[14]=-(eye[0]*z[0]+eye[1]*z[1]+eye[2]*z[2]);
    V.m[15]=1.0f;
    return V;
}

// ---------------------------------------------------------------------------- the shader, in C++
// Byte-for-byte the output contract in mvec_decode.hpp's main().
static void out_contract(const float backN[2], float W, float H, float px[2])
{
    px[0] = -0.5f * W * backN[0];
    px[1] =  0.5f * H * backN[1];
}

int main()
{
    std::printf("=== 1. BIT PATTERNS (the two immediates that must appear in STRAY's DXBC)\n");
    {
        const float invdiv = 1.0f / (0.499f * 0.5f);
        const float mad    = 32767.0f / 65535.0f * invdiv;
        char d[160];
        std::snprintf(d, sizeof(d), "InvDiv=%.9g bits=0x%08X (kVelocityDecodeScaleBits)", invdiv, bits(invdiv));
        ck(bits(invdiv) == 0x408041ABu, "InvDiv == 0x408041AB", d);
        std::snprintf(d, sizeof(d), "folded MAD bias=%.9g bits=0x%08X negated=0x%08X",
                      mad, bits(mad), bits(mad) ^ 0x80000000u);
        ck(bits(mad) == 0x4000412Bu, "bias*InvDiv == 0x4000412B", d);
        std::snprintf(d, sizeof(d), "bias=%.17g bits=0x%08X", 32767.0f/65535.0f, bits(32767.0f/65535.0f));
        ck(bits(32767.0f/65535.0f) == 0x3EFFFF00u, "32767/65535 == 0x3EFFFF00 (NOT 0.5)", d);
        ck(bits(0.5f) != bits(32767.0f/65535.0f), "the bias is provably not 0.5", "");
    }

    std::printf("\n=== 2. THE ZERO SENTINEL (why 0.499, not 0.5)\n");
    {
        float lo = 1e9f, hi = -1e9f;
        int   u16lo = 1 << 20, u16hi = -1;
        for (int i = 0; i <= 4000; ++i)
        {
            const float v = -2.0f + 4.0f * (float(i) / 4000.0f);
            const float e = ue_encode_x(v);
            if (e < lo) lo = e;
            if (e > hi) hi = e;
            const int q = int(e * 65535.0f + 0.5f);
            if (q < u16lo) u16lo = q;
            if (q > u16hi) u16hi = q;
        }
        char d[200];
        std::snprintf(d, sizeof(d), "encoded .x over V in [-2,2] spans [%.8f, %.8f], u16 [%d, %d]",
                      lo, hi, u16lo, u16hi);
        ck(lo > 0.0f && u16lo > 0, "exactly-zero is UNREACHABLE for a written texel", d);
        // The counterfactual: with a 0.5 scale factor the range would touch the sentinel.
        const float e_at_minus2_with_half = -2.0f * (0.5f * 0.5f) + 0.5f;
        std::snprintf(d, sizeof(d), "a 0.5/0.5 encode would give exactly %.8f at V=-2", e_at_minus2_with_half);
        ck(e_at_minus2_with_half == 0.0f, "0.499 is load-bearing: 0.5 would collide with the sentinel", d);
    }

    std::printf("\n=== 3. ENCODE -> unorm16 -> DECODE ROUND TRIP\n");
    {
        float worst = 0.0f, worst_at = 0.0f;
        for (int i = 0; i <= 100000; ++i)
        {
            const float v = -2.0f + 4.0f * (float(i) / 100000.0f);
            const float rt = ue_decode_x(unorm16_rt(ue_encode_x(v)));
            const float err = std::fabs(rt - v);
            if (err > worst) { worst = err; worst_at = v; }
        }
        char d[200];
        const float lsb = (1.0f / 65535.0f) * kInvDiv;
        std::snprintf(d, sizeof(d), "worst |err| = %.3e NDC at V=%.4f; quantisation step = %.6e NDC",
                      worst, worst_at, lsb);
        ck(worst <= lsb, "round trip is exact to within one unorm16 LSB", d);
        std::snprintf(d, sizeof(d), "1 LSB = %.6f px in X (960) and %.6f px in Y (540) at 1920x1080",
                      lsb * 960.0f, lsb * 540.0f);
        ck(lsb * 960.0f < 0.06f, "sub-0.06px quantisation, irrelevant for a denoiser", d);
        // The exact sentinel value must decode to ~0 velocity, and the clear value must not be
        // mistaken for a legal small motion.
        std::snprintf(d, sizeof(d), "encode(0) = %.8f decodes back to %.3e", ue_encode_x(0.0f),
                      ue_decode_x(unorm16_rt(ue_encode_x(0.0f))));
        ck(std::fabs(ue_decode_x(unorm16_rt(ue_encode_x(0.0f)))) < 1e-4f, "zero motion round-trips to zero", d);
    }

    std::printf("\n=== 4. CAMERA REPROJECTION vs INDEPENDENT GROUND TRUTH\n");
    // Two real camera poses. ClipToPrevClip = InvViewProj_cur * ViewProj_prev  (SceneView.cpp:2496-9)
    const float W = 1920.0f, H = 1080.0f;
    const M4 P = persp_revz(60.0f * 3.14159265358979f / 180.0f, W / H, 10.0f);
    float eye_c[3] = { 0.0f, 0.0f, 0.0f }, eye_p[3] = { -25.0f, -8.0f, -3.0f };
    float fwd_c[3] = { 0.06f, 0.03f, 1.0f }, fwd_p[3] = { 0.0f, 0.0f, 1.0f };
    float up[3]    = { 0.0f, 1.0f, 0.0f };
    const M4 VP_c = mul(look_at(eye_c, fwd_c, up), P);
    const M4 VP_p = mul(look_at(eye_p, fwd_p, up), P);
    M4 invVP_c;
    ck(invert(VP_c, invVP_c), "current view-projection is invertible", "");
    const M4 C2P = mul(invVP_c, VP_p);
    {
        // Ground truth: project a set of world points through BOTH matrices directly. The
        // reconstruction must agree with the difference of those two independent projections.
        const float pts[6][3] = {
            {  40.0f,  20.0f, 300.0f }, { -90.0f,  35.0f, 640.0f }, { 120.0f, -60.0f, 210.0f },
            {   0.0f,   0.0f, 150.0f }, {-200.0f,-110.0f, 900.0f }, {  15.0f,  70.0f, 1800.0f } };
        float worst = 0.0f;
        for (const auto &wp : pts)
        {
            const float w4[4] = { wp[0], wp[1], wp[2], 1.0f };
            float cc[4], pc[4];
            xform(VP_c, w4, cc);      // independent: world -> current clip
            xform(VP_p, w4, pc);      // independent: world -> previous clip
            const float sx = cc[0]/cc[3], sy = cc[1]/cc[3], dz = cc[2]/cc[3];
            const float px_ = pc[0]/pc[3], py_ = pc[1]/pc[3];
            const float truth[2] = { sx - px_, sy - py_ };   // BackN, ground truth

            // The shader's path: ScreenPos + DeviceZ through ClipToPrevClip only.
            const float tc[4] = { sx, sy, dz, 1.0f };
            float prev[4];
            xform(C2P, tc, prev);
            const float got[2] = { sx - prev[0]/prev[3], sy - prev[1]/prev[3] };
            worst = std::fmax(worst, std::fmax(std::fabs(got[0]-truth[0]), std::fabs(got[1]-truth[1])));
        }
        char d[200];
        std::snprintf(d, sizeof(d), "worst |BackN_reconstructed - BackN_truth| = %.3e NDC over 6 world points", worst);
        ck(worst < 1e-5f, "depth reprojection reproduces the true screen-space delta", d);
    }
    {
        // TRANSPOSE DISCRIMINATION. Identity is transpose-invariant, which is why a still-camera
        // test cannot catch a wrong convention. A general matrix must differ - and the shipped
        // (untransposed) one must be the one that matches ground truth.
        M4 T{};
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) T.m[4*r+c] = C2P.m[4*c+r];
        const float w4[4] = { 120.0f, -60.0f, 210.0f, 1.0f };
        float cc[4], pc[4];
        xform(VP_c, w4, cc); xform(VP_p, w4, pc);
        const float sx = cc[0]/cc[3], sy = cc[1]/cc[3], dz = cc[2]/cc[3];
        const float truth[2] = { sx - pc[0]/pc[3], sy - pc[1]/pc[3] };
        const float tc[4] = { sx, sy, dz, 1.0f };
        float a[4], b[4];
        xform(C2P, tc, a); xform(T, tc, b);
        const float ga[2] = { sx - a[0]/a[3], sy - a[1]/a[3] };
        const float gb[2] = { sx - b[0]/b[3], sy - b[1]/b[3] };
        const float ea = std::fabs(ga[0]-truth[0]) + std::fabs(ga[1]-truth[1]);
        const float eb = std::fabs(gb[0]-truth[0]) + std::fabs(gb[1]-truth[1]);
        char d[220];
        std::snprintf(d, sizeof(d), "untransposed err=%.3e, transposed err=%.3e (%.0fx worse)", ea, eb,
                      (double)eb/(ea>0.0f?ea:1e-9));
        ck(ea < 1e-5f && eb > 1e-3f, "row-major untransposed is correct AND transpose is detectably wrong", d);
    }

    std::printf("\n=== 5. THE SIGN CONVENTION (X negates, Y does not)\n");
    {
        // Pure RIGHT strafe: camera moves +X, static world. On screen the world moves LEFT, so a
        // surface's PREVIOUS position is to its RIGHT => MV.x must be POSITIVE.
        float ec[3] = { 0.0f, 0.0f, 0.0f }, ep[3] = { -30.0f, 0.0f, 0.0f };
        float fw[3] = { 0.0f, 0.0f, 1.0f };
        const M4 A = mul(look_at(ec, fw, up), P), B = mul(look_at(ep, fw, up), P);
        M4 iA; invert(A, iA);
        const M4 K = mul(iA, B);
        const float w4[4] = { 0.0f, 0.0f, 400.0f, 1.0f };
        float cc[4]; xform(A, w4, cc);
        const float sx = cc[0]/cc[3], sy = cc[1]/cc[3], dz = cc[2]/cc[3];
        const float tc[4] = { sx, sy, dz, 1.0f };
        float pv[4]; xform(K, tc, pv);
        const float backN[2] = { sx - pv[0]/pv[3], sy - pv[1]/pv[3] };
        float px[2]; out_contract(backN, W, H, px);
        char d[240];
        std::snprintf(d, sizeof(d), "strafe RIGHT: BackN.x=%+.6f (NDC) -> MV.x=%+.2f px", backN[0], px[0]);
        ck(backN[0] < 0.0f && px[0] > 0.0f, "MV.x POSITIVE: history lies to the RIGHT", d);

        // PITCH, both ways, asserting the PHYSICAL direction rather than mere sign agreement.
        // Camera pitches UP between prev and cur  -> world slides DOWN the screen -> a surface's
        // previous position is ABOVE its current one -> in a y-DOWN grid that is a SMALLER y,
        // so MV.y must be NEGATIVE. Pitching DOWN must give the opposite.
        for (int dir = 0; dir < 2; ++dir)
        {
            const float tilt = (dir == 0) ? -0.25f : +0.25f;   // previous camera's pitch
            float fp[3] = { 0.0f, tilt, 1.0f };
            const M4 Bp = mul(look_at(ec, fp, up), P);
            M4 iA2; invert(A, iA2);
            const M4 K2 = mul(iA2, Bp);
            float pv2[4]; xform(K2, tc, pv2);
            const float bn[2] = { sx - pv2[0]/pv2[3], sy - pv2[1]/pv2[3] };
            float p2[2]; out_contract(bn, W, H, p2);
            const bool pitched_up = (dir == 0);   // prev looked DOWN, so the camera pitched UP
            std::snprintf(d, sizeof(d),
                "camera pitched %s: BackN.y=%+.6f -> MV.y=%+.2f px (history is %s)",
                pitched_up ? "UP  " : "DOWN", bn[1], p2[1], p2[1] < 0.0f ? "ABOVE" : "BELOW");
            ck(pitched_up ? (p2[1] < 0.0f) : (p2[1] > 0.0f),
               pitched_up ? "pitch UP   => MV.y NEGATIVE (history above)"
                          : "pitch DOWN => MV.y POSITIVE (history below)", d);
        }

        // The asymmetry itself, stated as an invariant.
        const float b3[2] = { 0.1f, 0.1f };
        float p3[2]; out_contract(b3, W, H, p3);
        std::snprintf(d, sizeof(d), "BackN=(+0.1,+0.1) -> MV=(%+.1f, %+.1f) px", p3[0], p3[1]);
        ck(p3[0] < 0.0f && p3[1] > 0.0f, "X and Y carry OPPOSITE signs for equal input", d);
        // And the worked example from the header comment.
        const float b4[2] = { -0.104167f, 0.0f };
        float p4[2]; out_contract(b4, W, H, p4);
        std::snprintf(d, sizeof(d), "BackN.x=-0.104167 -> MV.x=%+.3f px (header claims +100.0)", p4[0]);
        ck(std::fabs(p4[0] - 100.0f) < 0.01f, "the header's worked example reproduces", d);
    }

    std::printf("\n=== 6. ROOT-CONSTANT BLOCK vs HLSL cbuffer PACKING\n");
    {
        struct mvec_args {
            uint32_t out_w, out_h, vel_w, vel_h, depth_w, depth_h;
            float view_min_x, view_min_y, view_size_x, view_size_y, inv_view_x, inv_view_y;
            uint32_t flags, pad0, pad1, pad2;
            float clip[16];
        };
        char d[200];
        std::snprintf(d, sizeof(d), "sizeof=%zu bytes = %zu dwords", sizeof(mvec_args), sizeof(mvec_args)/4);
        ck(sizeof(mvec_args) == 128 && sizeof(mvec_args)/4 == 32, "128 bytes / 32 root constants", d);
        // HLSL packs each 2-vector pair into one float4 register; the array must land on c4.
        std::snprintf(d, sizeof(d), "clip[] at byte offset %zu = register c%zu",
                      offsetof(mvec_args, clip), offsetof(mvec_args, clip)/16);
        ck(offsetof(mvec_args, clip) == 64, "g_clipToPrevClip starts exactly on register c4", d);
        ck(offsetof(mvec_args, flags) == 48, "g_flags lands on c3.x", "");
        ck(offsetof(mvec_args, view_size_x) == 32, "g_viewSize lands on c2.xy", "");
    }

    std::printf("\n=== 7. mvRemap / ViewportUVToScreenPos edge behaviour\n");
    {
        // ScreenPos must be y-UP over the view rect: pixel row 0 -> +1, last row -> -1.
        auto sp = [&](float cx, float cy) {
            return std::pair<float,float>(2.0f*(cx/W)-1.0f, 1.0f-2.0f*(cy/H)); };
        auto a = sp(0.5f, 0.5f);
        auto b = sp(W-0.5f, H-0.5f);
        char d[200];
        std::snprintf(d, sizeof(d), "px(0,0)->ScreenPos(%+.6f,%+.6f)  px(1919,1079)->(%+.6f,%+.6f)",
                      a.first, a.second, b.first, b.second);
        ck(a.second > 0.0f && b.second < 0.0f && a.first < 0.0f && b.first > 0.0f,
           "top-left is (-,+) and bottom-right is (+,-): y is UP", d);
        auto c = sp(W*0.5f, H*0.5f);
        std::snprintf(d, sizeof(d), "centre -> (%+.6f,%+.6f)", c.first, c.second);
        ck(std::fabs(c.first) < 1e-6f && std::fabs(c.second) < 1e-6f, "screen centre maps to the origin", d);
    }

    std::printf("\n=== 8. IDENTITY ClipToPrevClip => ZERO MOTION EVERYWHERE (the still-camera check)\n");
    {
        M4 I{}; I.m[0]=I.m[5]=I.m[10]=I.m[15]=1.0f;
        float worst = 0.0f;
        for (float sx = -1.0f; sx <= 1.0f; sx += 0.5f)
            for (float sy = -1.0f; sy <= 1.0f; sy += 0.5f)
                for (float dz : { 0.0f, 0.25f, 0.5f, 1.0f })
                {
                    const float v[4] = { sx, sy, dz, 1.0f };
                    float o[4]; xform(I, v, o);
                    if (!(o[3] > 1e-6f)) continue;
                    worst = std::fmax(worst, std::fabs(sx - o[0]/o[3]));
                    worst = std::fmax(worst, std::fabs(sy - o[1]/o[3]));
                }
        char d[160];
        std::snprintf(d, sizeof(d), "worst BackN over a 5x5x4 sample grid = %.3e", worst);
        ck(worst == 0.0f, "a still camera produces exactly zero motion", d);
    }

    std::printf("\n----------------------------------------------------------------\n");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
