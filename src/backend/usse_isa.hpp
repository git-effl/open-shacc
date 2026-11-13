/**
 * @file usse_isa.hpp
 * @brief Clean-room Universal Scalable Shader Engine (USSE) Instruction Set Architecture.
 *
 * Implemented strictly from public open-source architecture specifications, patents,
 * and open-source emulator decoders (Vita3K).
 * Contains no proprietary or reverse-engineered code.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef OPEN_SHACC_USSE_ISA_HPP
#define OPEN_SHACC_USSE_ISA_HPP

#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>

namespace open_shacc::backend {

// Clean-room USSE Opcodes
enum class UsseOpcode : uint8_t {
    NOP      = 0x00,
    VMOV     = 0x01,
    VADD     = 0x02,
    VSUB     = 0x03,
    VMUL     = 0x04,
    VMAD     = 0x05,
    VDP3     = 0x06,
    VDP4     = 0x07,
    VRCP     = 0x08,
    VRSQ     = 0x09,
    VMIN     = 0x0A,
    VMAX     = 0x0B,
    VFRC     = 0x0C,
    VDSX     = 0x0D,
    VDSY     = 0x0E,
    VPCK     = 0x0F,
    SMP      = 0x10,
    KILL     = 0x11,
    PHAS     = 0x1E,
    HALT     = 0x1F
};

enum class RegisterClass : uint8_t {
    Temporary      = 0, // 'r' / TEMP bank 0
    Output         = 1, // 'o' / OUTPUT bank 1
    InputAttr      = 2, // 'v' / PRIMATTR bank 2
    SecAttr        = 3, // 'sa' / SECATTR bank 3 (Uniforms/Constants)
    Constant       = 4, // Logical uniform scalar/vector (mapped to SECATTR bank 3 in hardware)
    TextureSampler = 5  // 's' / Texture sampler slot (mapped to SECATTR bank 3 in hardware)
};

inline uint64_t encode_bank(RegisterClass rc) {
    switch (rc) {
        case RegisterClass::Temporary:      return 0ULL;
        case RegisterClass::Output:         return 1ULL;
        case RegisterClass::InputAttr:      return 2ULL;
        case RegisterClass::SecAttr:
        case RegisterClass::Constant:
        case RegisterClass::TextureSampler: return 3ULL;
    }
    return 0ULL;
}

struct HardwareRegister {
    RegisterClass reg_class = RegisterClass::Temporary;
    uint8_t index = 0;       // Register offset
    uint8_t sub_index = 0;   // Vector component (0=x, 1=y, 2=z, 3=w)
    bool is_negated = false;
    bool is_absolute = false;

    [[nodiscard]] std::string to_string() const {
        std::ostringstream ss;
        if (is_negated) ss << "-";
        if (is_absolute) ss << "|";
        switch (reg_class) {
            case RegisterClass::Temporary:      ss << "r" << static_cast<int>(index); break;
            case RegisterClass::Output:         ss << "o" << static_cast<int>(index); break;
            case RegisterClass::InputAttr:      ss << "v" << static_cast<int>(index); break;
            case RegisterClass::SecAttr:        ss << "sa" << static_cast<int>(index); break;
            case RegisterClass::Constant:       ss << "c" << static_cast<int>(index); break;
            case RegisterClass::TextureSampler: ss << "s" << static_cast<int>(index); break;
        }
        static constexpr char kComponents[] = {'x', 'y', 'z', 'w'};
        if (sub_index < 4) {
            ss << "." << kComponents[sub_index];
        }
        if (is_absolute) ss << "|";
        return ss.str();
    }
};

/**
 * @brief 64-bit USSE Microcode Instruction Word representation.
 */
struct MicrocodeWord {
    uint64_t raw_word = 0;

    MicrocodeWord() = default;
    explicit MicrocodeWord(uint64_t val) : raw_word(val) {}

    static MicrocodeWord encode_phas(bool end = true) {
        // PHAS: "11111010s100eirc--matwwwppppppppbbnn--------xxxxxxoooooooddddddd"
        // End flag 'e' is bit 51
        uint64_t w = 0xFA40000000000000ULL;
        if (end) w |= (1ULL << 51);
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_vmov(
        HardwareRegister dst,
        HardwareRegister src0,
        uint8_t mask = 0b1111
    ) {
        // VMOV: "00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv"
        // op1 = 7 (bits 63..59), move_type = 0 (bits 47..46), move_data_type = 5 (F32, bits 42..40),
        // src1_swiz = 4 (XYZW, bits 38..35), dest_bank_sel = bits 33..32, src1_bank_sel = bits 31..30,
        // dest_mask = bits 27..24, dest_n = bits 23..18, src1_n = bits 11..6
        uint64_t w = (7ULL << 59);
        w |= (5ULL << 40); // F32 move data type
        w |= (4ULL << 35); // Identity XYZW swizzle

        uint64_t db = encode_bank(dst.reg_class);
        uint64_t sb1 = encode_bank(src0.reg_class);
        uint64_t dn = static_cast<uint64_t>(dst.index / 2) & 0x3FULL;
        uint64_t sn1 = static_cast<uint64_t>(src0.index / 2) & 0x3FULL;

        w |= (db << 32);
        w |= (sb1 << 30);
        w |= (static_cast<uint64_t>(mask & 0xF) << 24);
        w |= (dn << 18);
        w |= (sn1 << 6);
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_v32nmad(
        UsseOpcode op,
        HardwareRegister dst,
        HardwareRegister src1,
        HardwareRegister src2,
        uint8_t mask = 0b1111
    ) {
        // V32NMAD: "00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj"
        // op1 = 1 (bits 63..59), src2_swiz = 4 (bits 47..44), dest_mask = bits 42..39,
        // dest_bank_sel = bits 33..32, src1_bank_sel = bits 31..30, src2_bank_sel = bits 29..28,
        // dest_n = bits 27..22, src1_swiz = 0x20 (XYZW identity, bits 21..15), op2 = bits 14..12,
        // src1_n = bits 11..6, src2_n = bits 5..0
        uint64_t w = (1ULL << 59);

        uint8_t op2 = 1; // 0=MUL, 1=ADD, 2=FRC, 3=DSX, 4=DSY, 5=MIN, 6=MAX, 7=DP
        switch (op) {
            case UsseOpcode::VMUL: op2 = 0; break;
            case UsseOpcode::VADD:
            case UsseOpcode::VSUB: op2 = 1; break;
            case UsseOpcode::VFRC: op2 = 2; break;
            case UsseOpcode::VDSX: op2 = 3; break;
            case UsseOpcode::VDSY: op2 = 4; break;
            case UsseOpcode::VMIN: op2 = 5; break;
            case UsseOpcode::VMAX: op2 = 6; break;
            case UsseOpcode::VDP3:
            case UsseOpcode::VDP4: op2 = 7; break;
            default: op2 = 1; break;
        }

        w |= (4ULL << 44); // src2_swiz identity
        w |= (static_cast<uint64_t>(mask & 0xF) << 39);
        if (op == UsseOpcode::VSUB || src2.is_negated) {
            w |= (1ULL << 43); // src2 negate
        }
        if (src1.is_negated) {
            w |= (1ULL << 36); // src1 negate
        }

        uint64_t db  = encode_bank(dst.reg_class);
        uint64_t sb1 = encode_bank(src1.reg_class);
        uint64_t sb2 = encode_bank(src2.reg_class);
        uint64_t dn  = static_cast<uint64_t>(dst.index / 2) & 0x3FULL;
        uint64_t sn1 = static_cast<uint64_t>(src1.index / 2) & 0x3FULL;
        uint64_t sn2 = static_cast<uint64_t>(src2.index / 2) & 0x3FULL;

        w |= (db << 32);
        w |= (sb1 << 30);
        w |= (sb2 << 28);
        w |= (dn << 22);
        w |= (static_cast<uint64_t>(op2 & 7) << 12);
        w |= (sn1 << 6);
        w |= sn2;

        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_vpck(
        HardwareRegister dst,
        HardwareRegister src1,
        HardwareRegister src2,
        [[maybe_unused]] uint8_t mask = 0b1111
    ) {
        // VPCK F32 to F32 / F16
        uint64_t w = 0x40800D7EF0198002ULL;
        uint64_t dn = dst.index;
        w |= ((dn & 0x7FULL) << 30);
        uint64_t sn1 = static_cast<uint64_t>(src1.index) * 2ULL;
        w |= ((sn1 & 0x3FULL) << 12);
        uint64_t sn2 = static_cast<uint64_t>(src2.index) * 2ULL;
        w |= ((sn2 & 0x3FULL) << 1);
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_smp(
        HardwareRegister dst,
        HardwareRegister uv_src,
        uint16_t sampler_sa_offset
    ) {
        // SMP: 11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv
        uint64_t w = 0xE000000000000000ULL;
        uint64_t dn = dst.index;
        w |= ((dn & 0x7FULL) << 21);
        uint64_t uv = static_cast<uint64_t>(uv_src.index) * 2ULL;
        w |= ((uv & 0x7FULL) << 7);
        uint64_t samp = static_cast<uint64_t>(sampler_sa_offset / 2);
        w |= (samp & 0x7FULL);
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_kill() {
        // KILL: 11111001--11000000000pp0000001101111----------------------------
        return MicrocodeWord(0xF930000030F00000ULL);
    }
};

struct UsseInstruction {
    UsseOpcode opcode = UsseOpcode::NOP;
    HardwareRegister dst;
    HardwareRegister src0;
    HardwareRegister src1;
    HardwareRegister src2;
    uint8_t write_mask = 0b1111;
    std::string comment;

    [[nodiscard]] MicrocodeWord to_microcode() const {
        switch (opcode) {
            case UsseOpcode::PHAS: return MicrocodeWord::encode_phas(true);
            case UsseOpcode::HALT: return MicrocodeWord::encode_phas(true);
            case UsseOpcode::VMOV: return MicrocodeWord::encode_vmov(dst, src0, write_mask);
            case UsseOpcode::VPCK: return MicrocodeWord::encode_vpck(dst, src0, src1, write_mask);
            case UsseOpcode::SMP:  return MicrocodeWord::encode_smp(dst, src1, src0.index);
            case UsseOpcode::KILL: return MicrocodeWord::encode_kill();
            default:               return MicrocodeWord::encode_v32nmad(opcode, dst, src0, src1, write_mask);
        }
    }

    [[nodiscard]] std::string disassemble() const {
        std::ostringstream ss;
        switch (opcode) {
            case UsseOpcode::NOP:  ss << "NOP"; break;
            case UsseOpcode::VMOV: ss << "VMOV " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VADD: ss << "VADD " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VSUB: ss << "VSUB " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VMUL: ss << "VMUL " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VMAD: ss << "VMAD " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string() << ", " << src2.to_string(); break;
            case UsseOpcode::VDP3: ss << "VDP3 " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VDP4: ss << "VDP4 " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VRCP: ss << "VRCP " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VRSQ: ss << "VRSQ " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VMIN: ss << "VMIN " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VMAX: ss << "VMAX " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VFRC: ss << "VFRC " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VDSX: ss << "VDSX " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VDSY: ss << "VDSY " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VPCK: ss << "VPCK " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::SMP:  ss << "SMP  " << dst.to_string() << ", " << src1.to_string() << ", s" << static_cast<int>(src0.index); break;
            case UsseOpcode::KILL: ss << "KILL"; break;
            case UsseOpcode::PHAS: ss << "PHAS ; End phase"; break;
            case UsseOpcode::HALT: ss << "HALT ; Shader termination"; break;
        }
        if (!comment.empty()) {
            ss << "    // " << comment;
        }
        return ss.str();
    }
};

} // namespace open_shacc::backend

#endif // OPEN_SHACC_USSE_ISA_HPP
