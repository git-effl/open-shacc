/**
 * @file parser.hpp
 * @brief Clean-room Recursive Descent Parser for Cg / HLSL Shader Source.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_PARSER_HPP
#define OPEN_SHACC_PARSER_HPP

#include "lexer.hpp"
#include "ast.hpp"
#include <string>
#include <vector>
#include <memory>
#include <sstream>

namespace open_shacc::frontend {

struct Diagnostic {
    enum class Level { Error, Warning, Info };
    Level level = Level::Error;
    uint32_t line = 1;
    uint32_t column = 1;
    std::string message;

    [[nodiscard]] std::string format() const {
        std::ostringstream ss;
        ss << "[" << (level == Level::Error ? "ERROR" : (level == Level::Warning ? "WARNING" : "INFO"))
           << "] (line " << line << ", col " << column << "): " << message;
        return ss.str();
    }
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    bool parse(TranslationUnit& out_tu);
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    [[nodiscard]] std::string get_diagnostics_string() const;

private:
    const Token& peek() const;
    const Token& previous() const;
    bool is_at_end() const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& consume(TokenType type, const std::string& error_message);

    void error(const Token& tok, const std::string& message);

    // Parsing rules
    void parse_top_level_declaration(TranslationUnit& tu);
    void parse_struct_declaration(TranslationUnit& tu);
    void parse_uniform_or_global(TranslationUnit& tu);
    void parse_function_or_global(TranslationUnit& tu, DataType type, const std::string& name);

    DataType parse_type();
    std::string parse_optional_semantic();

    // Statement parsers
    StmtPtr parse_statement();
    StmtPtr parse_block_statement();
    StmtPtr parse_return_statement();
    StmtPtr parse_var_decl_or_assign();

    // Expression parsers
    ExprPtr parse_expression();
    ExprPtr parse_assignment();
    ExprPtr parse_equality();
    ExprPtr parse_relational();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_primary();

    std::vector<Token> tokens_;
    size_t current_ = 0;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace open_shacc::frontend

#endif // OPEN_SHACC_PARSER_HPP
