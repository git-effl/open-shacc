/**
 * @file usse_codegen.cpp
 * @brief Implementation of USSE Target Code Generator.
 *
 * Clean-room implementation based on open standard USSE / SGX instruction set architecture.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "usse_codegen.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace open_shacc::backend {

namespace {

uint8_t swizzle_char_to_sub(char c) {
    switch (c) {
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        case 'w': case 'a': return 3;
        default:            return 0;
    }
}

uint8_t swizzle_mask_bits(const std::string& swiz) {
    if (swiz.empty()) return 0xF;
    uint8_t mask = 0;
    for (char c : swiz) {
        mask |= static_cast<uint8_t>(1u << swizzle_char_to_sub(c));
    }
    return mask ? mask : 0xF;
}

bool semantic_contains(const std::string& sem, const char* needle) {
    if (sem.empty() || !needle) return false;
    std::string up = sem;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return up.find(needle) != std::string::npos;
}

} // namespace

UsseCodeGenerator::UsseCodeGenerator() = default;

void UsseCodeGenerator::emit(
    UsseOpcode op,
    HardwareRegister dst,
    HardwareRegister src0,
    HardwareRegister src1,
    uint8_t mask,
    const std::string& comment
) {
    UsseInstruction inst;
    inst.opcode = op;
    inst.dst = dst;
    inst.src0 = src0;
    inst.src1 = src1;
    inst.write_mask = mask;
    inst.comment = comment;
    instructions_.push_back(std::move(inst));
}

void UsseCodeGenerator::emit(
    UsseOpcode op,
    HardwareRegister dst,
    HardwareRegister src0,
    HardwareRegister src1,
    HardwareRegister src2,
    uint8_t mask,
    const std::string& comment
) {
    UsseInstruction inst;
    inst.opcode = op;
    inst.dst = dst;
    inst.src0 = src0;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.write_mask = mask;
    inst.comment = comment;
    instructions_.push_back(std::move(inst));
}

void UsseCodeGenerator::emit_phase_switch() {
    UsseInstruction inst;
    inst.opcode = UsseOpcode::PHAS;
    inst.comment = "End of shader phase";
    instructions_.push_back(std::move(inst));
}

bool UsseCodeGenerator::generate(
    const frontend::TranslationUnit& tu,
    const std::string& entry_name,
    std::optional<bool> force_fragment,
    CompiledProgram& out_program
) {
    tu_ = &tu;
    reg_allocator_.reset();
    instructions_.clear();
    return_struct_fields_.clear();
    error_message_.clear();
    uses_discard_ = false;

    // 1. Allocate uniform constant & sampler registers from top-level declarations
    for (const auto& u : tu.uniforms) {
        if (reg_allocator_.find_symbol(u.name)) continue;
        SymbolBinding binding;
        if (!reg_allocator_.allocate_uniform(u.name, u.type, u.semantic, binding)) {
            error_message_ = "Constant/sampler register overflow allocating uniform: " + u.name;
            return false;
        }
    }

    // Bind top-level constant/global variables
    for (const auto& g : tu.globals) {
        generate_var_decl(&g);
    }

    // 2. Find entry point
    const std::string target_entry = entry_name.empty() ? "main" : entry_name;
    const frontend::FunctionDecl* entry_fn = tu.find_entry_point(target_entry);
    if (!entry_fn && target_entry != "main") {
        entry_fn = tu.find_entry_point("main");
    }
    if (!entry_fn && !tu.functions.empty()) {
        entry_fn = &tu.functions.back();
    }

    if (!entry_fn) {
        error_message_ = "No entry point function '" + target_entry + "' found in translation unit";
        return false;
    }
    current_fn_ = entry_fn;

    // 3. Allocate parameters of entry function based on qualifiers (uniform, out, in)
    for (const auto& param : entry_fn->parameters) {
        if (param.is_uniform || param.type.is_sampler()) {
            if (param.type.base == frontend::BaseType::CustomStruct) {
                const auto* s = tu.find_struct(param.type.struct_name);
                if (s) {
                    for (const auto& field : s->fields) {
                        SymbolBinding binding;
                        if (!reg_allocator_.allocate_uniform(param.name + "." + field.name, field.type, field.semantic, binding)) {
                            error_message_ = "Uniform overflow allocating struct param: " + param.name + "." + field.name;
                            return false;
                        }
                    }
                }
            } else {
                SymbolBinding binding;
                if (!reg_allocator_.allocate_uniform(param.name, param.type, param.semantic, binding)) {
                    error_message_ = "Uniform overflow allocating parameter: " + param.name;
                    return false;
                }
            }
        } else if (param.is_out && !param.is_in) {
            if (param.type.base == frontend::BaseType::CustomStruct) {
                const auto* s = tu.find_struct(param.type.struct_name);
                if (s) {
                    for (const auto& field : s->fields) {
                        SymbolBinding binding;
                        std::string qualified = param.name + "." + field.name;
                        reg_allocator_.allocate_output(qualified, field.type, field.semantic, binding);
                        if (!reg_allocator_.find_symbol(field.name)) {
                            reg_allocator_.alias_symbol(field.name, binding);
                        }
                    }
                }
            } else {
                SymbolBinding binding;
                reg_allocator_.allocate_output(param.name, param.type, param.semantic, binding);
            }
        } else {
            if (param.type.base == frontend::BaseType::CustomStruct) {
                const auto* s = tu.find_struct(param.type.struct_name);
                if (s) {
                    for (const auto& field : s->fields) {
                        SymbolBinding binding;
                        std::string qualified = param.name + "." + field.name;
                        reg_allocator_.allocate_input(qualified, field.type, field.semantic, binding);
                        if (!reg_allocator_.find_symbol(field.name)) {
                            reg_allocator_.alias_symbol(field.name, binding);
                        }
                    }
                }
            } else {
                SymbolBinding binding;
                reg_allocator_.allocate_input(param.name, param.type, param.semantic, binding);
            }
        }
    }

    // 4. Determine shader stage
    bool is_frag = false;
    if (force_fragment.has_value()) {
        is_frag = *force_fragment;
    } else {
        bool has_position_out = semantic_contains(entry_fn->return_semantic, "POSITION");
        for (const auto& out : reg_allocator_.get_outputs()) {
            if (semantic_contains(out.semantic, "POSITION")) {
                has_position_out = true;
            }
        }
        if (entry_fn->return_type.base == frontend::BaseType::CustomStruct) {
            const auto* s = tu.find_struct(entry_fn->return_type.struct_name);
            if (s) {
                for (const auto& field : s->fields) {
                    if (semantic_contains(field.semantic, "POSITION")) {
                        has_position_out = true;
                    }
                }
            }
        }
        if (semantic_contains(entry_fn->return_semantic, "COLOR") ||
            semantic_contains(entry_fn->return_semantic, "FRAG") ||
            semantic_contains(entry_fn->return_semantic, "DEPTH") ||
            (!has_position_out && !reg_allocator_.get_samplers().empty())) {
            is_frag = true;
        }
    }

    // 5. Allocate outputs from return type
    if (entry_fn->return_type.base == frontend::BaseType::CustomStruct) {
        const auto* s = tu.find_struct(entry_fn->return_type.struct_name);
        if (s) {
            for (const auto& field : s->fields) {
                SymbolBinding binding;
                reg_allocator_.allocate_output(field.name, field.type, field.semantic, binding);
                return_struct_fields_.push_back(binding);
            }
        }
    } else if (entry_fn->return_type.base != frontend::BaseType::Void) {
        SymbolBinding binding;
        std::string out_name = is_frag ? "gl_FragColor" : "gl_Position";
        std::string out_sem = entry_fn->return_semantic.empty() ? (is_frag ? "COLOR" : "POSITION") : entry_fn->return_semantic;
        reg_allocator_.allocate_output(out_name, entry_fn->return_type, out_sem, binding);
    }

    // 6. Generate function body
    if (entry_fn->body) {
        generate_block(entry_fn->body.get());
    }

    if (!force_fragment.has_value()) {
        bool has_position_out = semantic_contains(entry_fn->return_semantic, "POSITION");
        bool has_color_or_depth_out = false;
        for (const auto& out : reg_allocator_.get_outputs()) {
            if (semantic_contains(out.semantic, "POSITION")) {
                has_position_out = true;
            }
            if (semantic_contains(out.semantic, "COLOR") || semantic_contains(out.semantic, "DEPTH")) {
                has_color_or_depth_out = true;
            }
        }
        if (uses_discard_ || (!has_position_out && has_color_or_depth_out)) {
            is_frag = true;
        }
    }

    // 7. Terminate shader microcode cleanly with a single phase switch (PHAS with end=true)
    emit_phase_switch();

    // 8. Assemble 64-bit binary microcode words & disassembly
    out_program.instructions = instructions_;
    out_program.uniforms = reg_allocator_.get_uniforms();
    out_program.samplers = reg_allocator_.get_samplers();
    out_program.inputs = reg_allocator_.get_inputs();
    out_program.outputs = reg_allocator_.get_outputs();
    out_program.temp_register_count = std::max<uint8_t>(1, reg_allocator_.get_temporary_count());
    out_program.const_register_count = reg_allocator_.get_constant_count();

    uint8_t total_pa = 0;
    for (const auto& in : out_program.inputs) {
        uint8_t end_reg = static_cast<uint8_t>(in.base_register + in.register_count);
        if (end_reg > total_pa) total_pa = end_reg;
    }
    out_program.input_register_count = std::max<uint8_t>(1, total_pa);
    out_program.is_vertex_shader = !is_frag;
    out_program.uses_discard = uses_discard_;

    std::ostringstream disasm;
    disasm << "; ================================================================\n";
    disasm << "; open-shacc USSE Disassembly (" << (out_program.is_vertex_shader ? "Vertex" : "Fragment") << ")\n";
    disasm << "; Clean-room architecture target: PowerVR SGX / USSE\n";
    disasm << "; ================================================================\n\n";

    disasm << "; --- Uniform Constant Mapping ---\n";
    for (const auto& u : out_program.uniforms) {
        disasm << ";   " << u.name << " -> sa" << u.base_register
               << " (size: " << u.register_count << " words)\n";
    }

    disasm << "\n; --- Texture Samplers ---\n";
    for (const auto& s : out_program.samplers) {
        disasm << ";   " << s.name << " -> s" << s.base_register << "\n";
    }

    disasm << "\n; --- Attributes / Inputs ---\n";
    for (const auto& in : out_program.inputs) {
        disasm << ";   " << in.name << " (" << in.semantic << ") -> pa" << in.base_register << "\n";
    }

    disasm << "\n; --- Outputs ---\n";
    for (const auto& out : out_program.outputs) {
        disasm << ";   " << out.name << " (" << out.semantic << ") -> o" << out.base_register << "\n";
    }

    disasm << "\n; --- Program Instructions ---\n";
    out_program.microcode_words.clear();
    out_program.microcode_words.reserve(instructions_.size());

    for (size_t i = 0; i < instructions_.size(); ++i) {
        const auto& inst = instructions_[i];
        MicrocodeWord word = inst.to_microcode();
        out_program.microcode_words.push_back(word.raw_word);

        std::ostringstream hex_word;
        hex_word << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << word.raw_word;

        disasm << std::left << std::setw(6) << i << ": [" << hex_word.str() << "]  " << inst.disassemble() << "\n";
    }

    out_program.disassembly = disasm.str();
    return true;
}

void UsseCodeGenerator::generate_statement(const frontend::Statement* stmt) {
    if (!stmt) return;
    if (auto* b = dynamic_cast<const frontend::BlockStmt*>(stmt)) {
        generate_block(b);
    } else if (auto* v = dynamic_cast<const frontend::VarDeclStmt*>(stmt)) {
        generate_var_decl(v);
    } else if (auto* a = dynamic_cast<const frontend::AssignStmt*>(stmt)) {
        generate_assign(a);
    } else if (auto* r = dynamic_cast<const frontend::ReturnStmt*>(stmt)) {
        generate_return(r);
    } else if (auto* i = dynamic_cast<const frontend::IfStmt*>(stmt)) {
        generate_if(i);
    } else if (auto* f = dynamic_cast<const frontend::ForStmt*>(stmt)) {
        generate_for(f);
    } else if (auto* w = dynamic_cast<const frontend::WhileStmt*>(stmt)) {
        generate_while(w);
    } else if (auto* dw = dynamic_cast<const frontend::DoWhileStmt*>(stmt)) {
        generate_do_while(dw);
    } else if (auto* d = dynamic_cast<const frontend::DiscardStmt*>(stmt)) {
        generate_discard(d);
    }
}

void UsseCodeGenerator::generate_block(const frontend::BlockStmt* block) {
    if (!block) return;
    for (const auto& s : block->statements) {
        generate_statement(s.get());
    }
}

void UsseCodeGenerator::generate_var_decl(const frontend::VarDeclStmt* decl) {
    if (!decl) return;

    // If local struct matches the entry function return struct, alias its fields to Output registers
    if (decl->type.base == frontend::BaseType::CustomStruct && current_fn_ &&
        current_fn_->return_type.base == frontend::BaseType::CustomStruct &&
        decl->type.struct_name == current_fn_->return_type.struct_name) {
        for (const auto& field : return_struct_fields_) {
            reg_allocator_.alias_symbol(decl->name + "." + field.name, field);
        }
        SymbolBinding whole{};
        whole.name = decl->name;
        whole.type = decl->type;
        whole.reg_class = RegisterClass::Output;
        whole.base_register = 0;
        whole.register_count = 4;
        reg_allocator_.alias_symbol(decl->name, whole);
        return;
    }

    size_t components = decl->type.component_count();
    if (components == 0) components = 1;

    if (components == 1) {
        HardwareRegister var_reg = reg_allocator_.allocate_temporary();
        SymbolBinding local_bind{};
        local_bind.name = decl->name;
        local_bind.type = decl->type;
        local_bind.reg_class = RegisterClass::Temporary;
        local_bind.base_register = var_reg.index;
        local_bind.register_count = 1;
        local_bind.semantic = decl->semantic;
        reg_allocator_.alias_symbol(decl->name, local_bind);

        if (decl->initializer) {
            HardwareRegister r = generate_expression(decl->initializer.get());
            emit(UsseOpcode::VMOV, var_reg, r, HardwareRegister{}, 0xF, "Declare " + decl->name);
        }
    } else {
        auto var_regs = reg_allocator_.allocate_temporary_vector(static_cast<uint8_t>(components));
        SymbolBinding local_bind{};
        local_bind.name = decl->name;
        local_bind.type = decl->type;
        local_bind.reg_class = RegisterClass::Temporary;
        local_bind.base_register = var_regs.empty() ? 0 : var_regs[0].index;
        local_bind.register_count = static_cast<uint16_t>(components);
        local_bind.semantic = decl->semantic;
        reg_allocator_.alias_symbol(decl->name, local_bind);

        if (decl->initializer) {
            auto regs = generate_vector_expression(decl->initializer.get(), components);
            for (size_t i = 0; i < components && i < var_regs.size() && i < regs.size(); ++i) {
                emit(UsseOpcode::VMOV, var_regs[i], regs[i], HardwareRegister{}, 0xF,
                     decl->name + "[" + std::to_string(i) + "]");
            }
        }
    }
}

void UsseCodeGenerator::generate_assign(const frontend::AssignStmt* assign) {
    if (!assign || !assign->lvalue) return;

    // Standalone expression statement (e.g. foo(); or i++;)
    if (!assign->rvalue) {
        generate_expression(assign->lvalue.get());
        return;
    }

    auto resolve_op = [](frontend::BinaryOp compound) -> std::optional<UsseOpcode> {
        switch (compound) {
            case frontend::BinaryOp::Add: return UsseOpcode::VADD;
            case frontend::BinaryOp::Sub: return UsseOpcode::VSUB;
            case frontend::BinaryOp::Mul: return UsseOpcode::VMUL;
            case frontend::BinaryOp::Div: return UsseOpcode::VMUL;
            default: return std::nullopt;
        }
    };
    auto compound_opcode = resolve_op(assign->compound_op);

    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(assign->lvalue.get())) {
        const auto* sym = reg_allocator_.find_symbol(var->name);
        size_t comp_count = sym ? std::max<size_t>(1, sym->register_count) : 4;
        auto rhs_regs = generate_vector_expression(assign->rvalue.get(), comp_count);
        for (size_t i = 0; i < comp_count && i < rhs_regs.size(); ++i) {
            HardwareRegister dst;
            dst.reg_class = sym ? sym->reg_class : RegisterClass::Temporary;
            dst.index = static_cast<uint8_t>((sym ? sym->base_register : 0) + i);
            dst.sub_index = static_cast<uint8_t>(i % 4);
            if (compound_opcode) {
                emit(*compound_opcode, dst, dst, rhs_regs[i], 0xF, "Compound assign to " + var->name);
            } else {
                emit(UsseOpcode::VMOV, dst, rhs_regs[i], HardwareRegister{}, 0xF, "Assign to " + var->name);
            }
        }
    } else if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(assign->lvalue.get())) {
        // Check struct member write: e.g. output.position = ...
        if (auto* target_var = dynamic_cast<const frontend::VariableExpr*>(swiz->target.get())) {
            std::string full_name = target_var->name + "." + swiz->components;
            const auto* sym = reg_allocator_.find_symbol(full_name);
            if (!sym) {
                // Only fall back to bare struct field if target is a known struct output/variable
                const auto* base_sym = reg_allocator_.find_symbol(target_var->name);
                if (!base_sym || base_sym->type.base == frontend::BaseType::CustomStruct) {
                    sym = reg_allocator_.find_symbol(swiz->components);
                }
            }

            if (sym) {
                size_t comp_count = std::max<size_t>(1, sym->register_count);
                auto rhs_regs = generate_vector_expression(assign->rvalue.get(), comp_count);
                for (size_t i = 0; i < comp_count && i < rhs_regs.size(); ++i) {
                    HardwareRegister dst;
                    dst.reg_class = sym->reg_class;
                    dst.index = static_cast<uint8_t>(sym->base_register + i);
                    dst.sub_index = static_cast<uint8_t>(i % 4);
                    if (compound_opcode) {
                        emit(*compound_opcode, dst, dst, rhs_regs[i], 0xF, full_name + " compound write");
                    } else {
                        emit(UsseOpcode::VMOV, dst, rhs_regs[i], HardwareRegister{}, 0xF, full_name + " write");
                    }
                }
                return;
            }

            // Component swizzle write on vector variable (e.g. col.rgb = ...; pos.w = 1.0;)
            if (const auto* base_sym = reg_allocator_.find_symbol(target_var->name)) {
                size_t n = std::max<size_t>(1, swiz->components.size());
                auto rhs_regs = generate_vector_expression(assign->rvalue.get(), n);
                uint8_t mask = swizzle_mask_bits(swiz->components);
                for (size_t i = 0; i < n && i < rhs_regs.size(); ++i) {
                    uint8_t comp_idx = swizzle_char_to_sub(swiz->components[i]);
                    HardwareRegister dst;
                    dst.reg_class = base_sym->reg_class;
                    dst.index = static_cast<uint8_t>(base_sym->base_register + comp_idx);
                    dst.sub_index = comp_idx;
                    if (compound_opcode) {
                        emit(*compound_opcode, dst, dst, rhs_regs[i], mask, target_var->name + "." + swiz->components);
                    } else {
                        emit(UsseOpcode::VMOV, dst, rhs_regs[i], HardwareRegister{}, mask, target_var->name + "." + swiz->components);
                    }
                }
                return;
            }
        }

        // Chained swizzle e.g. output.color.rgb = ...
        HardwareRegister dst = generate_expression(swiz->target.get());
        size_t n = std::max<size_t>(1, swiz->components.size());
        auto rhs_regs = generate_vector_expression(assign->rvalue.get(), n);
        uint8_t mask = swizzle_mask_bits(swiz->components);
        for (size_t i = 0; i < n && i < rhs_regs.size(); ++i) {
            HardwareRegister comp_dst = dst;
            comp_dst.sub_index = swizzle_char_to_sub(swiz->components[i]);
            if (compound_opcode) {
                emit(*compound_opcode, comp_dst, comp_dst, rhs_regs[i], mask, "Chained swizzle compound write");
            } else {
                emit(UsseOpcode::VMOV, comp_dst, rhs_regs[i], HardwareRegister{}, mask, "Chained swizzle write");
            }
        }
    } else if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(assign->lvalue.get())) {
        HardwareRegister dst = generate_array_access(arr);
        HardwareRegister rhs = generate_expression(assign->rvalue.get());
        if (compound_opcode) {
            emit(*compound_opcode, dst, dst, rhs, 0xF, "Array compound assign");
        } else {
            emit(UsseOpcode::VMOV, dst, rhs, HardwareRegister{}, 0xF, "Array assign");
        }
    }
}

void UsseCodeGenerator::generate_return(const frontend::ReturnStmt* ret) {
    if (!ret || !ret->value) return;

    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(ret->value.get())) {
        const auto* sym = reg_allocator_.find_symbol(var->name);
        if (sym && sym->reg_class == RegisterClass::Output) {
            // Struct fields were already written directly to output registers
            return;
        }
    }

    size_t out_comps = 4;
    if (current_fn_ && current_fn_->return_type.base != frontend::BaseType::CustomStruct) {
        out_comps = std::max<size_t>(1, current_fn_->return_type.component_count());
    }

    auto result_regs = generate_vector_expression(ret->value.get(), out_comps);
    uint8_t base_out = 0;
    if (!reg_allocator_.get_outputs().empty()) {
        base_out = static_cast<uint8_t>(reg_allocator_.get_outputs().back().base_register);
    }

    for (size_t i = 0; i < result_regs.size() && i < out_comps; ++i) {
        HardwareRegister dst;
        dst.reg_class = RegisterClass::Output;
        dst.index = static_cast<uint8_t>(base_out + i);
        dst.sub_index = static_cast<uint8_t>(i % 4);
        emit(UsseOpcode::VMOV, dst, result_regs[i], HardwareRegister{}, 0xF, "Shader output o" + std::to_string(dst.index));
    }
}

std::vector<HardwareRegister> UsseCodeGenerator::generate_matrix_vector_mul(
    const std::string& matrix_name,
    const std::vector<HardwareRegister>& vec
) {
    std::vector<HardwareRegister> out_regs;
    out_regs.reserve(4);

    const auto* sym = reg_allocator_.find_symbol(matrix_name);
    uint16_t base_c = sym ? sym->base_register : 0;

    for (int row = 0; row < 4; ++row) {
        HardwareRegister dst = reg_allocator_.allocate_temporary(static_cast<uint8_t>(row));
        HardwareRegister m_row;
        m_row.reg_class = RegisterClass::Constant;
        m_row.index = static_cast<uint8_t>((base_c + static_cast<uint16_t>(row * 4)) & 0xFF);
        m_row.sub_index = 0;

        HardwareRegister v_in = vec.empty() ? reg_allocator_.allocate_temporary() : vec[0];

        emit(UsseOpcode::VDP4, dst, m_row, v_in, 0xF, "mul(" + matrix_name + " row " + std::to_string(row) + ", v)");
        out_regs.push_back(dst);
    }

    return out_regs;
}

HardwareRegister UsseCodeGenerator::generate_expression(const frontend::Expression* expr) {
    if (!expr) return reg_allocator_.allocate_temporary();

    if (auto* lit = dynamic_cast<const frontend::LiteralExpr*>(expr)) {
        return generate_literal(lit);
    }
    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(expr)) {
        return generate_variable(var);
    }
    if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(expr)) {
        return generate_swizzle(swiz);
    }
    if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(expr)) {
        return generate_array_access(arr);
    }
    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(expr)) {
        return generate_binary_op(bin);
    }
    if (auto* un = dynamic_cast<const frontend::UnaryExpr*>(expr)) {
        return generate_unary_op(un);
    }
    if (auto* tern = dynamic_cast<const frontend::TernaryExpr*>(expr)) {
        return generate_ternary(tern);
    }
    if (auto* call = dynamic_cast<const frontend::CallExpr*>(expr)) {
        return generate_call(call);
    }
    if (auto* construct = dynamic_cast<const frontend::ConstructExpr*>(expr)) {
        auto vec = generate_vector_expression(construct, std::max<size_t>(1, construct->target_type.component_count()));
        return vec.empty() ? reg_allocator_.allocate_temporary() : vec[0];
    }

    return reg_allocator_.allocate_temporary();
}

std::vector<HardwareRegister> UsseCodeGenerator::generate_vector_expression(
    const frontend::Expression* expr,
    size_t expected_components
) {
    std::vector<HardwareRegister> results;
    if (!expr) {
        for (size_t i = 0; i < expected_components; ++i) {
            results.push_back(reg_allocator_.allocate_temporary(static_cast<uint8_t>(i % 4)));
        }
        return results;
    }

    if (auto* call = dynamic_cast<const frontend::CallExpr*>(expr)) {
        if (call->callee == "mul" && call->arguments.size() == 2) {
            if (auto* mat_var = dynamic_cast<const frontend::VariableExpr*>(call->arguments[0].get())) {
                auto vec_regs = generate_vector_expression(call->arguments[1].get(), 4);
                return generate_matrix_vector_mul(mat_var->name, vec_regs);
            }
            if (auto* vec_var = dynamic_cast<const frontend::VariableExpr*>(call->arguments[1].get())) {
                const auto* sym = reg_allocator_.find_symbol(vec_var->name);
                if (sym && sym->type.is_matrix()) {
                    auto vec_regs = generate_vector_expression(call->arguments[0].get(), 4);
                    return generate_matrix_vector_mul(vec_var->name, vec_regs);
                }
            }
        }
    }

    if (auto* construct = dynamic_cast<const frontend::ConstructExpr*>(expr)) {
        for (const auto& arg : construct->arguments) {
            results.push_back(generate_expression(arg.get()));
        }
        while (results.size() < expected_components) {
            results.push_back(results.empty() ? reg_allocator_.allocate_temporary() : results.back());
        }
        return results;
    }

    if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(expr)) {
        // Check struct field access first (e.g., input.position)
        if (auto* base_var = dynamic_cast<const frontend::VariableExpr*>(swiz->target.get())) {
            if (const auto* sym = reg_allocator_.find_symbol(base_var->name + "." + swiz->components)) {
                for (size_t i = 0; i < expected_components; ++i) {
                    HardwareRegister r;
                    r.reg_class = sym->reg_class;
                    r.index = static_cast<uint8_t>(sym->base_register + (i < sym->register_count ? i : 0));
                    r.sub_index = static_cast<uint8_t>(i % 4);
                    results.push_back(r);
                }
                return results;
            }
        }

        // Vector component swizzle (e.g., v.xyz or v.rgba)
        HardwareRegister base = generate_expression(swiz->target.get());
        for (char c : swiz->components) {
            HardwareRegister r = base;
            uint8_t sub_idx = swizzle_char_to_sub(c);
            r.index = static_cast<uint8_t>(base.index + sub_idx);
            r.sub_index = sub_idx;
            results.push_back(r);
        }
        while (results.size() < expected_components) {
            results.push_back(results.empty() ? base : results.back());
        }
        return results;
    }

    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(expr)) {
        auto left_regs = generate_vector_expression(bin->left.get(), expected_components);
        auto right_regs = generate_vector_expression(bin->right.get(), expected_components);

        UsseOpcode op = UsseOpcode::VADD;
        if (bin->op == frontend::BinaryOp::Sub) op = UsseOpcode::VSUB;
        else if (bin->op == frontend::BinaryOp::Mul) op = UsseOpcode::VMUL;

        for (size_t i = 0; i < expected_components; ++i) {
            HardwareRegister dst = reg_allocator_.allocate_temporary(static_cast<uint8_t>(i % 4));
            HardwareRegister l = i < left_regs.size() ? left_regs[i] : left_regs.front();
            HardwareRegister r = i < right_regs.size() ? right_regs[i] : right_regs.front();
            emit(op, dst, l, r, 0xF, "vector component " + std::to_string(i));
            results.push_back(dst);
        }
        return results;
    }

    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(expr)) {
        const auto* sym = reg_allocator_.find_symbol(var->name);
        if (sym && sym->register_count > 1) {
            for (size_t i = 0; i < expected_components; ++i) {
                HardwareRegister r;
                r.reg_class = sym->reg_class;
                r.index = static_cast<uint8_t>(sym->base_register + (i < sym->register_count ? i : 0));
                r.sub_index = static_cast<uint8_t>(i % 4);
                results.push_back(r);
            }
            return results;
        }
    }

    // Scalar fallback
    HardwareRegister single = generate_expression(expr);
    for (size_t i = 0; i < expected_components; ++i) {
        HardwareRegister comp = single;
        comp.sub_index = static_cast<uint8_t>(i % 4);
        results.push_back(comp);
    }
    return results;
}

HardwareRegister UsseCodeGenerator::generate_binary_op(const frontend::BinaryExpr* bin) {
    if (bin->op == frontend::BinaryOp::Assign) {
        auto stmt = std::make_unique<frontend::AssignStmt>();
        // Evaluate without consuming ownership
        HardwareRegister right = generate_expression(bin->right.get());
        if (auto* var = dynamic_cast<const frontend::VariableExpr*>(bin->left.get())) {
            const auto* sym = reg_allocator_.find_symbol(var->name);
            if (sym) {
                HardwareRegister dst;
                dst.reg_class = sym->reg_class;
                dst.index = static_cast<uint8_t>(sym->base_register & 0xFF);
                emit(UsseOpcode::VMOV, dst, right, HardwareRegister{}, 0xF, "Assign expr");
                return dst;
            }
        }
        return right;
    }

    HardwareRegister left = generate_expression(bin->left.get());
    HardwareRegister right = generate_expression(bin->right.get());
    HardwareRegister dst = reg_allocator_.allocate_temporary();

    UsseOpcode op = UsseOpcode::VADD;
    switch (bin->op) {
        case frontend::BinaryOp::Add: op = UsseOpcode::VADD; break;
        case frontend::BinaryOp::Sub: op = UsseOpcode::VSUB; break;
        case frontend::BinaryOp::Mul: op = UsseOpcode::VMUL; break;
        case frontend::BinaryOp::Div: op = UsseOpcode::VMUL; break;
        default: op = UsseOpcode::VADD; break;
    }

    emit(op, dst, left, right);
    return dst;
}

HardwareRegister UsseCodeGenerator::generate_unary_op(const frontend::UnaryExpr* un) {
    HardwareRegister opnd = generate_expression(un->operand.get());
    HardwareRegister dst = reg_allocator_.allocate_temporary();

    if (un->op == frontend::UnaryOp::Negate) {
        opnd.is_negated = !opnd.is_negated;
        emit(UsseOpcode::VMOV, dst, opnd, HardwareRegister{}, 0xF, "Negate");
    } else {
        emit(UsseOpcode::VMOV, dst, opnd, HardwareRegister{}, 0xF);
    }

    return dst;
}

HardwareRegister UsseCodeGenerator::generate_ternary(const frontend::TernaryExpr* tern) {
    [[maybe_unused]] HardwareRegister cond = generate_expression(tern->condition.get());
    HardwareRegister t_val = generate_expression(tern->then_expr.get());
    [[maybe_unused]] HardwareRegister e_val = generate_expression(tern->else_expr.get());
    HardwareRegister dst = reg_allocator_.allocate_temporary();
    emit(UsseOpcode::VMOV, dst, t_val, HardwareRegister{}, 0xF, "Ternary select");
    return dst;
}

HardwareRegister UsseCodeGenerator::generate_array_access(const frontend::ArrayAccessExpr* arr) {
    HardwareRegister base = generate_expression(arr->target.get());
    uint8_t offset = 0;
    if (auto* lit = dynamic_cast<const frontend::LiteralExpr*>(arr->index.get())) {
        if (std::holds_alternative<float>(lit->value)) {
            offset = static_cast<uint8_t>(std::get<float>(lit->value));
        } else if (std::holds_alternative<int>(lit->value)) {
            offset = static_cast<uint8_t>(std::get<int>(lit->value));
        }
    } else {
        generate_expression(arr->index.get());
    }
    base.index = static_cast<uint8_t>(base.index + offset);
    return base;
}

HardwareRegister UsseCodeGenerator::generate_call(const frontend::CallExpr* call) {
    HardwareRegister dst = reg_allocator_.allocate_temporary();

    if (call->callee == "tex2D" || call->callee == "tex2Dbias" ||
        call->callee == "tex2Dlod" || call->callee == "tex2Dproj" ||
        call->callee == "tex1D" || call->callee == "tex3D" ||
        call->callee == "texCUBE") {
        if (call->arguments.size() >= 2) {
            HardwareRegister samp_reg;
            samp_reg.reg_class = RegisterClass::TextureSampler;
            if (auto* var = dynamic_cast<const frontend::VariableExpr*>(call->arguments[0].get())) {
                const auto* sym = reg_allocator_.find_symbol(var->name);
                if (sym) {
                    samp_reg.reg_class = sym->reg_class;
                    samp_reg.index = static_cast<uint8_t>(sym->base_register & 0xFF);
                }
            } else if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(call->arguments[0].get())) {
                samp_reg = generate_array_access(arr);
            }
            HardwareRegister uv_reg = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::SMP, dst, samp_reg, uv_reg, 0xF, "Texture sample: " + call->callee);
        }
    } else if (call->callee == "dot" || call->callee == "dp4") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::VDP4, dst, a, b, 0xF, "dot4");
        }
    } else if (call->callee == "dp3") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::VDP3, dst, a, b, 0xF, "dot3");
        }
    } else if (call->callee == "max") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::VMAX, dst, a, b, 0xF);
        }
    } else if (call->callee == "min") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::VMIN, dst, a, b, 0xF);
        }
    } else if (call->callee == "clamp" && call->arguments.size() >= 3) {
        HardwareRegister x = generate_expression(call->arguments[0].get());
        HardwareRegister lo = generate_expression(call->arguments[1].get());
        HardwareRegister hi = generate_expression(call->arguments[2].get());
        HardwareRegister tmp = reg_allocator_.allocate_temporary();
        emit(UsseOpcode::VMAX, tmp, x, lo, 0xF, "clamp min");
        emit(UsseOpcode::VMIN, dst, tmp, hi, 0xF, "clamp max");
    } else if (call->callee == "saturate" && !call->arguments.empty()) {
        HardwareRegister x = generate_expression(call->arguments[0].get());
        emit(UsseOpcode::VMOV, dst, x, HardwareRegister{}, 0xF, "saturate");
    } else if ((call->callee == "lerp" || call->callee == "mix") && call->arguments.size() >= 3) {
        HardwareRegister a = generate_expression(call->arguments[0].get());
        HardwareRegister b = generate_expression(call->arguments[1].get());
        HardwareRegister t = generate_expression(call->arguments[2].get());
        HardwareRegister diff = reg_allocator_.allocate_temporary();
        HardwareRegister scaled = reg_allocator_.allocate_temporary();
        emit(UsseOpcode::VSUB, diff, b, a, 0xF, "lerp diff");
        emit(UsseOpcode::VMUL, scaled, diff, t, 0xF, "lerp scale");
        emit(UsseOpcode::VADD, dst, a, scaled, 0xF, "lerp add");
    } else if (call->callee == "normalize" && !call->arguments.empty()) {
        HardwareRegister a = generate_expression(call->arguments[0].get());
        HardwareRegister dp = reg_allocator_.allocate_temporary();
        emit(UsseOpcode::VDP3, dp, a, a, 0xF, "normalize dot");
        emit(UsseOpcode::VMUL, dst, a, dp, 0xF, "normalize scale");
    } else if (call->callee == "ddx" && !call->arguments.empty()) {
        HardwareRegister a = generate_expression(call->arguments[0].get());
        emit(UsseOpcode::VDSX, dst, a, a, 0xF, "ddx");
    } else if (call->callee == "ddy" && !call->arguments.empty()) {
        HardwareRegister a = generate_expression(call->arguments[0].get());
        emit(UsseOpcode::VDSY, dst, a, a, 0xF, "ddy");
    } else if ((call->callee == "frac" || call->callee == "fract") && !call->arguments.empty()) {
        HardwareRegister a = generate_expression(call->arguments[0].get());
        emit(UsseOpcode::VFRC, dst, a, a, 0xF, "frac");
    } else {
        // Evaluate arguments & look up user-defined helper functions if any
        HardwareRegister first_arg;
        bool has_arg = false;
        for (const auto& arg : call->arguments) {
            HardwareRegister r = generate_expression(arg.get());
            if (!has_arg) {
                first_arg = r;
                has_arg = true;
            }
        }
        if (has_arg) {
            emit(UsseOpcode::VMOV, dst, first_arg, HardwareRegister{}, 0xF, "Call " + call->callee);
        }
    }

    return dst;
}

void UsseCodeGenerator::generate_if(const frontend::IfStmt* if_stmt) {
    if (if_stmt && if_stmt->condition) {
        [[maybe_unused]] HardwareRegister cond = generate_expression(if_stmt->condition.get());
        if (if_stmt->then_branch) {
            generate_statement(if_stmt->then_branch.get());
        }
        if (if_stmt->else_branch) {
            generate_statement(if_stmt->else_branch.get());
        }
    }
}

void UsseCodeGenerator::generate_for(const frontend::ForStmt* for_stmt) {
    if (for_stmt) {
        if (for_stmt->init) generate_statement(for_stmt->init.get());
        if (for_stmt->condition) {
            [[maybe_unused]] HardwareRegister cond = generate_expression(for_stmt->condition.get());
        }
        if (for_stmt->body) generate_statement(for_stmt->body.get());
        if (for_stmt->increment) generate_statement(for_stmt->increment.get());
    }
}

void UsseCodeGenerator::generate_while(const frontend::WhileStmt* while_stmt) {
    if (while_stmt) {
        if (while_stmt->condition) {
            [[maybe_unused]] HardwareRegister cond = generate_expression(while_stmt->condition.get());
        }
        if (while_stmt->body) generate_statement(while_stmt->body.get());
    }
}

void UsseCodeGenerator::generate_do_while(const frontend::DoWhileStmt* dw_stmt) {
    if (dw_stmt) {
        if (dw_stmt->body) generate_statement(dw_stmt->body.get());
        if (dw_stmt->condition) {
            [[maybe_unused]] HardwareRegister cond = generate_expression(dw_stmt->condition.get());
        }
    }
}

void UsseCodeGenerator::generate_discard(const frontend::DiscardStmt*) {
    uses_discard_ = true;
    UsseInstruction inst;
    inst.opcode = UsseOpcode::KILL;
    inst.comment = "discard / pixel kill";
    instructions_.push_back(std::move(inst));
}

HardwareRegister UsseCodeGenerator::generate_swizzle(const frontend::SwizzleExpr* swiz) {
    // Check struct member lookup (e.g., input.position, input.uv)
    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(swiz->target.get())) {
        if (const auto* sym = reg_allocator_.find_symbol(var->name + "." + swiz->components)) {
            HardwareRegister r;
            r.reg_class = sym->reg_class;
            r.index = static_cast<uint8_t>(sym->base_register & 0xFF);
            r.sub_index = 0;
            return r;
        }
    }

    HardwareRegister base = generate_expression(swiz->target.get());
    if (!swiz->components.empty()) {
        base.sub_index = swizzle_char_to_sub(swiz->components[0]);
    }
    return base;
}

HardwareRegister UsseCodeGenerator::generate_variable(const frontend::VariableExpr* var) {
    const auto* sym = reg_allocator_.find_symbol(var->name);
    HardwareRegister r;
    if (sym) {
        r.reg_class = sym->reg_class;
        r.index = static_cast<uint8_t>(sym->base_register & 0xFF);
        r.sub_index = 0;
    } else {
        r.reg_class = RegisterClass::Temporary;
        r.index = 0;
    }
    return r;
}

HardwareRegister UsseCodeGenerator::generate_literal([[maybe_unused]] const frontend::LiteralExpr* lit) {
    HardwareRegister r = reg_allocator_.allocate_temporary();
    emit(UsseOpcode::VMOV, r, r, HardwareRegister{}, 0xF, "Literal load");
    return r;
}

} // namespace open_shacc::backend
