/**
 * @file usse_codegen.hpp
 * @brief USSE Target Code Generator translating AST to 64-bit microcode words.
 *
 * Clean-room implementation based strictly on public PowerVR SGX / USSE
 * architecture specifications and standard Cg / HLSL language semantics.
 */

#ifndef OPEN_SHACC_USSE_CODEGEN_HPP
#define OPEN_SHACC_USSE_CODEGEN_HPP

#include "usse_isa.hpp"
#include "regalloc.hpp"
#include "../frontend/ast.hpp"
#include <array>
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>

namespace open_shacc::backend {

struct CompiledProgram {
    std::vector<UsseInstruction> instructions;
    std::vector<uint64_t> microcode_words;
    std::string disassembly;
    std::vector<SymbolBinding> uniforms;
    std::vector<SymbolBinding> samplers;
    std::vector<SymbolBinding> inputs;
    std::vector<SymbolBinding> outputs;
    uint8_t temp_register_count = 0;
    uint16_t const_register_count = 0;
    uint8_t input_register_count = 0;
    uint16_t sampler_sa_base = 0;
    uint16_t uniform_sa_base = 0;
    bool is_vertex_shader = true;
    bool uses_discard = false;
    bool uses_depth = false;
    bool uses_nativecolor = false;
};

struct ValueRef {
    RegisterClass reg_class = RegisterClass::Temporary;
    uint16_t base_index = 0;
    uint8_t count = 1; // 1..4 for scalar/vector
    std::array<SwizzleChannel, 4> swiz = {
        SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W
    };
    bool is_negated = false;
    bool is_absolute = false;

    bool is_const = false;
    std::array<float, 4> const_vals = {0.0f, 0.0f, 0.0f, 0.0f};

    bool is_matrix = false;
    uint8_t mat_rows = 0;
    uint8_t mat_cols = 0;
    uint16_t row_stride = 4;

    bool is_array = false;
    uint32_t array_size = 1;
    uint16_t elem_stride = 4;

    std::string struct_name;
    std::string struct_instance;
};

class UsseCodeGenerator {
public:
    UsseCodeGenerator();

    bool generate(
        const frontend::TranslationUnit& tu,
        const std::string& entry_name,
        std::optional<bool> force_fragment,
        CompiledProgram& out_program
    );

    bool generate(const frontend::TranslationUnit& tu, CompiledProgram& out_program) {
        return generate(tu, "main", std::nullopt, out_program);
    }

    [[nodiscard]] const std::string& get_error_message() const { return error_message_; }

private:
    struct LoopContext {
        std::vector<size_t> break_patches;
        std::vector<size_t> continue_patches;
        std::optional<size_t> continue_target;
    };

    struct FunctionCallContext {
        bool active = false;
        std::optional<ValueRef> return_slot;
        std::vector<size_t> return_patches;
    };

    // Low-level emission helpers
    size_t emit_inst(UsseInstruction inst);
    size_t emit_br(int32_t rel_offset, ExtPredicate pred = ExtPredicate::None, const std::string& comment = "");
    void patch_br(size_t br_idx, size_t target_idx);
    void emit_phase_switch();

    // Constant & temporary management
    uint8_t alloc_temp_regs(uint8_t count);
    uint8_t alloc_temp_vec4();
    ValueRef make_temp_vec4(uint8_t count = 4);
    HardwareRegister get_zero_hw_reg();
    ValueRef get_or_create_float_const(float val);
    ValueRef make_const_scalar(float val);
    ValueRef make_const_vec(const std::array<float, 4>& vals, uint8_t count);

    // Hardware operand legalization & moves
    ValueRef legalize_source_value(const ValueRef& val);
    HardwareRegister to_hw_src1(const ValueRef& val, uint16_t& out_swiz12);
    HardwareRegister to_hw_src2(const ValueRef& val, Vec4Swiz& out_swiz4);
    ValueRef materialize_to_temp(const ValueRef& val, uint8_t target_count = 0, const std::string& comment = "");
    void emit_move(
        RegisterClass dst_class,
        uint16_t dst_base,
        uint8_t write_mask,
        const ValueRef& src,
        const std::string& comment = ""
    );
    void emit_move_mapped(
        RegisterClass dst_class,
        uint16_t dst_base,
        uint8_t write_mask,
        const std::array<uint8_t, 4>& target_lane_to_src_comp,
        const ValueRef& src,
        const std::string& comment = ""
    );

    // Arithmetic & comparison helpers
    ValueRef emit_binary_alu(frontend::BinaryOp op, const ValueRef& lhs, const ValueRef& rhs, const std::string& comment = "");
    ValueRef emit_vcomp_vec(UsseOpcode op, const ValueRef& src, const std::string& comment = "");
    ValueRef emit_dp(const ValueRef& a, const ValueRef& b, uint8_t comps, const std::string& comment = "");
    ValueRef emit_compare_to_float(frontend::BinaryOp op, const ValueRef& lhs, const ValueRef& rhs);
    void emit_condition_to_p0(const frontend::Expression* cond, bool invert = false);
    void emit_vtst_p0(const ValueRef& lhs, const ValueRef& rhs, VtstSignTest sign_t, VtstZeroTest zero_t, const std::string& comment = "");

    // Math & intrinsic expansions
    ValueRef emit_sin_cos(const ValueRef& x, bool is_cos);
    ValueRef emit_atan(const ValueRef& x);
    ValueRef emit_atan2(const ValueRef& y, const ValueRef& x);
    ValueRef emit_asin(const ValueRef& x);
    ValueRef emit_acos(const ValueRef& x);
    ValueRef emit_floor(const ValueRef& x);
    ValueRef emit_ceil(const ValueRef& x);
    ValueRef emit_trunc(const ValueRef& x);
    ValueRef emit_sqrt(const ValueRef& x);
    ValueRef emit_pow(const ValueRef& x, const ValueRef& y);
    ValueRef emit_shift(bool is_right, const ValueRef& lhs, const ValueRef& rhs, const std::string& comment = "");
    ValueRef emit_bitwise_op(frontend::BinaryOp op, const ValueRef& lhs, const ValueRef& rhs, const std::string& comment = "");
    ValueRef emit_matrix_mul(const ValueRef& lhs, const ValueRef& rhs);

    // Symbol/lvalue resolution
    ValueRef binding_to_value(const SymbolBinding& sym) const;
    bool resolve_lvalue(
        const frontend::Expression* expr,
        RegisterClass& out_class,
        uint16_t& out_base,
        uint8_t& out_mask,
        std::array<uint8_t, 4>& out_lane_map,
        ValueRef& out_cur_val,
        std::string& out_label
    );
    void assign_value_to_lvalue(
        const frontend::Expression* lvalue,
        frontend::BinaryOp compound_op,
        const ValueRef& rhs
    );

    // Expressions
    ValueRef generate_expr(const frontend::Expression* expr);
    ValueRef generate_binary_op(const frontend::BinaryExpr* bin);
    ValueRef generate_unary_op(const frontend::UnaryExpr* un);
    ValueRef generate_ternary(const frontend::TernaryExpr* tern);
    ValueRef generate_call(const frontend::CallExpr* call);
    ValueRef generate_construct(const frontend::ConstructExpr* construct);
    ValueRef generate_init_list(const frontend::InitListExpr* init_list, const frontend::DataType* hint_type = nullptr);
    ValueRef generate_swizzle(const frontend::SwizzleExpr* swiz);
    ValueRef generate_array_access(const frontend::ArrayAccessExpr* arr);
    ValueRef generate_variable(const frontend::VariableExpr* var);
    ValueRef generate_literal(const frontend::LiteralExpr* lit);

    // Helper function inlining & overload resolution
    const frontend::FunctionDecl* resolve_user_function(
        const std::string& name,
        const frontend::CallExpr* call,
        const std::vector<ValueRef>& eval_args
    ) const;
    ValueRef invoke_user_function(
        const frontend::FunctionDecl* fn,
        const frontend::CallExpr* call,
        std::vector<ValueRef> evaluated_args
    );

    // Statements
    void generate_statement(const frontend::Statement* stmt);
    void generate_block(const frontend::BlockStmt* block);
    void generate_var_decl(const frontend::VarDeclStmt* decl);
    void generate_assign(const frontend::AssignStmt* assign);
    void generate_return(const frontend::ReturnStmt* ret);
    void generate_if(const frontend::IfStmt* if_stmt);
    void generate_for(const frontend::ForStmt* for_stmt);
    void generate_while(const frontend::WhileStmt* while_stmt);
    void generate_do_while(const frontend::DoWhileStmt* dw_stmt);
    void generate_discard(const frontend::DiscardStmt* discard_stmt);

    // Constant evaluation helper
    static bool eval_const_int(const frontend::Expression* expr, int64_t& out_val);
    static bool statement_contains_discard(const frontend::Statement* stmt);

    RegisterAllocator reg_allocator_;
    std::vector<UsseInstruction> instructions_;
    std::string error_message_;
    const frontend::TranslationUnit* tu_ = nullptr;
    const frontend::FunctionDecl* entry_fn_ = nullptr;
    const frontend::FunctionDecl* active_fn_ = nullptr;
    std::vector<SymbolBinding> return_struct_fields_;

    bool is_fragment_ = false;
    bool uses_discard_ = false;
    bool uses_depth_ = false;
    bool uses_nativecolor_ = false;
    std::optional<HardwareRegister> depth_src_reg_;
    struct InOutBinding {
        SymbolBinding temp_sym;
        SymbolBinding out_sym;
    };
    std::vector<InOutBinding> inout_outputs_;

    // Lazy float literal constant cache (4 scalars packed per vec4 temp slot)
    bool has_zero_reg_ = false;
    uint8_t zero_reg_index_ = 0;
    uint8_t const_cache_base_ = 0;
    uint8_t const_cache_sub_ = 4; // 4 = forces fresh vec4 allocation on first literal
    std::unordered_map<uint32_t, uint8_t> float_const_cache_;

    // Scope stack for block-local symbol restoration and temp cleanup
    struct TempAlloc {
        uint8_t reg = 0;
        uint8_t pair_count = 2;
    };
    struct ScopeFrame {
        std::vector<std::pair<std::string, std::optional<SymbolBinding>>> shadowed_symbols;
        std::vector<TempAlloc> owned_temps;
    };
    std::vector<ScopeFrame> scopes_;
    void push_scope();
    void pop_scope();
    void preserve_and_pop_scope(const ValueRef& keep_val);
    void bind_scoped_symbol(const std::string& name, const SymbolBinding& binding);

    std::vector<LoopContext> loop_stack_;
    std::vector<size_t> entry_return_patches_;
    FunctionCallContext inline_call_ctx_;
    int inline_depth_ = 0;
};

} // namespace open_shacc::backend

#endif // OPEN_SHACC_USSE_CODEGEN_HPP
