/**
 * @file open_shacc.h
 * @brief Open-source clean-room Cg/HLSL shader compiler targeting USSE / GXP.
 *
 * Designed as a clean-room specification-based compiler library.
 * Built entirely from open industry specifications:
 *   - NVIDIA Cg Language Specification
 *   - Standard HLSL Shader Model specifications
 *   - Public Imagination Technologies USSE architecture documentation
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
/* Compiler Options Bitmasks & Profiles                                       */
/* -------------------------------------------------------------------------- */

typedef enum OpenShaccTargetProfile {
    OPEN_SHACC_PROFILE_VP_AUTO           = 0x0000,
    OPEN_SHACC_PROFILE_VP_VERTEX         = 0x0001,
    OPEN_SHACC_PROFILE_FP_FRAGMENT       = 0x0002
} OpenShaccTargetProfile;

typedef enum OpenShaccCompileFlags {
    OPEN_SHACC_OPT_NONE                  = 0x0000,
    OPEN_SHACC_OPT_DEBUG_INFO            = 0x0001,
    OPEN_SHACC_OPT_OPTIMIZE_SPEED        = 0x0002,
    OPEN_SHACC_OPT_OPTIMIZE_SIZE         = 0x0004,
    OPEN_SHACC_OPT_STRICT_CLEANROOM      = 0x0008,
    OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY   = 0x0010
} OpenShaccCompileFlags;

typedef struct OpenShaccCompileOptions {
    uint32_t profile;               /* OpenShaccTargetProfile */
    uint32_t flags;                 /* OpenShaccCompileFlags */
    const char* entry_point;        /* Defaults to "main" if NULL */
    const char* include_path;       /* Optional search path for #include */
    uint32_t reserved[4];
} OpenShaccCompileOptions;

/* -------------------------------------------------------------------------- */
/* Public C API Functions                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Compile a Cg/HLSL shader source string into a GXP binary container.
 *
 * @param source_code         Null-terminated shader source string.
 * @param options             Bitmask flags from OpenShaccCompileFlags.
 * @param compile_options     Optional pointer to advanced options (can be NULL).
 * @param out_program_binary  Pointer receiving the newly allocated GXP binary buffer.
 * @param out_size            Pointer receiving the size in bytes of the GXP binary.
 * @param out_diagnostic_log  Pointer receiving human-readable compiler logs or error messages (can be NULL).
 *                            If non-NULL, must be freed via SceShaccCgDestroyDiagnosticLog().
 * @return OPEN_SHACC_SUCCESS on success, or a negative OpenShaccErrorCode on failure.
 */
OPEN_SHACC_API int SceShaccCgCompileProgram(
    const char* source_code,
    uint32_t options,
    const void* compile_options,
    void** out_program_binary,
    size_t* out_size,
    char** out_diagnostic_log
);

/**
 * @brief Free a program binary buffer previously allocated by SceShaccCgCompileProgram.
 *
 * @param program_binary Buffer pointer returned in out_program_binary.
 */
OPEN_SHACC_API void SceShaccCgDestroyProgram(void* program_binary);

/**
 * @brief Free a diagnostic log buffer previously allocated by SceShaccCgCompileProgram.
 *
 * @param diagnostic_log Buffer pointer returned in out_diagnostic_log.
 */
OPEN_SHACC_API void SceShaccCgDestroyDiagnosticLog(char* diagnostic_log);

/**
 * @brief Retrieve library version string and build details.
 */
OPEN_SHACC_API const char* SceShaccCgGetVersion(void);

/**
 * @brief Disassemble a compiled GXP binary into human-readable USSE assembly.
 *
 * @param gxp_binary Buffer containing valid GXP binary data.
 * @param gxp_size   Size in bytes of gxp_binary.
 * @param out_disasm Pointer receiving the allocated disassembly text string.
 * @return OPEN_SHACC_SUCCESS on success, or negative error code on failure.
 */
OPEN_SHACC_API int SceShaccCgDisassembleGxp(
    const void* gxp_binary,
    size_t gxp_size,
    char** out_disasm
);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_SHACC_H */
