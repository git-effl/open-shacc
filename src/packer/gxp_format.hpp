/**
 * @file gxp_format.hpp
 * @brief Clean-room binary container specifications for GXP shader binaries.
 *
 * Implemented clean-room strictly from public open-source specifications.
 * Contains no proprietary constants or copyrighted structures.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_GXP_FORMAT_HPP
#define OPEN_SHACC_GXP_FORMAT_HPP

#include <cstdint>
#include <cstddef>

namespace open_shacc::packer {

constexpr uint32_t GXP_MAGIC = 0x00505847; // "GXP\0" in little endian ('G'=0x47, 'X'=0x58, 'P'=0x50, 0=0x00)
constexpr uint32_t GXP_VERSION_1 = 0x00000001;

enum class GxpShaderType : uint32_t {
    Vertex = 0,
    Fragment = 1
};

#pragma pack(push, 1)

struct GxpHeader {
    uint32_t magic;                  // "GXP\0" (0x00505847)
    uint32_t format_version;         // 1
    uint32_t shader_type;            // 0 = Vertex, 1 = Fragment
    uint32_t total_size;             // Total container size in bytes (16-byte aligned)
    uint32_t microcode_offset;       // Offset from start to microcode section
    uint32_t microcode_size;         // Size of microcode in bytes
    uint32_t symbol_table_offset;    // Offset to uniform symbol mappings
    uint32_t symbol_count;           // Number of uniform entries
    uint32_t attribute_table_offset; // Offset to vertex attributes
    uint32_t attribute_count;        // Number of input attributes
    uint16_t temp_reg_count;         // Maximum temporary registers used
    uint16_t const_reg_count;        // Constant uniform registers used
    uint32_t reserved[4];            // Reserved for future extensions / alignment
};

struct GxpSymbolEntry {
    char name[32];                   // Null-terminated uniform symbol name (e.g. "u_MVPMatrix")
    uint32_t register_offset;        // Constant register index (e.g. 0 for c0)
    uint32_t size_in_words;          // Size in 32-bit words (16 for float4x4, 4 for float4)
    uint32_t data_type;              // Standard data type identifier
    uint32_t semantic_index;         // Optional semantic index
};

struct GxpAttributeEntry {
    char name[32];                   // Input variable name (e.g. "position")
    char semantic[16];               // Semantic binding name (e.g. "POSITION", "TEXCOORD0")
    uint16_t stream_index;           // Vertex stream / slot index
    uint16_t component_count;        // 1..4 components
    uint16_t register_index;         // Input hardware register index (v0..v15)
    uint16_t reserved;
};

#pragma pack(pop)

static_assert(sizeof(GxpHeader) % 4 == 0, "GxpHeader must be 4-byte multiple");
static_assert(sizeof(GxpSymbolEntry) % 4 == 0, "GxpSymbolEntry must be 4-byte multiple");
static_assert(sizeof(GxpAttributeEntry) % 4 == 0, "GxpAttributeEntry must be 4-byte multiple");

inline size_t align16(size_t size) {
    return (size + 15) & ~15;
}

} // namespace open_shacc::packer

#endif // OPEN_SHACC_GXP_FORMAT_HPP
