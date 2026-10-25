/**
 * @file usse_isa.hpp
 * @brief Clean-room Universal Scalable Shader Engine (USSE) Instruction Set Architecture.
 *
 * Implemented strictly from public open-source architecture specifications and patents.
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

// Clean-room USSE ALU Opcodes
enum class UsseOpcode : uint8_t {
    NOP   = 0x00,
    MOV   = 0x01,
    FADD  = 0x02,
    FSUB  = 0x03,
    FMUL  = 0x04,
    FMAD  = 0x05, // Fused Multiply-Add: d = a * b + c
    DOT3  = 0x06, // 3-component dot product
    DOT4  = 0x07, // 4-component dot product
    RCP   = 0x08, // Reciprocal: 1.0 / x
    RSQ   = 0x09, // Reciprocal square root: 1.0 / sqrt(x)
    MIN   = 0x0A,
    MAX   = 0x0B,
    SLT   = 0x0C, // Set on Less Than
    SGE   = 0x0D, // Set on Greater or Equal
    PHAS  = 0x1E, // Phase change / End of shader execution
    HALT  = 0x1F
};

enum class RegisterClass : uint8_t {
    Temporary, // 'r' registers (r0 - r63)
    Constant,  // 'c' registers (c0 - c255)
    InputAttr, // 'v' / 'pa' registers (primary vertex attributes)
    Output     // 'o' / 'sa' registers (secondary attributes / outputs)
};

struct HardwareRegister {
    RegisterClass reg_class = RegisterClass::Temporary;
    uint8_t index = 0;       // Register number (0..255)
    uint8_t sub_index = 0;   // Vector component (0=x, 1=y, 2=z, 3=w)
    bool is_negated = false;
    bool is_absolute = false;

    [[nodiscard]] std::string to_string() const {
        std::ostringstream ss;
        if (is_negated) ss << "-";
        if (is_absolute) ss << "|";
        switch (reg_class) {
            case RegisterClass::Temporary: ss << "r" << static_cast<int>(index); break;
            case RegisterClass::Constant:  ss << "c" << static_cast<int>(index); break;
            case RegisterClass::InputAttr: ss << "v" << static_cast<int>(index); break;
            case RegisterClass::Output:    ss << "o" << static_cast<int>(index); break;
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
 * @brief 32-bit Microcode Instruction Word representation.
 *
 * Clean-room bit-packing format:
 * [31:26] Opcode (6 bits)
 * [25:24] Dest Reg Class (2 bits)
 * [23:18] Dest Reg Index (6 bits: 0..63)
 * [17:16] Dest Component Mask/Index (2 bits: x,y,z,w)
 * [15:10] Src0 Reg Index (6 bits)
 * [9:8]   Src0 Reg Class (2 bits)
 * [7:2]   Src1 Reg Index (6 bits)
 * [1:0]   Src1 Reg Class (2 bits)
 */
struct MicrocodeWord {
    uint32_t raw_word = 0;

    MicrocodeWord() = default;
    explicit MicrocodeWord(uint32_t val) : raw_word(val) {}

    static MicrocodeWord encode(
        UsseOpcode op,
        HardwareRegister dst,
        HardwareRegister src0,
        HardwareRegister src1
    ) {
        uint32_t w = 0;
        w |= (static_cast<uint32_t>(op) & 0x3F) << 26;
        w |= (static_cast<uint32_t>(dst.reg_class) & 0x03) << 24;
        w |= (static_cast<uint32_t>(dst.index) & 0x3F) << 18;
        w |= (static_cast<uint32_t>(dst.sub_index) & 0x03) << 16;
        w |= (static_cast<uint32_t>(src0.index) & 0x3F) << 10;
        w |= (static_cast<uint32_t>(src0.reg_class) & 0x03) << 8;
        w |= (static_cast<uint32_t>(src1.index) & 0x3F) << 2;
        w |= (static_cast<uint32_t>(src1.reg_class) & 0x03);
        return MicrocodeWord(w);
    }
};

struct UsseInstruction {
    UsseOpcode opcode = UsseOpcode::NOP;
    HardwareRegister dst;
    HardwareRegister src0;
    HardwareRegister src1;
    HardwareRegister src2; // Used for MAD (src0 * src1 + src2)
    std::string comment;

    [[nodiscard]] MicrocodeWord to_microcode() const {
        return MicrocodeWord::encode(opcode, dst, src0, src1);
    }

    [[nodiscard]] std::string disassemble() const {
        std::ostringstream ss;
        switch (opcode) {
            case UsseOpcode::NOP:  ss << "NOP"; break;
            case UsseOpcode::MOV:  ss << "MOV  " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::FADD: ss << "FADD " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::FSUB: ss << "FSUB " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::FMUL: ss << "FMUL " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::FMAD: ss << "FMAD " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string() << ", " << src2.to_string(); break;
            case UsseOpcode::DOT3: ss << "DOT3 " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::DOT4: ss << "DOT4 " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::RCP:  ss << "RCP  " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::RSQ:  ss << "RSQ  " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::MIN:  ss << "MIN  " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::MAX:  ss << "MAX  " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::SLT:  ss << "SLT  " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::SGE:  ss << "SGE  " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
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
