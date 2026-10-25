/**
 * @file regalloc.hpp
 * @brief Linear Scan Register Allocator for USSE virtual-to-physical register binding.
 *
 * SPDX-License-Identifier: MIT
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
    RegisterClass reg_class;
    uint8_t base_register = 0;
    uint8_t register_count = 1;
    std::string semantic;
};

class RegisterAllocator {
public:
    RegisterAllocator();

    void reset();

    // Allocate uniform constant registers (c0..c255)
    bool allocate_uniform(const std::string& name, const frontend::DataType& type, const std::string& semantic, SymbolBinding& out_binding);

    // Allocate input vertex attribute registers (v0..v15)
    bool allocate_input(const std::string& name, const frontend::DataType& type, const std::string& semantic, SymbolBinding& out_binding);

    // Allocate output semantic registers (o0..o15)
    bool allocate_output(const std::string& name, const frontend::DataType& type, const std::string& semantic, SymbolBinding& out_binding);

    // Allocate virtual temporary registers (r0..r63)
    HardwareRegister allocate_temporary(uint8_t sub_index = 0);
    std::vector<HardwareRegister> allocate_temporary_vector(uint8_t component_count);
    void free_temporary(uint8_t reg_index);

    // Lookup
    [[nodiscard]] const SymbolBinding* find_symbol(const std::string& name) const;
    [[nodiscard]] const std::vector<SymbolBinding>& get_uniforms() const { return uniforms_; }
    [[nodiscard]] const std::vector<SymbolBinding>& get_inputs() const { return inputs_; }
    [[nodiscard]] const std::vector<SymbolBinding>& get_outputs() const { return outputs_; }

    [[nodiscard]] uint8_t get_temporary_count() const { return next_temporary_; }
    [[nodiscard]] uint8_t get_constant_count() const { return next_constant_; }

private:
    uint8_t next_temporary_ = 0;
    uint8_t next_constant_ = 0;
    uint8_t next_input_ = 0;
    uint8_t next_output_ = 0;

    std::unordered_map<std::string, SymbolBinding> symbol_table_;
    std::vector<SymbolBinding> uniforms_;
    std::vector<SymbolBinding> inputs_;
    std::vector<SymbolBinding> outputs_;
};

} // namespace open_shacc::backend

#endif // OPEN_SHACC_REGALLOC_HPP
