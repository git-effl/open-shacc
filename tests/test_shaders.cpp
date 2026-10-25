/**
 * @file test_shaders.cpp
 * @brief Clean-room automated test suite for open-shacc compiler library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "open_shacc.h"
#include <iostream>
#include <cassert>
#include <cstring>
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

void test_version() {
    const char* ver = SceShaccCgGetVersion();
    TEST_ASSERT(ver != nullptr && std::string(ver).find("open-shacc") != std::string::npos, "Version string contains open-shacc");
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
        // std::cout << log << "\n";
        SceShaccCgDestroyDiagnosticLog(log);
    }

    TEST_ASSERT(res == OPEN_SHACC_SUCCESS, "Compilation should succeed for MVP vertex shader");
    TEST_ASSERT(bin != nullptr, "Binary buffer must not be null");
    TEST_ASSERT(size > 0, "Binary size must be greater than zero");
    TEST_ASSERT(size % 16 == 0, "Container size must be 16-byte aligned");

    // Verify GXP magic
    const char* magic = static_cast<const char*>(bin);
    TEST_ASSERT(magic[0] == 'G' && magic[1] == 'X' && magic[2] == 'P' && magic[3] == 0, "Magic must be 'GXP\\0'");

    SceShaccCgDestroyProgram(bin);
    g_tests_passed++;
    std::cout << "  [PASS] test_basic_mvp_shader\n";
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
    test_dot_product_lighting();
    test_syntax_error_handling();

    std::cout << "\nTest Results: " << g_tests_passed << " passed, " << g_tests_failed << " failed.\n";
    return (g_tests_failed == 0) ? 0 : 1;
}
