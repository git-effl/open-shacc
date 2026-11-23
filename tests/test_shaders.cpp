/**
 * @file test_shaders.cpp
 * @brief Clean-room automated test suite for open-shacc compiler library.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "open_shacc.h"
#include "../src/backend/usse_isa.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <string>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

static uint32_t read_u32(const uint8_t* base, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static int32_t read_i32(const uint8_t* base, size_t off) {
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint16_t read_u16(const uint8_t* base, size_t off) {
    uint16_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint64_t read_u64(const uint8_t* base, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

void test_version() {
    const char* ver = SceShaccCgGetVersion();
    TEST_ASSERT(ver != nullptr && std::string(ver).find("open-shacc") != std::string::npos, "Version string contains open-shacc");

    const char* scver = sceShaccCgGetVersionString();
    TEST_ASSERT(scver != nullptr && std::string(scver).find("open-shacc") != std::string::npos, "sceShaccCg version string contains open-shacc");

    g_tests_passed++;
    std::cout << "  [PASS] test_version\n";
}

void test_clean_room_bitfield_encoders() {
    using namespace open_shacc::backend;

    // PHAS end=true: op1=0b11111, op2=0b010, disc=0b100, end=1
    MicrocodeWord phas = MicrocodeWord::encode_phas(true);
    TEST_ASSERT((phas.raw_word >> 59) == 0b11111ULL, "PHAS op1 must be 11111");
    TEST_ASSERT(((phas.raw_word >> 56) & 0x7ULL) == 0b010ULL, "PHAS op2 must be 010");
    TEST_ASSERT(((phas.raw_word >> 51) & 1ULL) == 1ULL, "PHAS end bit must be 1");

    // LIMM r4, 1.0f (0x3F800000)
    HardwareRegister r4{};
    r4.reg_class = RegisterClass::Temporary;
    r4.index = 4;
    MicrocodeWord limm = MicrocodeWord::encode_limm(r4, 0x3F800000u);
    TEST_ASSERT((limm.raw_word >> 59) == 0b11111ULL, "LIMM op1 must be 11111");
    TEST_ASSERT(((limm.raw_word >> 56) & 0x7ULL) == 0b100ULL, "LIMM op2 must be 100");
    TEST_ASSERT(((limm.raw_word >> 21) & 0x7FULL) == 4ULL, "LIMM dst index must be un-doubled r4");
    uint32_t decoded_imm = static_cast<uint32_t>(
        (limm.raw_word & 0x1FFFFFULL) |
        (((limm.raw_word >> 36) & 0x1FULL) << 21) |
        (((limm.raw_word >> 44) & 0x3FULL) << 26)
    );
    TEST_ASSERT(decoded_imm == 0x3F800000u, "LIMM immediate bitfield roundtrip must preserve 1.0f");

    // DEPTHF feedback=1, PA/Temp source r2
    HardwareRegister r2{};
    r2.reg_class = RegisterClass::Temporary;
    r2.index = 2;
    MicrocodeWord depthf = MicrocodeWord::encode_depthf(r2, ShortPredicate::None, true);
    TEST_ASSERT(((depthf.raw_word >> 56) & 0x7ULL) == 0b011ULL, "DEPTHF op2 must be 011");
    TEST_ASSERT(((depthf.raw_word >> 35) & 0x3ULL) == 1ULL, "DEPTHF feedback field must sit at bits 36..35");
    TEST_ASSERT(((depthf.raw_word >> 14) & 0x7FULL) == 1ULL, "DEPTHF src0 doubled index for r2 must be 1");

    // SMP r0, pa2, sa8
    HardwareRegister r0{};
    r0.reg_class = RegisterClass::Temporary;
    r0.index = 0;
    HardwareRegister pa2{};
    pa2.reg_class = RegisterClass::InputAttr;
    pa2.index = 2;
    MicrocodeWord smp = MicrocodeWord::encode_smp(r0, pa2, 8, 1, 0);
    TEST_ASSERT((smp.raw_word >> 59) == 0b11100ULL, "SMP op1 must be 11100");
    TEST_ASSERT(((smp.raw_word >> 34) & 1ULL) == 1ULL, "SMP src0_bank must select InputAttr (1)");
    TEST_ASSERT(((smp.raw_word >> 30) & 3ULL) == 3ULL, "SMP src1_bank must select SECATTR (3)");
    TEST_ASSERT(((smp.raw_word >> 14) & 0x7FULL) == 1ULL, "SMP src0 doubled index for pa2 must be 1");
    TEST_ASSERT(((smp.raw_word >> 7) & 0x7FULL) == 4ULL, "SMP src1 doubled SA offset for sa8 must be 4");

    // BR offset = -3, pred = P0
    MicrocodeWord br = MicrocodeWord::encode_br(-3, ExtPredicate::P0);
    TEST_ASSERT((br.raw_word >> 59) == 0b11111ULL, "BR op1 must be 11111");
    TEST_ASSERT(((br.raw_word >> 56) & 0x7ULL) == static_cast<uint64_t>(ExtPredicate::P0), "BR predicate must encode P0");
    TEST_ASSERT((br.raw_word & 0xFFFFFULL) == (static_cast<uint32_t>(-3) & 0xFFFFFU), "BR offset must encode -3 in 20-bit two's complement");

    g_tests_passed++;
    std::cout << "  [PASS] test_clean_room_bitfield_encoders\n";
}

void test_basic_mvp_shader() {
    const char* shader_code = R"(
        uniform float4x4 u_MVPMatrix;
        uniform float4 u_Tint;

        struct VS_INPUT {
            float4 position : POSITION;
            float2 uv       : TEXCOORD0;
        };

        struct VS_OUTPUT {
            float4 position : POSITION;
            float2 uv       : TEXCOORD0;
            float4 color    : COLOR0;
        };

        VS_OUTPUT main(VS_INPUT input) {
            VS_OUTPUT output;
            output.position = mul(u_MVPMatrix, input.position);
            output.uv = input.uv;
            output.color = u_Tint;
            return output;
        }
    )";

    void* bin = nullptr;
    size_t size = 0;
    char* log = nullptr;

    int res = SceShaccCgCompileProgram(shader_code, OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY, nullptr, &bin, &size, &log);
    if (log) {
        SceShaccCgDestroyDiagnosticLog(log);
    }

    TEST_ASSERT(res == OPEN_SHACC_SUCCESS, "Compilation should succeed for MVP vertex shader");
    TEST_ASSERT(bin != nullptr, "Binary buffer must not be null");
    TEST_ASSERT(size >= 0x9C, "Binary size must fit SceGxmProgram header");
    TEST_ASSERT(size % 16 == 0, "Container size must be 16-byte aligned");

    const auto* bytes = static_cast<const uint8_t*>(bin);
    TEST_ASSERT(read_u32(bytes, 0x00) == 0x00505847u, "Magic must be 'GXP\\0'");
    TEST_ASSERT(read_u32(bytes, 0x08) == size, "SceGxmProgram size field must match buffer size");
    TEST_ASSERT(read_u32(bytes, 0x14) == 0u, "Vertex shader program_flags must not have fragment bit");
    TEST_ASSERT(read_u32(bytes, 0x24) == 4u, "Should emit 2 uniforms + 2 vertex attributes");
    TEST_ASSERT(read_i32(bytes, 0x2C) == (0x9C - 0x2C), "varyings_offset must point directly after header");
    TEST_ASSERT(read_u16(bytes, 0x30) >= 1, "primary_reg_count >= 1");
    TEST_ASSERT(read_u16(bytes, 0x34) >= 1, "temp_reg_count1 >= 1");
    TEST_ASSERT(read_u32(bytes, 0x3C) >= 5u, "Expected matrix mul + uv + tint + PHAS instructions");
    const int32_t ucode_rel = read_i32(bytes, 0x40);
    TEST_ASSERT(((0x40 + ucode_rel) % 8) == 0, "Primary microcode must be 8-byte aligned");
    TEST_ASSERT(read_u32(bytes, 0x64) == 20u, "float4x4 + float4 = 20 uniform words");
    TEST_ASSERT(read_u32(bytes, 0x90) == 1u, "container_count must be 1 for default UB");

    SceShaccCgDestroyProgram(bin);
    g_tests_passed++;
    std::cout << "  [PASS] test_basic_mvp_shader\n";
}

void test_sceshacccg_abi_and_reflection() {
    const char* shader_code = R"(
        uniform float4x4 u_MVPMatrix;
        uniform sampler2D texSampler;

        float4 main(float2 uv : TEXCOORD0) : COLOR {
            float4 col = tex2D(texSampler, uv);
            return col;
        }
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_FP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr, "sceShaccCgCompileProgram must return non-null output");
    TEST_ASSERT(output->programData != nullptr, "programData must not be null");
    TEST_ASSERT(output->programSize >= 0x9C, "programSize must fit SceGxmProgram");
    TEST_ASSERT((read_u32(output->programData, 0x14) & 1u) == 1u, "Fragment shader flag bit 0 must be set");
    TEST_ASSERT((read_u32(output->programData, 0x14) & (1u << 6)) != 0u, "Fragment NATIVECOLOR_USED flag must be set");
    TEST_ASSERT(read_u32(output->programData, 0x80) == 1u, "Should emit 1 dependent sampler entry");
    TEST_ASSERT(read_u32(output->programData, 0x90) == 2u, "Should emit container 14 + container 19");

    SceShaccCgParameter p_mvp = sceShaccCgGetParameterByName(output, "u_MVPMatrix");
    TEST_ASSERT(p_mvp != nullptr, "Should find u_MVPMatrix uniform parameter");
    TEST_ASSERT(sceShaccCgGetParameterClass(p_mvp) == SCE_SHACCCG_PARAMETERCLASS_MATRIX, "u_MVPMatrix must be matrix class");
    TEST_ASSERT(sceShaccCgGetParameterRows(p_mvp) == 4, "u_MVPMatrix rows must be 4");
    TEST_ASSERT(sceShaccCgGetParameterBufferIndex(p_mvp) == 14, "Uniform buffer index must be 14");

    SceShaccCgParameter p_row2 = sceShaccCgGetRowParameter(p_mvp, 2);
    TEST_ASSERT(p_row2 != nullptr, "GetRowParameter(2) must return valid row handle");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(p_row2) == 8u, "Row 2 resource index must be 8");

    SceShaccCgParameter p_samp = sceShaccCgGetParameterByName(output, "texSampler");
    TEST_ASSERT(p_samp != nullptr, "Should find texSampler parameter");
    TEST_ASSERT(sceShaccCgGetParameterClass(p_samp) == SCE_SHACCCG_PARAMETERCLASS_SAMPLER, "texSampler must be sampler class");

    SceShaccCgParameter first_p = sceShaccCgGetFirstParameter(output);
    TEST_ASSERT(first_p != nullptr, "GetFirstParameter must return a valid parameter");

    sceShaccCgDestroyCompileOutput(output);

    g_tests_passed++;
    std::cout << "  [PASS] test_sceshacccg_abi_and_reflection\n";
}

void test_vitagl_style_shader_and_preprocessor() {
    const char* shader_code = R"(
        #define num_textures 2
        #define lighting 1
        #define shading_mode 0 // flat

        #if lighting == 1 && shading_mode < 1
        void main(
            float3 position : POSITION,
            float2 uv : TEXCOORD0,
            uniform float4x4 Jwvp,
            uniform sampler2D tex[num_textures],
            float4 out vPosition : POSITION,
            float2 out vUv : TEXCOORD0
        ) {
            const float scale = 1.0f;
            float4 pos = float4(position, scale);
            vPosition = mul(Jwvp, pos);
            vUv = uv;
        }
        #else
        void main(float4 position : POSITION, float4 out vPosition : POSITION) {
            vPosition = position;
        }
        #endif
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_VP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr && output->programData != nullptr, "vitaGL-style shader must compile");

    SceShaccCgParameter p_wvp = sceShaccCgGetParameterByName(output, "Jwvp");
    TEST_ASSERT(p_wvp != nullptr, "uniform float4x4 Jwvp inside main() must be reflected as UNIFORM");
    TEST_ASSERT(sceShaccCgGetParameterVariability(p_wvp) == SCE_SHACCCG_VARIABILITY_UNIFORM, "Jwvp must be UNIFORM");
    TEST_ASSERT(sceShaccCgGetParameterBufferIndex(p_wvp) == 14, "Jwvp buffer index must be 14");

    SceShaccCgParameter p_tex = sceShaccCgGetParameterByName(output, "tex");
    TEST_ASSERT(p_tex != nullptr, "uniform sampler2D tex[2] inside main() must be reflected");
    TEST_ASSERT(sceShaccCgGetParameterClass(p_tex) == SCE_SHACCCG_PARAMETERCLASS_SAMPLER, "tex must be SAMPLER");
    TEST_ASSERT(sceShaccCgGetArraySize(p_tex) == 2, "tex array size must equal 2");
    TEST_ASSERT(sceShaccCgGetArrayParameter(p_tex, 1) != nullptr, "GetArrayParameter(1) must return valid sampler element");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_tex, 1)) == 1u, "tex[1] sampler unit must be 1");

    SceShaccCgParameter p_vpos = sceShaccCgGetParameterByName(output, "vPosition");
    TEST_ASSERT(p_vpos != nullptr, "out parameter vPosition must be reflected");
    TEST_ASSERT(sceShaccCgGetParameterDirection(p_vpos) == SCE_SHACCCG_DIRECTION_OUT, "vPosition must be DIRECTION_OUT");

    sceShaccCgDestroyCompileOutput(output);

    g_tests_passed++;
    std::cout << "  [PASS] test_vitagl_style_shader_and_preprocessor\n";
}

void test_blinn_phong_normalmap_fragment() {
    const char* shader_code = R"(
        uniform sampler2D u_AlbedoMap;
        uniform sampler2D u_NormalMap;
        uniform float3 u_LightDir;
        uniform float3 u_ViewDir;
        uniform float3 u_LightColor;
        uniform float3 u_AmbientColor;
        uniform float  u_Shininess;

        struct PS_IN {
            float2 uv      : TEXCOORD0;
            float3 normal  : TEXCOORD1;
            float3 tangent : TEXCOORD2;
            float4 color   : COLOR0;
        };

        float4 main(PS_IN vin) : COLOR {
            float4 albedo = tex2D(u_AlbedoMap, vin.uv) * vin.color;
            float3 nmap = tex2D(u_NormalMap, vin.uv).xyz * 2.0f - 1.0f;
            float3 N = normalize(vin.normal);
            float3 T = normalize(vin.tangent);
            float3 B = cross(N, T);
            float3 worldN = normalize(T * nmap.x + B * nmap.y + N * nmap.z);

            float3 L = normalize(u_LightDir);
            float3 V = normalize(u_ViewDir);
            float3 H = normalize(L + V);

            float ndotl = saturate(dot(worldN, L));
            float ndoth = saturate(dot(worldN, H));
            float spec = pow(ndoth, u_Shininess);

            float3 lit = albedo.rgb * (u_AmbientColor + u_LightColor * ndotl) + u_LightColor * spec;
            return float4(lit, albedo.a);
        }
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_FP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr && output->programData != nullptr, "Blinn-Phong normal map fragment shader must compile");

    const uint8_t* bytes = output->programData;
    TEST_ASSERT(read_u32(bytes, 0x80) == 2u, "Should emit 2 dependent sampler records");
    const uint32_t varyings_count = read_u32(bytes, 0x9C + 0x0C);
    TEST_ASSERT(varyings_count == 4u, "Fragment shader should expose 4 varying descriptors");

    const uint32_t instr_count = read_u32(bytes, 0x3C);
    TEST_ASSERT(instr_count > 20u, "Blinn-Phong shader should emit full math & SMP instruction stream");
    const size_t ucode_abs = 0x40 + static_cast<size_t>(read_i32(bytes, 0x40));
    const uint64_t last_word = read_u64(bytes, ucode_abs + (instr_count - 1) * sizeof(uint64_t));
    TEST_ASSERT((last_word >> 59) == 0b11111ULL && ((last_word >> 51) & 1ULL) == 1ULL, "Shader must end with PHAS end=1");

    sceShaccCgDestroyCompileOutput(output);
    g_tests_passed++;
    std::cout << "  [PASS] test_blinn_phong_normalmap_fragment\n";
}

void test_shadow_pcf_control_flow_and_helpers() {
    const char* shader_code = R"(
        uniform sampler2D u_ShadowMap;
        uniform float4 u_LightProj;
        uniform float u_Time;

        struct LitSample {
            float factor;
            float rim;
        };

        void apply_rim(float3 n, float3 v, inout float out_rim) {
            out_rim = pow(saturate(1.0f - abs(dot(n, v))), 2.0f);
        }

        LitSample compute_pcf(float4 shadow_coord, float3 n, float3 v) {
            static const float offsets[3] = { -0.001f, 0.0f, 0.001f };
            float shadow = 0.0f;
            for (int i = 0; i < 3; ++i) {
                float4 sc = shadow_coord;
                sc.x = sc.x + offsets[i] + 0.0002f * sin(u_Time);
                float depth_tap = tex2Dproj(u_ShadowMap, sc).x;
                if (depth_tap < shadow_coord.z) {
                    shadow += 1.0f;
                } else {
                    shadow += 0.25f;
                }
            }
            LitSample res;
            res.factor = shadow / 3.0f;
            float r = 0.0f;
            apply_rim(n, v, r);
            res.rim = r;
            return res;
        }

        float4 main(float4 shadow_uv : TEXCOORD0, float3 n : TEXCOORD1, float3 v : TEXCOORD2) : COLOR {
            LitSample s = compute_pcf(shadow_uv, normalize(n), normalize(v));
            float tint = (s.factor > 0.5f) ? cos(u_Time * 0.5f) : 0.2f;
            return float4(s.factor, s.rim, tint, 1.0f);
        }
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_FP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr && output->programData != nullptr, "PCF shadow + loop + helper shader must compile");

    const uint8_t* bytes = output->programData;
    const uint32_t instr_count = read_u32(bytes, 0x3C);
    const size_t ucode_abs = 0x40 + static_cast<size_t>(read_i32(bytes, 0x40));

    bool saw_vtst = false;
    bool saw_br = false;
    bool saw_smp = false;
    for (uint32_t i = 0; i < instr_count; ++i) {
        uint64_t w = read_u64(bytes, ucode_abs + i * sizeof(uint64_t));
        uint64_t op1 = (w >> 59);
        if (op1 == 0b01001ULL) saw_vtst = true;
        if (op1 == 0b11111ULL && ((w >> 52) & 3ULL) == 0ULL && ((w >> 38) & 1ULL) == 1ULL) saw_br = true;
        if (op1 == 0b11100ULL) saw_smp = true;
    }
    TEST_ASSERT(saw_vtst, "PCF loop & branches must emit VTST instructions");
    TEST_ASSERT(saw_br, "PCF loop & branches must emit BR instructions");
    TEST_ASSERT(saw_smp, "tex2Dproj must emit SMP instruction");

    sceShaccCgDestroyCompileOutput(output);
    g_tests_passed++;
    std::cout << "  [PASS] test_shadow_pcf_control_flow_and_helpers\n";
}

void test_alpha_clip_discard_and_depth() {
    const char* shader_code = R"(
        uniform sampler2D u_SpriteTex;
        uniform float u_AlphaCut;

        struct PS_OUT {
            float4 color : COLOR0;
            float  depth : DEPTH;
        };

        PS_OUT main(float2 uv : TEXCOORD0, float depth_in : TEXCOORD1) {
            float4 c = tex2D(u_SpriteTex, uv);
            clip(c.a - u_AlphaCut);
            if (c.r < 0.01f) {
                discard;
            }
            PS_OUT o;
            o.color = c;
            o.depth = saturate(depth_in);
            return o;
        }
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_FP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr && output->programData != nullptr, "clip/discard/DEPTH shader must compile");

    const uint8_t* bytes = output->programData;
    const uint32_t flags = read_u32(bytes, 0x14);
    TEST_ASSERT((flags & (1u << 3)) != 0u, "GXP_PROGRAM_FLAG_DISCARD_USED must be set");
    TEST_ASSERT((flags & (1u << 4)) != 0u, "GXP_PROGRAM_FLAG_DEPTH_USED must be set");

    const uint32_t instr_count = read_u32(bytes, 0x3C);
    const size_t ucode_abs = 0x40 + static_cast<size_t>(read_i32(bytes, 0x40));
    bool saw_kill = false;
    bool saw_depthf = false;
    for (uint32_t i = 0; i < instr_count; ++i) {
        uint64_t w = read_u64(bytes, ucode_abs + i * sizeof(uint64_t));
        if ((w >> 56) == 0b11111001ULL && ((w >> 52) & 3ULL) == 3ULL) saw_kill = true;
        if ((w >> 56) == 0b11111011ULL && ((w >> 52) & 3ULL) == 3ULL) saw_depthf = true;
    }
    TEST_ASSERT(saw_kill, "clip/discard must emit KILL instruction");
    TEST_ASSERT(saw_depthf, "DEPTH semantic write must emit DEPTHF instruction");

    sceShaccCgDestroyCompileOutput(output);
    g_tests_passed++;
    std::cout << "  [PASS] test_alpha_clip_discard_and_depth\n";
}

void test_canonical_vertex_output_order() {
    const char* shader_code = R"(
        uniform float4x4 u_MVP;
        struct VS_OUT {
            float2 uv1   : TEXCOORD1;
            float4 color : COLOR0;
            float  fog   : FOG;
            float4 pos   : POSITION;
            float2 uv0   : TEXCOORD0;
        };
        VS_OUT main(float4 p : POSITION, float2 t0 : TEXCOORD0, float2 t1 : TEXCOORD1) {
            VS_OUT o;
            o.pos = mul(u_MVP, p);
            o.uv0 = t0;
            o.uv1 = t1;
            o.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
            o.fog = 0.5f;
            return o;
        }
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_VP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr && output->programData != nullptr, "Out-of-order vertex outputs must compile");

    const uint8_t* bytes = output->programData;
    const uint32_t v_out1 = read_u32(bytes, 0x9C + 0x10);
    const uint32_t v_out2 = read_u32(bytes, 0x9C + 0x14);
    TEST_ASSERT((v_out1 & 0x800u) != 0u, "COLOR0 bit must be set in vertex_outputs1");
    TEST_ASSERT((v_out1 & 0x200u) != 0u, "FOG bit must be set in vertex_outputs1");
    TEST_ASSERT((v_out2 & 0x7u) != 0u, "TEXCOORD0 bits must be set in vertex_outputs2");
    TEST_ASSERT((v_out2 & (0x7u << 3)) != 0u, "TEXCOORD1 bits must be set in vertex_outputs2");

    SceShaccCgParameter p_pos = sceShaccCgGetParameterByName(output, "pos");
    TEST_ASSERT(p_pos != nullptr && sceShaccCgGetParameterResourceIndex(p_pos) == 0u, "Canonical POSITION must be o0");

    sceShaccCgDestroyCompileOutput(output);
    g_tests_passed++;
    std::cout << "  [PASS] test_canonical_vertex_output_order\n";
}

void test_dot_product_lighting() {
    const char* shader_code = R"(
        uniform float3 u_LightDir;

        float4 main(float3 normal : NORMAL, float4 pos : POSITION) : POSITION {
            float ndotl = dot(normal, u_LightDir);
            float4 out_pos = pos * ndotl;
            return out_pos;
        }
    )";

    void* bin = nullptr;
    size_t size = 0;
    char* log = nullptr;

    int res = SceShaccCgCompileProgram(shader_code, 0, nullptr, &bin, &size, &log);
    if (log) SceShaccCgDestroyDiagnosticLog(log);

    TEST_ASSERT(res == OPEN_SHACC_SUCCESS, "Dot product lighting shader compilation should succeed");
    TEST_ASSERT(size % 16 == 0, "Container must be 16-byte aligned");
    SceShaccCgDestroyProgram(bin);

    g_tests_passed++;
    std::cout << "  [PASS] test_dot_product_lighting\n";
}

void test_syntax_error_handling() {
    const char* bad_code = R"(
        uniform float4x4 u_Matrix
        float4 main(float4 p : POSITION) {
            return p +
        }
    )";

    void* bin = nullptr;
    size_t size = 0;
    char* log = nullptr;

    int res = SceShaccCgCompileProgram(bad_code, 0, nullptr, &bin, &size, &log);

    TEST_ASSERT(res == OPEN_SHACC_ERROR_PARSE_FAILED, "Should return parse failure for broken syntax");
    TEST_ASSERT(log != nullptr, "Diagnostic log should be populated on failure");

    if (log) SceShaccCgDestroyDiagnosticLog(log);
    if (bin) SceShaccCgDestroyProgram(bin);

    g_tests_passed++;
    std::cout << "  [PASS] test_syntax_error_handling\n";
}

int main() {
    std::cout << "Running open-shacc Clean-Room Test Suite...\n";
    test_version();
    test_clean_room_bitfield_encoders();
    test_basic_mvp_shader();
    test_sceshacccg_abi_and_reflection();
    test_vitagl_style_shader_and_preprocessor();
    test_blinn_phong_normalmap_fragment();
    test_shadow_pcf_control_flow_and_helpers();
    test_alpha_clip_discard_and_depth();
    test_canonical_vertex_output_order();
    test_dot_product_lighting();
    test_syntax_error_handling();

    std::cout << "\nTest Results: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return (g_tests_failed == 0) ? 0 : 1;
}
