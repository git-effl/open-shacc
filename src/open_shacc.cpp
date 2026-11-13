/**
 * @file open_shacc.cpp
 * @brief Public C API implementation for open-shacc shader compiler.
 *
 * Clean-room replacement for libshacccg.
 * Exports all 33 SceShaccCg library functions (NID 0xA05BBEBB) + 5 SceShaccCg wrapper functions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#include <deque>
#include <memory>
#include <optional>
#include <algorithm>

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
    uint32_t array_size = 1;
    bool is_referenced = true;
    bool is_reg_format = true;

    std::vector<std::unique_ptr<InternalParameter>> struct_fields;
    std::vector<std::unique_ptr<InternalParameter>> array_elements;

    InternalParameter* next = nullptr;
};

// Internal Compile Output wrapper
struct InternalCompileOutput {
    SceShaccCgCompileOutput public_output{};
    std::vector<uint8_t> program_buffer;
    std::vector<SceShaccCgDiagnosticMessage> diagnostics_list;
    std::deque<std::string> message_strings;
    std::vector<std::unique_ptr<InternalParameter>> parameters;
    InternalParameter* first_param = nullptr;
};

static SceShaccCgParameterBaseType data_type_to_base_type(const open_shacc::frontend::DataType& dt) {
    using open_shacc::frontend::BaseType;
    if (dt.is_sampler()) {
        if (dt.base == BaseType::Sampler1D) return SCE_SHACCCG_BASETYPE_SAMPLER1D;
        if (dt.base == BaseType::SamplerCube) return SCE_SHACCCG_BASETYPE_SAMPLERCUBE;
        return SCE_SHACCCG_BASETYPE_SAMPLER2D;
    }
    switch (dt.base) {
        case BaseType::Float: case BaseType::Float2: case BaseType::Float3: case BaseType::Float4:
        case BaseType::Float2x2: case BaseType::Float3x3: case BaseType::Float4x4:
            return SCE_SHACCCG_BASETYPE_FLOAT;
        case BaseType::Half: case BaseType::Half2: case BaseType::Half3: case BaseType::Half4:
        case BaseType::Half2x2: case BaseType::Half3x3: case BaseType::Half4x4:
            return SCE_SHACCCG_BASETYPE_HALF;
        case BaseType::Fixed: case BaseType::Fixed2: case BaseType::Fixed3: case BaseType::Fixed4:
            return SCE_SHACCCG_BASETYPE_FIXED;
        case BaseType::Int: case BaseType::Int2: case BaseType::Int3: case BaseType::Int4:
            return SCE_SHACCCG_BASETYPE_INT;
        case BaseType::UInt: case BaseType::UInt2: case BaseType::UInt3: case BaseType::UInt4:
            return SCE_SHACCCG_BASETYPE_UINT;
        case BaseType::Short: case BaseType::Short2: case BaseType::Short3: case BaseType::Short4:
            return SCE_SHACCCG_BASETYPE_SHORT;
        case BaseType::UShort: case BaseType::UShort2: case BaseType::UShort3: case BaseType::UShort4:
            return SCE_SHACCCG_BASETYPE_USHORT;
        case BaseType::Char:
            return SCE_SHACCCG_BASETYPE_CHAR;
        case BaseType::UChar:
            return SCE_SHACCCG_BASETYPE_UCHAR;
        case BaseType::Bool: case BaseType::Bool2: case BaseType::Bool3: case BaseType::Bool4:
            return SCE_SHACCCG_BASETYPE_BOOL;
        default:
            return SCE_SHACCCG_BASETYPE_FLOAT;
    }
}

static SceShaccCgParameterClass data_type_to_class(const open_shacc::frontend::DataType& dt) {
    if (dt.is_sampler()) return SCE_SHACCCG_PARAMETERCLASS_SAMPLER;
    if (dt.is_matrix()) return SCE_SHACCCG_PARAMETERCLASS_MATRIX;
    if (dt.component_count() > 1) return SCE_SHACCCG_PARAMETERCLASS_VECTOR;
    return SCE_SHACCCG_PARAMETERCLASS_SCALAR;
}

static void fill_dimensions(InternalParameter* param, const open_shacc::frontend::DataType& dt) {
    if (dt.is_matrix()) {
        param->rows = static_cast<uint32_t>(dt.matrix_rows());
        param->columns = static_cast<uint32_t>(dt.matrix_cols());
        param->vector_width = param->columns;
    } else if (dt.is_sampler()) {
        param->rows = 1;
        param->columns = 1;
        param->vector_width = 1;
    } else {
        param->rows = 1;
        param->columns = static_cast<uint32_t>(std::max<size_t>(1, dt.component_count()));
        param->vector_width = param->columns;
    }
    param->array_size = (dt.array_size > 1) ? dt.array_size : 1u;
}

static void append_parameter(InternalCompileOutput* out, InternalParameter*& prev, std::unique_ptr<InternalParameter> param) {
    if (prev) {
        prev->next = param.get();
    } else {
        out->first_param = param.get();
    }
    prev = param.get();
    out->parameters.push_back(std::move(param));
}

static const SceShaccCgCompileOutput* make_error_output(
    std::unique_ptr<InternalCompileOutput> out,
    uint32_t code,
    const std::string& message
) {
    out->message_strings.push_back(message);
    SceShaccCgDiagnosticMessage msg{};
    msg.level = SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR;
    msg.code = code;
    msg.message = out->message_strings.back().c_str();
    out->diagnostics_list.push_back(msg);

    out->public_output.programData = nullptr;
    out->public_output.programSize = 0;
    out->public_output.diagnosticCount = static_cast<int32_t>(out->diagnostics_list.size());
    out->public_output.diagnostics = out->diagnostics_list.data();
    return &out.release()->public_output;
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

OPEN_SHACC_API void sceShaccCgInitializeCallbackList(
    SceShaccCgCallbackList *callbacks,
    [[maybe_unused]] SceShaccCgCallbackDefaults defaults
) {
    if (!callbacks) return;
    std::memset(callbacks, 0, sizeof(SceShaccCgCallbackList));
}

OPEN_SHACC_API void sceShaccCgReleaseCompiler(void) {
}

OPEN_SHACC_API const char *sceShaccCgGetVersionString(void) {
    return "open-shacc 1.0.0 (Clean-room C++20 USSE/GXP Shader Compiler)";
}

OPEN_SHACC_API int sceShaccCgExtEnableExtensions(void) {
    return 0; // SCE_OK
}

OPEN_SHACC_API SceShaccCgCompileOutput const *sceShaccCgCompileProgram(
    const SceShaccCgCompileOptions *options,
    const SceShaccCgCallbackList *callbacks,
    [[maybe_unused]] int unk
) {
    if (!options || !options->mainSourceFile) return nullptr;

    auto out = std::make_unique<InternalCompileOutput>();

    // Prepare source text via openFile callback if provided
    std::string source_text;
    std::string filename = "main.cg";

    if (callbacks && callbacks->openFile) {
        const char* err_str = nullptr;
        const SceShaccCgSourceFile* src_file = callbacks->openFile(options->mainSourceFile, nullptr, options, &err_str);
        if (src_file && src_file->text) {
            source_text.assign(src_file->text, src_file->size ? src_file->size : std::strlen(src_file->text));
            if (src_file->fileName) filename = src_file->fileName;
            if (callbacks->releaseFile) {
                callbacks->releaseFile(src_file, options);
            }
        } else {
            if (src_file && callbacks->releaseFile) {
                callbacks->releaseFile(src_file, options);
            }
            std::string err_msg = err_str ? err_str : (std::string("Failed to open source file: ") + options->mainSourceFile);
            return make_error_output(std::move(out), 1000, err_msg);
        }
    } else {
        source_text = options->mainSourceFile;
    }

    // 1. C Preprocessor
    open_shacc::frontend::Preprocessor prep;
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
        return make_error_output(std::move(out), 1001, "Parsing failed: " + parser.get_diagnostics_string());
    }

    // 3. Target Codegen & Register Allocation (respect targetProfile & entryFunctionName)
    open_shacc::backend::UsseCodeGenerator codegen;
    open_shacc::backend::CompiledProgram program;

    std::string entry_name = (options->entryFunctionName && options->entryFunctionName[0] != '\0')
                           ? options->entryFunctionName
                           : "main";
    std::optional<bool> force_fragment = (options->targetProfile == SCE_SHACCCG_PROFILE_FP);

    if (!codegen.generate(tu, entry_name, force_fragment, program)) {
        return make_error_output(std::move(out), 1002, "Codegen failed: " + codegen.get_error_message());
    }

    // 4. Pack into SceGxmProgram (GXP) Binary Container
    open_shacc::packer::GxpEncoder encoder;
    if (!encoder.encode(program, out->program_buffer)) {
        return make_error_output(std::move(out), 1003, "GXP packaging failed: " + encoder.get_error_message());
    }

    // 5. Populate Reflection Parameters Table
    InternalParameter* prev_param = nullptr;

    // Uniforms (Default uniform buffer container index = 14)
    for (const auto& u : program.uniforms) {
        auto param = std::make_unique<InternalParameter>();
        param->name = u.name;
        param->semantic = u.semantic;
        param->param_class = data_type_to_class(u.type);
        param->base_type = data_type_to_base_type(u.type);
        param->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
        param->direction = SCE_SHACCCG_DIRECTION_IN;
        param->resource_index = u.base_register;
        param->buffer_index = 14;
        fill_dimensions(param.get(), u.type);
        append_parameter(out.get(), prev_param, std::move(param));
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
        param->buffer_index = 0;
        fill_dimensions(param.get(), s.type);
        append_parameter(out.get(), prev_param, std::move(param));
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
        param->buffer_index = 0;
        fill_dimensions(param.get(), in.type);
        append_parameter(out.get(), prev_param, std::move(param));
    }

    // Outputs
    for (const auto& out_sym : program.outputs) {
        auto param = std::make_unique<InternalParameter>();
        param->name = out_sym.name;
        param->semantic = out_sym.semantic;
        param->param_class = data_type_to_class(out_sym.type);
        param->base_type = data_type_to_base_type(out_sym.type);
        param->variability = SCE_SHACCCG_VARIABILITY_VARYING;
        param->direction = SCE_SHACCCG_DIRECTION_OUT;
        param->resource_index = out_sym.base_register;
        param->buffer_index = 0;
        fill_dimensions(param.get(), out_sym.type);
        append_parameter(out.get(), prev_param, std::move(param));
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

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetRowParameter(
    [[maybe_unused]] SceShaccCgParameter parameter,
    [[maybe_unused]] uint32_t row
) {
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

OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstUniformBlockParameter([[maybe_unused]] SceShaccCgParameter parameter) {
    return nullptr;
}

OPEN_SHACC_API const char *sceShaccCgGetParameterSemantic(SceShaccCgParameter parameter) {
    if (!parameter) return "";
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->semantic.c_str();
}

OPEN_SHACC_API const char *sceShaccCgGetParameterUserType([[maybe_unused]] SceShaccCgParameter parameter) {
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

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecision([[maybe_unused]] SceShaccCgParameter parameter) {
    return 0;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecisionCount([[maybe_unused]] SceShaccCgParameter parameter) {
    return 0;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatWidth([[maybe_unused]] SceShaccCgParameter parameter) {
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public Open-Shacc Wrapper C API Functions                                  */
/* -------------------------------------------------------------------------- */

OPEN_SHACC_API int SceShaccCgCompileProgram(
    const char* source_code,
    uint32_t options,
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
    diag_stream << "open-shacc v1.0.0 (Clean-room C++20 USSE/GXP Shader Compiler)\n";

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
                << program.microcode_words.size() * sizeof(uint64_t) << " bytes microcode)\n";
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
    if (!gxp_binary || !out_disasm || gxp_size < sizeof(open_shacc::packer::SceGxmProgramHeader)) {
        return OPEN_SHACC_ERROR_INVALID_ARGUMENT;
    }

    std::string err;
    const auto* data = static_cast<const uint8_t*>(gxp_binary);
    if (!open_shacc::packer::GxpEncoder::validate(data, gxp_size, err)) {
        return OPEN_SHACC_ERROR_INVALID_ARGUMENT;
    }

    const auto* header = reinterpret_cast<const open_shacc::packer::SceGxmProgramHeader*>(data);
    const bool is_frag = (header->program_flags & open_shacc::packer::GXP_PROGRAM_FLAG_FRAGMENT) != 0;
    const size_t ucode_abs = offsetof(open_shacc::packer::SceGxmProgramHeader, primary_program_offset)
                           + static_cast<size_t>(header->primary_program_offset);

    std::ostringstream ss;
    ss << "; SceGxmProgram (GXP) Header Analysis\n";
    ss << "; Magic: 0x" << std::hex << header->magic << " ('GXP\\0')\n";
    ss << "; Version: " << std::dec << static_cast<int>(header->major_version)
       << "." << static_cast<int>(header->minor_version)
       << " (SDK 0x" << std::hex << header->sdk_version << std::dec << ")\n";
    ss << "; Type: " << (is_frag ? "Fragment" : "Vertex") << "\n";
    ss << "; Total Container Size: " << header->size << " bytes\n";
    ss << "; Primary Microcode Offset: " << ucode_abs
       << " (" << header->primary_program_instr_count << " instructions, "
       << (header->primary_program_instr_count * 8) << " bytes)\n";
    ss << "; Parameters: " << header->parameter_count << "\n";
    ss << "; Default Uniform Buffer Words: " << header->default_uniform_buffer_count << "\n";
    ss << "; Registers: PA=" << header->primary_reg_count
       << ", SA=" << header->secondary_reg_count
       << ", TEMP=" << header->temp_reg_count1 << "\n";

    *out_disasm = duplicate_string(ss.str().c_str());
    return OPEN_SHACC_SUCCESS;
}

} // extern "C"
