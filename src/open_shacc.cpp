/**
 * @file open_shacc.cpp
 * @brief Public C API implementation for open-shacc shader compiler.
 *
 * Implemented strictly from public open-source specifications:
 *   - NVIDIA Cg Language Specification & Standard HLSL Shader Model syntax
 *   - Public Imagination Technologies USSE architecture and ISA specifications
 *   - Public SceGxmProgram (GXP) binary container layout and VitaSDK compilation ABIs
 */

#include "../include/open_shacc.h"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/preprocessor.hpp"
#include "backend/usse_codegen.hpp"
#include "packer/gxp_encoder.hpp"

#include <cstdio>
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
    std::string user_type;
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
    uint32_t sampler_precision = 0;
    uint32_t sampler_precision_count = 0;
    uint32_t sampler_width = 0;
    bool is_referenced = true;
    bool is_reg_format = true;

    std::vector<std::unique_ptr<InternalParameter>> struct_fields;
    std::vector<std::unique_ptr<InternalParameter>> uniform_block_members;
    std::vector<std::unique_ptr<InternalParameter>> array_elements;
    std::vector<std::unique_ptr<InternalParameter>> matrix_rows;

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
        case BaseType::CustomStruct:
            return SCE_SHACCCG_BASETYPE_STRUCT;
        case BaseType::UniformBlock:
            return SCE_SHACCCG_BASETYPE_UNIFORMBLOCK;
        default:
            return SCE_SHACCCG_BASETYPE_FLOAT;
    }
}

static SceShaccCgParameterClass data_type_to_class(const open_shacc::frontend::DataType& dt) {
    if (dt.is_sampler()) return SCE_SHACCCG_PARAMETERCLASS_SAMPLER;
    if (dt.base == open_shacc::frontend::BaseType::CustomStruct) return SCE_SHACCCG_PARAMETERCLASS_STRUCT;
    if (dt.base == open_shacc::frontend::BaseType::UniformBlock) return SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK;
    if (dt.is_matrix()) return SCE_SHACCCG_PARAMETERCLASS_MATRIX;
    if (dt.component_count() > 1) return SCE_SHACCCG_PARAMETERCLASS_VECTOR;
    return SCE_SHACCCG_PARAMETERCLASS_SCALAR;
}

static void fill_sampler_query_format(InternalParameter* param, SceShaccCgParameterBaseType base_type) {
    if (base_type == SCE_SHACCCG_BASETYPE_ISAMPLER1D ||
        base_type == SCE_SHACCCG_BASETYPE_ISAMPLER2D ||
        base_type == SCE_SHACCCG_BASETYPE_ISAMPLERCUBE) {
        param->sampler_precision = SCE_SHACCCG_BASETYPE_INT;
    } else if (base_type == SCE_SHACCCG_BASETYPE_USAMPLER1D ||
               base_type == SCE_SHACCCG_BASETYPE_USAMPLER2D ||
               base_type == SCE_SHACCCG_BASETYPE_USAMPLERCUBE) {
        param->sampler_precision = SCE_SHACCCG_BASETYPE_UINT;
    } else {
        param->sampler_precision = SCE_SHACCCG_BASETYPE_FLOAT;
    }
    param->sampler_precision_count = 1u;
    param->sampler_width = 4u;
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

namespace {

struct OwnedSourceFile {
    SceShaccCgSourceFile pub;
    std::string path;
    std::string contents;
};

static bool file_exists_readable(const char* path) {
    if (!path || path[0] == '\0') return false;
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    std::fclose(fp);
    return true;
}

static std::string join_dir_and_file(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    char back = dir.back();
    if (back == '/' || back == '\\' || back == ':') return dir + file;
    return dir + "/" + file;
}

static const char* default_locate_file(
    const char* fileName,
    const SceShaccCgSourceLocation* includedFrom,
    uint32_t searchPathCount,
    const char* const* searchPaths,
    [[maybe_unused]] const SceShaccCgCompileOptions* compileOptions,
    const char** errorString
) {
    if (!fileName || fileName[0] == '\0') {
        if (errorString) *errorString = "Empty filename";
        return nullptr;
    }

    auto alloc_copy = [](const std::string& s) -> const char* {
        char* buf = new char[s.size() + 1];
        std::memcpy(buf, s.c_str(), s.size() + 1);
        return buf;
    };

    if (includedFrom && includedFrom->file && includedFrom->file->fileName) {
        std::string from_path(includedFrom->file->fileName);
        size_t slash = from_path.find_last_of("/\\:");
        if (slash != std::string::npos) {
            std::string candidate = join_dir_and_file(from_path.substr(0, slash + 1), fileName);
            if (file_exists_readable(candidate.c_str())) {
                return alloc_copy(candidate);
            }
        }
    }

    if (file_exists_readable(fileName)) {
        return alloc_copy(fileName);
    }

    if (searchPaths) {
        for (uint32_t i = 0; i < searchPathCount; ++i) {
            if (!searchPaths[i]) continue;
            std::string candidate = join_dir_and_file(searchPaths[i], fileName);
            if (file_exists_readable(candidate.c_str())) {
                return alloc_copy(candidate);
            }
        }
    }

    if (errorString) *errorString = "File not found";
    return nullptr;
}

static const char* default_absolute_path(
    const char* fileName,
    [[maybe_unused]] const SceShaccCgSourceLocation* includedFrom,
    [[maybe_unused]] const SceShaccCgCompileOptions* compileOptions
) {
    if (!fileName) return nullptr;
    size_t len = std::strlen(fileName);
    char* buf = new char[len + 1];
    std::memcpy(buf, fileName, len + 1);
    return buf;
}

static void default_release_file_name(
    const char* fileName,
    [[maybe_unused]] const SceShaccCgCompileOptions* compileOptions
) {
    delete[] fileName;
}

static SceShaccCgSourceFile* default_open_file(
    const char* fileName,
    const SceShaccCgSourceLocation* includedFrom,
    const SceShaccCgCompileOptions* compileOptions,
    const char** errorString
) {
    if (!fileName) {
        if (errorString) *errorString = "Null filename";
        return nullptr;
    }

    std::string resolved = fileName;
    if (!file_exists_readable(resolved.c_str()) && compileOptions) {
        const char* located = default_locate_file(
            fileName,
            includedFrom,
            compileOptions->searchPathCount,
            compileOptions->searchPaths,
            compileOptions,
            errorString
        );
        if (located) {
            resolved = located;
            delete[] located;
        }
    }

    std::FILE* fp = std::fopen(resolved.c_str(), "rb");
    if (!fp) {
        if (errorString) *errorString = "Failed to open file";
        return nullptr;
    }

    auto* owned = new OwnedSourceFile();
    owned->path = resolved;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz > 0) {
        owned->contents.resize(static_cast<size_t>(sz));
        size_t nread = std::fread(owned->contents.data(), 1, static_cast<size_t>(sz), fp);
        owned->contents.resize(nread);
    }
    std::fclose(fp);

    owned->pub.fileName = owned->path.c_str();
    owned->pub.text = owned->contents.c_str();
    owned->pub.size = static_cast<uint32_t>(owned->contents.size());
    return &owned->pub;
}

static void default_release_file(
    const SceShaccCgSourceFile* file,
    [[maybe_unused]] const SceShaccCgCompileOptions* compileOptions
) {
    if (!file) return;
    const auto* owned = reinterpret_cast<const OwnedSourceFile*>(file);
    delete owned;
}

static int32_t default_file_date(
    [[maybe_unused]] const SceShaccCgSourceFile* file,
    [[maybe_unused]] const SceShaccCgSourceLocation* includedFrom,
    [[maybe_unused]] const SceShaccCgCompileOptions* compileOptions,
    int64_t* timeLastStatusChange,
    int64_t* timeLastModified
) {
    if (timeLastStatusChange) *timeLastStatusChange = 0;
    if (timeLastModified) *timeLastModified = 0;
    return 0;
}

} // namespace

OPEN_SHACC_API void sceShaccCgInitializeCallbackList(
    SceShaccCgCallbackList *callbacks,
    SceShaccCgCallbackDefaults defaults
) {
    if (!callbacks) return;
    std::memset(callbacks, 0, sizeof(SceShaccCgCallbackList));
    if (defaults == SCE_SHACCCG_SYSTEM_FILES) {
        callbacks->openFile = &default_open_file;
        callbacks->releaseFile = &default_release_file;
        callbacks->locateFile = &default_locate_file;
        callbacks->absolutePath = &default_absolute_path;
        callbacks->releaseFileName = &default_release_file_name;
        callbacks->fileDate = &default_file_date;
    }
}

OPEN_SHACC_API void sceShaccCgReleaseCompiler(void) {
}

OPEN_SHACC_API const char *sceShaccCgGetVersionString(void) {
    return "open-shacc 1.0.0 (Clean-room C++20 USSE/GXP Shader Compiler)";
}

OPEN_SHACC_API int sceShaccCgExtEnableExtensions(void) {
    return 0;
}

OPEN_SHACC_API SceShaccCgCompileOutput const *sceShaccCgCompileProgram(
    const SceShaccCgCompileOptions *options,
    const SceShaccCgCallbackList *callbacks,
    [[maybe_unused]] int unk
) {
    if (!options || !options->mainSourceFile) return nullptr;

    auto out = std::make_unique<InternalCompileOutput>();

    std::string source_text;
    std::string filename = "main.cg";

    if (callbacks && callbacks->openFile) {
        const char* err_str = nullptr;
        const SceShaccCgSourceFile* src_file = callbacks->openFile(options->mainSourceFile, nullptr, options, &err_str);
        if (src_file && src_file->text) {
            source_text.assign(src_file->text, src_file->size ? src_file->size : std::strlen(src_file->text));
            while (!source_text.empty() && source_text.back() == '\0') {
                source_text.pop_back();
            }
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

    open_shacc::frontend::Preprocessor prep;
    std::string processed_source;
    std::string prep_diag;
    if (!prep.preprocess(source_text, filename, options, callbacks, processed_source, prep_diag)) {
        return make_error_output(std::move(out), 1004, prep_diag.empty() ? "Preprocessing failed" : prep_diag);
    }

    open_shacc::frontend::Lexer lexer(processed_source);
    auto tokens = lexer.tokenize();

    open_shacc::frontend::Parser parser(std::move(tokens));
    open_shacc::frontend::TranslationUnit tu;

    if (!parser.parse(tu)) {
        return make_error_output(std::move(out), 1001, "Parsing failed: " + parser.get_diagnostics_string());
    }

    open_shacc::backend::UsseCodeGenerator codegen;
    open_shacc::backend::CompiledProgram program;

    std::string entry_name = (options->entryFunctionName && options->entryFunctionName[0] != '\0')
                           ? options->entryFunctionName
                           : "main";
    std::optional<bool> force_fragment = (options->targetProfile == SCE_SHACCCG_PROFILE_FP);

    if (!codegen.generate(tu, entry_name, force_fragment, program)) {
        return make_error_output(std::move(out), 1002, "Codegen failed: " + codegen.get_error_message());
    }

    open_shacc::packer::GxpEncoder encoder;
    if (!encoder.encode(program, out->program_buffer)) {
        return make_error_output(std::move(out), 1003, "GXP packaging failed: " + encoder.get_error_message());
    }

    InternalParameter* prev_param = nullptr;

    auto make_matrix_row_params = [](InternalParameter* mat_param, const open_shacc::frontend::DataType& dt, uint32_t base_reg, uint32_t row_stride) {
        size_t rows = dt.matrix_rows();
        size_t cols = dt.matrix_cols();
        for (size_t r = 0; r < rows; ++r) {
            auto rparam = std::make_unique<InternalParameter>();
            rparam->name = mat_param->name + "[" + std::to_string(r) + "]";
            rparam->semantic = mat_param->semantic;
            rparam->param_class = SCE_SHACCCG_PARAMETERCLASS_VECTOR;
            rparam->base_type = mat_param->base_type;
            rparam->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
            rparam->direction = SCE_SHACCCG_DIRECTION_IN;
            rparam->layout = mat_param->layout;
            rparam->resource_index = static_cast<uint32_t>(base_reg + r * row_stride);
            rparam->buffer_index = 14;
            rparam->rows = 1;
            rparam->columns = static_cast<uint32_t>(cols);
            rparam->vector_width = static_cast<uint32_t>(cols);
            rparam->array_size = 1;
            mat_param->matrix_rows.push_back(std::move(rparam));
        }
    };

    auto make_uniform_param = [&](const open_shacc::backend::SymbolBinding& u, const std::string& display_name) {
        auto param = std::make_unique<InternalParameter>();
        param->name = display_name;
        param->semantic = u.semantic;
        param->user_type = u.type.struct_name;
        param->param_class = data_type_to_class(u.type);
        param->base_type = data_type_to_base_type(u.type);
        param->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
        param->direction = SCE_SHACCCG_DIRECTION_IN;
        param->resource_index = u.base_register;
        param->buffer_index = 14;
        fill_dimensions(param.get(), u.type);

        const uint32_t arr_count = (u.type.array_size > 1) ? u.type.array_size : 1u;
        const uint32_t elem_stride = (arr_count > 1)
            ? static_cast<uint32_t>(std::max<uint16_t>(1, u.allocated_words / arr_count))
            : static_cast<uint32_t>(u.allocated_words);
        uint32_t mat_row_stride = 4u;
        if (u.type.is_matrix() && u.type.matrix_rows() > 0) {
            mat_row_stride = std::max<uint32_t>(1u, elem_stride / static_cast<uint32_t>(u.type.matrix_rows()));
            make_matrix_row_params(param.get(), u.type, u.base_register, mat_row_stride);
        }
        if (u.type.array_size > 1) {
            for (uint32_t k = 0; k < arr_count; ++k) {
                auto elem = std::make_unique<InternalParameter>();
                elem->name = display_name + "[" + std::to_string(k) + "]";
                elem->semantic = u.semantic;
                elem->user_type = u.type.struct_name;
                elem->param_class = param->param_class;
                elem->base_type = param->base_type;
                elem->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
                elem->direction = SCE_SHACCCG_DIRECTION_IN;
                elem->layout = param->layout;
                elem->resource_index = static_cast<uint32_t>(u.base_register + k * elem_stride);
                elem->buffer_index = 14;
                elem->rows = param->rows;
                elem->columns = param->columns;
                elem->vector_width = param->vector_width;
                elem->array_size = 1;
                if (u.type.is_matrix()) {
                    make_matrix_row_params(elem.get(), u.type, elem->resource_index, mat_row_stride);
                }
                param->array_elements.push_back(std::move(elem));
            }
        }
        return param;
    };

    auto find_uniform_binding = [&](const std::string& sym_name) -> const open_shacc::backend::SymbolBinding* {
        for (const auto& u : program.uniforms) {
            if (u.name == sym_name) return &u;
        }
        return nullptr;
    };

    std::vector<std::string> grouped_uniform_names;
    auto mark_grouped = [&](const std::string& n) { grouped_uniform_names.push_back(n); };
    auto is_grouped = [&](const std::string& n) {
        return std::find(grouped_uniform_names.begin(), grouped_uniform_names.end(), n) != grouped_uniform_names.end();
    };

    auto emit_struct_or_block_param = [&](const std::string& name, const open_shacc::frontend::DataType& dt, const std::string& sem) {
        const auto* sdecl = tu.find_struct(dt.struct_name);
        if (!sdecl) return;
        const bool is_block = (dt.base == open_shacc::frontend::BaseType::UniformBlock);
        auto container = std::make_unique<InternalParameter>();
        container->name = name;
        container->semantic = sem;
        container->user_type = dt.struct_name;
        container->param_class = is_block ? SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK : SCE_SHACCCG_PARAMETERCLASS_STRUCT;
        container->base_type = is_block ? SCE_SHACCCG_BASETYPE_UNIFORMBLOCK : SCE_SHACCCG_BASETYPE_STRUCT;
        container->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
        container->direction = SCE_SHACCCG_DIRECTION_IN;
        container->buffer_index = 14;
        container->rows = 1;
        container->columns = 1;
        container->vector_width = 1;
        container->array_size = 1;

        auto& child_vec = is_block ? container->uniform_block_members : container->struct_fields;
        bool has_res = false;
        for (const auto& field : sdecl->fields) {
            std::string lookup = is_block ? field.name : (name + "." + field.name);
            const auto* ub = find_uniform_binding(lookup);
            if (!ub) continue;
            if (!has_res) {
                container->resource_index = ub->base_register;
                has_res = true;
            }
            mark_grouped(lookup);
            auto member = make_uniform_param(*ub, field.name);
            if (!child_vec.empty()) {
                child_vec.back()->next = member.get();
            }
            child_vec.push_back(std::move(member));
        }
        if (!child_vec.empty()) {
            append_parameter(out.get(), prev_param, std::move(container));
        }
    };

    for (const auto& tu_u : tu.uniforms) {
        if ((tu_u.type.base == open_shacc::frontend::BaseType::CustomStruct && tu_u.type.array_size <= 1) ||
            tu_u.type.base == open_shacc::frontend::BaseType::UniformBlock) {
            emit_struct_or_block_param(tu_u.name, tu_u.type, tu_u.semantic);
        }
    }
    if (const auto* entry_decl = tu.find_entry_point(entry_name)) {
        for (const auto& ep : entry_decl->parameters) {
            if ((ep.is_uniform || ep.type.is_sampler()) &&
                ep.type.base == open_shacc::frontend::BaseType::CustomStruct) {
                emit_struct_or_block_param(ep.name, ep.type, ep.semantic);
            }
        }
    }

    for (const auto& u : program.uniforms) {
        if (is_grouped(u.name)) continue;
        append_parameter(out.get(), prev_param, make_uniform_param(u, u.name));
    }

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
        fill_sampler_query_format(param.get(), param->base_type);
        if (s.type.array_size > 1) {
            for (uint32_t k = 0; k < s.type.array_size; ++k) {
                auto elem = std::make_unique<InternalParameter>();
                elem->name = s.name + "[" + std::to_string(k) + "]";
                elem->param_class = SCE_SHACCCG_PARAMETERCLASS_SAMPLER;
                elem->base_type = param->base_type;
                elem->variability = SCE_SHACCCG_VARIABILITY_UNIFORM;
                elem->direction = SCE_SHACCCG_DIRECTION_IN;
                elem->resource_index = static_cast<uint32_t>(s.base_register + k);
                elem->buffer_index = 0;
                elem->rows = 1;
                elem->columns = 1;
                elem->vector_width = 1;
                elem->array_size = 1;
                fill_sampler_query_format(elem.get(), elem->base_type);
                param->array_elements.push_back(std::move(elem));
            }
        }
        append_parameter(out.get(), prev_param, std::move(param));
    }

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
    const std::string target(name);
    for (const auto& p : internal_out->parameters) {
        if (p->name == target) {
            return p.get();
        }
    }
    for (const auto& p : internal_out->parameters) {
        for (const auto& f : p->struct_fields) {
            if (f->name == target || (p->name + "." + f->name) == target) {
                return f.get();
            }
        }
        for (const auto& m : p->uniform_block_members) {
            if (m->name == target || (p->name + "." + m->name) == target) {
                return m.get();
            }
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
    if (!parameter) return nullptr;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    if (row < param->matrix_rows.size()) {
        return param->matrix_rows[row].get();
    }
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
    if (!parameter) return nullptr;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    if (!param->uniform_block_members.empty()) {
        return param->uniform_block_members.front().get();
    }
    return nullptr;
}

OPEN_SHACC_API const char *sceShaccCgGetParameterSemantic(SceShaccCgParameter parameter) {
    if (!parameter) return "";
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->semantic.c_str();
}

OPEN_SHACC_API const char *sceShaccCgGetParameterUserType(SceShaccCgParameter parameter) {
    if (!parameter) return "";
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->user_type.c_str();
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
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->sampler_precision;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecisionCount(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->sampler_precision_count;
}

OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatWidth(SceShaccCgParameter parameter) {
    if (!parameter) return 0;
    const auto* param = static_cast<const InternalParameter*>(parameter);
    return param->sampler_width;
}

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

    open_shacc::frontend::Preprocessor preprocessor;
    std::string processed_source;
    std::string prep_diag;

    if (!preprocessor.preprocess(source_code, "shader.cg", nullptr, nullptr, processed_source, prep_diag)) {
        diag_stream << "[ERROR] Preprocessing failed:\n" << prep_diag;
        if (out_diagnostic_log) {
            *out_diagnostic_log = duplicate_string(diag_stream.str().c_str());
        }
        return OPEN_SHACC_ERROR_PARSE_FAILED;
    }

    open_shacc::frontend::Lexer lexer(processed_source);
    auto tokens = lexer.tokenize();

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
