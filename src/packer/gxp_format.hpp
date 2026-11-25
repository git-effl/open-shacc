/**
 * @file gxp_format.hpp
 * @brief Clean-room binary container specifications for SceGxmProgram (GXP) shader binaries.
 *
 * Implemented strictly from public open-source specifications (VitaSDK).
 * Contains no proprietary code.
 */

#ifndef OPEN_SHACC_GXP_FORMAT_HPP
#define OPEN_SHACC_GXP_FORMAT_HPP

#include <cstdint>
#include <cstddef>

namespace open_shacc::packer {

constexpr uint32_t GXP_MAGIC = 0x00505847; // "GXP\0" in little endian

// SceGxmProgram::program_flags bits
constexpr uint32_t GXP_PROGRAM_FLAG_FRAGMENT        = (1u << 0);
constexpr uint32_t GXP_PROGRAM_FLAG_DISCARD_USED    = (1u << 3);
constexpr uint32_t GXP_PROGRAM_FLAG_DEPTH_USED      = (1u << 4);
constexpr uint32_t GXP_PROGRAM_FLAG_SPRITECOORD     = (1u << 5);
constexpr uint32_t GXP_PROGRAM_FLAG_NATIVECOLOR_USED = (1u << 6);

// SceGxmProgram::buffer_flags bit for default uniform buffer (container 14)
constexpr uint32_t GXP_BUFFER_FLAG_DEFAULT_UB = (1u << (14u * 2u)); // 0x10000000

// GXP vertex_outputs1 bit flags
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP0  = (1u << 0);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP1  = (1u << 1);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP2  = (1u << 2);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP3  = (1u << 3);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP4  = (1u << 4);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP5  = (1u << 5);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP6  = (1u << 6);
constexpr uint32_t GXP_VERTEX_OUTPUT_CLIP7  = (1u << 7);
constexpr uint32_t GXP_VERTEX_OUTPUT_PSIZE  = (1u << 8);
constexpr uint32_t GXP_VERTEX_OUTPUT_FOG    = (1u << 9);
constexpr uint32_t GXP_VERTEX_OUTPUT_COLOR1 = (1u << 10);
constexpr uint32_t GXP_VERTEX_OUTPUT_COLOR0 = (1u << 11);

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
    uint32_t magic;                                  // 0x00: "GXP\0" (0x00505847)
    uint8_t  major_version;                          // 0x04: 1
    uint8_t  minor_version;                          // 0x05: 4
    uint16_t sdk_version;                            // 0x06: 0x0350
    uint32_t size;                                   // 0x08: Total container size in bytes
    uint32_t binary_guid;                            // 0x0C: Microcode hash
    uint32_t source_guid;                            // 0x10: Source/parameter hash
    uint32_t program_flags;                          // 0x14: Fragment / discard / depth / nativecolor flags
    uint32_t buffer_flags;                           // 0x18: Uniform buffer usage flags
    uint32_t texunit_flags[2];                       // 0x1C: Texture unit flags (0..15)
    uint32_t parameter_count;                        // 0x24: Number of SceGxmProgramParameter entries
    int32_t  parameters_offset;                      // 0x28: Relative to &parameters_offset
    int32_t  varyings_offset;                        // 0x2C: Relative to &varyings_offset
    uint16_t primary_reg_count;                      // 0x30: PA register count (>= 1)
    uint16_t secondary_reg_count;                    // 0x32: SA register count
    uint32_t temp_reg_count1;                        // 0x34: Temporary register count (>= 1)
    uint16_t temp_reg_count2;                        // 0x38: 0
    uint16_t primary_program_phase_count;            // 0x3A: 1
    uint32_t primary_program_instr_count;            // 0x3C: 64-bit USSE instruction count
    int32_t  primary_program_offset;                 // 0x40: Relative to &primary_program_offset (8-byte aligned)
    uint32_t secondary_program_instr_count;          // 0x44: 0
    int32_t  secondary_program_offset;               // 0x48: Relative to &secondary_program_offset
    int32_t  secondary_program_offset_end;           // 0x4C: Relative to &secondary_program_offset_end
    uint32_t scratch_buffer_count;                   // 0x50: 0
    uint32_t thread_buffer_count;                    // 0x54: 0
    uint32_t literal_buffer_count;                   // 0x58: 0
    uint32_t data_buffer_count;                      // 0x5C: 0
    uint32_t texture_buffer_count;                   // 0x60: 0
    uint32_t default_uniform_buffer_count;           // 0x64: Default UB size in 32-bit words
    uint32_t literal_buffer_data_offset;             // 0x68: 0
    uint32_t compiler_version;                       // 0x6C: 0x0350 << 4
    uint32_t literals_count;                         // 0x70: 0
    uint32_t literals_offset;                        // 0x74: 0
    uint32_t uniform_buffer_count;                   // 0x78: 0
    uint32_t uniform_buffer_offset;                  // 0x7C: 0
    uint32_t dependent_sampler_count;                // 0x80: Number of SceGxmDependentSampler entries
    int32_t  dependent_sampler_offset;               // 0x84: Relative to &dependent_sampler_offset
    uint32_t texture_buffer_dependent_sampler_count; // 0x88: 0
    int32_t  texture_buffer_dependent_sampler_offset;// 0x8C: 0
    uint32_t container_count;                        // 0x90: Number of SceGxmProgramParameterContainer entries
    int32_t  container_offset;                       // 0x94: Relative to &container_offset
    int32_t  sampler_query_info_offset;              // 0x98: 0
};

struct SceGxmProgramVertexVaryings {
    union {
        struct {
            uint64_t attrib_pa_regs;   // 0x00: Vertex PA register bitmask
            uint64_t untyped_pa_regs;  // 0x08: Vertex untyped PA registers bitmask
        } vert;
        struct {
            uint8_t  unk0[8];               // 0x00..0x07
            uint8_t  fragment_output_start; // 0x08
            uint8_t  unk1;                  // 0x09
            uint8_t  output_param_type;     // 0x0A: GxpParameterType (0 = F32)
            uint8_t  output_comp_count;     // 0x0B: Component count (4)
            uint16_t varyings_count;        // 0x0C: Fragment varying descriptor count
            uint16_t pad0;                  // 0x0E: 0
        } frag;
    };
    uint32_t vertex_outputs1;          // 0x10: Vertex output flags; Frag rel offset from here to descriptors
    uint32_t vertex_outputs2;          // 0x14: Vertex TEXCOORD0..9 component flags
    uint32_t texcoord_pack_format;     // 0x18: 1 bit per texcoord (0=float, 1=half)
    uint16_t semantic_index_offset;    // 0x1C: 0
    uint16_t semantic_instance_offset; // 0x1E: 0
};

struct SceGxmProgramAttributeDescriptor {
    uint32_t attribute_info;         // 0x00: [15:12] sem code | 0xF (iterator) | ((comps-1)<<22) | 0x100000 (F32 color)
    uint32_t resource_index;         // 0x04: Resource / sampler index (0 for iterator)
    uint32_t size;                   // 0x08: ((pa_word_count - 1) & 3) << 4
    uint32_t component_info;         // 0x0C: PA register word offset
};

struct SceGxmDependentSampler {
    uint16_t resource_index_layout_offset; // 0x00: sampler_index * 4 + layout_pos
    uint16_t sa_offset;                    // 0x02: Offset relative to container 19 base_sa_offset
};

struct SceGxmProgramParameterContainer {
    uint16_t container_index;        // 0x00: 14 for default UB, 19 for data/dependent samplers
    uint16_t unk_02;                 // 0x02: 0
    uint16_t base_sa_offset;         // 0x04: Base SA register index
    uint16_t size_in_f32;            // 0x06: Size in 32-bit float words
};

struct SceGxmProgramParameter {
    int32_t  name_offset;            // 0x00: Relative to start of this parameter entry
    uint16_t type_info;              // 0x04: category:4 | type:4 | component_count:4 | container_index:4
    uint8_t  semantic;               // 0x06: GxpParameterSemantic
    uint8_t  semantic_index;         // 0x07: Semantic index (0..N, bit 4 set for samplerCUBE)
    uint32_t array_size;             // 0x08: Array element count (1 for scalar/vector, rows for matrix)
    int32_t  resource_index;         // 0x0C: Register/word/unit index
};

#pragma pack(pop)

static_assert(sizeof(SceGxmProgramHeader) == 0x9C, "SceGxmProgramHeader must be 0x9C bytes");
static_assert(offsetof(SceGxmProgramHeader, parameters_offset) == 0x28, "parameters_offset mismatch");
static_assert(offsetof(SceGxmProgramHeader, varyings_offset) == 0x2C, "varyings_offset mismatch");
static_assert(offsetof(SceGxmProgramHeader, primary_program_offset) == 0x40, "primary_program_offset mismatch");
static_assert(offsetof(SceGxmProgramHeader, default_uniform_buffer_count) == 0x64, "default_uniform_buffer_count mismatch");
static_assert(offsetof(SceGxmProgramHeader, dependent_sampler_count) == 0x80, "dependent_sampler_count mismatch");
static_assert(offsetof(SceGxmProgramHeader, container_count) == 0x90, "container_count mismatch");
static_assert(sizeof(SceGxmProgramVertexVaryings) == 0x20, "SceGxmProgramVertexVaryings must be 0x20 bytes");
static_assert(offsetof(SceGxmProgramVertexVaryings, vertex_outputs1) == 0x10, "vertex_outputs1 offset mismatch");
static_assert(offsetof(SceGxmProgramVertexVaryings, vertex_outputs2) == 0x14, "vertex_outputs2 offset mismatch");
static_assert(offsetof(SceGxmProgramVertexVaryings, texcoord_pack_format) == 0x18, "texcoord_pack_format offset mismatch");
static_assert(sizeof(SceGxmProgramAttributeDescriptor) == 0x10, "SceGxmProgramAttributeDescriptor must be 0x10 bytes");
static_assert(sizeof(SceGxmDependentSampler) == 0x04, "SceGxmDependentSampler must be 0x04 bytes");
static_assert(sizeof(SceGxmProgramParameterContainer) == 0x08, "SceGxmProgramParameterContainer must be 0x08 bytes");
static_assert(sizeof(SceGxmProgramParameter) == 0x10, "SceGxmProgramParameter must be 0x10 bytes");

using GxpHeader = SceGxmProgramHeader;

inline size_t align8(size_t size) {
    return (size + 7) & ~static_cast<size_t>(7);
}

inline size_t align16(size_t size) {
    return (size + 15) & ~static_cast<size_t>(15);
}

} // namespace open_shacc::packer

#endif // OPEN_SHACC_GXP_FORMAT_HPP
