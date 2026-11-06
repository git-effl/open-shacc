/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree structures for Cg / HLSL shader front-end.
 *
 * Clean-room implementation based on standard NVIDIA Cg language specification.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_AST_HPP
#define OPEN_SHACC_AST_HPP

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <variant>
#include <cstdint>

namespace open_shacc::frontend {

enum class BaseType {
    Void,
    Bool,
    Int,
    Short,
    UShort,
    Char,
    UChar,
    Float,
    Half,
    Fixed,
    Float2,
    Float3,
    Float4,
    Half2,
    Half3,
    Half4,
    Float2x2,
    Float3x3,
    Float4x4,
    Half2x2,
    Half3x3,
    Half4x4,
    Sampler2D,
    SamplerCube,
    CustomStruct,
    UniformBlock
};

struct DataType {
    BaseType base = BaseType::Void;
    std::string struct_name; // if CustomStruct
    uint32_t array_size = 1;  // 1 if scalar/vector/matrix; >1 if array

    [[nodiscard]] size_t component_count() const {
        switch (base) {
            case BaseType::Float:
            case BaseType::Half:
            case BaseType::Fixed:
            case BaseType::Int:
            case BaseType::Short:
            case BaseType::UShort:
            case BaseType::Char:
            case BaseType::UChar:
            case BaseType::Bool:       return 1;
            case BaseType::Float2:
            case BaseType::Half2:      return 2;
            case BaseType::Float3:
            case BaseType::Half3:      return 3;
            case BaseType::Float4:
            case BaseType::Half4:      return 4;
            case BaseType::Float2x2:
            case BaseType::Half2x2:    return 4;
            case BaseType::Float3x3:
            case BaseType::Half3x3:    return 9;
            case BaseType::Float4x4:
            case BaseType::Half4x4:    return 16;
            default: return 1;
        }
    }

    [[nodiscard]] size_t matrix_rows() const {
        switch (base) {
            case BaseType::Float2x2:
            case BaseType::Half2x2: return 2;
            case BaseType::Float3x3:
            case BaseType::Half3x3: return 3;
            case BaseType::Float4x4:
            case BaseType::Half4x4: return 4;
            default: return 0;
        }
    }

    [[nodiscard]] size_t matrix_cols() const {
        return matrix_rows();
    }

    [[nodiscard]] bool is_matrix() const {
        return matrix_rows() > 0;
    }

    [[nodiscard]] bool is_sampler() const {
        return base == BaseType::Sampler2D || base == BaseType::SamplerCube;
    }

    [[nodiscard]] std::string to_string() const {
        std::string s;
        switch (base) {
            case BaseType::Void: s = "void"; break;
            case BaseType::Bool: s = "bool"; break;
            case BaseType::Int: s = "int"; break;
            case BaseType::Short: s = "short"; break;
            case BaseType::UShort: s = "ushort"; break;
            case BaseType::Char: s = "char"; break;
            case BaseType::UChar: s = "uchar"; break;
            case BaseType::Float: s = "float"; break;
            case BaseType::Half: s = "half"; break;
            case BaseType::Fixed: s = "fixed"; break;
            case BaseType::Float2: s = "float2"; break;
            case BaseType::Float3: s = "float3"; break;
            case BaseType::Float4: s = "float4"; break;
            case BaseType::Half2: s = "half2"; break;
            case BaseType::Half3: s = "half3"; break;
            case BaseType::Half4: s = "half4"; break;
            case BaseType::Float2x2: s = "float2x2"; break;
            case BaseType::Float3x3: s = "float3x3"; break;
            case BaseType::Float4x4: s = "float4x4"; break;
            case BaseType::Half2x2: s = "half2x2"; break;
            case BaseType::Half3x3: s = "half3x3"; break;
            case BaseType::Half4x4: s = "half4x4"; break;
            case BaseType::Sampler2D: s = "sampler2D"; break;
            case BaseType::SamplerCube: s = "samplerCUBE"; break;
            case BaseType::CustomStruct: s = struct_name; break;
            case BaseType::UniformBlock: s = "uniform block"; break;
        }
        if (array_size > 1) {
            s += "[" + std::to_string(array_size) + "]";
        }
        return s;
    }
};

enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    LogicalAnd, LogicalOr, BitAnd, BitOr, BitXor, Shl, Shr
};

enum class UnaryOp {
    Negate, LogicalNot, BitNot, Incr, Decr
};

struct SourceLocation {
    uint32_t line = 1;
    uint32_t column = 1;
};

struct Expression;
using ExprPtr = std::unique_ptr<Expression>;

struct Expression {
    virtual ~Expression() = default;
    DataType deduced_type;
    SourceLocation location;
};

struct LiteralExpr : public Expression {
    std::variant<float, int, bool> value;
    explicit LiteralExpr(float f) { value = f; deduced_type = DataType{BaseType::Float, "", 1}; }
    explicit LiteralExpr(int i) { value = i; deduced_type = DataType{BaseType::Int, "", 1}; }
    explicit LiteralExpr(bool b) { value = b; deduced_type = DataType{BaseType::Bool, "", 1}; }
};

struct VariableExpr : public Expression {
    std::string name;
    explicit VariableExpr(std::string n) : name(std::move(n)) {}
};

struct SwizzleExpr : public Expression {
    ExprPtr target;
    std::string components;

    SwizzleExpr(ExprPtr t, std::string comps)
        : target(std::move(t)), components(std::move(comps)) {}
};

struct ArrayAccessExpr : public Expression {
    ExprPtr target;
    ExprPtr index;

    ArrayAccessExpr(ExprPtr t, ExprPtr idx)
        : target(std::move(t)), index(std::move(idx)) {}
};

struct BinaryExpr : public Expression {
    BinaryOp op;
    ExprPtr left;
    ExprPtr right;

    BinaryExpr(BinaryOp o, ExprPtr l, ExprPtr r)
        : op(o), left(std::move(l)), right(std::move(r)) {}
};

struct UnaryExpr : public Expression {
    UnaryOp op;
    ExprPtr operand;
    bool is_postfix = false;

    UnaryExpr(UnaryOp o, ExprPtr opnd, bool postfix = false)
        : op(o), operand(std::move(opnd)), is_postfix(postfix) {}
};

struct TernaryExpr : public Expression {
    ExprPtr condition;
    ExprPtr then_expr;
    ExprPtr else_expr;

    TernaryExpr(ExprPtr cond, ExprPtr t, ExprPtr e)
        : condition(std::move(cond)), then_expr(std::move(t)), else_expr(std::move(e)) {}
};

struct CallExpr : public Expression {
    std::string callee;
    std::vector<ExprPtr> arguments;

    CallExpr(std::string name, std::vector<ExprPtr> args)
        : callee(std::move(name)), arguments(std::move(args)) {}
};

struct ConstructExpr : public Expression {
    DataType target_type;
    std::vector<ExprPtr> arguments;

    ConstructExpr(DataType dt, std::vector<ExprPtr> args)
        : target_type(dt), arguments(std::move(args)) {
        deduced_type = dt;
    }
};

// Statements
struct Statement {
    virtual ~Statement() = default;
    SourceLocation location;
};
using StmtPtr = std::unique_ptr<Statement>;

struct VarDeclStmt : public Statement {
    DataType type;
    std::string name;
    std::string semantic;
    ExprPtr initializer;
};

struct AssignStmt : public Statement {
    ExprPtr lvalue;
    ExprPtr rvalue;
    BinaryOp compound_op = BinaryOp::Equal; // BinaryOp::Equal for standard '='
};

struct ReturnStmt : public Statement {
    ExprPtr value;
};

struct DiscardStmt : public Statement {};

struct BlockStmt : public Statement {
    std::vector<StmtPtr> statements;
};

struct IfStmt : public Statement {
    ExprPtr condition;
    StmtPtr then_branch;
    StmtPtr else_branch;
};

struct ForStmt : public Statement {
    StmtPtr init;
    ExprPtr condition;
    StmtPtr increment;
    StmtPtr body;
};

// Top-Level Declarations
struct StructField {
    DataType type;
    std::string name;
    std::string semantic;
};

struct StructDecl {
    std::string name;
    std::vector<StructField> fields;
};

struct UniformDecl {
    DataType type;
    std::string name;
    std::string semantic;
    uint32_t register_offset = 0;
};

struct ParameterDecl {
    DataType type;
    std::string name;
    std::string semantic;
    bool is_in = true;
    bool is_out = false;
    bool is_uniform = false;
};

struct FunctionDecl {
    DataType return_type;
    std::string return_semantic;
    std::string name;
    std::vector<ParameterDecl> parameters;
    std::unique_ptr<BlockStmt> body;
};

struct TranslationUnit {
    std::vector<StructDecl> structs;
    std::vector<UniformDecl> uniforms;
    std::vector<VarDeclStmt> globals;
    std::vector<FunctionDecl> functions;

    [[nodiscard]] const FunctionDecl* find_entry_point(const std::string& name = "main") const {
        for (const auto& fn : functions) {
            if (fn.name == name) return &fn;
        }
        return nullptr;
    }

    [[nodiscard]] const StructDecl* find_struct(const std::string& name) const {
        for (const auto& s : structs) {
            if (s.name == name) return &s;
        }
        return nullptr;
    }
};

} // namespace open_shacc::frontend

#endif // OPEN_SHACC_AST_HPP
