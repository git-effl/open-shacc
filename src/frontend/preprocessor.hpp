/**
 * @file preprocessor.hpp
 * @brief C Preprocessor for Cg / HLSL Shader Compilation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef OPEN_SHACC_PREPROCESSOR_HPP
#define OPEN_SHACC_PREPROCESSOR_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include "../../include/open_shacc.h"

namespace open_shacc::frontend {

struct MacroDefinition {
    std::string name;
    std::vector<std::string> params; // Empty for object-like macros
    bool is_function_like = false;
    std::string body;
};

class Preprocessor {
public:
    Preprocessor() = default;

    void clear_macros();
    void add_macro(const std::string& name, const std::string& body = "1");
    void add_function_macro(const std::string& name, const std::vector<std::string>& params, const std::string& body);

    bool preprocess(
        const std::string& source,
        const std::string& filename,
        const SceShaccCgCompileOptions* options,
        const SceShaccCgCallbackList* callbacks,
        std::string& out_processed,
        std::string& out_diagnostics
    );

private:
    std::unordered_map<std::string, MacroDefinition> macros_;
    std::vector<std::string> include_stack_;

    std::string process_source(
        const std::string& source,
        const std::string& filename,
        const SceShaccCgCompileOptions* options,
        const SceShaccCgCallbackList* callbacks,
        std::string& out_diagnostics,
        int depth = 0
    );

    static std::string strip_comments(const std::string& source);
    std::string expand_line_macros(const std::string& line);
    bool evaluate_condition(const std::string& expr);
};

} // namespace open_shacc::frontend

#endif // OPEN_SHACC_PREPROCESSOR_HPP
