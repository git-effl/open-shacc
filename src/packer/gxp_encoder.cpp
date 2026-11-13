/**
 * @file gxp_encoder.cpp
 * @brief Implementation of SceGxmProgram (GXP) Binary Container Packer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "gxp_encoder.hpp"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <utility>

namespace open_shacc::packer {

namespace {

uint32_t fnv1a_32(const void* data, size_t len, uint32_t seed = 0x811C9DC5u) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= bytes[i];
        h *= 0x01000193u;
    }
    return h;
}

std::pair<GxpParameterSemantic, uint8_t> parse_semantic(const std::string& raw_sem) {
    if (raw_sem.empty()) {
        return {GxpParameterSemantic::None, 0};
    }

    std::string upper;
    upper.reserve(raw_sem.size());
    for (char c : raw_sem) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    size_t idx_pos = upper.size();
    while (idx_pos > 0 && std::isdigit(static_cast<unsigned char>(upper[idx_pos - 1]))) {
        --idx_pos;
    }

    uint8_t sem_idx = 0;
    if (idx_pos < upper.size()) {
        sem_idx = static_cast<uint8_t>(std::atoi(upper.substr(idx_pos).c_str()));
    }
    std::string base = upper.substr(0, idx_pos);

    if (base == "POSITION" || base == "WPOS") return {GxpParameterSemantic::Position, sem_idx};
    if (base == "TEXCOORD")                   return {GxpParameterSemantic::Texcoord, sem_idx};
    if (base == "COLOR" || base == "COLOR0")  return {GxpParameterSemantic::Color, sem_idx};
    if (base == "NORMAL")                     return {GxpParameterSemantic::Normal, sem_idx};
    if (base == "TANGENT")                    return {GxpParameterSemantic::Tangent, sem_idx};
    if (base == "BINORMAL")                   return {GxpParameterSemantic::Binormal, sem_idx};
    if (base == "FOG" || base == "FOGCOORD")  return {GxpParameterSemantic::FogCoord, sem_idx};
    if (base == "PSIZE" || base == "POINTSIZE") return {GxpParameterSemantic::PointSize, sem_idx};
    if (base == "BLENDINDICES")               return {GxpParameterSemantic::BlendIndices, sem_idx};
    if (base == "BLENDWEIGHT")                return {GxpParameterSemantic::BlendWeight, sem_idx};
    if (base == "DIFFUSE")                    return {GxpParameterSemantic::Diffuse, sem_idx};
    if (base == "SPECULAR")                   return {GxpParameterSemantic::Specular, sem_idx};
    if (base == "ATTR")                       return {GxpParameterSemantic::Attr, sem_idx};
    if (base == "BCOL")                       return {GxpParameterSemantic::Bcol, sem_idx};
    return {GxpParameterSemantic::None, sem_idx};
}

GxpParameterType data_type_to_gxp_type(const frontend::DataType& dt) {
    switch (dt.base) {
        case frontend::BaseType::Half:
        case frontend::BaseType::Half2:
        case frontend::BaseType::Half3:
        case frontend::BaseType::Half4:
        case frontend::BaseType::Half2x2:
        case frontend::BaseType::Half3x3:
        case frontend::BaseType::Half4x4:
            return GxpParameterType::F16;
        case frontend::BaseType::Fixed:
        case frontend::BaseType::Fixed2:
        case frontend::BaseType::Fixed3:
        case frontend::BaseType::Fixed4:
            return GxpParameterType::C10;
        case frontend::BaseType::UInt:
        case frontend::BaseType::UInt2:
        case frontend::BaseType::UInt3:
        case frontend::BaseType::UInt4:
        case frontend::BaseType::Bool:
        case frontend::BaseType::Bool2:
        case frontend::BaseType::Bool3:
        case frontend::BaseType::Bool4:
            return GxpParameterType::U32;
        case frontend::BaseType::Int:
        case frontend::BaseType::Int2:
        case frontend::BaseType::Int3:
        case frontend::BaseType::Int4:
            return GxpParameterType::S32;
        case frontend::BaseType::UShort:
        case frontend::BaseType::UShort2:
        case frontend::BaseType::UShort3:
        case frontend::BaseType::UShort4:
            return GxpParameterType::U16;
        case frontend::BaseType::Short:
        case frontend::BaseType::Short2:
        case frontend::BaseType::Short3:
        case frontend::BaseType::Short4:
            return GxpParameterType::S16;
        case frontend::BaseType::UChar:
            return GxpParameterType::U8;
        case frontend::BaseType::Char:
            return GxpParameterType::S8;
        default:
            return GxpParameterType::F32;
    }
}

uint16_t pack_type_info(
    GxpParameterCategory cat,
    GxpParameterType type,
    uint16_t component_count,
    uint16_t container_index
) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(cat) & 0xFu) |
        ((static_cast<uint16_t>(type) & 0xFu) << 4) |
        ((component_count & 0xFu) << 8) |
        ((container_index & 0xFu) << 12)
    );
}

uint32_t make_attribute_info(const std::string& raw_sem) {
    auto [sem, sem_idx] = parse_semantic(raw_sem);
    if (sem == GxpParameterSemantic::Texcoord && sem_idx < 10) {
        return static_cast<uint32_t>(sem_idx & 0xFu) | (0xFu << 12);
    }
    if (sem == GxpParameterSemantic::Color) {
        uint32_t code = (sem_idx == 1) ? 0xBu : 0xAu;
        return 0xFu | (code << 12);
    }
    if (sem == GxpParameterSemantic::FogCoord) {
        return 0xFu | (0xCu << 12);
    }
    if (sem == GxpParameterSemantic::Position) {
        return 0xFu | (0xDu << 12);
    }
    return 0xFu | (0xFu << 12);
}

} // namespace

bool GxpEncoder::encode(const backend::CompiledProgram& program, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    error_message_.clear();

    const bool is_frag = !program.is_vertex_shader;

    // 1. Build logical parameter entries
    struct PendingParam {
        std::string name;
        SceGxmProgramParameter raw{};
    };
    std::vector<PendingParam> pending_params;

    // Uniforms (Default UB = container 14)
    for (const auto& u : program.uniforms) {
        PendingParam p;
        p.name = u.name;
        auto [sem, sem_idx] = parse_semantic(u.semantic);

        uint16_t comp_count = static_cast<uint16_t>(u.type.component_count());
        uint32_t arr_size = (u.type.array_size > 1) ? u.type.array_size : 1u;
        if (u.type.is_matrix()) {
            comp_count = static_cast<uint16_t>(u.type.matrix_cols());
            arr_size = static_cast<uint32_t>(u.type.matrix_rows() * (u.type.array_size > 1 ? u.type.array_size : 1u));
        }
        if (comp_count == 0) comp_count = 1;

        p.raw.name_offset = 0;
        p.raw.type_info = pack_type_info(
            GxpParameterCategory::Uniform,
            data_type_to_gxp_type(u.type),
            comp_count,
            14u
        );
        p.raw.semantic = static_cast<uint8_t>(sem);
        p.raw.semantic_index = sem_idx;
        p.raw.array_size = arr_size;
        p.raw.resource_index = static_cast<int32_t>(u.base_register);
        pending_params.push_back(std::move(p));
    }

    // Samplers
    for (const auto& s : program.samplers) {
        PendingParam p;
        p.name = s.name;
        p.raw.name_offset = 0;
        p.raw.type_info = pack_type_info(
            GxpParameterCategory::Sampler,
            GxpParameterType::F32,
            4u,
            0u
        );
        p.raw.semantic = static_cast<uint8_t>(GxpParameterSemantic::None);
        p.raw.semantic_index = 0;
        p.raw.array_size = (s.type.array_size > 1) ? s.type.array_size : 1u;
        p.raw.resource_index = static_cast<int32_t>(s.base_register);
        pending_params.push_back(std::move(p));
    }

    // Vertex inputs as ATTRIBUTE parameters (and fragment inputs if not duplicate)
    if (!is_frag) {
        for (const auto& in : program.inputs) {
            PendingParam p;
            p.name = in.name;
            auto [sem, sem_idx] = parse_semantic(in.semantic);
            uint16_t comp_count = static_cast<uint16_t>(in.type.component_count());
            if (comp_count == 0 || comp_count > 4) comp_count = 4;

            p.raw.name_offset = 0;
            p.raw.type_info = pack_type_info(
                GxpParameterCategory::Attribute,
                data_type_to_gxp_type(in.type),
                comp_count,
                0u
            );
            p.raw.semantic = static_cast<uint8_t>(sem);
            p.raw.semantic_index = sem_idx;
            p.raw.array_size = (in.type.array_size > 1) ? in.type.array_size : 1u;
            p.raw.resource_index = static_cast<int32_t>(in.base_register);
            pending_params.push_back(std::move(p));
        }
    }

    // 2. Compute exact section offsets
    const size_t header_off = 0;
    const size_t varyings_off = sizeof(SceGxmProgramHeader); // 0x9C
    const size_t frag_descs_off = varyings_off + sizeof(SceGxmProgramVertexVaryings); // 0xBC
    const size_t frag_desc_count = is_frag ? program.inputs.size() : 0;
    const size_t frag_descs_size = frag_desc_count * sizeof(SceGxmProgramAttributeDescriptor);

    const uint32_t default_ub_count = static_cast<uint32_t>(program.const_register_count);
    const uint32_t container_count = (default_ub_count > 0) ? 1u : 0u;
    const size_t container_off = frag_descs_off + frag_descs_size;
    const size_t container_size = container_count * sizeof(SceGxmProgramParameterContainer);

    const size_t params_off = container_off + container_size;
    const size_t params_size = pending_params.size() * sizeof(SceGxmProgramParameter);

    const size_t strings_off = params_off + params_size;
    std::vector<size_t> string_offsets;
    string_offsets.reserve(pending_params.size());
    size_t cur_str_off = strings_off;
    for (const auto& p : pending_params) {
        string_offsets.push_back(cur_str_off);
        cur_str_off += p.name.size() + 1;
    }

    const size_t ucode_off = align8(cur_str_off);
    const size_t ucode_raw_size = program.microcode_words.size() * sizeof(uint64_t);
    const size_t ucode_end_off = ucode_off + ucode_raw_size;
    const size_t total_size = align16(ucode_end_off);

    out_bytes.assign(total_size, 0);

    // 3. Populate Varyings struct
    SceGxmProgramVertexVaryings varyings{};
    if (!is_frag) {
        uint32_t pa_mask = 0;
        for (const auto& in : program.inputs) {
            uint32_t start_reg = in.base_register / 4u;
            uint32_t end_reg = (in.base_register + in.register_count + 3u) / 4u;
            for (uint32_t r = start_reg; r < end_reg && r < 16u; ++r) {
                pa_mask |= (1u << r);
            }
        }
        varyings.attrib_pa_regs = pa_mask;
        varyings.untyped_pa_regs = 0;

        uint32_t v_out1 = 0;
        uint32_t v_out2 = 0;
        for (const auto& out : program.outputs) {
            auto [sem, sem_idx] = parse_semantic(out.semantic);
            if (sem == GxpParameterSemantic::Color) {
                v_out1 |= (sem_idx == 1) ? 0x400u : 0x800u;
            } else if (sem == GxpParameterSemantic::FogCoord) {
                v_out1 |= 0x200u;
            } else if (sem == GxpParameterSemantic::PointSize) {
                v_out1 |= 0x100u;
            } else if (sem == GxpParameterSemantic::Texcoord && sem_idx < 10) {
                size_t comp = std::clamp<size_t>(out.type.component_count(), 2, 4);
                uint32_t bits = (comp == 2) ? 1u : ((comp == 3) ? 3u : 7u);
                v_out2 |= (bits << (sem_idx * 3u));
            }
        }
        varyings.vertex_outputs1 = v_out1;
        varyings.vertex_outputs2 = v_out2;
    } else {
        varyings.frag_output_param_type = 0; // F32
        varyings.frag_output_comp_count = 4;
        varyings.varyings_count = static_cast<uint32_t>(frag_desc_count);
        // Offset from &varyings.vertex_outputs1 (varyings_off + 0x10) to frag_descs_off (varyings_off + 0x20)
        varyings.vertex_outputs1 = (frag_desc_count > 0) ? 16u : 0u;
        varyings.vertex_outputs2 = 0;
    }
    std::memcpy(out_bytes.data() + varyings_off, &varyings, sizeof(varyings));

    // 4. Write Fragment Varying Descriptors
    for (size_t i = 0; i < frag_desc_count; ++i) {
        const auto& in = program.inputs[i];
        SceGxmProgramAttributeDescriptor desc{};
        desc.attribute_info = make_attribute_info(in.semantic);
        desc.size = static_cast<uint32_t>(std::clamp<size_t>(in.type.component_count(), 1, 4));
        desc.component_type = 0; // F32
        desc.reg_offset = in.base_register;
        std::memcpy(out_bytes.data() + frag_descs_off + (i * sizeof(desc)), &desc, sizeof(desc));
    }

    // 5. Write Default Uniform Buffer Container (container 14)
    if (container_count > 0) {
        SceGxmProgramParameterContainer cont{};
        cont.container_index = 14;
        cont.unk_02 = 0;
        cont.base_sa_offset = 0;
        cont.size_in_f32 = static_cast<uint16_t>(std::min<uint32_t>(default_ub_count, 0xFFFFu));
        std::memcpy(out_bytes.data() + container_off, &cont, sizeof(cont));
    }

    // 6. Write Parameters Table + Name Strings
    for (size_t i = 0; i < pending_params.size(); ++i) {
        const size_t entry_off = params_off + (i * sizeof(SceGxmProgramParameter));
        SceGxmProgramParameter param = pending_params[i].raw;
        param.name_offset = static_cast<int32_t>(string_offsets[i] - entry_off);
        std::memcpy(out_bytes.data() + entry_off, &param, sizeof(param));
        std::memcpy(
            out_bytes.data() + string_offsets[i],
            pending_params[i].name.c_str(),
            pending_params[i].name.size() + 1
        );
    }

    // 7. Write 64-bit USSE Microcode
    if (!program.microcode_words.empty()) {
        std::memcpy(out_bytes.data() + ucode_off, program.microcode_words.data(), ucode_raw_size);
    }

    // 8. Populate SceGxmProgramHeader
    SceGxmProgramHeader header{};
    header.magic = GXP_MAGIC;
    header.major_version = 1;
    header.minor_version = 4;
    header.sdk_version = 0x0350;
    header.size = static_cast<uint32_t>(total_size);
    header.binary_guid = fnv1a_32(out_bytes.data() + ucode_off, ucode_raw_size);
    header.source_guid = fnv1a_32(
        program.disassembly.data(),
        program.disassembly.size(),
        header.binary_guid ^ 0x9E3779B9u
    );

    if (is_frag) {
        header.program_flags |= GXP_PROGRAM_FLAG_FRAGMENT;
        if (program.uses_discard) {
            header.program_flags |= GXP_PROGRAM_FLAG_DISCARD_USED;
        }
    }
    if (default_ub_count > 0) {
        header.buffer_flags |= GXP_BUFFER_FLAG_DEFAULT_UB;
    }
    for (const auto& s : program.samplers) {
        uint32_t count = (s.type.array_size > 1) ? s.type.array_size : 1u;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t unit = static_cast<uint32_t>(s.base_register) + k;
            if (unit < 16u) {
                header.texunit_flags[unit / 8u] |= (1u << ((unit % 8u) * 4u));
            }
        }
    }

    header.parameter_count = static_cast<uint32_t>(pending_params.size());
    header.parameters_offset = pending_params.empty()
        ? 0
        : static_cast<int32_t>(params_off - offsetof(SceGxmProgramHeader, parameters_offset));
    header.varyings_offset = static_cast<int32_t>(varyings_off - offsetof(SceGxmProgramHeader, varyings_offset));

    header.primary_reg_count = std::max<uint16_t>(1, program.input_register_count);
    header.secondary_reg_count = static_cast<uint16_t>(default_ub_count);
    header.temp_reg_count1 = std::max<uint16_t>(1, program.temp_register_count);
    header.temp_reg_count2 = 0;
    header.primary_program_phase_count = 1;

    header.primary_program_instr_count = static_cast<uint32_t>(program.microcode_words.size());
    header.primary_program_offset = static_cast<int32_t>(ucode_off - offsetof(SceGxmProgramHeader, primary_program_offset));
    header.secondary_program_instr_count = 0;
    header.secondary_program_offset = static_cast<int32_t>(ucode_end_off - offsetof(SceGxmProgramHeader, secondary_program_offset));
    header.secondary_program_offset_end = static_cast<int32_t>(ucode_end_off - offsetof(SceGxmProgramHeader, secondary_program_offset_end));

    header.default_uniform_buffer_count = default_ub_count;
    header.compiler_version = 0x0350u << 4;
    header.container_count = container_count;
    header.container_offset = (container_count > 0)
        ? static_cast<int32_t>(container_off - offsetof(SceGxmProgramHeader, container_offset))
        : 0;
    header.dependent_common_imm_offset = 0;

    std::memcpy(out_bytes.data() + header_off, &header, sizeof(header));
    return true;
}

bool GxpEncoder::validate(const uint8_t* data, size_t size, std::string& out_error) {
    if (!data || size < sizeof(SceGxmProgramHeader)) {
        out_error = "Binary too small for SceGxmProgram header";
        return false;
    }

    const auto* header = reinterpret_cast<const SceGxmProgramHeader*>(data);
    if (header->magic != GXP_MAGIC) {
        out_error = "Invalid magic bytes (expected 'GXP\\0')";
        return false;
    }

    if (header->size > size || header->size < sizeof(SceGxmProgramHeader)) {
        out_error = "Declared container size invalid or exceeds buffer size";
        return false;
    }

    const int64_t ucode_abs = static_cast<int64_t>(offsetof(SceGxmProgramHeader, primary_program_offset))
                            + static_cast<int64_t>(header->primary_program_offset);
    const int64_t ucode_bytes = static_cast<int64_t>(header->primary_program_instr_count) * 8;
    if (ucode_abs < 0 || (ucode_abs + ucode_bytes) > static_cast<int64_t>(header->size)) {
        out_error = "Microcode payload extends past container boundary";
        return false;
    }

    if (header->parameter_count > 0) {
        const int64_t params_abs = static_cast<int64_t>(offsetof(SceGxmProgramHeader, parameters_offset))
                                 + static_cast<int64_t>(header->parameters_offset);
        const int64_t params_bytes = static_cast<int64_t>(header->parameter_count) * sizeof(SceGxmProgramParameter);
        if (params_abs < 0 || (params_abs + params_bytes) > static_cast<int64_t>(header->size)) {
            out_error = "Parameter table extends past container boundary";
            return false;
        }
    }

    return true;
}

} // namespace open_shacc::packer
