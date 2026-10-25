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
    Float,
    Float2,
    Float3,
    Float4,
    Float4x4,
    Sampler2D,
    CustomStruct
};

struct DataType {
    BaseType base = BaseType::Void;
    std::string struct_name; // if CustomStruct

    [[nodiscard]] size_t component_count() const {
        switch (base) {
            case BaseType::Float:  return 1;
            case BaseType::Float2: return 2;
            case BaseType::Float3: return 3;
            case BaseType::Float4: return 4;
            case BaseType::Float4x4: return 16;
            case BaseType::Int:    return 1;
            case BaseType::Bool:   return 1;
            default: return 0;
        }
    }

    [[nodiscard]] size_t word_count() const {
        return component_count(); // 32-bit words
    }

    [[nodiscard]] std::string to_string() const {
        switch (base) {
            case BaseType::Void: return "void";
            case BaseType::Bool: return "bool";
            case BaseType::Int: return "int";
            case BaseType::Float: return "float";
            case BaseType::Float2: return "float2";
            case BaseType::Float3: return "float3";
            case BaseType::Float4: return "float4";
            case BaseType::Float4x4: return "float4x4";
            case BaseType::Sampler2D: return "sampler2D";
            case BaseType::CustomStruct: return struct_name;
        }
        return "unknown";
    }
};

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Dot,
    Cross,
    Equal,
    NotEqual,
    Less,
    Greater
};

enum class UnaryOp {
    Negate,
    LogicalNot,
    Normalize,
    Length,
    Sin,
    Cos,
    Rcp,
    Rsq
};

struct SourceLocation {
    uint32_t line = 1;
    uint32_t column = 1;
};

// Forward declaration
struct Expression;
using ExprPtr = std::unique_ptr<Expression>;

struct Expression {
    virtual ~Expression() = default;
    DataType deduced_type;
    SourceLocation location;
};

struct LiteralExpr : public Expression {
    std::variant<float, int, bool> value;
    explicit LiteralExpr(float f) { value = f; deduced_type = {BaseType::Float}; }
    explicit LiteralExpr(int i) { value = i; deduced_type = {BaseType::Int}; }
    explicit LiteralExpr(bool b) { value = b; deduced_type = {BaseType::Bool}; }
};

struct VariableExpr : public Expression {
    std::string name;
    explicit VariableExpr(std::string n) : name(std::move(n)) {}
};

struct SwizzleExpr : public Expression {
    ExprPtr target;
    std::string components; // e.g., "xyzw", "x", "rgb", "wzyx"

    SwizzleExpr(ExprPtr t, std::string comps)
        : target(std::move(t)), components(std::move(comps)) {}
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

    UnaryExpr(UnaryOp o, ExprPtr opnd)
        : op(o), operand(std::move(opnd)) {}
};

struct CallExpr : public Expression {
    std::string callee; // "mul", "dot", "dp3", "dp4", "max", "min", "clamp", "tex2D"
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
    std::string semantic; // e.g. "POSITION", "TEXCOORD0"
    ExprPtr initializer;
};

struct AssignStmt : public Statement {
    ExprPtr lvalue;
    ExprPtr rvalue;
};

struct ReturnStmt : public Statement {
    ExprPtr value;
};

struct BlockStmt : public Statement {
    std::vector<StmtPtr> statements;
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
    uint32_t register_offset = 0; // populated during allocation
};

struct ParameterDecl {
    DataType type;
    std::string name;
    std::string semantic; // e.g. "POSITION", "NORMAL", "TEXCOORD0"
    bool is_in = true;
    bool is_out = false;
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
