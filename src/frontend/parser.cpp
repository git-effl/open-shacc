/**
 * @file parser.cpp
 * @brief Clean-room Cg/HLSL AST parser implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "parser.hpp"
#include <algorithm>

namespace open_shacc::frontend {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::peek() const {
    if (current_ >= tokens_.size()) return tokens_.back();
    return tokens_[current_];
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
        parse_top_level_declaration(out_tu);
    }
    return diagnostics_.empty();
}

DataType Parser::parse_type() {
    DataType dt;
    if (match(TokenType::KwVoid)) {
        dt.base = BaseType::Void;
    } else if (match(TokenType::KwBool)) {
        dt.base = BaseType::Bool;
    } else if (match(TokenType::KwInt)) {
        dt.base = BaseType::Int;
    } else if (match(TokenType::KwFloat)) {
        dt.base = BaseType::Float;
    } else if (match(TokenType::KwFloat2)) {
        dt.base = BaseType::Float2;
    } else if (match(TokenType::KwFloat3)) {
        dt.base = BaseType::Float3;
    } else if (match(TokenType::KwFloat4)) {
        dt.base = BaseType::Float4;
    } else if (match(TokenType::KwFloat4x4)) {
        dt.base = BaseType::Float4x4;
    } else if (match(TokenType::KwSampler2D)) {
        dt.base = BaseType::Sampler2D;
    } else if (check(TokenType::Identifier)) {
        // Struct type reference
        dt.base = BaseType::CustomStruct;
        dt.struct_name = advance().text;
    } else {
        error(peek(), "Expected data type identifier (float, float4, float4x4, etc.)");
    }
    return dt;
}

std::string Parser::parse_optional_semantic() {
    if (match(TokenType::Colon)) {
        if (check(TokenType::Identifier)) {
            return advance().text;
        }
        error(peek(), "Expected semantic binding name after ':' (e.g. POSITION, TEXCOORD0)");
    }
    return "";
}

void Parser::parse_top_level_declaration(TranslationUnit& tu) {
    if (check(TokenType::KwStruct)) {
        parse_struct_declaration(tu);
    } else if (check(TokenType::KwUniform)) {
        parse_uniform_or_global(tu);
    } else {
        // Could be a function or global variable
        DataType type = parse_type();
        if (type.base == BaseType::Void && is_at_end()) return;

        if (check(TokenType::Identifier)) {
            std::string name = advance().text;
            if (check(TokenType::OpenParen)) {
                parse_function_or_global(tu, type, name);
            } else {
                // Global variable or uniform without uniform keyword
                std::string semantic = parse_optional_semantic();
                tu.uniforms.push_back(UniformDecl{type, name, semantic, 0});
                match(TokenType::Semicolon);
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
        DataType field_type = parse_type();
        if (!check(TokenType::Identifier)) {
            error(peek(), "Expected field name in struct");
            break;
        }
        std::string field_name = advance().text;
        std::string semantic = parse_optional_semantic();
        consume(TokenType::Semicolon, "Expected ';' after struct field declaration");
        sdecl.fields.push_back(StructField{field_type, field_name, semantic});
    }

    consume(TokenType::CloseBrace, "Expected '}' after struct body");
    consume(TokenType::Semicolon, "Expected ';' after struct declaration");
    tu.structs.push_back(std::move(sdecl));
}

void Parser::parse_uniform_or_global(TranslationUnit& tu) {
    consume(TokenType::KwUniform, "Expected 'uniform'");
    DataType type = parse_type();
    if (!check(TokenType::Identifier)) {
        error(peek(), "Expected uniform variable name");
        return;
    }
    std::string name = advance().text;
    std::string semantic = parse_optional_semantic();
    consume(TokenType::Semicolon, "Expected ';' after uniform declaration");

    tu.uniforms.push_back(UniformDecl{type, name, semantic, 0});
}

void Parser::parse_function_or_global(TranslationUnit& tu, DataType type, const std::string& name) {
    consume(TokenType::OpenParen, "Expected '(' for function parameter list");

    FunctionDecl fn;
    fn.return_type = type;
    fn.name = name;

    if (!check(TokenType::CloseParen)) {
        do {
            bool is_in = true;
            bool is_out = false;
            if (match(TokenType::KwIn)) { is_in = true; is_out = false; }
            else if (match(TokenType::KwOut)) { is_in = false; is_out = true; }
            else if (match(TokenType::KwInOut)) { is_in = true; is_out = true; }

            DataType param_type = parse_type();
            std::string param_name;
            if (check(TokenType::Identifier)) {
                param_name = advance().text;
            }
            std::string semantic = parse_optional_semantic();
            fn.parameters.push_back(ParameterDecl{param_type, param_name, semantic, is_in, is_out});
        } while (match(TokenType::Comma));
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
    if (check(TokenType::OpenBrace)) {
        return parse_block_statement();
    }
    if (check(TokenType::KwReturn)) {
        return parse_return_statement();
    }
    return parse_var_decl_or_assign();
}

StmtPtr Parser::parse_block_statement() {
    consume(TokenType::OpenBrace, "Expected '{'");
    auto block = std::make_unique<BlockStmt>();
    while (!check(TokenType::CloseBrace) && !is_at_end()) {
        block->statements.push_back(parse_statement());
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

StmtPtr Parser::parse_var_decl_or_assign() {
    // Check if starts with a known type keyword
    bool is_type = check(TokenType::KwFloat) || check(TokenType::KwFloat2) ||
                   check(TokenType::KwFloat3) || check(TokenType::KwFloat4) ||
                   check(TokenType::KwFloat4x4) || check(TokenType::KwInt) ||
                   check(TokenType::KwBool);

    // If identifier followed by another identifier, could be custom struct type declaration
    if (!is_type && check(TokenType::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].type == TokenType::Identifier) {
        is_type = true;
    }

    if (is_type) {
        DataType type = parse_type();
        std::string name = consume(TokenType::Identifier, "Expected variable name").text;
        std::string semantic = parse_optional_semantic();

        auto decl = std::make_unique<VarDeclStmt>();
        decl->type = type;
        decl->name = name;
        decl->semantic = semantic;

        if (match(TokenType::Assign)) {
            decl->initializer = parse_expression();
        }
        consume(TokenType::Semicolon, "Expected ';' after variable declaration");
        return decl;
    }

    // Assignment or expression statement
    ExprPtr expr = parse_expression();
    consume(TokenType::Semicolon, "Expected ';' after expression statement");

    auto stmt = std::make_unique<AssignStmt>();
    stmt->lvalue = std::move(expr);
    return stmt;
}

ExprPtr Parser::parse_expression() {
    return parse_assignment();
}

ExprPtr Parser::parse_assignment() {
    ExprPtr expr = parse_equality();
    if (match(TokenType::Assign)) {
        ExprPtr val = parse_assignment();
        auto assign = std::make_unique<BinaryExpr>(BinaryOp::Equal, std::move(expr), std::move(val));
        return assign;
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
    ExprPtr expr = parse_additive();
    while (check(TokenType::Less) || check(TokenType::LessEqual) ||
           check(TokenType::Greater) || check(TokenType::GreaterEqual)) {
        TokenType op_type = advance().type;
        BinaryOp op = (op_type == TokenType::Less || op_type == TokenType::LessEqual) ? BinaryOp::Less : BinaryOp::Greater;
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
    while (check(TokenType::Star) || check(TokenType::Slash)) {
        TokenType op_type = advance().type;
        BinaryOp op = (op_type == TokenType::Star) ? BinaryOp::Mul : BinaryOp::Div;
        ExprPtr right = parse_unary();
        expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenType::Minus)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::Negate, std::move(opnd));
    }
    if (match(TokenType::Exclamation)) {
        ExprPtr opnd = parse_unary();
        return std::make_unique<UnaryExpr>(UnaryOp::LogicalNot, std::move(opnd));
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    ExprPtr expr = parse_primary();

    while (true) {
        if (match(TokenType::Dot)) {
            // Swizzle or struct member access
            std::string member = consume(TokenType::Identifier, "Expected member name or swizzle mask").text;
            expr = std::make_unique<SwizzleExpr>(std::move(expr), member);
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

    // Type constructor (e.g. float4(x, y, z, 1.0) or float2(0.0, 0.0))
    if (check(TokenType::KwFloat) || check(TokenType::KwFloat2) ||
        check(TokenType::KwFloat3) || check(TokenType::KwFloat4)) {
        DataType dt = parse_type();
        consume(TokenType::OpenParen, "Expected '(' in vector constructor");
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

        if (match(TokenType::OpenParen)) {
            // Function call (e.g. mul(matrix, vector), dot(a, b), dp3, dp4, normalize)
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
