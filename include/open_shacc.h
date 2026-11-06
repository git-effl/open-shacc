/**
 * @file open_shacc.h
 * @brief Open-source clean-room Cg/HLSL shader compiler targeting USSE / GXP.
 *
 * Designed as a clean-room specification-based compiler library for PS Vita and host platforms.
 * Built entirely from open industry specifications:
 *   - NVIDIA Cg Language Specification
 *   - Standard HLSL Shader Model specifications
 *   - Public Imagination Technologies USSE architecture documentation
 *   - PS Vita SceShaccCg ABI and SceGxm binary container specs
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_H
#define OPEN_SHACC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* API Visibility Macros                                                      */
/* -------------------------------------------------------------------------- */

#if defined(_WIN32) || defined(__CYGWIN__)
    #if defined(OPEN_SHACC_BUILD_SHARED)
        #define OPEN_SHACC_API __declspec(dllexport)
    #elif defined(OPEN_SHACC_USE_SHARED)
        #define OPEN_SHACC_API __declspec(dllimport)
    #else
        #define OPEN_SHACC_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define OPEN_SHACC_API __attribute__((visibility("default")))
    #else
        #define OPEN_SHACC_API
    #endif
#endif

/* -------------------------------------------------------------------------- */
/* Status and Error Codes                                                     */
/* -------------------------------------------------------------------------- */

typedef enum OpenShaccErrorCode {
    OPEN_SHACC_SUCCESS                   =  0,
    OPEN_SHACC_ERROR_INVALID_ARGUMENT    = -1,
    OPEN_SHACC_ERROR_PARSE_FAILED        = -2,
    OPEN_SHACC_ERROR_TYPECHECK_FAILED    = -3,
    OPEN_SHACC_ERROR_CODEGEN_FAILED      = -4,
    OPEN_SHACC_ERROR_PACKER_FAILED       = -5,
    OPEN_SHACC_ERROR_OUT_OF_REGISTERS    = -6,
    OPEN_SHACC_ERROR_OUT_OF_MEMORY       = -7,
    OPEN_SHACC_ERROR_INTERNAL            = -99
} OpenShaccErrorCode;

/* -------------------------------------------------------------------------- */
/* VitaSDK SceShaccCg Types & Enums                                           */
/* -------------------------------------------------------------------------- */

typedef enum SceShaccCgDiagnosticLevel {
    SCE_SHACCCG_DIAGNOSTIC_LEVEL_INFO    = 0,
    SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING = 1,
    SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR   = 2
} SceShaccCgDiagnosticLevel;

typedef enum SceShaccCgTargetProfile {
    SCE_SHACCCG_PROFILE_VP               = 0,
    SCE_SHACCCG_PROFILE_FP               = 1
} SceShaccCgTargetProfile;

typedef enum SceShaccCgCallbackDefaults {
    SCE_SHACCCG_SYSTEM_FILES             = 0,
    SCE_SHACCCG_TRIVIAL                  = 1
} SceShaccCgCallbackDefaults;

typedef enum SceShaccCgLocale {
    SCE_SHACCCG_ENGLISH                  = 0,
    SCE_SHACCCG_JAPANESE                 = 1
} SceShaccCgLocale;

typedef enum SceShaccCgParameterClass {
    SCE_SHACCCG_PARAMETERCLASS_INVALID      = 0x00,
    SCE_SHACCCG_PARAMETERCLASS_SCALAR       = 0x01,
    SCE_SHACCCG_PARAMETERCLASS_VECTOR       = 0x02,
    SCE_SHACCCG_PARAMETERCLASS_MATRIX       = 0x03,
    SCE_SHACCCG_PARAMETERCLASS_STRUCT       = 0x04,
    SCE_SHACCCG_PARAMETERCLASS_ARRAY        = 0x05,
    SCE_SHACCCG_PARAMETERCLASS_SAMPLER      = 0x06,
    SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK = 0x07
} SceShaccCgParameterClass;

typedef enum SceShaccCgParameterBaseType {
    SCE_SHACCCG_BASETYPE_INVALID        = 0x00,
    SCE_SHACCCG_BASETYPE_FLOAT          = 0x01,
    SCE_SHACCCG_BASETYPE_HALF           = 0x02,
    SCE_SHACCCG_BASETYPE_FIXED          = 0x03,
    SCE_SHACCCG_BASETYPE_BOOL           = 0x04,
    SCE_SHACCCG_BASETYPE_CHAR           = 0x05,
    SCE_SHACCCG_BASETYPE_UCHAR          = 0x06,
    SCE_SHACCCG_BASETYPE_SHORT          = 0x07,
    SCE_SHACCCG_BASETYPE_USHORT         = 0x08,
    SCE_SHACCCG_BASETYPE_INT            = 0x09,
    SCE_SHACCCG_BASETYPE_UINT           = 0x0A,
    SCE_SHACCCG_BASETYPE_SAMPLER1D      = 0x0B,
    SCE_SHACCCG_BASETYPE_ISAMPLER1D     = 0x0C,
    SCE_SHACCCG_BASETYPE_USAMPLER1D     = 0x0D,
    SCE_SHACCCG_BASETYPE_SAMPLER2D      = 0x0E,
    SCE_SHACCCG_BASETYPE_ISAMPLER2D     = 0x0F,
    SCE_SHACCCG_BASETYPE_USAMPLER2D     = 0x10,
    SCE_SHACCCG_BASETYPE_SAMPLERCUBE    = 0x11,
    SCE_SHACCCG_BASETYPE_ISAMPLERCUBE   = 0x12,
    SCE_SHACCCG_BASETYPE_USAMPLERCUBE   = 0x13,
    SCE_SHACCCG_BASETYPE_ARRAY          = 0x17,
    SCE_SHACCCG_BASETYPE_STRUCT         = 0x18,
    SCE_SHACCCG_BASETYPE_UNIFORMBLOCK   = 0x19
} SceShaccCgParameterBaseType;

typedef enum SceShaccCgParameterMemoryLayout {
    SCE_SHACCCG_MEMORYLAYOUT_INVALID    = 0,
    SCE_SHACCCG_MEMORYLAYOUT_COLUMN_MAJOR = 1,
    SCE_SHACCCG_MEMORYLAYOUT_ROW_MAJOR  = 2
} SceShaccCgParameterMemoryLayout;

typedef enum SceShaccCgParameterVariability {
    SCE_SHACCCG_VARIABILITY_INVALID     = 0,
    SCE_SHACCCG_VARIABILITY_VARYING     = 1,
    SCE_SHACCCG_VARIABILITY_UNIFORM     = 2
} SceShaccCgParameterVariability;

typedef enum SceShaccCgParameterDirection {
    SCE_SHACCCG_DIRECTION_INVALID       = 0,
    SCE_SHACCCG_DIRECTION_IN            = 1,
    SCE_SHACCCG_DIRECTION_OUT           = 2
} SceShaccCgParameterDirection;

typedef struct SceShaccCgSourceFile {
    const char *fileName;
    const char *text;
    uint32_t size;
} SceShaccCgSourceFile;

typedef struct SceShaccCgSourceLocation {
    const SceShaccCgSourceFile *file;
    uint32_t lineNumber;
    uint32_t columnNumber;
} SceShaccCgSourceLocation;

typedef struct SceShaccCgCompileOptions {
    const char *mainSourceFile;
    SceShaccCgTargetProfile targetProfile;
    const char *entryFunctionName;
    uint32_t searchPathCount;
    const char* const *searchPaths;
    uint32_t macroDefinitionCount;
    const char* const *macroDefinitions;
    uint32_t includeFileCount;
    const char* const *includeFiles;
    uint32_t suppressedWarningsCount;
    const uint32_t *suppressedWarnings;
    SceShaccCgLocale locale;
    int32_t useFx;
    int32_t noStdlib;
    int32_t optimizationLevel;
    int32_t useFastmath;
    int32_t useFastprecision;
    int32_t useFastint;
    int field_48;
    int32_t warningsAsErrors;
    int32_t performanceWarnings;
    int32_t warningLevel;
    int32_t pedantic;
    int32_t pedanticError;
    int field_60;
    int field_64;
} SceShaccCgCompileOptions;

typedef struct SceShaccCgCallbackList {
    SceShaccCgSourceFile* (*openFile)(
        const char *fileName,
        const SceShaccCgSourceLocation *includedFrom,
        const SceShaccCgCompileOptions *compileOptions,
        const char **errorString);
    void (*releaseFile)(
        const SceShaccCgSourceFile *file,
        const SceShaccCgCompileOptions *compileOptions);
    const char* (*locateFile)(
        const char *fileName,
        const SceShaccCgSourceLocation *includedFrom,
        uint32_t searchPathCount,
        const char *const *searchPaths,
        const SceShaccCgCompileOptions *compileOptions,
        const char **errorString);
    const char* (*absolutePath)(
        const char *fileName,
        const SceShaccCgSourceLocation *includedFrom,
        const SceShaccCgCompileOptions *compileOptions);
    void (*releaseFileName)(
        const char *fileName,
        const SceShaccCgCompileOptions *compileOptions);
    int32_t (*fileDate)(
        const SceShaccCgSourceFile *file,
        const SceShaccCgSourceLocation *includedFrom,
        const SceShaccCgCompileOptions *compileOptions,
        int64_t *timeLastStatusChange,
        int64_t *timeLastModified);
} SceShaccCgCallbackList;

typedef struct SceShaccCgDiagnosticMessage {
    SceShaccCgDiagnosticLevel level;
    uint32_t code;
    const SceShaccCgSourceLocation *location;
    const char *message;
} SceShaccCgDiagnosticMessage;

typedef struct SceShaccCgCompileOutput {
    const uint8_t *programData;
    uint32_t programSize;
    int32_t diagnosticCount;
    const SceShaccCgDiagnosticMessage *diagnostics;
} SceShaccCgCompileOutput;

typedef const void* SceShaccCgParameter;

/* -------------------------------------------------------------------------- */
/* Standard SceShaccCg Library Functions (PS Vita ABI / NID Export Table)      */
/* -------------------------------------------------------------------------- */

OPEN_SHACC_API int sceShaccCgInitializeCompileOptions(SceShaccCgCompileOptions *options);
OPEN_SHACC_API SceShaccCgCompileOutput const *sceShaccCgCompileProgram(
    const SceShaccCgCompileOptions *options,
    const SceShaccCgCallbackList *callbacks,
    int unk
);
OPEN_SHACC_API int sceShaccCgSetDefaultAllocator(void *(*malloc_cb)(unsigned int), void (*free_cb)(void *));
OPEN_SHACC_API void sceShaccCgInitializeCallbackList(SceShaccCgCallbackList *callbacks, SceShaccCgCallbackDefaults defaults);
OPEN_SHACC_API void sceShaccCgDestroyCompileOutput(SceShaccCgCompileOutput const *output);
OPEN_SHACC_API void sceShaccCgReleaseCompiler(void);
OPEN_SHACC_API const char *sceShaccCgGetVersionString(void);

/* Parameter Reflection APIs */
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstParameter(const SceShaccCgCompileOutput *output);
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetNextParameter(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetParameterByName(const SceShaccCgCompileOutput *output, const char *name);
OPEN_SHACC_API const char *sceShaccCgGetParameterName(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameterClass sceShaccCgGetParameterClass(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameterBaseType sceShaccCgGetParameterBaseType(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameterVariability sceShaccCgGetParameterVariability(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameterDirection sceShaccCgGetParameterDirection(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameterMemoryLayout sceShaccCgGetParameterMemoryLayout(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetParameterResourceIndex(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetParameterBufferIndex(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetParameterRows(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetParameterColumns(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetParameterVectorWidth(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetArraySize(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetArrayParameter(SceShaccCgParameter parameter, uint32_t index);
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetRowParameter(SceShaccCgParameter parameter, uint32_t row);
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstStructParameter(SceShaccCgParameter parameter);
OPEN_SHACC_API SceShaccCgParameter sceShaccCgGetFirstUniformBlockParameter(SceShaccCgParameter parameter);
OPEN_SHACC_API const char *sceShaccCgGetParameterSemantic(SceShaccCgParameter parameter);
OPEN_SHACC_API const char *sceShaccCgGetParameterUserType(SceShaccCgParameter parameter);
OPEN_SHACC_API int sceShaccCgIsParameterReferenced(SceShaccCgParameter parameter);
OPEN_SHACC_API int sceShaccCgIsParameterRegFormat(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecision(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatPrecisionCount(SceShaccCgParameter parameter);
OPEN_SHACC_API uint32_t sceShaccCgGetSamplerQueryFormatWidth(SceShaccCgParameter parameter);

/* -------------------------------------------------------------------------- */
/* Public Open-Shacc Wrapper C API Functions                                  */
/* -------------------------------------------------------------------------- */

enum OpenShaccCompileFlags {
    OPEN_SHACC_OPT_NONE                  = 0x0000,
    OPEN_SHACC_OPT_DEBUG_INFO            = 0x0001,
    OPEN_SHACC_OPT_OPTIMIZE_SPEED        = 0x0002,
    OPEN_SHACC_OPT_OPTIMIZE_SIZE         = 0x0004,
    OPEN_SHACC_OPT_STRICT_CLEANROOM      = 0x0008,
    OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY   = 0x0010
};

OPEN_SHACC_API int SceShaccCgCompileProgram(
    const char* source_code,
    uint32_t options,
    const void* compile_options,
    void** out_program_binary,
    size_t* out_size,
    char** out_diagnostic_log
);

OPEN_SHACC_API void SceShaccCgDestroyProgram(void* program_binary);
OPEN_SHACC_API void SceShaccCgDestroyDiagnosticLog(char* diagnostic_log);
OPEN_SHACC_API const char* SceShaccCgGetVersion(void);
OPEN_SHACC_API int SceShaccCgDisassembleGxp(
    const void* gxp_binary,
    size_t gxp_size,
    char** out_disasm
);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_SHACC_H */
