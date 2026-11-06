/**
 * @file lexer.cpp
 * @brief Implementation of the Cg/HLSL tokenizer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "lexer.hpp"
#include <cctype>
#include <unordered_map>
#include <cstdlib>

namespace open_shacc::frontend {

static const std::unordered_map<std::string_view, TokenType> kKeywords = {
    {"struct",      TokenType::KwStruct},
    {"uniform",     TokenType::KwUniform},
    {"const",       TokenType::KwConst},
    {"varying",     TokenType::KwVarying},
    {"in",          TokenType::KwIn},
    {"out",         TokenType::KwOut},
    {"inout",       TokenType::KwInOut},
    {"inline",      TokenType::KwInline},
    {"return",      TokenType::KwReturn},
    {"discard",     TokenType::KwDiscard},
    {"if",          TokenType::KwIf},
    {"else",        TokenType::KwElse},
    {"for",         TokenType::KwFor},
    {"while",       TokenType::KwWhile},
    {"void",        TokenType::KwVoid},
    {"bool",        TokenType::KwBool},
    {"int",         TokenType::KwInt},
    {"short",       TokenType::KwShort},
    {"ushort",      TokenType::KwUShort},
    {"char",        TokenType::KwChar},
    {"uchar",       TokenType::KwUChar},
    {"float",       TokenType::KwFloat},
    {"float2",      TokenType::KwFloat2},
    {"float3",      TokenType::KwFloat3},
    {"float4",      TokenType::KwFloat4},
    {"float2x2",    TokenType::KwFloat2x2},
    {"float3x3",    TokenType::KwFloat3x3},
    {"float4x4",    TokenType::KwFloat4x4},
    {"half",        TokenType::KwHalf},
    {"half2",       TokenType::KwHalf2},
    {"half3",       TokenType::KwHalf3},
    {"half4",       TokenType::KwHalf4},
    {"half2x2",     TokenType::KwHalf2x2},
    {"half3x3",     TokenType::KwHalf3x3},
    {"half4x4",     TokenType::KwHalf4x4},
    {"fixed",       TokenType::KwFixed},
    {"sampler2D",   TokenType::KwSampler2D},
    {"samplerCUBE", TokenType::KwSamplerCube},
    {"sampler_cube",TokenType::KwSamplerCube}
};

Lexer::Lexer(std::string_view source) : source_(source) {}

bool Lexer::is_at_end() const {
    return cursor_ >= source_.size();
}

char Lexer::peek() const {
    if (is_at_end()) return '\0';
    return source_[cursor_];
}

char Lexer::peek_next() const {
    if (cursor_ + 1 >= source_.size()) return '\0';
    return source_[cursor_ + 1];
}

char Lexer::advance() {
    if (is_at_end()) return '\0';
    char c = source_[cursor_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (is_at_end() || source_[cursor_] != expected) return false;
    advance();
    return true;
}

void Lexer::skip_whitespace_and_comments() {
    while (!is_at_end()) {
        char c = peek();
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
        } else if (c == '/' && peek_next() == '/') {
            advance(); advance();
            while (!is_at_end() && peek() != '\n') {
                advance();
            }
        } else if (c == '/' && peek_next() == '*') {
            advance(); advance();
            while (!is_at_end()) {
                if (peek() == '*' && peek_next() == '/') {
                    advance(); advance();
                    break;
                }
                advance();
            }
        } else {
            break;
        }
    }
}

Token Lexer::scan_identifier_or_keyword() {
    uint32_t start_col = column_;
    uint32_t start_line = line_;
    size_t start_idx = cursor_;

    while (!is_at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        advance();
    }

    std::string_view text = source_.substr(start_idx, cursor_ - start_idx);
    Token tok;
    tok.line = start_line;
    tok.column = start_col;
    tok.text = std::string(text);

    auto it = kKeywords.find(text);
    if (it != kKeywords.end()) {
        tok.type = it->second;
    } else {
        tok.type = TokenType::Identifier;
    }
    return tok;
}

Token Lexer::scan_number() {
    uint32_t start_col = column_;
    uint32_t start_line = line_;
    size_t start_idx = cursor_;

    // Hex literal check: 0x...
    if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
        advance(); advance();
        while (!is_at_end() && std::isxdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        std::string_view text = source_.substr(start_idx, cursor_ - start_idx);
        Token tok;
        tok.type = TokenType::NumberLiteral;
        tok.line = start_line;
        tok.column = start_col;
        tok.text = std::string(text);
        tok.number_value = static_cast<double>(std::strtoul(tok.text.c_str(), nullptr, 16));
        return tok;
    }

    while (!is_at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek_next()))) {
        advance(); // consume '.'
        while (!is_at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    if (peek() == 'e' || peek() == 'E') {
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (!is_at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    if (peek() == 'f' || peek() == 'F') {
        advance();
    }

    std::string_view text = source_.substr(start_idx, cursor_ - start_idx);
    Token tok;
    tok.type = TokenType::NumberLiteral;
    tok.line = start_line;
    tok.column = start_col;
    tok.text = std::string(text);
    tok.number_value = std::strtod(tok.text.c_str(), nullptr);
    return tok;
}

Token Lexer::scan_string() {
    uint32_t start_col = column_;
    uint32_t start_line = line_;
    advance(); // Consume leading "

    std::string content;
    while (!is_at_end() && peek() != '"') {
        if (peek() == '\\') advance();
        content.push_back(advance());
    }
    if (!is_at_end()) advance(); // Consume trailing "

    Token tok;
    tok.type = TokenType::StringLiteral;
    tok.line = start_line;
    tok.column = start_col;
    tok.text = std::move(content);
    return tok;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skip_whitespace_and_comments();
        if (is_at_end()) break;

        uint32_t cur_line = line_;
        uint32_t cur_col = column_;
        char c = peek();

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(scan_identifier_or_keyword());
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek_next())))) {
            tokens.push_back(scan_number());
        } else {
            advance();
            Token tok;
            tok.line = cur_line;
            tok.column = cur_col;
            tok.text = std::string(1, c);

            switch (c) {
                case '(': tok.type = TokenType::OpenParen; break;
                case ')': tok.type = TokenType::CloseParen; break;
                case '{': tok.type = TokenType::OpenBrace; break;
                case '}': tok.type = TokenType::CloseBrace; break;
                case '[': tok.type = TokenType::OpenBracket; break;
                case ']': tok.type = TokenType::CloseBracket; break;
                case ';': tok.type = TokenType::Semicolon; break;
                case ':': tok.type = TokenType::Colon; break;
                case ',': tok.type = TokenType::Comma; break;
                case '.': tok.type = TokenType::Dot; break;
                case '?': tok.type = TokenType::Question; break;
                case '%': tok.type = TokenType::Percent; break;
                case '~': tok.type = TokenType::Tilde; break;
                case '^': tok.type = TokenType::Caret; break;
                case '+':
                    if (match('+')) { tok.type = TokenType::PlusPlus; tok.text = "++"; }
                    else if (match('=')) { tok.type = TokenType::PlusAssign; tok.text = "+="; }
                    else { tok.type = TokenType::Plus; }
                    break;
                case '-':
                    if (match('-')) { tok.type = TokenType::MinusMinus; tok.text = "--"; }
                    else if (match('=')) { tok.type = TokenType::MinusAssign; tok.text = "-="; }
                    else { tok.type = TokenType::Minus; }
                    break;
                case '*':
                    if (match('=')) { tok.type = TokenType::StarAssign; tok.text = "*="; }
                    else { tok.type = TokenType::Star; }
                    break;
                case '/':
                    if (match('=')) { tok.type = TokenType::SlashAssign; tok.text = "/="; }
                    else { tok.type = TokenType::Slash; }
                    break;
                case '!':
                    if (match('=')) { tok.type = TokenType::NotEqual; tok.text = "!="; }
                    else { tok.type = TokenType::Exclamation; }
                    break;
                case '=':
                    if (match('=')) { tok.type = TokenType::Equal; tok.text = "=="; }
                    else { tok.type = TokenType::Assign; }
                    break;
                case '<':
                    if (match('=')) { tok.type = TokenType::LessEqual; tok.text = "<="; }
                    else if (match('<')) { tok.type = TokenType::Shl; tok.text = "<<"; }
                    else { tok.type = TokenType::Less; }
                    break;
                case '>':
                    if (match('=')) { tok.type = TokenType::GreaterEqual; tok.text = ">="; }
                    else if (match('>')) { tok.type = TokenType::Shr; tok.text = ">>"; }
                    else { tok.type = TokenType::Greater; }
                    break;
                case '&':
                    if (match('&')) { tok.type = TokenType::AndAnd; tok.text = "&&"; }
                    else { tok.type = TokenType::Ampersand; }
                    break;
                case '|':
                    if (match('|')) { tok.type = TokenType::OrOr; tok.text = "||"; }
                    else { tok.type = TokenType::Pipe; }
                    break;
                case '"':
                    cursor_--;
                    tok = scan_string();
                    break;
                default:
                    break;
            }
            tokens.push_back(tok);
        }
    }

    Token eof;
    eof.type = TokenType::EndOfFile;
    eof.line = line_;
    eof.column = column_;
    tokens.push_back(eof);

    return tokens;
}

} // namespace open_shacc::frontend
