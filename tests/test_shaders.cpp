/**
 * @file test_shaders.cpp
 * @brief Clean-room automated test suite for open-shacc compiler library.
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

#define TEST_ASSERT_COMPILED(output_ptr, msg) \
    do { \
        if (!(output_ptr) || !(output_ptr)->programData) { \
            std::cerr << "  [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            if ((output_ptr) && (output_ptr)->diagnosticCount > 0 && (output_ptr)->diagnostics) { \
                for (int32_t di = 0; di < (output_ptr)->diagnosticCount; ++di) { \
                    std::cerr << "    diag[" << di << "]: " << ((output_ptr)->diagnostics[di].message ? (output_ptr)->diagnostics[di].message : "<null>") << "\n"; \
                } \
            } \
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

    // V32NMAD ExtVecPredicate::NegP0 must encode 4 (not 5 from ExtPredicate)
    MicrocodeWord vadd_negp0 = MicrocodeWord::encode_v32nmad(
        UsseOpcode::VADD, r0, r2, r4, 0xF, kSwiz12XYZW, Vec4Swiz::XYZW, ExtPredicate::NegP0
    );
    TEST_ASSERT((vadd_negp0.raw_word >> 59) == 0b00001ULL, "VADD op1 must be 00001");
    TEST_ASSERT(((vadd_negp0.raw_word >> 56) & 0x7ULL) == 4ULL, "V32NMAD NegP0 predicate must encode 4");

    // KILL with ShortPredicate::P0 (1) and NegP0 (3)
    MicrocodeWord kill_p0 = MicrocodeWord::encode_kill(ShortPredicate::P0);
    TEST_ASSERT((kill_p0.raw_word >> 56) == 0b11111001ULL, "KILL op1/op2 mismatch");
    TEST_ASSERT(((kill_p0.raw_word >> 41) & 0x3ULL) == 1ULL, "KILL ShortPredicate::P0 must encode 1");
    MicrocodeWord kill_negp0 = MicrocodeWord::encode_kill(ShortPredicate::NegP0);
    TEST_ASSERT(((kill_negp0.raw_word >> 41) & 0x3ULL) == 3ULL, "KILL ShortPredicate::NegP0 must encode 3");

    // SMBO src1 offset encoding
    MicrocodeWord smbo = MicrocodeWord::encode_smbo(0, 0, 64, 0);
    TEST_ASSERT((smbo.raw_word >> 59) == 0b11111ULL, "SMBO op1 must be 11111");
    TEST_ASSERT(((smbo.raw_word >> 56) & 0x7ULL) == 0b011ULL, "SMBO op2 must be 011");
    TEST_ASSERT(((smbo.raw_word >> 52) & 0x3ULL) == 0b01ULL, "SMBO opcat must be 01");
    TEST_ASSERT(((smbo.raw_word >> 12) & 0xFFFULL) == 64ULL, "SMBO src1 offset must encode 64 at bits 23..12");

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
    TEST_ASSERT((read_u32(output->programData, 0x14) & (1u << 6)) == 0u, "Ordinary COLOR output shader must NOT set NATIVECOLOR_USED");
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
    const uint16_t varyings_count = read_u16(bytes, 0x9C + 0x0C);
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
    TEST_ASSERT_COMPILED(output, "PCF shadow + loop + helper shader must compile");

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

void test_gxp_vertex_varyings_and_frag_formats() {
    // 1. Vertex shader with CLIP0/1, PSIZE, FOG, _HALF texcoord, odd-aligned outputs
    const char* vp_code = R"(
        uniform float4x4 u_MVP;
        struct VS_OUT {
            float  psize : PSIZE;
            float3 uv1   : TEXCOORD1;
            float2 uv0   : TEXCOORD0_HALF;
            float  fog   : FOG;
            float4 color : COLOR0;
            float4 pos   : POSITION;
            float  clip0 : CLIP0;
            float  clip1 : CLIP1;
        };
        VS_OUT main(float4 p : POSITION, float2 t0 : TEXCOORD0, float3 t1 : TEXCOORD1) {
            VS_OUT o;
            o.pos = mul(u_MVP, p);
            o.color = float4(1.0f, 0.5f, 0.25f, 1.0f);
            o.fog = 0.75f;
            o.uv0 = t0;
            o.uv1 = t1;
            o.psize = 4.0f;
            o.clip0 = p.x;
            o.clip1 = p.y;
            return o;
        }
    )";

    SceShaccCgCompileOptions vp_opts;
    sceShaccCgInitializeCompileOptions(&vp_opts);
    vp_opts.mainSourceFile = vp_code;
    vp_opts.targetProfile = SCE_SHACCCG_PROFILE_VP;

    const SceShaccCgCompileOutput* vp_out = sceShaccCgCompileProgram(&vp_opts, nullptr, 0);
    TEST_ASSERT(vp_out != nullptr && vp_out->programData != nullptr, "Extended VP outputs shader must compile");

    const uint8_t* vp_bytes = vp_out->programData;
    const uint32_t v_out1 = read_u32(vp_bytes, 0x9C + 0x10);
    const uint32_t v_out2 = read_u32(vp_bytes, 0x9C + 0x14);
    const uint32_t tc_pack = read_u32(vp_bytes, 0x9C + 0x18);
    TEST_ASSERT((v_out1 & (1u << 0)) != 0u, "CLIP0 bit must be set");
    TEST_ASSERT((v_out1 & (1u << 1)) != 0u, "CLIP1 bit must be set");
    TEST_ASSERT((v_out1 & (1u << 8)) != 0u, "PSIZE bit must be set");
    TEST_ASSERT((v_out1 & (1u << 9)) != 0u, "FOG bit must be set");
    TEST_ASSERT((v_out1 & (1u << 11)) != 0u, "COLOR0 bit must be set");
    TEST_ASSERT((v_out2 & 0x7u) == 1u, "TEXCOORD0 float2 must encode 1 in vertex_outputs2");
    TEST_ASSERT(((v_out2 >> 3) & 0x7u) == 3u, "TEXCOORD1 float3 must encode 3 in vertex_outputs2");
    TEST_ASSERT((tc_pack & 0x1u) != 0u, "TEXCOORD0_HALF must set bit 0 in texcoord_pack_format");
    sceShaccCgDestroyCompileOutput(vp_out);

    // 2. Fragment shader with SPRITECOORD and TEXCOORD0_HALF
    const char* fp_code = R"(
        uniform sampler2D u_SpriteTex;
        float4 main(float2 tc : TEXCOORD0_HALF, float2 sp : SPRITECOORD) : COLOR {
            return tex2D(u_SpriteTex, sp * 0.5f + tc * 0.5f);
        }
    )";

    SceShaccCgCompileOptions fp_opts;
    sceShaccCgInitializeCompileOptions(&fp_opts);
    fp_opts.mainSourceFile = fp_code;
    fp_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;

    const SceShaccCgCompileOutput* fp_out = sceShaccCgCompileProgram(&fp_opts, nullptr, 0);
    TEST_ASSERT(fp_out != nullptr && fp_out->programData != nullptr, "SPRITECOORD + _HALF FP shader must compile");

    const uint8_t* fp_bytes = fp_out->programData;
    const uint32_t fp_flags = read_u32(fp_bytes, 0x14);
    TEST_ASSERT((fp_flags & (1u << 5)) != 0u, "GXP_PROGRAM_FLAG_SPRITECOORD (1<<5) must be set");
    TEST_ASSERT(read_u16(fp_bytes, 0x9C + 0x0C) == 2u, "Fragment varyings_count must be 2");
    TEST_ASSERT((read_u32(fp_bytes, 0x9C + 0x18) & 1u) != 0u, "Fragment texcoord_pack_format bit 0 must be set for _HALF");

    const uint32_t attr0_info = read_u32(fp_bytes, 0xBC + 0x00);
    const uint32_t attr1_info = read_u32(fp_bytes, 0xBC + 0x10);
    TEST_ASSERT((attr0_info & 0x20000000u) != 0u, "TEXCOORD0_HALF descriptor must set F16 bit 29");
    TEST_ASSERT((attr1_info & 0x40000000u) != 0u, "SPRITECOORD descriptor must set spritecoord bit 30");
    sceShaccCgDestroyCompileOutput(fp_out);

    g_tests_passed++;
    std::cout << "  [PASS] test_gxp_vertex_varyings_and_frag_formats\n";
}

void test_high_sa_smbo_and_dynamic_array_write() {
    const char* shader_code = R"(
        uniform float4 u_Bones[36];
        uniform float4 u_Tint;

        struct VS_OUT {
            float4 pos : POSITION;
            float4 col : COLOR0;
        };

        VS_OUT main(float4 pos : POSITION, float idx : TEXCOORD0) {
            VS_OUT o;
            float4 vClip[4];
            vClip[0] = float4(0.0f, 0.0f, 0.0f, 1.0f);
            vClip[1] = float4(0.0f, 0.0f, 0.0f, 1.0f);
            vClip[2] = float4(0.0f, 0.0f, 0.0f, 1.0f);
            vClip[3] = float4(0.0f, 0.0f, 0.0f, 1.0f);
            int k = int(idx);
            vClip[k] = u_Tint;
            vClip[k].xy += float2(0.1f, 0.2f);
            o.pos = pos + u_Bones[35];
            o.col = vClip[1] * u_Tint;
            return o;
        }
    )";

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = shader_code;
    opts.targetProfile = SCE_SHACCCG_PROFILE_VP;

    const SceShaccCgCompileOutput* output = sceShaccCgCompileProgram(&opts, nullptr, 0);
    TEST_ASSERT(output != nullptr && output->programData != nullptr, "High SA (>128) + dynamic array write shader must compile");

    const uint8_t* bytes = output->programData;
    const uint32_t instr_count = read_u32(bytes, 0x3C);
    const size_t ucode_abs = 0x40 + static_cast<size_t>(read_i32(bytes, 0x40));

    bool saw_smbo_nonzero = false;
    bool saw_smbo_reset = false;
    bool saw_vmov = false;
    for (uint32_t i = 0; i < instr_count; ++i) {
        uint64_t w = read_u64(bytes, ucode_abs + i * sizeof(uint64_t));
        uint64_t op1 = (w >> 59);
        if (op1 == 0b11111ULL && ((w >> 56) & 0x7ULL) == 0b011ULL && ((w >> 52) & 0x3ULL) == 0b01ULL) {
            uint64_t s1_off = (w >> 12) & 0xFFFULL;
            if (s1_off != 0ULL) saw_smbo_nonzero = true;
            else saw_smbo_reset = true;
        }
        if (op1 == 0b00111ULL) {
            saw_vmov = true;
        }
    }
    TEST_ASSERT(saw_smbo_nonzero, "High SA read (>=128) must emit nonzero SMBO window offset");
    TEST_ASSERT(saw_smbo_reset, "High SA read must emit SMBO 0 reset after VMOV");
    TEST_ASSERT(saw_vmov, "High SA read must emit VMOV transfer");

    sceShaccCgDestroyCompileOutput(output);
    g_tests_passed++;
    std::cout << "  [PASS] test_high_sa_smbo_and_dynamic_array_write\n";
}

void test_vitagl_real_world_ffp_and_glsl_translator() {
    // 1. vitaGL FFP-style multi-light vertex shader (preprocessor macros, loops, dynamic arrays, half texcoords)
    const char* ffp_v = R"(
        #define POSITION POSITION
        #define NUM_LIGHTS 4
        #define HAS_CLIP 1

        uniform float4x4 modelview;
        uniform float4x4 wvp;
        uniform float4 light_pos[NUM_LIGHTS];
        uniform float4 light_diff[NUM_LIGHTS];
        uniform float4 clip_planes[2];
        uniform float fog_density;

        float calc_diffuse(float3 n, float3 ldir) {
            return max(dot(n, normalize(ldir)), 0.0f);
        }

        void main(
            float3 pos : POSITION,
            float3 norm : NORMAL,
            float2 uv : TEXCOORD0,
            float4 out vPos : POSITION,
            float4 out vCol : COLOR0,
            float2 out vTex : TEXCOORD0_HALF,
            float  out vFog : FOG,
            float  out vPSize : PSIZE,
            float  out vClip[2] : CLP0
        ) {
            float4 p4 = float4(pos, 1.0f);
            float4 mv = mul(modelview, p4);
            vPos = mul(wvp, p4);
            vTex = uv;
            vPSize = 1.0f;
            vFog = clamp(exp(-fog_density * abs(mv.z)), 0.0f, 1.0f);

            float3 acc = float3(0.1f, 0.1f, 0.1f);
            for (int i = 0; i < NUM_LIGHTS; ++i) {
                float d = calc_diffuse(norm, light_pos[i].xyz - mv.xyz);
                acc += light_diff[i].rgb * d;
            }
            vCol = float4(saturate(acc), 1.0f);

            for (int c = 0; c < 2; ++c) {
                vClip[c] = dot(mv, clip_planes[c]);
            }
        }
    )";

    SceShaccCgCompileOptions vp_opts;
    sceShaccCgInitializeCompileOptions(&vp_opts);
    vp_opts.mainSourceFile = ffp_v;
    vp_opts.targetProfile = SCE_SHACCCG_PROFILE_VP;
    const SceShaccCgCompileOutput* vp_res = sceShaccCgCompileProgram(&vp_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(vp_res, "vitaGL FFP multi-light vertex shader must compile");
    TEST_ASSERT(read_u32(vp_res->programData, 0x34) < 120u, "FFP vertex shader must stay under 120 temps");
    TEST_ASSERT((read_u32(vp_res->programData, 0x9C + 0x10) & 0x3u) == 0x3u, "CLP0 array[2] must set CLIP0 and CLIP1 flags");
    sceShaccCgDestroyCompileOutput(vp_res);

    // 2. vitaGL FFP-style multi-texture fragment shader with alpha test discard
    const char* ffp_f = R"(
        #define NUM_TEX 2
        uniform sampler2D tex[NUM_TEX];
        uniform float4 texenv_col[NUM_TEX];
        uniform float4 fog_color;
        uniform float alpha_ref;

        float4 blend_stage(float4 prev, float4 sample_c, float4 env_c) {
            return lerp(prev, sample_c * env_c, env_c.a);
        }

        float4 main(
            float4 vCol : COLOR0,
            float2 vTex : TEXCOORD0_HALF,
            float2 vPoint : SPRITECOORD,
            float  vFog : FOG
        ) : COLOR {
            float4 res = vCol;
            for (int t = 0; t < NUM_TEX; ++t) {
                float2 tc = (t == 0) ? vTex : vPoint;
                float4 smp = (t == 0) ? tex2D(tex[0], tc) : tex2D(tex[1], tc);
                res = blend_stage(res, smp, texenv_col[t]);
            }
            if (res.a < alpha_ref) {
                discard;
            }
            return float4(lerp(fog_color.rgb, res.rgb, vFog), res.a);
        }
    )";

    SceShaccCgCompileOptions fp_opts;
    sceShaccCgInitializeCompileOptions(&fp_opts);
    fp_opts.mainSourceFile = ffp_f;
    fp_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* fp_res = sceShaccCgCompileProgram(&fp_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(fp_res, "vitaGL FFP fragment shader must compile");
    TEST_ASSERT(read_u32(fp_res->programData, 0x34) < 120u, "FFP fragment shader must stay under 120 temps");
    TEST_ASSERT((read_u32(fp_res->programData, 0x14) & (1u << 3)) != 0u, "DISCARD_USED flag must be set");
    TEST_ASSERT((read_u32(fp_res->programData, 0x14) & (1u << 5)) != 0u, "SPRITECOORD flag must be set");
    sceShaccCgDestroyCompileOutput(fp_res);

    // 3. vitaGL glsl_translator-style shader with varying out/in, overloads, sampler helper, trig & bitops
    const char* glsl_trans = R"(
        #define VOUT varying out
        #define VIN varying in
        VOUT float4 gl_Position : POSITION;
        VIN float2 v_uv : TEXCOORD0;
        uniform sampler2D u_Tex;
        uniform float4x4 u_Mat;

        float4 vglMul(float4 a, float4 b) { return a * b; }
        float4 vglMul(float4x4 m, float4 v) { return mul(m, v); }
        int4 vglMul(int4 a, int4 b) { return a * b; }

        float4 glslTexture2D(sampler2D x, float2 s) {
            return tex2D(x, s);
        }

        void main(float4 a_pos : POSITION, out float4 v_col : COLOR0) {
            int mask = (1 << 2) | 3;
            float angle = atan(v_uv.y, v_uv.x) + asin(0.5f) * (float(mask) * 0.1f);
            float4 pos = vglMul(u_Mat, a_pos);
            float4 tex_c = glslTexture2D(u_Tex, v_uv);
            v_col = vglMul(vglUnpack(tex_c), float4(cos(angle), fmod(angle, 1.0f), 1.0f, 1.0f));
            gl_Position = pos;
        }
    )";

    SceShaccCgCompileOptions glsl_opts;
    sceShaccCgInitializeCompileOptions(&glsl_opts);
    glsl_opts.mainSourceFile = glsl_trans;
    glsl_opts.targetProfile = SCE_SHACCCG_PROFILE_VP;
    const SceShaccCgCompileOutput* glsl_res = sceShaccCgCompileProgram(&glsl_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(glsl_res, "vitaGL glsl_translator-style shader must compile");
    TEST_ASSERT(read_u32(glsl_res->programData, 0x34) < 120u, "glsl_translator shader must stay under 120 temps");
    sceShaccCgDestroyCompileOutput(glsl_res);

    g_tests_passed++;
    std::cout << "  [PASS] test_vitagl_real_world_ffp_and_glsl_translator\n";
}

void test_gxp_uniform_stride_nativecolor_and_sampler_sa() {
    // 1. Tight uniform packing: float3x3 (9 words, row_stride=3), float[3] (3), float2[2] (4), float3[2] (6)
    const char* stride_src = R"(
        uniform float3x3 u_NormalMat;
        uniform float    u_Weights[3];
        uniform float2   u_Offsets[2];
        uniform float3   u_Colors[2];

        float4 main(float3 n : NORMAL, float4 p : POSITION) : POSITION {
            float3 tn = mul(u_NormalMat, n);
            float w = u_Weights[0] + u_Weights[2];
            float2 off = u_Offsets[1];
            float3 col = u_Colors[1];
            return float4(tn + col, w + off.x + p.w);
        }
    )";
    SceShaccCgCompileOptions s_opts;
    sceShaccCgInitializeCompileOptions(&s_opts);
    s_opts.mainSourceFile = stride_src;
    s_opts.targetProfile = SCE_SHACCCG_PROFILE_VP;
    const SceShaccCgCompileOutput* s_out = sceShaccCgCompileProgram(&s_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(s_out, "Tight uniform stride shader must compile");

    SceShaccCgParameter p_mat = sceShaccCgGetParameterByName(s_out, "u_NormalMat");
    TEST_ASSERT(p_mat != nullptr, "u_NormalMat must be reflected");
    TEST_ASSERT(sceShaccCgGetParameterColumns(p_mat) == 3u && sceShaccCgGetParameterRows(p_mat) == 3u, "u_NormalMat 3x3");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetRowParameter(p_mat, 0)) == 0u, "float3x3 row 0 at UB+0");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetRowParameter(p_mat, 1)) == 3u, "float3x3 row 1 at UB+3");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetRowParameter(p_mat, 2)) == 6u, "float3x3 row 2 at UB+6");

    SceShaccCgParameter p_w = sceShaccCgGetParameterByName(s_out, "u_Weights");
    TEST_ASSERT(p_w != nullptr && sceShaccCgGetArraySize(p_w) == 3u, "u_Weights[3] reflected");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_w, 0)) == 9u, "float[3] elem 0 at UB+9");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_w, 1)) == 10u, "float[3] elem 1 at UB+10");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_w, 2)) == 11u, "float[3] elem 2 at UB+11");

    SceShaccCgParameter p_off = sceShaccCgGetParameterByName(s_out, "u_Offsets");
    TEST_ASSERT(p_off != nullptr && sceShaccCgGetArraySize(p_off) == 2u, "u_Offsets[2] reflected");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_off, 0)) == 12u, "float2[2] elem 0 at UB+12");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_off, 1)) == 14u, "float2[2] elem 1 at UB+14");

    SceShaccCgParameter p_col = sceShaccCgGetParameterByName(s_out, "u_Colors");
    TEST_ASSERT(p_col != nullptr && sceShaccCgGetArraySize(p_col) == 2u, "u_Colors[2] reflected");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_col, 0)) == 16u, "float3[2] elem 0 at UB+16");
    TEST_ASSERT(sceShaccCgGetParameterResourceIndex(sceShaccCgGetArrayParameter(p_col, 1)) == 19u, "float3[2] elem 1 at UB+19");
    TEST_ASSERT(read_u32(s_out->programData, 0x64) == 22u, "Tight UB count must be 9 + 3 + 4 + 6 = 22 words");
    sceShaccCgDestroyCompileOutput(s_out);

    // 2. Conditional NATIVECOLOR_USED when reading native framebuffer color
    const char* nc_src = R"(
        uniform float4 u_SrcColor;
        void main(inout float4 io_Col : COLOR) {
            io_Col = lerp(io_Col, u_SrcColor, u_SrcColor.a);
        }
    )";
    SceShaccCgCompileOptions nc_opts;
    sceShaccCgInitializeCompileOptions(&nc_opts);
    nc_opts.mainSourceFile = nc_src;
    nc_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* nc_out = sceShaccCgCompileProgram(&nc_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(nc_out, "Programmable blend inout COLOR shader must compile");
    TEST_ASSERT((read_u32(nc_out->programData, 0x14) & (1u << 6)) != 0u, "inout COLOR shader must set NATIVECOLOR_USED");
    sceShaccCgDestroyCompileOutput(nc_out);

    // 3. Sampler placed at sa0 even with > 260 uniform F32 words (no 7-bit SMP SA truncation!)
    const char* big_sa_src = R"(
        uniform float4 u_BigPalette[70];
        uniform sampler2D u_Tex;
        float4 main(float2 uv : TEXCOORD0) : COLOR {
            return tex2D(u_Tex, uv) * u_BigPalette[69];
        }
    )";
    SceShaccCgCompileOptions big_opts;
    sceShaccCgInitializeCompileOptions(&big_opts);
    big_opts.mainSourceFile = big_sa_src;
    big_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* big_out = sceShaccCgCompileProgram(&big_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(big_out, "High uniform + sampler shader must compile");

    const uint8_t* bytes = big_out->programData;
    TEST_ASSERT(read_u32(bytes, 0x64) == 280u, "Default UB must be 280 F32 words");
    TEST_ASSERT(read_u16(bytes, 0x32) == 284u, "Total SA regs must be 4 (sampler) + 280 (UB) = 284");

    const uint32_t cont_count = read_u32(bytes, 0x90);
    const size_t cont_abs = 0x94 + static_cast<size_t>(read_i32(bytes, 0x94));
    TEST_ASSERT(cont_count == 2u, "Must have containers 14 and 19");
    uint16_t cont14_base = 0xFFFFu;
    uint16_t cont19_base = 0xFFFFu;
    for (uint32_t i = 0; i < cont_count; ++i) {
        uint16_t idx = read_u16(bytes, cont_abs + i * 8u);
        uint16_t base_sa = read_u16(bytes, cont_abs + i * 8u + 4u);
        if (idx == 14u) cont14_base = base_sa;
        if (idx == 19u) cont19_base = base_sa;
    }
    TEST_ASSERT(cont19_base == 0u, "Container 19 (sampler) base_sa_offset must be sa0");
    TEST_ASSERT(cont14_base == 4u, "Container 14 (default UB) base_sa_offset must be sa4");

    // Verify SMP instruction actually encodes sa0 (bits 13:7 == 0)
    const uint32_t instr_count = read_u32(bytes, 0x3C);
    const size_t ucode_abs = 0x40 + static_cast<size_t>(read_i32(bytes, 0x40));
    bool saw_smp_sa0 = false;
    for (uint32_t i = 0; i < instr_count; ++i) {
        uint64_t w = read_u64(bytes, ucode_abs + i * sizeof(uint64_t));
        if ((w >> 59) == 0b11100ULL) {
            TEST_ASSERT(((w >> 7) & 0x7FULL) == 0ULL, "SMP must encode sampler SA offset 0 (sa0)");
            saw_smp_sa0 = true;
        }
    }
    TEST_ASSERT(saw_smp_sa0, "Must emit SMP instruction at sa0");
    sceShaccCgDestroyCompileOutput(big_out);

    g_tests_passed++;
    std::cout << "  [PASS] test_gxp_uniform_stride_nativecolor_and_sampler_sa\n";
}

void test_dynamic_bitwise_varying_linkage_preprocessor_and_reflection() {
    // 1. Dynamic runtime bitwise/shift ops (<<, >>, &, |, ^, ~)
    const char* bit_src = R"(
        #define APPLY_MIX(fn, ...) fn(__VA_ARGS__)

        float4 main(float4 v : COLOR0, float s : TEXCOORD0) : COLOR {
            int a = int(v.x * 255.0f);
            int b = int(v.y * 255.0f);
            int sh = int(s * 3.0f);
            int dyn_shl = a << sh;
            int dyn_shr = b >> sh;
            int dyn_and = dyn_shl & b;
            int dyn_or  = dyn_and | dyn_shr;
            int dyn_xor = dyn_or ^ (~a);
            float res = APPLY_MIX(saturate, float(dyn_xor) * 0.001f);
            return float4(res, float(a & 15), float(b & 7), 1.0f);
        }
    )";
    SceShaccCgCompileOptions bit_opts;
    sceShaccCgInitializeCompileOptions(&bit_opts);
    bit_opts.mainSourceFile = bit_src;
    bit_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* bit_out = sceShaccCgCompileProgram(&bit_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(bit_out, "Dynamic bitwise/shift + variadic macro shader must compile");

    const uint8_t* bit_bytes = bit_out->programData;
    const uint32_t bit_icount = read_u32(bit_bytes, 0x3C);
    const size_t bit_ucode = 0x40 + static_cast<size_t>(read_i32(bit_bytes, 0x40));
    bool saw_vfrc = false;
    bool saw_vexp = false;
    bool saw_br = false;
    for (uint32_t i = 0; i < bit_icount; ++i) {
        uint64_t w = read_u64(bit_bytes, bit_ucode + i * sizeof(uint64_t));
        uint64_t op1 = (w >> 59);
        if (op1 == 0b00110ULL) {
            uint64_t op2 = (w >> 41) & 0x3ULL;
            if (op2 == 3ULL) saw_vexp = true;
        }
        if (op1 == 0b00001ULL) {
            uint64_t op2 = (w >> 12) & 0x7ULL;
            if (op2 == 2ULL) saw_vfrc = true;
        }
        if (op1 == 0b11111ULL && ((w >> 52) & 3ULL) == 0ULL && ((w >> 38) & 1ULL) == 1ULL) {
            saw_br = true;
        }
    }
    TEST_ASSERT(saw_vexp, "Dynamic shift must emit VEXP instruction");
    TEST_ASSERT(saw_vfrc, "Dynamic bitwise & / | / ^ must emit VFRC LSB extraction");
    TEST_ASSERT(saw_br, "Dynamic bitwise loop must emit BR loop instruction");
    sceShaccCgDestroyCompileOutput(bit_out);

    // 2. Varying linkage: preserve explicit NORMAL/TANGENT/DIFFUSE semantics & assign non-colliding canonical slots
    const char* vp_link = R"(
        uniform float4x4 u_MVP;
        struct VS_OUT {
            float3 tangent : TANGENT;
            float2 uv0     : TEXCOORD0;
            float4 diff    : DIFFUSE;
            float3 normal  : NORMAL;
            float4 pos     : POSITION;
        };
        VS_OUT main(float4 p : POSITION, float3 n : NORMAL, float3 t : TANGENT, float2 uv : TEXCOORD0) {
            VS_OUT o;
            o.pos = mul(u_MVP, p);
            o.normal = n;
            o.tangent = t;
            o.uv0 = uv;
            o.diff = float4(1.0f, 1.0f, 1.0f, 1.0f);
            return o;
        }
    )";
    SceShaccCgCompileOptions vpl_opts;
    sceShaccCgInitializeCompileOptions(&vpl_opts);
    vpl_opts.mainSourceFile = vp_link;
    vpl_opts.targetProfile = SCE_SHACCCG_PROFILE_VP;
    const SceShaccCgCompileOutput* vpl_out = sceShaccCgCompileProgram(&vpl_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(vpl_out, "VP with NORMAL/TANGENT/DIFFUSE/TEXCOORD0 must compile");

    SceShaccCgParameter p_vnorm = sceShaccCgGetParameterByName(vpl_out, "normal");
    SceShaccCgParameter p_vtang = sceShaccCgGetParameterByName(vpl_out, "tangent");
    SceShaccCgParameter p_vdiff = sceShaccCgGetParameterByName(vpl_out, "diff");
    TEST_ASSERT(p_vnorm && std::string(sceShaccCgGetParameterSemantic(p_vnorm)) == "NORMAL", "VP must preserve NORMAL semantic");
    TEST_ASSERT(p_vtang && std::string(sceShaccCgGetParameterSemantic(p_vtang)) == "TANGENT", "VP must preserve TANGENT semantic");
    TEST_ASSERT(p_vdiff && std::string(sceShaccCgGetParameterSemantic(p_vdiff)) == "DIFFUSE", "VP must preserve DIFFUSE semantic");
    const uint32_t vp_vout1 = read_u32(vpl_out->programData, 0x9C + 0x10);
    const uint32_t vp_vout2 = read_u32(vpl_out->programData, 0x9C + 0x14);
    TEST_ASSERT((vp_vout1 & (1u << 11)) != 0u, "DIFFUSE must map to COLOR0 bit in vertex_outputs1");
    TEST_ASSERT((vp_vout2 & 0x7u) == 1u, "Explicit TEXCOORD0 float2 stays at slot 0");
    TEST_ASSERT(((vp_vout2 >> 3) & 0x7u) == 3u, "NORMAL float3 assigns canonical TEXCOORD1 slot 1");
    TEST_ASSERT(((vp_vout2 >> 6) & 0x7u) == 3u, "TANGENT float3 assigns canonical TEXCOORD2 slot 2");
    sceShaccCgDestroyCompileOutput(vpl_out);

    // Fragment shader declaring same varyings in reversed order
    const char* fp_link = R"(
        float4 main(
            float3 norm : NORMAL,
            float4 diff : DIFFUSE,
            float3 tang : TANGENT,
            float2 uv0  : TEXCOORD0
        ) : COLOR {
            return float4(normalize(norm + tang) * uv0.x, 1.0f) * diff;
        }
    )";
    SceShaccCgCompileOptions fpl_opts;
    sceShaccCgInitializeCompileOptions(&fpl_opts);
    fpl_opts.mainSourceFile = fp_link;
    fpl_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* fpl_out = sceShaccCgCompileProgram(&fpl_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(fpl_out, "FP with NORMAL/DIFFUSE/TANGENT/TEXCOORD0 must compile");

    SceShaccCgParameter p_fnorm = sceShaccCgGetParameterByName(fpl_out, "norm");
    TEST_ASSERT(p_fnorm && std::string(sceShaccCgGetParameterSemantic(p_fnorm)) == "NORMAL", "FP must preserve NORMAL semantic");
    TEST_ASSERT(read_u16(fpl_out->programData, 0x9C + 0x0C) == 4u, "FP must have 4 varyings");
    // Check iterator encodings: bits 15:12 encode 0..9=TEXCOORD0..9, 0xA=COLOR0
    // [0]=NORMAL->TEXCOORD1 (0x1), [1]=DIFFUSE->COLOR0 (0xA), [2]=TANGENT->TEXCOORD2 (0x2), [3]=TEXCOORD0 (0x0)
    auto read_it_id = [&](uint32_t k) -> uint32_t {
        uint32_t info = read_u32(fpl_out->programData, 0xBC + k * 16u);
        return (info >> 12) & 0xFu;
    };
    TEST_ASSERT(read_it_id(0) == 0x1u, "FP NORMAL must map to hw TEXCOORD1 (0x1) matching VP");
    TEST_ASSERT(read_it_id(1) == 0xAu, "FP DIFFUSE must map to hw COLOR0 (0xA) matching VP");
    TEST_ASSERT(read_it_id(2) == 0x2u, "FP TANGENT must map to hw TEXCOORD2 (0x2) matching VP");
    TEST_ASSERT(read_it_id(3) == 0x0u, "FP TEXCOORD0 must stay on hw TEXCOORD0 (0x0) matching VP");
    sceShaccCgDestroyCompileOutput(fpl_out);

    // 3. Preprocessor error propagation (#error, missing #include, unterminated #if)
    auto expect_prep_fail = [&](const char* bad_prep_src, const char* label) -> bool {
        SceShaccCgCompileOptions o;
        sceShaccCgInitializeCompileOptions(&o);
        o.mainSourceFile = bad_prep_src;
        o.targetProfile = SCE_SHACCCG_PROFILE_FP;
        const SceShaccCgCompileOutput* res = sceShaccCgCompileProgram(&o, nullptr, 0);
        bool ok = (res != nullptr && res->programData == nullptr && res->diagnosticCount > 0);
        if (res) sceShaccCgDestroyCompileOutput(res);

        void* bin = nullptr;
        size_t sz = 0;
        char* log = nullptr;
        int rc = SceShaccCgCompileProgram(bad_prep_src, 0, nullptr, &bin, &sz, &log);
        if (rc == OPEN_SHACC_SUCCESS || log == nullptr) ok = false;
        if (log) SceShaccCgDestroyDiagnosticLog(log);
        if (bin) SceShaccCgDestroyProgram(bin);
        if (!ok) {
            std::cerr << "  [FAIL] Preprocessor failure expected for " << label << "\n";
        }
        return ok;
    };
    TEST_ASSERT(expect_prep_fail("#error unsupported target\nfloat4 main() : COLOR { return float4(1,0,0,1); }", "#error"), "#error must fail compilation");
    TEST_ASSERT(expect_prep_fail("#include \"nonexistent_header.h\"\nfloat4 main() : COLOR { return float4(1,0,0,1); }", "#include"), "Missing #include must fail compilation");
    TEST_ASSERT(expect_prep_fail("#if 1\nfloat4 main() : COLOR { return float4(1,0,0,1); }", "unclosed #if"), "Unclosed #if must fail compilation");

    // 4. Reflection: struct user type, cbuffer uniform block members, and sampler query format
    const char* refl_src = R"(
        struct Material {
            float4 albedo;
            float  roughness;
        };
        cbuffer PerFrame {
            float4x4 u_ViewProj;
            float    u_Time;
        };
        uniform Material u_Mat;
        uniform sampler2D u_AlbedoTex;

        float4 main(float2 uv : TEXCOORD0, float4 p : POSITION) : COLOR {
            float4 c = tex2D(u_AlbedoTex, uv);
            float4 proj = mul(u_ViewProj, p);
            return c * u_Mat.albedo * saturate(u_Mat.roughness + u_Time + proj.x);
        }
    )";
    SceShaccCgCompileOptions refl_opts;
    sceShaccCgInitializeCompileOptions(&refl_opts);
    refl_opts.mainSourceFile = refl_src;
    refl_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* refl_out = sceShaccCgCompileProgram(&refl_opts, nullptr, 0);
    TEST_ASSERT_COMPILED(refl_out, "Struct + cbuffer + sampler reflection shader must compile");

    SceShaccCgParameter p_mat = sceShaccCgGetParameterByName(refl_out, "u_Mat");
    TEST_ASSERT(p_mat != nullptr, "Struct uniform u_Mat must be reflected");
    TEST_ASSERT(sceShaccCgGetParameterClass(p_mat) == SCE_SHACCCG_PARAMETERCLASS_STRUCT, "u_Mat class must be STRUCT");
    TEST_ASSERT(std::string(sceShaccCgGetParameterUserType(p_mat)) == "Material", "GetParameterUserType(u_Mat) must return Material");
    SceShaccCgParameter f0 = sceShaccCgGetFirstStructParameter(p_mat);
    TEST_ASSERT(f0 != nullptr && std::string(sceShaccCgGetParameterName(f0)) == "albedo", "First struct field must be albedo");
    SceShaccCgParameter f1 = sceShaccCgGetNextParameter(f0);
    TEST_ASSERT(f1 != nullptr && std::string(sceShaccCgGetParameterName(f1)) == "roughness", "Second struct field must be roughness");

    SceShaccCgParameter p_blk = sceShaccCgGetParameterByName(refl_out, "PerFrame");
    TEST_ASSERT(p_blk != nullptr, "cbuffer PerFrame must be reflected");
    TEST_ASSERT(sceShaccCgGetParameterClass(p_blk) == SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK, "PerFrame class must be UNIFORMBLOCK");
    TEST_ASSERT(std::string(sceShaccCgGetParameterUserType(p_blk)) == "PerFrame", "GetParameterUserType(PerFrame) must return PerFrame");
    SceShaccCgParameter b0 = sceShaccCgGetFirstUniformBlockParameter(p_blk);
    TEST_ASSERT(b0 != nullptr && std::string(sceShaccCgGetParameterName(b0)) == "u_ViewProj", "First UB member must be u_ViewProj");
    SceShaccCgParameter b1 = sceShaccCgGetNextParameter(b0);
    TEST_ASSERT(b1 != nullptr && std::string(sceShaccCgGetParameterName(b1)) == "u_Time", "Second UB member must be u_Time");

    SceShaccCgParameter p_ts = sceShaccCgGetParameterByName(refl_out, "u_AlbedoTex");
    TEST_ASSERT(p_ts != nullptr, "u_AlbedoTex must be reflected");
    TEST_ASSERT(sceShaccCgGetSamplerQueryFormatPrecision(p_ts) == SCE_SHACCCG_BASETYPE_FLOAT, "Sampler format precision must be FLOAT");
    TEST_ASSERT(sceShaccCgGetSamplerQueryFormatPrecisionCount(p_ts) == 1u, "Sampler format precision count must be 1");
    TEST_ASSERT(sceShaccCgGetSamplerQueryFormatWidth(p_ts) == 4u, "Sampler query width must be 4");

    sceShaccCgDestroyCompileOutput(refl_out);

    g_tests_passed++;
    std::cout << "  [PASS] test_dynamic_bitwise_varying_linkage_preprocessor_and_reflection\n";
}

static SceShaccCgSourceFile* nul_padded_open_cb(
    const char* fileName,
    const SceShaccCgSourceLocation*,
    const SceShaccCgCompileOptions*,
    const char**
) {
    static const char kSrcWithTrailingNul[] = "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(uv, 0.0f, 1.0f); }\0";
    static SceShaccCgSourceFile f{};
    f.fileName = fileName;
    f.text = kSrcWithTrailingNul;
    f.size = static_cast<uint32_t>(sizeof(kSrcWithTrailingNul)); // includes trailing NUL
    return &f;
}

void test_callback_system_files_and_nul_buffer() {
    SceShaccCgCallbackList sys_cb;
    sceShaccCgInitializeCallbackList(&sys_cb, SCE_SHACCCG_SYSTEM_FILES);
    TEST_ASSERT(sys_cb.openFile != nullptr && sys_cb.releaseFile != nullptr, "SYSTEM_FILES must populate openFile/releaseFile");
    TEST_ASSERT(sys_cb.locateFile != nullptr && sys_cb.releaseFileName != nullptr, "SYSTEM_FILES must populate locateFile/releaseFileName");

    SceShaccCgCompileOptions sys_opts;
    sceShaccCgInitializeCompileOptions(&sys_opts);
    sys_opts.mainSourceFile = "samples/basic_mvp.cg";
    sys_opts.targetProfile = SCE_SHACCCG_PROFILE_VP;
    const SceShaccCgCompileOutput* sys_out = sceShaccCgCompileProgram(&sys_opts, &sys_cb, 0);
    TEST_ASSERT_COMPILED(sys_out, "SYSTEM_FILES callback must load and compile samples/basic_mvp.cg");
    sceShaccCgDestroyCompileOutput(sys_out);

    SceShaccCgCallbackList nul_cb;
    sceShaccCgInitializeCallbackList(&nul_cb, SCE_SHACCCG_TRIVIAL);
    TEST_ASSERT(nul_cb.openFile == nullptr, "TRIVIAL callback list must leave openFile null");
    nul_cb.openFile = &nul_padded_open_cb;

    SceShaccCgCompileOptions nul_opts;
    sceShaccCgInitializeCompileOptions(&nul_opts);
    nul_opts.mainSourceFile = "inline_nul.cg";
    nul_opts.targetProfile = SCE_SHACCCG_PROFILE_FP;
    const SceShaccCgCompileOutput* nul_out = sceShaccCgCompileProgram(&nul_opts, &nul_cb, 0);
    TEST_ASSERT_COMPILED(nul_out, "openFile buffer with trailing NUL counted in size must compile cleanly");
    sceShaccCgDestroyCompileOutput(nul_out);

    g_tests_passed++;
    std::cout << "  [PASS] test_callback_system_files_and_nul_buffer\n";
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
    test_gxp_vertex_varyings_and_frag_formats();
    test_high_sa_smbo_and_dynamic_array_write();
    test_vitagl_real_world_ffp_and_glsl_translator();
    test_gxp_uniform_stride_nativecolor_and_sampler_sa();
    test_dynamic_bitwise_varying_linkage_preprocessor_and_reflection();
    test_callback_system_files_and_nul_buffer();
    test_syntax_error_handling();

    std::cout << "\nTest Results: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return (g_tests_failed == 0) ? 0 : 1;
}
