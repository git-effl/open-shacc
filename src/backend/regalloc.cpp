/**
 * @file regalloc.cpp
 * @brief Implementation of the USSE register allocator.
 */

#include "regalloc.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace open_shacc::backend {

namespace {

std::string upper_copy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string strip_sem_qualifiers(const std::string& s, bool* out_is_half = nullptr, bool* out_is_centroid = nullptr) {
    std::string up = upper_copy(s);
    auto strip = [&](const char* suf, size_t len, bool* flag) {
        if (up.size() > len && up.compare(up.size() - len, len, suf) == 0) {
            up.resize(up.size() - len);
            if (flag) *flag = true;
        }
    };
    strip("_CENTROID", 9, out_is_centroid);
    strip("_HALF", 5, out_is_half);
    strip("_CENTROID", 9, out_is_centroid);
    return up;
}

int vertex_output_sort_key(const std::string& sem_raw) {
    std::string up = strip_sem_qualifiers(sem_raw);
    if (up.find("POSITION") == 0 || up == "HPOS" || up == "WPOS") return 0;
    if (up == "COLOR" || up == "COLOR0" || up == "COL0" || up == "DIFFUSE" || up == "BCOL0") return 1;
    if (up == "COLOR1" || up == "COL1" || up == "SPECULAR" || up == "BCOL1") return 2;
    if (up.find("FOG") == 0) return 3;
    if (up.find("TEXCOORD") == 0) {
        int idx = std::atoi(up.c_str() + 8);
        return 4 + std::clamp(idx, 0, 9);
    }
    if (up == "PSIZE" || up == "POINTSIZE") return 14;
    if (up.find("CLP") == 0) {
        int idx = std::atoi(up.c_str() + 3);
        return 15 + std::clamp(idx, 0, 7);
    }
    if (up.find("CLIPDISTANCE") == 0) {
        int idx = std::atoi(up.c_str() + 12);
        return 15 + std::clamp(idx, 0, 7);
    }
    if (up.find("CLIP") == 0) {
        int idx = std::atoi(up.c_str() + 4);
        return 15 + std::clamp(idx, 0, 7);
    }
    return 100;
}

bool try_map_direct_varying_hw_semantic(const std::string& requested_sem, uint32_t& used_texcoords, std::string& out_hw) {
    bool is_half = false;
    bool is_centroid = false;
    std::string up = strip_sem_qualifiers(requested_sem, &is_half, &is_centroid);
    std::string suffix = is_half ? "_HALF" : "";

    if (up.find("TEXCOORD") == 0) {
        int idx = std::clamp(std::atoi(up.c_str() + 8), 0, 9);
        used_texcoords |= (1u << static_cast<uint32_t>(idx));
        out_hw = "TEXCOORD" + std::to_string(idx) + suffix;
        return true;
    }
    if (up == "COLOR" || up == "COLOR0" || up == "COL0" || up == "DIFFUSE" || up == "BCOL0") {
        out_hw = "COLOR0" + suffix;
        return true;
    }
    if (up == "COLOR1" || up == "COL1" || up == "SPECULAR" || up == "BCOL1") {
        out_hw = "COLOR1" + suffix;
        return true;
    }
    if (up.find("FOG") == 0) {
        out_hw = "FOG" + suffix;
        return true;
    }
    if (up == "WPOS" || up == "VPOS" || up == "POSITION" || up == "HPOS") {
        out_hw = up;
        return true;
    }
    if (up == "SPRITECOORD" || up == "POINTCOORD") {
        out_hw = up + suffix;
        return true;
    }
    if (up == "PSIZE" || up == "POINTSIZE" || up.find("CLP") == 0 || up.find("CLIP") == 0) {
        out_hw = up;
        return true;
    }
    return false;
}

std::string alloc_next_texcoord_hw_semantic(const std::string& requested_sem, uint32_t& used_texcoords) {
    bool is_half = false;
    bool is_centroid = false;
    strip_sem_qualifiers(requested_sem, &is_half, &is_centroid);
    std::string suffix = is_half ? "_HALF" : "";
    for (int idx = 0; idx < 10; ++idx) {
        if ((used_texcoords & (1u << static_cast<uint32_t>(idx))) == 0) {
            used_texcoords |= (1u << static_cast<uint32_t>(idx));
            return "TEXCOORD" + std::to_string(idx) + suffix;
        }
    }
    return "TEXCOORD9" + suffix;
}

} // namespace

RegisterAllocator::RegisterAllocator() {
    reset();
}

void RegisterAllocator::reset() {
    peak_temporary_ = 0;
    next_constant_ = 0;
    next_input_ = 0;
    next_output_ = 0;
    next_sampler_ = 0;
    used_frag_texcoords_ = 0;
    temp_overflow_ = false;
    std::fill(std::begin(temp_pairs_used_), std::end(temp_pairs_used_), false);
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
        // sceGxmSetUniformDataF packs matrix rows tightly (float2x2=4, float3x3=9, float4x4=16 words)
        uint16_t row_stride = static_cast<uint16_t>(cols);
        words_per_elem = static_cast<uint16_t>(rows * row_stride);
        if (cols >= 3 && (next_constant_ % 4 != 0)) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 3) & ~3u);
        } else if (cols == 2 && (next_constant_ % 2 != 0)) {
            next_constant_ = static_cast<uint16_t>((next_constant_ + 1) & ~1u);
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
    uint16_t words_needed = static_cast<uint16_t>(words_per_elem * array_elems);

    // Samplers occupy sa0..(next_sampler_*4-1), uniforms occupy sa(uniform_sa_base)..; cap total SA space at 1024 words
    if (static_cast<uint32_t>(get_uniform_sa_base()) + next_constant_ + words_needed > 1024u) {
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
    if (static_cast<uint32_t>(next_sampler_ + count) * 4u + next_constant_ > 1024u) {
        return false;
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
    std::string direct;
    if (try_map_direct_varying_hw_semantic(requested_sem, used_frag_texcoords_, direct)) {
        return direct;
    }
    return alloc_next_texcoord_hw_semantic(requested_sem, used_frag_texcoords_);
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

    // Pass 1: assign direct SGX semantics and reserve explicit TEXCOORD0..9 slots first
    uint32_t used_vp_texcoords = 0;
    std::vector<size_t> custom_indices;
    for (size_t i = 0; i < outputs_.size(); ++i) {
        std::string hw;
        if (try_map_direct_varying_hw_semantic(outputs_[i].semantic, used_vp_texcoords, hw)) {
            outputs_[i].hw_semantic = hw;
        } else {
            custom_indices.push_back(i);
        }
    }

    // Pass 2: assign deterministic non-colliding TEXCOORD slots to custom varyings (NORMAL, TANGENT, etc.)
    std::stable_sort(custom_indices.begin(), custom_indices.end(), [&](size_t a, size_t b) {
        return strip_sem_qualifiers(outputs_[a].semantic) < strip_sem_qualifiers(outputs_[b].semantic);
    });
    for (size_t idx : custom_indices) {
        outputs_[idx].hw_semantic = alloc_next_texcoord_hw_semantic(outputs_[idx].semantic, used_vp_texcoords);
    }

    std::stable_sort(outputs_.begin(), outputs_.end(), [](const SymbolBinding& a, const SymbolBinding& b) {
        return vertex_output_sort_key(a.hw_semantic) < vertex_output_sort_key(b.hw_semantic);
    });

    uint16_t cur_out = 0;
    for (auto& out : outputs_) {
        std::string up = strip_sem_qualifiers(out.hw_semantic);
        uint16_t stride = 4u;
        if (up == "POSITION" || up == "HPOS" || up == "COLOR" || up == "COLOR0" || up == "COL0" ||
            up == "COLOR1" || up == "COL1" || up == "DIFFUSE" || up == "SPECULAR") {
            stride = 4u;
        } else if (up.find("FOG") == 0) {
            stride = 2u;
        } else if (up.find("TEXCOORD") == 0) {
            size_t comp = std::clamp<size_t>(out.type.component_count(), 2, 4);
            stride = static_cast<uint16_t>(comp);
        } else if (up == "PSIZE" || up == "POINTSIZE") {
            stride = 1u;
        } else if (up.find("CLP") == 0 || up.find("CLIP") == 0) {
            uint16_t arr = static_cast<uint16_t>(out.type.array_size > 1 ? out.type.array_size : 1u);
            stride = arr;
        }

        out.base_register = cur_out;
        out.allocated_words = stride;
        cur_out = static_cast<uint16_t>(cur_out + stride);
        // Update matching symbol table entries and aliases while keeping declared out.semantic intact
        for (auto& [k, sym] : symbol_table_) {
            if (sym.reg_class == RegisterClass::Output &&
                (k == out.name || (k.size() > out.name.size() && k.rfind("." + out.name) == k.size() - out.name.size() - 1))) {
                sym.base_register = out.base_register;
                sym.allocated_words = out.allocated_words;
                sym.hw_semantic = out.hw_semantic;
            }
        }
    }
    next_output_ = static_cast<uint8_t>(cur_out);
}

void RegisterAllocator::finalize_fragment_varyings() {
    if (inputs_.empty()) return;

    used_frag_texcoords_ = 0;
    std::vector<size_t> custom_indices;
    for (size_t i = 0; i < inputs_.size(); ++i) {
        std::string hw;
        if (try_map_direct_varying_hw_semantic(inputs_[i].semantic, used_frag_texcoords_, hw)) {
            inputs_[i].hw_semantic = hw;
        } else {
            custom_indices.push_back(i);
        }
    }

    std::stable_sort(custom_indices.begin(), custom_indices.end(), [&](size_t a, size_t b) {
        return strip_sem_qualifiers(inputs_[a].semantic) < strip_sem_qualifiers(inputs_[b].semantic);
    });
    for (size_t idx : custom_indices) {
        inputs_[idx].hw_semantic = alloc_next_texcoord_hw_semantic(inputs_[idx].semantic, used_frag_texcoords_);
    }

    for (const auto& in : inputs_) {
        for (auto& [k, sym] : symbol_table_) {
            if (sym.reg_class == RegisterClass::InputAttr && sym.base_register == in.base_register) {
                sym.hw_semantic = in.hw_semantic;
            }
        }
    }
}

void RegisterAllocator::alias_symbol(const std::string& alias_name, const SymbolBinding& binding) {
    if (!alias_name.empty()) {
        symbol_table_[alias_name] = binding;
    }
}

void RegisterAllocator::erase_symbol(const std::string& name) {
    symbol_table_.erase(name);
}

uint8_t RegisterAllocator::allocate_temp_pairs(uint8_t pair_count) {
    if (pair_count == 0) pair_count = 1;
    if (pair_count > kMaxTempPairs) {
        temp_overflow_ = true;
        return 0;
    }

    for (uint8_t p = 0; p <= static_cast<uint8_t>(kMaxTempPairs - pair_count); ++p) {
        bool free_run = true;
        for (uint8_t k = 0; k < pair_count; ++k) {
            if (temp_pairs_used_[p + k]) {
                free_run = false;
                p = static_cast<uint8_t>(p + k);
                break;
            }
        }
        if (free_run) {
            for (uint8_t k = 0; k < pair_count; ++k) {
                temp_pairs_used_[p + k] = true;
            }
            uint8_t end_word = static_cast<uint8_t>((p + pair_count) * 2u);
            if (end_word > peak_temporary_) {
                peak_temporary_ = end_word;
            }
            return static_cast<uint8_t>(p * 2u);
        }
    }

    temp_overflow_ = true;
    return 0;
}

void RegisterAllocator::free_temp_pairs(uint8_t base_reg, uint8_t pair_count) {
    if ((base_reg & 1u) != 0 || pair_count == 0) {
        return;
    }
    uint8_t p = static_cast<uint8_t>(base_reg / 2u);
    if (p >= kMaxTempPairs) {
        return;
    }
    for (uint8_t k = 0; k < pair_count && (p + k) < kMaxTempPairs; ++k) {
        temp_pairs_used_[p + k] = false;
    }
}

uint8_t RegisterAllocator::allocate_temp_pair() {
    return allocate_temp_pairs(1);
}

uint8_t RegisterAllocator::allocate_temp_vec4() {
    return allocate_temp_pairs(2);
}

uint8_t RegisterAllocator::allocate_contiguous_temp_vec4s(uint8_t vec4_count) {
    if (vec4_count <= 1) {
        return allocate_temp_vec4();
    }
    return allocate_temp_pairs(static_cast<uint8_t>(vec4_count * 2u));
}

void RegisterAllocator::free_temp_pair(uint8_t base_reg) {
    free_temp_pairs(base_reg, 1);
}

void RegisterAllocator::free_temp_vec4(uint8_t base_reg) {
    free_temp_pairs(base_reg, 2);
}

HardwareRegister RegisterAllocator::allocate_temporary(uint8_t sub_index) {
    HardwareRegister reg;
    reg.reg_class = RegisterClass::Temporary;
    reg.index = allocate_temp_pair();
    reg.sub_index = static_cast<uint8_t>(sub_index % 2u);
    return reg;
}

std::vector<HardwareRegister> RegisterAllocator::allocate_temporary_vector(uint8_t component_count) {
    std::vector<HardwareRegister> regs;
    regs.reserve(component_count);
    uint8_t base = (component_count <= 2) ? allocate_temp_pair() : allocate_temp_vec4();
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
    free_temp_pairs(static_cast<uint8_t>(reg_index & ~1u), 1);
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
