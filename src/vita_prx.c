/**
 * @file vita_prx.c
 * @brief PlayStation Vita PRX module entry points and runtime symbols.
 *
 * Compiled as pure C to avoid C++ linkage conflicts with libstdc++.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#if defined(__vita__) || defined(__psp2__)

#include <stddef.h>
#include <stdint.h>

/* Reserve a compact 4 MiB heap for PRX shader compilation instead of newlib's 128 MiB default */
unsigned int _newlib_heap_size_user = 4 * 1024 * 1024;

/* VitaSDK newlib internal init/cleanup functions */
extern void _init_vita_heap(void);
extern void _free_vita_heap(void);
extern void _init_vita_malloc(void);
extern void _free_vita_malloc(void);
extern void _init_vita_reent(void);
extern void _free_vita_reent(void);
__attribute__((weak)) extern void *sceKernelGetTLSAddr(int key);

/* Linker-provided C++ constructor/destructor arrays */
typedef void (*init_fn_t)(void);
__attribute__((weak)) extern init_fn_t __preinit_array_start[];
__attribute__((weak)) extern init_fn_t __preinit_array_end[];
__attribute__((weak)) extern init_fn_t __init_array_start[];
__attribute__((weak)) extern init_fn_t __init_array_end[];
__attribute__((weak)) extern init_fn_t __fini_array_start[];
__attribute__((weak)) extern init_fn_t __fini_array_end[];

/* Provided for C++ runtime destructors when linking with -nostartfiles */
__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;

static int g_prx_initialized = 0;

static void run_init_arrays(void) {
    if (__preinit_array_start && __preinit_array_end) {
        size_t count = (size_t)(__preinit_array_end - __preinit_array_start);
        for (size_t i = 0; i < count; ++i) {
            if (__preinit_array_start[i]) {
                __preinit_array_start[i]();
            }
        }
    }
    if (__init_array_start && __init_array_end) {
        size_t count = (size_t)(__init_array_end - __init_array_start);
        for (size_t i = 0; i < count; ++i) {
            if (__init_array_start[i]) {
                __init_array_start[i]();
            }
        }
    }
}

static void run_fini_arrays(void) {
    if (__fini_array_start && __fini_array_end) {
        size_t count = (size_t)(__fini_array_end - __fini_array_start);
        for (size_t i = count; i > 0; --i) {
            if (__fini_array_start[i - 1]) {
                __fini_array_start[i - 1]();
            }
        }
    }
}

/* Referenced by newlib libc exit routines in PRX mode */
void _free_vita_newlib(void) {
    if (!g_prx_initialized) return;
    _free_vita_malloc();
    _free_vita_reent();
    _free_vita_heap();
    g_prx_initialized = 0;
}

/* Module lifecycle hooks defined in module.yml */
__attribute__((visibility("default"))) int module_start(size_t argc, const void* args) {
    (void)argc;
    (void)args;

    if (!g_prx_initialized) {
        void **tls_slot = (&sceKernelGetTLSAddr != NULL) ? (void **)sceKernelGetTLSAddr(0x89) : NULL;
        void *saved_reent = tls_slot ? *tls_slot : NULL;

        _init_vita_heap();
        _init_vita_reent();
        _init_vita_malloc();

        /* Do not clobber the host application's TLS _reent pointer if already set */
        if (tls_slot && saved_reent) {
            *tls_slot = saved_reent;
        }

        run_init_arrays();
        g_prx_initialized = 1;
    }

    return 0; /* SCE_KERNEL_START_SUCCESS */
}

__attribute__((visibility("default"))) int module_stop(size_t argc, const void* args) {
    (void)argc;
    (void)args;

    if (g_prx_initialized) {
        run_fini_arrays();
        _free_vita_newlib();
    }

    return 0; /* SCE_KERNEL_STOP_SUCCESS */
}

/* Default entry symbol for ELF linker */
int _start(size_t argc, const void* args) {
    (void)argc;
    (void)args;
    return 0;
}

#endif /* __vita__ || __psp2__ */
