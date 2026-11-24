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
    for (auto it = frame.owned_vec4_temps.rbegin(); it != frame.owned_vec4_temps.rend(); ++it) {
        reg_allocator_.free_temp_vec4(*it);
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

ValueRef UsseCodeGenerator::legalize_source_value(const ValueRef& val_in) {
    if (val_in.is_const || val_in.is_matrix || val_in.is_array) {
        return val_in;
    }

    const uint8_t n = std::clamp<uint8_t>(val_in.count, 1, 4);
    const uint16_t even_base = val_in.base_index & ~1u;
    const uint8_t bias = static_cast<uint8_t>(val_in.base_index & 1u);
    const bool is_sa = (val_in.reg_class == RegisterClass::Constant ||
                        val_in.reg_class == RegisterClass::SecAttr ||
                        val_in.reg_class == RegisterClass::TextureSampler);

    bool need_p0 = false;
    bool need_p1 = false;
    bool need_p2 = false;
    for (uint8_t i = 0; i < n; ++i) {
        uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
        if (c < 4u) {
            uint8_t p = static_cast<uint8_t>(c + bias);
            if (p < 2u) need_p0 = true;
            else if (p < 4u) need_p1 = true;
            else need_p2 = true;
        }
    }

    // 1. High secondary-attribute access (sa >= 128) using SMBO + VMOV + SMBO(0)
    if (is_sa && (even_base >= 128u || (need_p2 && (even_base + 4u) >= 128u))) {
        auto emit_sa_pair_vmov = [&](uint16_t dst_reg, uint16_t sa_pair_even, uint8_t mask) {
            UsseInstruction smbo{};
            smbo.opcode = UsseOpcode::SMBO;
            smbo.smbo_src1_offset = static_cast<uint16_t>(sa_pair_even >> 1);
            smbo.comment = "SA window offset";
            emit_inst(std::move(smbo));

            UsseInstruction mov{};
            mov.opcode = UsseOpcode::VMOV;
            mov.dst.reg_class = RegisterClass::Temporary;
            mov.dst.index = static_cast<uint8_t>(dst_reg & 0xFFu);
            mov.src0.reg_class = RegisterClass::SecAttr;
            mov.src0.index = 0;
            mov.write_mask = mask;
            mov.src2_swiz4 = Vec4Swiz::XYZW;
            mov.comment = "Load sa" + std::to_string(sa_pair_even);
            emit_inst(std::move(mov));
        };

        ValueRef tmp = make_temp_vec4(n);
        if (need_p0) {
            emit_sa_pair_vmov(tmp.base_index, even_base, 0b0011);
        }
        if (need_p1) {
            emit_sa_pair_vmov(static_cast<uint16_t>(tmp.base_index + 2u), static_cast<uint16_t>(even_base + 2u), 0b0011);
        }
        if (need_p2) {
            if (!need_p0) {
                emit_sa_pair_vmov(tmp.base_index, static_cast<uint16_t>(even_base + 4u), 0b0001);
            } else {
                ValueRef extra = make_temp_vec4(1);
                emit_sa_pair_vmov(extra.base_index, static_cast<uint16_t>(even_base + 4u), 0b0001);
                // Reset MOE immediately before emitting VADD lane fixups
                UsseInstruction rst{};
                rst.opcode = UsseOpcode::SMBO;
                rst.smbo_src1_offset = 0;
                rst.comment = "Reset SA window";
                emit_inst(std::move(rst));

                ValueRef packed = make_temp_vec4(n);
                for (uint8_t i = 0; i < n; ++i) {
                    uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
                    std::array<uint8_t, 4> map = {0, 0, 0, 0};
                    if (c < 4u && (c + bias) >= 4u) {
                        emit_move_mapped(RegisterClass::Temporary, packed.base_index,
                                         static_cast<uint8_t>(1u << i), map, extra, "high sa p4");
                    } else if (c < 4u) {
                        map[i] = static_cast<uint8_t>(c + bias);
                        ValueRef tmp_raw = tmp;
                        tmp_raw.count = 4;
                        tmp_raw.swiz = {SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W};
                        emit_move_mapped(RegisterClass::Temporary, packed.base_index,
                                         static_cast<uint8_t>(1u << i), map, tmp_raw, "high sa p0..3");
                    } else {
                        emit_move_mapped(RegisterClass::Temporary, packed.base_index,
                                         static_cast<uint8_t>(1u << i), map, make_const_scalar(
                                             c == 4 ? 0.0f : (c == 5 ? 1.0f : (c == 6 ? 2.0f : 0.5f))));
                    }
                }
                packed.is_negated = val_in.is_negated;
                packed.is_absolute = val_in.is_absolute;
                if (n == 1) {
                    packed.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
                }
                return packed;
            }
        }

        UsseInstruction rst{};
        rst.opcode = UsseOpcode::SMBO;
        rst.smbo_src1_offset = 0;
        rst.comment = "Reset SA window";
        emit_inst(std::move(rst));

        for (uint8_t i = 0; i < n; ++i) {
            uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
            if (c < 4u) {
                uint8_t p = static_cast<uint8_t>(c + bias);
                tmp.swiz[i] = sub_to_swiz_chan(p >= 4u ? static_cast<uint8_t>(p - 4u) : p);
            } else {
                tmp.swiz[i] = val_in.swiz[i];
            }
        }
        for (uint8_t i = n; i < 4; ++i) {
            tmp.swiz[i] = tmp.swiz[n - 1];
        }
        tmp.is_negated = val_in.is_negated;
        tmp.is_absolute = val_in.is_absolute;
        return tmp;
    }

    // 2. Odd-aligned cross-4-word boundary reads (bias == 1 and channel + bias >= 4)
    if (bias != 0 && need_p2) {
        if (!need_p0 && !need_p1) {
            ValueRef shifted = val_in;
            shifted.base_index = static_cast<uint16_t>(even_base + 4u);
            for (uint8_t i = 0; i < n; ++i) {
                uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
                if (c < 4u) {
                    shifted.swiz[i] = sub_to_swiz_chan(static_cast<uint8_t>((c + bias) - 4u));
                }
            }
            for (uint8_t i = n; i < 4; ++i) {
                shifted.swiz[i] = shifted.swiz[n - 1];
            }
            return shifted;
        }

        ValueRef tmp = make_temp_vec4(n);
        uint8_t lo_mask = 0;
        uint8_t hi_mask = 0;
        std::array<uint8_t, 4> lo_map = {0, 0, 0, 0};
        std::array<uint8_t, 4> hi_map = {0, 0, 0, 0};
        for (uint8_t i = 0; i < n; ++i) {
            uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
            uint8_t p = (c < 4u) ? static_cast<uint8_t>(c + bias) : 0u;
            if (c >= 4u || p < 4u) {
                lo_mask |= static_cast<uint8_t>(1u << i);
                lo_map[i] = i;
            } else {
                hi_mask |= static_cast<uint8_t>(1u << i);
                hi_map[i] = i;
            }
        }
        if (lo_mask != 0) {
            ValueRef lo_src = val_in;
            lo_src.is_negated = false;
            lo_src.is_absolute = false;
            lo_src.base_index = even_base;
            for (uint8_t i = 0; i < n; ++i) {
                uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
                if (c < 4u && (c + bias) < 4u) {
                    lo_src.swiz[i] = sub_to_swiz_chan(static_cast<uint8_t>(c + bias));
                } else if (c < 4u) {
                    lo_src.swiz[i] = SwizzleChannel::X;
                }
            }
            emit_move_mapped(RegisterClass::Temporary, tmp.base_index, lo_mask, lo_map, lo_src, "odd read lo");
        }
        if (hi_mask != 0) {
            ValueRef hi_src = val_in;
            hi_src.is_negated = false;
            hi_src.is_absolute = false;
            hi_src.base_index = static_cast<uint16_t>(even_base + 4u);
            for (uint8_t i = 0; i < n; ++i) {
                uint8_t c = static_cast<uint8_t>(val_in.swiz[i]);
                if (c < 4u && (c + bias) >= 4u) {
                    hi_src.swiz[i] = sub_to_swiz_chan(static_cast<uint8_t>((c + bias) - 4u));
                } else if (c < 4u) {
                    hi_src.swiz[i] = SwizzleChannel::X;
                }
            }
            emit_move_mapped(RegisterClass::Temporary, tmp.base_index, hi_mask, hi_map, hi_src, "odd read hi");
        }
        tmp.is_negated = val_in.is_negated;
        tmp.is_absolute = val_in.is_absolute;
        if (n == 1) {
            tmp.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        }
        return tmp;
    }

    return val_in;
}

HardwareRegister UsseCodeGenerator::to_hw_src1(const ValueRef& val_in, uint16_t& out_swiz12) {
    if (val_in.is_const) {
        std::array<SwizzleChannel, 4> chs{};
        bool all_inline = true;
        for (int i = 0; i < 4; ++i) {
            int lane = (i < val_in.count) ? i : (val_in.count - 1);
            if (!is_inline_const_val(val_in.const_vals[lane], chs[i])) {
                all_inline = false;
                break;
            }
        }
        if (all_inline) {
            out_swiz12 = pack_swizzle12(chs[0], chs[1], chs[2], chs[3]);
            HardwareRegister hw;
            hw.reg_class = RegisterClass::Temporary;
            hw.index = has_zero_reg_ ? zero_reg_index_ : 0;
            hw.is_negated = val_in.is_negated;
            hw.is_absolute = val_in.is_absolute;
            return hw;
        }
    }

    ValueRef val = legalize_source_value(val_in);
    uint16_t even_base = val.base_index & ~1u;
    uint8_t bias = static_cast<uint8_t>(val.base_index & 1u);
    uint8_t active_n = std::clamp<uint8_t>(val.count, 1, 4);
    std::array<SwizzleChannel, 4> adj = val.swiz;
    for (uint8_t i = 0; i < 4; ++i) {
        uint8_t src_slot = (i < active_n) ? i : static_cast<uint8_t>(active_n - 1);
        uint8_t c = static_cast<uint8_t>(val.swiz[src_slot]);
        if (c < 4u) {
            adj[i] = sub_to_swiz_chan(static_cast<uint8_t>(std::min<uint8_t>(3u, c + bias)));
        } else {
            adj[i] = val.swiz[src_slot];
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

HardwareRegister UsseCodeGenerator::to_hw_src2(const ValueRef& val_in, Vec4Swiz& out_swiz4) {
    if (!val_in.is_const && !val_in.is_negated) {
        ValueRef val = legalize_source_value(val_in);
        uint16_t even_base = val.base_index & ~1u;
        uint8_t bias = static_cast<uint8_t>(val.base_index & 1u);
        uint8_t active_n = std::clamp<uint8_t>(val.count, 1, 4);
        std::array<SwizzleChannel, 4> adj = val.swiz;
        for (uint8_t i = 0; i < 4; ++i) {
            uint8_t src_slot = (i < active_n) ? i : static_cast<uint8_t>(active_n - 1);
            uint8_t c = static_cast<uint8_t>(val.swiz[src_slot]);
            if (c < 4u) {
                adj[i] = sub_to_swiz_chan(static_cast<uint8_t>(std::min<uint8_t>(3u, c + bias)));
            } else {
                adj[i] = val.swiz[src_slot];
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

    ValueRef tmp = materialize_to_temp(val_in, val_in.count, "Materialize src2");
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
    uint8_t eff_mask = write_mask & 0xFu;
    if (eff_mask == 0) eff_mask = 0x1u;

    uint8_t mask0 = 0;
    uint8_t mask1 = 0;
    std::array<uint8_t, 4> logical_for_p0 = {0, 0, 0, 0};
    std::array<uint8_t, 4> logical_for_p1 = {0, 0, 0, 0};
    for (uint8_t logical_dl = 0; logical_dl < 4; ++logical_dl) {
        if ((eff_mask & (1u << logical_dl)) == 0) continue;
        uint8_t phys = static_cast<uint8_t>(logical_dl + dst_bias);
        if (phys < 4u) {
            mask0 |= static_cast<uint8_t>(1u << phys);
            logical_for_p0[phys] = logical_dl;
        } else {
            uint8_t sub1 = static_cast<uint8_t>(phys - 4u);
            mask1 |= static_cast<uint8_t>(1u << sub1);
            logical_for_p1[sub1] = logical_dl;
        }
    }

    auto eval_const_lane = [&](uint8_t logical_dl) -> float {
        uint8_t sc = target_lane_to_src_comp[logical_dl];
        uint8_t clamped_sc = (sc < src.count) ? sc : (src.count - 1);
        float cv = src.const_vals[clamped_sc];
        if (src.is_absolute) cv = std::fabs(cv);
        if (src.is_negated) cv = -cv;
        return cv;
    };

    if (src.is_const) {
        auto emit_const_chunk = [&](uint16_t chunk_even, uint8_t chunk_mask, const std::array<uint8_t, 4>& lmap) {
            if (chunk_mask == 0) return;
            bool all_inline = true;
            std::array<SwizzleChannel, 4> dst_swiz = {
                SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero
            };
            for (uint8_t p = 0; p < 4; ++p) {
                if ((chunk_mask & (1u << p)) == 0) continue;
                float cv = eval_const_lane(lmap[p]);
                if (!is_inline_const_val(cv, dst_swiz[p])) {
                    all_inline = false;
                    break;
                }
            }
            if (all_inline) {
                HardwareRegister zero_hw = get_zero_hw_reg();
                UsseInstruction inst;
                inst.opcode = UsseOpcode::VADD;
                inst.dst.reg_class = dst_class;
                inst.dst.index = static_cast<uint8_t>(chunk_even & 0xFFu);
                inst.src0 = zero_hw;
                inst.src1 = zero_hw;
                inst.write_mask = chunk_mask;
                inst.src1_swiz12 = pack_swizzle12(dst_swiz[0], dst_swiz[1], dst_swiz[2], dst_swiz[3]);
                inst.src2_swiz4 = Vec4Swiz::XXXX;
                inst.comment = comment;
                emit_inst(std::move(inst));
                return;
            }
            for (uint8_t p = 0; p < 4; ++p) {
                if ((chunk_mask & (1u << p)) == 0) continue;
                float cv = eval_const_lane(lmap[p]);
                ValueRef loaded = get_or_create_float_const(cv);
                std::array<uint8_t, 4> zero_map = {0, 0, 0, 0};
                emit_move_mapped(dst_class, static_cast<uint16_t>(chunk_even + p), 0x1u, zero_map, loaded, comment);
            }
        };

        emit_const_chunk(dst_even, mask0, logical_for_p0);
        emit_const_chunk(static_cast<uint16_t>(dst_even + 4u), mask1, logical_for_p1);
        return;
    }

    uint16_t raw_swiz12 = 0;
    HardwareRegister src1_hw = to_hw_src1(src, raw_swiz12);
    HardwareRegister zero_hw = get_zero_hw_reg();

    auto emit_reg_chunk = [&](uint16_t chunk_even, uint8_t chunk_mask, const std::array<uint8_t, 4>& lmap) {
        if (chunk_mask == 0) return;
        std::array<SwizzleChannel, 4> final_swiz = src1_hw.swizzle;
        for (uint8_t p = 0; p < 4; ++p) {
            if ((chunk_mask & (1u << p)) == 0) continue;
            uint8_t sc = target_lane_to_src_comp[lmap[p]];
            uint8_t clamped_sc = (sc < src.count) ? sc : (src.count - 1);
            final_swiz[p] = src1_hw.swizzle[clamped_sc];
        }
        UsseInstruction inst;
        inst.opcode = UsseOpcode::VADD;
        inst.dst.reg_class = dst_class;
        inst.dst.index = static_cast<uint8_t>(chunk_even & 0xFFu);
        inst.src0 = src1_hw;
        inst.src1 = zero_hw;
        inst.write_mask = chunk_mask;
        inst.src1_swiz12 = pack_swizzle12(final_swiz[0], final_swiz[1], final_swiz[2], final_swiz[3]);
        inst.src2_swiz4 = Vec4Swiz::XXXX;
        inst.comment = comment;
        emit_inst(std::move(inst));
    };

    emit_reg_chunk(dst_even, mask0, logical_for_p0);
    emit_reg_chunk(static_cast<uint16_t>(dst_even + 4u), mask1, logical_for_p1);
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
                case frontend::BinaryOp::BitAnd: folded[i] = static_cast<float>(static_cast<int32_t>(l) & static_cast<int32_t>(r)); break;
                case frontend::BinaryOp::BitOr:  folded[i] = static_cast<float>(static_cast<int32_t>(l) | static_cast<int32_t>(r)); break;
                case frontend::BinaryOp::BitXor: folded[i] = static_cast<float>(static_cast<int32_t>(l) ^ static_cast<int32_t>(r)); break;
                case frontend::BinaryOp::Shl:    folded[i] = static_cast<float>(static_cast<int32_t>(l) << (static_cast<int32_t>(r) & 31)); break;
                case frontend::BinaryOp::Shr:    folded[i] = static_cast<float>(static_cast<int32_t>(l) >> (static_cast<int32_t>(r) & 31)); break;
                default: folded[i] = 0.0f; break;
            }
        }
        return make_const_vec(folded, n);
    }

    // Integer shift/bitwise handling when non-const
    if (op == frontend::BinaryOp::Shl) {
        if (rhs_in.is_const) {
            uint32_t sh = static_cast<uint32_t>(static_cast<int32_t>(rhs_in.const_vals[0]) & 31);
            return emit_binary_alu(frontend::BinaryOp::Mul, lhs_in, make_const_scalar(static_cast<float>(1u << sh)), "shl");
        }
        return lhs_in;
    }
    if (op == frontend::BinaryOp::Shr) {
        if (rhs_in.is_const) {
            uint32_t sh = static_cast<uint32_t>(static_cast<int32_t>(rhs_in.const_vals[0]) & 31);
            return emit_floor(emit_binary_alu(frontend::BinaryOp::Mul, lhs_in,
                                              make_const_scalar(1.0f / static_cast<float>(1u << sh)), "shr"));
        }
        return lhs_in;
    }
    if (op == frontend::BinaryOp::BitAnd || op == frontend::BinaryOp::BitOr || op == frontend::BinaryOp::BitXor) {
        if (op == frontend::BinaryOp::BitAnd && rhs_in.is_const && rhs_in.const_vals[0] == 0.0f) {
            return make_const_scalar(0.0f);
        }
        return lhs_in;
    }

    // 2. Division via reciprocal + multiply
    if (op == frontend::BinaryOp::Div) {
        ValueRef out = make_temp_vec4(n);
        push_scope();
        ValueRef inv_rhs = emit_vcomp_vec(UsseOpcode::VRCP, rhs_in, comment.empty() ? "rcp" : comment);
        ValueRef prod = emit_binary_alu(frontend::BinaryOp::Mul, lhs_in, inv_rhs, comment);
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(n), prod, comment);
        pop_scope();
        return out;
    }

    // 3. Modulo / fmod: x - y * trunc(x / y)
    if (op == frontend::BinaryOp::Mod) {
        ValueRef out = make_temp_vec4(n);
        push_scope();
        ValueRef q = emit_binary_alu(frontend::BinaryOp::Div, lhs_in, rhs_in, "mod div");
        ValueRef t = emit_trunc(q);
        ValueRef p = emit_binary_alu(frontend::BinaryOp::Mul, rhs_in, t, "mod mul");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Sub, lhs_in, p, "mod sub");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(n), res, "mod");
        pop_scope();
        return out;
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

    // Non-inline splat constants can be read directly from the packed LIMM cache without allocating a temp vec4
    auto resolve_alu_const = [&](ValueRef& v) {
        if (!v.is_const) return;
        bool all_inline = true;
        SwizzleChannel dummy{};
        for (uint8_t i = 0; i < v.count; ++i) {
            float cv = v.const_vals[i];
            if (v.is_absolute) cv = std::fabs(cv);
            if (v.is_negated) cv = -cv;
            if (!is_inline_const_val(cv, dummy)) {
                all_inline = false;
                break;
            }
        }
        if (all_inline) return;

        bool is_splat = true;
        for (uint8_t i = 1; i < v.count; ++i) {
            if (v.const_vals[i] != v.const_vals[0]) {
                is_splat = false;
                break;
            }
        }
        if (is_splat) {
            float cv = v.const_vals[0];
            if (v.is_absolute) cv = std::fabs(cv);
            if (v.is_negated) cv = -cv;
            v = get_or_create_float_const(cv);
            v.count = n;
            v.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
            return;
        }
        v = materialize_to_temp(v, n, "const alu vec");
    };

    resolve_alu_const(lhs);
    resolve_alu_const(rhs);

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
    ValueRef src = legalize_source_value(src_in);
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
        if (v.is_const) {
            float cv = v.const_vals[0];
            if (v.is_absolute) cv = std::fabs(cv);
            if (v.is_negated) cv = -cv;
            if (cv == 0.0f) {
                return get_zero_hw_reg();
            }
        }
        if (!v.is_const && !v.is_negated && !v.is_absolute &&
            (v.base_index & 1u) == 0 && v.base_index < 128u && v.swiz[0] == SwizzleChannel::X &&
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
    ValueRef out = make_temp_vec4(x.count);
    push_scope();
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

    ValueRef sub = emit_binary_alu(frontend::BinaryOp::Sub, x, f, "floor");
    emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(x.count), sub, "floor");
    pop_scope();
    return out;
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
    if (x.count == 1) {
        ValueRef dst = make_temp_vec4(1);
        push_scope();
        ValueRef nx = x;
        nx.is_negated = !nx.is_negated;
        ValueRef fneg = emit_floor(nx);
        fneg.is_negated = !fneg.is_negated;
        ValueRef fpos = emit_floor(x);
        emit_vtst_p0(x, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "trunc sign");
        emit_move(RegisterClass::Temporary, dst.base_index, 0x1, fpos, "trunc pos");
        size_t br_end = emit_br(0, ExtPredicate::NegP0, "skip trunc neg");
        emit_move(RegisterClass::Temporary, dst.base_index, 0x1, fneg, "trunc neg");
        patch_br(br_end, instructions_.size());
        pop_scope();
        dst.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return dst;
    }
    ValueRef dst = make_temp_vec4(x.count);
    for (uint8_t c = 0; c < x.count; ++c) {
        push_scope();
        ValueRef xc = x;
        xc.count = 1;
        xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
        ValueRef tr = emit_trunc(xc);
        std::array<uint8_t, 4> map = {0, 0, 0, 0};
        emit_move_mapped(RegisterClass::Temporary, dst.base_index, static_cast<uint8_t>(1u << c), map, tr, "trunc lane");
        pop_scope();
    }
    return dst;
}

ValueRef UsseCodeGenerator::emit_sqrt(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = (x.const_vals[i] > 0.0f) ? std::sqrt(x.const_vals[i]) : 0.0f;
        return make_const_vec(res, x.count);
    }
    ValueRef out = make_temp_vec4(x.count);
    push_scope();
    ValueRef rsq = emit_vcomp_vec(UsseOpcode::VRSQ, x, "sqrt rsqrt");
    ValueRef res = emit_binary_alu(frontend::BinaryOp::Mul, x, rsq, "sqrt mul");
    emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(x.count), res, "sqrt");
    pop_scope();
    return out;
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
    uint8_t n = std::max(x.count, y.count);
    ValueRef out = make_temp_vec4(n);
    push_scope();
    ValueRef lx = emit_vcomp_vec(UsseOpcode::VLOG, x, "pow log2");
    ValueRef prod = emit_binary_alu(frontend::BinaryOp::Mul, lx, y, "pow y*log2(x)");
    ValueRef res = emit_vcomp_vec(UsseOpcode::VEXP, prod, "pow exp2");
    emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(n), res, "pow");
    pop_scope();
    return out;
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

    uint8_t n = std::clamp<uint8_t>(x_in.count, 1, 4);
    uint8_t wmask = mask_for_count(n);
    ValueRef out = make_temp_vec4(n);

    // Phase 1: Range-reduce x into [-pi, pi] inside out, using tight subscopes to cap peak temps at <= 3 vec4s
    {
        push_scope();
        ValueRef x = x_in;
        if (is_cos) {
            x = emit_binary_alu(frontend::BinaryOp::Add, x, make_const_scalar(kHalfPi), "cos shift");
        }
        ValueRef scaled = emit_binary_alu(frontend::BinaryOp::Mul, x, make_const_scalar(kInvTwoPi), "sin/cos inv2pi");
        ValueRef shifted = emit_binary_alu(frontend::BinaryOp::Add, scaled, make_const_scalar(0.5f), "sin/cos +0.5");
        emit_move(RegisterClass::Temporary, out.base_index, wmask, shifted, "sin/cos wrap");
        pop_scope();
    }
    {
        push_scope();
        ValueRef frc = make_temp_vec4(n);
        {
            uint16_t sw12 = kSwiz12XYZW;
            HardwareRegister hw_s1 = to_hw_src1(out, sw12);
            Vec4Swiz sw4 = Vec4Swiz::XYZW;
            HardwareRegister hw_s2 = to_hw_src2(out, sw4);
            UsseInstruction inst;
            inst.opcode = UsseOpcode::VFRC;
            inst.dst.reg_class = RegisterClass::Temporary;
            inst.dst.index = static_cast<uint8_t>(frc.base_index);
            inst.src0 = hw_s1;
            inst.src1 = hw_s2;
            inst.write_mask = wmask;
            inst.src1_swiz12 = sw12;
            inst.src2_swiz4 = sw4;
            inst.comment = "sin/cos frac";
            emit_inst(std::move(inst));
        }

        ValueRef xr = emit_binary_alu(frontend::BinaryOp::Sub,
                                      emit_binary_alu(frontend::BinaryOp::Mul, frc, make_const_scalar(kTwoPi), "sin/cos *2pi"),
                                      make_const_scalar(kPi), "sin/cos -pi");
        emit_move(RegisterClass::Temporary, out.base_index, wmask, xr, "sin/cos xr");
        pop_scope();
    }

    // Phase 2: y = B * xr + C * xr * |xr|, overwrite out with y, release phase-2 temps
    {
        push_scope();
        ValueRef abs_xr = out;
        abs_xr.is_absolute = true;
        ValueRef term1 = emit_binary_alu(frontend::BinaryOp::Mul, out, make_const_scalar(kB), "sin B*x");
        ValueRef term2 = emit_binary_alu(frontend::BinaryOp::Mul,
                                         emit_binary_alu(frontend::BinaryOp::Mul, out, abs_xr, "x*|x|"),
                                         make_const_scalar(kC), "C*x*|x|");
        ValueRef y = emit_binary_alu(frontend::BinaryOp::Add, term1, term2, "sin poly y");
        emit_move(RegisterClass::Temporary, out.base_index, wmask, y, "sin/cos y");
        pop_scope();
    }

    // Phase 3: y + P * (y * |y| - y), store into out, release phase-3 temps
    {
        push_scope();
        ValueRef abs_y = out;
        abs_y.is_absolute = true;
        ValueRef y_abs_y = emit_binary_alu(frontend::BinaryOp::Mul, out, abs_y, "y*|y|");
        ValueRef diff = emit_binary_alu(frontend::BinaryOp::Sub, y_abs_y, out, "y*|y|-y");
        ValueRef corr = emit_binary_alu(frontend::BinaryOp::Mul, diff, make_const_scalar(kP), "P*(y*|y|-y)");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Add, out, corr, is_cos ? "cos result" : "sin result");
        emit_move(RegisterClass::Temporary, out.base_index, wmask, res, is_cos ? "cos" : "sin");
        pop_scope();
    }

    return out;
}

ValueRef UsseCodeGenerator::emit_atan(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = std::atan(x.const_vals[i]);
        return make_const_vec(res, x.count);
    }
    constexpr float kHalfPi = 1.57079632679f;
    uint8_t n = std::clamp<uint8_t>(x.count, 1, 4);

    auto atan_scalar = [&](const ValueRef& sx) -> ValueRef {
        ValueRef mag = make_temp_vec4(1);
        ValueRef r = make_temp_vec4(1);
        ValueRef ax = sx;
        ax.is_negated = false;
        ax.is_absolute = true;

        {
            push_scope();
            ValueRef inv_ax = emit_vcomp_vec(UsseOpcode::VRCP,
                                             emit_binary_alu(frontend::BinaryOp::Add, ax, make_const_scalar(1e-6f), "atan eps"),
                                             "atan 1/|x|");
            ValueRef ax_t = materialize_to_temp(ax, 1, "atan |x|");
            ValueRef inv_t = materialize_to_temp(inv_ax, 1, "atan rcp");
            Vec4Swiz sw4 = Vec4Swiz::XXXX; HardwareRegister hw_s2 = to_hw_src2(inv_t, sw4);
            uint16_t sw12 = kSwiz12XYZW; HardwareRegister hw_s1 = to_hw_src1(ax_t, sw12);
            UsseInstruction imin{};
            imin.opcode = UsseOpcode::VMIN;
            imin.dst.reg_class = RegisterClass::Temporary; imin.dst.index = static_cast<uint8_t>(r.base_index);
            imin.src0 = hw_s1; imin.src1 = hw_s2; imin.write_mask = 0x1;
            imin.src1_swiz12 = sw12; imin.src2_swiz4 = sw4;
            emit_inst(std::move(imin));
            pop_scope();
        }

        // Accumulate polynomial in mag, keeping r2 in a dedicated subscope so peak is <= 4 vec4s
        {
            push_scope();
            ValueRef r2 = emit_binary_alu(frontend::BinaryOp::Mul, r, r, "atan r^2");
            {
                push_scope();
                ValueRef p = emit_binary_alu(frontend::BinaryOp::Sub,
                                             make_const_scalar(0.288679f),
                                             emit_binary_alu(frontend::BinaryOp::Mul, r2, make_const_scalar(0.079331f)),
                                             "atan p1");
                emit_move(RegisterClass::Temporary, mag.base_index, 0x1, p, "atan p");
                pop_scope();
            }
            {
                push_scope();
                ValueRef q = emit_binary_alu(frontend::BinaryOp::Sub,
                                             make_const_scalar(0.995354f),
                                             emit_binary_alu(frontend::BinaryOp::Mul, r2, mag),
                                             "atan q");
                ValueRef raw_mag = emit_binary_alu(frontend::BinaryOp::Mul, r, q, "atan_small");
                emit_move(RegisterClass::Temporary, mag.base_index, 0x1, raw_mag, "atan mag");
                pop_scope();
            }
            pop_scope();
        }

        {
            push_scope();
            emit_vtst_p0(ax, make_const_scalar(1.0f), VtstSignTest::GtOrGe, VtstZeroTest::ExcludeZero, "|x|>1");
            size_t br_fold = emit_br(0, ExtPredicate::NegP0, "skip pi/2-atan");
            ValueRef folded = emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(kHalfPi), mag, "pi/2 - atan");
            emit_move(RegisterClass::Temporary, mag.base_index, 0x1, folded, "atan folded");
            patch_br(br_fold, instructions_.size());

            emit_vtst_p0(sx, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "x<0");
            size_t br_sign = emit_br(0, ExtPredicate::NegP0, "skip atan neg");
            ValueRef neg_mag = mag; neg_mag.is_negated = !neg_mag.is_negated;
            emit_move(RegisterClass::Temporary, mag.base_index, 0x1, neg_mag, "atan neg");
            patch_br(br_sign, instructions_.size());
            pop_scope();
        }
        mag.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return mag;
    };

    if (n == 1) return atan_scalar(x);
    ValueRef out = make_temp_vec4(n);
    for (uint8_t c = 0; c < n; ++c) {
        push_scope();
        ValueRef xc = x; xc.count = 1;
        xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
        ValueRef ac = atan_scalar(xc);
        std::array<uint8_t, 4> map = {0, 0, 0, 0};
        emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, ac, "atan lane");
        pop_scope();
    }
    return out;
}

ValueRef UsseCodeGenerator::emit_atan2(const ValueRef& y, const ValueRef& x) {
    uint8_t n = std::max(y.count, x.count);
    n = std::clamp<uint8_t>(n, 1, 4);
    if (y.is_const && x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < n; ++i) {
            float yv = y.const_vals[i < y.count ? i : y.count - 1];
            float xv = x.const_vals[i < x.count ? i : x.count - 1];
            res[i] = std::atan2(yv, xv);
        }
        return make_const_vec(res, n);
    }
    constexpr float kPi = 3.14159265359f;
    auto atan2_scalar = [&](const ValueRef& sy, const ValueRef& sx) -> ValueRef {
        ValueRef a = make_temp_vec4(1);
        {
            push_scope();
            ValueRef ax = sx; ax.is_negated = false; ax.is_absolute = true;
            ValueRef denom = emit_binary_alu(frontend::BinaryOp::Add, ax, make_const_scalar(1e-6f), "atan2 denom");
            ValueRef ratio = emit_binary_alu(frontend::BinaryOp::Div, sy, denom, "atan2 y/|x|");
            emit_move(RegisterClass::Temporary, a.base_index, 0x1, ratio, "atan2 ratio");
            pop_scope();
        }
        {
            push_scope();
            ValueRef base_a = emit_atan(a);
            emit_move(RegisterClass::Temporary, a.base_index, 0x1, base_a, "atan2 a");
            pop_scope();
        }
        {
            push_scope();
            emit_vtst_p0(sx, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "atan2 x<0");
            size_t br_pos_x = emit_br(0, ExtPredicate::NegP0, "x >= 0");
            emit_vtst_p0(sy, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "atan2 y<0");
            size_t br_neg_y = emit_br(0, ExtPredicate::P0, "x<0, y<0");
            ValueRef q2 = emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(kPi), a, "pi - a");
            emit_move(RegisterClass::Temporary, a.base_index, 0x1, q2);
            size_t br_done = emit_br(0, ExtPredicate::None, "atan2 q2 done");
            patch_br(br_neg_y, instructions_.size());
            ValueRef q3 = emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(-kPi), a, "-pi - a");
            emit_move(RegisterClass::Temporary, a.base_index, 0x1, q3);
            patch_br(br_done, instructions_.size());
            patch_br(br_pos_x, instructions_.size());
            pop_scope();
        }
        a.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return a;
    };

    if (n == 1) return atan2_scalar(y, x);
    ValueRef out = make_temp_vec4(n);
    for (uint8_t c = 0; c < n; ++c) {
        push_scope();
        ValueRef yc = y; yc.count = 1;
        if (y.count > 1) yc.swiz = {y.swiz[c], y.swiz[c], y.swiz[c], y.swiz[c]};
        ValueRef xc = x; xc.count = 1;
        if (x.count > 1) xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
        ValueRef ac = atan2_scalar(yc, xc);
        std::array<uint8_t, 4> map = {0, 0, 0, 0};
        emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, ac, "atan2 lane");
        pop_scope();
    }
    return out;
}

ValueRef UsseCodeGenerator::emit_acos(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = std::acos(std::clamp(x.const_vals[i], -1.0f, 1.0f));
        return make_const_vec(res, x.count);
    }
    constexpr float kPi = 3.14159265359f;
    uint8_t n = std::clamp<uint8_t>(x.count, 1, 4);
    auto acos_scalar = [&](const ValueRef& sx) -> ValueRef {
        ValueRef res = make_temp_vec4(1);
        ValueRef c_ax = make_temp_vec4(1);
        {
            push_scope();
            ValueRef ax = sx; ax.is_negated = false; ax.is_absolute = true;
            ValueRef ax_t = materialize_to_temp(ax, 1);
            ValueRef one_t = materialize_to_temp(make_const_scalar(1.0f), 1);
            Vec4Swiz sw4 = Vec4Swiz::XXXX; HardwareRegister hw_s2 = to_hw_src2(one_t, sw4);
            uint16_t sw12 = kSwiz12XYZW; HardwareRegister hw_s1 = to_hw_src1(ax_t, sw12);
            UsseInstruction imin{};
            imin.opcode = UsseOpcode::VMIN;
            imin.dst.reg_class = RegisterClass::Temporary; imin.dst.index = static_cast<uint8_t>(c_ax.base_index);
            imin.src0 = hw_s1; imin.src1 = hw_s2; imin.write_mask = 0x1;
            imin.src1_swiz12 = sw12; imin.src2_swiz4 = sw4;
            emit_inst(std::move(imin));
            pop_scope();
        }
        {
            push_scope();
            ValueRef poly = emit_binary_alu(frontend::BinaryOp::Sub,
                                            make_const_scalar(0.0742610f),
                                            emit_binary_alu(frontend::BinaryOp::Mul, c_ax, make_const_scalar(0.0187293f)));
            emit_move(RegisterClass::Temporary, res.base_index, 0x1, poly);
            pop_scope();
        }
        {
            push_scope();
            ValueRef poly = emit_binary_alu(frontend::BinaryOp::Add,
                                            make_const_scalar(-0.2121144f),
                                            emit_binary_alu(frontend::BinaryOp::Mul, c_ax, res));
            emit_move(RegisterClass::Temporary, res.base_index, 0x1, poly);
            pop_scope();
        }
        {
            push_scope();
            ValueRef poly = emit_binary_alu(frontend::BinaryOp::Add,
                                            make_const_scalar(1.5707288f),
                                            emit_binary_alu(frontend::BinaryOp::Mul, c_ax, res));
            ValueRef raw = emit_binary_alu(frontend::BinaryOp::Mul,
                                           emit_sqrt(emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(1.0f), c_ax)),
                                           poly, "acos poly");
            emit_move(RegisterClass::Temporary, res.base_index, 0x1, raw, "acos res");
            pop_scope();
        }
        {
            push_scope();
            emit_vtst_p0(sx, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "acos x<0");
            size_t br_pos = emit_br(0, ExtPredicate::NegP0, "acos x>=0");
            ValueRef mirrored = emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(kPi), res, "pi - acos(|x|)");
            emit_move(RegisterClass::Temporary, res.base_index, 0x1, mirrored);
            patch_br(br_pos, instructions_.size());
            pop_scope();
        }
        res.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return res;
    };
    if (n == 1) return acos_scalar(x);
    ValueRef out = make_temp_vec4(n);
    for (uint8_t c = 0; c < n; ++c) {
        push_scope();
        ValueRef xc = x; xc.count = 1;
        xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
        ValueRef ac = acos_scalar(xc);
        std::array<uint8_t, 4> map = {0, 0, 0, 0};
        emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, ac, "acos lane");
        pop_scope();
    }
    return out;
}

ValueRef UsseCodeGenerator::emit_asin(const ValueRef& x) {
    if (x.is_const) {
        std::array<float, 4> res{};
        for (uint8_t i = 0; i < x.count; ++i) res[i] = std::asin(std::clamp(x.const_vals[i], -1.0f, 1.0f));
        return make_const_vec(res, x.count);
    }
    constexpr float kHalfPi = 1.57079632679f;
    ValueRef out = make_temp_vec4(x.count);
    push_scope();
    ValueRef res = emit_binary_alu(frontend::BinaryOp::Sub, make_const_scalar(kHalfPi), emit_acos(x), "asin");
    emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(x.count), res, "asin");
    pop_scope();
    return out;
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
        // Check for dynamic array element write (e.g., vClip[i] = ... or vClip[i].xy = ...)
        const frontend::Expression* arr_expr_base = lvalue;
        std::string tail_swiz;
        if (auto* swiz = dynamic_cast<const frontend::SwizzleExpr*>(lvalue)) {
            if (is_swizzle_letters(swiz->components)) {
                tail_swiz = swiz->components;
                arr_expr_base = swiz->target.get();
            }
        }
        if (auto* arr = dynamic_cast<const frontend::ArrayAccessExpr*>(arr_expr_base)) {
            int64_t const_idx = 0;
            if (!eval_const_int(arr->index.get(), const_idx)) {
                RegisterClass base_cls{};
                uint16_t base_reg = 0;
                uint8_t base_mask = 0xF;
                std::array<uint8_t, 4> base_map{};
                ValueRef base_val;
                std::string base_lbl;
                if (resolve_lvalue(arr->target.get(), base_cls, base_reg, base_mask, base_map, base_val, base_lbl)) {
                    uint32_t limit = base_val.is_matrix ? base_val.mat_rows : std::min<uint32_t>(base_val.array_size, 16u);
                    uint16_t stride = base_val.is_matrix ? base_val.row_stride : base_val.elem_stride;
                    uint8_t write_mask = mask_for_count(base_val.count);
                    std::array<uint8_t, 4> write_map = {0, 1, 2, 3};
                    if (!tail_swiz.empty()) {
                        write_mask = 0;
                        write_map = {0, 0, 0, 0};
                        for (size_t s = 0; s < tail_swiz.size() && s < 4; ++s) {
                            uint8_t lane = swizzle_char_to_sub(tail_swiz[s]);
                            write_mask |= static_cast<uint8_t>(1u << lane);
                            write_map[lane] = static_cast<uint8_t>(s);
                        }
                    }
                    ValueRef idx_val = generate_expr(arr->index.get());
                    for (uint32_t k = 0; k < limit; ++k) {
                        emit_vtst_p0(idx_val, make_const_scalar(static_cast<float>(k)),
                                     VtstSignTest::NeOrEq, VtstZeroTest::IncludeZero,
                                     base_lbl + "[" + std::to_string(k) + "] write check");
                        size_t br_skip = emit_br(0, ExtPredicate::NegP0, "skip dyn arr write");
                        uint16_t elem_dst = static_cast<uint16_t>(base_reg + static_cast<uint16_t>(k) * stride);
                        ValueRef val_to_write = rhs;
                        if (compound_op != frontend::BinaryOp::Assign) {
                            ValueRef cur_elem = base_val;
                            cur_elem.base_index = elem_dst;
                            cur_elem.is_array = false;
                            cur_elem.is_matrix = false;
                            val_to_write = emit_binary_alu(compound_op, cur_elem, rhs, base_lbl + " dyn compound");
                        }
                        emit_move_mapped(base_cls, elem_dst, write_mask, write_map, val_to_write,
                                         base_lbl + "[" + std::to_string(k) + "] write");
                        patch_br(br_skip, instructions_.size());
                    }
                    return;
                }
            }
        }
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

        for (const auto& v : tu.varyings) {
            if (v.is_out) {
                if (semantic_contains(v.semantic, "POSITION") || semantic_contains(v.semantic, "HPOS")) {
                    has_pos_out = true;
                }
                if (semantic_contains(v.semantic, "COLOR") || semantic_contains(v.semantic, "FRAG")) {
                    has_color_out = true;
                }
                if (semantic_contains(v.semantic, "DEPTH")) {
                    has_depth_out = true;
                }
            }
        }

        bool has_discard = statement_contains_discard(entry_fn->body.get());
        if (has_discard || has_depth_out || (!has_pos_out && (has_color_out || !reg_allocator_.get_samplers().empty()))) {
            is_frag = true;
        }
    }
    is_fragment_ = is_frag;

    // 4. Allocate non-uniform input and output parameters of entry function & top-level varyings
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

    for (const auto& v : tu.varyings) {
        if (v.is_uniform || v.type.is_sampler()) continue;
        if (reg_allocator_.find_symbol(v.name)) continue;

        if (v.is_out && !v.is_in) {
            SymbolBinding binding;
            reg_allocator_.allocate_output(v.name, v.type, v.semantic, binding);
            if (is_frag && semantic_contains(v.semantic, "DEPTH")) {
                uses_depth_ = true;
            }
        } else {
            SymbolBinding binding;
            reg_allocator_.allocate_input(v.name, v.type, v.semantic, binding, is_frag);
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
        if (reg_allocator_.find_symbol(out_name)) {
            out_name = "$entry_return";
        }
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

    if (reg_allocator_.has_temp_overflow()) {
        error_message_ = "Temporary register overflow (exceeded 120 F32 scalar temporaries)";
        return false;
    }

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
            push_scope();
            ValueRef rhs = generate_expr(decl->initializer.get());
            frontend::VariableExpr lhs_var(decl->name);
            assign_value_to_lvalue(&lhs_var, frontend::BinaryOp::Assign, rhs);
            pop_scope();
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
                push_scope();
                ValueRef rhs = generate_expr(decl->initializer.get());
                frontend::VariableExpr lhs_var(decl->name);
                assign_value_to_lvalue(&lhs_var, frontend::BinaryOp::Assign, rhs);
                pop_scope();
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
                push_scope();
                ValueRef ev = generate_expr(il->elements[e].get());
                emit_move(RegisterClass::Temporary, static_cast<uint16_t>(base + e * 4),
                          mask_for_count(comp_n), ev, decl->name + "[" + std::to_string(e) + "]");
                pop_scope();
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
            push_scope();
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
            pop_scope();
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
        push_scope();
        ValueRef init_v = generate_expr(decl->initializer.get());
        emit_move(RegisterClass::Temporary, base, mask_for_count(n), init_v, "init " + decl->name);
        pop_scope();
    }
}

void UsseCodeGenerator::generate_assign(const frontend::AssignStmt* assign) {
    if (!assign || !assign->lvalue) return;

    push_scope();
    if (!assign->rvalue) {
        generate_expr(assign->lvalue.get());
        pop_scope();
        return;
    }

    ValueRef rhs = generate_expr(assign->rvalue.get());
    assign_value_to_lvalue(assign->lvalue.get(), assign->compound_op, rhs);
    pop_scope();
}

void UsseCodeGenerator::generate_return(const frontend::ReturnStmt* ret) {
    if (!ret) return;

    if (inline_call_ctx_.active) {
        if (ret->value && inline_call_ctx_.return_slot.has_value()) {
            push_scope();
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
            } else if (inline_call_ctx_.return_slot->is_matrix && rv.is_matrix) {
                uint8_t rows = std::min(inline_call_ctx_.return_slot->mat_rows, rv.mat_rows);
                uint8_t cols = std::min(inline_call_ctx_.return_slot->mat_cols, rv.mat_cols);
                for (uint8_t r = 0; r < rows; ++r) {
                    ValueRef rval;
                    rval.reg_class = rv.reg_class;
                    rval.base_index = static_cast<uint16_t>(rv.base_index + r * rv.row_stride);
                    rval.count = cols;
                    emit_move(RegisterClass::Temporary,
                              static_cast<uint16_t>(inline_call_ctx_.return_slot->base_index + r * 4),
                              mask_for_count(cols), rval, "helper mat return");
                }
            } else {
                emit_move(RegisterClass::Temporary, inline_call_ctx_.return_slot->base_index,
                          mask_for_count(inline_call_ctx_.return_slot->count), rv, "helper return");
            }
            pop_scope();
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

    push_scope();
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
                pop_scope();
                return;
            }
        }
    }

    emit_move(RegisterClass::Output, base_out, mask_for_count(out_comps), res, "Shader output o" + std::to_string(base_out));
    pop_scope();
}

void UsseCodeGenerator::generate_if(const frontend::IfStmt* if_stmt) {
    if (!if_stmt) return;
    push_scope();
    emit_condition_to_p0(if_stmt->condition.get());
    pop_scope();
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
        push_scope();
        emit_condition_to_p0(for_stmt->condition.get());
        pop_scope();
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
        push_scope();
        emit_condition_to_p0(while_stmt->condition.get());
        pop_scope();
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
        push_scope();
        emit_condition_to_p0(dw_stmt->condition.get());
        pop_scope();
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
    inst.short_pred = ShortPredicate::None;
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
    ValueRef dst = make_temp_vec4(4);
    push_scope();
    emit_condition_to_p0(tern->condition.get());
    pop_scope();
    size_t br_else = emit_br(0, ExtPredicate::NegP0, "ternary false");

    push_scope();
    ValueRef t_val = generate_expr(tern->then_expr.get());
    emit_move(RegisterClass::Temporary, dst.base_index, mask_for_count(t_val.count), t_val, "ternary then");
    uint8_t t_cnt = t_val.count;
    pop_scope();
    size_t br_end = emit_br(0, ExtPredicate::None, "ternary end");

    patch_br(br_else, instructions_.size());
    push_scope();
    ValueRef e_val = generate_expr(tern->else_expr.get());
    emit_move(RegisterClass::Temporary, dst.base_index, mask_for_count(e_val.count), e_val, "ternary else");
    uint8_t e_cnt = e_val.count;
    pop_scope();

    patch_br(br_end, instructions_.size());
    dst.count = std::max(t_cnt, e_cnt);
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

const frontend::FunctionDecl* UsseCodeGenerator::resolve_user_function(
    const std::string& name,
    const frontend::CallExpr* call,
    const std::vector<ValueRef>& eval_args
) const {
    if (!tu_) return nullptr;

    auto is_int_type = [](frontend::BaseType b) {
        using BT = frontend::BaseType;
        return b == BT::Int || b == BT::Int2 || b == BT::Int3 || b == BT::Int4 ||
               b == BT::UInt || b == BT::UInt2 || b == BT::UInt3 || b == BT::UInt4 ||
               b == BT::Short || b == BT::Short2 || b == BT::Short3 || b == BT::Short4 ||
               b == BT::UShort || b == BT::UShort2 || b == BT::UShort3 || b == BT::UShort4 ||
               b == BT::Char || b == BT::UChar ||
               b == BT::Bool || b == BT::Bool2 || b == BT::Bool3 || b == BT::Bool4;
    };

    const frontend::FunctionDecl* best_fn = nullptr;
    int best_score = -100000;

    for (const auto& fdecl : tu_->functions) {
        if (fdecl.name != name || !fdecl.body) continue;
        if (fdecl.parameters.size() != eval_args.size()) continue;

        int score = 0;
        bool incompatible = false;
        for (size_t i = 0; i < fdecl.parameters.size(); ++i) {
            const auto& param = fdecl.parameters[i];
            const auto& arg = eval_args[i];
            const bool arg_is_sampler = (!arg.is_const && arg.reg_class == RegisterClass::TextureSampler);

            if (param.type.is_sampler() || arg_is_sampler) {
                if (param.type.is_sampler() && arg_is_sampler) {
                    score += 100;
                } else {
                    incompatible = true;
                    break;
                }
            } else if (param.type.is_matrix() || arg.is_matrix) {
                if (param.type.is_matrix() && arg.is_matrix) {
                    if (param.type.matrix_rows() == arg.mat_rows && param.type.matrix_cols() == arg.mat_cols) {
                        score += 100;
                    } else {
                        incompatible = true;
                        break;
                    }
                } else {
                    incompatible = true;
                    break;
                }
            } else if (param.type.base == frontend::BaseType::CustomStruct || !arg.struct_name.empty()) {
                if (param.type.base == frontend::BaseType::CustomStruct &&
                    param.type.struct_name == arg.struct_name) {
                    score += 100;
                } else {
                    incompatible = true;
                    break;
                }
            } else {
                size_t pcomp = param.type.component_count();
                if (pcomp == arg.count) {
                    score += 50;
                } else if (arg.count == 1 && pcomp > 1) {
                    score += 5;
                } else if (arg.count > pcomp) {
                    score -= 20;
                } else {
                    incompatible = true;
                    break;
                }

                // Prefer matching float vs int family when overloading e.g. vglMul(float4,float4) vs vglMul(int4,int4)
                bool arg_int_hint = false;
                if (call && i < call->arguments.size()) {
                    if (auto* lit = dynamic_cast<const frontend::LiteralExpr*>(call->arguments[i].get())) {
                        arg_int_hint = std::holds_alternative<int>(lit->value);
                    } else if (auto* ve = dynamic_cast<const frontend::VariableExpr*>(call->arguments[i].get())) {
                        if (const auto* sym = reg_allocator_.find_symbol(ve->name)) {
                            arg_int_hint = is_int_type(sym->type.base);
                        }
                    }
                }
                if (is_int_type(param.type.base) == arg_int_hint) {
                    score += 10;
                }
            }
        }

        if (!incompatible && score > best_score) {
            best_score = score;
            best_fn = &fdecl;
        }
    }

    if (best_fn) return best_fn;

    // Fallback: first matching name/body
    for (const auto& fdecl : tu_->functions) {
        if (fdecl.name == name && fdecl.body) {
            return &fdecl;
        }
    }
    return nullptr;
}

ValueRef UsseCodeGenerator::invoke_user_function(
    const frontend::FunctionDecl* fn,
    const frontend::CallExpr* call,
    std::vector<ValueRef> evaluated_args
) {
    if (!fn || !fn->body || inline_depth_ >= 8) {
        return make_const_scalar(0.0f);
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
    } else if (fn->return_type.is_matrix()) {
        uint8_t rows = static_cast<uint8_t>(fn->return_type.matrix_rows());
        uint8_t cols = static_cast<uint8_t>(fn->return_type.matrix_cols());
        uint8_t rbase = reg_allocator_.allocate_contiguous_temp_vec4s(rows);
        if (!scopes_.empty()) {
            for (uint8_t r = 0; r < rows; ++r) {
                scopes_.back().owned_vec4_temps.push_back(static_cast<uint8_t>(rbase + r * 4));
            }
        }
        ret_val.reg_class = RegisterClass::Temporary;
        ret_val.base_index = rbase;
        ret_val.count = cols;
        ret_val.is_matrix = true;
        ret_val.mat_rows = rows;
        ret_val.mat_cols = cols;
        ret_val.row_stride = 4;
        inline_call_ctx_.return_slot = ret_val;
    } else if (fn->return_type.base != frontend::BaseType::Void) {
        uint8_t rc = static_cast<uint8_t>(std::clamp<size_t>(fn->return_type.component_count(), 1, 4));
        ret_val = make_temp_vec4(rc);
        inline_call_ctx_.return_slot = ret_val;
    }

    push_scope();
    for (size_t i = 0; i < fn->parameters.size() && i < evaluated_args.size(); ++i) {
        const auto& param = fn->parameters[i];
        if (param.type.is_sampler()) {
            SymbolBinding sbind{};
            sbind.name = param.name;
            sbind.type = param.type;
            sbind.reg_class = RegisterClass::TextureSampler;
            sbind.base_register = evaluated_args[i].base_index;
            sbind.register_count = 1;
            sbind.allocated_words = 1;
            bind_scoped_symbol(param.name, sbind);
            continue;
        }
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
        if (param.type.is_matrix()) {
            uint8_t rows = static_cast<uint8_t>(param.type.matrix_rows());
            uint8_t cols = static_cast<uint8_t>(param.type.matrix_cols());
            uint8_t mbase = reg_allocator_.allocate_contiguous_temp_vec4s(rows);
            if (!scopes_.empty()) {
                for (uint8_t r = 0; r < rows; ++r) {
                    scopes_.back().owned_vec4_temps.push_back(static_cast<uint8_t>(mbase + r * 4));
                }
            }
            SymbolBinding pbind{};
            pbind.name = param.name;
            pbind.type = param.type;
            pbind.reg_class = RegisterClass::Temporary;
            pbind.base_register = mbase;
            pbind.register_count = static_cast<uint16_t>(rows * 4u);
            pbind.allocated_words = static_cast<uint16_t>(rows * 4u);
            bind_scoped_symbol(param.name, pbind);
            if (param.is_in && evaluated_args[i].is_matrix) {
                const auto& marg = evaluated_args[i];
                for (uint8_t r = 0; r < rows && r < marg.mat_rows; ++r) {
                    ValueRef rval;
                    rval.reg_class = marg.reg_class;
                    rval.base_index = static_cast<uint16_t>(marg.base_index + r * marg.row_stride);
                    rval.count = cols;
                    emit_move(RegisterClass::Temporary, static_cast<uint16_t>(mbase + r * 4),
                              mask_for_count(cols), rval, "arg " + param.name + " row");
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

    // 0. VitaGL unpack/bit_cast pass-throughs (safe F32 identity when inputs are F32 floats)
    if ((fn == "vglUnpack" || fn == "bit_cast") && !call->arguments.empty()) {
        return generate_expr(call->arguments[0].get());
    }

    // 1. Texture Sampling (1D/2D/3D/CUBE with optional bias/lod/proj)
    if (fn == "tex2D" || fn == "tex2Dbias" || fn == "tex2Dlod" || fn == "tex2Dproj" ||
        fn == "tex1D" || fn == "tex1Dbias" || fn == "tex1Dlod" || fn == "tex1Dproj" ||
        fn == "tex3D" || fn == "tex3Dbias" || fn == "tex3Dlod" || fn == "tex3Dproj" ||
        fn == "texCUBE" || fn == "texCUBEbias" || fn == "texCUBElod" || fn == "texCUBEproj") {
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
            if (fn.rfind("tex1D", 0) == 0) dim = 0;
            else if (fn.rfind("tex3D", 0) == 0 || fn.rfind("texCUBE", 0) == 0) dim = 2;

            uint8_t lod_mode = 0;
            ValueRef lod_val;
            if (fn == "tex2Dbias" || fn == "tex1Dbias" || fn == "tex3Dbias" || fn == "texCUBEbias") {
                lod_mode = 1;
                lod_val = uv;
                lod_val.swiz = {uv.swiz[3], uv.swiz[3], uv.swiz[3], uv.swiz[3]};
                lod_val.count = 1;
                lod_val = materialize_to_temp(lod_val, 1, "tex bias");
            } else if (fn == "tex2Dlod" || fn == "tex1Dlod" || fn == "tex3Dlod" || fn == "texCUBElod") {
                lod_mode = 2;
                lod_val = uv;
                lod_val.swiz = {uv.swiz[3], uv.swiz[3], uv.swiz[3], uv.swiz[3]};
                lod_val.count = 1;
                lod_val = materialize_to_temp(lod_val, 1, "tex lod");
            } else if (fn == "tex2Dproj" || fn == "tex1Dproj" || fn == "tex3Dproj" || fn == "texCUBEproj") {
                uint8_t proj_lane = (uv.count >= 4) ? 3 : static_cast<uint8_t>(std::max<int>(0, static_cast<int>(uv.count) - 1));
                ValueRef w_sc = uv;
                w_sc.count = 1;
                w_sc.swiz = {uv.swiz[proj_lane], uv.swiz[proj_lane], uv.swiz[proj_lane], uv.swiz[proj_lane]};
                ValueRef w_rcp = emit_vcomp_vec(UsseOpcode::VRCP, w_sc, "texproj 1/w");
                ValueRef uv_coords = uv;
                uv_coords.count = static_cast<uint8_t>(std::max<int>(1, static_cast<int>(uv.count) - 1));
                uv = emit_binary_alu(frontend::BinaryOp::Mul, uv_coords, w_rcp, "texproj uv/w");
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
        ValueRef out = make_temp_vec4(3);
        push_scope();
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef b = generate_expr(call->arguments[1].get());
        ValueRef a_yzx = a; a_yzx.count = 3; a_yzx.swiz = {a.swiz[1], a.swiz[2], a.swiz[0], a.swiz[0]};
        ValueRef b_zxy = b; b_zxy.count = 3; b_zxy.swiz = {b.swiz[2], b.swiz[0], b.swiz[1], b.swiz[0]};
        ValueRef a_zxy = a; a_zxy.count = 3; a_zxy.swiz = {a.swiz[2], a.swiz[0], a.swiz[1], a.swiz[0]};
        ValueRef b_yzx = b; b_yzx.count = 3; b_yzx.swiz = {b.swiz[1], b.swiz[2], b.swiz[0], b.swiz[0]};
        ValueRef t0 = emit_binary_alu(frontend::BinaryOp::Mul, a_yzx, b_zxy, "cross L");
        ValueRef t1 = emit_binary_alu(frontend::BinaryOp::Mul, a_zxy, b_yzx, "cross R");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Sub, t0, t1, "cross");
        emit_move(RegisterClass::Temporary, out.base_index, 0b0111, res, "cross");
        pop_scope();
        return out;
    }

    if (fn == "length" && !call->arguments.empty()) {
        ValueRef out = make_temp_vec4(1);
        push_scope();
        ValueRef v = generate_expr(call->arguments[0].get());
        ValueRef dp = emit_dp(v, v, v.count, "length dot");
        emit_move(RegisterClass::Temporary, out.base_index, 0x1, emit_sqrt(dp), "length");
        pop_scope();
        out.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return out;
    }

    if (fn == "distance" && call->arguments.size() >= 2) {
        ValueRef out = make_temp_vec4(1);
        push_scope();
        ValueRef diff = emit_binary_alu(frontend::BinaryOp::Sub,
                                        generate_expr(call->arguments[0].get()),
                                        generate_expr(call->arguments[1].get()), "distance diff");
        emit_move(RegisterClass::Temporary, out.base_index, 0x1,
                  emit_sqrt(emit_dp(diff, diff, diff.count, "distance dot")), "distance");
        pop_scope();
        out.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
        return out;
    }

    if (fn == "normalize" && !call->arguments.empty()) {
        ValueRef out = make_temp_vec4(4);
        uint8_t n_cnt = 4;
        push_scope();
        ValueRef v = generate_expr(call->arguments[0].get());
        n_cnt = v.count;
        ValueRef dp = emit_dp(v, v, v.count, "normalize dot");
        ValueRef rsq = emit_vcomp_vec(UsseOpcode::VRSQ, dp, "normalize rsqrt");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Mul, v, rsq, "normalize scale");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(n_cnt), res, "normalize");
        pop_scope();
        out.count = n_cnt;
        return out;
    }

    if (fn == "reflect" && call->arguments.size() >= 2) {
        ValueRef out = make_temp_vec4(4);
        uint8_t cnt = 4;
        push_scope();
        ValueRef i_vec = generate_expr(call->arguments[0].get());
        ValueRef n_vec = generate_expr(call->arguments[1].get());
        cnt = std::max(i_vec.count, n_vec.count);
        ValueRef dp = emit_dp(n_vec, i_vec, cnt, "reflect dot");
        ValueRef k = emit_binary_alu(frontend::BinaryOp::Mul, dp, make_const_scalar(2.0f), "2*dot");
        ValueRef term = emit_binary_alu(frontend::BinaryOp::Mul, n_vec, k, "2*dot*N");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Sub, i_vec, term, "reflect");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), res, "reflect");
        pop_scope();
        out.count = cnt;
        return out;
    }

    if (fn == "refract" && call->arguments.size() >= 3) {
        ValueRef out = make_temp_vec4(4);
        uint8_t cnt = 4;
        push_scope();
        ValueRef i_vec = generate_expr(call->arguments[0].get());
        ValueRef n_vec = generate_expr(call->arguments[1].get());
        ValueRef eta = generate_expr(call->arguments[2].get());
        cnt = i_vec.count;
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
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), make_const_scalar(0.0f), "refract 0");
        size_t br_tir = emit_br(0, ExtPredicate::P0, "refract k<0");
        ValueRef term1 = emit_binary_alu(frontend::BinaryOp::Mul, eta, i_vec, "eta*I");
        ValueRef factor = emit_binary_alu(frontend::BinaryOp::Add,
                                          emit_binary_alu(frontend::BinaryOp::Mul, eta, ndoti),
                                          emit_sqrt(k), "refract factor");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Sub, term1,
                                       emit_binary_alu(frontend::BinaryOp::Mul, factor, n_vec), "refract res");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), res);
        patch_br(br_tir, instructions_.size());
        pop_scope();
        out.count = cnt;
        return out;
    }

    if (fn == "faceforward" && call->arguments.size() >= 3) {
        ValueRef out = make_temp_vec4(4);
        uint8_t cnt = 4;
        push_scope();
        ValueRef n = generate_expr(call->arguments[0].get());
        ValueRef i = generate_expr(call->arguments[1].get());
        ValueRef ng = generate_expr(call->arguments[2].get());
        cnt = n.count;
        ValueRef dp = emit_dp(ng, i, std::max(ng.count, i.count), "faceforward dot");
        emit_vtst_p0(dp, make_const_scalar(0.0f), VtstSignTest::LtOrLe, VtstZeroTest::ExcludeZero, "ng.i < 0");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), n);
        size_t br_neg = emit_br(0, ExtPredicate::P0, "faceforward keep");
        ValueRef neg_n = n;
        neg_n.is_negated = !neg_n.is_negated;
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), neg_n);
        patch_br(br_neg, instructions_.size());
        pop_scope();
        out.count = cnt;
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
        ValueRef dst = make_temp_vec4(n);
        push_scope();
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
        pop_scope();
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
        ValueRef dst = make_temp_vec4(n);
        push_scope();
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
        pop_scope();
        return dst;
    }

    // 5. Interpolation / Shaping / Unary Math
    if ((fn == "lerp" || fn == "mix") && call->arguments.size() >= 3) {
        ValueRef out = make_temp_vec4(4);
        uint8_t cnt = 4;
        push_scope();
        ValueRef a = generate_expr(call->arguments[0].get());
        ValueRef b = generate_expr(call->arguments[1].get());
        ValueRef t = generate_expr(call->arguments[2].get());
        cnt = std::max({a.count, b.count, t.count});
        ValueRef diff = emit_binary_alu(frontend::BinaryOp::Sub, b, a, "lerp (b - a)");
        ValueRef scaled = emit_binary_alu(frontend::BinaryOp::Mul, diff, t, "lerp (b - a) * t");
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Add, a, scaled, "lerp a + ...");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), res, "lerp");
        pop_scope();
        out.count = cnt;
        return out;
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
            push_scope();
            ValueRef ec = edge; ec.count = 1;
            if (edge.count > 1) ec.swiz = {edge.swiz[c], edge.swiz[c], edge.swiz[c], edge.swiz[c]};
            ValueRef xc = x; xc.count = 1;
            if (x.count > 1) xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
            ValueRef sc = emit_compare_to_float(frontend::BinaryOp::GreaterEqual, xc, ec);
            std::array<uint8_t, 4> map = {0, 0, 0, 0};
            emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, sc, "step");
            pop_scope();
        }
        return out;
    }

    if (fn == "smoothstep" && call->arguments.size() >= 3) {
        ValueRef out = make_temp_vec4(4);
        uint8_t cnt = 4;
        push_scope();
        ValueRef e0 = generate_expr(call->arguments[0].get());
        ValueRef e1 = generate_expr(call->arguments[1].get());
        ValueRef x = generate_expr(call->arguments[2].get());
        ValueRef t = emit_binary_alu(frontend::BinaryOp::Div,
                                     emit_binary_alu(frontend::BinaryOp::Sub, x, e0, "smoothstep num"),
                                     emit_binary_alu(frontend::BinaryOp::Sub, e1, e0, "smoothstep den"),
                                     "smoothstep t");
        cnt = t.count;
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
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Mul, t2, poly, "smoothstep");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(cnt), res, "smoothstep");
        pop_scope();
        out.count = cnt;
        return out;
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
            ValueRef out = make_temp_vec4(1);
            push_scope();
            ValueRef pos = emit_compare_to_float(frontend::BinaryOp::Greater, x, zero);
            ValueRef neg = emit_compare_to_float(frontend::BinaryOp::Less, x, zero);
            ValueRef s = emit_binary_alu(frontend::BinaryOp::Sub, pos, neg, "sign");
            emit_move(RegisterClass::Temporary, out.base_index, 0x1, s, "sign");
            pop_scope();
            out.swiz = {SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X};
            return out;
        }
        ValueRef out = make_temp_vec4(x.count);
        for (uint8_t c = 0; c < x.count; ++c) {
            push_scope();
            ValueRef xc = x; xc.count = 1;
            xc.swiz = {x.swiz[c], x.swiz[c], x.swiz[c], x.swiz[c]};
            ValueRef pos = emit_compare_to_float(frontend::BinaryOp::Greater, xc, zero);
            ValueRef neg = emit_compare_to_float(frontend::BinaryOp::Less, xc, zero);
            ValueRef s = emit_binary_alu(frontend::BinaryOp::Sub, pos, neg, "sign lane");
            std::array<uint8_t, 4> map = {0, 0, 0, 0};
            emit_move_mapped(RegisterClass::Temporary, out.base_index, static_cast<uint8_t>(1u << c), map, s);
            pop_scope();
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
        ValueRef out = make_temp_vec4(x.count);
        push_scope();
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Div, emit_sin_cos(x, false), emit_sin_cos(x, true), "tan");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(x.count), res, "tan");
        pop_scope();
        return out;
    }
    if (fn == "atan" && !call->arguments.empty()) {
        if (call->arguments.size() >= 2) {
            return emit_atan2(generate_expr(call->arguments[0].get()), generate_expr(call->arguments[1].get()));
        }
        return emit_atan(generate_expr(call->arguments[0].get()));
    }
    if (fn == "atan2" && call->arguments.size() >= 2) {
        return emit_atan2(generate_expr(call->arguments[0].get()), generate_expr(call->arguments[1].get()));
    }
    if (fn == "asin" && !call->arguments.empty()) return emit_asin(generate_expr(call->arguments[0].get()));
    if (fn == "acos" && !call->arguments.empty()) return emit_acos(generate_expr(call->arguments[0].get()));
    if (fn == "radians" && !call->arguments.empty()) {
        return emit_binary_alu(frontend::BinaryOp::Mul, generate_expr(call->arguments[0].get()),
                               make_const_scalar(0.01745329251f), "radians");
    }
    if (fn == "degrees" && !call->arguments.empty()) {
        return emit_binary_alu(frontend::BinaryOp::Mul, generate_expr(call->arguments[0].get()),
                               make_const_scalar(57.2957795131f), "degrees");
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
        ValueRef out = make_temp_vec4(a.count);
        push_scope();
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
        ValueRef res = emit_binary_alu(frontend::BinaryOp::Add, call_deriv(UsseOpcode::VDSX), call_deriv(UsseOpcode::VDSY), "fwidth");
        emit_move(RegisterClass::Temporary, out.base_index, mask_for_count(a.count), res, "fwidth");
        pop_scope();
        return out;
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
            kinst.short_pred = ShortPredicate::P0;
            kinst.comment = "clip kill";
            emit_inst(std::move(kinst));
        }
        return make_const_scalar(0.0f);
    }

    // 6. User-defined helper function lookup with overload resolution & inline expansion
    if (tu_) {
        std::vector<ValueRef> eval_args;
        eval_args.reserve(call->arguments.size());
        for (const auto& a : call->arguments) {
            eval_args.push_back(generate_expr(a.get()));
        }
        if (const auto* target_fn = resolve_user_function(fn, call, eval_args)) {
            return invoke_user_function(target_fn, call, std::move(eval_args));
        }
        return eval_args.empty() ? make_const_scalar(0.0f) : eval_args.back();
    }

    // Fallback: evaluate arguments cleanly
    ValueRef last;
    for (const auto& a : call->arguments) {
        last = generate_expr(a.get());
    }
    return last;
}

} // namespace open_shacc::backend
