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

/* Provided for C++ runtime destructors when linking with -nostartfiles */
__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;

/* Referenced by newlib libc exit routines in PRX mode */
void _free_vita_newlib(void) {}

/* Module lifecycle hooks defined in module.yml */
__attribute__((visibility("default"))) int module_start(size_t argc, const void* args) {
    (void)argc;
    (void)args;
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
