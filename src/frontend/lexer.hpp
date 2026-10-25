/**
 * @file lexer.hpp
 * @brief Lexical analyzer for NVIDIA Cg and Microsoft HLSL shader syntax.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_LEXER_HPP
#define OPEN_SHACC_LEXER_HPP

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace open_shacc::frontend {

enum class TokenType {
    EndOfFile,
    Identifier,
    NumberLiteral,
    StringLiteral,

    // Keywords
    KwStruct,
    KwUniform,
    KwConst,
    KwIn,
    KwOut,
    KwInOut,
    KwReturn,
    KwVoid,
    KwBool,
    KwInt,
    KwFloat,
    KwFloat2,
    KwFloat3,
    KwFloat4,
    KwFloat4x4,
    KwSampler2D,

    // Symbols & Operators
    OpenParen,       // (
    CloseParen,      // )
    OpenBrace,       // {
    CloseBrace,      // }
    OpenBracket,     // [
    CloseBracket,    // ]
    Semicolon,       // ;
    Colon,           // :
    Comma,           // ,
    Dot,             // .
    Assign,          // =
    Plus,            // +
    Minus,           // -
    Star,            // *
    Slash,           // /
    Equal,           // ==
    NotEqual,        // !=
    Less,            // <
    Greater,         // >
    LessEqual,       // <=
    GreaterEqual,    // >=
    Exclamation      // !
};

struct Token {
    TokenType type = TokenType::EndOfFile;
    std::string text;
    double number_value = 0.0;
    uint32_t line = 1;
    uint32_t column = 1;
};

class Lexer {
public:
    explicit Lexer(std::string_view source);
    std::vector<Token> tokenize();

private:
    char peek() const;
    char peek_next() const;
    char advance();
    bool match(char expected);
    bool is_at_end() const;

    void skip_whitespace_and_comments();
    Token scan_identifier_or_keyword();
    Token scan_number();
    Token scan_string();

    std::string_view source_;
    size_t cursor_ = 0;
    uint32_t line_ = 1;
    uint32_t column_ = 1;
};

} // namespace open_shacc::frontend

#endif // OPEN_SHACC_LEXER_HPP
