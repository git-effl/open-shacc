/**
 * @file regalloc.cpp
 * @brief Implementation of the linear scan register allocator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "regalloc.hpp"

namespace open_shacc::backend {

RegisterAllocator::RegisterAllocator() {
    reset();
}

void RegisterAllocator::reset() {
    next_temporary_ = 0;
    next_constant_ = 0;
    next_input_ = 0;
    next_output_ = 0;
    next_sampler_ = 0;
    symbol_table_.clear();
    uniforms_.clear();
    samplers_.clear();
    inputs_.clear();
    outputs_.clear();
}

bool RegisterAllocator::allocate_uniform(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    if (type.is_sampler()) {
        return allocate_sampler(name, type, out_binding);
    }

    uint16_t words_per_elem = 1;
    if (type.is_matrix()) {
        size_t rows = type.matrix_rows();
        size_t cols = type.matrix_cols();
        words_per_elem = static_cast<uint16_t>(rows * cols);
        if (next_constant_ % 4 != 0) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 3) & ~3u);
        }
    } else {
        words_per_elem = static_cast<uint16_t>(type.component_count());
        if (words_per_elem == 0) words_per_elem = 1;
        if (words_per_elem == 4 && next_constant_ % 4 != 0) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 3) & ~3u);
        }
    }

    uint16_t array_elems = static_cast<uint16_t>(type.array_size > 1 ? type.array_size : 1);
    uint16_t words_needed = static_cast<uint16_t>(words_per_elem * array_elems);

    if (static_cast<uint32_t>(next_constant_) + words_needed > 1024u) {
        return false; // Exceeded default uniform buffer capacity
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::Constant;
    out_binding.base_register = next_constant_;
    out_binding.register_count = words_needed;
    out_binding.semantic = semantic;

    next_constant_ = static_cast<uint16_t>(next_constant_ + words_needed);

    symbol_table_[name] = out_binding;
    uniforms_.push_back(out_binding);
    return true;
}

bool RegisterAllocator::allocate_sampler(
    const std::string& name,
    const frontend::DataType& type,
    SymbolBinding& out_binding
) {
    uint8_t count = static_cast<uint8_t>(type.array_size > 1 ? type.array_size : 1);
    if (static_cast<int>(next_sampler_) + count > 16) {
        return false; // Exceeded texture sampler slots (s0..s15)
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::TextureSampler;
    out_binding.base_register = next_sampler_;
    out_binding.register_count = count;
    out_binding.semantic = "";

    next_sampler_ = static_cast<uint8_t>(next_sampler_ + count);

    symbol_table_[name] = out_binding;
    samplers_.push_back(out_binding);
    return true;
}

bool RegisterAllocator::allocate_input(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    uint16_t comp = static_cast<uint16_t>(type.component_count());
    if (comp == 0) comp = 1;
    uint16_t count = static_cast<uint16_t>(comp * (type.array_size > 1 ? type.array_size : 1));

    if (static_cast<uint16_t>(next_input_) + count > 64u) {
        return false; // Limit of 16 vec4 input attributes (64 scalar PA registers)
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::InputAttr;
    out_binding.base_register = next_input_;
    out_binding.register_count = count;
    out_binding.semantic = semantic;

    next_input_ = static_cast<uint8_t>(next_input_ + count);

    symbol_table_[name] = out_binding;
    inputs_.push_back(out_binding);
    return true;
}

bool RegisterAllocator::allocate_output(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    uint16_t comp = static_cast<uint16_t>(type.component_count());
    if (comp == 0) comp = 1;
    uint16_t count = static_cast<uint16_t>(comp * (type.array_size > 1 ? type.array_size : 1));

    if (static_cast<uint16_t>(next_output_) + count > 64u) {
        return false;
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::Output;
    out_binding.base_register = next_output_;
    out_binding.register_count = count;
    out_binding.semantic = semantic;

    next_output_ = static_cast<uint8_t>(next_output_ + count);

    symbol_table_[name] = out_binding;
    outputs_.push_back(out_binding);
    return true;
}

void RegisterAllocator::alias_symbol(const std::string& alias_name, const SymbolBinding& binding) {
    if (!alias_name.empty()) {
        symbol_table_[alias_name] = binding;
    }
}

HardwareRegister RegisterAllocator::allocate_temporary(uint8_t sub_index) {
    if (next_temporary_ >= 64) {
        next_temporary_ = 0;
    }
    HardwareRegister reg;
    reg.reg_class = RegisterClass::Temporary;
    reg.index = next_temporary_++;
    reg.sub_index = sub_index;
    return reg;
}

std::vector<HardwareRegister> RegisterAllocator::allocate_temporary_vector(uint8_t component_count) {
    std::vector<HardwareRegister> regs;
    regs.reserve(component_count);
    uint8_t base = next_temporary_;
    if (static_cast<int>(next_temporary_) + component_count > 64) {
        base = 0;
        next_temporary_ = component_count;
    } else {
        next_temporary_ = static_cast<uint8_t>(next_temporary_ + component_count);
    }

    for (uint8_t i = 0; i < component_count; ++i) {
        HardwareRegister r;
        r.reg_class = RegisterClass::Temporary;
        r.index = static_cast<uint8_t>(base + i);
        r.sub_index = static_cast<uint8_t>(i % 4);
        regs.push_back(r);
    }
    return regs;
}

void RegisterAllocator::free_temporary([[maybe_unused]] uint8_t reg_index) {
}

const SymbolBinding* RegisterAllocator::find_symbol(const std::string& name) const {
    auto it = symbol_table_.find(name);
    if (it != symbol_table_.end()) {
        return &it->second;
    }
    return nullptr;
}

} // namespace open_shacc::backend
