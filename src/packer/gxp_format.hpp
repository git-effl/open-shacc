/**
 * @file gxp_format.hpp
 * @brief Clean-room binary container specifications for SceGxmProgram (GXP) shader binaries.
 *
 * Implemented clean-room strictly from public open-source specifications (Vita3K / VitaSDK).
 * Contains no proprietary code.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_GXP_FORMAT_HPP
#define OPEN_SHACC_GXP_FORMAT_HPP

#include <cstdint>
#include <cstddef>

namespace open_shacc::packer {

constexpr uint32_t GXP_MAGIC = 0x00505847; // "GXP\0" in little endian

// SceGxmProgram::program_flags bits
constexpr uint32_t GXP_PROGRAM_FLAG_FRAGMENT     = (1u << 0);
constexpr uint32_t GXP_PROGRAM_FLAG_DISCARD_USED = (1u << 3);

// SceGxmProgram::buffer_flags bit for default uniform buffer (container 14)
constexpr uint32_t GXP_BUFFER_FLAG_DEFAULT_UB = (1u << (14u * 2u)); // 0x10000000

// SceGxmParameterCategory
enum class GxpParameterCategory : uint16_t {
    Attribute        = 0,
    Uniform          = 1,
    Sampler          = 2,
    AuxiliarySurface = 3,
    UniformBuffer    = 4
};

// SceGxmParameterType
enum class GxpParameterType : uint16_t {
    F32       = 0,
    F16       = 1,
    C10       = 2,
    U32       = 3,
    S32       = 4,
    U16       = 5,
    S16       = 6,
    U8        = 7,
    S8        = 8,
    Aggregate = 9
};

// SceGxmParameterSemantic
enum class GxpParameterSemantic : uint8_t {
    None         = 0,
    Attr         = 1,
    Bcol         = 2,
    Binormal     = 3,
    BlendIndices = 4,
    BlendWeight  = 5,
    Color        = 6,
    Diffuse      = 7,
    FogCoord     = 8,
    Normal       = 9,
    PointSize    = 10,
    Position     = 11,
    Specular     = 12,
    Tangent      = 13,
    Texcoord     = 14
};

#pragma pack(push, 1)

struct SceGxmProgramHeader {
    uint32_t magic;                        // 0x00: "GXP\0" (0x00505847)
    uint8_t  major_version;                // 0x04: 1
    uint8_t  minor_version;                // 0x05: 4
    uint16_t sdk_version;                  // 0x06: 0x0350
    uint32_t size;                         // 0x08: Total container size in bytes
    uint32_t binary_guid;                  // 0x0C: Microcode hash
    uint32_t source_guid;                  // 0x10: Source/parameter hash
    uint32_t program_flags;                // 0x14: Fragment / discard flags
    uint32_t buffer_flags;                 // 0x18: Uniform buffer usage flags
    uint32_t texunit_flags[2];             // 0x1C: Texture unit flags (0..15)
    uint32_t parameter_count;              // 0x24: Number of SceGxmProgramParameter entries
    int32_t  parameters_offset;            // 0x28: Relative to &parameters_offset
    int32_t  varyings_offset;              // 0x2C: Relative to &varyings_offset
    uint16_t primary_reg_count;            // 0x30: PA register count (>= 1)
    uint16_t secondary_reg_count;          // 0x32: SA register count
    uint16_t temp_reg_count1;              // 0x34: Temporary register count (>= 1)
    uint16_t unk_36;                       // 0x36: 0
    uint16_t temp_reg_count2;              // 0x38: 0
    uint16_t primary_program_phase_count;  // 0x3A: 1
    uint32_t primary_program_instr_count;  // 0x3C: 64-bit USSE instruction count
    int32_t  primary_program_offset;       // 0x40: Relative to &primary_program_offset (8-byte aligned)
    uint32_t secondary_program_instr_count;// 0x44: 0
    int32_t  secondary_program_offset;     // 0x48: Relative to &secondary_program_offset -> ucode end
    int32_t  secondary_program_offset_end; // 0x4C: Relative to &secondary_program_offset_end -> ucode end
    uint32_t unk_50[5];                    // 0x50..0x63: 0
    uint32_t default_uniform_buffer_count; // 0x64: Default UB size in 32-bit words
    uint32_t unk_68;                       // 0x68: 0
    uint32_t compiler_version;             // 0x6C: 0x0350 << 4
    uint32_t unk_70[8];                    // 0x70..0x8F: 0
    uint32_t container_count;              // 0x90: Number of SceGxmProgramParameterContainer entries
    int32_t  container_offset;             // 0x94: Relative to &container_offset
    int32_t  dependent_common_imm_offset;  // 0x98: 0
};

struct SceGxmProgramVertexVaryings {
    uint16_t frag_output_param_type; // 0x00: Fragment: 0 (F32); Vertex: 0
    uint16_t frag_output_comp_count; // 0x02: Fragment: 4; Vertex: 0
    uint32_t attrib_pa_regs;         // 0x04: Vertex: PA register bitmask
    uint32_t untyped_pa_regs;        // 0x08: Vertex: 0
    uint32_t varyings_count;         // 0x0C: Fragment: varying descriptor count
    uint32_t vertex_outputs1;        // 0x10: Vertex: output flags; Fragment: rel offset from here to descriptors
    uint32_t vertex_outputs2;        // 0x14: Vertex: TEXCOORD0..9 component flags
    uint32_t unk_18;                 // 0x18: 0
    uint32_t unk_1C;                 // 0x1C: 0
};

struct SceGxmProgramAttributeDescriptor {
    uint32_t attribute_info;         // 0x00: Low nibble 0..9 TEXCOORD or 0xF + [15:12] semantic
    uint32_t size;                   // 0x04: Component count (1..4)
    uint32_t component_type;         // 0x08: 0 (F32)
    uint32_t reg_offset;             // 0x0C: PA register index
};

struct SceGxmProgramParameterContainer {
    uint16_t container_index;        // 0x00: 14 for default uniform buffer
    uint16_t unk_02;                 // 0x02: 0
    uint16_t base_sa_offset;         // 0x04: 0
    uint16_t size_in_f32;            // 0x06: default_uniform_buffer_count
};

struct SceGxmProgramParameter {
    int32_t  name_offset;            // 0x00: Relative to start of this parameter entry
    uint16_t type_info;              // 0x04: category:4 | type:4 | component_count:4 | container_index:4
    uint8_t  semantic;               // 0x06: GxpParameterSemantic
    uint8_t  semantic_index;         // 0x07: Semantic index (0..N)
    uint32_t array_size;             // 0x08: Array element count (1 for scalar/vector, rows for matrix)
    int32_t  resource_index;         // 0x0C: Register/word/unit index
};

#pragma pack(pop)

static_assert(sizeof(SceGxmProgramHeader) == 0x9C, "SceGxmProgramHeader must be 0x9C bytes");
static_assert(offsetof(SceGxmProgramHeader, parameters_offset) == 0x28, "parameters_offset mismatch");
static_assert(offsetof(SceGxmProgramHeader, varyings_offset) == 0x2C, "varyings_offset mismatch");
static_assert(offsetof(SceGxmProgramHeader, primary_program_offset) == 0x40, "primary_program_offset mismatch");
static_assert(offsetof(SceGxmProgramHeader, default_uniform_buffer_count) == 0x64, "default_uniform_buffer_count mismatch");
static_assert(offsetof(SceGxmProgramHeader, container_count) == 0x90, "container_count mismatch");
static_assert(sizeof(SceGxmProgramVertexVaryings) == 0x20, "SceGxmProgramVertexVaryings must be 0x20 bytes");
static_assert(sizeof(SceGxmProgramAttributeDescriptor) == 0x10, "SceGxmProgramAttributeDescriptor must be 0x10 bytes");
static_assert(sizeof(SceGxmProgramParameterContainer) == 0x08, "SceGxmProgramParameterContainer must be 0x08 bytes");
static_assert(sizeof(SceGxmProgramParameter) == 0x10, "SceGxmProgramParameter must be 0x10 bytes");

// Backwards-compatible alias for any callers referencing GxpHeader
using GxpHeader = SceGxmProgramHeader;

inline size_t align8(size_t size) {
    return (size + 7) & ~static_cast<size_t>(7);
}

inline size_t align16(size_t size) {
    return (size + 15) & ~static_cast<size_t>(15);
}

} // namespace open_shacc::packer

#endif // OPEN_SHACC_GXP_FORMAT_HPP
