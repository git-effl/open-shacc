/**
 * @file regalloc.hpp
 * @brief USSE Register Allocator with even-aligned vec4 temp pooling and GXM canonical layout.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef OPEN_SHACC_REGALLOC_HPP
#define OPEN_SHACC_REGALLOC_HPP

#include "usse_isa.hpp"
#include "../frontend/ast.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace open_shacc::backend {

struct SymbolBinding {
    std::string name;
    frontend::DataType type;
    RegisterClass reg_class = RegisterClass::Temporary;
    uint16_t base_register = 0;
    uint16_t register_count = 1;
    uint16_t allocated_words = 1; // Physical register word footprint (e.g. even-aligned for PA)
    std::string semantic;
    std::string hw_semantic;      // Canonical hardware semantic (e.g. TEXCOORD0 for fragment varyings)
};

class RegisterAllocator {
public:
    RegisterAllocator();

    void reset();

    // Allocate uniform constant registers (sa0..) or samplers (s0..s15)
    bool allocate_uniform(const std::string& name, const frontend::DataType& type, const std::string& semantic, SymbolBinding& out_binding);

    // Allocate texture samplers (s0..s15)
    bool allocate_sampler(const std::string& name, const frontend::DataType& type, SymbolBinding& out_binding);

    // Allocate vertex or fragment input attribute registers (pa0..pa63)
    bool allocate_input(const std::string& name, const frontend::DataType& type, const std::string& semantic, SymbolBinding& out_binding, bool is_fragment = false);

    // Allocate output semantic registers (o0..o63)
    bool allocate_output(const std::string& name, const frontend::DataType& type, const std::string& semantic, SymbolBinding& out_binding);

    // Reorder and re-assign vertex output registers in canonical SGX/GXM order:
    // POSITION(+4) -> COLOR0(+4) -> COLOR1(+4) -> FOG(+2) -> TEXCOORD0..9(+2/+3/+4) -> PSIZE(+1) -> CLIP0..7(+1)
    void finalize_canonical_vertex_outputs();

    // Register an alias without allocating a duplicate input/output
    void alias_symbol(const std::string& alias_name, const SymbolBinding& binding);
    void erase_symbol(const std::string& name);

    // Scoped temporary vec4 slot allocation (4-word aligned, up to r116, safely below FPINTERNAL r120)
    HardwareRegister allocate_temporary(uint8_t sub_index = 0);
    std::vector<HardwareRegister> allocate_temporary_vector(uint8_t component_count);
    uint8_t allocate_temp_vec4();
    uint8_t allocate_contiguous_temp_vec4s(uint8_t vec4_count);
    void free_temp_vec4(uint8_t base_reg);
    void free_temporary(uint8_t reg_index);

    // Lookup
    [[nodiscard]] const SymbolBinding* find_symbol(const std::string& name) const;
    [[nodiscard]] SymbolBinding* find_symbol_mut(const std::string& name);
    [[nodiscard]] const std::vector<SymbolBinding>& get_uniforms() const { return uniforms_; }
    [[nodiscard]] const std::vector<SymbolBinding>& get_samplers() const { return samplers_; }
    [[nodiscard]] const std::vector<SymbolBinding>& get_inputs() const { return inputs_; }
    [[nodiscard]] const std::vector<SymbolBinding>& get_outputs() const { return outputs_; }

    [[nodiscard]] uint8_t get_temporary_count() const { return peak_temporary_; }
    [[nodiscard]] uint16_t get_constant_count() const { return next_constant_; }
    [[nodiscard]] uint8_t get_sampler_count() const { return next_sampler_; }
    [[nodiscard]] bool has_temp_overflow() const { return temp_overflow_; }
    [[nodiscard]] uint16_t get_sampler_sa_base() const { return static_cast<uint16_t>((next_constant_ + 3u) & ~3u); }
    [[nodiscard]] uint16_t get_sampler_sa_offset(uint16_t sampler_unit) const {
        return static_cast<uint16_t>(get_sampler_sa_base() + sampler_unit * 4u);
    }

private:
    std::string assign_fragment_hw_semantic(const std::string& requested_sem);

    uint8_t next_temporary_ = 0;
    uint8_t peak_temporary_ = 0;
    uint16_t next_constant_ = 0;
    uint8_t next_input_ = 0;
    uint8_t next_output_ = 0;
    uint8_t next_sampler_ = 0;
    uint32_t used_frag_texcoords_ = 0;
    bool temp_overflow_ = false;

    std::vector<uint8_t> free_vec4_slots_;
    std::unordered_map<std::string, SymbolBinding> symbol_table_;
    std::vector<SymbolBinding> uniforms_;
    std::vector<SymbolBinding> samplers_;
    std::vector<SymbolBinding> inputs_;
    std::vector<SymbolBinding> outputs_;
};

} // namespace open_shacc::backend

#endif // OPEN_SHACC_REGALLOC_HPP
