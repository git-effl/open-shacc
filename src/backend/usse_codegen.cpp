/**
 * @file usse_codegen.cpp
 * @brief Clean-room USSE Target Code Generator translating Cg/HLSL AST to 64-bit SGX microcode.
 *
 * All instruction sequences, math lowering, control flow, and register mappings
 * are derived strictly from public PowerVR SGX / USSE specifications and
 * open-source Cg / HLSL language semantics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "usse_codegen.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace open_shacc::backend {

namespace {

uint8_t swizzle_char_to_sub(char c) {
    switch (c) {
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        case 'w': case 'a': return 3;
        default:            return 0;
    }
}

SwizzleChannel sub_to_swiz_chan(uint8_t sub) {
    switch (sub & 3u) {
        case 0: return SwizzleChannel::X;
        case 1: return SwizzleChannel::Y;
        case 2: return SwizzleChannel::Z;
        default: return SwizzleChannel::W;
    }
}

uint8_t mask_for_count(uint8_t count) {
    switch (count) {
        case 1: return 0b0001;
        case 2: return 0b0011;
        case 3: return 0b0111;
        default: return 0b1111;
    }
}

uint32_t float_to_bits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

bool is_inline_const_val(float v, SwizzleChannel& out_ch) {
    if (v == 0.0f) { out_ch = SwizzleChannel::Zero; return true; }
    if (v == 1.0f) { out_ch = SwizzleChannel::One;  return true; }
    if (v == 2.0f) { out_ch = SwizzleChannel::Two;  return true; }
    if (v == 0.5f) { out_ch = SwizzleChannel::Half; return true; }
    return false;
}

std::optional<Vec4Swiz> match_vec4_swiz(const std::array<SwizzleChannel, 4>& s) {
    auto eq = [&](SwizzleChannel a, SwizzleChannel b, SwizzleChannel c, SwizzleChannel d) {
        return s[0] == a && s[1] == b && s[2] == c && s[3] == d;
    };
    using SC = SwizzleChannel;
    if (eq(SC::X, SC::X, SC::X, SC::X)) return Vec4Swiz::XXXX;
    if (eq(SC::Y, SC::Y, SC::Y, SC::Y)) return Vec4Swiz::YYYY;
    if (eq(SC::Z, SC::Z, SC::Z, SC::Z)) return Vec4Swiz::ZZZZ;
    if (eq(SC::W, SC::W, SC::W, SC::W)) return Vec4Swiz::WWWW;
    if (eq(SC::X, SC::Y, SC::Z, SC::W)) return Vec4Swiz::XYZW;
    if (eq(SC::Y, SC::Z, SC::W, SC::W)) return Vec4Swiz::YZWW;
    if (eq(SC::X, SC::Y, SC::Z, SC::Z)) return Vec4Swiz::XYZZ;
    if (eq(SC::X, SC::X, SC::Y, SC::Z)) return Vec4Swiz::XXYZ;
    if (eq(SC::X, SC::Y, SC::X, SC::Y)) return Vec4Swiz::XYXY;
    if (eq(SC::X, SC::Y, SC::W, SC::Z)) return Vec4Swiz::XYWZ;
    if (eq(SC::Z, SC::X, SC::Y, SC::W)) return Vec4Swiz::ZXYW;
    if (eq(SC::Z, SC::W, SC::Z, SC::W)) return Vec4Swiz::ZWZW;
    if (eq(SC::Y, SC::Z, SC::X, SC::Z)) return Vec4Swiz::YZXZ;
    if (eq(SC::X, SC::X, SC::Y, SC::Y)) return Vec4Swiz::XXYY;
    if (eq(SC::X, SC::Z, SC::W, SC::W)) return Vec4Swiz::XZWW;
    if (eq(SC::X, SC::Y, SC::Z, SC::One)) return Vec4Swiz::XYZ1;
    return std::nullopt;
}

bool semantic_contains(const std::string& sem, const char* needle) {
    if (sem.empty() || !needle) return false;
    std::string up = sem;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return up.find(needle) != std::string::npos;
}

bool is_swizzle_letters(const std::string& comps) {
    if (comps.empty() || comps.size() > 4) return false;
    bool xyzw = true, rgba = true;
    for (char c : comps) {
        if (c != 'x' && c != 'y' && c != 'z' && c != 'w') xyzw = false;
        if (c != 'r' && c != 'g' && c != 'b' && c != 'a') rgba = false;
    }
    return xyzw || rgba;
}

} // namespace

UsseCodeGenerator::UsseCodeGenerator() = default;

bool UsseCodeGenerator::eval_const_int(const frontend::Expression* expr, int64_t& out_val) {
    if (!expr) return false;
    if (auto* lit = dynamic_cast<const frontend::LiteralExpr*>(expr)) {
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
    if (auto* un = dynamic_cast<const frontend::UnaryExpr*>(expr)) {
        int64_t v = 0;
        if (!eval_const_int(un->operand.get(), v)) return false;
        if (un->op == frontend::UnaryOp::Negate) { out_val = -v; return true; }
        if (un->op == frontend::UnaryOp::LogicalNot) { out_val = (v == 0) ? 1 : 0; return true; }
    }
    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(expr)) {
        int64_t l = 0, r = 0;
        if (!eval_const_int(bin->left.get(), l) || !eval_const_int(bin->right.get(), r)) return false;
        switch (bin->op) {
            case frontend::BinaryOp::Add: out_val = l + r; return true;
            case frontend::BinaryOp::Sub: out_val = l - r; return true;
            case frontend::BinaryOp::Mul: out_val = l * r; return true;
            case frontend::BinaryOp::Div: out_val = (r != 0) ? (l / r) : 0; return true;
            case frontend::BinaryOp::Mod: out_val = (r != 0) ? (l % r) : 0; return true;
            default: return false;
        }
    }
    return false;
}

bool UsseCodeGenerator::statement_contains_discard(const frontend::Statement* stmt) {
    if (!stmt) return false;
    if (dynamic_cast<const frontend::DiscardStmt*>(stmt)) return true;
    if (auto* b = dynamic_cast<const frontend::BlockStmt*>(stmt)) {
        for (const auto& s : b->statements) {
            if (statement_contains_discard(s.get())) return true;
        }
    } else if (auto* i = dynamic_cast<const frontend::IfStmt*>(stmt)) {
        return statement_contains_discard(i->then_branch.get()) ||
               statement_contains_discard(i->else_branch.get());
    } else if (auto* f = dynamic_cast<const frontend::ForStmt*>(stmt)) {
        return statement_contains_discard(f->body.get());
    } else if (auto* w = dynamic_cast<const frontend::WhileStmt*>(stmt)) {
        return statement_contains_discard(w->body.get());
    } else if (auto* dw = dynamic_cast<const frontend::DoWhileStmt*>(stmt)) {
        return statement_contains_discard(dw->body.get());
    } else if (auto* a = dynamic_cast<const frontend::AssignStmt*>(stmt)) {
        if (auto* c = dynamic_cast<const frontend::CallExpr*>(a->lvalue.get())) {
            if (c->callee == "clip") return true;
        }
    }
    return false;
}

size_t UsseCodeGenerator::emit_inst(UsseInstruction inst) {
    size_t idx = instructions_.size();
    instructions_.push_back(std::move(inst));
    return idx;
}

size_t UsseCodeGenerator::emit_br(int32_t rel_offset, ExtPredicate pred, const std::string& comment) {
    UsseInstruction inst;
    inst.opcode = UsseOpcode::BR;
    inst.pred = pred;
    inst.branch_offset = rel_offset;
    inst.comment = comment;
    return emit_inst(std::move(inst));
}

void UsseCodeGenerator::patch_br(size_t br_idx, size_t target_idx) {
    if (br_idx < instructions_.size()) {
        instructions_[br_idx].branch_offset = static_cast<int32_t>(target_idx) - static_cast<int32_t>(br_idx);
    }
}

void UsseCodeGenerator::emit_phase_switch() {
    UsseInstruction inst;
    inst.opcode = UsseOpcode::PHAS;
    inst.comment = "End of shader phase";
    emit_inst(std::move(inst));
}

uint8_t UsseCodeGenerator::alloc_temp_vec4() {
    uint8_t reg = reg_allocator_.allocate_temp_vec4();
    if (!scopes_.empty()) {
        scopes_.back().owned_vec4_temps.push_back(reg);
    }
    return reg;
}

ValueRef UsseCodeGenerator::make_temp_vec4(uint8_t count) {
    ValueRef v;
    v.reg_class = RegisterClass::Temporary;
    v.base_index = alloc_temp_vec4();
    v.count = std::clamp<uint8_t>(count, 1, 4);
    v.swiz = {SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W};
    return v;
}

void UsseCodeGenerator::push_scope() {
    scopes_.emplace_back();
}

void UsseCodeGenerator::pop_scope() {
    if (scopes_.empty()) return;
    ScopeFrame frame = std::move(scopes_.back());
    scopes_.pop_back();
    for (auto it = frame.shadowed_symbols.rbegin(); it != frame.shadowed_symbols.rend(); ++it) {
        if (it->second.has_value()) {
            reg_allocator_.alias_symbol(it->first, *it->second);
        } else {
            reg_allocator_.erase_symbol(it->first);
        }
    }
    for (uint8_t reg : frame.owned_vec4_temps) {
        reg_allocator_.free_temp_vec4(reg);
    }
}

void UsseCodeGenerator::bind_scoped_symbol(const std::string& name, const SymbolBinding& binding) {
    if (!scopes_.empty()) {
        const auto* existing = reg_allocator_.find_symbol(name);
        std::optional<SymbolBinding> prev = existing ? std::optional<SymbolBinding>(*existing) : std::nullopt;
        scopes_.back().shadowed_symbols.emplace_back(name, prev);
    }
    reg_allocator_.alias_symbol(name, binding);
}

HardwareRegister UsseCodeGenerator::get_zero_hw_reg() {
    if (!has_zero_reg_) {
        // Allocate an even-aligned vec4 constant slot at shader scope (never freed)
        uint8_t base = reg_allocator_.allocate_temp_vec4();
        zero_reg_index_ = base;
        has_zero_reg_ = true;
        float_const_cache_[float_to_bits(0.0f)] = base;

        UsseInstruction inst;
        inst.opcode = UsseOpcode::LIMM;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = base;
        inst.imm32 = 0x00000000u;
        inst.comment = "Load 0.0f constant";
        emit_inst(std::move(inst));
    }
    HardwareRegister r;
    r.reg_class = RegisterClass::Temporary;
    r.index = zero_reg_index_;
    r.sub_index = 0;
    return r;
}

ValueRef UsseCodeGenerator::get_or_create_float_const(float val) {
    SwizzleChannel inline_ch{};
    if (is_inline_const_val(val, inline_ch)) {
        return make_const_scalar(val);
    }

    uint32_t bits = float_to_bits(val);
    uint8_t reg_idx = 0;
    auto it = float_const_cache_.find(bits);
    if (it != float_const_cache_.end()) {
        reg_idx = it->second;
    } else {
        if (const_cache_sub_ >= 4) {
            // Allocate persistent vec4 constant slot directly from reg_allocator_
            const_cache_base_ = reg_allocator_.allocate_temp_vec4();
            const_cache_sub_ = 0;
        }
        reg_idx = static_cast<uint8_t>(const_cache_base_ + const_cache_sub_);
        const_cache_sub_++;
        float_const_cache_[bits] = reg_idx;

        UsseInstruction inst;
        inst.opcode = UsseOpcode::LIMM;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = reg_idx;
        inst.imm32 = bits;
        std::ostringstream cs;
        cs << "LIMM " << val << "f";
        inst.comment = cs.str();
        emit_inst(std::move(inst));
    }

    ValueRef v;
    v.reg_class = RegisterClass::Temporary;
    v.base_index = reg_idx;
    v.count = 1;
    v.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
    v.is_const = false; // Materialized in register
    return v;
}

ValueRef UsseCodeGenerator::make_const_scalar(float val) {
    ValueRef v;
    v.is_const = true;
    v.count = 1;
    v.const_vals = {val, val, val, val};
    return v;
}

ValueRef UsseCodeGenerator::make_const_vec(const std::array<float, 4>& vals, uint8_t count) {
    ValueRef v;
    v.is_const = true;
    v.count = std::clamp<uint8_t>(count, 1, 4);
    v.const_vals = vals;
    return v;
}

HardwareRegister UsseCodeGenerator::to_hw_src1(const ValueRef& val, uint16_t& out_swiz12) const {
    if (val.is_const) {
        std::array<SwizzleChannel, 4> chs{};
        bool all_inline = true;
        for (int i = 0; i < 4; ++i) {
            int lane = (i < val.count) ? i : (val.count - 1);
            if (!is_inline_const_val(val.const_vals[lane], chs[i])) {
                all_inline = false;
                break;
            }
        }
        if (all_inline) {
            out_swiz12 = pack_swizzle12(chs[0], chs[1], chs[2], chs[3]);
            HardwareRegister hw;
            hw.reg_class = RegisterClass::Temporary;
            hw.index = has_zero_reg_ ? zero_reg_index_ : 0;
            hw.is_negated = val.is_negated;
            hw.is_absolute = val.is_absolute;
            return hw;
        }
    }

    uint16_t even_base = val.base_index & ~1u;
    uint8_t bias = static_cast<uint8_t>(val.base_index & 1u);
    std::array<SwizzleChannel, 4> adj = val.swiz;
    if (bias != 0) {
        for (int i = 0; i < 4; ++i) {
            uint8_t c = static_cast<uint8_t>(adj[i]);
            if (c < 4u) {
                adj[i] = sub_to_swiz_chan(static_cast<uint8_t>(std::min<uint8_t>(3u, c + bias)));
            }
        }
    }

    out_swiz12 = pack_swizzle12(adj[0], adj[1], adj[2], adj[3]);
    HardwareRegister hw;
    hw.reg_class = val.reg_class;
    hw.index = static_cast<uint8_t>(even_base & 0xFFu);
    hw.sub_index = static_cast<uint8_t>(adj[0]) & 3u;
    hw.swizzle = adj;
    hw.is_negated = val.is_negated;
    hw.is_absolute = val.is_absolute;
    return hw;
}

HardwareRegister UsseCodeGenerator::to_hw_src2(const ValueRef& val, Vec4Swiz& out_swiz4) {
    if (!val.is_const && !val.is_negated) {
        uint16_t even_base = val.base_index & ~1u;
        uint8_t bias = static_cast<uint8_t>(val.base_index & 1u);
        std::array<SwizzleChannel, 4> adj = val.swiz;
        if (bias != 0) {
            for (int i = 0; i < 4; ++i) {
                uint8_t c = static_cast<uint8_t>(adj[i]);
                if (c < 4u) {
                    adj[i] = sub_to_swiz_chan(static_cast<uint8_t>(std::min<uint8_t>(3u, c + bias)));
                }
            }
        }
        if (auto matched = match_vec4_swiz(adj)) {
            out_swiz4 = *matched;
            HardwareRegister hw;
            hw.reg_class = val.reg_class;
            hw.index = static_cast<uint8_t>(even_base & 0xFFu);
            hw.sub_index = static_cast<uint8_t>(adj[0]) & 3u;
            hw.swizzle = adj;
            hw.is_absolute = val.is_absolute;
            return hw;
        }
    }

    ValueRef tmp = materialize_to_temp(val, val.count, "Materialize src2");
    out_swiz4 = (tmp.count == 1) ? Vec4Swiz::XXXX : Vec4Swiz::XYZW;
    HardwareRegister hw;
    hw.reg_class = RegisterClass::Temporary;
    hw.index = static_cast<uint8_t>(tmp.base_index & 0xFFu);
    hw.sub_index = 0;
    return hw;
}

ValueRef UsseCodeGenerator::materialize_to_temp(const ValueRef& val, uint8_t target_count, const std::string& comment) {
    uint8_t n = (target_count > 0) ? target_count : val.count;
    n = std::clamp<uint8_t>(n, 1, 4);

    if (val.is_const) {
        // Check if every needed lane can be satisfied inline or via LIMM
        bool all_inline = true;
        std::array<SwizzleChannel, 4> chs = {SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero};
        for (uint8_t i = 0; i < n; ++i) {
            uint8_t src_i = (i < val.count) ? i : (val.count - 1);
            float cv = val.const_vals[src_i];
            if (val.is_absolute) cv = std::fabs(cv);
            if (val.is_negated) cv = -cv;
            if (!is_inline_const_val(cv, chs[i])) {
                all_inline = false;
                break;
            }
        }
        if (all_inline) {
            ValueRef tmp = make_temp_vec4(n);
            HardwareRegister zero_hw = get_zero_hw_reg();
            UsseInstruction inst;
            inst.opcode = UsseOpcode::VADD;
            inst.dst.reg_class = RegisterClass::Temporary;
            inst.dst.index = static_cast<uint8_t>(tmp.base_index);
            inst.src0 = zero_hw;
            inst.src1 = zero_hw;
            inst.write_mask = mask_for_count(n);
            inst.src1_swiz12 = pack_swizzle12(chs[0], chs[1], chs[2], chs[3]);
            inst.src2_swiz4 = Vec4Swiz::XXXX;
            inst.comment = comment.empty() ? "Inline const vec" : comment;
            emit_inst(std::move(inst));
            return tmp;
        }

        // Check splat of single non-inline scalar literal
        bool is_splat = (val.count == 1);
        if (!is_splat) {
            is_splat = true;
            for (uint8_t i = 1; i < n; ++i) {
                if (val.const_vals[i] != val.const_vals[0]) {
                    is_splat = false;
                    break;
                }
            }
        }
        if (is_splat) {
            float cv = val.const_vals[0];
            if (val.is_absolute) cv = std::fabs(cv);
            if (val.is_negated) cv = -cv;
            ValueRef sc = get_or_create_float_const(cv);
            ValueRef tmp = make_temp_vec4(n);
            emit_move(RegisterClass::Temporary, tmp.base_index, mask_for_count(n), sc, comment);
            return tmp;
        }

        // Mixed literal vector
        ValueRef tmp = make_temp_vec4(n);
        for (uint8_t i = 0; i < n; ++i) {
            uint8_t src_i = (i < val.count) ? i : (val.count - 1);
            float cv = val.const_vals[src_i];
            if (val.is_absolute) cv = std::fabs(cv);
            if (val.is_negated) cv = -cv;
            SwizzleChannel ic{};
            if (is_inline_const_val(cv, ic)) {
                ValueRef ic_v = make_const_scalar(cv);
                std::array<uint8_t, 4> map = {0, 0, 0, 0};
                emit_move_mapped(RegisterClass::Temporary, tmp.base_index, static_cast<uint8_t>(1u << i), map, ic_v, comment);
            } else {
                ValueRef c_v = get_or_create_float_const(cv);
                std::array<uint8_t, 4> map = {0, 0, 0, 0};
                emit_move_mapped(RegisterClass::Temporary, tmp.base_index, static_cast<uint8_t>(1u << i), map, c_v, comment);
            }
        }
        return tmp;
    }

    ValueRef tmp = make_temp_vec4(n);
    emit_move(RegisterClass::Temporary, tmp.base_index, mask_for_count(n), val, comment);
    return tmp;
}

void UsseCodeGenerator::emit_move(
    RegisterClass dst_class,
    uint16_t dst_base,
    uint8_t write_mask,
    const ValueRef& src,
    const std::string& comment
) {
    std::array<uint8_t, 4> identity_map = {0, 1, 2, 3};
    emit_move_mapped(dst_class, dst_base, write_mask, identity_map, src, comment);
}

void UsseCodeGenerator::emit_move_mapped(
    RegisterClass dst_class,
    uint16_t dst_base,
    uint8_t write_mask,
    const std::array<uint8_t, 4>& target_lane_to_src_comp,
    const ValueRef& src,
    const std::string& comment
) {
    uint16_t dst_even = dst_base & ~1u;
    uint8_t dst_bias = static_cast<uint8_t>(dst_base & 1u);
    uint8_t hw_mask = static_cast<uint8_t>((write_mask & 0xFu) << dst_bias) & 0xFu;
    if (hw_mask == 0) hw_mask = 0x1u;

    if (src.is_const) {
        // Check if every active target lane is an inline constant {0, 1, 2, 0.5}
        bool all_inline = true;
        std::array<SwizzleChannel, 4> dst_swiz = {
            SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero
        };
        for (uint8_t dl = 0; dl < 4; ++dl) {
            if ((hw_mask & (1u << dl)) == 0) continue;
            uint8_t logical_dl = (dl >= dst_bias) ? static_cast<uint8_t>(dl - dst_bias) : 0;
            uint8_t sc = target_lane_to_src_comp[logical_dl];
            uint8_t clamped_sc = (sc < src.count) ? sc : (src.count - 1);
            float cv = src.const_vals[clamped_sc];
            if (src.is_absolute) cv = std::fabs(cv);
            if (src.is_negated) cv = -cv;
            if (!is_inline_const_val(cv, dst_swiz[dl])) {
                all_inline = false;
                break;
            }
        }
        if (all_inline) {
            HardwareRegister zero_hw = get_zero_hw_reg();
            UsseInstruction inst;
            inst.opcode = UsseOpcode::VADD;
            inst.dst.reg_class = dst_class;
            inst.dst.index = static_cast<uint8_t>(dst_even & 0xFFu);
            inst.src0 = zero_hw;
            inst.src1 = zero_hw;
            inst.write_mask = hw_mask;
            inst.src1_swiz12 = pack_swizzle12(dst_swiz[0], dst_swiz[1], dst_swiz[2], dst_swiz[3]);
            inst.src2_swiz4 = Vec4Swiz::XXXX;
            inst.comment = comment;
            emit_inst(std::move(inst));
            return;
        }

        // Materialize each non-inline constant lane cleanly
        for (uint8_t dl = 0; dl < 4; ++dl) {
            if ((hw_mask & (1u << dl)) == 0) continue;
            uint8_t logical_dl = (dl >= dst_bias) ? static_cast<uint8_t>(dl - dst_bias) : 0;
            uint8_t sc = target_lane_to_src_comp[logical_dl];
            uint8_t clamped_sc = (sc < src.count) ? sc : (src.count - 1);
            float cv = src.const_vals[clamped_sc];
            if (src.is_absolute) cv = std::fabs(cv);
            if (src.is_negated) cv = -cv;
            ValueRef loaded = get_or_create_float_const(cv);
            std::array<uint8_t, 4> zero_map = {0, 0, 0, 0};
            emit_move_mapped(dst_class, dst_base, static_cast<uint8_t>(1u << logical_dl), zero_map, loaded, comment);
        }
        return;
    }

    uint16_t raw_swiz12 = 0;
    HardwareRegister src1_hw = to_hw_src1(src, raw_swiz12);
    std::array<SwizzleChannel, 4> final_swiz = src1_hw.swizzle;
    for (uint8_t dl = 0; dl < 4; ++dl) {
        if ((hw_mask & (1u << dl)) == 0) continue;
        uint8_t logical_dl = (dl >= dst_bias) ? static_cast<uint8_t>(dl - dst_bias) : 0;
        uint8_t sc = target_lane_to_src_comp[logical_dl];
        uint8_t clamped_sc = (sc < src.count) ? sc : (src.count - 1);
        final_swiz[dl] = src1_hw.swizzle[clamped_sc];
    }

    HardwareRegister zero_hw = get_zero_hw_reg();
    UsseInstruction inst;
    inst.opcode = UsseOpcode::VADD;
    inst.dst.reg_class = dst_class;
    inst.dst.index = static_cast<uint8_t>(dst_even & 0xFFu);
    inst.src0 = src1_hw;
    inst.src1 = zero_hw;
    inst.write_mask = hw_mask;
    inst.src1_swiz12 = pack_swizzle12(final_swiz[0], final_swiz[1], final_swiz[2], final_swiz[3]);
    inst.src2_swiz4 = Vec4Swiz::XXXX;
    inst.comment = comment;
    emit_inst(std::move(inst));
}

ValueRef UsseCodeGenerator::emit_binary_alu(
    frontend::BinaryOp op,
    const ValueRef& lhs_in,
    const ValueRef& rhs_in,
    const std::string& comment
) {
    uint8_t n = std::max(lhs_in.count, rhs_in.count);
    n = std::clamp<uint8_t>(n, 1, 4);

    // 1. Compile-time constant folding
    if (lhs_in.is_const && rhs_in.is_const) {
        std::array<float, 4> folded{};
        for (uint8_t i = 0; i < n; ++i) {
            float l = lhs_in.const_vals[i < lhs_in.count ? i : lhs_in.count - 1];
            float r = rhs_in.const_vals[i < rhs_in.count ? i : rhs_in.count - 1];
            if (lhs_in.is_absolute) l = std::fabs(l);
            if (lhs_in.is_negated) l = -l;
            if (rhs_in.is_absolute) r = std::fabs(r);
            if (rhs_in.is_negated) r = -r;
            switch (op) {
                case frontend::BinaryOp::Add: folded[i] = l + r; break;
                case frontend::BinaryOp::Sub: folded[i] = l - r; break;
                case frontend::BinaryOp::Mul: folded[i] = l * r; break;
                case frontend::BinaryOp::Div: folded[i] = (r != 0.0f) ? (l / r) : 0.0f; break;
                case frontend::BinaryOp::Mod: folded[i] = (r != 0.0f) ? std::fmod(l, r) : 0.0f; break;
                default: folded[i] = 0.0f; break;
            }
        }
        return make_const_vec(folded, n);
    }

    // 2. Division via reciprocal + multiply
    if (op == frontend::BinaryOp::Div) {
        ValueRef inv_rhs = emit_vcomp_vec(UsseOpcode::VRCP, rhs_in, comment.empty() ? "rcp" : comment);
        return emit_binary_alu(frontend::BinaryOp::Mul, lhs_in, inv_rhs, comment);
    }

    // 3. Modulo / fmod: x - y * trunc(x / y)
    if (op == frontend::BinaryOp::Mod) {
        ValueRef q = emit_binary_alu(frontend::BinaryOp::Div, lhs_in, rhs_in, "mod div");
        ValueRef t = emit_trunc(q);
        ValueRef p = emit_binary_alu(frontend::BinaryOp::Mul, rhs_in, t, "mod mul");
        return emit_binary_alu(frontend::BinaryOp::Sub, lhs_in, p, "mod sub");
    }

    // 4. Prepare scalar-to-vector broadcast swizzles
    auto expand_swiz = [](ValueRef v, uint8_t target_n) {
        if (v.count == 1 && target_n > 1) {
            v.swiz = {v.swiz[0], v.swiz[0], v.swiz[0], v.swiz[0]};
            v.count = target_n;
        }
        return v;
    };

    ValueRef lhs = expand_swiz(lhs_in, n);
    ValueRef rhs = expand_swiz(rhs_in, n);

    // Ensure non-inline constants are materialized when needed (only src1 can use inline consts 0/1/2/0.5)
    auto has_only_inline_consts = [](const ValueRef& v) {
        if (!v.is_const) return false;
        SwizzleChannel dummy{};
        for (uint8_t i = 0; i < v.count; ++i) {
            if (!is_inline_const_val(v.const_vals[i], dummy)) return false;
        }
        return true;
    };

    if (lhs.is_const && !has_only_inline_consts(lhs)) {
        lhs = materialize_to_temp(lhs, n, "const lhs");
    }
    if (rhs.is_const && !has_only_inline_consts(rhs)) {
        rhs = materialize_to_temp(rhs, n, "const rhs");
    }

    ValueRef dst = make_temp_vec4(n);
    uint8_t wmask = mask_for_count(n);

    if (op == frontend::BinaryOp::Sub) {
        // V32NMAD VSUB(src1, src2) computes (-src1) + src2 = src2 - src1.
        // Put rhs (subtrahend) in hw_src1 and lhs (minuend) in hw_src2.
        if (lhs.is_const) {
            lhs = materialize_to_temp(lhs, n, "sub lhs");
        }
        Vec4Swiz src2_sw4 = Vec4Swiz::XYZW;
        HardwareRegister hw_src2 = to_hw_src2(lhs, src2_sw4);
        uint16_t src1_sw12 = kSwiz12XYZW;
        HardwareRegister hw_src1 = to_hw_src1(rhs, src1_sw12);

        UsseInstruction inst;
        inst.opcode = UsseOpcode::VSUB;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = static_cast<uint8_t>(dst.base_index);
        inst.src0 = hw_src1;
        inst.src1 = hw_src2;
        inst.write_mask = wmask;
        inst.src1_swiz12 = src1_sw12;
        inst.src2_swiz4 = src2_sw4;
        inst.comment = comment;
        emit_inst(std::move(inst));
        return dst;
    }

    // Commutative Add or Mul
    UsseOpcode hw_op = (op == frontend::BinaryOp::Mul) ? UsseOpcode::VMUL : UsseOpcode::VADD;
    if (op == frontend::BinaryOp::Mul && lhs.is_negated && rhs.is_negated) {
        lhs.is_negated = false;
        rhs.is_negated = false;
    }

    // Put inline constant, negated operand, or arbitrary-swizzled operand into hw_src1
    bool swap = false;
    if (rhs.is_const || rhs.is_negated) {
        if (!lhs.is_const && !lhs.is_negated) swap = true;
    }
    ValueRef& s1_ref = swap ? rhs : lhs;
    ValueRef& s2_ref = swap ? lhs : rhs;

    if (s2_ref.is_const) {
        s2_ref = materialize_to_temp(s2_ref, n, "const src2");
    }

    Vec4Swiz src2_sw4 = Vec4Swiz::XYZW;
    HardwareRegister hw_src2 = to_hw_src2(s2_ref, src2_sw4);
    uint16_t src1_sw12 = kSwiz12XYZW;
    HardwareRegister hw_src1 = to_hw_src1(s1_ref, src1_sw12);

    UsseInstruction inst;
    inst.opcode = hw_op;
    inst.dst.reg_class = RegisterClass::Temporary;
    inst.dst.index = static_cast<uint8_t>(dst.base_index);
    inst.src0 = hw_src1;
    inst.src1 = hw_src2;
    inst.write_mask = wmask;
    inst.src1_swiz12 = src1_sw12;
    inst.src2_swiz4 = src2_sw4;
    inst.comment = comment;
    emit_inst(std::move(inst));
    return dst;
}

ValueRef UsseCodeGenerator::emit_vcomp_vec(UsseOpcode op, const ValueRef& src_in, const std::string& comment) {
    uint8_t n = std::clamp<uint8_t>(src_in.count, 1, 4);

    if (src_in.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < n; ++i) {
            float v = src_in.const_vals[i < src_in.count ? i : src_in.count - 1];
            if (src_in.is_absolute) v = std::fabs(v);
            if (src_in.is_negated) v = -v;
            switch (op) {
                case UsseOpcode::VRCP: res[i] = (v != 0.0f) ? (1.0f / v) : 0.0f; break;
                case UsseOpcode::VRSQ: res[i] = (v > 0.0f) ? (1.0f / std::sqrt(v)) : 0.0f; break;
                case UsseOpcode::VLOG: res[i] = (v > 0.0f) ? std::log2(v) : 0.0f; break;
                case UsseOpcode::VEXP: res[i] = std::exp2(v); break;
                default: res[i] = v; break;
            }
        }
        return make_const_vec(res, n);
    }

    // VCOMP only reads register lanes 0..3 (no inline constant swizzles Zero/One/Two/Half)
    ValueRef src = src_in;
    bool has_inline = false;
    for (uint8_t i = 0; i < n; ++i) {
        if (static_cast<uint8_t>(src.swiz[i]) >= 4u) {
            has_inline = true;
            break;
        }
    }
    if (has_inline || src.reg_class == RegisterClass::Output) {
        src = materialize_to_temp(src, n, "vcomp src");
    }

    ValueRef dst = make_temp_vec4(n);
    uint16_t even_base = src.base_index & ~1u;
    uint8_t bias = static_cast<uint8_t>(src.base_index & 1u);

    // Group lanes that read the exact same source sub-component into one VCOMP
    std::array<uint8_t, 4> lane_masks = {0, 0, 0, 0};
    for (uint8_t i = 0; i < n; ++i) {
        uint8_t sc = static_cast<uint8_t>(src.swiz[i < src.count ? i : src.count - 1]);
        uint8_t phys_lane = static_cast<uint8_t>(std::min<uint8_t>(3u, sc + bias));
        lane_masks[phys_lane] |= static_cast<uint8_t>(1u << i);
    }

    for (uint8_t phys_lane = 0; phys_lane < 4; ++phys_lane) {
        if (lane_masks[phys_lane] == 0) continue;
        UsseInstruction inst;
        inst.opcode = op;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = static_cast<uint8_t>(dst.base_index);
        inst.src0.reg_class = src.reg_class;
        inst.src0.index = static_cast<uint8_t>(even_base & 0xFFu);
        inst.src0.sub_index = phys_lane;
        inst.src0.is_negated = src.is_negated;
        inst.src0.is_absolute = src.is_absolute;
        inst.src_comp = phys_lane;
        inst.write_mask = lane_masks[phys_lane];
        inst.comment = comment;
        emit_inst(std::move(inst));
    }
    return dst;
}

ValueRef UsseCodeGenerator::emit_dp(const ValueRef& a_in, const ValueRef& b_in, uint8_t comps, const std::string& comment) {
    uint8_t n = std::clamp<uint8_t>(comps, 1, 4);
    if (n == 1) {
        return emit_binary_alu(frontend::BinaryOp::Mul, a_in, b_in, comment);
    }
    if (a_in.is_const && b_in.is_const) {
        float acc = 0.0f;
        for (uint8_t i = 0; i < n; ++i) {
            float av = a_in.const_vals[i < a_in.count ? i : a_in.count - 1];
            float bv = b_in.const_vals[i < b_in.count ? i : b_in.count - 1];
            if (a_in.is_absolute) av = std::fabs(av);
            if (a_in.is_negated) av = -av;
            if (b_in.is_absolute) bv = std::fabs(bv);
            if (b_in.is_negated) bv = -bv;
            acc += av * bv;
        }
        return make_const_scalar(acc);
    }

    // Hardware V32NMAD VDP computes 4 products (x, y, z, w).
    // For n < 4, zero unused lanes in BOTH src1 (via SwizzleChannel::Zero) and src2 (via materialized clean temp).
    ValueRef a = a_in;
    ValueRef b = b_in;
    if (a.is_const) a = materialize_to_temp(a, n, "dp a");
    if (b.is_const) b = materialize_to_temp(b, n, "dp b");

    for (uint8_t i = n; i < 4; ++i) {
        a.swiz[i] = SwizzleChannel::Zero;
    }

    // Materialize b with zeroed unused lanes when n < 4 to guarantee 0.0 * non-NaN
    if (n < 4) {
        ValueRef b_clean = make_temp_vec4(4);
        ValueRef b_padded = b;
        for (uint8_t i = n; i < 4; ++i) {
            b_padded.swiz[i] = SwizzleChannel::Zero;
        }
        b_padded.count = 4;
        emit_move(RegisterClass::Temporary, b_clean.base_index, 0xF, b_padded, "zero unused dp lanes");
        b = b_clean;
    }

    Vec4Swiz src2_sw4 = Vec4Swiz::XYZW;
    HardwareRegister hw_src2 = to_hw_src2(b, src2_sw4);
    uint16_t src1_sw12 = kSwiz12XYZW;
    HardwareRegister hw_src1 = to_hw_src1(a, src1_sw12);

    ValueRef dst = make_temp_vec4(1);
    UsseInstruction inst;
    inst.opcode = (n <= 3) ? UsseOpcode::VDP3 : UsseOpcode::VDP4;
    inst.dst.reg_class = RegisterClass::Temporary;
    inst.dst.index = static_cast<uint8_t>(dst.base_index);
    inst.src0 = hw_src1;
    inst.src1 = hw_src2;
    inst.write_mask = 0b0001;
    inst.src1_swiz12 = src1_sw12;
    inst.src2_swiz4 = src2_sw4;
    inst.comment = comment;
    emit_inst(std::move(inst));

    dst.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
    return dst;
}

void UsseCodeGenerator::emit_vtst_p0(
    const ValueRef& lhs_in,
    const ValueRef& rhs_in,
    VtstSignTest sign_t,
    VtstZeroTest zero_t,
    const std::string& comment
) {
    // Materialize operands into clean lane-.x scalar temps if needed
    auto prep_scalar_x = [&](const ValueRef& v) -> HardwareRegister {
        if (!v.is_const && !v.is_negated && !v.is_absolute &&
            (v.base_index & 1u) == 0 && v.swiz[0] == SwizzleChannel::X &&
            (v.reg_class == RegisterClass::Temporary || v.reg_class == RegisterClass::InputAttr ||
             v.reg_class == RegisterClass::Constant || v.reg_class == RegisterClass::SecAttr)) {
            HardwareRegister r;
            r.reg_class = v.reg_class;
            r.index = static_cast<uint8_t>(v.base_index & 0xFFu);
            return r;
        }
        ValueRef t = materialize_to_temp(v, 1, "vtst op");
        HardwareRegister r;
        r.reg_class = RegisterClass::Temporary;
        r.index = static_cast<uint8_t>(t.base_index & 0xFFu);
        return r;
    };

    HardwareRegister l_hw = prep_scalar_x(lhs_in);
    HardwareRegister r_hw = prep_scalar_x(rhs_in);

    UsseInstruction inst;
    inst.opcode = UsseOpcode::VTST;
    inst.pdst_n = 0;
    inst.src0 = l_hw;
    inst.src1 = r_hw;
    inst.vtst_sign = sign_t;
    inst.vtst_zero = zero_t;
    inst.vtst_chan = 0;
    inst.comment = comment;
    emit_inst(std::move(inst));
}

ValueRef UsseCodeGenerator::emit_compare_to_float(frontend::BinaryOp op, const ValueRef& lhs, const ValueRef& rhs) {
    if (lhs.is_const && rhs.is_const) {
        float l = lhs.const_vals[0], r = rhs.const_vals[0];
        bool res = false;
        switch (op) {
            case frontend::BinaryOp::Equal:        res = (l == r); break;
            case frontend::BinaryOp::NotEqual:     res = (l != r); break;
            case frontend::BinaryOp::Less:         res = (l < r);  break;
            case frontend::BinaryOp::LessEqual:    res = (l <= r); break;
            case frontend::BinaryOp::Greater:      res = (l > r);  break;
            case frontend::BinaryOp::GreaterEqual: res = (l >= r); break;
            default: break;
        }
        return make_const_scalar(res ? 1.0f : 0.0f);
    }

    VtstSignTest st = VtstSignTest::NeOrEq;
    VtstZeroTest zt = VtstZeroTest::ExcludeZero;
    switch (op) {
        case frontend::BinaryOp::Equal:        st = VtstSignTest::NeOrEq; zt = VtstZeroTest::IncludeZero; break;
        case frontend::BinaryOp::NotEqual:     st = VtstSignTest::NeOrEq; zt = VtstZeroTest::ExcludeZero; break;
        case frontend::BinaryOp::Less:         st = VtstSignTest::LtOrLe; zt = VtstZeroTest::ExcludeZero; break;
        case frontend::BinaryOp::LessEqual:    st = VtstSignTest::LtOrLe; zt = VtstZeroTest::IncludeZero; break;
        case frontend::BinaryOp::Greater:      st = VtstSignTest::GtOrGe; zt = VtstZeroTest::ExcludeZero; break;
        case frontend::BinaryOp::GreaterEqual: st = VtstSignTest::GtOrGe; zt = VtstZeroTest::IncludeZero; break;
        default: break;
    }

    emit_vtst_p0(lhs, rhs, st, zt, "compare");

    ValueRef dst = make_temp_vec4(1);
    UsseInstruction f0;
    f0.opcode = UsseOpcode::LIMM;
    f0.dst.reg_class = RegisterClass::Temporary;
    f0.dst.index = static_cast<uint8_t>(dst.base_index);
    f0.imm32 = 0x00000000u; // 0.0f
    emit_inst(std::move(f0));

    UsseInstruction f1;
    f1.opcode = UsseOpcode::LIMM;
    f1.pred = ExtPredicate::P0;
    f1.dst.reg_class = RegisterClass::Temporary;
    f1.dst.index = static_cast<uint8_t>(dst.base_index);
    f1.imm32 = 0x3F800000u; // 1.0f
    f1.comment = "bool select";
    emit_inst(std::move(f1));

    dst.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
    return dst;
}

void UsseCodeGenerator::emit_condition_to_p0(const frontend::Expression* cond, bool invert) {
    if (!cond) {
        ValueRef one = make_const_scalar(1.0f);
        ValueRef zero = make_const_scalar(0.0f);
        emit_vtst_p0(one, zero, invert ? VtstSignTest::NeOrEq : VtstSignTest::NeOrEq,
                     invert ? VtstZeroTest::IncludeZero : VtstZeroTest::ExcludeZero, "always cond");
        return;
    }

    if (auto* un = dynamic_cast<const frontend::UnaryExpr*>(cond)) {
        if (un->op == frontend::UnaryOp::LogicalNot) {
            emit_condition_to_p0(un->operand.get(), !invert);
            return;
        }
    }

    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(cond)) {
        if (bin->op == frontend::BinaryOp::Equal || bin->op == frontend::BinaryOp::NotEqual ||
            bin->op == frontend::BinaryOp::Less || bin->op == frontend::BinaryOp::LessEqual ||
            bin->op == frontend::BinaryOp::Greater || bin->op == frontend::BinaryOp::GreaterEqual) {
            ValueRef l = generate_expr(bin->left.get());
            ValueRef r = generate_expr(bin->right.get());
            frontend::BinaryOp eff = bin->op;
            if (invert) {
                switch (eff) {
                    case frontend::BinaryOp::Equal:        eff = frontend::BinaryOp::NotEqual; break;
                    case frontend::BinaryOp::NotEqual:     eff = frontend::BinaryOp::Equal; break;
                    case frontend::BinaryOp::Less:         eff = frontend::BinaryOp::GreaterEqual; break;
                    case frontend::BinaryOp::LessEqual:    eff = frontend::BinaryOp::Greater; break;
                    case frontend::BinaryOp::Greater:      eff = frontend::BinaryOp::LessEqual; break;
                    case frontend::BinaryOp::GreaterEqual: eff = frontend::BinaryOp::Less; break;
                    default: break;
                }
            }
            VtstSignTest st = VtstSignTest::NeOrEq;
            VtstZeroTest zt = VtstZeroTest::ExcludeZero;
            switch (eff) {
                case frontend::BinaryOp::Equal:        st = VtstSignTest::NeOrEq; zt = VtstZeroTest::IncludeZero; break;
                case frontend::BinaryOp::NotEqual:     st = VtstSignTest::NeOrEq; zt = VtstZeroTest::ExcludeZero; break;
                case frontend::BinaryOp::Less:         st = VtstSignTest::LtOrLe; zt = VtstZeroTest::ExcludeZero; break;
                case frontend::BinaryOp::LessEqual:    st = VtstSignTest::LtOrLe; zt = VtstZeroTest::IncludeZero; break;
                case frontend::BinaryOp::Greater:      st = VtstSignTest::GtOrGe; zt = VtstZeroTest::ExcludeZero; break;
                case frontend::BinaryOp::GreaterEqual: st = VtstSignTest::GtOrGe; zt = VtstZeroTest::IncludeZero; break;
                default: break;
            }
            emit_vtst_p0(l, r, st, zt, "branch cond");
            return;
        }
    }

    ValueRef v = generate_expr(cond);
    ValueRef zero = make_const_scalar(0.0f);
    emit_vtst_p0(v, zero, VtstSignTest::NeOrEq,
                 invert ? VtstZeroTest::IncludeZero : VtstZeroTest::ExcludeZero, "truthy cond");
}

ValueRef UsseCodeGenerator::emit_floor(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = std::floor(x.const_vals[i]);
        return make_const_vec(res, x.count);
    }
    ValueRef f = make_temp_vec4(x.count);
    uint16_t sw12 = kSwiz12XYZW;
    HardwareRegister hw_s1 = to_hw_src1(x, sw12);
    Vec4Swiz sw4 = Vec4Swiz::XYZW;
    HardwareRegister hw_s2 = to_hw_src2(x, sw4);

    UsseInstruction frc;
    frc.opcode = UsseOpcode::VFRC;
    frc.dst.reg_class = RegisterClass::Temporary;
    frc.dst.index = static_cast<uint8_t>(f.base_index);
    frc.src0 = hw_s1;
    frc.src1 = hw_s2;
    frc.write_mask = mask_for_count(x.count);
    frc.src1_swiz12 = sw12;
    frc.src2_swiz4 = sw4;
    frc.comment = "frac for floor";
    emit_inst(std::move(frc));

    return emit_binary_alu(frontend::BinaryOp::Sub, x, f, "floor");
}

ValueRef UsseCodeGenerator::emit_ceil(const ValueRef& x) {
    ValueRef nx = x;
    nx.is_negated = !nx.is_negated;
    ValueRef fnx = emit_floor(nx);
    fnx.is_negated = !fnx.is_negated;
    return fnx;
}

ValueRef UsseCodeGenerator::emit_trunc(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = std::trunc(x.const_vals[i]);
        return make_const_vec(res, x.count);
    }
    ValueRef ax = x;
    ax.is_negated = false;
    ax.is_absolute = true;
    ValueRef fl = emit_floor(ax);
    // Multiply by sign(x): +1 if x >= 0 else -1 (Wait: if x < 0 negate floor(|x|))
    // Using sign(x) via x / max(|x|, 1e-12) or component check; simplest is float floor(x) + step adjustment:
    // trunc(x) = (x >= 0) ? floor(x) : -floor(-x)
    ValueRef nx = x;
    nx.is_negated = !nx.is_negated;
    ValueRef fneg = emit_floor(nx);
    fneg.is_negated = !fneg.is_negated;
    ValueRef fpos = emit_floor(x);
    // Compute per-component blend mask: is_neg = (x < 0.0) -> for scalars/vecs compare per component
    if (x.count == 1) {
        emit_vtst_p0(x, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "trunc sign");
        ValueRef dst = make_temp_vec4(1);
        emit_move(RegisterClass::Temporary, dst.base_index, 0x1, fpos, "trunc pos");
        size_t br_end = emit_br(0, ExtPredicate::NegP0, "skip trunc neg");
        emit_move(RegisterClass::Temporary, dst.base_index, 0x1, fneg, "trunc neg");
        patch_br(br_end, instructions_.size());
        dst.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return dst;
    }
    ValueRef dst = make_temp_vec4(x.count);
    for (uint8_t c = 0; c < x.count; ++c) {
        ValueRef xc = x;
        xc.count = 1;
        xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
        ValueRef tr = emit_trunc(xc);
        std::array<uint8_t, 4> map = {0, 0, 0, 0};
        emit_move_mapped(RegisterClass::Temporary, dst.base_index, static_cast<uint8_t>(1u << c), map, tr, "trunc lane");
    }
    return dst;
}

ValueRef UsseCodeGenerator::emit_sqrt(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = (x.const_vals[i] > 0.0f) ? std::sqrt(x.const_vals[i]) : 0.0f;
        return make_const_vec(res, x.count);
    }
    // Clamp below by tiny epsilon or multiply x * rsqrt(max(x, 1e-12)) so sqrt(0) = 0 (not Inf/NaN)
    ValueRef rsq = emit_vcomp_vec(UsseOpcode::VRSQ, x, "sqrt rsqrt");
    return emit_binary_alu(frontend::BinaryOp::Mul, x, rsq, "sqrt mul");
}

ValueRef UsseCodeGenerator::emit_pow(const ValueRef& x, const ValueRef& y) {
    if (x.is_const && y.is_const) {
        uint8_t n = std::max(x.count, y.count);
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < n; ++i) {
            float xv = x.const_vals[i < x.count ? i : x.count - 1];
            float yv = y.const_vals[i < y.count ? i : y.count - 1];
            res[i] = std::pow(std::max(xv, 1e-8f), yv);
        }
        return make_const_vec(res, n);
    }
    ValueRef lx = emit_vcomp_vec(UsseOpcode::VLOG, x, "pow log2");
    ValueRef prod = emit_binary_alu(frontend::BinaryOp::Mul, lx, y, "pow y*log2(x)");
    return emit_vcomp_vec(UsseOpcode::VEXP, prod, "pow exp2");
}

ValueRef UsseCodeGenerator::emit_sin_cos(const ValueRef& x_in, bool is_cos) {
    if (x_in.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x_in.count; ++i) {
            res[i] = is_cos ? std::cos(x_in.const_vals[i]) : std::sin(x_in.const_vals[i]);
        }
        return make_const_vec(res, x_in.count);
    }

    constexpr float kTwoPi = 6.28318530718f;
    constexpr float kInvTwoPi = 0.15915494309f;
    constexpr float kPi = 3.14159265359f;
    constexpr float kHalfPi = 1.57079632679f;
    constexpr float kB = 1.27323954474f;   // 4 / pi
    constexpr float kC = -0.40528473456f;  // -4 / pi^2
    constexpr float kP = 0.225f;

    ValueRef x = x_in;
    if (is_cos) {
        x = emit_binary_alu(frontend::BinaryOp::Add, x, make_const_scalar(kHalfPi), "cos shift");
    }

    // Range-reduce x into [-pi, pi]: xr = frac(x * (1/2pi) + 0.5) * 2pi - pi
    ValueRef scaled = emit_binary_alu(frontend::BinaryOp::Mul, x, make_const_scalar(kInvTwoPi), "sin/cos inv2pi");
    ValueRef shifted = emit_binary_alu(frontend::BinaryOp::Add, scaled, make_const_scalar(0.5f), "sin/cos +0.5");

    ValueRef frc = make_temp_vec4(x.count);
    {
        uint16_t sw12 = kSwiz12XYZW;
        HardwareRegister hw_s1 = to_hw_src1(shifted, sw12);
        Vec4Swiz sw4 = Vec4Swiz::XYZW;
        HardwareRegister hw_s2 = to_hw_src2(shifted, sw4);
        UsseInstruction inst;
        inst.opcode = UsseOpcode::VFRC;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = static_cast<uint8_t>(frc.base_index);
        inst.src0 = hw_s1;
        inst.src1 = hw_s2;
        inst.write_mask = mask_for_count(x.count);
        inst.src1_swiz12 = sw12;
        inst.src2_swiz4 = sw4;
        inst.comment = "sin/cos frac";
        emit_inst(std::move(inst));
    }

    ValueRef xr = emit_binary_alu(frontend::BinaryOp::Sub,
                                  emit_binary_alu(frontend::BinaryOp::Mul, frc, make_const_scalar(kTwoPi), "sin/cos *2pi"),
                                  make_const_scalar(kPi), "sin/cos -pi");

    // y = B * xr + C * xr * |xr|
    ValueRef abs_xr = xr;
    abs_xr.is_absolute = true;
    ValueRef term1 = emit_binary_alu(frontend::BinaryOp::Mul, xr, make_const_scalar(kB), "sin B*x");
    ValueRef term2 = emit_binary_alu(frontend::BinaryOp::Mul,
                                     emit_binary_alu(frontend::BinaryOp::Mul, xr, abs_xr, "x*|x|"),
                                     make_const_scalar(kC), "C*x*|x|");
    ValueRef y = emit_binary_alu(frontend::BinaryOp::Add, term1, term2, "sin poly y");

    // y + P * (y * |y| - y)
    ValueRef abs_y = y;
    abs_y.is_absolute = true;
    ValueRef y_abs_y = emit_binary_alu(frontend::BinaryOp::Mul, y, abs_y, "y*|y|");
    ValueRef diff = emit_binary_alu(frontend::BinaryOp::Sub, y_abs_y, y, "y*|y|-y");
    ValueRef corr = emit_binary_alu(frontend::BinaryOp::Mul, diff, make_const_scalar(kP), "P*(y*|y|-y)");
    return emit_binary_alu(frontend::BinaryOp::Add, y, corr, is_cos ? "cos result" : "sin result");
}

ValueRef UsseCodeGenerator::emit_matrix_mul(const ValueRef& lhs, const ValueRef& rhs) {
    // 1. Matrix * Vector
    if (lhs.is_matrix && !rhs.is_matrix) {
        uint8_t rows = std::clamp<uint8_t>(lhs.mat_rows, 2, 4);
        uint8_t cols = std::clamp<uint8_t>(lhs.mat_cols, 2, 4);
        ValueRef vec_clean = rhs;
        if (cols < 4 || rhs.is_const) {
            vec_clean = make_temp_vec4(4);
            ValueRef padded = rhs;
            for (uint8_t c = cols; c < 4; ++c) {
                padded.swiz[c] = SwizzleChannel::Zero;
            }
            padded.count = 4;
            emit_move(RegisterClass::Temporary, vec_clean.base_index, 0xF, padded, "mat*vec input");
        }

        ValueRef out = make_temp_vec4(rows);
        Vec4Swiz vec_sw4 = Vec4Swiz::XYZW;
        HardwareRegister hw_vec = to_hw_src2(vec_clean, vec_sw4);

        for (uint8_t r = 0; r < rows; ++r) {
            ValueRef mrow;
            mrow.reg_class = lhs.reg_class;
            mrow.base_index = static_cast<uint16_t>(lhs.base_index + r * lhs.row_stride);
            mrow.count = cols;
            mrow.swiz = {SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W};
            for (uint8_t c = cols; c < 4; ++c) mrow.swiz[c] = SwizzleChannel::Zero;

            uint16_t row_sw12 = kSwiz12XYZW;
            HardwareRegister hw_row = to_hw_src1(mrow, row_sw12);

            UsseInstruction inst;
            inst.opcode = (cols <= 3) ? UsseOpcode::VDP3 : UsseOpcode::VDP4;
            inst.dst.reg_class = RegisterClass::Temporary;
            inst.dst.index = static_cast<uint8_t>(out.base_index);
            inst.src0 = hw_row;
            inst.src1 = hw_vec;
            inst.write_mask = static_cast<uint8_t>(1u << r);
            inst.src1_swiz12 = row_sw12;
            inst.src2_swiz4 = vec_sw4;
            inst.comment = "mul(mat row " + std::to_string(r) + ", vec)";
            emit_inst(std::move(inst));
        }
        return out;
    }

    // 2. Vector * Matrix (row-vector * mat -> col dot products)
    if (!lhs.is_matrix && rhs.is_matrix) {
        uint8_t rows = std::clamp<uint8_t>(rhs.mat_rows, 2, 4);
        uint8_t cols = std::clamp<uint8_t>(rhs.mat_cols, 2, 4);
        ValueRef out = make_temp_vec4(cols);
        for (uint8_t c = 0; c < cols; ++c) {
            ValueRef col_vec = make_temp_vec4(4);
            SwizzleChannel ch = sub_to_swiz_chan(c);
            for (uint8_t r = 0; r < rows; ++r) {
                ValueRef mrow;
                mrow.reg_class = rhs.reg_class;
                mrow.base_index = static_cast<uint16_t>(rhs.base_index + r * rhs.row_stride);
                mrow.count = 1;
                mrow.swiz = {ch, ch, ch, ch};
                std::array<uint8_t, 4> map = {0, 0, 0, 0};
                emit_move_mapped(RegisterClass::Temporary, col_vec.base_index, static_cast<uint8_t>(1u << r), map, mrow, "extract mat col");
            }
            for (uint8_t r = rows; r < 4; ++r) {
                std::array<uint8_t, 4> map = {0, 0, 0, 0};
                emit_move_mapped(RegisterClass::Temporary, col_vec.base_index, static_cast<uint8_t>(1u << r), map, make_const_scalar(0.0f));
            }
            ValueRef dp = emit_dp(lhs, col_vec, rows, "mul(vec, mat col " + std::to_string(c) + ")");
            std::array<uint8_t, 4> map = {0, 0, 0, 0};
            emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, dp);
        }
        return out;
    }

    // 3. Matrix * Matrix
    if (lhs.is_matrix && rhs.is_matrix) {
        uint8_t rows = std::clamp<uint8_t>(lhs.mat_rows, 2, 4);
        uint8_t cols = std::clamp<uint8_t>(rhs.mat_cols, 2, 4);
        uint8_t base = reg_allocator_.allocate_contiguous_temp_vec4s(rows);
        if (!scopes_.empty()) {
            for (uint8_t r = 0; r < rows; ++r) {
                scopes_.back().owned_vec4_temps.push_back(static_cast<uint8_t>(base + r * 4));
            }
        }
        for (uint8_t r = 0; r < rows; ++r) {
            ValueRef row_v;
            row_v.reg_class = lhs.reg_class;
            row_v.base_index = static_cast<uint16_t>(lhs.base_index + r * lhs.row_stride);
            row_v.count = lhs.mat_cols;
            ValueRef res_row = emit_matrix_mul(row_v, rhs);
            emit_move(RegisterClass::Temporary, static_cast<uint16_t>(base + r * 4), mask_for_count(cols), res_row, "mat*mat row");
        }
        ValueRef m;
        m.reg_class = RegisterClass::Temporary;
        m.base_index = base;
        m.count = cols;
        m.is_matrix = true;
        m.mat_rows = rows;
        m.mat_cols = cols;
        m.row_stride = 4;
        return m;
    }

    return emit_binary_alu(frontend::BinaryOp::Mul, lhs, rhs, "mul");
}

ValueRef UsseCodeGenerator::binding_to_value(const SymbolBinding& sym) const {
    ValueRef v;
    v.reg_class = sym.reg_class;
    v.base_index = sym.base_register;
    if (sym.type.is_matrix()) {
        v.is_matrix = true;
        v.mat_rows = static_cast<uint8_t>(sym.type.matrix_rows());
        v.mat_cols = static_cast<uint8_t>(sym.type.matrix_cols());
        uint32_t arr = (sym.type.array_size > 1) ? sym.type.array_size : 1u;
        uint16_t per_mat = static_cast<uint16_t>(sym.allocated_words / arr);
        v.row_stride = (v.mat_rows > 0 && per_mat >= v.mat_rows)
                     ? static_cast<uint16_t>(per_mat / v.mat_rows)
                     : 4u;
        v.count = v.mat_cols;
    } else if (sym.type.array_size > 1) {
        v.is_array = true;
        v.array_size = sym.type.array_size;
        uint16_t per_elem = (sym.allocated_words >= sym.type.array_size)
            ? static_cast<uint16_t>(sym.allocated_words / sym.type.array_size)
            : 4;
        v.elem_stride = std::max<uint16_t>(1, per_elem);
        v.count = static_cast<uint8_t>(std::clamp<size_t>(sym.type.component_count(), 1, 4));
    } else if (sym.type.base == frontend::BaseType::CustomStruct) {
        v.struct_name = sym.type.struct_name;
        v.struct_instance = sym.name;
        v.count = 4;
    } else {
        v.count = static_cast<uint8_t>(std::clamp<size_t>(sym.type.component_count(), 1, 4));
    }
    return v;
}

bool UsseCodeGenerator::resolve_lvalue(
    const frontend::Expression* expr,
    RegisterClass& out_class,
    uint16_t& out_base,
    uint8_t& out_mask,
    std::array<uint8_t, 4>& out_lane_map,
    ValueRef& out_cur_val,
    std::string& out_label
) {
    out_lane_map = {0, 1, 2, 3};

    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(expr)) {
        const auto* sym = reg_allocator_.find_symbol(var->name);
        if (!sym) return false;
        out_class = sym->reg_class;
        out_base = sym->base_register;
        uint8_t n = static_cast<uint8_t>(std::clamp<size_t>(sym->type.component_count(), 1, 4));
        out_mask = mask_for_count(n);
        out_cur_val = binding_to_value(*sym);
        out_label = var->name;
        return true;
    }

    if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(expr)) {
        if (auto* base_var = dynamic_cast<const frontend::VariableExpr*>(swiz->target.get())) {
            std::string qualified = base_var->name + "." + swiz->components;
            const auto* sym = reg_allocator_.find_symbol(qualified);
            if (!sym) {
                const auto* base_sym = reg_allocator_.find_symbol(base_var->name);
                if (!base_sym || base_sym->type.base == frontend::BaseType::CustomStruct) {
                    sym = reg_allocator_.find_symbol(swiz->components);
                }
            }
            if (sym) {
                out_class = sym->reg_class;
                out_base = sym->base_register;
                uint8_t n = static_cast<uint8_t>(std::clamp<size_t>(sym->type.component_count(), 1, 4));
                out_mask = mask_for_count(n);
                out_cur_val = binding_to_value(*sym);
                out_label = qualified;
                return true;
            }
        }

        // Chained swizzle e.g. output.color.rgb = ... or col.xy = ...
        RegisterClass inner_cls{};
        uint16_t inner_base = 0;
        uint8_t inner_mask = 0xF;
        std::array<uint8_t, 4> inner_map{};
        ValueRef inner_val;
        std::string inner_label;
        if (!resolve_lvalue(swiz->target.get(), inner_cls, inner_base, inner_mask, inner_map, inner_val, inner_label)) {
            return false;
        }
        out_class = inner_cls;
        out_base = inner_base;
        out_mask = 0;
        out_lane_map = {0, 0, 0, 0};
        for (size_t i = 0; i < swiz->components.size() && i < 4; ++i) {
            uint8_t lane = swizzle_char_to_sub(swiz->components[i]);
            out_mask |= static_cast<uint8_t>(1u << lane);
            out_lane_map[lane] = static_cast<uint8_t>(i);
        }
        if (out_mask == 0) out_mask = 0xF;
        out_cur_val = generate_swizzle(swiz);
        out_label = inner_label + "." + swiz->components;
        return true;
    }

    if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(expr)) {
        int64_t idx = 0;
        if (eval_const_int(arr->index.get(), idx) && idx >= 0) {
            RegisterClass inner_cls{};
            uint16_t inner_base = 0;
            uint8_t inner_mask = 0xF;
            std::array<uint8_t, 4> inner_map{};
            ValueRef inner_val;
            std::string inner_label;
            if (!resolve_lvalue(arr->target.get(), inner_cls, inner_base, inner_mask, inner_map, inner_val, inner_label)) {
                return false;
            }
            uint16_t stride = inner_val.is_matrix ? inner_val.row_stride : inner_val.elem_stride;
            out_class = inner_cls;
            out_base = static_cast<uint16_t>(inner_base + static_cast<uint16_t>(idx) * stride);
            out_mask = mask_for_count(inner_val.count);
            out_cur_val = inner_val;
            out_cur_val.base_index = out_base;
            out_cur_val.is_array = false;
            out_cur_val.is_matrix = false;
            out_label = inner_label + "[" + std::to_string(idx) + "]";
            return true;
        }
    }

    return false;
}

void UsseCodeGenerator::assign_value_to_lvalue(
    const frontend::Expression* lvalue,
    frontend::BinaryOp compound_op,
    const ValueRef& rhs
) {
    RegisterClass dst_cls{};
    uint16_t dst_base = 0;
    uint8_t dst_mask = 0xF;
    std::array<uint8_t, 4> lane_map{};
    ValueRef cur_val;
    std::string label;

    if (!resolve_lvalue(lvalue, dst_cls, dst_base, dst_mask, lane_map, cur_val, label)) {
        return;
    }

    // Struct-to-struct copy (e.g. VS_OUTPUT o = helper(); or o = other;)
    if (!cur_val.struct_name.empty() && !rhs.struct_name.empty() && tu_) {
        if (const auto* s = tu_->find_struct(cur_val.struct_name)) {
            const std::string& lhs_prefix = label;
            const std::string& rhs_prefix = rhs.struct_instance;
            for (const auto& f : s->fields) {
                const auto* dst_sym = reg_allocator_.find_symbol(lhs_prefix + "." + f.name);
                if (!dst_sym) dst_sym = reg_allocator_.find_symbol(f.name);
                const auto* src_sym = !rhs_prefix.empty()
                    ? reg_allocator_.find_symbol(rhs_prefix + "." + f.name)
                    : nullptr;
                if (!src_sym) src_sym = reg_allocator_.find_symbol(f.name);
                if (dst_sym && src_sym) {
                    emit_move(dst_sym->reg_class, dst_sym->base_register,
                              mask_for_count(static_cast<uint8_t>(dst_sym->type.component_count())),
                              binding_to_value(*src_sym), lhs_prefix + "." + f.name);
                }
            }
            return;
        }
    }

    ValueRef final_rhs = rhs;
    if (compound_op != frontend::BinaryOp::Assign) {
        final_rhs = emit_binary_alu(compound_op, cur_val, rhs, label + " compound");
    }

    // Track fragment DEPTH output if written
    if (is_fragment_ && dst_cls == RegisterClass::Output && uses_depth_) {
        // Check if this symbol corresponds to DEPTH semantic
        bool is_depth = false;
        for (const auto& o : reg_allocator_.get_outputs()) {
            if (o.base_register == dst_base && semantic_contains(o.semantic, "DEPTH")) {
                is_depth = true;
                break;
            }
        }
        if (is_depth) {
            if (!depth_src_reg_) {
                HardwareRegister dr;
                dr.reg_class = RegisterClass::Temporary;
                dr.index = reg_allocator_.allocate_temp_vec4();
                depth_src_reg_ = dr;
            }
            emit_move(RegisterClass::Temporary, depth_src_reg_->index, 0x1, final_rhs, "store DEPTH");
            return;
        }
    }

    emit_move_mapped(dst_cls, dst_base, dst_mask, lane_map, final_rhs, label + " write");
}

bool UsseCodeGenerator::generate(
    const frontend::TranslationUnit& tu,
    const std::string& entry_name,
    std::optional<bool> force_fragment,
    CompiledProgram& out_program
) {
    tu_ = &tu;
    reg_allocator_.reset();
    instructions_.clear();
    return_struct_fields_.clear();
    entry_return_patches_.clear();
    loop_stack_.clear();
    scopes_.clear();
    error_message_.clear();
    uses_discard_ = false;
    uses_depth_ = false;
    depth_src_reg_.reset();
    has_zero_reg_ = false;
    zero_reg_index_ = 0;
    const_cache_base_ = 0;
    const_cache_sub_ = 4;
    float_const_cache_.clear();
    inline_call_ctx_ = {};
    inline_depth_ = 0;

    // 1. Find entry point
    const std::string target_entry = entry_name.empty() ? "main" : entry_name;
    const frontend::FunctionDecl* entry_fn = tu.find_entry_point(target_entry);
    if (!entry_fn && target_entry != "main") {
        entry_fn = tu.find_entry_point("main");
    }
    if (!entry_fn && !tu.functions.empty()) {
        entry_fn = &tu.functions.back();
    }
    if (!entry_fn) {
        error_message_ = "No entry point function '" + target_entry + "' found in translation unit";
        return false;
    }
    entry_fn_ = entry_fn;
    active_fn_ = entry_fn;

    // 2. Allocate top-level uniform & sampler declarations
    for (const auto& u : tu.uniforms) {
        if (reg_allocator_.find_symbol(u.name)) continue;
        SymbolBinding binding;
        if (!reg_allocator_.allocate_uniform(u.name, u.type, u.semantic, binding)) {
            error_message_ = "Constant/sampler register overflow allocating uniform: " + u.name;
            return false;
        }
    }

    // Also pre-scan entry parameters for uniforms/samplers (needed before stage heuristic)
    for (const auto& param : entry_fn->parameters) {
        if (param.is_uniform || param.type.is_sampler()) {
            if (param.type.base == frontend::BaseType::CustomStruct) {
                if (const auto* s = tu.find_struct(param.type.struct_name)) {
                    for (const auto& field : s->fields) {
                        SymbolBinding binding;
                        std::string qname = param.name + "." + field.name;
                        if (!reg_allocator_.find_symbol(qname)) {
                            if (!reg_allocator_.allocate_uniform(qname, field.type, field.semantic, binding)) {
                                error_message_ = "Uniform overflow allocating struct param: " + qname;
                                return false;
                            }
                        }
                    }
                }
            } else if (!reg_allocator_.find_symbol(param.name)) {
                SymbolBinding binding;
                if (!reg_allocator_.allocate_uniform(param.name, param.type, param.semantic, binding)) {
                    error_message_ = "Uniform overflow allocating parameter: " + param.name;
                    return false;
                }
            }
        }
    }

    // 3. Determine shader stage BEFORE allocating inputs/outputs
    bool is_frag = false;
    if (force_fragment.has_value()) {
        is_frag = *force_fragment;
    } else {
        bool has_pos_out = semantic_contains(entry_fn->return_semantic, "POSITION") ||
                           semantic_contains(entry_fn->return_semantic, "HPOS");
        bool has_color_out = semantic_contains(entry_fn->return_semantic, "COLOR") ||
                             semantic_contains(entry_fn->return_semantic, "FRAG");
        bool has_depth_out = semantic_contains(entry_fn->return_semantic, "DEPTH");

        for (const auto& param : entry_fn->parameters) {
            if (param.is_out) {
                if (param.type.base == frontend::BaseType::CustomStruct) {
                    if (const auto* s = tu.find_struct(param.type.struct_name)) {
                        for (const auto& f : s->fields) {
                            if (semantic_contains(f.semantic, "POSITION") || semantic_contains(f.semantic, "HPOS")) {
                                has_pos_out = true;
                            }
                            if (semantic_contains(f.semantic, "COLOR") || semantic_contains(f.semantic, "FRAG")) {
                                has_color_out = true;
                            }
                            if (semantic_contains(f.semantic, "DEPTH")) {
                                has_depth_out = true;
                            }
                        }
                    }
                } else {
                    if (semantic_contains(param.semantic, "POSITION") || semantic_contains(param.semantic, "HPOS")) {
                        has_pos_out = true;
                    }
                    if (semantic_contains(param.semantic, "COLOR") || semantic_contains(param.semantic, "FRAG")) {
                        has_color_out = true;
                    }
                    if (semantic_contains(param.semantic, "DEPTH")) {
                        has_depth_out = true;
                    }
                }
            }
        }

        if (entry_fn->return_type.base == frontend::BaseType::CustomStruct) {
            if (const auto* s = tu.find_struct(entry_fn->return_type.struct_name)) {
                for (const auto& f : s->fields) {
                    if (semantic_contains(f.semantic, "POSITION") || semantic_contains(f.semantic, "HPOS")) {
                        has_pos_out = true;
                    }
                    if (semantic_contains(f.semantic, "COLOR") || semantic_contains(f.semantic, "FRAG")) {
                        has_color_out = true;
                    }
                    if (semantic_contains(f.semantic, "DEPTH")) {
                        has_depth_out = true;
                    }
                }
            }
        }

        bool has_discard = statement_contains_discard(entry_fn->body.get());
        if (has_discard || has_depth_out || (!has_pos_out && (has_color_out || !reg_allocator_.get_samplers().empty()))) {
            is_frag = true;
        }
    }
    is_fragment_ = is_frag;

    // 4. Allocate non-uniform input and output parameters of entry function
    for (const auto& param : entry_fn->parameters) {
        if (param.is_uniform || param.type.is_sampler()) continue;

        if (param.is_out && !param.is_in) {
            if (param.type.base == frontend::BaseType::CustomStruct) {
                if (const auto* s = tu.find_struct(param.type.struct_name)) {
                    for (const auto& field : s->fields) {
                        SymbolBinding binding;
                        std::string qualified = param.name + "." + field.name;
                        reg_allocator_.allocate_output(qualified, field.type, field.semantic, binding);
                        return_struct_fields_.push_back(binding);
                        if (!reg_allocator_.find_symbol(field.name)) {
                            reg_allocator_.alias_symbol(field.name, binding);
                        }
                    }
                    SymbolBinding whole{};
                    whole.name = param.name;
                    whole.type = param.type;
                    whole.reg_class = RegisterClass::Output;
                    reg_allocator_.alias_symbol(param.name, whole);
                }
            } else {
                SymbolBinding binding;
                reg_allocator_.allocate_output(param.name, param.type, param.semantic, binding);
                if (is_frag && semantic_contains(param.semantic, "DEPTH")) {
                    uses_depth_ = true;
                }
            }
        } else {
            if (param.type.base == frontend::BaseType::CustomStruct) {
                if (const auto* s = tu.find_struct(param.type.struct_name)) {
                    for (const auto& field : s->fields) {
                        SymbolBinding binding;
                        std::string qualified = param.name + "." + field.name;
                        reg_allocator_.allocate_input(qualified, field.type, field.semantic, binding, is_frag);
                        if (!reg_allocator_.find_symbol(field.name)) {
                            reg_allocator_.alias_symbol(field.name, binding);
                        }
                    }
                }
            } else {
                SymbolBinding binding;
                reg_allocator_.allocate_input(param.name, param.type, param.semantic, binding, is_frag);
            }
        }
    }

    // 5. Allocate outputs from entry return type
    if (entry_fn->return_type.base == frontend::BaseType::CustomStruct) {
        if (const auto* s = tu.find_struct(entry_fn->return_type.struct_name)) {
            for (const auto& field : s->fields) {
                SymbolBinding binding;
                reg_allocator_.allocate_output(field.name, field.type, field.semantic, binding);
                return_struct_fields_.push_back(binding);
                if (is_frag && semantic_contains(field.semantic, "DEPTH")) {
                    uses_depth_ = true;
                }
            }
        }
    } else if (entry_fn->return_type.base != frontend::BaseType::Void) {
        SymbolBinding binding;
        std::string out_name = is_frag ? "gl_FragColor" : "gl_Position";
        std::string out_sem = entry_fn->return_semantic.empty()
                            ? (is_frag ? "COLOR" : "POSITION")
                            : entry_fn->return_semantic;
        reg_allocator_.allocate_output(out_name, entry_fn->return_type, out_sem, binding);
        if (is_frag && semantic_contains(out_sem, "DEPTH")) {
            uses_depth_ = true;
        }
    }

    // 6. Finalize canonical vertex output register order BEFORE generating entry body
    if (!is_frag) {
        reg_allocator_.finalize_canonical_vertex_outputs();
        for (auto& rf : return_struct_fields_) {
            if (const auto* updated = reg_allocator_.find_symbol(rf.name)) {
                rf = *updated;
            }
        }
    }

    // 7. Emit global variable initializers, then entry function body
    push_scope();
    for (const auto& g : tu.globals) {
        generate_var_decl(&g);
    }
    if (entry_fn->body) {
        generate_block(entry_fn->body.get());
    }
    pop_scope();

    size_t epilogue_idx = instructions_.size();
    for (size_t br_idx : entry_return_patches_) {
        patch_br(br_idx, epilogue_idx);
    }

    // 8. Emit DEPTHF feedback if fragment depth output was written
    if (is_frag && uses_depth_ && depth_src_reg_.has_value()) {
        UsseInstruction df;
        df.opcode = UsseOpcode::DEPTHF;
        df.src0 = *depth_src_reg_;
        df.comment = "Fragment depth feedback";
        emit_inst(std::move(df));
    }

    // 9. Terminate shader cleanly with PHAS (end=true)
    emit_phase_switch();

    // 10. Assemble binary microcode words & metadata
    out_program.instructions = instructions_;
    out_program.uniforms = reg_allocator_.get_uniforms();
    out_program.samplers = reg_allocator_.get_samplers();
    out_program.inputs = reg_allocator_.get_inputs();
    out_program.outputs = reg_allocator_.get_outputs();
    out_program.temp_register_count = std::max<uint8_t>(1, reg_allocator_.get_temporary_count());
    out_program.const_register_count = reg_allocator_.get_constant_count();
    out_program.sampler_sa_base = reg_allocator_.get_sampler_sa_base();

    uint8_t total_pa = 0;
    for (const auto& in : out_program.inputs) {
        uint8_t end_reg = static_cast<uint8_t>(in.base_register + std::max<uint16_t>(in.allocated_words, in.register_count));
        if (end_reg > total_pa) total_pa = end_reg;
    }
    out_program.input_register_count = std::max<uint8_t>(1, total_pa);
    out_program.is_vertex_shader = !is_frag;
    out_program.uses_discard = uses_discard_;
    out_program.uses_depth = uses_depth_ && depth_src_reg_.has_value();

    std::ostringstream disasm;
    disasm << "; ================================================================\n";
    disasm << "; open-shacc USSE Disassembly (" << (out_program.is_vertex_shader ? "Vertex" : "Fragment") << ")\n";
    disasm << "; Clean-room architecture target: PowerVR SGX / USSE\n";
    disasm << "; ================================================================\n\n";

    disasm << "; --- Uniform Constant Mapping ---\n";
    for (const auto& u : out_program.uniforms) {
        disasm << ";   " << u.name << " -> sa" << u.base_register
               << " (size: " << u.register_count << " words)\n";
    }

    disasm << "\n; --- Texture Samplers ---\n";
    for (const auto& s : out_program.samplers) {
        disasm << ";   " << s.name << " -> s" << s.base_register
               << " (sa" << reg_allocator_.get_sampler_sa_offset(s.base_register) << ")\n";
    }

    disasm << "\n; --- Attributes / Inputs ---\n";
    for (const auto& in : out_program.inputs) {
        disasm << ";   " << in.name << " (" << (in.hw_semantic.empty() ? in.semantic : in.hw_semantic)
               << ") -> pa" << in.base_register << "\n";
    }

    disasm << "\n; --- Outputs ---\n";
    for (const auto& out : out_program.outputs) {
        disasm << ";   " << out.name << " (" << out.semantic << ") -> o" << out.base_register << "\n";
    }

    disasm << "\n; --- Program Instructions ---\n";
    out_program.microcode_words.clear();
    out_program.microcode_words.reserve(instructions_.size());

    for (size_t i = 0; i < instructions_.size(); ++i) {
        const auto& inst = instructions_[i];
        MicrocodeWord word = inst.to_microcode();
        out_program.microcode_words.push_back(word.raw_word);

        std::ostringstream hex_word;
        hex_word << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << word.raw_word;
        disasm << std::left << std::setw(6) << i << ": [" << hex_word.str() << "]  " << inst.disassemble() << "\n";
    }

    out_program.disassembly = disasm.str();
    return true;
}

void UsseCodeGenerator::generate_statement(const frontend::Statement* stmt) {
    if (!stmt) return;
    if (auto* b = dynamic_cast<const frontend::BlockStmt*>(stmt)) {
        generate_block(b);
    } else if (auto* v = dynamic_cast<const frontend::VarDeclStmt*>(stmt)) {
        generate_var_decl(v);
    } else if (auto* a = dynamic_cast<const frontend::AssignStmt*>(stmt)) {
        generate_assign(a);
    } else if (auto* r = dynamic_cast<const frontend::ReturnStmt*>(stmt)) {
        generate_return(r);
    } else if (auto* i = dynamic_cast<const frontend::IfStmt*>(stmt)) {
        generate_if(i);
    } else if (auto* f = dynamic_cast<const frontend::ForStmt*>(stmt)) {
        generate_for(f);
    } else if (auto* w = dynamic_cast<const frontend::WhileStmt*>(stmt)) {
        generate_while(w);
    } else if (auto* dw = dynamic_cast<const frontend::DoWhileStmt*>(stmt)) {
        generate_do_while(dw);
    } else if (auto* d = dynamic_cast<const frontend::DiscardStmt*>(stmt)) {
        generate_discard(d);
    } else if (dynamic_cast<const frontend::BreakStmt*>(stmt)) {
        if (!loop_stack_.empty()) {
            size_t br = emit_br(0, ExtPredicate::None, "break");
            loop_stack_.back().break_patches.push_back(br);
        }
    } else if (dynamic_cast<const frontend::ContinueStmt*>(stmt)) {
        if (!loop_stack_.empty()) {
            size_t br = emit_br(0, ExtPredicate::None, "continue");
            loop_stack_.back().continue_patches.push_back(br);
        }
    }
}

void UsseCodeGenerator::generate_block(const frontend::BlockStmt* block) {
    if (!block) return;
    push_scope();
    for (const auto& s : block->statements) {
        generate_statement(s.get());
    }
    pop_scope();
}

void UsseCodeGenerator::generate_var_decl(const frontend::VarDeclStmt* decl) {
    if (!decl) return;

    // Entry function return struct variable -> alias directly to Output registers
    if (decl->type.base == frontend::BaseType::CustomStruct && active_fn_ == entry_fn_ &&
        entry_fn_ && entry_fn_->return_type.base == frontend::BaseType::CustomStruct &&
        decl->type.struct_name == entry_fn_->return_type.struct_name) {
        for (const auto& field : return_struct_fields_) {
            bind_scoped_symbol(decl->name + "." + field.name, field);
        }
        SymbolBinding whole{};
        whole.name = decl->name;
        whole.type = decl->type;
        whole.reg_class = RegisterClass::Output;
        whole.base_register = 0;
        whole.register_count = 4;
        bind_scoped_symbol(decl->name, whole);
        if (decl->initializer) {
            ValueRef rhs = generate_expr(decl->initializer.get());
            frontend::VariableExpr lhs_var(decl->name);
            assign_value_to_lvalue(&lhs_var, frontend::BinaryOp::Assign, rhs);
        }
        return;
    }

    // General custom struct local variable
    if (decl->type.base == frontend::BaseType::CustomStruct && tu_) {
        if (const auto* s = tu_->find_struct(decl->type.struct_name)) {
            for (const auto& f : s->fields) {
                frontend::VarDeclStmt fdecl;
                fdecl.type = f.type;
                fdecl.name = decl->name + "." + f.name;
                fdecl.semantic = f.semantic;
                generate_var_decl(&fdecl);
            }
            SymbolBinding whole{};
            whole.name = decl->name;
            whole.type = decl->type;
            whole.reg_class = RegisterClass::Temporary;
            bind_scoped_symbol(decl->name, whole);
            if (decl->initializer) {
                ValueRef rhs = generate_expr(decl->initializer.get());
                frontend::VariableExpr lhs_var(decl->name);
                assign_value_to_lvalue(&lhs_var, frontend::BinaryOp::Assign, rhs);
            }
            return;
        }
    }

    // Local array declaration
    if (decl->type.array_size > 1) {
        uint8_t n_elems = static_cast<uint8_t>(std::min<uint32_t>(decl->type.array_size, 16u));
        uint8_t base = reg_allocator_.allocate_contiguous_temp_vec4s(n_elems);
        if (!scopes_.empty()) {
            for (uint8_t e = 0; e < n_elems; ++e) {
                scopes_.back().owned_vec4_temps.push_back(static_cast<uint8_t>(base + e * 4));
            }
        }
        SymbolBinding bind{};
        bind.name = decl->name;
        bind.type = decl->type;
        bind.reg_class = RegisterClass::Temporary;
        bind.base_register = base;
        bind.register_count = static_cast<uint16_t>(n_elems * decl->type.component_count());
        bind.allocated_words = static_cast<uint16_t>(n_elems * 4u);
        bind.semantic = decl->semantic;
        bind_scoped_symbol(decl->name, bind);

        if (auto* il = dynamic_cast<const frontend::InitListExpr*>(decl->initializer.get())) {
            uint8_t comp_n = static_cast<uint8_t>(std::clamp<size_t>(decl->type.component_count(), 1, 4));
            for (size_t e = 0; e < il->elements.size() && e < n_elems; ++e) {
                ValueRef ev = generate_expr(il->elements[e].get());
                emit_move(RegisterClass::Temporary, static_cast<uint16_t>(base + e * 4),
                          mask_for_count(comp_n), ev, decl->name + "[" + std::to_string(e) + "]");
            }
        }
        return;
    }

    // Matrix local variable
    if (decl->type.is_matrix()) {
        uint8_t rows = static_cast<uint8_t>(decl->type.matrix_rows());
        uint8_t cols = static_cast<uint8_t>(decl->type.matrix_cols());
        uint8_t base = reg_allocator_.allocate_contiguous_temp_vec4s(rows);
        if (!scopes_.empty()) {
            for (uint8_t r = 0; r < rows; ++r) {
                scopes_.back().owned_vec4_temps.push_back(static_cast<uint8_t>(base + r * 4));
            }
        }
        SymbolBinding bind{};
        bind.name = decl->name;
        bind.type = decl->type;
        bind.reg_class = RegisterClass::Temporary;
        bind.base_register = base;
        bind.register_count = static_cast<uint16_t>(rows * 4u);
        bind.allocated_words = static_cast<uint16_t>(rows * 4u);
        bind_scoped_symbol(decl->name, bind);

        if (decl->initializer) {
            ValueRef iv = generate_expr(decl->initializer.get());
            if (iv.is_matrix) {
                for (uint8_t r = 0; r < rows && r < iv.mat_rows; ++r) {
                    ValueRef rval;
                    rval.reg_class = iv.reg_class;
                    rval.base_index = static_cast<uint16_t>(iv.base_index + r * iv.row_stride);
                    rval.count = cols;
                    emit_move(RegisterClass::Temporary, static_cast<uint16_t>(base + r * 4),
                              mask_for_count(cols), rval, decl->name + " row " + std::to_string(r));
                }
            }
        }
        return;
    }

    // Scalar or Vector local variable
    uint8_t n = static_cast<uint8_t>(std::clamp<size_t>(decl->type.component_count(), 1, 4));
    uint8_t base = alloc_temp_vec4();
    SymbolBinding local_bind{};
    local_bind.name = decl->name;
    local_bind.type = decl->type;
    local_bind.reg_class = RegisterClass::Temporary;
    local_bind.base_register = base;
    local_bind.register_count = n;
    local_bind.allocated_words = 4;
    local_bind.semantic = decl->semantic;
    bind_scoped_symbol(decl->name, local_bind);

    if (decl->initializer) {
        ValueRef init_v = generate_expr(decl->initializer.get());
        emit_move(RegisterClass::Temporary, base, mask_for_count(n), init_v, "init " + decl->name);
    }
}

void UsseCodeGenerator::generate_assign(const frontend::AssignStmt* assign) {
    if (!assign || !assign->lvalue) return;

    if (!assign->rvalue) {
        generate_expr(assign->lvalue.get());
        return;
    }

    ValueRef rhs = generate_expr(assign->rvalue.get());
    assign_value_to_lvalue(assign->lvalue.get(), assign->compound_op, rhs);
}

void UsseCodeGenerator::generate_return(const frontend::ReturnStmt* ret) {
    if (!ret) return;

    if (inline_call_ctx_.active) {
        if (ret->value && inline_call_ctx_.return_slot.has_value()) {
            ValueRef rv = generate_expr(ret->value.get());
            if (!inline_call_ctx_.return_slot->struct_name.empty() && tu_) {
                if (const auto* s = tu_->find_struct(inline_call_ctx_.return_slot->struct_name)) {
                    for (const auto& f : s->fields) {
                        const auto* dst_sym = reg_allocator_.find_symbol(inline_call_ctx_.return_slot->struct_instance + "." + f.name);
                        const auto* src_sym = !rv.struct_instance.empty()
                            ? reg_allocator_.find_symbol(rv.struct_instance + "." + f.name)
                            : nullptr;
                        if (dst_sym && src_sym) {
                            emit_move(dst_sym->reg_class, dst_sym->base_register,
                                      mask_for_count(static_cast<uint8_t>(dst_sym->type.component_count())),
                                      binding_to_value(*src_sym), "helper struct return");
                        }
                    }
                }
            } else if (!inline_call_ctx_.return_slot->is_matrix) {
                emit_move(RegisterClass::Temporary, inline_call_ctx_.return_slot->base_index,
                          mask_for_count(inline_call_ctx_.return_slot->count), rv, "helper return");
            }
        }
        size_t br = emit_br(0, ExtPredicate::None, "helper return jump");
        inline_call_ctx_.return_patches.push_back(br);
        return;
    }

    if (!ret->value) {
        size_t br = emit_br(0, ExtPredicate::None, "early return");
        entry_return_patches_.push_back(br);
        return;
    }

    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(ret->value.get())) {
        const auto* sym = reg_allocator_.find_symbol(var->name);
        if (sym && sym->reg_class == RegisterClass::Output) {
            return;
        }
    }

    if (entry_fn_ && entry_fn_->return_type.base == frontend::BaseType::CustomStruct && tu_) {
        if (auto* var = dynamic_cast<const frontend::VariableExpr*>(ret->value.get())) {
            for (const auto& rf : return_struct_fields_) {
                if (const auto* src_sym = reg_allocator_.find_symbol(var->name + "." + rf.name)) {
                    emit_move(rf.reg_class, rf.base_register,
                              mask_for_count(static_cast<uint8_t>(rf.type.component_count())),
                              binding_to_value(*src_sym), "return struct." + rf.name);
                }
            }
            return;
        }
    }

    ValueRef res = generate_expr(ret->value.get());
    uint8_t base_out = 0;
    uint8_t out_comps = static_cast<uint8_t>(std::clamp<size_t>(
        entry_fn_ ? entry_fn_->return_type.component_count() : 4, 1, 4));
    if (!reg_allocator_.get_outputs().empty()) {
        base_out = static_cast<uint8_t>(reg_allocator_.get_outputs().back().base_register);
    }

    if (is_fragment_ && uses_depth_) {
        for (const auto& o : reg_allocator_.get_outputs()) {
            if (semantic_contains(o.semantic, "DEPTH")) {
                if (!depth_src_reg_) {
                    HardwareRegister dr;
                    dr.reg_class = RegisterClass::Temporary;
                    dr.index = reg_allocator_.allocate_temp_vec4();
                    depth_src_reg_ = dr;
                }
                emit_move(RegisterClass::Temporary, depth_src_reg_->index, 0x1, res, "return DEPTH");
                return;
            }
        }
    }

    emit_move(RegisterClass::Output, base_out, mask_for_count(out_comps), res, "Shader output o" + std::to_string(base_out));
}

void UsseCodeGenerator::generate_if(const frontend::IfStmt* if_stmt) {
    if (!if_stmt) return;
    emit_condition_to_p0(if_stmt->condition.get());
    size_t br_to_else = emit_br(0, ExtPredicate::NegP0, "if false -> else/end");

    if (if_stmt->then_branch) {
        generate_statement(if_stmt->then_branch.get());
    }

    if (if_stmt->else_branch) {
        size_t br_to_end = emit_br(0, ExtPredicate::None, "then -> end");
        patch_br(br_to_else, instructions_.size());
        generate_statement(if_stmt->else_branch.get());
        patch_br(br_to_end, instructions_.size());
    } else {
        patch_br(br_to_else, instructions_.size());
    }
}

void UsseCodeGenerator::generate_for(const frontend::ForStmt* for_stmt) {
    if (!for_stmt) return;
    push_scope();
    if (for_stmt->init) {
        generate_statement(for_stmt->init.get());
    }

    LoopContext loop;
    loop_stack_.push_back(loop);

    size_t head_idx = instructions_.size();
    std::optional<size_t> cond_br;
    if (for_stmt->condition) {
        emit_condition_to_p0(for_stmt->condition.get());
        cond_br = emit_br(0, ExtPredicate::NegP0, "for cond false -> end");
    }

    if (for_stmt->body) {
        generate_statement(for_stmt->body.get());
    }

    size_t incr_idx = instructions_.size();
    for (size_t br : loop_stack_.back().continue_patches) {
        patch_br(br, incr_idx);
    }

    if (for_stmt->increment) {
        generate_statement(for_stmt->increment.get());
    }

    emit_br(static_cast<int32_t>(head_idx) - static_cast<int32_t>(instructions_.size()), ExtPredicate::None, "for loop back");

    size_t end_idx = instructions_.size();
    if (cond_br.has_value()) {
        patch_br(*cond_br, end_idx);
    }
    for (size_t br : loop_stack_.back().break_patches) {
        patch_br(br, end_idx);
    }

    loop_stack_.pop_back();
    pop_scope();
}

void UsseCodeGenerator::generate_while(const frontend::WhileStmt* while_stmt) {
    if (!while_stmt) return;
    loop_stack_.emplace_back();

    size_t head_idx = instructions_.size();
    std::optional<size_t> cond_br;
    if (while_stmt->condition) {
        emit_condition_to_p0(while_stmt->condition.get());
        cond_br = emit_br(0, ExtPredicate::NegP0, "while false -> end");
    }

    if (while_stmt->body) {
        generate_statement(while_stmt->body.get());
    }

    for (size_t br : loop_stack_.back().continue_patches) {
        patch_br(br, head_idx);
    }

    emit_br(static_cast<int32_t>(head_idx) - static_cast<int32_t>(instructions_.size()), ExtPredicate::None, "while loop back");

    size_t end_idx = instructions_.size();
    if (cond_br.has_value()) {
        patch_br(*cond_br, end_idx);
    }
    for (size_t br : loop_stack_.back().break_patches) {
        patch_br(br, end_idx);
    }

    loop_stack_.pop_back();
}

void UsseCodeGenerator::generate_do_while(const frontend::DoWhileStmt* dw_stmt) {
    if (!dw_stmt) return;
    loop_stack_.emplace_back();

    size_t body_idx = instructions_.size();
    if (dw_stmt->body) {
        generate_statement(dw_stmt->body.get());
    }

    size_t cond_idx = instructions_.size();
    for (size_t br : loop_stack_.back().continue_patches) {
        patch_br(br, cond_idx);
    }

    if (dw_stmt->condition) {
        emit_condition_to_p0(dw_stmt->condition.get());
        emit_br(static_cast<int32_t>(body_idx) - static_cast<int32_t>(instructions_.size()), ExtPredicate::P0, "do-while loop");
    }

    size_t end_idx = instructions_.size();
    for (size_t br : loop_stack_.back().break_patches) {
        patch_br(br, end_idx);
    }

    loop_stack_.pop_back();
}

void UsseCodeGenerator::generate_discard(const frontend::DiscardStmt*) {
    uses_discard_ = true;
    UsseInstruction inst;
    inst.opcode = UsseOpcode::KILL;
    inst.kill_pred = KillPredicate::None;
    inst.comment = "discard / pixel kill";
    emit_inst(std::move(inst));
}

ValueRef UsseCodeGenerator::generate_expr(const frontend::Expression* expr) {
    if (!expr) return make_const_scalar(0.0f);

    if (auto* lit = dynamic_cast<const frontend::LiteralExpr*>(expr)) return generate_literal(lit);
    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(expr)) return generate_variable(var);
    if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(expr)) return generate_swizzle(swiz);
    if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(expr)) return generate_array_access(arr);
    if (auto* bin = dynamic_cast<const frontend::BinaryExpr*>(expr)) return generate_binary_op(bin);
    if (auto* un = dynamic_cast<const frontend::UnaryExpr*>(expr)) return generate_unary_op(un);
    if (auto* tern = dynamic_cast<const frontend::TernaryExpr*>(expr)) return generate_ternary(tern);
    if (auto* call = dynamic_cast<const frontend::CallExpr*>(expr)) return generate_call(call);
    if (auto* construct = dynamic_cast<const frontend::ConstructExpr*>(expr)) return generate_construct(construct);
    if (auto* il = dynamic_cast<const frontend::InitListExpr*>(expr)) return generate_init_list(il);

    return make_const_scalar(0.0f);
}

ValueRef UsseCodeGenerator::generate_literal(const frontend::LiteralExpr* lit) {
    if (std::holds_alternative<float>(lit->value)) {
        return make_const_scalar(std::get<float>(lit->value));
    }
    if (std::holds_alternative<int>(lit->value)) {
        return make_const_scalar(static_cast<float>(std::get<int>(lit->value)));
    }
    if (std::holds_alternative<bool>(lit->value)) {
        return make_const_scalar(std::get<bool>(lit->value) ? 1.0f : 0.0f);
    }
    return make_const_scalar(0.0f);
}

ValueRef UsseCodeGenerator::generate_variable(const frontend::VariableExpr* var) {
    const auto* sym = reg_allocator_.find_symbol(var->name);
    if (sym) {
        return binding_to_value(*sym);
    }
    return make_const_scalar(0.0f);
}

ValueRef UsseCodeGenerator::generate_swizzle(const frontend::SwizzleExpr* swiz) {
    // 1. Check struct field access (e.g., input.position, light.diffuse)
    if (auto* var = dynamic_cast<const frontend::VariableExpr*>(swiz->target.get())) {
        if (const auto* sym = reg_allocator_.find_symbol(var->name + "." + swiz->components)) {
            return binding_to_value(*sym);
        }
        const auto* base_sym = reg_allocator_.find_symbol(var->name);
        if (!base_sym || base_sym->type.base == frontend::BaseType::CustomStruct) {
            if (const auto* field_sym = reg_allocator_.find_symbol(swiz->components)) {
                return binding_to_value(*field_sym);
            }
        }
    }

    // 2. Vector component swizzle (e.g., v.xyz, col.rgb, pos.w, v.yx)
    ValueRef base = generate_expr(swiz->target.get());
    if (!is_swizzle_letters(swiz->components)) {
        return base;
    }

    uint8_t n = static_cast<uint8_t>(std::clamp<size_t>(swiz->components.size(), 1, 4));
    if (base.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < n; ++i) {
            uint8_t sub = swizzle_char_to_sub(swiz->components[i]);
            res[i] = base.const_vals[sub < base.count ? sub : base.count - 1];
        }
        ValueRef out = make_const_vec(res, n);
        out.is_negated = base.is_negated;
        out.is_absolute = base.is_absolute;
        return out;
    }

    ValueRef out = base;
    out.count = n;
    for (uint8_t i = 0; i < 4; ++i) {
        char c = (i < n) ? swiz->components[i] : swiz->components.back();
        uint8_t sub = swizzle_char_to_sub(c);
        uint8_t src_idx = (sub < base.count) ? sub : static_cast<uint8_t>(base.count - 1);
        out.swiz[i] = base.swiz[src_idx];
    }
    return out;
}

ValueRef UsseCodeGenerator::generate_array_access(const frontend::ArrayAccessExpr* arr) {
    ValueRef base = generate_expr(arr->target.get());
    uint16_t stride = base.is_matrix ? base.row_stride : base.elem_stride;

    int64_t idx = 0;
    if (eval_const_int(arr->index.get(), idx) && idx >= 0) {
        base.base_index = static_cast<uint16_t>(base.base_index + static_cast<uint16_t>(idx) * stride);
        base.is_array = false;
        base.is_matrix = false;
        return base;
    }

    // Dynamic array indexing via compare-and-select chain over known array/matrix elements
    ValueRef idx_val = generate_expr(arr->index.get());
    uint32_t limit = base.is_matrix ? base.mat_rows : std::min<uint32_t>(base.array_size, 16u);
    if (limit <= 1) {
        base.is_array = false;
        base.is_matrix = false;
        return base;
    }

    ValueRef out = make_temp_vec4(base.count);
    ValueRef elem0 = base;
    elem0.is_array = false;
    elem0.is_matrix = false;
    emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(base.count), elem0, "arr[0]");

    for (uint32_t k = 1; k < limit; ++k) {
        emit_vtst_p0(idx_val, make_const_scalar(static_cast<float>(k)),
                     VtstSignTest::NeOrEq, VtstZeroTest::IncludeZero, "arr idx == " + std::to_string(k));
        size_t br_skip = emit_br(0, ExtPredicate::NegP0, "skip arr elem");
        ValueRef ek = base;
        ek.base_index = static_cast<uint16_t>(base.base_index + static_cast<uint16_t>(k) * stride);
        ek.is_array = false;
        ek.is_matrix = false;
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(base.count), ek, "arr[" + std::to_string(k) + "]");
        patch_br(br_skip, instructions_.size());
    }
    return out;
}

ValueRef UsseCodeGenerator::generate_binary_op(const frontend::BinaryExpr* bin) {
    if (bin->op == frontend::BinaryOp::Assign) {
        ValueRef rhs = generate_expr(bin->right.get());
        assign_value_to_lvalue(bin->left.get(), frontend::BinaryOp::Assign, rhs);
        return rhs;
    }

    if (bin->op == frontend::BinaryOp::Equal || bin->op == frontend::BinaryOp::NotEqual ||
        bin->op == frontend::BinaryOp::Less || bin->op == frontend::BinaryOp::LessEqual ||
        bin->op == frontend::BinaryOp::Greater || bin->op == frontend::BinaryOp::GreaterEqual) {
        return emit_compare_to_float(bin->op, generate_expr(bin->left.get()), generate_expr(bin->right.get()));
    }

    if (bin->op == frontend::BinaryOp::LogicalAnd || bin->op == frontend::BinaryOp::LogicalOr) {
        ValueRef l = generate_expr(bin->left.get());
        ValueRef r = generate_expr(bin->right.get());
        if (l.is_const && r.is_const) {
            bool b = (bin->op == frontend::BinaryOp::LogicalAnd)
                   ? (l.const_vals[0] != 0.0f && r.const_vals[0] != 0.0f)
                   : (l.const_vals[0] != 0.0f || r.const_vals[0] != 0.0f);
            return make_const_scalar(b ? 1.0f : 0.0f);
        }
        // Normalize both to {0, 1}, then min for && or max for ||
        ValueRef bl = emit_compare_to_float(frontend::BinaryOp::NotEqual, l, make_const_scalar(0.0f));
        ValueRef br = emit_compare_to_float(frontend::BinaryOp::NotEqual, r, make_const_scalar(0.0f));
        ValueRef dst = make_temp_vec4(1);
        Vec4Swiz sw4 = Vec4Swiz::XYZW;
        HardwareRegister hw_s2 = to_hw_src2(br, sw4);
        uint16_t sw12 = kSwiz12XYZW;
        HardwareRegister hw_s1 = to_hw_src1(bl, sw12);
        UsseInstruction inst;
        inst.opcode = (bin->op == frontend::BinaryOp::LogicalAnd) ? UsseOpcode::VMIN : UsseOpcode::VMAX;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = static_cast<uint8_t>(dst.base_index);
        inst.src0 = hw_s1;
        inst.src1 = hw_s2;
        inst.write_mask = 0x1;
        inst.src1_swiz12 = sw12;
        inst.src2_swiz4 = sw4;
        emit_inst(std::move(inst));
        dst.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return dst;
    }

    ValueRef lhs = generate_expr(bin->left.get());
    ValueRef rhs = generate_expr(bin->right.get());

    if (bin->op == frontend::BinaryOp::Mul && (lhs.is_matrix || rhs.is_matrix)) {
        return emit_matrix_mul(lhs, rhs);
    }

    return emit_binary_alu(bin->op, lhs, rhs);
}

ValueRef UsseCodeGenerator::generate_unary_op(const frontend::UnaryExpr* un) {
    if (un->op == frontend::UnaryOp::Incr || un->op == frontend::UnaryOp::Decr) {
        ValueRef cur = generate_expr(un->operand.get());
        ValueRef delta = make_const_scalar(1.0f);
        ValueRef updated = emit_binary_alu(
            (un->op == frontend::UnaryOp::Incr) ? frontend::BinaryOp::Add : frontend::BinaryOp::Sub,
            cur, delta, (un->op == frontend::UnaryOp::Incr) ? "++" : "--"
        );
        if (un->is_postfix) {
            ValueRef prev_copy = materialize_to_temp(cur, cur.count, "postfix save");
            assign_value_to_lvalue(un->operand.get(), frontend::BinaryOp::Assign, updated);
            return prev_copy;
        }
        assign_value_to_lvalue(un->operand.get(), frontend::BinaryOp::Assign, updated);
        return updated;
    }

    ValueRef opnd = generate_expr(un->operand.get());
    if (un->op == frontend::UnaryOp::Negate) {
        if (opnd.is_const) {
            opnd.is_negated = !opnd.is_negated;
            return opnd;
        }
        opnd.is_negated = !opnd.is_negated;
        return opnd;
    }
    if (un->op == frontend::UnaryOp::LogicalNot) {
        return emit_compare_to_float(frontend::BinaryOp::Equal, opnd, make_const_scalar(0.0f));
    }
    return opnd;
}

ValueRef UsseCodeGenerator::generate_ternary(const frontend::TernaryExpr* tern) {
    emit_condition_to_p0(tern->condition.get());
    size_t br_else = emit_br(0, ExtPredicate::NegP0, "ternary false");

    ValueRef dst = make_temp_vec4(4);
    ValueRef t_val = generate_expr(tern->then_expr.get());
    emit_move(RegisterClass::Temporary, dst.base_index, mask_for_count(t_val.count), t_val, "ternary then");
    size_t br_end = emit_br(0, ExtPredicate::None, "ternary end");

    patch_br(br_else, instructions_.size());
    ValueRef e_val = generate_expr(tern->else_expr.get());
    emit_move(RegisterClass::Temporary, dst.base_index, mask_for_count(e_val.count), e_val, "ternary else");

    patch_br(br_end, instructions_.size());
    dst.count = std::max(t_val.count, e_val.count);
    return dst;
}

ValueRef UsseCodeGenerator::generate_construct(const frontend::ConstructExpr* construct) {
    const auto& dt = construct->target_type;

    // Matrix cast / constructor
    if (dt.is_matrix()) {
        uint8_t rows = static_cast<uint8_t>(dt.matrix_rows());
        uint8_t cols = static_cast<uint8_t>(dt.matrix_cols());
        if (construct->arguments.size() == 1) {
            ValueRef src = generate_expr(construct->arguments[0].get());
            if (src.is_matrix) {
                src.mat_rows = std::min(src.mat_rows, rows);
                src.mat_cols = std::min(src.mat_cols, cols);
                src.count = src.mat_cols;
                return src;
            }
        }
        uint8_t base = reg_allocator_.allocate_contiguous_temp_vec4s(rows);
        if (!scopes_.empty()) {
            for (uint8_t r = 0; r < rows; ++r) {
                scopes_.back().owned_vec4_temps.push_back(static_cast<uint8_t>(base + r * 4));
            }
        }
        std::vector<ValueRef> args;
        args.reserve(construct->arguments.size());
        for (const auto& a : construct->arguments) args.push_back(generate_expr(a.get()));

        if (args.size() == rows) {
            for (uint8_t r = 0; r < rows; ++r) {
                emit_move(RegisterClass::Temporary, static_cast<uint16_t>(base + r * 4),
                          mask_for_count(cols), args[r], "mat row ctor");
            }
        } else {
            // Identity / scalar fill fallback
            for (uint8_t r = 0; r < rows; ++r) {
                std::array<float, 4> rvals = {0, 0, 0, 0};
                if (!args.empty() && args[0].is_const) {
                    rvals[r] = args[0].const_vals[0];
                } else {
                    rvals[r] = 1.0f;
                }
                emit_move(RegisterClass::Temporary, static_cast<uint16_t>(base + r * 4),
                          mask_for_count(cols), make_const_vec(rvals, cols), "mat identity row");
            }
        }
        ValueRef m;
        m.reg_class = RegisterClass::Temporary;
        m.base_index = base;
        m.count = cols;
        m.is_matrix = true;
        m.mat_rows = rows;
        m.mat_cols = cols;
        m.row_stride = 4;
        return m;
    }

    uint8_t target_n = static_cast<uint8_t>(std::clamp<size_t>(dt.component_count(), 1, 4));
    if (construct->arguments.empty()) {
        return make_const_vec({0, 0, 0, 0}, target_n);
    }

    // Evaluate arguments and flatten scalar/vector components
    std::vector<ValueRef> arg_vals;
    arg_vals.reserve(construct->arguments.size());
    bool all_const = true;
    for (const auto& a : construct->arguments) {
        arg_vals.push_back(generate_expr(a.get()));
        if (!arg_vals.back().is_const) all_const = false;
    }

    if (all_const) {
        std::array<float, 4> res = {0, 0, 0, 0};
        if (arg_vals.size() == 1 && arg_vals[0].count == 1) {
            float v = arg_vals[0].const_vals[0];
            if (arg_vals[0].is_absolute) v = std::fabs(v);
            if (arg_vals[0].is_negated) v = -v;
            res = {v, v, v, v};
        } else {
            uint8_t cursor = 0;
            for (const auto& av : arg_vals) {
                for (uint8_t c = 0; c < av.count && cursor < target_n; ++c) {
                    float v = av.const_vals[c];
                    if (av.is_absolute) v = std::fabs(v);
                    if (av.is_negated) v = -v;
                    res[cursor++] = v;
                }
            }
        }
        return make_const_vec(res, target_n);
    }

    // Single argument splat or truncation
    if (arg_vals.size() == 1) {
        const auto& a = arg_vals[0];
        if (a.count == 1 && target_n > 1) {
            ValueRef out = a;
            out.count = target_n;
            out.swiz = {a.swiz[0], a.swiz[0], a.swiz[0], a.swiz[0]};
            return out;
        }
        if (a.count >= target_n) {
            ValueRef out = a;
            out.count = target_n;
            return out;
        }
    }

    // Pack flattened components into one temp vec4 (each arg uses at most 1 move!)
    ValueRef out = make_temp_vec4(target_n);
    uint8_t cursor = 0;
    for (const auto& av : arg_vals) {
        if (cursor >= target_n) break;
        uint8_t take = static_cast<uint8_t>(std::min<uint8_t>(av.count, target_n - cursor));
        uint8_t mask = 0;
        std::array<uint8_t, 4> map = {0, 0, 0, 0};
        for (uint8_t c = 0; c < take; ++c) {
            uint8_t dl = static_cast<uint8_t>(cursor + c);
            mask |= static_cast<uint8_t>(1u << dl);
            map[dl] = c;
        }
        emit_move_mapped(RegisterClass::Temporary, out.base_index, mask, map, av, "construct vec");
        cursor = static_cast<uint8_t>(cursor + take);
    }
    return out;
}

ValueRef UsseCodeGenerator::generate_init_list(const frontend::InitListExpr* init_list, const frontend::DataType*) {
    if (!init_list || init_list->elements.empty()) {
        return make_const_scalar(0.0f);
    }
    return generate_expr(init_list->elements[0].get());
}

ValueRef UsseCodeGenerator::invoke_user_function(const frontend::FunctionDecl* fn, const frontend::CallExpr* call) {
    if (!fn || !fn->body || inline_depth_ >= 8) {
        return make_const_scalar(0.0f);
    }

    // Evaluate caller arguments before pushing helper scope
    std::vector<ValueRef> evaluated_args;
    evaluated_args.reserve(call->arguments.size());
    for (const auto& a : call->arguments) {
        evaluated_args.push_back(generate_expr(a.get()));
    }

    const frontend::FunctionDecl* saved_active_fn = active_fn_;
    FunctionCallContext saved_ctx = std::move(inline_call_ctx_);
    inline_call_ctx_ = {};
    inline_call_ctx_.active = true;
    active_fn_ = fn;
    inline_depth_++;

    // Allocate return slot in caller's scope so it remains valid after helper scope pops
    ValueRef ret_val;
    if (fn->return_type.base == frontend::BaseType::CustomStruct && tu_) {
        std::string ret_inst = "__ret_" + std::to_string(inline_depth_);
        if (const auto* s = tu_->find_struct(fn->return_type.struct_name)) {
            for (const auto& f : s->fields) {
                frontend::VarDeclStmt fdecl;
                fdecl.type = f.type;
                fdecl.name = ret_inst + "." + f.name;
                generate_var_decl(&fdecl);
            }
            SymbolBinding ret_whole{};
            ret_whole.name = ret_inst;
            ret_whole.type = fn->return_type;
            ret_whole.reg_class = RegisterClass::Temporary;
            bind_scoped_symbol(ret_inst, ret_whole);
        }
        ret_val.struct_name = fn->return_type.struct_name;
        ret_val.struct_instance = ret_inst;
        ret_val.count = 4;
        inline_call_ctx_.return_slot = ret_val;
    } else if (fn->return_type.base != frontend::BaseType::Void) {
        uint8_t rc = static_cast<uint8_t>(std::clamp<size_t>(fn->return_type.component_count(), 1, 4));
        ret_val = make_temp_vec4(rc);
        inline_call_ctx_.return_slot = ret_val;
    }

    push_scope();
    for (size_t i = 0; i < fn->parameters.size() && i < evaluated_args.size(); ++i) {
        const auto& param = fn->parameters[i];
        if (param.type.base == frontend::BaseType::CustomStruct && tu_) {
            if (const auto* s = tu_->find_struct(param.type.struct_name)) {
                for (const auto& f : s->fields) {
                    frontend::VarDeclStmt fdecl;
                    fdecl.type = f.type;
                    fdecl.name = param.name + "." + f.name;
                    generate_var_decl(&fdecl);
                }
                SymbolBinding pbind{};
                pbind.name = param.name;
                pbind.type = param.type;
                pbind.reg_class = RegisterClass::Temporary;
                bind_scoped_symbol(param.name, pbind);
                if (param.is_in) {
                    frontend::VariableExpr pvar(param.name);
                    assign_value_to_lvalue(&pvar, frontend::BinaryOp::Assign, evaluated_args[i]);
                }
            }
            continue;
        }
        uint8_t pc = static_cast<uint8_t>(std::clamp<size_t>(param.type.component_count(), 1, 4));
        uint8_t pbase = alloc_temp_vec4();
        SymbolBinding pbind{};
        pbind.name = param.name;
        pbind.type = param.type;
        pbind.reg_class = RegisterClass::Temporary;
        pbind.base_register = pbase;
        pbind.register_count = pc;
        pbind.allocated_words = 4;
        bind_scoped_symbol(param.name, pbind);
        if (param.is_in) {
            emit_move(RegisterClass::Temporary, pbase, mask_for_count(pc), evaluated_args[i], "arg " + param.name);
        }
    }

    generate_block(fn->body.get());

    size_t epilogue_idx = instructions_.size();
    for (size_t br : inline_call_ctx_.return_patches) {
        patch_br(br, epilogue_idx);
    }

    // Capture out/inout parameter register values while helper scope symbols are active,
    // then restore caller symbol bindings BEFORE writing back to caller lvalues,
    // then release helper temporary registers.
    struct PendingOutScalar {
        size_t arg_index;
        ValueRef val;
    };
    struct PendingOutStructField {
        size_t arg_index;
        std::string field_name;
        ValueRef val;
    };
    std::vector<PendingOutScalar> out_scalars;
    std::vector<PendingOutStructField> out_struct_fields;

    for (size_t i = 0; i < fn->parameters.size() && i < call->arguments.size(); ++i) {
        if (!fn->parameters[i].is_out) continue;
        const auto& param = fn->parameters[i];
        if (param.type.base == frontend::BaseType::CustomStruct && tu_) {
            if (const auto* s = tu_->find_struct(param.type.struct_name)) {
                for (const auto& f : s->fields) {
                    if (const auto* fsym = reg_allocator_.find_symbol(param.name + "." + f.name)) {
                        out_struct_fields.push_back({i, f.name, binding_to_value(*fsym)});
                    }
                }
            }
        } else if (const auto* sym = reg_allocator_.find_symbol(param.name)) {
            out_scalars.push_back({i, binding_to_value(*sym)});
        }
    }

    // Restore caller's symbol table mappings while keeping helper temps alive for copyback
    if (!scopes_.empty()) {
        auto& shadowed = scopes_.back().shadowed_symbols;
        for (auto it = shadowed.rbegin(); it != shadowed.rend(); ++it) {
            if (it->second.has_value()) {
                reg_allocator_.alias_symbol(it->first, *it->second);
            } else {
                reg_allocator_.erase_symbol(it->first);
            }
        }
        shadowed.clear();
    }

    for (const auto& os : out_scalars) {
        assign_value_to_lvalue(call->arguments[os.arg_index].get(), frontend::BinaryOp::Assign, os.val);
    }
    if (!out_struct_fields.empty()) {
        for (const auto& osf : out_struct_fields) {
            RegisterClass dc{}; uint16_t db = 0; uint8_t dm = 0xF;
            std::array<uint8_t, 4> map{}; ValueRef cv; std::string lbl;
            if (resolve_lvalue(call->arguments[osf.arg_index].get(), dc, db, dm, map, cv, lbl)) {
                const auto* dst_sym = reg_allocator_.find_symbol(lbl + "." + osf.field_name);
                if (!dst_sym) dst_sym = reg_allocator_.find_symbol(osf.field_name);
                if (dst_sym) {
                    emit_move(dst_sym->reg_class, dst_sym->base_register,
                              mask_for_count(static_cast<uint8_t>(dst_sym->type.component_count())),
                              osf.val, lbl + "." + osf.field_name);
                }
            }
        }
    }

    pop_scope();

    inline_depth_--;
    active_fn_ = saved_active_fn;
    inline_call_ctx_ = std::move(saved_ctx);
    return ret_val;
}

ValueRef UsseCodeGenerator::generate_call(const frontend::CallExpr* call) {
    const std::string& fn = call->callee;

    // 1. Texture Sampling (tex1D / tex2D / tex2Dbias / tex2Dlod / tex2Dproj / tex3D / texCUBE)
    if (fn == "tex2D" || fn == "tex2Dbias" || fn == "tex2Dlod" ||
        fn == "tex2Dproj" || fn == "tex1D" || fn == "tex3D" || fn == "texCUBE") {
        if (call->arguments.size() >= 2) {
            uint16_t samp_unit = 0;
            if (auto* var = dynamic_cast<const frontend::VariableExpr*>(call->arguments[0].get())) {
                if (const auto* sym = reg_allocator_.find_symbol(var->name)) {
                    samp_unit = sym->base_register;
                }
            } else if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(call->arguments[0].get())) {
                int64_t idx = 0;
                if (auto* base_var = dynamic_cast<const frontend::VariableExpr*>(arr->target.get())) {
                    if (const auto* sym = reg_allocator_.find_symbol(base_var->name)) {
                        eval_const_int(arr->index.get(), idx);
                        samp_unit = static_cast<uint16_t>(sym->base_register + std::max<int64_t>(0, idx));
                    }
                }
            }

            ValueRef uv = generate_expr(call->arguments[1].get());
            uint8_t dim = 1; // 2D
            if (fn == "tex1D") dim = 0;
            else if (fn == "tex3D" || fn == "texCUBE") dim = 2;

            uint8_t lod_mode = 0;
            ValueRef lod_val;
            if (fn == "tex2Dbias") {
                lod_mode = 1;
                lod_val = uv;
                lod_val.swiz = {uv.swiz[3], uv.swiz[3], uv.swiz[3], uv.swiz[3]};
                lod_val.count = 1;
                lod_val = materialize_to_temp(lod_val, 1, "tex bias");
            } else if (fn == "tex2Dlod") {
                lod_mode = 2;
                lod_val = uv;
                lod_val.swiz = {uv.swiz[3], uv.swiz[3], uv.swiz[3], uv.swiz[3]};
                lod_val.count = 1;
                lod_val = materialize_to_temp(lod_val, 1, "tex lod");
            } else if (fn == "tex2Dproj") {
                ValueRef w_sc = uv;
                w_sc.count = 1;
                w_sc.swiz = {uv.swiz[3], uv.swiz[3], uv.swiz[3], uv.swiz[3]};
                ValueRef w_rcp = emit_vcomp_vec(UsseOpcode::VRCP, w_sc, "tex2Dproj 1/w");
                ValueRef uv_xyz = uv;
                uv_xyz.count = 3;
                uv = emit_binary_alu(frontend::BinaryOp::Mul, uv_xyz, w_rcp, "tex2Dproj uv/w");
            }

            // Ensure UV is in an even-aligned TEMP or PRIMATTR with natural xyzw swizzle
            bool uv_direct = !uv.is_const && !uv.is_negated && !uv.is_absolute &&
                             (uv.base_index & 1u) == 0 &&
                             (uv.reg_class == RegisterClass::Temporary || uv.reg_class == RegisterClass::InputAttr) &&
                             uv.swiz[0] == SwizzleChannel::X &&
                             (uv.count < 2 || uv.swiz[1] == SwizzleChannel::Y) &&
                             (uv.count < 3 || uv.swiz[2] == SwizzleChannel::Z);
            if (!uv_direct) {
                uv = materialize_to_temp(uv, std::max<uint8_t>(2, uv.count), "SMP uv");
            }

            ValueRef dst = make_temp_vec4(4);
            UsseInstruction inst;
            inst.opcode = UsseOpcode::SMP;
            inst.dst.reg_class = RegisterClass::Temporary;
            inst.dst.index = static_cast<uint8_t>(dst.base_index);
            inst.src0.reg_class = RegisterClass::TextureSampler;
            inst.src0.index = static_cast<uint8_t>(samp_unit & 0xFFu);
            inst.src1.reg_class = uv.reg_class;
            inst.src1.index = static_cast<uint8_t>(uv.base_index & 0xFFu);
            inst.sampler_sa_offset = reg_allocator_.get_sampler_sa_offset(samp_unit);
            inst.smp_dim = dim;
            inst.smp_lod_mode = lod_mode;
            if (lod_mode != 0) {
                inst.src2.reg_class = lod_val.reg_class;
                inst.src2.index = static_cast<uint8_t>(lod_val.base_index & 0xFFu);
            }
            inst.write_mask = 0xF;
            inst.comment = "Texture sample: " + fn;
            emit_inst(std::move(inst));
            return dst;
        }
    }

    // 2. Matrix / Vector Multiply
    if (fn == "mul" && call->arguments.size() == 2) {
        return emit_matrix_mul(generate_expr(call->arguments[0].get()),
                               generate_expr(call->arguments[1].get()));
    }

    // 3. Dot products & vector geometry
    if ((fn == "dot" || fn == "dp4" || fn == "dp3") && call->arguments.size() >= 2) {
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef b = generate_expr(call->arguments[1].get());
        uint8_t c = (fn == "dp4") ? 4 : ((fn == "dp3") ? 3 : std::max(a.count, b.count));
        return emit_dp(a, b, c, fn);
    }

    if (fn == "cross" && call->arguments.size() >= 2) {
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef b = generate_expr(call->arguments[1].get());
        ValueRef a_yzx = a; a_yzx.count = 3; a_yzx.swiz = {a.swiz[1], a.swiz[2], a.swiz[0], a.swiz[0]};
        ValueRef b_zxy = b; b_zxy.count = 3; b_zxy.swiz = {b.swiz[2], b.swiz[0], b.swiz[1], b.swiz[0]};
        ValueRef a_zxy = a; a_zxy.count = 3; a_zxy.swiz = {a.swiz[2], a.swiz[0], a.swiz[1], a.swiz[0]};
        ValueRef b_yzx = b; b_yzx.count = 3; b_yzx.swiz = {b.swiz[1], b.swiz[2], b.swiz[0], b.swiz[0]};
        ValueRef t0 = emit_binary_alu(frontend::BinaryOp::Mul, a_yzx, b_zxy, "cross L");
        ValueRef t1 = emit_binary_alu(frontend::BinaryOp::Mul, a_zxy, b_yzx, "cross R");
        return emit_binary_alu(frontend::BinaryOp::Sub, t0, t1, "cross");
    }

    if (fn == "length" && !call->arguments.empty()) {
        ValueRef v = generate_expr(call->arguments[0].get());
        ValueRef dp = emit_dp(v, v, v.count, "length dot");
        return emit_sqrt(dp);
    }

    if (fn == "distance" && call->arguments.size() >= 2) {
        ValueRef diff = emit_binary_alu(frontend::BinaryOp::Sub,
                                        generate_expr(call->arguments[0].get()),
                                        generate_expr(call->arguments[1].get()), "distance diff");
        return emit_sqrt(emit_dp(diff, diff, diff.count, "distance dot"));
    }

    if (fn == "normalize" && !call->arguments.empty()) {
        ValueRef v = generate_expr(call->arguments[0].get());
        ValueRef dp = emit_dp(v, v, v.count, "normalize dot");
        ValueRef rsq = emit_vcomp_vec(UsseOpcode::VRSQ, dp, "normalize rsqrt");
        return emit_binary_alu(frontend::BinaryOp::Mul, v, rsq, "normalize scale");
    }

    if (fn == "reflect" && call->arguments.size() >= 2) {
        ValueRef i_vec = generate_expr(call->arguments[0].get());
        ValueRef n_vec = generate_expr(call->arguments[1].get());
        ValueRef dp = emit_dp(n_vec, i_vec, std::max(i_vec.count, n_vec.count), "reflect dot");
        ValueRef k = emit_binary_alu(frontend::BinaryOp::Mul, dp, make_const_scalar(2.0f), "2*dot");
        ValueRef term = emit_binary_alu(frontend::BinaryOp::Mul, n_vec, k, "2*dot*N");
        return emit_binary_alu(frontend::BinaryOp::Sub, i_vec, term, "reflect");
    }

    if (fn == "refract" && call->arguments.size() >= 3) {
        ValueRef i_vec = generate_expr(call->arguments[0].get());
        ValueRef n_vec = generate_expr(call->arguments[1].get());
        ValueRef eta = generate_expr(call->arguments[2].get());
        ValueRef ndoti = emit_dp(n_vec, i_vec, std::max(i_vec.count, n_vec.count), "refract N.I");
        ValueRef one = make_const_scalar(1.0f);
        ValueRef k = emit_binary_alu(
            frontend::BinaryOp::Sub,
            one,
            emit_binary_alu(
                frontend::BinaryOp::Mul,
                emit_binary_alu(frontend::BinaryOp::Mul, eta, eta),
                emit_binary_alu(frontend::BinaryOp::Sub, one, emit_binary_alu(frontend::BinaryOp::Mul, ndoti, ndoti))
            ),
            "refract k"
        );
        emit_vtst_p0(k, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "refract TIR");
        ValueRef out = make_temp_vec4(i_vec.count);
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(i_vec.count), make_const_scalar(0.0f), "refract 0");
        size_t br_tir = emit_br(0, ExtPredicate::P0, "refract k<0");
        ValueRef term1 = emit_binary_alu(frontend::BinaryOp::Mul, eta, i_vec, "eta*I");
        ValueRef factor = emit_binary_alu(frontend::BinaryOp::Add,
                                          emit_binary_alu(frontend::BinaryOp::Mul, eta, ndoti),
                                          emit_sqrt(k), "refract factor");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Sub, term1,
                                       emit_binary_alu(frontend::BinaryOp::Mul, factor, n_vec), "refract res");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(i_vec.count), res);
        patch_br(br_tir, instructions_.size());
        return out;
    }

    if (fn == "faceforward" && call->arguments.size() >= 3) {
        ValueRef n = generate_expr(call->arguments[0].get());
        ValueRef i = generate_expr(call->arguments[1].get());
        ValueRef ng = generate_expr(call->arguments[2].get());
        ValueRef dp = emit_dp(ng, i, std::max(ng.count, i.count), "faceforward dot");
        emit_vtst_p0(dp, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "ng.i < 0");
        ValueRef out = make_temp_vec4(n.count);
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(n.count), n);
        size_t br_neg = emit_br(0, ExtPredicate::P0, "faceforward keep");
        ValueRef neg_n = n;
        neg_n.is_negated = !neg_n.is_negated;
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(n.count), neg_n);
        patch_br(br_neg, instructions_.size());
        return out;
    }

    // 4. Componentwise min/max/clamp/saturate
    if ((fn == "min" || fn == "max") && call->arguments.size() >= 2) {
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef b = generate_expr(call->arguments[1].get());
        uint8_t n = std::max(a.count, b.count);
        if (a.is_const && b.is_const) {
            std::array<float, 4> res{};
            for (uint8_t i = 0; i < n; ++i) {
                float av = a.const_vals[i < a.count ? i : a.count - 1];
                float bv = b.const_vals[i < b.count ? i : b.count - 1];
                res[i] = (fn == "min") ? std::min(av, bv) : std::max(av, bv);
            }
            return make_const_vec(res, n);
        }
        if (a.count == 1 && n > 1) { a.swiz = {a.swiz[0], a.swiz[0], a.swiz[0], a.swiz[0]}; a.count = n; }
        if (b.count == 1 && n > 1) { b.swiz = {b.swiz[0], b.swiz[0], b.swiz[0], b.swiz[0]}; b.count = n; }
        if (b.is_const) b = materialize_to_temp(b, n, fn + " const");

        ValueRef dst = make_temp_vec4(n);
        Vec4Swiz sw4 = Vec4Swiz::XYZW;
        HardwareRegister hw_s2 = to_hw_src2(b, sw4);
        uint16_t sw12 = kSwiz12XYZW;
        HardwareRegister hw_s1 = to_hw_src1(a, sw12);
        UsseInstruction inst;
        inst.opcode = (fn == "min") ? UsseOpcode::VMIN : UsseOpcode::VMAX;
        inst.dst.reg_class = RegisterClass::Temporary;
        inst.dst.index = static_cast<uint8_t>(dst.base_index);
        inst.src0 = hw_s1;
        inst.src1 = hw_s2;
        inst.write_mask = mask_for_count(n);
        inst.src1_swiz12 = sw12;
        inst.src2_swiz4 = sw4;
        inst.comment = fn;
        emit_inst(std::move(inst));
        return dst;
    }

    if (fn == "clamp" && call->arguments.size() >= 3) {
        ValueRef x = generate_expr(call->arguments[0].get());
        ValueRef lo = generate_expr(call->arguments[1].get());
        ValueRef hi = generate_expr(call->arguments[2].get());
        uint8_t n = std::max({x.count, lo.count, hi.count});
        if (x.count == 1 && n > 1) { x.swiz = {x.swiz[0], x.swiz[0], x.swiz[0], x.swiz[0]}; x.count = n; }
        if (lo.count == 1 && n > 1) { lo.swiz = {lo.swiz[0], lo.swiz[0], lo.swiz[0], lo.swiz[0]}; lo.count = n; }
        if (hi.count == 1 && n > 1) { hi.swiz = {hi.swiz[0], hi.swiz[0], hi.swiz[0], hi.swiz[0]}; hi.count = n; }
        if (lo.is_const) lo = materialize_to_temp(lo, n, "clamp lo");
        if (hi.is_const) hi = materialize_to_temp(hi, n, "clamp hi");

        ValueRef t = make_temp_vec4(n);
        Vec4Swiz sw4_lo = Vec4Swiz::XYZW;
        HardwareRegister hw_lo = to_hw_src2(lo, sw4_lo);
        uint16_t sw12_x = kSwiz12XYZW;
        HardwareRegister hw_x = to_hw_src1(x, sw12_x);
        UsseInstruction imax;
        imax.opcode = UsseOpcode::VMAX;
        imax.dst.reg_class = RegisterClass::Temporary;
        imax.dst.index = static_cast<uint8_t>(t.base_index);
        imax.src0 = hw_x;
        imax.src1 = hw_lo;
        imax.write_mask = mask_for_count(n);
        imax.src1_swiz12 = sw12_x;
        imax.src2_swiz4 = sw4_lo;
        imax.comment = "clamp min";
        emit_inst(std::move(imax));

        ValueRef dst = make_temp_vec4(n);
        Vec4Swiz sw4_hi = Vec4Swiz::XYZW;
        HardwareRegister hw_hi = to_hw_src2(hi, sw4_hi);
        uint16_t sw12_t = kSwiz12XYZW;
        HardwareRegister hw_t = to_hw_src1(t, sw12_t);
        UsseInstruction imin;
        imin.opcode = UsseOpcode::VMIN;
        imin.dst.reg_class = RegisterClass::Temporary;
        imin.dst.index = static_cast<uint8_t>(dst.base_index);
        imin.src0 = hw_t;
        imin.src1 = hw_hi;
        imin.write_mask = mask_for_count(n);
        imin.src1_swiz12 = sw12_t;
        imin.src2_swiz4 = sw4_hi;
        imin.comment = "clamp max";
        emit_inst(std::move(imin));
        return dst;
    }

    if (fn == "saturate" && !call->arguments.empty()) {
        ValueRef x = generate_expr(call->arguments[0].get());
        if (x.is_const) {
            std::array<float, 4> res{};
            for (uint8_t i = 0; i < x.count; ++i) res[i] = std::clamp(x.const_vals[i], 0.0f, 1.0f);
            return make_const_vec(res, x.count);
        }
        uint8_t n = x.count;
        ValueRef zero_v = materialize_to_temp(make_const_scalar(0.0f), n, "0.0");
        ValueRef one_v = materialize_to_temp(make_const_scalar(1.0f), n, "1.0");
        ValueRef t = make_temp_vec4(n);
        Vec4Swiz sw4_0 = Vec4Swiz::XXXX;
        HardwareRegister hw_0 = to_hw_src2(zero_v, sw4_0);
        uint16_t sw12_x = kSwiz12XYZW;
        HardwareRegister hw_x = to_hw_src1(x, sw12_x);
        UsseInstruction imax;
        imax.opcode = UsseOpcode::VMAX;
        imax.dst.reg_class = RegisterClass::Temporary;
        imax.dst.index = static_cast<uint8_t>(t.base_index);
        imax.src0 = hw_x;
        imax.src1 = hw_0;
        imax.write_mask = mask_for_count(n);
        imax.src1_swiz12 = sw12_x;
        imax.src2_swiz4 = sw4_0;
        imax.comment = "saturate floor 0";
        emit_inst(std::move(imax));

        ValueRef dst = make_temp_vec4(n);
        Vec4Swiz sw4_1 = Vec4Swiz::XYZW;
        HardwareRegister hw_1 = to_hw_src2(one_v, sw4_1);
        uint16_t sw12_t = kSwiz12XYZW;
        HardwareRegister hw_t = to_hw_src1(t, sw12_t);
        UsseInstruction imin;
        imin.opcode = UsseOpcode::VMIN;
        imin.dst.reg_class = RegisterClass::Temporary;
        imin.dst.index = static_cast<uint8_t>(dst.base_index);
        imin.src0 = hw_t;
        imin.src1 = hw_1;
        imin.write_mask = mask_for_count(n);
        imin.src1_swiz12 = sw12_t;
        imin.src2_swiz4 = sw4_1;
        imin.comment = "saturate ceil 1";
        emit_inst(std::move(imin));
        return dst;
    }

    // 5. Interpolation / Shaping / Unary Math
    if ((fn == "lerp" || fn == "mix") && call->arguments.size() >= 3) {
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef b = generate_expr(call->arguments[1].get());
        ValueRef t = generate_expr(call->arguments[2].get());
        ValueRef diff = emit_binary_alu(frontend::BinaryOp::Sub, b, a, "lerp (b - a)");
        ValueRef scaled = emit_binary_alu(frontend::BinaryOp::Mul, diff, t, "lerp (b - a) * t");
        return emit_binary_alu(frontend::BinaryOp::Add, a, scaled, "lerp a + ...");
    }

    if (fn == "step" && call->arguments.size() >= 2) {
        ValueRef edge = generate_expr(call->arguments[0].get());
        ValueRef x = generate_expr(call->arguments[1].get());
        uint8_t n = std::max(edge.count, x.count);
        if (n == 1) {
            return emit_compare_to_float(frontend::BinaryOp::GreaterEqual, x, edge);
        }
        ValueRef out = make_temp_vec4(n);
        for (uint8_t c = 0; c < n; ++c) {
            ValueRef ec = edge; ec.count = 1;
            if (edge.count > 1) ec.swiz = {edge.swiz[c], edge.swiz[c], edge.swiz[c], edge.swiz[c]};
            ValueRef xc = x; xc.count = 1;
            if (x.count > 1) xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
            ValueRef sc = emit_compare_to_float(frontend::BinaryOp::GreaterEqual, xc, ec);
            std::array<uint8_t, 4> map = {0, 0, 0, 0};
            emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, sc, "step");
        }
        return out;
    }

    if (fn == "smoothstep" && call->arguments.size() >= 3) {
        ValueRef e0 = generate_expr(call->arguments[0].get());
        ValueRef e1 = generate_expr(call->arguments[1].get());
        ValueRef x = generate_expr(call->arguments[2].get());
        ValueRef t = emit_binary_alu(frontend::BinaryOp::Div,
                                     emit_binary_alu(frontend::BinaryOp::Sub, x, e0, "smoothstep num"),
                                     emit_binary_alu(frontend::BinaryOp::Sub, e1, e0, "smoothstep den"),
                                     "smoothstep t");
        // saturate(t)
        ValueRef zero_v = materialize_to_temp(make_const_scalar(0.0f), t.count);
        ValueRef one_v = materialize_to_temp(make_const_scalar(1.0f), t.count);
        ValueRef t0 = make_temp_vec4(t.count);
        Vec4Swiz sw4_0 = Vec4Swiz::XXXX; HardwareRegister hw_0 = to_hw_src2(zero_v, sw4_0);
        uint16_t sw12_t = kSwiz12XYZW; HardwareRegister hw_t = to_hw_src1(t, sw12_t);
        UsseInstruction imax{};
        imax.opcode = UsseOpcode::VMAX;
        imax.dst.reg_class = RegisterClass::Temporary; imax.dst.index = static_cast<uint8_t>(t0.base_index);
        imax.src0 = hw_t; imax.src1 = hw_0; imax.write_mask = mask_for_count(t.count);
        imax.src1_swiz12 = sw12_t; imax.src2_swiz4 = sw4_0;
        emit_inst(std::move(imax));
        ValueRef ts = make_temp_vec4(t.count);
        Vec4Swiz sw4_1 = Vec4Swiz::XYZW; HardwareRegister hw_1 = to_hw_src2(one_v, sw4_1);
        uint16_t sw12_t0 = kSwiz12XYZW; HardwareRegister hw_t0 = to_hw_src1(t0, sw12_t0);
        UsseInstruction imin{};
        imin.opcode = UsseOpcode::VMIN;
        imin.dst.reg_class = RegisterClass::Temporary; imin.dst.index = static_cast<uint8_t>(ts.base_index);
        imin.src0 = hw_t0; imin.src1 = hw_1; imin.write_mask = mask_for_count(t.count);
        imin.src1_swiz12 = sw12_t0; imin.src2_swiz4 = sw4_1;
        emit_inst(std::move(imin));

        ValueRef t2 = emit_binary_alu(frontend::BinaryOp::Mul, ts, ts, "t*t");
        ValueRef s2 = emit_binary_alu(frontend::BinaryOp::Mul, make_const_scalar(2.0f), ts, "2*t");
        ValueRef poly = emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(3.0f), s2, "3 - 2*t");
        return emit_binary_alu(frontend::BinaryOp::Mul, t2, poly, "smoothstep");
    }

    if (fn == "abs" && !call->arguments.empty()) {
        ValueRef a = generate_expr(call->arguments[0].get());
        a.is_negated = false;
        a.is_absolute = true;
        return a;
    }

    if (fn == "sign" && !call->arguments.empty()) {
        ValueRef x = generate_expr(call->arguments[0].get());
        ValueRef zero = make_const_scalar(0.0f);
        if (x.count == 1) {
            ValueRef pos = emit_compare_to_float(frontend::BinaryOp::Greater, x, zero);
            ValueRef neg = emit_compare_to_float(frontend::BinaryOp::Less, x, zero);
            return emit_binary_alu(frontend::BinaryOp::Sub, pos, neg, "sign");
        }
        ValueRef out = make_temp_vec4(x.count);
        for (uint8_t c = 0; c < x.count; ++c) {
            ValueRef xc = x; xc.count = 1;
            xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
            ValueRef pos = emit_compare_to_float(frontend::BinaryOp::Greater, xc, zero);
            ValueRef neg = emit_compare_to_float(frontend::BinaryOp::Less, xc, zero);
            ValueRef s = emit_binary_alu(frontend::BinaryOp::Sub, pos, neg, "sign lane");
            std::array<uint8_t, 4> map = {0, 0, 0, 0};
            emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, s);
        }
        return out;
    }

    if ((fn == "frac" || fn == "fract") && !call->arguments.empty()) {
        ValueRef a = generate_expr(call->arguments[0].get());
        if (a.is_const) {
            std::array<float, 4> res{};
            for (uint8_t i = 0; i < a.count; ++i) res[i] = a.const_vals[i] - std::floor(a.const_vals[i]);
            return make_const_vec(res, a.count);
        }
        ValueRef dst = make_temp_vec4(a.count);
        uint16_t sw12 = kSwiz12XYZW; HardwareRegister hw_s1 = to_hw_src1(a, sw12);
        Vec4Swiz sw4 = Vec4Swiz::XYZW; HardwareRegister hw_s2 = to_hw_src2(a, sw4);
        UsseInstruction inst{};
        inst.opcode = UsseOpcode::VFRC;
        inst.dst.reg_class = RegisterClass::Temporary; inst.dst.index = static_cast<uint8_t>(dst.base_index);
        inst.src0 = hw_s1; inst.src1 = hw_s2; inst.write_mask = mask_for_count(a.count);
        inst.src1_swiz12 = sw12; inst.src2_swiz4 = sw4; inst.comment = fn;
        emit_inst(std::move(inst));
        return dst;
    }

    if (fn == "floor" && !call->arguments.empty()) return emit_floor(generate_expr(call->arguments[0].get()));
    if (fn == "ceil"  && !call->arguments.empty()) return emit_ceil(generate_expr(call->arguments[0].get()));
    if (fn == "trunc" && !call->arguments.empty()) return emit_trunc(generate_expr(call->arguments[0].get()));
    if (fn == "round" && !call->arguments.empty()) {
        return emit_floor(emit_binary_alu(frontend::BinaryOp::Add,
                                          generate_expr(call->arguments[0].get()),
                                          make_const_scalar(0.5f), "round +0.5"));
    }
    if (fn == "fmod" && call->arguments.size() >= 2) {
        return emit_binary_alu(frontend::BinaryOp::Mod,
                               generate_expr(call->arguments[0].get()),
                               generate_expr(call->arguments[1].get()), "fmod");
    }

    if (fn == "rcp"   && !call->arguments.empty()) return emit_vcomp_vec(UsseOpcode::VRCP, generate_expr(call->arguments[0].get()), "rcp");
    if (fn == "rsqrt" && !call->arguments.empty()) return emit_vcomp_vec(UsseOpcode::VRSQ, generate_expr(call->arguments[0].get()), "rsqrt");
    if (fn == "sqrt"  && !call->arguments.empty()) return emit_sqrt(generate_expr(call->arguments[0].get()));
    if (fn == "exp2"  && !call->arguments.empty()) return emit_vcomp_vec(UsseOpcode::VEXP, generate_expr(call->arguments[0].get()), "exp2");
    if (fn == "log2"  && !call->arguments.empty()) return emit_vcomp_vec(UsseOpcode::VLOG, generate_expr(call->arguments[0].get()), "log2");
    if (fn == "exp"   && !call->arguments.empty()) {
        return emit_vcomp_vec(UsseOpcode::VEXP,
                              emit_binary_alu(frontend::BinaryOp::Mul,
                                              generate_expr(call->arguments[0].get()),
                                              make_const_scalar(1.44269504f), "exp scale"),
                              "exp");
    }
    if (fn == "log" && !call->arguments.empty()) {
        return emit_binary_alu(frontend::BinaryOp::Mul,
                               emit_vcomp_vec(UsseOpcode::VLOG, generate_expr(call->arguments[0].get()), "log"),
                               make_const_scalar(0.69314718f), "ln(2)");
    }
    if (fn == "pow" && call->arguments.size() >= 2) {
        return emit_pow(generate_expr(call->arguments[0].get()), generate_expr(call->arguments[1].get()));
    }
    if (fn == "sin" && !call->arguments.empty()) return emit_sin_cos(generate_expr(call->arguments[0].get()), false);
    if (fn == "cos" && !call->arguments.empty()) return emit_sin_cos(generate_expr(call->arguments[0].get()), true);
    if (fn == "tan" && !call->arguments.empty()) {
        ValueRef x = generate_expr(call->arguments[0].get());
        return emit_binary_alu(frontend::BinaryOp::Div, emit_sin_cos(x, false), emit_sin_cos(x, true), "tan");
    }
    if (fn == "sincos" && call->arguments.size() >= 3) {
        ValueRef x = generate_expr(call->arguments[0].get());
        assign_value_to_lvalue(call->arguments[1].get(), frontend::BinaryOp::Assign, emit_sin_cos(x, false));
        assign_value_to_lvalue(call->arguments[2].get(), frontend::BinaryOp::Assign, emit_sin_cos(x, true));
        return make_const_scalar(0.0f);
    }

    if ((fn == "ddx" || fn == "ddy") && !call->arguments.empty()) {
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef dst = make_temp_vec4(a.count);
        uint16_t sw12 = kSwiz12XYZW; HardwareRegister hw_s1 = to_hw_src1(a, sw12);
        Vec4Swiz sw4 = Vec4Swiz::XYZW; HardwareRegister hw_s2 = to_hw_src2(a, sw4);
        UsseInstruction inst{};
        inst.opcode = (fn == "ddx") ? UsseOpcode::VDSX : UsseOpcode::VDSY;
        inst.dst.reg_class = RegisterClass::Temporary; inst.dst.index = static_cast<uint8_t>(dst.base_index);
        inst.src0 = hw_s1; inst.src1 = hw_s2; inst.write_mask = mask_for_count(a.count);
        inst.src1_swiz12 = sw12; inst.src2_swiz4 = sw4; inst.comment = fn;
        emit_inst(std::move(inst));
        return dst;
    }
    if (fn == "fwidth" && !call->arguments.empty()) {
        ValueRef a = generate_expr(call->arguments[0].get());
        auto call_deriv = [&](UsseOpcode op) {
            ValueRef dst = make_temp_vec4(a.count);
            uint16_t sw12 = kSwiz12XYZW; HardwareRegister hw_s1 = to_hw_src1(a, sw12);
            Vec4Swiz sw4 = Vec4Swiz::XYZW; HardwareRegister hw_s2 = to_hw_src2(a, sw4);
            UsseInstruction inst{};
            inst.opcode = op;
            inst.dst.reg_class = RegisterClass::Temporary; inst.dst.index = static_cast<uint8_t>(dst.base_index);
            inst.src0 = hw_s1; inst.src1 = hw_s2; inst.write_mask = mask_for_count(a.count);
            inst.src1_swiz12 = sw12; inst.src2_swiz4 = sw4;
            emit_inst(std::move(inst));
            dst.is_absolute = true;
            return dst;
        };
        return emit_binary_alu(frontend::BinaryOp::Add, call_deriv(UsseOpcode::VDSX), call_deriv(UsseOpcode::VDSY), "fwidth");
    }

    if ((fn == "any" || fn == "all") && !call->arguments.empty()) {
        ValueRef v = generate_expr(call->arguments[0].get());
        ValueRef acc = make_const_scalar(fn == "all" ? 1.0f : 0.0f);
        for (uint8_t c = 0; c < v.count; ++c) {
            ValueRef vc = v; vc.count = 1;
            vc.swiz = {v.swiz[c], v.swiz[c], v.swiz[c], v.swiz[c]};
            ValueRef b = emit_compare_to_float(frontend::BinaryOp::NotEqual, vc, make_const_scalar(0.0f));
            if (c == 0) { acc = b; continue; }
            ValueRef t = make_temp_vec4(1);
            Vec4Swiz sw4 = Vec4Swiz::XYZW; HardwareRegister hw_s2 = to_hw_src2(b, sw4);
            uint16_t sw12 = kSwiz12XYZW; HardwareRegister hw_s1 = to_hw_src1(acc, sw12);
            UsseInstruction inst{};
            inst.opcode = (fn == "all") ? UsseOpcode::VMIN : UsseOpcode::VMAX;
            inst.dst.reg_class = RegisterClass::Temporary; inst.dst.index = static_cast<uint8_t>(t.base_index);
            inst.src0 = hw_s1; inst.src1 = hw_s2; inst.write_mask = 0x1;
            inst.src1_swiz12 = sw12; inst.src2_swiz4 = sw4;
            emit_inst(std::move(inst));
            acc = t;
        }
        return acc;
    }

    if (fn == "clip" && !call->arguments.empty()) {
        uses_discard_ = true;
        ValueRef x = generate_expr(call->arguments[0].get());
        ValueRef zero = make_const_scalar(0.0f);
        for (uint8_t c = 0; c < x.count; ++c) {
            ValueRef xc = x; xc.count = 1;
            xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
            emit_vtst_p0(xc, zero, VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "clip < 0");
            UsseInstruction kinst;
            kinst.opcode = UsseOpcode::KILL;
            kinst.kill_pred = KillPredicate::P0;
            kinst.comment = "clip kill";
            emit_inst(std::move(kinst));
        }
        return make_const_scalar(0.0f);
    }

    // 6. User-defined helper function lookup & inline expansion
    if (tu_) {
        for (const auto& fdecl : tu_->functions) {
            if (fdecl.name == fn && fdecl.body) {
                return invoke_user_function(&fdecl, call);
            }
        }
    }

    // Fallback: evaluate arguments cleanly
    ValueRef last;
    for (const auto& a : call->arguments) {
        last = generate_expr(a.get());
    }
    return last;
}

} // namespace open_shacc::backend
