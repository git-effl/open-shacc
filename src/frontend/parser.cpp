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
    if (match(TokenType::KwVoid))          dt.base = BaseType::Void;
    else if (match(TokenType::KwBool))     dt.base = BaseType::Bool;
    else if (match(TokenType::KwInt))      dt.base = BaseType::Int;
    else if (match(TokenType::KwShort))    dt.base = BaseType::Short;
    else if (match(TokenType::KwUShort))   dt.base = BaseType::UShort;
    else if (match(TokenType::KwChar))     dt.base = BaseType::Char;
    else if (match(TokenType::KwUChar))    dt.base = BaseType::UChar;
    else if (match(TokenType::KwFloat))    dt.base = BaseType::Float;
    else if (match(TokenType::KwFloat2))   dt.base = BaseType::Float2;
    else if (match(TokenType::KwFloat3))   dt.base = BaseType::Float3;
    else if (match(TokenType::KwFloat4))   dt.base = BaseType::Float4;
    else if (match(TokenType::KwFloat2x2)) dt.base = BaseType::Float2x2;
    else if (match(TokenType::KwFloat3x3)) dt.base = BaseType::Float3x3;
    else if (match(TokenType::KwFloat4x4)) dt.base = BaseType::Float4x4;
    else if (match(TokenType::KwHalf))     dt.base = BaseType::Half;
    else if (match(TokenType::KwHalf2))    dt.base = BaseType::Half2;
    else if (match(TokenType::KwHalf3))    dt.base = BaseType::Half3;
    else if (match(TokenType::KwHalf4))    dt.base = BaseType::Half4;
    else if (match(TokenType::KwHalf2x2))  dt.base = BaseType::Half2x2;
    else if (match(TokenType::KwHalf3x3))  dt.base = BaseType::Half3x3;
    else if (match(TokenType::KwHalf4x4))  dt.base = BaseType::Half4x4;
    else if (match(TokenType::KwFixed))    dt.base = BaseType::Fixed;
    else if (match(TokenType::KwSampler2D)) dt.base = BaseType::Sampler2D;
    else if (match(TokenType::KwSamplerCube)) dt.base = BaseType::SamplerCube;
    else if (check(TokenType::Identifier)) {
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
    // Skip optional qualifiers
    while (match(TokenType::KwConst) || match(TokenType::KwInline) || match(TokenType::KwVarying)) {
        match(TokenType::KwIn);
        match(TokenType::KwOut);
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

            // Optional array dimensions e.g., float4 param[16]
            if (match(TokenType::OpenBracket)) {
                if (check(TokenType::NumberLiteral)) {
                    type.array_size = static_cast<uint32_t>(advance().number_value);
                }
                consume(TokenType::CloseBracket, "Expected ']' after array size");
            }

            if (check(TokenType::OpenParen)) {
                parse_function_or_global(tu, type, name);
            } else {
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

        if (match(TokenType::OpenBracket)) {
            if (check(TokenType::NumberLiteral)) {
                field_type.array_size = static_cast<uint32_t>(advance().number_value);
            }
            consume(TokenType::CloseBracket, "Expected ']'");
        }

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

    if (match(TokenType::OpenBracket)) {
        if (check(TokenType::NumberLiteral)) {
            type.array_size = static_cast<uint32_t>(advance().number_value);
        }
        consume(TokenType::CloseBracket, "Expected ']' after array size");
    }

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
            bool is_unif = false;

            while (check(TokenType::KwIn) || check(TokenType::KwOut) || check(TokenType::KwInOut) || check(TokenType::KwUniform) || check(TokenType::KwConst)) {
                if (match(TokenType::KwIn)) { is_in = true; is_out = false; }
                else if (match(TokenType::KwOut)) { is_in = false; is_out = true; }
                else if (match(TokenType::KwInOut)) { is_in = true; is_out = true; }
                else if (match(TokenType::KwUniform)) { is_unif = true; }
                else if (match(TokenType::KwConst)) {}
            }

            DataType param_type = parse_type();

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
                if (check(TokenType::NumberLiteral)) {
                    param_type.array_size = static_cast<uint32_t>(advance().number_value);
                }
                consume(TokenType::CloseBracket, "Expected ']'");
            }

            std::string semantic = parse_optional_semantic();
            fn.parameters.push_back(ParameterDecl{param_type, param_name, semantic, is_in, is_out, is_unif});
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
    if (check(TokenType::OpenBrace))   return parse_block_statement();
    if (check(TokenType::KwReturn))    return parse_return_statement();
    if (check(TokenType::KwIf))        return parse_if_statement();
    if (check(TokenType::KwFor))       return parse_for_statement();
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
        ExprPtr expr = parse_expression();
        auto stmt = std::make_unique<AssignStmt>();
        stmt->lvalue = std::move(expr);
        incr_stmt = std::move(stmt);
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

StmtPtr Parser::parse_var_decl_or_assign() {
    bool is_type = check(TokenType::KwFloat) || check(TokenType::KwFloat2) ||
                   check(TokenType::KwFloat3) || check(TokenType::KwFloat4) ||
                   check(TokenType::KwFloat2x2) || check(TokenType::KwFloat3x3) || check(TokenType::KwFloat4x4) ||
                   check(TokenType::KwHalf) || check(TokenType::KwHalf2) ||
                   check(TokenType::KwHalf3) || check(TokenType::KwHalf4) ||
                   check(TokenType::KwInt) || check(TokenType::KwShort) || check(TokenType::KwUShort) ||
                   check(TokenType::KwChar) || check(TokenType::KwUChar) || check(TokenType::KwBool);

    if (!is_type && check(TokenType::Identifier) && current_ + 1 < tokens_.size() &&
        tokens_[current_ + 1].type == TokenType::Identifier) {
        is_type = true;
    }

    if (is_type) {
        DataType type = parse_type();
        std::string name = consume(TokenType::Identifier, "Expected variable name").text;

        if (match(TokenType::OpenBracket)) {
            if (check(TokenType::NumberLiteral)) {
                type.array_size = static_cast<uint32_t>(advance().number_value);
            }
            consume(TokenType::CloseBracket, "Expected ']'");
        }

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

    ExprPtr expr = parse_expression();

    // Check compound assignment e.g. x += y; x++;
    BinaryOp op = BinaryOp::Equal;
    if (match(TokenType::Assign))          op = BinaryOp::Equal;
    else if (match(TokenType::PlusAssign))  op = BinaryOp::Add;
    else if (match(TokenType::MinusAssign)) op = BinaryOp::Sub;
    else if (match(TokenType::StarAssign))  op = BinaryOp::Mul;
    else if (match(TokenType::SlashAssign)) op = BinaryOp::Div;

    if (op != BinaryOp::Equal) {
        ExprPtr rhs = parse_expression();
        auto assign = std::make_unique<AssignStmt>();
        assign->lvalue = std::move(expr);
        assign->rvalue = std::move(rhs);
        assign->compound_op = op;
        consume(TokenType::Semicolon, "Expected ';' after assignment");
        return assign;
    }

    consume(TokenType::Semicolon, "Expected ';' after expression statement");
    auto stmt = std::make_unique<AssignStmt>();
    stmt->lvalue = std::move(expr);
    return stmt;
}

ExprPtr Parser::parse_expression() {
    return parse_assignment();
}

ExprPtr Parser::parse_assignment() {
    ExprPtr expr = parse_ternary();
    if (match(TokenType::Assign)) {
        ExprPtr val = parse_assignment();
        return std::make_unique<BinaryExpr>(BinaryOp::Equal, std::move(expr), std::move(val));
    }
    return expr;
}

ExprPtr Parser::parse_ternary() {
    ExprPtr expr = parse_logical_or();
    if (match(TokenType::Question)) {
        ExprPtr then_e = parse_expression();
        consume(TokenType::Colon, "Expected ':' in ternary expression");
        ExprPtr else_e = parse_expression();
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

    // C-style cast e.g. (float4)x or (int)y
    if (check(TokenType::OpenParen) && current_ + 1 < tokens_.size()) {
        TokenType t = tokens_[current_ + 1].type;
        if (t == TokenType::KwFloat || t == TokenType::KwFloat2 || t == TokenType::KwFloat3 || t == TokenType::KwFloat4 ||
            t == TokenType::KwHalf || t == TokenType::KwHalf2 || t == TokenType::KwHalf3 || t == TokenType::KwHalf4 ||
            t == TokenType::KwInt || t == TokenType::KwShort || t == TokenType::KwUShort || t == TokenType::KwBool) {
            advance(); // consume '('
            DataType dt = parse_type();
            consume(TokenType::CloseParen, "Expected ')' after cast type");
            ExprPtr target = parse_unary();
            std::vector<ExprPtr> args;
            args.push_back(std::move(target));
            return std::make_unique<ConstructExpr>(dt, std::move(args));
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

    // Type constructor (e.g. float4(x, y, z, 1.0) or half2(0.0, 0.0))
    if (check(TokenType::KwFloat) || check(TokenType::KwFloat2) || check(TokenType::KwFloat3) || check(TokenType::KwFloat4) ||
        check(TokenType::KwHalf) || check(TokenType::KwHalf2) || check(TokenType::KwHalf3) || check(TokenType::KwHalf4) ||
        check(TokenType::KwInt) || check(TokenType::KwShort) || check(TokenType::KwUShort) || check(TokenType::KwBool)) {
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

        // Template argument check e.g. bit_cast<short2>(x)
        if (check(TokenType::Less)) {
            advance(); // consume '<'
            parse_type(); // type inside template
            consume(TokenType::Greater, "Expected '>' in template call");
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
