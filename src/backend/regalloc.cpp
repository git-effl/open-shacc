/**
 * @file regalloc.cpp
 * @brief Implementation of the linear scan register allocator.
 *
 * SPDX-License-Identifier: MIT
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
    symbol_table_.clear();
    uniforms_.clear();
    inputs_.clear();
    outputs_.clear();
}

bool RegisterAllocator::allocate_uniform(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    uint8_t words_needed = 0;
    if (type.base == frontend::BaseType::Float4x4) {
        // float4x4 requires 16 float words (or 4 vec4 registers = 16 scalar registers)
        words_needed = 16;
    } else if (type.base == frontend::BaseType::Float4) {
        words_needed = 4;
    } else if (type.base == frontend::BaseType::Float3) {
        words_needed = 3;
    } else if (type.base == frontend::BaseType::Float2) {
        words_needed = 2;
    } else {
        words_needed = 1;
    }

    if (static_cast<int>(next_constant_) + words_needed > 256) {
        return false; // Exceeded constant register capacity (c0..c255)
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::Constant;
    out_binding.base_register = next_constant_;
    out_binding.register_count = words_needed;
    out_binding.semantic = semantic;

    next_constant_ += words_needed;

    symbol_table_[name] = out_binding;
    uniforms_.push_back(out_binding);
    return true;
}

bool RegisterAllocator::allocate_input(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    uint8_t count = static_cast<uint8_t>(type.component_count());
    if (count == 0) count = 1;

    if (next_input_ + count > 16) {
        return false; // Limit of 16 input streams / attribute channels
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::InputAttr;
    out_binding.base_register = next_input_;
    out_binding.register_count = count;
    out_binding.semantic = semantic;

    next_input_ += count;

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
    uint8_t count = static_cast<uint8_t>(type.component_count());
    if (count == 0) count = 1;

    if (next_output_ + count > 16) {
        return false;
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::Output;
    out_binding.base_register = next_output_;
    out_binding.register_count = count;
    out_binding.semantic = semantic;

    next_output_ += count;

    symbol_table_[name] = out_binding;
    outputs_.push_back(out_binding);
    return true;
}

HardwareRegister RegisterAllocator::allocate_temporary(uint8_t sub_index) {
    if (next_temporary_ >= 64) {
        // Wrap or fallback
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
    next_temporary_ += component_count;
    if (next_temporary_ >= 64) {
        base = 0;
        next_temporary_ = component_count;
    }

    for (uint8_t i = 0; i < component_count; ++i) {
        HardwareRegister r;
        r.reg_class = RegisterClass::Temporary;
        r.index = base + i;
        r.sub_index = i % 4;
        regs.push_back(r);
    }
    return regs;
}

void RegisterAllocator::free_temporary([[maybe_unused]] uint8_t reg_index) {
    // For linear scan allocator, monotonic bump with reuse after basic block
}

const SymbolBinding* RegisterAllocator::find_symbol(const std::string& name) const {
    auto it = symbol_table_.find(name);
    if (it != symbol_table_.end()) {
        return &it->second;
    }
    return nullptr;
}

} // namespace open_shacc::backend
