/**
 * @file preprocessor.cpp
 * @brief Implementation of C Preprocessor for Cg / HLSL Shader Compilation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "preprocessor.hpp"
#include <sstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace open_shacc::frontend {

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static bool is_ident_start(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || uc == '_';
}

static bool is_ident_char(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || uc == '_';
}

void Preprocessor::clear_macros() {
    macros_.clear();
}

void Preprocessor::add_macro(const std::string& name, const std::string& body) {
    if (name.empty()) return;
    MacroDefinition def;
    def.name = name;
    def.body = body;
    def.is_function_like = false;
    macros_[name] = std::move(def);
}

void Preprocessor::add_function_macro(
    const std::string& name,
    const std::vector<std::string>& params,
    const std::string& body
) {
    if (name.empty()) return;
    MacroDefinition def;
    def.name = name;
    def.params = params;
    def.is_function_like = true;
    def.body = body;
    macros_[name] = std::move(def);
}

std::string Preprocessor::strip_comments(const std::string& source) {
    std::string out;
    out.reserve(source.size());

    size_t i = 0;
    const size_t n = source.size();
    bool in_string = false;

    while (i < n) {
        char c = source[i];
        if (in_string) {
            out.push_back(c);
            if (c == '\\' && i + 1 < n) {
                out.push_back(source[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }

        if (c == '"') {
            in_string = true;
            out.push_back(c);
            ++i;
        } else if (c == '/' && i + 1 < n && source[i + 1] == '/') {
            // Single-line comment
            i += 2;
            while (i < n && source[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < n && source[i + 1] == '*') {
            // Block comment - preserve newlines for accurate line numbers
            i += 2;
            out.push_back(' ');
            while (i < n) {
                if (source[i] == '*' && i + 1 < n && source[i + 1] == '/') {
                    i += 2;
                    break;
                }
                if (source[i] == '\n') {
                    out.push_back('\n');
                }
                ++i;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }

    return out;
}

bool Preprocessor::preprocess(
    const std::string& source,
    const std::string& filename,
    const SceShaccCgCompileOptions* options,
    const SceShaccCgCallbackList* callbacks,
    std::string& out_processed,
    std::string& out_diagnostics
) {
    include_stack_.clear();

    // Built-in preprocessor defines (preserve caller-supplied pre-added macros)
    if (macros_.find("__PSP2__") == macros_.end()) add_macro("__PSP2__", "1");
    if (macros_.find("__vita__") == macros_.end()) add_macro("__vita__", "1");
    if (macros_.find("GL_ES") == macros_.end())    add_macro("GL_ES", "1");
    if (macros_.find("VITAGL") == macros_.end())   add_macro("VITAGL", "1");

    // Add macro definitions passed in options
    if (options && options->macroDefinitions && options->macroDefinitionCount > 0) {
        for (uint32_t i = 0; i < options->macroDefinitionCount; ++i) {
            if (!options->macroDefinitions[i]) continue;
            std::string def_str = options->macroDefinitions[i];
            size_t eq_pos = def_str.find('=');
            if (eq_pos != std::string::npos) {
                add_macro(trim(def_str.substr(0, eq_pos)), trim(def_str.substr(eq_pos + 1)));
            } else {
                add_macro(trim(def_str), "1");
            }
        }
    }

    std::ostringstream combined;
    // Process forced include files from options first, if any
    if (options && options->includeFiles && options->includeFileCount > 0 && callbacks && callbacks->openFile) {
        for (uint32_t i = 0; i < options->includeFileCount; ++i) {
            if (!options->includeFiles[i]) continue;
            const char* err_str = nullptr;
            const SceShaccCgSourceFile* inc_file = callbacks->openFile(
                options->includeFiles[i], nullptr, options, &err_str
            );
            if (inc_file && inc_file->text) {
                std::string inc_text(inc_file->text, inc_file->size ? inc_file->size : std::strlen(inc_file->text));
                combined << process_source(
                    inc_text,
                    inc_file->fileName ? inc_file->fileName : options->includeFiles[i],
                    options, callbacks, out_diagnostics, 1
                ) << "\n";
                if (callbacks->releaseFile) {
                    callbacks->releaseFile(inc_file, options);
                }
            }
        }
    }

    combined << process_source(source, filename, options, callbacks, out_diagnostics, 0);
    out_processed = combined.str();
    return true;
}

struct CondState {
    bool outer_active = true;
    bool active = false;
    bool any_branch_taken = false;
    bool in_else = false;
};

std::string Preprocessor::process_source(
    const std::string& source,
    const std::string& filename,
    const SceShaccCgCompileOptions* options,
    const SceShaccCgCallbackList* callbacks,
    std::string& out_diagnostics,
    int depth
) {
    if (depth > 32) {
        out_diagnostics += "Error: Maximum #include depth exceeded in " + filename + "\n";
        return "";
    }

    std::string clean_source = strip_comments(source);
    std::vector<CondState> cond_stack;
    auto is_current_active = [&]() -> bool {
        return cond_stack.empty() || cond_stack.back().active;
    };

    std::istringstream stream(clean_source);
    std::ostringstream output;
    std::string line;
    uint32_t line_no = 0;

    while (std::getline(stream, line)) {
        line_no++;

        // Handle line continuation '\' (including CRLF \r\)
        while (!line.empty() && (line.back() == '\r' || line.back() == '\\')) {
            if (line.back() == '\r') {
                line.pop_back();
                continue;
            }
            if (line.back() == '\\') {
                line.pop_back();
                std::string next_line;
                if (std::getline(stream, next_line)) {
                    line_no++;
                    line += " " + next_line;
                } else {
                    break;
                }
            }
        }

        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] == '#') {
            std::string directive_line = trim(trimmed.substr(1));
            std::istringstream dir_ss(directive_line);
            std::string dir_name;
            dir_ss >> dir_name;

            if (dir_name == "ifdef") {
                std::string macro_name;
                dir_ss >> macro_name;
                bool outer = is_current_active();
                bool is_def = outer && (macros_.find(macro_name) != macros_.end());
                cond_stack.push_back(CondState{outer, is_def, is_def, false});
                output << "\n";
                continue;
            } else if (dir_name == "ifndef") {
                std::string macro_name;
                dir_ss >> macro_name;
                bool outer = is_current_active();
                bool not_def = outer && (macros_.find(macro_name) == macros_.end());
                cond_stack.push_back(CondState{outer, not_def, not_def, false});
                output << "\n";
                continue;
            } else if (dir_name == "if") {
                std::string expr;
                std::getline(dir_ss, expr);
                bool outer = is_current_active();
                bool cond = outer ? evaluate_condition(expr) : false;
                cond_stack.push_back(CondState{outer, cond, cond, false});
                output << "\n";
                continue;
            } else if (dir_name == "elif") {
                if (!cond_stack.empty()) {
                    auto& parent = cond_stack.back();
                    if (!parent.in_else) {
                        if (!parent.outer_active || parent.any_branch_taken) {
                            parent.active = false;
                        } else {
                            std::string expr;
                            std::getline(dir_ss, expr);
                            bool cond = evaluate_condition(expr);
                            parent.active = cond;
                            if (cond) parent.any_branch_taken = true;
                        }
                    }
                }
                output << "\n";
                continue;
            } else if (dir_name == "else") {
                if (!cond_stack.empty()) {
                    auto& parent = cond_stack.back();
                    if (!parent.in_else) {
                        parent.in_else = true;
                        parent.active = parent.outer_active && !parent.any_branch_taken;
                        parent.any_branch_taken = true;
                    } else {
                        parent.active = false;
                    }
                }
                output << "\n";
                continue;
            } else if (dir_name == "endif") {
                if (!cond_stack.empty()) {
                    cond_stack.pop_back();
                }
                output << "\n";
                continue;
            }

            if (!is_current_active()) {
                output << "\n";
                continue;
            }

            if (dir_name == "define") {
                std::string rest;
                std::getline(dir_ss, rest);
                rest = trim(rest);

                size_t name_end = 0;
                while (name_end < rest.length() && is_ident_char(rest[name_end])) {
                    name_end++;
                }

                std::string macro_name = rest.substr(0, name_end);
                std::string remainder = rest.substr(name_end);

                if (!remainder.empty() && remainder[0] == '(') {
                    // Function-like macro (no whitespace between name and '(')
                    size_t paren_close = remainder.find(')');
                    if (paren_close != std::string::npos) {
                        std::string params_str = remainder.substr(1, paren_close - 1);
                        std::string body_str = trim(remainder.substr(paren_close + 1));

                        std::vector<std::string> params;
                        std::istringstream p_ss(params_str);
                        std::string p;
                        while (std::getline(p_ss, p, ',')) {
                            p = trim(p);
                            if (!p.empty()) params.push_back(p);
                        }
                        add_function_macro(macro_name, params, body_str);
                    }
                } else {
                    // Object-like macro
                    add_macro(macro_name, trim(remainder));
                }
                output << "\n";
                continue;
            } else if (dir_name == "undef") {
                std::string macro_name;
                dir_ss >> macro_name;
                macros_.erase(macro_name);
                output << "\n";
                continue;
            } else if (dir_name == "include") {
                std::string rest;
                std::getline(dir_ss, rest);
                rest = trim(rest);
                if (!rest.empty() && (rest.front() == '"' || rest.front() == '<')) {
                    char close_char = (rest.front() == '"') ? '"' : '>';
                    size_t close_pos = rest.find(close_char, 1);
                    if (close_pos != std::string::npos) {
                        rest = rest.substr(1, close_pos - 1);
                    } else {
                        rest.erase(0, 1);
                    }
                }

                if (callbacks && callbacks->openFile && !rest.empty()) {
                    SceShaccCgSourceLocation loc{};
                    loc.lineNumber = line_no;
                    loc.columnNumber = 1;

                    const char* err_str = nullptr;
                    const SceShaccCgSourceFile* inc_file = callbacks->openFile(
                        rest.c_str(), &loc, options, &err_str
                    );

                    if (inc_file && inc_file->text) {
                        std::string inc_text(inc_file->text, inc_file->size ? inc_file->size : std::strlen(inc_file->text));
                        std::string inc_out = process_source(
                            inc_text,
                            inc_file->fileName ? inc_file->fileName : rest,
                            options, callbacks, out_diagnostics, depth + 1
                        );
                        output << inc_out << "\n";

                        if (callbacks->releaseFile) {
                            callbacks->releaseFile(inc_file, options);
                        }
                    } else {
                        output << "\n";
                    }
                } else {
                    output << "\n";
                }
                continue;
            } else if (dir_name == "error") {
                std::string rest;
                std::getline(dir_ss, rest);
                out_diagnostics += "#error " + trim(rest) + "\n";
                output << "\n";
                continue;
            } else {
                // Unknown or ignored directive (#pragma, #line, #version, #extension)
                output << "\n";
                continue;
            }
        }

        if (!is_current_active()) {
            output << "\n";
            continue;
        }

        // Expand macros in non-directive lines
        std::string expanded = expand_line_macros(line);
        output << expanded << "\n";
    }

    return output.str();
}

std::string Preprocessor::expand_line_macros(const std::string& line) {
    if (macros_.empty() || line.empty()) return line;

    std::string result = line;
    bool changed = true;
    int max_passes = 16;

    while (changed && max_passes-- > 0) {
        changed = false;
        std::string new_result;
        new_result.reserve(result.size());
        size_t i = 0;
        bool in_string = false;

        while (i < result.length()) {
            if (in_string) {
                char c = result[i++];
                new_result.push_back(c);
                if (c == '\\' && i < result.length()) {
                    new_result.push_back(result[i++]);
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }

            if (result[i] == '"') {
                in_string = true;
                new_result.push_back(result[i++]);
                continue;
            }

            if (is_ident_start(result[i])) {
                size_t start = i;
                while (i < result.length() && is_ident_char(result[i])) {
                    i++;
                }
                std::string token = result.substr(start, i - start);

                auto it = macros_.find(token);
                if (it != macros_.end()) {
                    const auto& def = it->second;
                    if (!def.is_function_like) {
                        new_result += def.body;
                        changed = true;
                    } else {
                        // Check if followed by '('
                        size_t peek = i;
                        while (peek < result.length() && std::isspace(static_cast<unsigned char>(result[peek]))) {
                            peek++;
                        }
                        if (peek < result.length() && result[peek] == '(') {
                            size_t arg_start = peek + 1;
                            int paren_depth = 1;
                            size_t arg_end = arg_start;
                            std::vector<std::string> args;
                            std::string cur_arg;

                            while (arg_end < result.length() && paren_depth > 0) {
                                char c = result[arg_end];
                                if (c == '(') paren_depth++;
                                else if (c == ')') paren_depth--;

                                if (c == ',' && paren_depth == 1) {
                                    args.push_back(trim(cur_arg));
                                    cur_arg.clear();
                                } else if (paren_depth > 0) {
                                    cur_arg += c;
                                }
                                arg_end++;
                            }
                            if (!args.empty() || !trim(cur_arg).empty()) {
                                args.push_back(trim(cur_arg));
                            }

                            std::string expanded_body = def.body;
                            for (size_t p = 0; p < def.params.size() && p < args.size(); ++p) {
                                const std::string& param_pattern = def.params[p];
                                if (param_pattern.empty()) continue;
                                size_t pos = 0;
                                while ((pos = expanded_body.find(param_pattern, pos)) != std::string::npos) {
                                    bool valid_boundary = true;
                                    if (pos > 0 && is_ident_char(expanded_body[pos - 1])) valid_boundary = false;
                                    size_t end_pos = pos + param_pattern.length();
                                    if (end_pos < expanded_body.length() && is_ident_char(expanded_body[end_pos])) valid_boundary = false;

                                    if (valid_boundary) {
                                        expanded_body.replace(pos, param_pattern.length(), args[p]);
                                        pos += args[p].length();
                                    } else {
                                        pos += param_pattern.length();
                                    }
                                }
                            }

                            // Token concatenation ##
                            size_t concat_pos = 0;
                            while ((concat_pos = expanded_body.find("##")) != std::string::npos) {
                                size_t left = concat_pos;
                                while (left > 0 && std::isspace(static_cast<unsigned char>(expanded_body[left - 1]))) left--;
                                size_t right = concat_pos + 2;
                                while (right < expanded_body.length() && std::isspace(static_cast<unsigned char>(expanded_body[right]))) right++;
                                expanded_body.erase(left, right - left);
                            }

                            new_result += expanded_body;
                            i = arg_end;
                            changed = true;
                        } else {
                            new_result += token;
                        }
                    }
                } else {
                    new_result += token;
                }
            } else {
                new_result += result[i++];
            }
        }
        result = std::move(new_result);
    }

    return result;
}

namespace {

class ConstExprEvaluator {
public:
    explicit ConstExprEvaluator(const std::string& expr) : s_(expr) {}

    long long eval() {
        pos_ = 0;
        return parse_logical_or();
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    bool match(const char* tok) {
        skip_ws();
        size_t len = std::strlen(tok);
        if (s_.compare(pos_, len, tok) == 0) {
            // Avoid matching '=' inside '==', '&' inside '&&', '|' inside '||', '<' inside '<='/'<<', etc.
            if (len == 1 && (tok[0] == '<' || tok[0] == '>' || tok[0] == '!' || tok[0] == '&' || tok[0] == '|')) {
                char next = (pos_ + 1 < s_.size()) ? s_[pos_ + 1] : '\0';
                if (next == '=' || next == tok[0]) return false;
            }
            pos_ += len;
            return true;
        }
        return false;
    }

    long long parse_logical_or() {
        long long v = parse_logical_and();
        while (match("||")) {
            long long r = parse_logical_and();
            v = (v != 0 || r != 0) ? 1 : 0;
        }
        return v;
    }

    long long parse_logical_and() {
        long long v = parse_bit_or();
        while (match("&&")) {
            long long r = parse_bit_or();
            v = (v != 0 && r != 0) ? 1 : 0;
        }
        return v;
    }

    long long parse_bit_or() {
        long long v = parse_bit_xor();
        while (match("|")) {
            long long r = parse_bit_xor();
            v |= r;
        }
        return v;
    }

    long long parse_bit_xor() {
        long long v = parse_bit_and();
        while (match("^")) {
            long long r = parse_bit_and();
            v ^= r;
        }
        return v;
    }

    long long parse_bit_and() {
        long long v = parse_equality();
        while (match("&")) {
            long long r = parse_equality();
            v &= r;
        }
        return v;
    }

    long long parse_equality() {
        long long v = parse_relational();
        while (true) {
            if (match("==")) {
                long long r = parse_relational();
                v = (v == r) ? 1 : 0;
            } else if (match("!=")) {
                long long r = parse_relational();
                v = (v != r) ? 1 : 0;
            } else {
                break;
            }
        }
        return v;
    }

    long long parse_relational() {
        long long v = parse_shift();
        while (true) {
            if (match("<=")) {
                v = (v <= parse_shift()) ? 1 : 0;
            } else if (match(">=")) {
                v = (v >= parse_shift()) ? 1 : 0;
            } else if (match("<")) {
                v = (v < parse_shift()) ? 1 : 0;
            } else if (match(">")) {
                v = (v > parse_shift()) ? 1 : 0;
            } else {
                break;
            }
        }
        return v;
    }

    long long parse_shift() {
        long long v = parse_additive();
        while (true) {
            if (match("<<")) {
                v <<= (parse_additive() & 63);
            } else if (match(">>")) {
                v >>= (parse_additive() & 63);
            } else {
                break;
            }
        }
        return v;
    }

    long long parse_additive() {
        long long v = parse_multiplicative();
        while (true) {
            if (match("+")) {
                v += parse_multiplicative();
            } else if (match("-")) {
                v -= parse_multiplicative();
            } else {
                break;
            }
        }
        return v;
    }

    long long parse_multiplicative() {
        long long v = parse_unary();
        while (true) {
            if (match("*")) {
                v *= parse_unary();
            } else if (match("/")) {
                long long r = parse_unary();
                v = (r != 0) ? (v / r) : 0;
            } else if (match("%")) {
                long long r = parse_unary();
                v = (r != 0) ? (v % r) : 0;
            } else {
                break;
            }
        }
        return v;
    }

    long long parse_unary() {
        skip_ws();
        if (match("!")) return parse_unary() == 0 ? 1 : 0;
        if (match("~")) return ~parse_unary();
        if (match("-")) return -parse_unary();
        if (match("+")) return parse_unary();
        return parse_primary();
    }

    long long parse_primary() {
        skip_ws();
        if (pos_ >= s_.size()) return 0;

        if (match("(")) {
            long long v = parse_logical_or();
            match(")");
            return v;
        }

        if (std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            char* end_ptr = nullptr;
            long long v = std::strtoll(s_.c_str() + pos_, &end_ptr, 0);
            pos_ = static_cast<size_t>(end_ptr - s_.c_str());
            while (pos_ < s_.size() && (s_[pos_] == 'u' || s_[pos_] == 'U' || s_[pos_] == 'l' || s_[pos_] == 'L')) {
                ++pos_;
            }
            return v;
        }

        if (is_ident_start(s_[pos_])) {
            size_t start = pos_;
            while (pos_ < s_.size() && is_ident_char(s_[pos_])) ++pos_;
            std::string id = s_.substr(start, pos_ - start);
            if (id == "true") return 1;
            return 0; // Undefined identifiers evaluate to 0 in #if
        }

        // Skip unrecognized token to ensure forward progress
        ++pos_;
        return 0;
    }
};

} // namespace

bool Preprocessor::evaluate_condition(const std::string& expr) {
    std::string trimmed = trim(expr);
    if (trimmed.empty()) return false;

    // 1. Evaluate defined(MACRO) and defined MACRO BEFORE macro expansion
    std::string eval_expr;
    eval_expr.reserve(trimmed.size());
    size_t i = 0;
    while (i < trimmed.size()) {
        if (is_ident_start(trimmed[i])) {
            size_t start = i;
            while (i < trimmed.size() && is_ident_char(trimmed[i])) ++i;
            std::string tok = trimmed.substr(start, i - start);
            if (tok == "defined") {
                size_t cur = i;
                while (cur < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cur]))) ++cur;
                bool has_paren = false;
                if (cur < trimmed.size() && trimmed[cur] == '(') {
                    has_paren = true;
                    ++cur;
                    while (cur < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cur]))) ++cur;
                }
                size_t m_start = cur;
                while (cur < trimmed.size() && is_ident_char(trimmed[cur])) ++cur;
                std::string macro_name = trimmed.substr(m_start, cur - m_start);
                if (has_paren) {
                    while (cur < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[cur]))) ++cur;
                    if (cur < trimmed.size() && trimmed[cur] == ')') ++cur;
                }
                bool is_def = (macros_.find(macro_name) != macros_.end());
                eval_expr += (is_def ? "1" : "0");
                i = cur;
                continue;
            }
            eval_expr += tok;
        } else {
            eval_expr.push_back(trimmed[i++]);
        }
    }

    // 2. Expand remaining macros inside the condition expression
    eval_expr = expand_line_macros(eval_expr);

    // 3. Evaluate integer constant expression
    ConstExprEvaluator evaluator(eval_expr);
    return evaluator.eval() != 0;
}

} // namespace open_shacc::frontend
