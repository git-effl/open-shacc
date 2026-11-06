/**
 * @file open_shacc.cpp
 * @brief Public C API implementation for open-shacc shader compiler.
 *
 * Clean-room replacement for libshacccg.
 * Exports all 33 SceShaccCg library functions (NID 0xA05BBEBB) + 5 SceShaccCg wrapper functions.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../include/open_shacc.h"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/preprocessor.hpp"
#include "backend/usse_codegen.hpp"
#include "packer/gxp_encoder.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <unordered_map>

// Global allocator callbacks
static void* (*g_malloc_cb)(unsigned int) = nullptr;
static void (*g_free_cb)(void*) = nullptr;

static void* shacc_malloc(size_t size) {
    if (g_malloc_cb) return g_malloc_cb(static_cast<unsigned int>(size));
    return std::malloc(size);
}

static void shacc_free(void* ptr) {
    if (!ptr) return;
    if (g_free_cb) g_free_cb(ptr);
    else std::free(ptr);
}

static char* duplicate_string(const char* str) {
    if (!str) return nullptr;
    size_t len = std::strlen(str);
    char* copy = static_cast<char*>(shacc_malloc(len + 1));
    if (copy) {
        std::memcpy(copy, str, len + 1);
    }
    return copy;
}

// Internal Parameter Reflection node
struct InternalParameter {
    std::string name;
    std::string semantic;
    SceShaccCgParameterClass param_class = SCE_SHACCCG_PARAMETERCLASS_SCALAR;
    SceShaccCgParameterBaseType base_type = SCE_SHACCCG_BASETYPE_FLOAT;
    SceShaccCgParameterVariability variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
    SceShaccCgParameterDirection direction = SCE_SHACCCG_DIRECTION_IN;
    SceShaccCgParameterMemoryLayout layout = SCE_SHACCCG_MEMORYLAYOUT_COLUMN_MAJOR;
    uint32_t resource_index = 0;
    uint32_t buffer_index = 0;
    uint32_t rows = 1;
    uint32_t columns = 1;
    uint32_t vector_width = 1;
    uint32_t array_size = 0;
    bool is_referenced = true;
    bool is_reg_format = true;

    std::vector<std::unique_ptr<InternalParameter>> struct_fields;
    std::vector<std::unique_ptr<InternalParameter>> array_elements;

    InternalParameter* next = nullptr;
};

// Internal Compile Output wrapper
struct InternalCompileOutput {
    SceShaccCgCompileOutput public_output;
    std::vector<uint8_t> program_buffer;
    std::vector<SceShaccCgDiagnosticMessage> diagnostics_list;
    std::vector<std::string> message_strings;
    std::vector<std::unique_ptr<InternalParameter>> parameters;
    InternalParameter* first_param = nullptr;
};

static SceShaccCgParameterBaseType data_type_to_base_type(const open_shacc::frontend::DataType& dt) {
    if (dt.is_sampler()) {
        if (dt.base == open_shacc::frontend::BaseType::Sampler2D) return SCE_SHACCCG_BASETYPE_SAMPLER2D;
        if (dt.base == open_shacc::frontend::BaseType::SamplerCube) return SCE_SHACCCG_BASETYPE_SAMPLERCUBE;
        return SCE_SHACCCG_BASETYPE_SAMPLER2D;
    }
    switch (dt.base) {
        case open_shacc::frontend::BaseType::Float:
        case open_shacc::frontend::BaseType::Float2:
        case open_shacc::frontend::BaseType::Float3:
        case open_shacc::frontend::BaseType::Float4:
        case open_shacc::frontend::BaseType::Float2x2:
        case open_shacc::frontend::BaseType::Float3x3:
        case open_shacc::frontend::BaseType::Float4x4:
            return SCE_SHACCCG_BASETYPE_FLOAT;
        case open_shacc::frontend::BaseType::Half:
        case open_shacc::frontend::BaseType::Half2:
        case open_shacc::frontend::BaseType::Half3:
        case open_shacc::frontend::BaseType::Half4:
            return SCE_SHACCCG_BASETYPE_HALF;
        case open_shacc::frontend::BaseType::Fixed:
        case open_shacc::frontend::BaseType::Fixed2:
        case open_shacc::frontend::BaseType::Fixed3:
        case open_shacc::frontend::BaseType::Fixed4:
            return SCE_SHACCCG_BASETYPE_FIXED;
        case open_shacc::frontend::BaseType::Int:
        case open_shacc::frontend::BaseType::Int2:
        case open_shacc::frontend::BaseType::Int3:
        case open_shacc::frontend::BaseType::Int4:
            return SCE_SHACCCG_BASETYPE_INT;
        case open_shacc::frontend::BaseType::Bool:
            return SCE_SHACCCG_BASETYPE_BOOL;
        default:
            return SCE_SHACCCG_BASETYPE_FLOAT;
    }
}

static SceShaccCgParameterClass data_type_to_class(const open_shacc::frontend::DataType& dt) {
    if (dt.is_sampler()) return SCE_SHACCCG_PARAMETERCLASS_SAMPLER;
    if (dt.base == open_shacc::frontend::BaseType::Float4x4 ||
        dt.base == open_shacc::frontend::BaseType::Float3x3 ||
        dt.base == open_shacc::frontend::BaseType::Float2x2) {
        return SCE_SHACCCG_PARAMETERCLASS_MATRIX;
    }
    if (dt.component_count() > 1) {
        return SCE_SHACCCG_PARAMETERCLASS_VECTOR;
    }
    return SCE_SHACCCG_PARAMETERCLASS_SCALAR;
}

extern "C" {

/* -------------------------------------------------------------------------- */
/* Standard SceShaccCg Library Functions                                       */
/* -------------------------------------------------------------------------- */

OPEN_SHACC_API int sceShaccCgInitializeCompileOptions(SceShaccCgCompileOptions *options) {
    if (!options) return OPEN_SHACC_ERROR_INVALID_ARGUMENT;
    std::memset(options, 0, sizeof(SceShaccCgCompileOptions));
    options->targetProfile = SCE_SHACCCG_PROFILE_VP;
    options->entryFunctionName = "main";
    options->locale = SCE_SHACCCG_ENGLISH;
    options->optimizationLevel = 3;
    return OPEN_SHACC_SUCCESS;
}

OPEN_SHACC_API int sceShaccCgSetDefaultAllocator(void *(*malloc_cb)(unsigned int), void (*free_cb)(void *)) {
    g_malloc_cb = malloc_cb;
    g_free_cb = free_cb;
    return OPEN_SHACC_SUCCESS;
}

OPEN_SHACC_API void sceShaccCgInitializeCallbackList(SceShaccCgCallbackList *callbacks, SceShaccCgCallbackDefaults defaults) {
    if (!callbacks) return;
    std::memset(callbacks, 0, sizeof(SceShaccCgCallbackList));
}

OPEN_SHACC_API void sceShaccCgReleaseCompiler(void) {
    // No-op for stateless compiler
}

OPEN_SHACC_API const char *sceShaccCgGetVersionString(void) {
    return "open-shacc 1.0.0 (Clean-room C++20 USSE/GXP Shader Compiler)";
}

OPEN_SHACC_API SceShaccCgCompileOutput const *sceShaccCgCompileProgram(
    const SceShaccCgCompileOptions *options,
    const SceShaccCgCallbackList *callbacks,
    [[maybe_unused]] int unk
) {
    if (!options || !options->mainSourceFile) return nullptr;

    auto out = std::make_unique<InternalCompileOutput>();

    // Prepare source text via callbacks if provided, or read from options
    std::string source_text = options->mainSourceFile;
    std::string filename = "main.cg";

    if (callbacks && callbacks->openFile) {
        const char* err_str = nullptr;
        const SceShaccCgSourceFile* src_file = callbacks->openFile(options->mainSourceFile, nullptr, options, &err_str);
        if (src_file && src_file->text) {
            source_text = std::string(src_file->text, src_file->size ? src_file->size : std::strlen(src_file->text));
            if (src_file->fileName) filename = src_file->fileName;
        }
    }

    // 1. C Preprocessor
    open_shacc::frontend::Preprocessor prep;
    // Add macro definitions from compile options
    if (options->macroDefinitions && options->macroDefinitionCount > 0) {
        for (uint32_t i = 0; i < options->macroDefinitionCount; ++i) {
            if (options->macroDefinitions[i]) {
                std::string def = options->macroDefinitions[i];
                size_t eq = def.find('=');
                if (eq != std::string::npos) {
                    prep.add_macro(def.substr(0, eq), def.substr(eq + 1));
                } else {
                    prep.add_macro(def, "1");
                }
            }
        }
    }

    std::string processed_source;
    std::string prep_diag;
    if (!prep.preprocess(source_text, filename, options, callbacks, processed_source, prep_diag)) {
        processed_source = source_text;
    }

    // 2. Lexical & Syntax Parsing
    open_shacc::frontend::Lexer lexer(processed_source);
    auto tokens = lexer.tokenize();

    open_shacc::frontend::Parser parser(std::move(tokens));
    open_shacc::frontend::TranslationUnit tu;

    if (!parser.parse(tu)) {
        out->message_strings.push_back("Parsing failed: " + parser.get_diagnostics_string());
        SceShaccCgDiagnosticMessage msg{};
        msg.level = SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR;
        msg.code = 1001;
        msg.message = out->message_strings.back().c_str();
        out->diagnostics_list.push_back(msg);

        out->public_output.programData = nullptr;
        out->public_output.programSize = 0;
        out->public_output.diagnosticCount = static_cast<int32_t>(out->diagnostics_list.size());
        out->public_output.diagnostics = out->diagnostics_list.data();
        return &out.release()->public_output;
    }

    // 3. Target Codegen & Register Allocation
    open_shacc::backend::UsseCodeGenerator codegen;
    open_shacc::backend::CompiledProgram program;

    if (!codegen.generate(tu, program)) {
        out->message_strings.push_back("Codegen failed: " + codegen.get_error_message());
        SceShaccCgDiagnosticMessage msg{};
        msg.level = SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR;
        msg.code = 1002;
        msg.message = out->message_strings.back().c_str();
        out->diagnostics_list.push_back(msg);

        out->public_output.programData = nullptr;
        out->public_output.programSize = 0;
        out->public_output.diagnosticCount = static_cast<int32_t>(out->diagnostics_list.size());
        out->public_output.diagnostics = out->diagnostics_list.data();
        return &out.release()->public_output;
    }

    // Set profile if explicitly specified
    if (options->targetProfile == SCE_SHACCCG_PROFILE_FP) {
        program.is_vertex_shader = false;
    }

    // 4. Pack into GXP Binary Container
    open_shacc::packer::GxpEncoder encoder;
    if (!encoder.encode(program, out->program_buffer)) {
        out->message_strings.push_back("GXP packaging failed: " + encoder.get_error_message());
        SceShaccCgDiagnosticMessage msg{};
        msg.level = SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR;
        msg.code = 1003;
        msg.message = out->message_strings.back().c_str();
        out->diagnostics_list.push_back(msg);

        out->public_output.programData = nullptr;
        out->public_output.programSize = 0;
        out->public_output.diagnosticCount = static_cast<int32_t>(out->diagnostics_list.size());
        out->public_output.diagnostics = out->diagnostics_list.data();
        return &out.release()->public_output;
    }

    // 5. Populate Reflection Parameters Table
    InternalParameter* prev_param = nullptr;

    // Uniforms
    for (const auto& u : program.uniforms) {
        auto param = std::make_unique<InternalParameter>();
        param->name = u.name;
        param->semantic = u.semantic;
        param->param_class = data_type_to_class(u.type);
        param->base_type = data_type_to_base_type(u.type);
        param->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
        param->direction = SCE_SHACCCG_DIRECTION_IN;
        param->resource_index = u.base_register;

        if (u.type.base == open_shacc::frontend::BaseType::Float4x4) {
            param->rows = 4;
            param->columns = 4;
            param->vector_width = 4;
        } else if (u.type.base == open_shacc::frontend::BaseType::Float3x3) {
            param->rows = 3;
            param->columns = 3;
            param->vector_width = 3;
        } else if (u.type.base == open_shacc::frontend::BaseType::Float2x2) {
            param->rows = 2;
            param->columns = 2;
            param->vector_width = 2;
        } else {
            param->rows = 1;
            param->columns = static_cast<uint32_t>(u.type.component_count());
            param->vector_width = param->columns;
        }

        if (prev_param) {
            prev_param->next = param.get();
        } else {
            out->first_param = param.get();
        }
        prev_param = param.get();
        out->parameters.push_back(std::move(param));
    }

    // Samplers
    for (const auto& s : program.samplers) {
        auto param = std::make_unique<InternalParameter>();
        param->name = s.name;
        param->param_class = SCE_SHACCCG_PARAMETERCLASS_SAMPLER;
        param->base_type = data_type_to_base_type(s.type);
        param->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
        param->direction = SCE_SHACCCG_DIRECTION_IN;
        param->resource_index = s.base_register;

        if (prev_param) {
            prev_param->next = param.get();
        } else {
            out->first_param = param.get();
        }
        prev_param = param.get();
        out->parameters.push_back(std::move(param));
    }

    // Inputs
    for (const auto& in : program.inputs) {
        auto param = std::make_unique<InternalParameter>();
        param->name = in.name;
        param->semantic = in.semantic;
        param->param_class = data_type_to_class(in.type);
        param->base_type = data_type_to_base_type(in.type);
        param->variability = SCE_SHACCCG_VARIABILITY_VARYING;
        param->direction = SCE_SHACCCG_DIRECTION_IN;
        param->resource_index = in.base_register;
        param->columns = static_cast<uint32_t>(in.type.component_count());
        param->vector_width = param->columns;

        if (prev_param) {
            prev_param->next = param.get();
        } else {
            out->first_param = param.get();
        }
        prev_param = param.get();
        out->parameters.push_back(std::move(param));
    }

    out->public_output.programData = out->program_buffer.data();
    out->public_output.programSize = static_cast<uint32_t>(out->program_buffer.size());
    out->public_output.diagnosticCount = 0;
    out->public_output.diagnostics = nullptr;

    return &out.release()->public_output;
}

OPEN_SHACC_API void sceShaccCgDestroyCompileOutput(SceShaccCgCompileOutput const *output) {
    if (!output) return;
    const auto* internal_out = reinterpret_cast<const InternalCompileOutput*>(output);
    delete internal_out;
}

/* -------------------------------------------------------------------------- */
/* Parameter Reflection APIs                                                  */
/* -------------------------------------------------------------------------- */

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstParameter(const SceShaccCgCompileOutput *output) {
    if (!output) return nullptr;
    const auto* internal_out = reinterpret_cast<const InternalCompileOutput*>(output);
    return internal_out->first_param;
}

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetNextParameter(SceShaccCgParameter parameter) {
    if (!parameter) return nullptr;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->next;
}

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetParameterByName(const SceShaccCgCompileOutput *output, const char *name) {
    if (!output || !name) return nullptr;
    const auto* internal_out = reinterpret_cast<const InternalCompileOutput*>(output);
    for (const auto& p : internal_out->parameters) {
        if (p->name == name) {
            return p.get();
        }
    }
    return nullptr;
}

OPEN_SHACC_API const char *sceShaccCgGetParameterName(SceShaccCgParameter parameter) {
    if (!parameter) return "";
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->name.c_str();
}

OPEN_SHACC_API SceShaccCgParameterClass sceShaccCgGetParameterClass(SceShaccCgParameter parameter) {
    if (!parameter) return SCE_SHACCCG_PARAMETERCLASS_INVALID;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->param_class;
}

OPEN_SHACC_API SceShaccCgParameterBaseType sceShaccCgGetParameterBaseType(SceShaccCgParameter parameter) {
    if (!parameter) return SCE_SHACCCG_BASETYPE_INVALID;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->base_type;
}

OPEN_SHACC_API SceShaccCgParameterVariability sceShaccCgGetParameterVariability(SceShaccCgParameter parameter) {
    if (!parameter) return SCE_SHACCCG_VARIABILITY_INVALID;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->variability;
}

OPEN_SHACC_API SceShaccCgParameterDirection sceShaccCgGetParameterDirection(SceShaccCgParameter parameter) {
    if (!parameter) return SCE_SHACCCG_DIRECTION_INVALID;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->direction;
}

OPEN_SHACC_API SceShaccCgParameterMemoryLayout sceShaccCgGetParameterMemoryLayout(SceShaccCgParameter parameter) {
    if (!parameter) return SCE_SHACCCG_MEMORYLAYOUT_INVALID;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->layout;
}

OPEN_SHACC_API uint32_t sceShaccCgGetParameterResourceIndex(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->resource_index;
}

OPEN_SHACC_API uint32_t sceShaccCgGetParameterBufferIndex(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->buffer_index;
}

OPEN_SHACC_API uint32_t sceShaccCgGetParameterRows(SceShaccCgParameter parameter) {
    if (!parameter) return 1;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->rows;
}

OPEN_SHACC_API uint32_t sceShaccCgGetParameterColumns(SceShaccCgParameter parameter) {
    if (!parameter) return 1;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->columns;
}

OPEN_SHACC_API uint32_t sceShaccCgGetParameterVectorWidth(SceShaccCgParameter parameter) {
    if (!parameter) return 1;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->vector_width;
}

OPEN_SHACC_API uint32_t sceShaccCgGetArraySize(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->array_size;
}

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetArrayParameter(SceShaccCgParameter parameter, uint32_t index) {
    if (!parameter) return nullptr;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    if (index < param->array_elements.size()) {
        return param->array_elements[index].get();
    }
    return nullptr;
}

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetRowParameter(SceShaccCgParameter parameter, uint32_t row) {
    return nullptr;
}

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstStructParameter(SceShaccCgParameter parameter) {
    if (!parameter) return nullptr;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    if (!param->struct_fields.empty()) {
        return param->struct_fields.front().get();
    }
    return nullptr;
}

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstUniformBlockParameter(SceShaccCgParameter parameter) {
    return nullptr;
}

OPEN_SHACC_API const char *sceShaccCgGetParameterSemantic(SceShaccCgParameter parameter) {
    if (!parameter) return "";
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->semantic.c_str();
}

OPEN_SHACC_API const char *sceShaccCgGetParameterUserType(SceShaccCgParameter parameter) {
    return "";
}

OPEN_SHACC_API int sceShaccCgIsParameterReferenced(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->is_referenced ? 1 : 0;
}

OPEN_SHACC_API int sceShaccCgIsParameterRegFormat(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->is_reg_format ? 1 : 0;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecision(SceShaccCgParameter parameter) {
    return 0;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecisionCount(SceShaccCgParameter parameter) {
    return 0;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatWidth(SceShaccCgParameter parameter) {
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public Open-Shacc Wrapper C API Functions                                  */
/* -------------------------------------------------------------------------- */

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

    // 1. C Preprocessor
    open_shacc::frontend::Preprocessor preprocessor;
    std::string processed_source;
    std::string prep_diag;

    if (!preprocessor.preprocess(source_code, "shader.cg", nullptr, nullptr, processed_source, prep_diag)) {
        processed_source = source_code;
    }

    // 2. Lexical Analysis
    open_shacc::frontend::Lexer lexer(processed_source);
    auto tokens = lexer.tokenize();

    // 3. Syntax & Semantic Parsing
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

    // 4. Target Code Generation & Register Allocation
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

    // 5. Container Packing (GXP serialization)
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
    void* buffer = shacc_malloc(gxp_bytes.size());
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
        shacc_free(program_binary);
    }
}

OPEN_SHACC_API void SceShaccCgDestroyDiagnosticLog(char* diagnostic_log) {
    if (diagnostic_log) {
        shacc_free(diagnostic_log);
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
