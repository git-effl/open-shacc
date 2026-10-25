/**
 * @file main.cpp
 * @brief Standalone command-line interface for open-shacc compiler.
 *
 * Usage:
 *   open-shacc-cli <input.cg> -o <output.gxp> [-v]
 *
 * SPDX-License-Identifier: MIT
 */

#include "open_shacc.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "open-shacc CLI Compiler v1.0.0\n";
        std::cout << "Clean-room Cg/HLSL to USSE / GXP Shader Compiler\n\n";
        std::cout << "Usage:\n";
        std::cout << "  open-shacc-cli <input.cg> [-o output.gxp] [-v] [--disasm]\n";
        return 0;
    }

    std::string input_path;
    std::string output_path = "out.gxp";
    bool verbose = false;
    bool show_disasm = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--disasm") {
            show_disasm = true;
        } else if (arg[0] != '-') {
            input_path = arg;
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: No input shader file specified.\n";
        return 1;
    }

    std::ifstream file(input_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file: " << input_path << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    uint32_t flags = OPEN_SHACC_OPT_NONE;
    if (verbose || show_disasm) {
        flags |= OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY;
    }

    void* gxp_data = nullptr;
    size_t gxp_size = 0;
    char* log = nullptr;

    std::cout << "Compiling " << input_path << "...\n";
    int status = SceShaccCgCompileProgram(
        source.c_str(),
        flags,
        nullptr,
        &gxp_data,
        &gxp_size,
        &log
    );

    if (log) {
        std::cout << log << "\n";
        SceShaccCgDestroyDiagnosticLog(log);
    }

    if (status != OPEN_SHACC_SUCCESS) {
        std::cerr << "Compilation failed with error code: " << status << "\n";
        return 1;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "Error: Could not open output file for writing: " << output_path << "\n";
        SceShaccCgDestroyProgram(gxp_data);
        return 1;
    }

    out.write(reinterpret_cast<const char*>(gxp_data), gxp_size);
    out.close();

    std::cout << "Successfully generated " << output_path << " (" << gxp_size << " bytes, 16-byte aligned)\n";
    SceShaccCgDestroyProgram(gxp_data);

    return 0;
}
