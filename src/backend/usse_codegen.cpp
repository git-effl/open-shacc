/**
 * @file usse_codegen.cpp
 * @brief Implementation of USSE Target Code Generator.
 *
 * Clean-room implementation based on open standard USSE / SGX instruction set architecture.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usse_codegen.hpp"
#include <sstream>
#include <cassert>

namespace open_shacc::backend {

UsseCodeGenerator::UsseCodeGenerator() = default;

void UsseCodeGenerator::emit(
    UsseOpcode op,
    HardwareRegister dst,
    HardwareRegister src0,
    HardwareRegister src1,
    const std::string& comment
) {
    UsseInstruction inst;
    inst.opcode = op;
    inst.dst = dst;
    inst.src0 = src0;
    inst.src1 = src1;
    inst.comment = comment;
    instructions_.push_back(inst);
}

void UsseCodeGenerator::emit(
    UsseOpcode op,
    HardwareRegister dst,
    HardwareRegister src0,
    HardwareRegister src1,
    HardwareRegister src2,
    const std::string& comment
) {
    UsseInstruction inst;
    inst.opcode = op;
    inst.dst = dst;
    inst.src0 = src0;
    inst.src1 = src1;
    inst.src2 = src2;
    inst.comment = comment;
    instructions_.push_back(inst);
}

void UsseCodeGenerator::emit_phase_switch() {
    UsseInstruction inst;
    inst.opcode = UsseOpcode::PHAS;
    inst.comment = "End of vertex phase";
    instructions_.push_back(inst);
}

void UsseCodeGenerator::emit_halt() {
    UsseInstruction inst;
    inst.opcode = UsseOpcode::HALT;
    inst.comment = "Program terminate";
    instructions_.push_back(inst);
}

bool UsseCodeGenerator::generate(const frontend::TranslationUnit& tu, CompiledProgram& out_program) {
    tu_ = &tu;
    reg_allocator_.reset();
    instructions_.clear();
    error_message_.clear();

    // 1. Allocate uniform registers (c0..c255)
    for (const auto& u : tu.uniforms) {
        SymbolBinding binding;
        if (!reg_allocator_.allocate_uniform(u.name, u.type, u.semantic, binding)) {
            error_message_ = "Constant register overflow allocating uniform: " + u.name;
            return false;
        }
    }

    // 2. Find entry point (main)
    const frontend::FunctionDecl* entry_fn = tu.find_entry_point("main");
    if (!entry_fn && !tu.functions.empty()) {
        entry_fn = &tu.functions.front();
    }

    if (!entry_fn) {
        error_message_ = "No entry point function 'main' found in translation unit";
        return false;
    }
    current_fn_ = entry_fn;

    // 3. Allocate inputs from parameters
    for (const auto& param : entry_fn->parameters) {
        if (param.type.base == frontend::BaseType::CustomStruct) {
            // Expand struct fields as inputs
            const auto* s = tu.find_struct(param.type.struct_name);
            if (s) {
                for (const auto& field : s->fields) {
                    SymbolBinding binding;
                    std::string qualified_name = param.name + "." + field.name;
                    reg_allocator_.allocate_input(qualified_name, field.type, field.semantic, binding);
                    // Also bind short name if unique
                    reg_allocator_.allocate_input(field.name, field.type, field.semantic, binding);
                }
            }
        } else {
            SymbolBinding binding;
            reg_allocator_.allocate_input(param.name, param.type, param.semantic, binding);
        }
    }

    // 4. Allocate outputs from return type
    if (entry_fn->return_type.base == frontend::BaseType::CustomStruct) {
        const auto* s = tu.find_struct(entry_fn->return_type.struct_name);
        if (s) {
            for (const auto& field : s->fields) {
                SymbolBinding binding;
                reg_allocator_.allocate_output(field.name, field.type, field.semantic, binding);
            }
        }
    } else if (entry_fn->return_type.base != frontend::BaseType::Void) {
        SymbolBinding binding;
        reg_allocator_.allocate_output("gl_Position", entry_fn->return_type, entry_fn->return_semantic, binding);
    }

    // 5. Generate function body
    if (entry_fn->body) {
        generate_block(entry_fn->body.get());
    }

    // 6. Terminate shader microcode with phase switch & halt
    emit_phase_switch();
    emit_halt();

    // 7. Assemble binary microcode words & disassembly
    out_program.instructions = instructions_;
    out_program.uniforms = reg_allocator_.get_uniforms();
    out_program.inputs = reg_allocator_.get_inputs();
    out_program.outputs = reg_allocator_.get_outputs();
    out_program.temp_register_count = reg_allocator_.get_temporary_count();
    out_program.const_register_count = reg_allocator_.get_constant_count();
    out_program.is_vertex_shader = true;

    std::ostringstream disasm;
    disasm << "; ================================================================\n";
    disasm << "; open-shacc USSE Disassembly\n";
    disasm << "; Clean-room architecture target: PowerVR SGX / USSE\n";
    disasm << "; ================================================================\n\n";

    disasm << "; --- Uniform Constant Mapping ---\n";
    for (const auto& u : out_program.uniforms) {
        disasm << ";   " << u.name << " -> c" << static_cast<int>(u.base_register)
               << " (size: " << static_cast<int>(u.register_count) << " words)\n";
    }

    disasm << "\n; --- Vertex Attribute Inputs ---\n";
    for (const auto& in : out_program.inputs) {
        disasm << ";   " << in.name << " (" << in.semantic << ") -> v" << static_cast<int>(in.base_register) << "\n";
    }

    disasm << "\n; --- Program Instructions ---\n";
    out_program.microcode_words.clear();

    for (size_t i = 0; i < instructions_.size(); ++i) {
        const auto& inst = instructions_[i];
        MicrocodeWord word = inst.to_microcode();
        out_program.microcode_words.push_back(word.raw_word);

        std::ostringstream hex_word;
        hex_word << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << word.raw_word;

        disasm << std::left << std::setw(6) << i << ": [" << hex_word.str() << "]  " << inst.disassemble() << "\n";
    }

    out_program.disassembly = disasm.str();
    return true;
}

void UsseCodeGenerator::generate_statement(const frontend::Statement* stmt) {
    if (auto* b = dynamic_cast<const frontend::BlockStmt*>(stmt)) {
        generate_block(b);
    } else if (auto* v = dynamic_cast<const frontend::VarDeclStmt*>(stmt)) {
        generate_var_decl(v);
    } else if (auto* a = dynamic_cast<const frontend::AssignStmt*>(stmt)) {
        generate_assign(a);
    } else if (auto* r = dynamic_cast<const frontend::ReturnStmt*>(stmt)) {
        generate_return(r);
    }
}

void UsseCodeGenerator::generate_block(const frontend::BlockStmt* block) {
    for (const auto& s : block->statements) {
        generate_statement(s.get());
    }
}

void UsseCodeGenerator::generate_var_decl(const frontend::VarDeclStmt* decl) {
    if (decl->initializer) {
        size_t components = decl->type.component_count();
        if (components == 0) components = 1;

        if (components == 1) {
            HardwareRegister r = generate_expression(decl->initializer.get());
            HardwareRegister var_reg = reg_allocator_.allocate_temporary();
            emit(UsseOpcode::MOV, var_reg, r, HardwareRegister{}, "Declare " + decl->name);
        } else {
            auto regs = generate_vector_expression(decl->initializer.get(), components);
            auto var_regs = reg_allocator_.allocate_temporary_vector(static_cast<uint8_t>(components));
            for (size_t i = 0; i < components && i < regs.size(); ++i) {
                emit(UsseOpcode::MOV, var_regs[i], regs[i], HardwareRegister{}, decl->name + "[" + std::to_string(i) + "]");
            }
        }
    }
}

void UsseCodeGenerator::generate_assign(const frontend::AssignStmt* assign) {
    // If lvalue is variable or struct member
    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(assign->lvalue.get())) {
        HardwareRegister val_reg = generate_expression(assign->rvalue.get());
        const auto* sym = reg_allocator_.find_symbol(var->name);
        if (sym) {
            HardwareRegister dst;
            dst.reg_class = sym->reg_class;
            dst.index = sym->base_register;
            emit(UsseOpcode::MOV, dst, val_reg, HardwareRegister{}, "Assign to " + var->name);
        }
    } else if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(assign->lvalue.get())) {
        // e.g. output.position = ...
        if (auto* target_var = dynamic_cast<const frontend::VariableExpr*>(swiz->target.get())) {
            std::string full_name = target_var->name + "." + swiz->components;
            const auto* sym = reg_allocator_.find_symbol(swiz->components);
            if (!sym) sym = reg_allocator_.find_symbol(full_name);

            if (sym) {
                size_t comp_count = sym->register_count;
                auto rhs_regs = generate_vector_expression(assign->rvalue.get(), comp_count);
                for (size_t i = 0; i < comp_count && i < rhs_regs.size(); ++i) {
                    HardwareRegister dst;
                    dst.reg_class = sym->reg_class;
                    dst.index = sym->base_register + static_cast<uint8_t>(i);
                    dst.sub_index = static_cast<uint8_t>(i % 4);
                    emit(UsseOpcode::MOV, dst, rhs_regs[i], HardwareRegister{}, full_name + " write");
                }
            }
        }
    }
}

void UsseCodeGenerator::generate_return(const frontend::ReturnStmt* ret) {
    if (!ret->value) return;

    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(ret->value.get())) {
        // Returning local struct or variable
        const auto* sym = reg_allocator_.find_symbol(var->name);
        if (sym && sym->reg_class == RegisterClass::Output) {
            // Already mapped to output
            return;
        }
    } else {
        // Returning a vector expression directly (e.g. return mul(u_MVPMatrix, pos);)
        auto result_regs = generate_vector_expression(ret->value.get(), 4);
        for (size_t i = 0; i < result_regs.size() && i < 4; ++i) {
            HardwareRegister dst;
            dst.reg_class = RegisterClass::Output;
            dst.index = static_cast<uint8_t>(i);
            dst.sub_index = static_cast<uint8_t>(i);
            emit(UsseOpcode::MOV, dst, result_regs[i], HardwareRegister{}, "Vertex output o" + std::to_string(i));
        }
    }
}

std::vector<HardwareRegister> UsseCodeGenerator::generate_matrix_vector_mul(
    const std::string& matrix_name,
    const std::vector<HardwareRegister>& vec
) {
    std::vector<HardwareRegister> out_regs;
    out_regs.reserve(4);

    const auto* sym = reg_allocator_.find_symbol(matrix_name);
    uint8_t base_c = sym ? sym->base_register : 0;

    // Multiply 4x4 matrix by 4-component vector via 4 DOT4 instructions
    // row 0: c[base + 0 .. base + 3]
    // row 1: c[base + 4 .. base + 7]
    // row 2: c[base + 8 .. base + 11]
    // row 3: c[base + 12 .. base + 15]
    for (int row = 0; row < 4; ++row) {
        HardwareRegister dst = reg_allocator_.allocate_temporary(static_cast<uint8_t>(row));
        HardwareRegister m_row;
        m_row.reg_class = RegisterClass::Constant;
        m_row.index = base_c + static_cast<uint8_t>(row * 4);
        m_row.sub_index = 0;

        HardwareRegister v_in = vec.empty() ? reg_allocator_.allocate_temporary() : vec[0];

        emit(UsseOpcode::DOT4, dst, m_row, v_in, "mul(" + matrix_name + " row " + std::to_string(row) + ", v)");
        out_regs.push_back(dst);
    }

    return out_regs;
}

HardwareRegister UsseCodeGenerator::generate_expression(const frontend::Expression* expr) {
    if (auto* lit = dynamic_cast<const frontend::LiteralExpr*>(expr)) {
        return generate_literal(lit);
    }
    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(expr)) {
        return generate_variable(var);
    }
    if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(expr)) {
        return generate_swizzle(swiz);
    }
    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(expr)) {
        return generate_binary_op(bin);
    }
    if (auto* un = dynamic_cast<const frontend::UnaryExpr*>(expr)) {
        return generate_unary_op(un);
    }
    if (auto* call = dynamic_cast<const frontend::CallExpr*>(expr)) {
        return generate_call(call);
    }

    return reg_allocator_.allocate_temporary();
}

std::vector<HardwareRegister> UsseCodeGenerator::generate_vector_expression(
    const frontend::Expression* expr,
    size_t expected_components
) {
    std::vector<HardwareRegister> results;

    if (auto* call = dynamic_cast<const frontend::CallExpr*>(expr)) {
        if (call->callee == "mul" && call->arguments.size() == 2) {
            // Check if arg 0 is matrix identifier
            if (auto* mat_var = dynamic_cast<const frontend::VariableExpr*>(call->arguments[0].get())) {
                auto vec_regs = generate_vector_expression(call->arguments[1].get(), 4);
                return generate_matrix_vector_mul(mat_var->name, vec_regs);
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
        // Extract multi-component swizzle
        for (char c : swiz->components) {
            uint8_t sub_idx = 0;
            if (c == 'y' || c == 'g') sub_idx = 1;
            else if (c == 'z' || c == 'b') sub_idx = 2;
            else if (c == 'w' || c == 'a') sub_idx = 3;

            HardwareRegister base = generate_expression(swiz->target.get());
            HardwareRegister r = base;
            r.sub_index = sub_idx;
            results.push_back(r);
        }
        return results;
    }

    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(expr)) {
        auto left_regs = generate_vector_expression(bin->left.get(), expected_components);
        auto right_regs = generate_vector_expression(bin->right.get(), expected_components);

        UsseOpcode op = UsseOpcode::FADD;
        if (bin->op == frontend::BinaryOp::Sub) op = UsseOpcode::FSUB;
        else if (bin->op == frontend::BinaryOp::Mul) op = UsseOpcode::FMUL;

        for (size_t i = 0; i < expected_components; ++i) {
            HardwareRegister dst = reg_allocator_.allocate_temporary(static_cast<uint8_t>(i % 4));
            HardwareRegister l = i < left_regs.size() ? left_regs[i] : left_regs.front();
            HardwareRegister r = i < right_regs.size() ? right_regs[i] : right_regs.front();
            emit(op, dst, l, r, "vector component " + std::to_string(i));
            results.push_back(dst);
        }
        return results;
    }

    // Scalar fallback
    HardwareRegister single = generate_expression(expr);
    for (size_t i = 0; i < expected_components; ++i) {
        results.push_back(single);
    }
    return results;
}

HardwareRegister UsseCodeGenerator::generate_binary_op(const frontend::BinaryExpr* bin) {
    HardwareRegister left = generate_expression(bin->left.get());
    HardwareRegister right = generate_expression(bin->right.get());
    HardwareRegister dst = reg_allocator_.allocate_temporary();

    UsseOpcode op = UsseOpcode::FADD;
    switch (bin->op) {
        case frontend::BinaryOp::Add: op = UsseOpcode::FADD; break;
        case frontend::BinaryOp::Sub: op = UsseOpcode::FSUB; break;
        case frontend::BinaryOp::Mul: op = UsseOpcode::FMUL; break;
        case frontend::BinaryOp::Dot: op = UsseOpcode::DOT4; break;
        default: op = UsseOpcode::FADD; break;
    }

    emit(op, dst, left, right);
    return dst;
}

HardwareRegister UsseCodeGenerator::generate_unary_op(const frontend::UnaryExpr* un) {
    HardwareRegister opnd = generate_expression(un->operand.get());
    HardwareRegister dst = reg_allocator_.allocate_temporary();

    if (un->op == frontend::UnaryOp::Negate) {
        opnd.is_negated = !opnd.is_negated;
        emit(UsseOpcode::MOV, dst, opnd, HardwareRegister{}, "Negate");
    } else if (un->op == frontend::UnaryOp::Rcp) {
        emit(UsseOpcode::RCP, dst, opnd, HardwareRegister{}, "Reciprocal");
    } else if (un->op == frontend::UnaryOp::Rsq) {
        emit(UsseOpcode::RSQ, dst, opnd, HardwareRegister{}, "Reciprocal square root");
    } else {
        emit(UsseOpcode::MOV, dst, opnd, HardwareRegister{});
    }

    return dst;
}

HardwareRegister UsseCodeGenerator::generate_call(const frontend::CallExpr* call) {
    HardwareRegister dst = reg_allocator_.allocate_temporary();

    if (call->callee == "dot" || call->callee == "dp4") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::DOT4, dst, a, b, "dot4");
        }
    } else if (call->callee == "dp3") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::DOT3, dst, a, b, "dot3");
        }
    } else if (call->callee == "max") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::MAX, dst, a, b);
        }
    } else if (call->callee == "min") {
        if (call->arguments.size() >= 2) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            HardwareRegister b = generate_expression(call->arguments[1].get());
            emit(UsseOpcode::MIN, dst, a, b);
        }
    } else {
        // Generic fallback
        if (!call->arguments.empty()) {
            HardwareRegister a = generate_expression(call->arguments[0].get());
            emit(UsseOpcode::MOV, dst, a, HardwareRegister{}, "Call " + call->callee);
        }
    }

    return dst;
}

HardwareRegister UsseCodeGenerator::generate_swizzle(const frontend::SwizzleExpr* swiz) {
    HardwareRegister base = generate_expression(swiz->target.get());
    if (!swiz->components.empty()) {
        char c = swiz->components[0];
        if (c == 'y' || c == 'g') base.sub_index = 1;
        else if (c == 'z' || c == 'b') base.sub_index = 2;
        else if (c == 'w' || c == 'a') base.sub_index = 3;
        else base.sub_index = 0;
    }
    return base;
}

HardwareRegister UsseCodeGenerator::generate_variable(const frontend::VariableExpr* var) {
    const auto* sym = reg_allocator_.find_symbol(var->name);
    HardwareRegister r;
    if (sym) {
        r.reg_class = sym->reg_class;
        r.index = sym->base_register;
        r.sub_index = 0;
    } else {
        r.reg_class = RegisterClass::Temporary;
        r.index = 0;
    }
    return r;
}

HardwareRegister UsseCodeGenerator::generate_literal(const frontend::LiteralExpr* lit) {
    HardwareRegister r = reg_allocator_.allocate_temporary();
    // In USSE microcode literals are typically loaded or referenced via constant bank
    emit(UsseOpcode::MOV, r, r, HardwareRegister{}, "Literal load");
    return r;
}

} // namespace open_shacc::backend
