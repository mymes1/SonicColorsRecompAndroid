// Vulkan GLSL generation for the GX transform unit and the TEV pixel pipeline.
//
// Mirrors shadergen.cpp (the HLSL path): every TEV operation is integer arithmetic with
// the same ordering, so the Android image matches the PC image. Differences are only
// syntactic (GLSL type names, explicit float/int conversions, std140 constants).
#include "shadergen_glsl.h"
#include "vertex_loader.h"

#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif

namespace
{
void W(std::string& s, const std::string& t) { s += t; }

// ---------------------------------------------------------------- declarations
const char* kLayout = R"(#version 450
#extension GL_ARB_separate_shader_objects : enable
// The backend binds one identical descriptor set for both stages:
//   0 XF memory (uniform float array, 0x680 floats)  1 constants (std140)
//   2..9 textures   10..17 samplers
layout(std430, set = 0, binding = 0) readonly buffer XFMem { float xf[]; };
)";

const char* kVSConstants = R"(
layout(std140, set = 0, binding = 1) uniform VSConst
{
    mat4 proj;
    vec4 depth;            // x = farZ/2^24, y = zRange/2^24, z = flip x, w = flip y
    ivec4 materials[4];    // amb0, amb1, mat0, mat1 (0..255)
    vec4 pixelOffset;      // xy: subpixel correction in clip units
} vs;
)";

const char* kPSConstants = R"(
// Binding 11: the vertex stage's constants live at binding 1, so the two stages never share
// a buffer (a descriptor set serves every stage of the pipeline).
layout(std140, set = 0, binding = 11) uniform PSConst
{
    ivec4 colors[4];
    ivec4 konst[4];
    ivec4 alphaRef;
    ivec4 texDim[8];
    ivec4 indScale[4];
    ivec4 indMtx[6];
    ivec4 zBias[2];
    vec4 fogF;
    ivec4 fogI;
    ivec4 fogColor;
    vec4 fogRange[3];
    vec4 depthParams;
} ps;
)";

const char* kHelpers = R"(
vec4 xrow(uint r) { uint b = r * 4u; return vec4(xf[b], xf[b + 1u], xf[b + 2u], xf[b + 3u]); }
vec3 nrow(uint b) { return vec3(xf[b], xf[b + 1u], xf[b + 2u]); }
vec3 xf3(uint b) { return vec3(xf[b], xf[b + 1u], xf[b + 2u]); }
uint xfu(uint b) { return floatBitsToUint(xf[b]); }
// bit test instead of isnan(); no NaN support needed, same as the HLSL path
bool isNaN_(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u; }
ivec4 lightColor(uint i)
{
    uint c = xfu(0x600u + i * 16u + 3u);
    return ivec4(int(c >> 24u), int((c >> 16u) & 0xFFu), int((c >> 8u) & 0xFFu), int(c & 0xFFu));
}
)";

// ---------------------------------------------------------------- vertex lighting
// Mirrors GenLight in shadergen.cpp.
void GenLight(std::string& s, uint32_t chanReg, int light, bool alpha)
{
    uint32_t diffuse = (chanReg >> 7) & 3;
    uint32_t attn = (chanReg >> 9) & 3;
    std::string b = std::format("0x{:X}u", 0x600 + light * 16);
    if (attn == 1)
    {
        W(s, std::format("\t\tldir = normalize(xf3({0}u + 10u) - pos);\n", b));
        W(s, std::format("\t\tattn = (dot(n, ldir) >= 0.0) ? max(0.0, dot(n, xf3({0}u + 13u))) : 0.0;\n", b));
        W(s, std::format("\t\tcosAttn = xf3({0}u + 4u);\n", b));
        W(s, std::format("\t\tdistAttn = {1}(xf3({0}u + 7u));\n", b, diffuse == 0 ? "" : "normalize"));
        W(s, "\t\tattn = max(0.0, dot(cosAttn, vec3(1.0, attn, attn * attn))) / dot(distAttn, vec3(1.0, attn, attn * attn));\n");
    }
    else if (attn == 3)
    {
        W(s, std::format("\t\tldir = xf3({0}u + 10u) - pos;\n", b));
        W(s, "\t\tdist2 = dot(ldir, ldir); dist = sqrt(dist2); ldir = ldir / dist;\n");
        W(s, std::format("\t\tattn = max(0.0, dot(ldir, xf3({0}u + 13u)));\n", b));
        W(s, std::format("\t\tattn = max(0.0, xf[{0}u + 4u] + xf[{0}u + 5u] * attn + xf[{0}u + 6u] * attn * attn) / "
                         "dot(xf3({0}u + 7u), vec3(1.0, dist, dist2));\n", b));
    }
    else
    {
        W(s, std::format("\t\tldir = normalize(xf3({0}u + 10u) - pos);\n", b));
        W(s, "\t\tattn = 1.0;\n");
        W(s, "\t\tif (length(ldir) == 0.0) ldir = n;\n");
    }
    // DiffuseFunc: 0 = none, 1 = sign(dot), 2 = clamped dot, 3 = raw dot
    std::string factor;
    if (diffuse == 0) factor = "1.0";
    else if (diffuse == 2) factor = "max(0.0, dot(ldir, n))";
    else factor = "dot(ldir, n)";
    if (alpha)
        W(s, std::format("\t\tlacc.w += int(round(attn * ({}) * float(lightColor({}u).a)));\n", factor, light));
    else
        W(s, std::format("\t\tlacc.rgb += ivec3(round(attn * ({}) * vec3(lightColor({}u).rgb)));\n", factor, light));
}

void GenLightingChannel(std::string& s, const VSKey& k, int j)
{
    uint32_t c = k.chan[j];
    uint32_t a = k.chan[j + 2];
    W(s, std::format("vec4 lighting{}(vec4 base, vec3 pos, vec3 n)\n{{\n", j));
    W(s, "\tivec4 lacc;\n\tvec3 ldir = vec3(0.0), cosAttn = vec3(0.0), distAttn = vec3(1.0);\n\tfloat dist = 0.0, dist2 = 0.0, attn = 0.0;\n");
    bool cMatVtx = (c & 1) != 0, aMatVtx = (a & 1) != 0;
    W(s, cMatVtx ? "\tivec4 mat = ivec4(round(base * 255.0));\n" : std::format("\tivec4 mat = vs.materials[{}];\n", j + 2));
    if (c & 2)
        W(s, (c & 0x40) ? "\tlacc = ivec4(round(base * 255.0));\n" : std::format("\tlacc = vs.materials[{}];\n", j));
    else
        W(s, "\tlacc = ivec4(255, 255, 255, 255);\n");
    if (aMatVtx != cMatVtx)
        W(s, aMatVtx ? "\tmat.w = int(round(base.w * 255.0));\n" : std::format("\tmat.w = vs.materials[{}].w;\n", j + 2));
    if (a & 2)
        W(s, (a & 0x40) ? "\tlacc.w = int(round(base.w * 255.0));\n" : std::format("\tlacc.w = vs.materials[{}].w;\n", j));
    else
        W(s, "\tlacc.w = 255;\n");
    auto mask = [](uint32_t r) { return ((r >> 2) & 0xF) | (((r >> 11) & 0xF) << 4); };
    if (c & 2)
        for (int i = 0; i < 8; i++)
            if (mask(c) & (1 << i)) { W(s, "\t{\n"); GenLight(s, c, i, false); W(s, "\t}\n"); }
    if (a & 2)
        for (int i = 0; i < 8; i++)
            if (mask(a) & (1 << i)) { W(s, "\t{\n"); GenLight(s, a, i, true); W(s, "\t}\n"); }
    W(s, "\tlacc = clamp(lacc, ivec4(0), ivec4(255));\n");
    // hardware 8-bit multiply: (mat * (lacc + (lacc >> 7))) >> 8
    W(s, "\treturn vec4(mat * (lacc + (lacc >> 7))) / 255.0;\n}\n");
}

// ---------------------------------------------------------------- vertex shader
std::string VertexShader(const VSKey& k)
{
    std::string s = kLayout;
    s += kVSConstants;
    s += kHelpers;
    // Vertex inputs, one location each, matching the vertex binding in
    // runtime/platform/android/renderer_vk.cpp (HostVertex). Matrices indices are the one
    // packed ubyte4 the vertex loader produces for the three 32-bit index words.
    s += R"(
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNrm;
layout(location = 2) in vec3 inBin;
layout(location = 3) in vec3 inTan;
layout(location = 4) in vec4 inCol0;
layout(location = 5) in vec4 inCol1;
layout(location = 6) in vec2 inTex0;
layout(location = 7) in vec2 inTex1;
layout(location = 8) in vec2 inTex2;
layout(location = 9) in vec2 inTex3;
layout(location = 10) in vec2 inTex4;
layout(location = 11) in vec2 inTex5;
layout(location = 12) in vec2 inTex6;
layout(location = 13) in vec2 inTex7;
layout(location = 14) in uvec4 inMtx;   // 9 matrix indices, 8 bits each: 4 + 4 + 1
)";
    GenLightingChannel(s, k, 0);
    GenLightingChannel(s, k, 1);
    for (uint32_t i = 0; i < k.numTexGens; i++)
        W(s, std::format("layout(location = {}) out vec3 vTex{};\n", 2 + i, i));
    W(s, "layout(location = 0) out vec4 vCol0;\nlayout(location = 1) out vec4 vCol1;\nlayout(location = 10) out vec4 vClip;\n");
    W(s, "\nvoid main()\n{\n");
    W(s, "\tuint mtx[9] = uint[9](inMtx.x & 0xFFu, (inMtx.x >> 8u) & 0xFFu, (inMtx.x >> 16u) & 0xFFu, inMtx.x >> 24u,\n"
         "\t                      inMtx.y & 0xFFu, (inMtx.y >> 8u) & 0xFFu, (inMtx.y >> 16u) & 0xFFu, inMtx.y >> 24u,\n"
         "\t                      inMtx.z & 0xFFu);\n");
    W(s, "\tvec4 rawpos = vec4(inPos, 1.0);\n");
    W(s, "\tuint pidx = mtx[0];\n");
    W(s, "\tvec4 pos = vec4(dot(xrow(pidx), rawpos), dot(xrow(pidx + 1u), rawpos), dot(xrow(pidx + 2u), rawpos), 1.0);\n");
    W(s, "\tuint nb = 0x400u + (pidx & 31u) * 3u;\n");
    bool hasN = (k.components & VC_NRM) != 0;
    if (hasN)
    {
        W(s, "\tvec3 n = normalize(vec3(dot(nrow(nb), inNrm), dot(nrow(nb + 3u), inNrm), dot(nrow(nb + 6u), inNrm)));\n");
        W(s, "\tvec3 tt = vec3(dot(nrow(nb), inTan), dot(nrow(nb + 3u), inTan), dot(nrow(nb + 6u), inTan));\n");
        W(s, "\tvec3 bb = vec3(dot(nrow(nb), inBin), dot(nrow(nb + 3u), inBin), dot(nrow(nb + 6u), inBin));\n");
    }
    else
    {
        W(s, "\tvec3 n = vec3(0.0, 0.0, 1.0);\n\tvec3 tt = vec3(0.0);\n\tvec3 bb = vec3(0.0);\n");
    }
    bool c0 = (k.components & VC_COL0) != 0, c1 = (k.components & VC_COL1) != 0;
    W(s, std::format("\tvec4 vc0 = {};\n", c0 ? "inCol0" : (c1 ? "inCol1" : "vec4(1.0)")));
    W(s, std::format("\tvec4 vc1 = {};\n", (c0 && c1) ? "inCol1" : "vec4(1.0)"));
    W(s, "\tvCol0 = lighting0(vc0, pos.xyz, n);\n");
    W(s, "\tvCol1 = lighting1(vc1, pos.xyz, n);\n");
    if (k.numColorChans == 0) W(s, "\tvCol0 = vec4(0.0);\n");
    if (k.numColorChans < 2) W(s, "\tvCol1 = vec4(0.0);\n");

    // texgens
    for (uint32_t i = 0; i < k.numTexGens; i++)
    {
        uint32_t info = k.texMtxInfo[i];
        uint32_t projection = (info >> 1) & 1;
        uint32_t inputForm = (info >> 2) & 1;
        uint32_t type = (info >> 4) & 7;
        uint32_t srcRow = (info >> 7) & 31;
        uint32_t embossSrc = (info >> 12) & 7;
        uint32_t embossLight = (info >> 15) & 7;
        W(s, "\t{\n\t\tvec4 coord = vec4(0.0, 0.0, 1.0, 1.0);\n");
        switch (srcRow)
        {
        case 0: W(s, "\t\tcoord = vec4(rawpos.xyz, 1.0);\n"); break;
        case 1: if (hasN) W(s, "\t\tcoord = vec4(inNrm, 1.0);\n"); break;
        case 2: break;
        case 3: if (k.components & VC_NBT) W(s, "\t\tcoord = vec4(inTan, 1.0);\n"); break;
        case 4: if (k.components & VC_NBT) W(s, "\t\tcoord = vec4(inBin, 1.0);\n"); break;
        default:
        {
            uint32_t t = srcRow - 5;
            if (t < 8 && (k.components & (VC_TEX0 << t)))
                W(s, std::format("\t\tcoord = vec4(inTex{0}.xy, 1.0, 1.0);\n", t));
            break;
        }
        }
        if (inputForm == 0) W(s, "\t\tcoord.z = 1.0;\n");
        W(s, "\t\tif (isNaN_(coord.x)) coord.x = 1.0;\n\t\tif (isNaN_(coord.y)) coord.y = 1.0;\n\t\tif (isNaN_(coord.z)) coord.z = 1.0;\n");
        W(s, "\t\tvec3 r = vec3(0.0);\n");
        if (type == 0)
        {
            W(s, std::format("\t\tuint t = mtx[{}];\n", 1 + i));
            if (projection)
                W(s, "\t\tr = vec3(dot(xrow(t), coord), dot(xrow(t + 1u), coord), dot(xrow(t + 2u), coord));\n");
            else
                W(s, "\t\tr = vec3(dot(xrow(t), coord), dot(xrow(t + 1u), coord), 1.0);\n");
        }
        else if (type == 1)
        {
            W(s, std::format("\t\tvec3 ldir = normalize(xf3(0x{:X}u + 10u) - pos.xyz);\n", 0x600 + embossLight * 16));
            W(s, std::format("\t\tr = vTex{} + vec3(dot(ldir, tt), dot(ldir, bb), 0.0);\n", embossSrc < i ? embossSrc : 0));
        }
        else if (type == 2)
            W(s, "\t\tr = vec3(vCol0.x, vCol0.y, 1.0);\n");
        else
            W(s, "\t\tr = vec3(vCol1.x, vCol1.y, 1.0);\n");
        if (k.dualTex && type == 0)
        {
            uint32_t post = k.postMtxInfo[i];
            uint32_t base = (post & 0x3F) * 4;
            if ((post >> 8) & 1) W(s, "\t\tr = normalize(r);\n");
            W(s, std::format("\t\tvec4 rp = vec4(r, 1.0);\n"
                             "\t\tr = vec3(dot(vec4(xf[0x500u+{0}u], xf[0x501u+{0}u], xf[0x502u+{0}u], xf[0x503u+{0}u]), rp), "
                             "dot(vec4(xf[0x504u+{0}u], xf[0x505u+{0}u], xf[0x506u+{0}u], xf[0x507u+{0}u]), rp), "
                             "dot(vec4(xf[0x508u+{0}u], xf[0x509u+{0}u], xf[0x50Au+{0}u], xf[0x50Bu+{0}u]), rp));\n", base));
        }
        // hardware special case when q is 0 (Dolphin: neheGX lesson 8, Rogue Squadron 3)
        if (type == 0) W(s, "\t\tif (r.z == 0.0) r.xy = clamp(r.xy / 2.0, vec2(-1.0), vec2(1.0));\n");
        W(s, std::format("\t\tvTex{} = r;\n\t}}\n", i));
    }

    // GX depth: farZ + zRange * z/w, in 2^24 units; same expression as the HLSL path
    W(s, "\tvec4 clipPos = vs.proj * pos;\n");
    W(s, "\tvClip = clipPos;\n");
    W(s, "\tvec4 outPos = clipPos;\n");
    W(s, "\toutPos.z = outPos.z * (1.0 - 1e-7);\n");
    W(s, "\toutPos.z = outPos.w * vs.depth.x + outPos.z * vs.depth.y;\n");
    W(s, "\toutPos.xy *= vs.depth.zw;\n");
    W(s, "\toutPos.xy += vs.pixelOffset.xy * outPos.w;\n");
    W(s, "\tgl_Position = outPos;\n");
    W(s, "}\n");
    return s;
}

// ---------------------------------------------------------------- pixel shader
const char* kColorIn[16] = {
    "prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "textemp.rgb", "textemp.aaa", "rastemp.rgb", "rastemp.aaa", "ivec3(255,255,255)", "ivec3(128,128,128)", "konsttemp.rgb", "ivec3(0,0,0)",
};
const char* kAlphaIn[8] = { "prev.a", "c0.a", "c1.a", "c2.a", "textemp.a", "rastemp.a", "konsttemp.a", "0" };
const char* kOut[4] = { "prev", "c0", "c1", "c2" };
const char* kKC[32] = {
    "ivec3(255,255,255)", "ivec3(223,223,223)", "ivec3(191,191,191)", "ivec3(159,159,159)", "ivec3(128,128,128)", "ivec3(96,96,96)", "ivec3(64,64,64)", "ivec3(32,32,32)",
    "ivec3(0,0,0)", "ivec3(0,0,0)", "ivec3(0,0,0)", "ivec3(0,0,0)",
    "ps.konst[0].rgb", "ps.konst[1].rgb", "ps.konst[2].rgb", "ps.konst[3].rgb",
    "ps.konst[0].rrr", "ps.konst[1].rrr", "ps.konst[2].rrr", "ps.konst[3].rrr",
    "ps.konst[0].ggg", "ps.konst[1].ggg", "ps.konst[2].ggg", "ps.konst[3].ggg",
    "ps.konst[0].bbb", "ps.konst[1].bbb", "ps.konst[2].bbb", "ps.konst[3].bbb",
    "ps.konst[0].aaa", "ps.konst[1].aaa", "ps.konst[2].aaa", "ps.konst[3].aaa",
};
const char* kKA[32] = {
    "255", "223", "191", "159", "128", "96", "64", "32",
    "0", "0", "0", "0", "0", "0", "0", "0",
    "ps.konst[0].r", "ps.konst[1].r", "ps.konst[2].r", "ps.konst[3].r",
    "ps.konst[0].g", "ps.konst[1].g", "ps.konst[2].g", "ps.konst[3].g",
    "ps.konst[0].b", "ps.konst[1].b", "ps.konst[2].b", "ps.konst[3].b",
    "ps.konst[0].a", "ps.konst[1].a", "ps.konst[2].a", "ps.konst[3].a",
};
const char* kRas[8] = {
    "ivec4(round(vCol0 * 255.0))", "ivec4(round(vCol1 * 255.0))", "ivec4(0)", "ivec4(0)", "ivec4(0)",
    "ivec4(alphabump)", "ivec4(alphabump | (alphabump >> 5))", "ivec4(0)",
};

// One component of a stage's wrapped texcoord. GX wrap modes: 0 = clamp to the edge, 1..5 =
// repeat/mirror over 256>>(w-1) texels of 7-bit fixed point, 6 and up = drop the coordinate.
void WrapCoord(std::string& s, uint32_t w, char c, uint32_t texcoord)
{
    // .2 is the component character: a format string cannot mix {} with {0} numbering, so the
    // character is always passed explicitly.
    if (w == 0) W(s, std::format("\twrappedcoord.{0} = fixpoint_uv{1}.{0};\n", c, texcoord));
    else if (w >= 6) W(s, std::format("\twrappedcoord.{0} = 0;\n", c));
    else
    {
        const int32_t span = (256 << 7) >> (w - 1);
        W(s, std::format("\twrappedcoord.{0} = fixpoint_uv{1}.{0} & {2};\n", c, texcoord, span - 1));
    }
}

std::string TevRegular(const std::string& comp, uint32_t bias, uint32_t op, uint32_t scale)
{
    static const char* scaleL[4] = { "", " << 1", " << 2", "" };
    static const char* scaleR[4] = { "", "", "", " >> 1" };
    static const char* lerpBias[2] = { " + 128", " + 127" };
    static const char* biasT[4] = { "", " + 128", " - 128", "" };
    return std::format("(((tevin_d.{0}{1}){2}) {3} (((((tevin_a.{0} << 8) + (tevin_b.{0} - tevin_a.{0}) * "
                       "(tevin_c.{0} + (tevin_c.{0} >> 7))){2}){4}) >> 8)){5}",
                       comp, biasT[bias], scaleL[scale], op ? "-" : "+", scale != 3 ? lerpBias[op] : "", scaleR[scale]);
}

std::string SampleTexFn()
{
    std::string s = R"(
ivec4 sampleTex(uint m, ivec2 uv, ivec2 dim)
{
    vec2 c = vec2(uv) / (128.0 * vec2(dim));
    vec4 r;
    switch (m)
    {
)";
    for (int i = 0; i < 8; i++)
        s += std::format("    case {}u: r = textureLod(tex{}, c, 0.0); break;\n", i, i);
    s += "    default: r = textureLod(tex0, c, 0.0); break;\n    }\n    return ivec4(round(r * 255.0));\n}\n";
    return s;
}

std::string PixelShader(const PSKey& k)
{
    std::string s = kLayout;
    s += kPSConstants;
    s += kHelpers;
    for (int i = 0; i < 8; i++)
        s += std::format("layout(set = 0, binding = {}) uniform sampler2D tex{};\n", 2 + i, i);
    s += "int idot3(ivec3 a, ivec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }\n";
    s += SampleTexFn();
    for (uint32_t i = 0; i < k.numTexGens; i++)
        W(s, std::format("layout(location = {}) in vec3 vTex{};\n", 2 + i, i));
    W(s, "layout(location = 0) in vec4 vCol0;\nlayout(location = 1) in vec4 vCol1;\nlayout(location = 10) in vec4 vClip;\n");
    W(s, "layout(location = 0) out vec4 oCol0;\nlayout(location = 1) out vec4 oCol1;\n");
    W(s, "\nvoid main()\n{\n");
    W(s, "\tivec4 c0 = ps.colors[1], c1 = ps.colors[2], c2 = ps.colors[3], prev = ps.colors[0];\n");
    W(s, "\tivec4 rastemp = ivec4(0), textemp = ivec4(0), konsttemp = ivec4(0);\n");
    W(s, "\tint alphabump = 0;\n\tivec3 tevcoord = ivec3(0);\n\tivec2 wrappedcoord = ivec2(0);\n");
    W(s, "\tivec4 tevin_a, tevin_b, tevin_c, tevin_d;\n");

    // 7-bit fixed point texcoords in texels of the stage's texmap
    for (uint32_t t = 0; t < k.numTexGens; t++)
    {
        int map = -1;
        for (uint32_t st = 0; st < k.numTevStages && map < 0; st++)
        {
            uint32_t ord = (k.tevOrder[st / 2] >> ((st & 1) * 12)) & 0xFFF;
            if (((ord >> 3) & 7) == t) map = ord & 7;
        }
        for (uint32_t is = 0; is < k.numIndStages && map < 0; is++)
            if (((k.indOrder >> (6 * is + 3)) & 7) == t) map = (k.indOrder >> (6 * is)) & 7;
        if (map < 0) map = 0;
        W(s, std::format("\tvec2 uvf{0} = (vTex{0}.z == 0.0) ? vTex{0}.xy : vTex{0}.xy / vTex{0}.z;\n", t));
        W(s, std::format("\tivec2 fixpoint_uv{0} = ivec2(uvf{0} * vec2(ps.texDim[{1}].xy) * 128.0);\n", t, map));
    }

    for (uint32_t is = 0; is < k.numIndStages; is++)
    {
        uint32_t map = (k.indOrder >> (6 * is)) & 7;
        uint32_t coord = (k.indOrder >> (6 * is + 3)) & 7;
        if (coord < k.numTexGens)
        {
            W(s, std::format("\tivec2 indcoord{0} = fixpoint_uv{1} >> ps.indScale[{0}].xy;\n", is, coord));
            W(s, std::format("\tivec3 iindtex{0} = sampleTex({1}u, indcoord{0}, ps.texDim[{1}].xy).abg;\n", is, map));
        }
        else
            W(s, std::format("\tivec3 iindtex{0} = ivec3(0);\n", is));
    }

    for (uint32_t n = 0; n < k.numTevStages; n++)
    {
        uint32_t ord = (k.tevOrder[n / 2] >> ((n & 1) * 12)) & 0xFFF;
        uint32_t texmap = ord & 7, texcoord = (ord >> 3) & 7, texEnable = (ord >> 6) & 1, chan = (ord >> 7) & 7;
        bool hasCoord = texcoord < k.numTexGens;
        if (!hasCoord) texcoord = 0;
        uint32_t ind = k.tevInd[n];
        uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3, mid = (ind >> 9) & 15;
        // GX's tangent-space wrap modes. Not named sw/tw: <charconv> (pulled in by <format>)
        // exports std::sw and std::tw, which would shadow these inside the lambda below.
        const uint32_t wrapS = (ind >> 13) & 7, wrapT = (ind >> 16) & 7;
        W(s, std::format("\t// stage {}\n", n));
        bool hasInd = bt < k.numIndStages;
        if (hasInd && bs != 0)
        {
            static const char* sel[4] = { "", "x", "y", "z" };
            static const char shift[4] = { '0', '5', '4', '3' };
            W(s, std::format("\talphabump = (iindtex{}.{} << {}) & 248;\n", bt, sel[bs], shift[fmt]));
        }
        if (hasInd && (mid & 3) != 0 && k.numTexGens > 0)
        {
            static const char fshift[4] = { '0', '3', '4', '5' };
            W(s, std::format("\tivec3 iindtevcrd{0} = iindtex{1} >> {2};\n", n, bt, fshift[fmt]));
            if (bias)
            {
                const int badd = fmt == 0 ? -128 : 1;
                const int bx = (bias & 1) ? badd : 0, by = (bias & 2) ? badd : 0, bz = (bias & 4) ? badd : 0;
                W(s, std::format("\tiindtevcrd{} += ivec3({}, {}, {});\n", n, bx, by, bz));
            }
            uint32_t mtxIdx = 2 * ((mid & 3) - 1);
            uint32_t mtxId = mid >> 2;   // 0 = indirect matrix, 1 = S, 2 = T
            if (mtxId == 0)
                W(s, std::format("\tivec2 indtevtrans{0} = ivec2(idot3(ps.indMtx[{1}].xyz, iindtevcrd{0}), "
                                 "idot3(ps.indMtx[{2}].xyz, iindtevcrd{0})) >> 3;\n", n, mtxIdx, mtxIdx + 1));
            else if (mtxId == 1)
                W(s, std::format("\tivec2 indtevtrans{0} = ivec2(fixpoint_uv{1} * iindtevcrd{0}.xx) >> 8;\n", n, texcoord));
            else
                W(s, std::format("\tivec2 indtevtrans{0} = ivec2(fixpoint_uv{1} * iindtevcrd{0}.yy) >> 8;\n", n, texcoord));
            W(s, std::format("\tif (ps.indMtx[{1}].w >= 0) indtevtrans{0} >>= ps.indMtx[{1}].w; else indtevtrans{0} <<= (-ps.indMtx[{1}].w);\n", n, mtxIdx));
        }
        else
            W(s, std::format("\tivec2 indtevtrans{} = ivec2(0, 0);\n", n));

        if (k.numTexGens > 0)
        {
            WrapCoord(s, wrapS, 'x', texcoord);
            WrapCoord(s, wrapT, 'y', texcoord);
            W(s, std::format("\ttevcoord.xy = wrappedcoord + indtevtrans{};\n", n));
            W(s, "\ttevcoord.xy = (tevcoord.xy << 8) >> 8;\n");
        }

        uint32_t cc = k.colorComb[n], ac = k.alphaComb[n];
        uint32_t ca = (cc >> 12) & 15, cb = (cc >> 8) & 15, ccc = (cc >> 4) & 15, cd = cc & 15;
        uint32_t aa = (ac >> 13) & 7, ab = (ac >> 10) & 7, acc = (ac >> 7) & 7, ad = (ac >> 4) & 7;
        uint32_t rswap = ac & 3, tswap = (ac >> 2) & 3;
        auto swapStr = [&](uint32_t table) {
            uint32_t rg = k.ksel[table * 2], ba = k.ksel[table * 2 + 1];
            const std::string rgba = "rgba";
            char str[5] = { rgba[rg & 3], rgba[(rg >> 2) & 3], rgba[ba & 3], rgba[(ba >> 2) & 3], 0 };
            return std::string(str);
        };
        bool usesRas = ca == 10 || ca == 11 || cb == 10 || cb == 11 || ccc == 10 || ccc == 11 || cd == 10 || cd == 11 ||
                       aa == 5 || ab == 5 || acc == 5 || ad == 5;
        if (usesRas)
            W(s, std::format("\trastemp = {}.{};\n", kRas[chan], swapStr(rswap)));
        if (texEnable && k.numTexGens > 0)
            W(s, std::format("\ttextemp = sampleTex({0}u, tevcoord.xy, ps.texDim[{0}].xy).{1};\n", texmap, swapStr(tswap)));
        else if (k.numTexGens == 0)
            W(s, "\ttextemp = ivec4(0, 0, 0, 0);\n");
        else
            W(s, "\ttextemp = ivec4(255, 255, 255, 255);\n");
        bool usesK = ca == 14 || cb == 14 || ccc == 14 || cd == 14 || aa == 6 || ab == 6 || acc == 6 || ad == 6;
        if (usesK)
        {
            uint32_t ks = k.ksel[n / 2];
            uint32_t kc = (n & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31;
            uint32_t ka = (n & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
            W(s, std::format("\tkonsttemp = ivec4({}, {});\n", kKC[kc], kKA[ka]));
        }
        W(s, std::format("\ttevin_a = ivec4({}, {}) & ivec4(255);\n", kColorIn[ca], kAlphaIn[aa]));
        W(s, std::format("\ttevin_b = ivec4({}, {}) & ivec4(255);\n", kColorIn[cb], kAlphaIn[ab]));
        W(s, std::format("\ttevin_c = ivec4({}, {}) & ivec4(255);\n", kColorIn[ccc], kAlphaIn[acc]));
        W(s, std::format("\ttevin_d = ivec4({}, {});\n", kColorIn[cd], kAlphaIn[ad]));

        uint32_t cbias = (cc >> 16) & 3, cop = (cc >> 18) & 1, cclamp = (cc >> 19) & 1, cscale = (cc >> 20) & 3, cdest = (cc >> 22) & 3;
        uint32_t abias = (ac >> 16) & 3, aop = (ac >> 18) & 1, aclamp = (ac >> 19) & 1, ascale = (ac >> 20) & 3, adest = (ac >> 22) & 3;
        W(s, std::format("\t{}.rgb = clamp(", kOut[cdest]));
        if (cbias != 3)
            W(s, TevRegular("rgb", cbias, cop, cscale));
        else
        {
            static const char* gt[4] = {
                "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : ivec3(0,0,0))",
                "((idot3(tevin_a.rgb, ivec3(1,256,0)) > idot3(tevin_b.rgb, ivec3(1,256,0))) ? tevin_c.rgb : ivec3(0,0,0))",
                "((idot3(tevin_a.rgb, ivec3(1,256,65536)) > idot3(tevin_b.rgb, ivec3(1,256,65536))) ? tevin_c.rgb : ivec3(0,0,0))",
                "(max(sign(tevin_a.rgb - tevin_b.rgb), ivec3(0)) * tevin_c.rgb)" };
            static const char* eq[4] = {
                "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : ivec3(0,0,0))",
                "((idot3(tevin_a.rgb, ivec3(1,256,0)) == idot3(tevin_b.rgb, ivec3(1,256,0))) ? tevin_c.rgb : ivec3(0,0,0))",
                "((idot3(tevin_a.rgb, ivec3(1,256,65536)) == idot3(tevin_b.rgb, ivec3(1,256,65536))) ? tevin_c.rgb : ivec3(0,0,0))",
                "((ivec3(1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)" };
            W(s, std::format("tevin_d.rgb + {}", cop ? eq[cscale] : gt[cscale]));
        }
        W(s, cclamp ? ", ivec3(0), ivec3(255));\n" : ", ivec3(-1024), ivec3(1023));\n");
        W(s, std::format("\t{}.a = clamp(", kOut[adest]));
        if (abias != 3)
            W(s, TevRegular("a", abias, aop, ascale));
        else
        {
            static const char* gt[4] = {
                "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",
                "((idot3(tevin_a.rgb, ivec3(1,256,0)) > idot3(tevin_b.rgb, ivec3(1,256,0))) ? tevin_c.a : 0)",
                "((idot3(tevin_a.rgb, ivec3(1,256,65536)) > idot3(tevin_b.rgb, ivec3(1,256,65536))) ? tevin_c.a : 0)",
                "((tevin_a.a > tevin_b.a) ? tevin_c.a : 0)" };
            static const char* eq[4] = {
                "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",
                "((idot3(tevin_a.rgb, ivec3(1,256,0)) == idot3(tevin_b.rgb, ivec3(1,256,0))) ? tevin_c.a : 0)",
                "((idot3(tevin_a.rgb, ivec3(1,256,65536)) == idot3(tevin_b.rgb, ivec3(1,256,65536))) ? tevin_c.a : 0)",
                "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)" };
            W(s, std::format("tevin_d.a + {}", aop ? eq[ascale] : gt[ascale]));
        }
        W(s, aclamp ? ", 0, 255);\n" : ", -1024, 1023);\n");
    }
    W(s, "\tprev = prev & ivec4(255);\n");

    // alpha range test
    {
        uint32_t comp0 = k.alphaFunc & 7, comp1 = (k.alphaFunc >> 3) & 7, logic = (k.alphaFunc >> 6) & 3;
        static const char* lg[4] = { " && ", " || ", " != ", " == " };
        static const char* op[8] = { "", " < ", " == ", " <= ", " > ", " != ", " >= ", "" };
        bool always = (logic == 0 && comp0 == 7 && comp1 == 7) || (logic == 1 && (comp0 == 7 || comp1 == 7));
        if (!always)
        {
            auto term = [](uint32_t comp, const char* ref) -> std::string {
                if (comp == 0) return "false";
                if (comp == 7) return "true";
                return std::format("(prev.a{}{})", op[comp], ref);
            };
            W(s, std::format("\tif (!({}{}{})) discard;\n", term(comp0, "ps.alphaRef.x"), lg[logic], term(comp1, "ps.alphaRef.y")));
        }
    }
    W(s, "\tif (prev.a == 1) prev.a = 0;\n");

    // z texture + 24-bit depth
    W(s, "\tint zCoord = int(ps.depthParams.x) + int((vClip.z / vClip.w) * ps.depthParams.y);\n");
    W(s, "\tzCoord = clamp(zCoord, 0, 16777215);\n");
    uint32_t zop = (k.zTexType >> 2) & 3;
    if (zop != 0 && k.numTevStages > 0)
    {
        W(s, std::format("\tzCoord = int(dot(vec3(ps.zBias[0].rgb), vec3(textemp.rgb))) + ps.zBias[1].w{};\n",
                         zop == 1 ? " + zCoord" : ""));
        W(s, "\tzCoord = zCoord & 0xFFFFFF;\n");
    }
    if (k.perPixelDepth) W(s, "\tgl_FragDepth = float(zCoord) / 16777216.0;\n");

    // fog
    uint32_t fsel = k.fogType & 7, fproj = (k.fogType >> 3) & 1, frange = (k.fogType >> 4) & 1;
    if (fsel >= 2)
    {
        if (fproj == 0)
            W(s, "\tfloat ze = (ps.fogF.x * 16777216.0) / float(ps.fogI.y - (zCoord >> ps.fogI.w));\n");
        else
            W(s, "\tfloat ze = ps.fogF.x * float(zCoord) / 16777216.0;\n");
        if (frange)
        {
            W(s, "\tfloat offset = (2.0 * (gl_FragCoord.x / ps.fogF.w)) - 1.0 - ps.fogF.z;\n"
                 "\tfloat floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
                 "\tuint indexlower = uint(floatindex); uint indexupper = indexlower + 1u;\n"
                 "\tfloat klower = ps.fogRange[indexlower >> 2u][indexlower & 3u];\n"
                 "\tfloat kupper = ps.fogRange[indexupper >> 2u][indexupper & 3u];\n"
                 "\tfloat kk = mix(klower, kupper, fract(floatindex));\n"
                 "\tze *= sqrt(offset * offset + kk * kk) / kk;\n");
        }
        W(s, "\tfloat fog = clamp(ze - ps.fogF.y, 0.0, 1.0);\n");
        switch (fsel)
        {
        case 4: W(s, "\tfog = 1.0 - exp2(-8.0 * fog);\n"); break;
        case 5: W(s, "\tfog = 1.0 - exp2(-8.0 * fog * fog);\n"); break;
        case 6: W(s, "\tfog = exp2(-8.0 * (1.0 - fog));\n"); break;
        case 7: W(s, "\tfog = 1.0 - fog; fog = exp2(-8.0 * fog * fog);\n"); break;
        }
        W(s, "\tint ifog = int(round(fog * 256.0));\n");
        W(s, "\tprev.rgb = (prev.rgb * (256 - ifog) + ps.fogColor.rgb * ifog) >> 8;\n");
    }

    bool rgba6 = k.pixelFormat == 1;
    if (rgba6) W(s, "\toCol0.rgb = vec3(prev.rgb >> 2) / 63.0;\n");
    else W(s, "\toCol0.rgb = vec3(prev.rgb) / 255.0;\n");
    if (k.dstAlphaEnable)
        W(s, rgba6 ? "\toCol0.a = float(ps.alphaRef.z >> 2) / 63.0;\n" : "\toCol0.a = float(ps.alphaRef.z) / 255.0;\n");
    else
        W(s, rgba6 ? "\toCol0.a = float(prev.a >> 2) / 63.0;\n" : "\toCol0.a = float(prev.a) / 255.0;\n");
    W(s, "\toCol1 = vec4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
    W(s, "}\n");
    return s;
}

// ---------------------------------------------------------------- util shader
std::string UtilShader()
{
    return R"(#version 450
layout(std140, set = 0, binding = 1) uniform U
{
    vec4 rect;
    vec4 uv;
    vec4 color;
    vec4 params;
    ivec4 iparams;
} u;
layout(set = 0, binding = 2) uniform sampler2D src;
layout(set = 0, binding = 3) uniform sampler2D srcDepth;

layout(location = 0) out vec2 vUv;

void main()
{
    // Full-viewport triangle strip generated from gl_VertexIndex (0..3).
    vec2 t = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(mix(u.rect.x, u.rect.z, t.x), mix(u.rect.y, u.rect.w, t.y), u.params.x, 1.0);
    vUv = mix(u.uv.xy, u.uv.zw, t);
}
)";
}

std::string UtilFragment()
{
    return R"(#version 450
layout(std140, set = 0, binding = 1) uniform U
{
    vec4 rect;
    vec4 uv;
    vec4 color;
    vec4 params;
    ivec4 iparams;
} u;
layout(set = 0, binding = 2) uniform sampler2D src;
layout(set = 0, binding = 3) uniform sampler2D srcDepth;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oCol;
)";
}

std::string ClearFragment()
{
    return R"(
void main()
{
    oCol = u.color;
}
)";
}

std::string CopyFragment()
{
    return R"(
void main()
{
    // u.iparams.x: format, .y: intensity, .z: depth source, .w: half-scale box filter
    vec4 c;
    if (u.iparams.z != 0)
    {
        float d = textureLod(srcDepth, vUv, 0.0).r;
        uint z = uint(saturate(d) * 16777215.0);
        float h = float((z >> 16u) & 0xFFu) / 255.0;
        float m = float((z >> 8u) & 0xFFu) / 255.0;
        float l = float(z & 0xFFu) / 255.0;
        c = vec4(h, m, l, h);
        if (u.iparams.x == 0 || u.iparams.x == 1 || u.iparams.x == 8) c = vec4(h, h, h, h);
        else if (u.iparams.x == 9) c = vec4(m, m, m, m);
        else if (u.iparams.x == 10) c = vec4(l, l, l, l);
        else if (u.iparams.x == 3) c = vec4(m, m, m, h);
        else if (u.iparams.x == 11 || u.iparams.x == 12) c = vec4(m, l, l, m);
        else c = vec4(h, m, l, 1.0);
        oCol = c;
        return;
    }
    c = textureLod(src, vUv, 0.0);
    if (u.iparams.w != 0)
    {
        vec2 d = u.params.zw * 0.25;
        c = (textureLod(src, vUv + vec2(-d.x, -d.y), 0.0) + textureLod(src, vUv + vec2(d.x, -d.y), 0.0) +
             textureLod(src, vUv + vec2(-d.x, d.y), 0.0) + textureLod(src, vUv + vec2(d.x, d.y), 0.0)) * 0.25;
    }
    if (u.iparams.y != 0)
    {
        // RGB -> Y (BT.601, studio range as on hardware)
        float y = dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
        c.rgb = vec3(y);
    }
    if (u.iparams.x == 0 || u.iparams.x == 1) c = vec4(c.r, c.r, c.r, c.r);
    else if (u.iparams.x == 2 || u.iparams.x == 3) c = vec4(c.r, c.r, c.r, c.a);
    else if (u.iparams.x == 4) c.a = 1.0;
    else if (u.iparams.x == 7 || u.iparams.x == 8) c = vec4(c.r, c.r, c.r, c.r);
    else if (u.iparams.x == 9) c = vec4(c.g, c.g, c.g, c.g);
    else if (u.iparams.x == 10) c = vec4(c.b, c.b, c.b, c.b);
    else if (u.iparams.x == 11) c = vec4(c.r, c.r, c.r, c.g);
    else if (u.iparams.x == 12) c = vec4(c.g, c.g, c.g, c.b);
    oCol = c;
}
)";
}

}   // namespace

std::string GenerateVertexShaderGLSL(const VSKey& key) { return VertexShader(key); }
std::string GenerateFragmentShaderGLSL(const PSKey& key) { return PixelShader(key); }

std::string GenerateUtilShaderGLSL() { return UtilShader(); }
std::string GenerateUtilFragmentGLSL(bool copy) { return (copy ? UtilFragment() + CopyFragment() : UtilFragment() + ClearFragment()); }
