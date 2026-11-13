/**
 * @file parser.cpp
 * @brief Clean-room Cg/HLSL AST parser implementation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "parser.hpp"
#include <algorithm>
#include <cmath>

namespace open_shacc::frontend {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::peek() const {
    if (current_ >= tokens_.size()) return tokens_.back();
    return tokens_[current_];
}

const Token& Parser::peek_next(size_t offset) const {
    if (current_ + offset >= tokens_.size()) return tokens_.back();
    return tokens_[current_ + offset];
}

const Token& Parser::previous() const {
    if (current_ == 0) return tokens_.front();
    return tokens_[current_ - 1];
}

bool Parser::is_at_end() const {
    return peek().type == TokenType::EndOfFile;
}

const Token& Parser::advance() {
    if (!is_at_end()) current_++;
    return previous();
}

bool Parser::check(TokenType type) const {
    if (is_at_end()) return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::consume(TokenType type, const std::string& error_message) {
    if (check(type)) return advance();
    error(peek(), error_message);
    static const Token kErrToken;
    return kErrToken;
}

bool Parser::is_value_type_token(TokenType t) {
    switch (t) {
        case TokenType::KwBool: case TokenType::KwBool2: case TokenType::KwBool3: case TokenType::KwBool4:
        case TokenType::KwInt: case TokenType::KwInt2: case TokenType::KwInt3: case TokenType::KwInt4:
        case TokenType::KwUInt: case TokenType::KwUInt2: case TokenType::KwUInt3: case TokenType::KwUInt4:
        case TokenType::KwShort: case TokenType::KwShort2: case TokenType::KwShort3: case TokenType::KwShort4:
        case TokenType::KwUShort: case TokenType::KwUShort2: case TokenType::KwUShort3: case TokenType::KwUShort4:
        case TokenType::KwChar: case TokenType::KwUChar:
        case TokenType::KwFloat: case TokenType::KwFloat2: case TokenType::KwFloat3: case TokenType::KwFloat4:
        case TokenType::KwFloat2x2: case TokenType::KwFloat3x3: case TokenType::KwFloat4x4:
        case TokenType::KwHalf: case TokenType::KwHalf2: case TokenType::KwHalf3: case TokenType::KwHalf4:
        case TokenType::KwHalf2x2: case TokenType::KwHalf3x3: case TokenType::KwHalf4x4:
        case TokenType::KwFixed: case TokenType::KwFixed2: case TokenType::KwFixed3: case TokenType::KwFixed4:
            return true;
        default:
            return false;
    }
}

bool Parser::is_type_token(TokenType t) {
    if (is_value_type_token(t)) return true;
    switch (t) {
        case TokenType::KwVoid:
        case TokenType::KwUnsigned:
        case TokenType::KwSigned:
        case TokenType::KwSampler1D:
        case TokenType::KwSampler2D:
        case TokenType::KwSampler3D:
        case TokenType::KwSamplerCube:
            return true;
        default:
            return false;
    }
}

void Parser::error(const Token& tok, const std::string& message) {
    diagnostics_.push_back({
        Diagnostic::Level::Error,
        tok.line,
        tok.column,
        message
    });
}

std::string Parser::get_diagnostics_string() const {
    std::ostringstream ss;
    for (const auto& diag : diagnostics_) {
        ss << diag.format() << "\n";
    }
    return ss.str();
}

bool Parser::parse(TranslationUnit& out_tu) {
    while (!is_at_end()) {
        size_t before = current_;
        parse_top_level_declaration(out_tu);
        if (current_ == before && !is_at_end()) {
            advance();
        }
    }
    return diagnostics_.empty();
}

static bool eval_const_int_expr(const Expression* expr, int64_t& out_val) {
    if (!expr) return false;
    if (auto* lit = dynamic_cast<const LiteralExpr*>(expr)) {
        if (std::holds_alternative<int>(lit->value)) {
            out_val = std::get<int>(lit->value);
            return true;
        }
        if (std::holds_alternative<float>(lit->value)) {
            out_val = static_cast<int64_t>(std::get<float>(lit->value));
            return true;
        }
        if (std::holds_alternative<bool>(lit->value)) {
            out_val = std::get<bool>(lit->value) ? 1 : 0;
            return true;
        }
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        int64_t l = 0, r = 0;
        if (!eval_const_int_expr(bin->left.get(), l) || !eval_const_int_expr(bin->right.get(), r)) {
            return false;
        }
        switch (bin->op) {
            case BinaryOp::Add: out_val = l + r; return true;
            case BinaryOp::Sub: out_val = l - r; return true;
            case BinaryOp::Mul: out_val = l * r; return true;
            case BinaryOp::Div: out_val = (r != 0) ? (l / r) : 1; return true;
            case BinaryOp::Mod: out_val = (r != 0) ? (l % r) : 0; return true;
            case BinaryOp::Shl: out_val = l << (r & 31); return true;
            case BinaryOp::Shr: out_val = l >> (r & 31); return true;
            default: return false;
        }
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        int64_t v = 0;
        if (!eval_const_int_expr(un->operand.get(), v)) return false;
        if (un->op == UnaryOp::Negate) { out_val = -v; return true; }
    }
    return false;
}

uint32_t Parser::parse_array_dimension() {
    uint32_t dim = 1;
    if (!check(TokenType::CloseBracket)) {
        ExprPtr dim_expr = parse_expression();
        int64_t val = 1;
        if (eval_const_int_expr(dim_expr.get(), val) && val > 0) {
            dim = static_cast<uint32_t>(val);
        }
    }
    consume(TokenType::CloseBracket, "Expected ']' after array size");
    return dim;
}

DataType Parser::parse_type() {
    DataType dt;
    bool is_unsigned = false;
    while (check(TokenType::KwUnsigned) || check(TokenType::KwSigned)) {
        if (match(TokenType::KwUnsigned)) is_unsigned = true;
        else match(TokenType::KwSigned);
    }

    if (match(TokenType::KwVoid))             dt.base = BaseType::Void;
    else if (match(TokenType::KwBool))        dt.base = BaseType::Bool;
    else if (match(TokenType::KwBool2))       dt.base = BaseType::Bool2;
    else if (match(TokenType::KwBool3))       dt.base = BaseType::Bool3;
    else if (match(TokenType::KwBool4))       dt.base = BaseType::Bool4;
    else if (match(TokenType::KwInt))         dt.base = is_unsigned ? BaseType::UInt : BaseType::Int;
    else if (match(TokenType::KwInt2))        dt.base = is_unsigned ? BaseType::UInt2 : BaseType::Int2;
    else if (match(TokenType::KwInt3))        dt.base = is_unsigned ? BaseType::UInt3 : BaseType::Int3;
    else if (match(TokenType::KwInt4))        dt.base = is_unsigned ? BaseType::UInt4 : BaseType::Int4;
    else if (match(TokenType::KwUInt))        dt.base = BaseType::UInt;
    else if (match(TokenType::KwUInt2))       dt.base = BaseType::UInt2;
    else if (match(TokenType::KwUInt3))       dt.base = BaseType::UInt3;
    else if (match(TokenType::KwUInt4))       dt.base = BaseType::UInt4;
    else if (match(TokenType::KwShort))       dt.base = is_unsigned ? BaseType::UShort : BaseType::Short;
    else if (match(TokenType::KwShort2))      dt.base = is_unsigned ? BaseType::UShort2 : BaseType::Short2;
    else if (match(TokenType::KwShort3))      dt.base = is_unsigned ? BaseType::UShort3 : BaseType::Short3;
    else if (match(TokenType::KwShort4))      dt.base = is_unsigned ? BaseType::UShort4 : BaseType::Short4;
    else if (match(TokenType::KwUShort))      dt.base = BaseType::UShort;
    else if (match(TokenType::KwUShort2))     dt.base = BaseType::UShort2;
    else if (match(TokenType::KwUShort3))     dt.base = BaseType::UShort3;
    else if (match(TokenType::KwUShort4))     dt.base = BaseType::UShort4;
    else if (match(TokenType::KwChar))        dt.base = is_unsigned ? BaseType::UChar : BaseType::Char;
    else if (match(TokenType::KwUChar))       dt.base = BaseType::UChar;
    else if (match(TokenType::KwFloat))       dt.base = BaseType::Float;
    else if (match(TokenType::KwFloat2))      dt.base = BaseType::Float2;
    else if (match(TokenType::KwFloat3))      dt.base = BaseType::Float3;
    else if (match(TokenType::KwFloat4))      dt.base = BaseType::Float4;
    else if (match(TokenType::KwFloat2x2))    dt.base = BaseType::Float2x2;
    else if (match(TokenType::KwFloat3x3))    dt.base = BaseType::Float3x3;
    else if (match(TokenType::KwFloat4x4))    dt.base = BaseType::Float4x4;
    else if (match(TokenType::KwHalf))        dt.base = BaseType::Half;
    else if (match(TokenType::KwHalf2))       dt.base = BaseType::Half2;
    else if (match(TokenType::KwHalf3))       dt.base = BaseType::Half3;
    else if (match(TokenType::KwHalf4))       dt.base = BaseType::Half4;
    else if (match(TokenType::KwHalf2x2))     dt.base = BaseType::Half2x2;
    else if (match(TokenType::KwHalf3x3))     dt.base = BaseType::Half3x3;
    else if (match(TokenType::KwHalf4x4))     dt.base = BaseType::Half4x4;
    else if (match(TokenType::KwFixed))       dt.base = BaseType::Fixed;
    else if (match(TokenType::KwFixed2))      dt.base = BaseType::Fixed2;
    else if (match(TokenType::KwFixed3))      dt.base = BaseType::Fixed3;
    else if (match(TokenType::KwFixed4))      dt.base = BaseType::Fixed4;
    else if (match(TokenType::KwSampler1D))   dt.base = BaseType::Sampler1D;
    else if (match(TokenType::KwSampler2D))   dt.base = BaseType::Sampler2D;
    else if (match(TokenType::KwSampler3D))   dt.base = BaseType::Sampler3D;
    else if (match(TokenType::KwSamplerCube)) dt.base = BaseType::SamplerCube;
    else if (is_unsigned) {
        dt.base = BaseType::UInt;
    } else if (check(TokenType::Identifier)) {
        dt.base = BaseType::CustomStruct;
        dt.struct_name = advance().text;
    } else {
        error(peek(), "Expected data type identifier");
    }
    return dt;
}

std::string Parser::parse_optional_semantic() {
    if (match(TokenType::Colon)) {
        if (check(TokenType::Identifier)) {
            return advance().text;
        }
        error(peek(), "Expected semantic binding name after ':'");
    }
    return "";
}

void Parser::parse_top_level_declaration(TranslationUnit& tu) {
    bool is_static_or_const = false;
    bool is_varying = false;
    while (check(TokenType::KwConst) || check(TokenType::KwInline) ||
           check(TokenType::KwVarying) || check(TokenType::KwStatic) ||
           check(TokenType::KwIn) || check(TokenType::KwOut)) {
        if (match(TokenType::KwConst) || match(TokenType::KwStatic)) {
            is_static_or_const = true;
        } else if (match(TokenType::KwVarying)) {
            is_varying = true;
        } else {
            advance();
        }
    }
    (void)is_varying;

    if (match(TokenType::Semicolon)) {
        return;
    }

    if (check(TokenType::KwStruct)) {
        parse_struct_declaration(tu);
    } else if (check(TokenType::KwUniform)) {
        parse_uniform_or_global(tu);
    } else {
        DataType type = parse_type();
        if (type.base == BaseType::Void && is_at_end()) return;

        if (check(TokenType::Identifier)) {
            std::string name = advance().text;

            if (match(TokenType::OpenBracket)) {
                type.array_size = parse_array_dimension();
            }

            if (check(TokenType::OpenParen)) {
                parse_function_or_global(tu, type, name);
            } else {
                std::string semantic = parse_optional_semantic();
                if (match(TokenType::Assign)) {
                    VarDeclStmt global_var;
                    global_var.type = type;
                    global_var.name = name;
                    global_var.semantic = semantic;
                    global_var.initializer = parse_expression();
                    tu.globals.push_back(std::move(global_var));
                    while (match(TokenType::Comma)) {
                        std::string next_name = consume(TokenType::Identifier, "Expected global variable name").text;
                        VarDeclStmt extra;
                        extra.type = type;
                        extra.name = next_name;
                        if (match(TokenType::Assign)) {
                            extra.initializer = parse_expression();
                        }
                        tu.globals.push_back(std::move(extra));
                    }
                    consume(TokenType::Semicolon, "Expected ';' after global variable declaration");
                } else if (is_static_or_const) {
                    VarDeclStmt global_var;
                    global_var.type = type;
                    global_var.name = name;
                    global_var.semantic = semantic;
                    tu.globals.push_back(std::move(global_var));
                    consume(TokenType::Semicolon, "Expected ';' after declaration");
                } else {
                    tu.uniforms.push_back(UniformDecl{type, name, semantic, 0});
                    while (match(TokenType::Comma)) {
                        std::string next_name = consume(TokenType::Identifier, "Expected uniform variable name").text;
                        DataType next_type = type;
                        next_type.array_size = 1;
                        if (match(TokenType::OpenBracket)) {
                            next_type.array_size = parse_array_dimension();
                        }
                        std::string next_sem = parse_optional_semantic();
                        tu.uniforms.push_back(UniformDecl{next_type, next_name, next_sem, 0});
                    }
                    match(TokenType::Semicolon);
                }
            }
        } else {
            error(peek(), "Expected declaration identifier after type");
            advance();
        }
    }
}

void Parser::parse_struct_declaration(TranslationUnit& tu) {
    consume(TokenType::KwStruct, "Expected 'struct'");
    if (!check(TokenType::Identifier)) {
        error(peek(), "Expected struct name identifier");
        return;
    }
    std::string struct_name = advance().text;
    consume(TokenType::OpenBrace, "Expected '{' to begin struct body");

    StructDecl sdecl;
    sdecl.name = struct_name;

    while (!check(TokenType::CloseBrace) && !is_at_end()) {
        while (match(TokenType::KwConst)) {}
        DataType field_type = parse_type();
        do {
            if (!check(TokenType::Identifier)) {
                error(peek(), "Expected field name in struct");
                break;
            }
            std::string field_name = advance().text;
            DataType this_type = field_type;
            if (match(TokenType::OpenBracket)) {
                this_type.array_size = parse_array_dimension();
            }
            std::string semantic = parse_optional_semantic();
            sdecl.fields.push_back(StructField{this_type, field_name, semantic});
        } while (match(TokenType::Comma));
        consume(TokenType::Semicolon, "Expected ';' after struct field declaration");
    }

    consume(TokenType::CloseBrace, "Expected '}' after struct body");
    match(TokenType::Semicolon);
    tu.structs.push_back(std::move(sdecl));
}

void Parser::parse_uniform_or_global(TranslationUnit& tu) {
    consume(TokenType::KwUniform, "Expected 'uniform'");
    while (match(TokenType::KwConst)) {}
    DataType type = parse_type();
    do {
        if (!check(TokenType::Identifier)) {
            error(peek(), "Expected uniform variable name");
            break;
        }
        std::string name = advance().text;
        DataType this_type = type;
        if (match(TokenType::OpenBracket)) {
            this_type.array_size = parse_array_dimension();
        }
        std::string semantic = parse_optional_semantic();
        tu.uniforms.push_back(UniformDecl{this_type, name, semantic, 0});
    } while (match(TokenType::Comma));
    consume(TokenType::Semicolon, "Expected ';' after uniform declaration");
}

void Parser::parse_function_or_global(TranslationUnit& tu, DataType type, const std::string& name) {
    consume(TokenType::OpenParen, "Expected '(' for function parameter list");

    FunctionDecl fn;
    fn.return_type = type;
    fn.name = name;

    if (!check(TokenType::CloseParen)) {
        // Handle f(void) parameter list
        if (check(TokenType::KwVoid) && peek_next().type == TokenType::CloseParen) {
            advance();
        } else {
            do {
                bool is_in = true;
                bool is_out = false;
                bool is_unif = false;

                while (check(TokenType::KwIn) || check(TokenType::KwOut) ||
                       check(TokenType::KwInOut) || check(TokenType::KwUniform) ||
                       check(TokenType::KwConst) || check(TokenType::KwVarying)) {
                    if (match(TokenType::KwIn)) { is_in = true; is_out = false; }
                    else if (match(TokenType::KwOut)) { is_in = false; is_out = true; }
                    else if (match(TokenType::KwInOut)) { is_in = true; is_out = true; }
                    else if (match(TokenType::KwUniform)) { is_unif = true; }
                    else advance();
                }

                DataType param_type = parse_type();

                // Support post-type qualifiers e.g. `float4 inout Ambient` or `float4 out Pos`
                while (check(TokenType::KwIn) || check(TokenType::KwOut) || check(TokenType::KwInOut)) {
                    if (match(TokenType::KwIn)) { is_in = true; is_out = false; }
                    else if (match(TokenType::KwOut)) { is_in = false; is_out = true; }
                    else if (match(TokenType::KwInOut)) { is_in = true; is_out = true; }
                }

                std::string param_name;
                if (check(TokenType::Identifier)) {
                    param_name = advance().text;
                }

                if (match(TokenType::OpenBracket)) {
                    param_type.array_size = parse_array_dimension();
                }

                std::string semantic = parse_optional_semantic();
                if (match(TokenType::Assign)) {
                    // Optional default parameter value
                    parse_expression();
                }
                fn.parameters.push_back(ParameterDecl{param_type, param_name, semantic, is_in, is_out, is_unif});
            } while (match(TokenType::Comma));
        }
    }

    consume(TokenType::CloseParen, "Expected ')' closing parameter list");
    fn.return_semantic = parse_optional_semantic();

    if (check(TokenType::OpenBrace)) {
        auto body_stmt = parse_block_statement();
        fn.body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(body_stmt.release()));
    } else {
        consume(TokenType::Semicolon, "Expected function body '{' or ';' for prototype");
    }

    tu.functions.push_back(std::move(fn));
}

StmtPtr Parser::parse_statement() {
    if (match(TokenType::Semicolon))   return std::make_unique<BlockStmt>();
    if (check(TokenType::OpenBrace))   return parse_block_statement();
    if (check(TokenType::KwReturn))    return parse_return_statement();
    if (check(TokenType::KwIf))        return parse_if_statement();
    if (check(TokenType::KwFor))       return parse_for_statement();
    if (check(TokenType::KwWhile))     return parse_while_statement();
    if (check(TokenType::KwDo))        return parse_do_while_statement();
    if (match(TokenType::KwBreak)) {
        consume(TokenType::Semicolon, "Expected ';' after break");
        return std::make_unique<BreakStmt>();
    }
    if (match(TokenType::KwContinue)) {
        consume(TokenType::Semicolon, "Expected ';' after continue");
        return std::make_unique<ContinueStmt>();
    }
    if (match(TokenType::KwDiscard)) {
        consume(TokenType::Semicolon, "Expected ';' after discard");
        return std::make_unique<DiscardStmt>();
    }
    return parse_var_decl_or_assign();
}

StmtPtr Parser::parse_block_statement() {
    consume(TokenType::OpenBrace, "Expected '{'");
    auto block = std::make_unique<BlockStmt>();
    while (!check(TokenType::CloseBrace) && !is_at_end()) {
        size_t before = current_;
        block->statements.push_back(parse_statement());
        if (current_ == before && !is_at_end()) {
            advance();
        }
    }
    consume(TokenType::CloseBrace, "Expected '}'");
    return block;
}

StmtPtr Parser::parse_return_statement() {
    consume(TokenType::KwReturn, "Expected 'return'");
    auto ret = std::make_unique<ReturnStmt>();
    if (!check(TokenType::Semicolon)) {
        ret->value = parse_expression();
    }
    consume(TokenType::Semicolon, "Expected ';' after return statement");
    return ret;
}

StmtPtr Parser::parse_if_statement() {
    consume(TokenType::KwIf, "Expected 'if'");
    consume(TokenType::OpenParen, "Expected '(' after 'if'");
    ExprPtr cond = parse_expression();
    consume(TokenType::CloseParen, "Expected ')' after if condition");

    StmtPtr then_b = parse_statement();
    StmtPtr else_b = nullptr;
    if (match(TokenType::KwElse)) {
        else_b = parse_statement();
    }

    auto if_stmt = std::make_unique<IfStmt>();
    if_stmt->condition = std::move(cond);
    if_stmt->then_branch = std::move(then_b);
    if_stmt->else_branch = std::move(else_b);
    return if_stmt;
}

StmtPtr Parser::parse_for_statement() {
    consume(TokenType::KwFor, "Expected 'for'");
    consume(TokenType::OpenParen, "Expected '(' after 'for'");

    StmtPtr init_stmt = nullptr;
    if (!check(TokenType::Semicolon)) {
        init_stmt = parse_var_decl_or_assign();
    } else {
        advance(); // consume ';'
    }

    ExprPtr cond = nullptr;
    if (!check(TokenType::Semicolon)) {
        cond = parse_expression();
    }
    consume(TokenType::Semicolon, "Expected ';' after for condition");

    StmtPtr incr_stmt = nullptr;
    if (!check(TokenType::CloseParen)) {
        incr_stmt = parse_expr_or_assign_stmt();
    }
    consume(TokenType::CloseParen, "Expected ')' after for clause");

    StmtPtr body_stmt = parse_statement();

    auto for_stmt = std::make_unique<ForStmt>();
    for_stmt->init = std::move(init_stmt);
    for_stmt->condition = std::move(cond);
    for_stmt->increment = std::move(incr_stmt);
    for_stmt->body = std::move(body_stmt);
    return for_stmt;
}

StmtPtr Parser::parse_while_statement() {
    consume(TokenType::KwWhile, "Expected 'while'");
    consume(TokenType::OpenParen, "Expected '(' after 'while'");
    ExprPtr cond = parse_expression();
    consume(TokenType::CloseParen, "Expected ')' after while condition");
    StmtPtr body = parse_statement();

    auto while_stmt = std::make_unique<WhileStmt>();
    while_stmt->condition = std::move(cond);
    while_stmt->body = std::move(body);
    return while_stmt;
}

StmtPtr Parser::parse_do_while_statement() {
    consume(TokenType::KwDo, "Expected 'do'");
    StmtPtr body = parse_statement();
    consume(TokenType::KwWhile, "Expected 'while' after do body");
    consume(TokenType::OpenParen, "Expected '(' after 'while'");
    ExprPtr cond = parse_expression();
    consume(TokenType::CloseParen, "Expected ')' after do-while condition");
    consume(TokenType::Semicolon, "Expected ';' after do-while");

    auto dw = std::make_unique<DoWhileStmt>();
    dw->body = std::move(body);
    dw->condition = std::move(cond);
    return dw;
}

StmtPtr Parser::parse_expr_or_assign_stmt() {
    ExprPtr expr = parse_expression();

    BinaryOp op = BinaryOp::Equal;
    bool has_compound = false;
    if (match(TokenType::PlusAssign))       { op = BinaryOp::Add; has_compound = true; }
    else if (match(TokenType::MinusAssign)) { op = BinaryOp::Sub; has_compound = true; }
    else if (match(TokenType::StarAssign))  { op = BinaryOp::Mul; has_compound = true; }
    else if (match(TokenType::SlashAssign)) { op = BinaryOp::Div; has_compound = true; }

    if (has_compound) {
        ExprPtr rhs = parse_expression();
        auto assign = std::make_unique<AssignStmt>();
        assign->lvalue = std::move(expr);
        assign->rvalue = std::move(rhs);
        assign->compound_op = op;
        return assign;
    }

    // Unwrap BinaryExpr(BinaryOp::Assign, lvalue, rvalue) emitted by parse_assignment()
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get()); bin && bin->op == BinaryOp::Assign) {
        auto assign = std::make_unique<AssignStmt>();
        assign->lvalue = std::move(bin->left);
        assign->rvalue = std::move(bin->right);
        assign->compound_op = BinaryOp::Assign;
        return assign;
    }

    auto stmt = std::make_unique<AssignStmt>();
    stmt->lvalue = std::move(expr);
    stmt->compound_op = BinaryOp::Assign;
    return stmt;
}

StmtPtr Parser::parse_var_decl_or_assign() {
    while (match(TokenType::KwConst) || match(TokenType::KwStatic)) {}

    bool is_type = is_type_token(peek().type);
    if (!is_type && check(TokenType::Identifier) && peek_next().type == TokenType::Identifier) {
        is_type = true;
    }

    if (is_type) {
        DataType base_type = parse_type();
        std::vector<std::unique_ptr<VarDeclStmt>> decls;

        do {
            std::string name = consume(TokenType::Identifier, "Expected variable name").text;
            DataType this_type = base_type;
            this_type.array_size = 1;

            if (match(TokenType::OpenBracket)) {
                this_type.array_size = parse_array_dimension();
            }

            std::string semantic = parse_optional_semantic();

            auto decl = std::make_unique<VarDeclStmt>();
            decl->type = this_type;
            decl->name = name;
            decl->semantic = semantic;

            if (match(TokenType::Assign)) {
                decl->initializer = parse_expression();
            }
            decls.push_back(std::move(decl));
        } while (match(TokenType::Comma));

        consume(TokenType::Semicolon, "Expected ';' after variable declaration");

        if (decls.size() == 1) {
            return std::move(decls[0]);
        }
        auto block = std::make_unique<BlockStmt>();
        for (auto& d : decls) {
            block->statements.push_back(std::move(d));
        }
        return block;
    }

    StmtPtr stmt = parse_expr_or_assign_stmt();
    consume(TokenType::Semicolon, "Expected ';' after expression statement");
    return stmt;
}

ExprPtr Parser::parse_expression() {
    return parse_assignment();
}

ExprPtr Parser::parse_assignment() {
    ExprPtr expr = parse_ternary();
    if (match(TokenType::Assign)) {
        ExprPtr val = parse_assignment();
        return std::make_unique<BinaryExpr>(BinaryOp::Assign, std::move(expr), std::move(val));
    }
    return expr;
}

ExprPtr Parser::parse_ternary() {
    ExprPtr expr = parse_logical_or();
    if (match(TokenType::Question)) {
        ExprPtr then_e = parse_expression();
        consume(TokenType::Colon, "Expected ':' in ternary expression");
        ExprPtr else_e = parse_ternary();
        return std::make_unique<TernaryExpr>(std::move(expr), std::move(then_e), std::move(else_e));
    }
    return expr;
}

ExprPtr Parser::parse_logical_or() {
    ExprPtr expr = parse_logical_and();
    while (match(TokenType::OrOr)) {
        ExprPtr right = parse_logical_and();
        expr = std::make_unique<BinaryExpr>(BinaryOp::LogicalOr, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_logical_and() {
    ExprPtr expr = parse_bitwise_or();
    while (match(TokenType::AndAnd)) {
        ExprPtr right = parse_bitwise_or();
        expr = std::make_unique<BinaryExpr>(BinaryOp::LogicalAnd, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_bitwise_or() {
    ExprPtr expr = parse_bitwise_xor();
    while (match(TokenType::Pipe)) {
        ExprPtr right = parse_bitwise_xor();
        expr = std::make_unique<BinaryExpr>(BinaryOp::BitOr, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_bitwise_xor() {
    ExprPtr expr = parse_bitwise_and();
    while (match(TokenType::Caret)) {
        ExprPtr right = parse_bitwise_and();
        expr = std::make_unique<BinaryExpr>(BinaryOp::BitXor, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_bitwise_and() {
    ExprPtr expr = parse_equality();
    while (match(TokenType::Ampersand)) {
        ExprPtr right = parse_equality();
        expr = std::make_unique<BinaryExpr>(BinaryOp::BitAnd, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_equality() {
    ExprPtr expr = parse_relational();
    while (check(TokenType::Equal) || check(TokenType::NotEqual)) {
        TokenType op_type = advance().type;
        BinaryOp op = (op_type == TokenType::Equal) ? BinaryOp::Equal : BinaryOp::NotEqual;
        ExprPtr right = parse_relational();
        expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_relational() {
    ExprPtr expr = parse_shift();
    while (check(TokenType::Less) || check(TokenType::LessEqual) ||
           check(TokenType::Greater) || check(TokenType::GreaterEqual)) {
        TokenType op_type = advance().type;
        BinaryOp op = BinaryOp::Less;
        if (op_type == TokenType::LessEqual) op = BinaryOp::LessEqual;
        else if (op_type == TokenType::Greater) op = BinaryOp::Greater;
        else if (op_type == TokenType::GreaterEqual) op = BinaryOp::GreaterEqual;

        ExprPtr right = parse_shift();
        expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_shift() {
    ExprPtr expr = parse_additive();
    while (check(TokenType::Shl) || check(TokenType::Shr)) {
        TokenType op_type = advance().type;
        BinaryOp op = (op_type == TokenType::Shl) ? BinaryOp::Shl : BinaryOp::Shr;
        ExprPtr right = parse_additive();
        expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_additive() {
    ExprPtr expr = parse_multiplicative();
    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        TokenType op_type = advance().type;
        BinaryOp op = (op_type == TokenType::Plus) ? BinaryOp::Add : BinaryOp::Sub;
        ExprPtr right = parse_multiplicative();
        expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_multiplicative() {
    ExprPtr expr = parse_unary();
    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
        TokenType op_type = advance().type;
        BinaryOp op = (op_type == TokenType::Star) ? BinaryOp::Mul : ((op_type == TokenType::Slash) ? BinaryOp::Div : BinaryOp::Mod);
        ExprPtr right = parse_unary();
        expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenType::Plus)) {
        return parse_unary();
    }
    if (match(TokenType::Minus)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::Negate, std::move(opnd));
    }
    if (match(TokenType::Exclamation)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::LogicalNot, std::move(opnd));
    }
    if (match(TokenType::Tilde)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::BitNot, std::move(opnd));
    }
    if (match(TokenType::PlusPlus)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::Incr, std::move(opnd), false);
    }
    if (match(TokenType::MinusMinus)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::Decr, std::move(opnd), false);
    }

    // C-style cast e.g. (float4)x, (unsigned short)y, (int)z
    if (check(TokenType::OpenParen)) {
        TokenType t = peek_next().type;
        if (is_value_type_token(t) || t == TokenType::KwUnsigned || t == TokenType::KwSigned) {
            size_t saved = current_;
            advance(); // consume '('
            DataType dt = parse_type();
            if (match(TokenType::CloseParen)) {
                ExprPtr target = parse_unary();
                std::vector<ExprPtr> args;
                args.push_back(std::move(target));
                return std::make_unique<ConstructExpr>(dt, std::move(args));
            }
            current_ = saved;
        }
    }

    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    ExprPtr expr = parse_primary();

    while (true) {
        if (match(TokenType::Dot)) {
            std::string member = consume(TokenType::Identifier, "Expected member name or swizzle mask").text;
            expr = std::make_unique<SwizzleExpr>(std::move(expr), member);
        } else if (match(TokenType::OpenBracket)) {
            ExprPtr idx = parse_expression();
            consume(TokenType::CloseBracket, "Expected ']'");
            expr = std::make_unique<ArrayAccessExpr>(std::move(expr), std::move(idx));
        } else if (match(TokenType::PlusPlus)) {
            expr = std::make_unique<UnaryExpr>(UnaryOp::Incr, std::move(expr), true);
        } else if (match(TokenType::MinusMinus)) {
            expr = std::make_unique<UnaryExpr>(UnaryOp::Decr, std::move(expr), true);
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::parse_primary() {
    if (match(TokenType::NumberLiteral)) {
        float val = static_cast<float>(previous().number_value);
        return std::make_unique<LiteralExpr>(val);
    }

    if (match(TokenType::OpenParen)) {
        ExprPtr expr = parse_expression();
        consume(TokenType::CloseParen, "Expected ')' after parenthesized expression");
        return expr;
    }

    // Type constructor (e.g. float4(x, y, z, 1.0), short2(a, b), half2(0.0, 0.0))
    if (is_value_type_token(peek().type)) {
        DataType dt = parse_type();
        consume(TokenType::OpenParen, "Expected '(' in constructor");
        std::vector<ExprPtr> args;
        if (!check(TokenType::CloseParen)) {
            do {
                args.push_back(parse_expression());
            } while (match(TokenType::Comma));
        }
        consume(TokenType::CloseParen, "Expected ')' after constructor arguments");
        return std::make_unique<ConstructExpr>(dt, std::move(args));
    }

    // Function calls or identifier
    if (check(TokenType::Identifier)) {
        std::string id = advance().text;

        if (id == "true") return std::make_unique<LiteralExpr>(true);
        if (id == "false") return std::make_unique<LiteralExpr>(false);

        // Template argument check e.g. bit_cast<short2>(x) only when followed by (...)
        if (check(TokenType::Less) && (is_type_token(peek_next().type) || peek_next().type == TokenType::Identifier)) {
            size_t saved = current_;
            advance(); // consume '<'
            int angle_depth = 1;
            while (!is_at_end() && angle_depth > 0) {
                if (check(TokenType::Less)) angle_depth++;
                else if (check(TokenType::Greater)) angle_depth--;
                advance();
            }
            if (!check(TokenType::OpenParen)) {
                // Not a template call, rewind so '<' is parsed as comparison
                current_ = saved;
            }
        }

        if (match(TokenType::OpenParen)) {
            std::vector<ExprPtr> args;
            if (!check(TokenType::CloseParen)) {
                do {
                    args.push_back(parse_expression());
                } while (match(TokenType::Comma));
            }
            consume(TokenType::CloseParen, "Expected ')' after argument list");
            return std::make_unique<CallExpr>(id, std::move(args));
        }

        return std::make_unique<VariableExpr>(id);
    }

    error(peek(), "Unexpected token in primary expression: " + peek().text);
    advance();
    return std::make_unique<LiteralExpr>(0.0f);
}

} // namespace open_shacc::frontend
