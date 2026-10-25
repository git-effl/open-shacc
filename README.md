# open-shacc

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Clean-Room](https://img.shields.io/badge/Clean--Room-Certified-green.svg)](#clean-room-statement)

An open-source, clean-room replacement for the `libshacccg` Cg/HLSL shader compiler targeting the Universal Scalable Shader Engine (USSE) and packing into the standard `GXP\0` binary container.

---

## Clean-Room Statement

`open-shacc` is engineered from scratch under strict clean-room software engineering rules:
- **Zero Proprietary Headers**: No headers, symbol lists, or definitions from proprietary SDKs were utilized.
- **Specification-Driven**: Parsers conform to the public NVIDIA Cg Language Specification and standard HLSL Shader Model grammar.
- **Public ISA Architecture**: The USSE microcode generator is synthesized from public open-source drivers (e.g. Mesa 3D open graphics stack) and standard hardware architecture patents.
- **100% Modern C++20**: Zero external binary dependencies; self-contained lexer, AST parser, linear scan register allocator, and 16-byte aligned GXP container serializer.

---

## Architectural Pipeline

```
 [ Cg / HLSL Source ]
          │
          ▼
┌──────────────────┐
│  Lexer & Parser  │  <-- Extracts types (float, float4, float4x4), uniforms,
└─────────┬────────┘      structs, semantics (: POSITION, : TEXCOORD0),
          │               and builds vector arithmetic AST (mul, dp3, dp4, swizzle)
          ▼
┌──────────────────┐
│ Linear Scan Reg  │  <-- Maps virtual variables to temporary registers (r0..r63),
│    Allocator     │      constant uniforms (c0..c255), and primary inputs (v0..v15)
└─────────┬────────┘
          │
          ▼
┌──────────────────┐
│  USSE Codegen    │  <-- Emits 32-bit VLIW microcode words (FADD, FMUL, FMAD,
└─────────┬────────┘      DOT3, DOT4, MOV, PHAS, HALT) with disassembly logging
          │
          ▼
┌──────────────────┐
│   GXP Encoder    │  <-- Packs "GXP\0" magic header, uniform symbol table,
└─────────┬────────┘      attribute layout, and 16-byte aligned microcode words
          │
          ▼
  [ .gxp Binary ]
```

---

## Building with CMake

### Requirements
- Modern C++20 compiler (`g++ >= 11`, `clang++ >= 13`, or `MSVC >= 2019`)
- CMake >= 3.20
- Ninja or Make

### Commands
```bash
# Clone the repository
git clone https://github.com/git-effl/open-shacc.git
cd open-shacc

# Configure build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build shared library, static library, CLI, and test suite
cmake --build build --config Release

# Run tests
ctest --test-dir build --output-on-failure
```

---

## Public C API Usage

Include `open_shacc.h` in your C or C++ application:

```c
#include <open_shacc.h>
#include <stdio.h>

int main() {
    const char* shader_source = 
        "uniform float4x4 u_MVPMatrix;\n"
        "float4 main(float4 position : POSITION) : POSITION {\n"
        "    return mul(u_MVPMatrix, position);\n"
        "}\n";

    void* gxp_binary = NULL;
    size_t gxp_size = 0;
    char* diag_log = NULL;

    int result = SceShaccCgCompileProgram(
        shader_source,
        OPEN_SHACC_OPT_VERBOSE_DISASSEMBLY,
        NULL,
        &gxp_binary,
        &gxp_size,
        &diag_log
    );

    if (diag_log) {
        printf("%s\n", diag_log);
        SceShaccCgDestroyDiagnosticLog(diag_log);
    }

    if (result == OPEN_SHACC_SUCCESS) {
        printf("Successfully generated GXP binary: %zu bytes (16-byte aligned)\n", gxp_size);
        // Use gxp_binary...
        SceShaccCgDestroyProgram(gxp_binary);
    }

    return 0;
}
```

---

## CLI Usage

```bash
# Compile Cg shader to GXP binary
./open-shacc-cli my_shader.cg -o my_shader.gxp -v

# Inspect disassembly
./open-shacc-cli my_shader.cg --disasm
```

---

## GXP Binary Format Layout

All sections are padded to 16-byte boundaries:

| Offset | Section | Description |
|---|---|---|
| `0x00` | `GxpHeader` | Magic `"GXP\0"` (`0x00505847`), version, offsets, register counts |
| `0x30` | Uniform Symbol Table | Maps uniform identifiers (e.g. `u_MVPMatrix`) to constant register `c` offsets |
| `...`  | Attribute Table | Input streams, semantics (`POSITION`, `TEXCOORD0`), hardware input registers |
| `...`  | Microcode Section | Aligned 32-bit USSE microcode instruction stream |

---

## License

`open-shacc` is licensed under the permissive [MIT License](LICENSE).
