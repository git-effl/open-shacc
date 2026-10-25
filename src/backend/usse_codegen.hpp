/**
 * @file usse_codegen.hpp
 * @brief USSE Target Code Generator translating AST to 32-bit microcode words.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_USSE_CODEGEN_HPP
#define OPEN_SHACC_USSE_CODEGEN_HPP

#include "usse_isa.hpp"
#include "regalloc.hpp"
#include "../frontend/ast.hpp"
#include <vector>
#include <string>
#include <memory>

namespace open_shacc::backend {

struct CompiledProgram {
    std::vector<UsseInstruction> instructions;
    std::vector<uint32_t> microcode_words;
    std::string disassembly;
    std::vector<SymbolBinding> uniforms;
    std::vector<SymbolBinding> inputs;
    std::vector<SymbolBinding> outputs;
    uint8_t temp_register_count = 0;
    uint8_t const_register_count = 0;
    bool is_vertex_shader = true;
};

class UsseCodeGenerator {
public:
    UsseCodeGenerator();

    bool generate(const frontend::TranslationUnit& tu, CompiledProgram& out_program);
    [[nodiscard]] const std::string& get_error_message() const { return error_message_; }

private:
    void emit(UsseOpcode op, HardwareRegister dst, HardwareRegister src0, HardwareRegister src1, const std::string& comment = "");
    void emit(UsseOpcode op, HardwareRegister dst, HardwareRegister src0, HardwareRegister src1, HardwareRegister src2, const std::string& comment = "");
    void emit_phase_switch();
    void emit_halt();

    // Expression code generation returning hardware register
    HardwareRegister generate_expression(const frontend::Expression* expr);
    std::vector<HardwareRegister> generate_vector_expression(const frontend::Expression* expr, size_t expected_components);

    // Specific expressions
    HardwareRegister generate_binary_op(const frontend::BinaryExpr* bin);
    HardwareRegister generate_unary_op(const frontend::UnaryExpr* un);
    HardwareRegister generate_call(const frontend::CallExpr* call);
    HardwareRegister generate_swizzle(const frontend::SwizzleExpr* swiz);
    HardwareRegister generate_variable(const frontend::VariableExpr* var);
    HardwareRegister generate_literal(const frontend::LiteralExpr* lit);

    // Statements
    void generate_statement(const frontend::Statement* stmt);
    void generate_block(const frontend::BlockStmt* block);
    void generate_var_decl(const frontend::VarDeclStmt* decl);
    void generate_assign(const frontend::AssignStmt* assign);
    void generate_return(const frontend::ReturnStmt* ret);

    // Matrix multiply helper
    std::vector<HardwareRegister> generate_matrix_vector_mul(
        const std::string& matrix_name,
        const std::vector<HardwareRegister>& vec
    );

    RegisterAllocator reg_allocator_;
    std::vector<UsseInstruction> instructions_;
    std::string error_message_;
    const frontend::TranslationUnit* tu_ = nullptr;
    const frontend::FunctionDecl* current_fn_ = nullptr;
};

} // namespace open_shacc::backend

#endif // OPEN_SHACC_USSE_CODEGEN_HPP
