/**
 * @file vita_prx.c
 * @brief PlayStation Vita PRX module entry points and runtime symbols.
 *
 * Compiled as pure C to avoid C++ linkage conflicts with libstdc++.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(__vita__) || defined(__psp2__)

#include <stddef.h>
#include <stdint.h>

/* Forward declarations of SceShaccCg C functions */
extern int sceShaccCgInitializeCompileOptions(void *options);
extern void *sceShaccCgCompileProgram(const void *options, const void *callbacks, int unk);
extern int sceShaccCgSetDefaultAllocator(void *(*malloc_cb)(unsigned int), void (*free_cb)(void *));
extern void sceShaccCgInitializeCallbackList(void *callbacks, int defaults);
extern void sceShaccCgDestroyCompileOutput(const void *output);
extern void sceShaccCgReleaseCompiler(void);
extern const char *sceShaccCgGetVersionString(void);
extern int sceShaccCgExtEnableExtensions(void);

#if __has_include(<taihen.h>)
#include <taihen.h>

/* Weak symbol declaration so linking succeeds whether or not taihen_stub is linked */
__attribute__((weak)) int taiHookFunctionImportForUser(tai_hook_ref_t *ref, const char *module_name, uint32_t library_nid, uint32_t func_nid, const void *func);

static tai_hook_ref_t g_hook_init_opts;
static tai_hook_ref_t g_hook_compile;
static tai_hook_ref_t g_hook_allocator;
static tai_hook_ref_t g_hook_callbacks;
static tai_hook_ref_t g_hook_ext;

static void hook_shacc_imports(void) {
    if (&taiHookFunctionImportForUser != NULL) {
        taiHookFunctionImport(&g_hook_init_opts, TAI_MAIN_MODULE, 0xA05BBEBB, 0x69B14B33, (const void*)(uintptr_t)sceShaccCgInitializeCompileOptions);
        taiHookFunctionImport(&g_hook_compile, TAI_MAIN_MODULE, 0xA05BBEBB, 0x69CFFF7C, (const void*)(uintptr_t)sceShaccCgCompileProgram);
        taiHookFunctionImport(&g_hook_allocator, TAI_MAIN_MODULE, 0xA05BBEBB, 0x3266175D, (const void*)(uintptr_t)sceShaccCgSetDefaultAllocator);
        taiHookFunctionImport(&g_hook_callbacks, TAI_MAIN_MODULE, 0xA05BBEBB, 0x0EB75865, (const void*)(uintptr_t)sceShaccCgInitializeCallbackList);
        taiHookFunctionImport(&g_hook_ext, TAI_MAIN_MODULE, 0xA05BBEBB, 0xD748B49E, (const void*)(uintptr_t)sceShaccCgExtEnableExtensions);
    }
}
#endif

/* Provided for C++ runtime destructors when linking with -nostartfiles */
__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;

/* Referenced by newlib libc exit routines in PRX mode */
void _free_vita_newlib(void) {}

/* Module lifecycle hooks defined in module.yml */
__attribute__((visibility("default"))) int module_start(size_t argc, const void* args) {
    (void)argc;
    (void)args;
#if __has_include(<taihen.h>)
    hook_shacc_imports();
#endif
    return 0; /* SCE_KERNEL_START_SUCCESS */
}

__attribute__((visibility("default"))) int module_stop(size_t argc, const void* args) {
    (void)argc;
    (void)args;
    return 0; /* SCE_KERNEL_STOP_SUCCESS */
}

/* Default entry symbol for ELF linker */
int _start(size_t argc, const void* args) {
    (void)argc;
    (void)args;
    return 0;
}

#endif /* __vita__ || __psp2__ */
