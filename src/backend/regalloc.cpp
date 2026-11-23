/**
 * @file regalloc.cpp
 * @brief Implementation of the USSE register allocator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "regalloc.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace open_shacc::backend {

namespace {

int vertex_output_sort_key(const std::string& sem_raw) {
    std::string up;
    up.reserve(sem_raw.size());
    for (char c : sem_raw) {
        up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (up.find("POSITION") == 0 || up == "HPOS" || up == "WPOS") return 0;
    if (up == "COLOR" || up == "COLOR0" || up == "COL0" || up == "DIFFUSE") return 1;
    if (up == "COLOR1" || up == "COL1" || up == "SPECULAR") return 2;
    if (up.find("FOG") == 0) return 3;
    if (up.find("TEXCOORD") == 0) {
        int idx = std::atoi(up.c_str() + 8);
        return 4 + std::clamp(idx, 0, 9);
    }
    if (up == "PSIZE" || up == "POINTSIZE") return 14;
    return 100;
}

std::string upper_copy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

RegisterAllocator::RegisterAllocator() {
    reset();
}

void RegisterAllocator::reset() {
    next_temporary_ = 0;
    peak_temporary_ = 0;
    next_constant_ = 0;
    next_input_ = 0;
    next_output_ = 0;
    next_sampler_ = 0;
    used_frag_texcoords_ = 0;
    free_vec4_slots_.clear();
    symbol_table_.clear();
    uniforms_.clear();
    samplers_.clear();
    inputs_.clear();
    outputs_.clear();
}

bool RegisterAllocator::allocate_uniform(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    if (type.is_sampler()) {
        return allocate_sampler(name, type, out_binding);
    }

    uint16_t words_per_elem = 1;
    if (type.is_matrix()) {
        size_t rows = type.matrix_rows();
        size_t cols = type.matrix_cols();
        // Align matrix rows to 4-word stride so row indexing m[i] stays 4-word/even aligned
        uint16_t row_stride = static_cast<uint16_t>((cols == 4 || cols == 3) ? 4 : ((cols + 1) & ~1u));
        words_per_elem = static_cast<uint16_t>(rows * row_stride);
        if (next_constant_ % 4 != 0) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 3) & ~3u);
        }
    } else {
        words_per_elem = static_cast<uint16_t>(type.component_count());
        if (words_per_elem == 0) words_per_elem = 1;
        if (words_per_elem >= 3 && (next_constant_ % 4 != 0)) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 3) & ~3u);
        } else if (words_per_elem == 2 && (next_constant_ % 2 != 0)) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 1) & ~1u);
        }
    }

    uint16_t array_elems = static_cast<uint16_t>(type.array_size > 1 ? type.array_size : 1);
    if (array_elems > 1 && (words_per_elem % 4 != 0)) {
        // Vector/scalar arrays in USSE read cleanly with 4-word or 2-word stride; align to 4 for index addressing
        words_per_elem = static_cast<uint16_t>((words_per_elem + 3u) & ~3u);
        if (next_constant_ % 4 != 0) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 3u) & ~3u);
        }
    }
    uint16_t words_needed = static_cast<uint16_t>(words_per_elem * array_elems);

    // Keep below SA register limit while leaving headroom for sampler descriptors in container 19
    if (static_cast<uint32_t>(next_constant_) + words_needed > 960u) {
        return false;
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::Constant;
    out_binding.base_register = next_constant_;
    out_binding.register_count = words_needed;
    out_binding.allocated_words = words_needed;
    out_binding.semantic = semantic;
    out_binding.hw_semantic = semantic;

    next_constant_ = static_cast<uint16_t>(next_constant_ + words_needed);

    symbol_table_[name] = out_binding;
    uniforms_.push_back(out_binding);
    return true;
}

bool RegisterAllocator::allocate_sampler(
    const std::string& name,
    const frontend::DataType& type,
    SymbolBinding& out_binding
) {
    uint8_t count = static_cast<uint8_t>(type.array_size > 1 ? type.array_size : 1);
    if (static_cast<int>(next_sampler_) + count > 16) {
        return false; // Exceeded texture sampler slots (s0..s15)
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::TextureSampler;
    out_binding.base_register = next_sampler_;
    out_binding.register_count = count;
    out_binding.allocated_words = count;
    out_binding.semantic = "";
    out_binding.hw_semantic = "";

    next_sampler_ = static_cast<uint8_t>(next_sampler_ + count);

    symbol_table_[name] = out_binding;
    samplers_.push_back(out_binding);
    return true;
}

std::string RegisterAllocator::assign_fragment_hw_semantic(const std::string& requested_sem) {
    std::string up = upper_copy(requested_sem);
    if (up.find("TEXCOORD") == 0) {
        int idx = std::clamp(std::atoi(up.c_str() + 8), 0, 9);
        used_frag_texcoords_ |= (1u << static_cast<uint32_t>(idx));
        return "TEXCOORD" + std::to_string(idx);
    }
    if (up == "COLOR" || up == "COLOR0" || up == "COL0" || up == "COLOR1" || up == "COL1" ||
        up.find("FOG") == 0 || up == "WPOS" || up == "VPOS" || up == "POSITION" || up == "SPRITECOORD") {
        return up;
    }
    // Map unrecognized varying semantics (e.g. NORMAL, TANGENT, empty) to the next unused TEXCOORD slot
    for (int idx = 0; idx < 10; ++idx) {
        if ((used_frag_texcoords_ & (1u << static_cast<uint32_t>(idx))) == 0) {
            used_frag_texcoords_ |= (1u << static_cast<uint32_t>(idx));
            return "TEXCOORD" + std::to_string(idx);
        }
    }
    return "TEXCOORD9";
}

bool RegisterAllocator::allocate_input(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding,
    bool is_fragment
) {
    uint16_t comp = static_cast<uint16_t>(type.component_count());
    if (comp == 0) comp = 1;
    uint16_t array_elems = static_cast<uint16_t>(type.array_size > 1 ? type.array_size : 1);
    uint16_t count = static_cast<uint16_t>(comp * array_elems);

    // Align PA offsets:
    // - Vertex attributes allocate in 4-word vec4 slots (matching GXM attribute fetcher)
    // - Fragment varyings allocate in even-aligned (2 or 4 word) PA slots so USSE double-reg reads always work
    uint16_t word_footprint = is_fragment
        ? static_cast<uint16_t>(comp <= 2 ? 2 : 4)
        : static_cast<uint16_t>(((count + 3u) / 4u) * 4u);

    if (static_cast<uint16_t>(next_input_) + word_footprint > 64u) {
        return false;
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::InputAttr;
    out_binding.base_register = next_input_;
    out_binding.register_count = count;
    out_binding.allocated_words = word_footprint;
    out_binding.semantic = semantic;
    out_binding.hw_semantic = is_fragment ? assign_fragment_hw_semantic(semantic) : semantic;

    next_input_ = static_cast<uint8_t>(next_input_ + word_footprint);

    symbol_table_[name] = out_binding;
    inputs_.push_back(out_binding);
    return true;
}

bool RegisterAllocator::allocate_output(
    const std::string& name,
    const frontend::DataType& type,
    const std::string& semantic,
    SymbolBinding& out_binding
) {
    uint16_t comp = static_cast<uint16_t>(type.component_count());
    if (comp == 0) comp = 1;
    uint16_t array_elems = static_cast<uint16_t>(type.array_size > 1 ? type.array_size : 1);
    uint16_t count = static_cast<uint16_t>(comp * array_elems);

    // Ensure outputs are 4-word aligned (or 1 word for PSIZE/DEPTH) for clean USSE double-reg writes
    uint16_t footprint = static_cast<uint16_t>(((count + 3u) / 4u) * 4u);
    if (static_cast<uint16_t>(next_output_) + footprint > 64u) {
        return false;
    }

    out_binding.name = name;
    out_binding.type = type;
    out_binding.reg_class = RegisterClass::Output;
    out_binding.base_register = next_output_;
    out_binding.register_count = count;
    out_binding.allocated_words = footprint;
    out_binding.semantic = semantic;
    out_binding.hw_semantic = semantic;

    next_output_ = static_cast<uint8_t>(next_output_ + footprint);

    symbol_table_[name] = out_binding;
    outputs_.push_back(out_binding);
    return true;
}

void RegisterAllocator::finalize_canonical_vertex_outputs() {
    if (outputs_.empty()) return;

    std::stable_sort(outputs_.begin(), outputs_.end(), [](const SymbolBinding& a, const SymbolBinding& b) {
        return vertex_output_sort_key(a.semantic) < vertex_output_sort_key(b.semantic);
    });

    uint16_t cur_out = 0;
    for (auto& out : outputs_) {
        std::string up = upper_copy(out.semantic);
        uint16_t hw_words = (up == "PSIZE" || up == "POINTSIZE") ? 2u : 4u; // Keep even-aligned for USSE
        out.base_register = cur_out;
        out.allocated_words = hw_words;
        cur_out = static_cast<uint16_t>(cur_out + ((up == "PSIZE" || up == "POINTSIZE") ? 1u : 4u));
        // Update matching symbol table entries and aliases
        for (auto& [k, sym] : symbol_table_) {
            if (sym.reg_class == RegisterClass::Output &&
                (k == out.name || (k.size() > out.name.size() && k.rfind("." + out.name) == k.size() - out.name.size() - 1))) {
                sym.base_register = out.base_register;
                sym.allocated_words = out.allocated_words;
            }
        }
    }
    next_output_ = static_cast<uint8_t>(cur_out);
}

void RegisterAllocator::alias_symbol(const std::string& alias_name, const SymbolBinding& binding) {
    if (!alias_name.empty()) {
        symbol_table_[alias_name] = binding;
    }
}

void RegisterAllocator::erase_symbol(const std::string& name) {
    symbol_table_.erase(name);
}

uint8_t RegisterAllocator::allocate_temp_vec4() {
    if (!free_vec4_slots_.empty()) {
        uint8_t reg = free_vec4_slots_.back();
        free_vec4_slots_.pop_back();
        return reg;
    }
    // Keep strictly below 120 (SGX FPINTERNAL threshold for doubled F32 addressing)
    if (next_temporary_ + 4u > 120u) {
        // Fallback: reuse last safe slot rather than clobbering FPINTERNAL
        return 112;
    }
    uint8_t base = next_temporary_;
    next_temporary_ = static_cast<uint8_t>(next_temporary_ + 4u);
    if (next_temporary_ > peak_temporary_) {
        peak_temporary_ = next_temporary_;
    }
    return base;
}

uint8_t RegisterAllocator::allocate_contiguous_temp_vec4s(uint8_t vec4_count) {
    if (vec4_count <= 1) {
        return allocate_temp_vec4();
    }
    uint16_t words = static_cast<uint16_t>(vec4_count) * 4u;
    if (static_cast<uint16_t>(next_temporary_) + words > 120u) {
        return 0;
    }
    uint8_t base = next_temporary_;
    next_temporary_ = static_cast<uint8_t>(next_temporary_ + words);
    if (next_temporary_ > peak_temporary_) {
        peak_temporary_ = next_temporary_;
    }
    return base;
}

void RegisterAllocator::free_temp_vec4(uint8_t base_reg) {
    if (base_reg < peak_temporary_ && (base_reg % 4u == 0)) {
        if (std::find(free_vec4_slots_.begin(), free_vec4_slots_.end(), base_reg) == free_vec4_slots_.end()) {
            free_vec4_slots_.push_back(base_reg);
        }
    }
}

HardwareRegister RegisterAllocator::allocate_temporary(uint8_t sub_index) {
    HardwareRegister reg;
    reg.reg_class = RegisterClass::Temporary;
    reg.index = allocate_temp_vec4();
    reg.sub_index = static_cast<uint8_t>(sub_index % 4u);
    return reg;
}

std::vector<HardwareRegister> RegisterAllocator::allocate_temporary_vector(uint8_t component_count) {
    std::vector<HardwareRegister> regs;
    regs.reserve(component_count);
    uint8_t base = allocate_temp_vec4();
    for (uint8_t i = 0; i < component_count; ++i) {
        HardwareRegister r;
        r.reg_class = RegisterClass::Temporary;
        r.index = base;
        r.sub_index = static_cast<uint8_t>(i % 4u);
        regs.push_back(r);
    }
    return regs;
}

void RegisterAllocator::free_temporary(uint8_t reg_index) {
    free_temp_vec4(static_cast<uint8_t>(reg_index & ~3u));
}

const SymbolBinding* RegisterAllocator::find_symbol(const std::string& name) const {
    auto it = symbol_table_.find(name);
    if (it != symbol_table_.end()) {
        return &it->second;
    }
    return nullptr;
}

SymbolBinding* RegisterAllocator::find_symbol_mut(const std::string& name) {
    auto it = symbol_table_.find(name);
    if (it != symbol_table_.end()) {
        return &it->second;
    }
    return nullptr;
}

} // namespace open_shacc::backend
