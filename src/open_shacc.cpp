/**
 * @file open_shacc.cpp
 * @brief Public C API implementation for open-shacc shader compiler.
 *
 * Clean-room replacement for libshacccg.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/open_shacc.h"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "backend/usse_codegen.hpp"
#include "packer/gxp_encoder.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>

static char* duplicate_string(const char* str) {
    if (!str) return nullptr;
    size_t len = std::strlen(str);
    char* copy = static_cast<char*>(std::malloc(len + 1));
    if (copy) {
        std::memcpy(copy, str, len + 1);
    }
    return copy;
}

extern "C" {

OPEN_SHACC_API int SceShaccCgCompileProgram(
    const char* source_code,
    [[maybe_unused]] uint32_t options,
    [[maybe_unused]] const void* compile_options,
    void** out_program_binary,
    size_t* out_size,
    char** out_diagnostic_log
) {
    if (!source_code || !out_program_binary || !out_size) {
        if (out_diagnostic_log) {
            *out_diagnostic_log = duplicate_string("open-shacc: Null pointer passed in required arguments.");
        }
        return OPEN_SHACC_ERROR_INVALID_ARGUMENT;
    }

    *out_program_binary = nullptr;
    *out_size = 0;
    if (out_diagnostic_log) {
        *out_diagnostic_log = nullptr;
    }

    std::ostringstream diag_stream;
    diag_stream << "open-shacc v1.0.0 (Clean-room Cg/HLSL to USSE compiler)\n";

    // 1. Lexical Analysis
    open_shacc::frontend::Lexer lexer(source_code);
    auto tokens = lexer.tokenize();

    // 2. Syntax & Semantic Parsing
    open_shacc::frontend::Parser parser(std::move(tokens));
    open_shacc::frontend::TranslationUnit tu;

    if (!parser.parse(tu)) {
        diag_stream << "[ERROR] Parsing failed:\n" << parser.get_diagnostics_string();
        if (out_diagnostic_log) {
            *out_diagnostic_log = duplicate_string(diag_stream.str().c_str());
        }
        return OPEN_SHACC_ERROR_PARSE_FAILED;
    }

    diag_stream << "[INFO] Parsed successfully. Structs: " << tu.structs.size()
                << ", Uniforms: " << tu.uniforms.size()
                << ", Functions: " << tu.functions.size() << "\n";

    // 3. Target Code Generation & Register Allocation
    open_shacc::backend::UsseCodeGenerator codegen;
    open_shacc::backend::CompiledProgram program;

    if (!codegen.generate(tu, program)) {
        diag_stream << "[ERROR] Codegen failed: " << codegen.get_error_message() << "\n";
        if (out_diagnostic_log) {
            *out_diagnostic_log = duplicate_string(diag_stream.str().c_str());
        }
        return OPEN_SHACC_ERROR_CODEGEN_FAILED;
    }

    diag_stream << "[INFO] Generated " << program.instructions.size() << " USSE instructions ("
                << program.microcode_words.size() * 4 << " bytes microcode)\n";
    diag_stream << "[INFO] Allocated registers: "
                << static_cast<int>(program.temp_register_count) << " temps, "
                << static_cast<int>(program.const_register_count) << " constants\n";

    // 4. Container Packing (GXP serialization)
    open_shacc::packer::GxpEncoder encoder;
    std::vector<uint8_t> gxp_bytes;

    if (!encoder.encode(program, gxp_bytes)) {
        diag_stream << "[ERROR] GXP packaging failed: " << encoder.get_error_message() << "\n";
        if (out_diagnostic_log) {
            *out_diagnostic_log = duplicate_string(diag_stream.str().c_str());
        }
        return OPEN_SHACC_ERROR_PACKER_FAILED;
    }

    // Allocate output program buffer
    void* buffer = std::malloc(gxp_bytes.size());
    if (!buffer) {
        diag_stream << "[ERROR] Memory allocation failure for program binary\n";
        if (out_diagnostic_log) {
            *out_diagnostic_log = duplicate_string(diag_stream.str().c_str());
        }
        return OPEN_SHACC_ERROR_OUT_OF_MEMORY;
    }

    std::memcpy(buffer, gxp_bytes.data(), gxp_bytes.size());
    *out_program_binary = buffer;
    *out_size = gxp_bytes.size();

    diag_stream << "[SUCCESS] GXP package built successfully (" << gxp_bytes.size() << " bytes)\n";

    if (options & OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY) {
        diag_stream << "\n" << program.disassembly;
    }

    if (out_diagnostic_log) {
        *out_diagnostic_log = duplicate_string(diag_stream.str().c_str());
    }

    return OPEN_SHACC_SUCCESS;
}

OPEN_SHACC_API void SceShaccCgDestroyProgram(void* program_binary) {
    if (program_binary) {
        std::free(program_binary);
    }
}

OPEN_SHACC_API void SceShaccCgDestroyDiagnosticLog(char* diagnostic_log) {
    if (diagnostic_log) {
        std::free(diagnostic_log);
    }
}

OPEN_SHACC_API const char* SceShaccCgGetVersion(void) {
    return "open-shacc 1.0.0 (Clean-room C++20 USSE/GXP Shader Compiler)";
}

OPEN_SHACC_API int SceShaccCgDisassembleGxp(
    const void* gxp_binary,
    size_t gxp_size,
    char** out_disasm
) {
    if (!gxp_binary || !out_disasm || gxp_size < sizeof(open_shacc::packer::GxpHeader)) {
        return OPEN_SHACC_ERROR_INVALID_ARGUMENT;
    }

    std::string err;
    const auto* data = static_cast<const uint8_t*>(gxp_binary);
    if (!open_shacc::packer::GxpEncoder::validate(data, gxp_size, err)) {
        return OPEN_SHACC_ERROR_INVALID_ARGUMENT;
    }

    const auto* header = reinterpret_cast<const open_shacc::packer::GxpHeader*>(data);
    std::ostringstream ss;
    ss << "; GXP Header Analysis\n";
    ss << "; Magic: 0x" << std::hex << header->magic << " ('GXP\\0')\n";
    ss << "; Version: " << std::dec << header->format_version << "\n";
    ss << "; Type: " << (header->shader_type == 0 ? "Vertex" : "Fragment") << "\n";
    ss << "; Total Container Size: " << header->total_size << " bytes\n";
    ss << "; Microcode Offset: " << header->microcode_offset << " (size: " << header->microcode_size << " bytes)\n";
    ss << "; Uniform Symbols: " << header->symbol_count << "\n";
    ss << "; Vertex Attributes: " << header->attribute_count << "\n";

    *out_disasm = duplicate_string(ss.str().c_str());
    return OPEN_SHACC_SUCCESS;
}

} // extern "C"
