/**
 * @file test_shaders.cpp
 * @brief Clean-room automated test suite for open-shacc compiler library.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "open_shacc.h"
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

void test_version() {
    const char* ver = SceShaccCgGetVersion();
    TEST_ASSERT(ver != nullptr && std::string(ver).find("open-shacc") != std::string::npos, "Version string contains open-shacc");

    const char* scver = sceShaccCgGetVersionString();
    TEST_ASSERT(scver != nullptr && std::string(scver).find("open-shacc") != std::string::npos, "sceShaccCg version string contains open-shacc");

    g_tests_passed++;
    std::cout << "  [PASS] test_version\n";
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

    SceShaccCgParameter p_mvp = sceShaccCgGetParameterByName(output, "u_MVPMatrix");
    TEST_ASSERT(p_mvp != nullptr, "Should find u_MVPMatrix uniform parameter");
    TEST_ASSERT(sceShaccCgGetParameterClass(p_mvp) == SCE_SHACCCG_PARAMETERCLASS_MATRIX, "u_MVPMatrix must be matrix class");
    TEST_ASSERT(sceShaccCgGetParameterRows(p_mvp) == 4, "u_MVPMatrix rows must be 4");
    TEST_ASSERT(sceShaccCgGetParameterBufferIndex(p_mvp) == 14, "Uniform buffer index must be 14");

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

    SceShaccCgParameter p_vpos = sceShaccCgGetParameterByName(output, "vPosition");
    TEST_ASSERT(p_vpos != nullptr, "out parameter vPosition must be reflected");
    TEST_ASSERT(sceShaccCgGetParameterDirection(p_vpos) == SCE_SHACCCG_DIRECTION_OUT, "vPosition must be DIRECTION_OUT");

    sceShaccCgDestroyCompileOutput(output);

    g_tests_passed++;
    std::cout << "  [PASS] test_vitagl_style_shader_and_preprocessor\n";
}

void test_dot_product_lighting() {
    const char* shader_code = R"(
        uniform float3 u_LightDir;

        float4 main(float3 normal : NORMAL, float4 pos : POSITION) : POSITION {
            float ndotl = dot(normal, u_LightDir);
            float4 out_pos = pos;
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
    test_basic_mvp_shader();
    test_sceshacccg_abi_and_reflection();
    test_vitagl_style_shader_and_preprocessor();
    test_dot_product_lighting();
    test_syntax_error_handling();

    std::cout << "\nTest Results: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return (g_tests_failed == 0) ? 0 : 1;
}
