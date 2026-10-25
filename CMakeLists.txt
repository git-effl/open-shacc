cmake_minimum_required(VERSION 3.20)
project(open-shacc
    VERSION 1.0.0
    DESCRIPTION "Clean-room open-source Cg/HLSL to USSE shader compiler and GXP binary packer"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# PlayStation Vita (VitaSDK) Platform Detection
# -----------------------------------------------------------------------------
if(DEFINED VITA OR DEFINED VITASDK OR CMAKE_SYSTEM_NAME STREQUAL "Generic" OR CMAKE_SYSTEM_NAME STREQUAL "Vita")
    set(IS_VITA_BUILD TRUE)
    message(STATUS "Configuring open-shacc for PlayStation Vita (VitaSDK)")
    set(OPEN_SHACC_BUILD_SHARED OFF CACHE BOOL "Shared library disabled on Vita" FORCE)
    set(OPEN_SHACC_BUILD_CLI OFF CACHE BOOL "CLI disabled on Vita" FORCE)
    set(OPEN_SHACC_BUILD_TESTS OFF CACHE BOOL "Tests disabled on Vita" FORCE)
    add_compile_definitions(__psp2__ __vita__)
endif()

# -----------------------------------------------------------------------------
# Build Options
# -----------------------------------------------------------------------------
option(OPEN_SHACC_BUILD_SHARED "Build open-shacc as a shared dynamic library" ON)
option(OPEN_SHACC_BUILD_STATIC "Build open-shacc as a static library" ON)
option(OPEN_SHACC_BUILD_CLI "Build open-shacc command-line tool" ON)
option(OPEN_SHACC_BUILD_TESTS "Build open-shacc test suite" ON)

# -----------------------------------------------------------------------------
# Compiler Flags & Strict Clean-Room Warnings
# -----------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
endif()

# -----------------------------------------------------------------------------
# Core Library Sources
# -----------------------------------------------------------------------------
set(OPEN_SHACC_SOURCES
    src/open_shacc.cpp
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/backend/regalloc.cpp
    src/backend/usse_codegen.cpp
    src/packer/gxp_encoder.cpp
)

set(OPEN_SHACC_HEADERS
    include/open_shacc.h
    src/frontend/ast.hpp
    src/frontend/lexer.hpp
    src/frontend/parser.hpp
    src/backend/usse_isa.hpp
    src/backend/regalloc.hpp
    src/backend/usse_codegen.hpp
    src/packer/gxp_format.hpp
    src/packer/gxp_encoder.hpp
)

# -----------------------------------------------------------------------------
# Shared Dynamic Library Target
# -----------------------------------------------------------------------------
if(OPEN_SHACC_BUILD_SHARED)
    add_library(open-shacc SHARED ${OPEN_SHACC_SOURCES} ${OPEN_SHACC_HEADERS})
    target_include_directories(open-shacc
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(open-shacc
        PRIVATE
            OPEN_SHACC_BUILD_SHARED
    )
    set_target_properties(open-shacc PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        OUTPUT_NAME "open-shacc"
    )
endif()

# -----------------------------------------------------------------------------
# Static Library Target
# -----------------------------------------------------------------------------
if(OPEN_SHACC_BUILD_STATIC)
    add_library(open-shacc-static STATIC ${OPEN_SHACC_SOURCES} ${OPEN_SHACC_HEADERS})
    target_include_directories(open-shacc-static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(open-shacc-static PROPERTIES
        OUTPUT_NAME "open-shacc"
    )
endif()

# -----------------------------------------------------------------------------
# CLI Executable
# -----------------------------------------------------------------------------
if(OPEN_SHACC_BUILD_CLI)
    add_executable(open-shacc-cli src/cli/main.cpp)
    if(OPEN_SHACC_BUILD_SHARED)
        target_link_libraries(open-shacc-cli PRIVATE open-shacc)
    else()
        target_link_libraries(open-shacc-cli PRIVATE open-shacc-static)
    endif()
    target_include_directories(open-shacc-cli PRIVATE include)
endif()

# -----------------------------------------------------------------------------
# Automated Unit Tests
# -----------------------------------------------------------------------------
if(OPEN_SHACC_BUILD_TESTS)
    enable_testing()
    add_executable(open-shacc-tests tests/test_shaders.cpp)
    if(OPEN_SHACC_BUILD_SHARED)
        target_link_libraries(open-shacc-tests PRIVATE open-shacc)
    else()
        target_link_libraries(open-shacc-tests PRIVATE open-shacc-static)
    endif()
    target_include_directories(open-shacc-tests PRIVATE include)
    add_test(NAME ShaderCompilerTests COMMAND open-shacc-tests)
endif()

# -----------------------------------------------------------------------------
# PlayStation Vita SUPRX Plugin Target
# -----------------------------------------------------------------------------
if(IS_VITA_BUILD)
    add_executable(open-shacc-elf ${OPEN_SHACC_SOURCES} ${OPEN_SHACC_HEADERS})
    target_compile_definitions(open-shacc-elf PRIVATE OPEN_SHACC_BUILD_SUPRX)
    target_include_directories(open-shacc-elf
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    # Modules do not have a main() function; entry point is module_start.
    # -nostartfiles prevents linking crt0.o and avoids undefined reference to main.
    set_target_properties(open-shacc-elf PROPERTIES
        LINK_FLAGS "-nostartfiles -Wl,-q"
    )
    target_link_libraries(open-shacc-elf PRIVATE -lSceKernel_stub)

    if(COMMAND vita_create_self)
        vita_create_self(open-shacc.suprx open-shacc-elf CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/module.yml)
    else()
        add_custom_command(TARGET open-shacc-elf POST_BUILD
            COMMAND vita-elf-create open-shacc-elf open-shacc.velf -e ${CMAKE_CURRENT_SOURCE_DIR}/module.yml
            COMMAND vita-make-fself -s open-shacc.velf open-shacc.suprx
            COMMENT "Building open-shacc.suprx plugin"
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# Install Rules
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

install(FILES include/open_shacc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(IS_VITA_BUILD)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/open-shacc.suprx
            DESTINATION ${CMAKE_INSTALL_PREFIX}/bin OPTIONAL)
endif()

if(OPEN_SHACC_BUILD_SHARED)
    install(TARGETS open-shacc
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(OPEN_SHACC_BUILD_STATIC)
    install(TARGETS open-shacc-static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(OPEN_SHACC_BUILD_CLI)
    install(TARGETS open-shacc-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
