/**
 * @file preprocessor.cpp
 * @brief Implementation of C Preprocessor for Cg / HLSL Shader Compilation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "preprocessor.hpp"
#include <sstream>
#include <iostream>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <stack>

namespace open_shacc::frontend {

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

void Preprocessor::add_macro(const std::string& name, const std::string& body) {
    MacroDefinition def;
    def.name = name;
    def.body = body;
    def.is_function_like = false;
    macros_[name] = def;
}

void Preprocessor::add_function_macro(
    const std::string& name,
    const std::vector<std::string>& params,
    const std::string& body
) {
    MacroDefinition def;
    def.name = name;
    def.params = params;
    def.is_function_like = true;
    def.body = body;
    macros_[name] = def;
}

bool Preprocessor::preprocess(
    const std::string& source,
    const std::string& filename,
    const SceShaccCgCompileOptions* options,
    const SceShaccCgCallbackList* callbacks,
    std::string& out_processed,
    std::string& out_diagnostics
) {
    macros_.clear();
    include_stack_.clear();

    // Built-in preprocessor defines
    add_macro("__PSP2__", "1");
    add_macro("__vita__", "1");
    add_macro("GL_ES", "1");
    add_macro("VITAGL", "1");

    // Add macro definitions passed in options
    if (options && options->macroDefinitions && options->macroDefinitionCount > 0) {
        for (uint32_t i = 0; i < options->macroDefinitionCount; ++i) {
            if (!options->macroDefinitions[i]) continue;
            std::string def_str = options->macroDefinitions[i];
            size_t eq_pos = def_str.find('=');
            if (eq_pos != std::string::npos) {
                add_macro(def_str.substr(0, eq_pos), def_str.substr(eq_pos + 1));
            } else {
                add_macro(def_str, "1");
            }
        }
    }

    out_processed = process_source(source, filename, options, callbacks, out_diagnostics, 0);
    return true;
}

struct CondState {
    bool active = true;
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

    std::vector<CondState> cond_stack;
    auto is_current_active = [&]() -> bool {
        for (const auto& s : cond_stack) {
            if (!s.active) return false;
        }
        return true;
    };

    std::istringstream stream(source);
    std::ostringstream output;
    std::string line;
    uint32_t line_no = 0;

    while (std::getline(stream, line)) {
        line_no++;

        // Handle line continuation '\'
        while (!line.empty() && line.back() == '\\') {
            line.pop_back();
            std::string next_line;
            if (std::getline(stream, next_line)) {
                line_no++;
                line += next_line;
            } else {
                break;
            }
        }

        std::string trimmed = trim(line);
        if (trimmed.length() > 0 && trimmed[0] == '#') {
            std::string directive_line = trim(trimmed.substr(1));
            std::istringstream dir_ss(directive_line);
            std::string dir_name;
            dir_ss >> dir_name;

            if (dir_name == "ifdef") {
                std::string macro_name;
                dir_ss >> macro_name;
                bool is_def = (macros_.find(macro_name) != macros_.end());
                bool active = is_current_active() && is_def;
                cond_stack.push_back(CondState{active, is_def, false});
                output << "\n";
                continue;
            } else if (dir_name == "ifndef") {
                std::string macro_name;
                dir_ss >> macro_name;
                bool is_def = (macros_.find(macro_name) != macros_.end());
                bool active = is_current_active() && !is_def;
                cond_stack.push_back(CondState{active, !is_def, false});
                output << "\n";
                continue;
            } else if (dir_name == "if") {
                std::string expr;
                std::getline(dir_ss, expr);
                bool cond = evaluate_condition(expr);
                bool active = is_current_active() && cond;
                cond_stack.push_back(CondState{active, cond, false});
                output << "\n";
                continue;
            } else if (dir_name == "elif") {
                if (!cond_stack.empty()) {
                    auto& parent = cond_stack.back();
                    if (!parent.in_else) {
                        if (parent.any_branch_taken) {
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
                    parent.in_else = true;
                    parent.active = !parent.any_branch_taken;
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
                while (name_end < rest.length() && (std::isalnum(rest[name_end]) || rest[name_end] == '_')) {
                    name_end++;
                }

                std::string macro_name = rest.substr(0, name_end);
                std::string remainder = rest.substr(name_end);

                if (!remainder.empty() && remainder[0] == '(') {
                    // Function-like macro
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
                    rest.erase(0, 1);
                    if (!rest.empty()) rest.pop_back();
                }

                if (callbacks && callbacks->openFile) {
                    SceShaccCgSourceLocation loc{};
                    loc.lineNumber = line_no;
                    loc.columnNumber = 1;

                    const char* err_str = nullptr;
                    const SceShaccCgSourceFile* inc_file = callbacks->openFile(
                        rest.c_str(), &loc, options, &err_str
                    );

                    if (inc_file && inc_file->text) {
                        std::string inc_text(inc_file->text, inc_file->size ? inc_file->size : std::strlen(inc_file->text));
                        std::string inc_out = process_source(inc_text, inc_file->fileName ? inc_file->fileName : rest, options, callbacks, out_diagnostics, depth + 1);
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
            } else if (dir_name == "pragma") {
                // Pragma directive - emit newline to maintain line count
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
    int max_passes = 8;

    while (changed && max_passes-- > 0) {
        changed = false;
        std::string new_result;
        size_t i = 0;

        while (i < result.length()) {
            if (std::isalpha(result[i]) || result[i] == '_') {
                size_t start = i;
                while (i < result.length() && (std::isalnum(result[i]) || result[i] == '_')) {
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
                        while (peek < result.length() && std::isspace(result[peek])) peek++;
                        if (peek < result.length() && result[peek] == '(') {
                            // Extract arguments
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
                            args.push_back(trim(cur_arg));

                            std::string expanded_body = def.body;
                            for (size_t p = 0; p < def.params.size() && p < args.size(); ++p) {
                                // Substitute param
                                std::string param_pattern = def.params[p];
                                size_t pos = 0;
                                while ((pos = expanded_body.find(param_pattern, pos)) != std::string::npos) {
                                    bool valid_boundary = true;
                                    if (pos > 0 && (std::isalnum(expanded_body[pos - 1]) || expanded_body[pos - 1] == '_')) valid_boundary = false;
                                    size_t end_pos = pos + param_pattern.length();
                                    if (end_pos < expanded_body.length() && (std::isalnum(expanded_body[end_pos]) || expanded_body[end_pos] == '_')) valid_boundary = false;

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
                                while (left > 0 && std::isspace(expanded_body[left - 1])) left--;
                                size_t right = concat_pos + 2;
                                while (right < expanded_body.length() && std::isspace(expanded_body[right])) right++;
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
        result = new_result;
    }

    return result;
}

bool Preprocessor::evaluate_condition(const std::string& expr) {
    std::string trimmed = trim(expr);
    if (trimmed.empty()) return false;

    // Support defined(MACRO) and defined MACRO
    std::string eval_expr = trimmed;
    size_t pos = 0;
    while ((pos = eval_expr.find("defined")) != std::string::npos) {
        size_t start = pos + 7;
        while (start < eval_expr.length() && (std::isspace(eval_expr[start]) || eval_expr[start] == '(')) start++;
        size_t end = start;
        while (end < eval_expr.length() && (std::isalnum(eval_expr[end]) || eval_expr[end] == '_')) end++;
        std::string name = eval_expr.substr(start, end - start);
        bool is_def = (macros_.find(name) != macros_.end());

        size_t replace_end = end;
        while (replace_end < eval_expr.length() && std::isspace(eval_expr[replace_end])) replace_end++;
        if (replace_end < eval_expr.length() && eval_expr[replace_end] == ')') replace_end++;

        eval_expr.replace(pos, replace_end - pos, is_def ? "1" : "0");
    }

    // Simple numeric/boolean check
    for (char c : eval_expr) {
        if (c == '1' || c == 't' || c == 'T') return true;
    }
    return false;
}

} // namespace open_shacc::frontend
