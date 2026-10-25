/**
 * @file gxp_encoder.cpp
 * @brief Implementation of GXP Binary Container Packer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "gxp_encoder.hpp"
#include <cstring>
#include <algorithm>

namespace open_shacc::packer {

bool GxpEncoder::encode(const backend::CompiledProgram& program, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    error_message_.clear();

    // 1. Calculate section sizes and offsets
    size_t header_size = align16(sizeof(GxpHeader));

    size_t symbols_raw_size = program.uniforms.size() * sizeof(GxpSymbolEntry);
    size_t symbols_size = align16(symbols_raw_size);

    size_t attrs_raw_size = program.inputs.size() * sizeof(GxpAttributeEntry);
    size_t attrs_size = align16(attrs_raw_size);

    size_t microcode_raw_size = program.microcode_words.size() * sizeof(uint32_t);
    size_t microcode_size = align16(microcode_raw_size);

    uint32_t symbol_offset = static_cast<uint32_t>(header_size);
    uint32_t attr_offset = static_cast<uint32_t>(symbol_offset + symbols_size);
    uint32_t ucode_offset = static_cast<uint32_t>(attr_offset + attrs_size);
    uint32_t total_size = static_cast<uint32_t>(ucode_offset + microcode_size);

    out_bytes.resize(total_size, 0); // Zero-initialized padding

    // 2. Populate Header
    GxpHeader header{};
    header.magic = GXP_MAGIC;
    header.format_version = GXP_VERSION_1;
    header.shader_type = program.is_vertex_shader ? static_cast<uint32_t>(GxpShaderType::Vertex)
                                                  : static_cast<uint32_t>(GxpShaderType::Fragment);
    header.total_size = total_size;
    header.microcode_offset = ucode_offset;
    header.microcode_size = static_cast<uint32_t>(microcode_raw_size);
    header.symbol_table_offset = symbol_offset;
    header.symbol_count = static_cast<uint32_t>(program.uniforms.size());
    header.attribute_table_offset = attr_offset;
    header.attribute_count = static_cast<uint32_t>(program.inputs.size());
    header.temp_reg_count = program.temp_register_count;
    header.const_reg_count = program.const_register_count;

    std::memcpy(out_bytes.data(), &header, sizeof(GxpHeader));

    // 3. Write Symbol Table
    uint8_t* sym_ptr = out_bytes.data() + symbol_offset;
    for (size_t i = 0; i < program.uniforms.size(); ++i) {
        const auto& u = program.uniforms[i];
        GxpSymbolEntry entry{};
        std::strncpy(entry.name, u.name.c_str(), sizeof(entry.name) - 1);
        entry.register_offset = u.base_register;
        entry.size_in_words = u.register_count;
        entry.data_type = static_cast<uint32_t>(u.type.base);
        entry.semantic_index = 0;

        std::memcpy(sym_ptr + (i * sizeof(GxpSymbolEntry)), &entry, sizeof(GxpSymbolEntry));
    }

    // 4. Write Attribute Table
    uint8_t* attr_ptr = out_bytes.data() + attr_offset;
    for (size_t i = 0; i < program.inputs.size(); ++i) {
        const auto& in = program.inputs[i];
        GxpAttributeEntry entry{};
        std::strncpy(entry.name, in.name.c_str(), sizeof(entry.name) - 1);
        std::strncpy(entry.semantic, in.semantic.c_str(), sizeof(entry.semantic) - 1);
        entry.stream_index = static_cast<uint16_t>(i);
        entry.component_count = in.register_count;
        entry.register_index = in.base_register;
        entry.reserved = 0;

        std::memcpy(attr_ptr + (i * sizeof(GxpAttributeEntry)), &entry, sizeof(GxpAttributeEntry));
    }

    // 5. Write Microcode
    uint8_t* ucode_ptr = out_bytes.data() + ucode_offset;
    if (!program.microcode_words.empty()) {
        std::memcpy(ucode_ptr, program.microcode_words.data(), microcode_raw_size);
    }

    return true;
}

bool GxpEncoder::validate(const uint8_t* data, size_t size, std::string& out_error) {
    if (!data || size < sizeof(GxpHeader)) {
        out_error = "Binary too small for GXP header";
        return false;
    }

    const auto* header = reinterpret_cast<const GxpHeader*>(data);
    if (header->magic != GXP_MAGIC) {
        out_error = "Invalid magic bytes (expected 'GXP\\0')";
        return false;
    }

    if (header->total_size > size) {
        out_error = "Declared container size exceeds actual buffer size";
        return false;
    }

    if (header->microcode_offset + header->microcode_size > header->total_size) {
        out_error = "Microcode payload extends past container boundary";
        return false;
    }

    return true;
}

} // namespace open_shacc::packer
