/**
 * @file usse_isa.hpp
 * @brief Clean-room Universal Scalable Shader Engine (USSE) Instruction Set Architecture.
 *
 * Implemented strictly from public open-source architecture specifications, patents,
 * and open-source emulator decoders (Vita3K).
 * Contains no proprietary or reverse-engineered code.
 *
 * SPDX-License-Identifier: MIT
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
    Temporary = 0, // 'r' / TEMP bank 0
    Output    = 1, // 'o' / OUTPUT bank 1
    InputAttr = 2, // 'v' / PRIMATTR bank 2
    SecAttr   = 3, // 'sa' / SECATTR bank 3 (Uniforms/Constants)
    Constant  = 4  // 'c' / FPCONSTANT bank
};

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
            case RegisterClass::Temporary: ss << "r" << static_cast<int>(index); break;
            case RegisterClass::Output:    ss << "o" << static_cast<int>(index); break;
            case RegisterClass::InputAttr: ss << "v" << static_cast<int>(index); break;
            case RegisterClass::SecAttr:   ss << "sa" << static_cast<int>(index); break;
            case RegisterClass::Constant:  ss << "c" << static_cast<int>(index); break;
        }
        static const char kComponents[] = {'x', 'y', 'z', 'w'};
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
        // PHAS bit pattern: 11111010s100e...
        // 0xFA00000000000000ULL
        uint64_t w = 0xFA04000000000000ULL;
        if (end) w |= (1ULL << 55); // end bit
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_vmov(
        HardwareRegister dst,
        HardwareRegister src0,
        uint8_t mask = 0b1111
    ) {
        // 00111 = VMOV op1
        uint64_t w = 0x1C00000000000000ULL;
        // dest_bank (0=TEMP, 1=OUTPUT, 2=PRIMATTR, 3=SECATTR)
        uint64_t db = static_cast<uint64_t>(dst.reg_class) & 3;
        w |= (db << 42);
        w |= (static_cast<uint64_t>(mask & 0xF) << 36);
        // Double reg indexing for float
        uint64_t dn = dst.index / 2;
        w |= ((dn & 0x3F) << 30);
        uint64_t sn0 = src0.index / 2;
        w |= ((sn0 & 0x3F) << 24);
        uint64_t sb0 = static_cast<uint64_t>(src0.reg_class) & 3;
        w |= ((sb0 & 3) << 18);
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_v32nmad(
        UsseOpcode op,
        HardwareRegister dst,
        HardwareRegister src1,
        HardwareRegister src2,
        uint8_t mask = 0b1111
    ) {
        // 00001 = V32NMAD op1 (32-bit float vector operation)
        // Bit 59 = 1 for 32-bit float
        uint64_t w = 0x0800000000000000ULL | (1ULL << 59);
        uint8_t op2 = 1; // 0=MUL, 1=ADD, 2=FRC, 3=DSX, 4=DSY, 5=MIN, 6=MAX, 7=DP
        switch (op) {
            case UsseOpcode::VMUL: op2 = 0; break;
            case UsseOpcode::VADD: op2 = 1; break;
            case UsseOpcode::VFRC: op2 = 2; break;
            case UsseOpcode::VDSX: op2 = 3; break;
            case UsseOpcode::VDSY: op2 = 4; break;
            case UsseOpcode::VMIN: op2 = 5; break;
            case UsseOpcode::VMAX: op2 = 6; break;
            case UsseOpcode::VDP3:
            case UsseOpcode::VDP4: op2 = 7; break;
            default: op2 = 1; break;
        }

        w |= (static_cast<uint64_t>(op2 & 7) << 12);
        w |= (static_cast<uint64_t>(mask & 0xF) << 36);

        uint64_t db = static_cast<uint64_t>(dst.reg_class) & 3;
        w |= (db << 20);
        uint64_t dn = dst.index / 2;
        w |= ((dn & 0x3F) << 24);

        uint64_t sb1 = static_cast<uint64_t>(src1.reg_class) & 3;
        w |= (sb1 << 18);
        uint64_t sn1 = src1.index / 2;
        w |= ((sn1 & 0x3F) << 6);

        uint64_t sb2 = static_cast<uint64_t>(src2.reg_class) & 3;
        w |= (sb2 << 16);
        uint64_t sn2 = src2.index / 2;
        w |= (sn2 & 0x3F);

        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_vpck(
        HardwareRegister dst,
        HardwareRegister src1,
        HardwareRegister src2,
        uint8_t mask = 0b1111
    ) {
        // VPCK F32 to F32 / F16: "01000pppsnuydercaaaaffftttmmmmbbkkllgggggggoohiijjqqqqqqvwwwwwwx"
        // 01000 = VPCK
        uint64_t w = 0x20800d7ef0198002ULL;
        uint64_t dn = dst.index; // VPCK does not double dest
        w |= ((dn & 0x7F) << 30);
        uint64_t sn1 = src1.index * 2;
        w |= ((sn1 & 0x3F) << 12);
        uint64_t sn2 = src2.index * 2;
        w |= ((sn2 & 0x3F) << 1);
        return MicrocodeWord(w);
    }

    static MicrocodeWord encode_smp(
        HardwareRegister dst,
        HardwareRegister uv_src,
        uint16_t sampler_sa_offset
    ) {
        // SMP: "11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv"
        // 11100 = SMP (0xE000000000000000ULL)
        uint64_t w = 0xE000000000000000ULL;
        uint64_t dn = dst.index; // dest TEMP
        w |= ((dn & 0x7F) << 21);
        uint64_t uv = uv_src.index * 2; // uv src
        w |= ((uv & 0x7F) << 7);
        uint64_t samp = sampler_sa_offset / 2; // SECATTR bank 3
        w |= (samp & 0x7F);
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
            case UsseOpcode::VMOV: return MicrocodeWord::encode_vmov(dst, src0, write_mask);
            case UsseOpcode::VPCK: return MicrocodeWord::encode_vpck(dst, src0, src1, write_mask);
            case UsseOpcode::SMP:  return MicrocodeWord::encode_smp(dst, src0, src1.index);
            case UsseOpcode::KILL: return MicrocodeWord::encode_kill();
            case UsseOpcode::PHAS + 1: return MicrocodeWord::encode_phas(true);
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
            case UsseOpcode::VPCK: ss << "VPCK " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::SMP:  ss << "SMP  " << dst.to_string() << ", " << src0.to_string() << ", sa" << static_cast<int>(src1.index); break;
            case UsseOpcode::KILL: ss << "KILL"; break;
            case UsseOpcode::PHAS: ss << "PHAS ; Phase switch"; break;
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
