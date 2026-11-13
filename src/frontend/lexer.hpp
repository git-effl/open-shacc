/**
 * @file lexer.hpp
 * @brief Lexical analyzer for NVIDIA Cg and Microsoft HLSL shader syntax.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
    KwStruct, KwUniform, KwConst, KwVarying, KwStatic, KwUnsigned, KwSigned,
    KwIn, KwOut, KwInOut, KwInline,
    KwReturn, KwDiscard, KwIf, KwElse, KwFor, KwWhile, KwDo, KwBreak, KwContinue,
    KwVoid, KwBool, KwBool2, KwBool3, KwBool4,
    KwInt, KwInt2, KwInt3, KwInt4,
    KwUInt, KwUInt2, KwUInt3, KwUInt4,
    KwShort, KwShort2, KwShort3, KwShort4,
    KwUShort, KwUShort2, KwUShort3, KwUShort4,
    KwChar, KwUChar,
    KwFloat, KwFloat2, KwFloat3, KwFloat4, KwFloat2x2, KwFloat3x3, KwFloat4x4,
    KwHalf, KwHalf2, KwHalf3, KwHalf4, KwHalf2x2, KwHalf3x3, KwHalf4x4,
    KwFixed, KwFixed2, KwFixed3, KwFixed4,
    KwSampler1D, KwSampler2D, KwSampler3D, KwSamplerCube,

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
    Question,        // ?
    Assign,          // =
    Plus,            // +
    Minus,           // -
    Star,            // *
    Slash,           // /
    Percent,         // %
    PlusPlus,        // ++
    MinusMinus,      // --
    PlusAssign,      // +=
    MinusAssign,     // -=
    StarAssign,      // *=
    SlashAssign,     // /=
    Equal,           // ==
    NotEqual,        // !=
    Less,            // <
    Greater,         // >
    LessEqual,       // <=
    GreaterEqual,    // >=
    Ampersand,       // &
    Pipe,            // |
    Caret,           // ^
    Tilde,           // ~
    AndAnd,          // &&
    OrOr,            // ||
    Shl,             // <<
    Shr,             // >>
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
