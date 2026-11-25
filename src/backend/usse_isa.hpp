/**
 * @file usse_isa.hpp
 * @brief Clean-room Universal Scalable Shader Engine (USSE) Instruction Set Architecture.
 *
 * Implemented strictly from public open-source PowerVR SGX / USSE architecture
 * specifications and open-source VitaSDK public headers.
 * Every 64-bit instruction word is constructed from named bitfields with zero
 * opaque binary constants.
 */

#ifndef OPEN_SHACC_USSE_ISA_HPP
#define OPEN_SHACC_USSE_ISA_HPP

#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>
#include <array>

namespace open_shacc::backend {

// -----------------------------------------------------------------------------
// Clean-room bitfield insertion helpers
// -----------------------------------------------------------------------------
namespace detail {

constexpr uint64_t bit_mask_range(uint8_t hi, uint8_t lo) {
    uint8_t width = static_cast<uint8_t>(hi - lo + 1u);
    if (width >= 64u) {
        return ~0ULL;
    }
    return ((1ULL << width) - 1ULL) << lo;
}

constexpr void set_bits(uint64_t& word, uint8_t hi, uint8_t lo, uint64_t value) {
    uint8_t width = static_cast<uint8_t>(hi - lo + 1u);
    uint64_t mask = (width >= 64u) ? ~0ULL : ((1ULL << width) - 1ULL);
    word = (word & ~(mask << lo)) | ((value & mask) << lo);
}

constexpr void set_bit(uint64_t& word, uint8_t bit, bool value) {
    set_bits(word, bit, bit, value ? 1ULL : 0ULL);
}

} // namespace detail

// -----------------------------------------------------------------------------
// Logical Compiler Opcodes mapped to USSE Hardware Units
// -----------------------------------------------------------------------------
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
    VLOG     = 0x0F,
    VEXP     = 0x10,
    LIMM     = 0x11,
    VTST     = 0x12,
    BR       = 0x13,
    SMP      = 0x14,
    DEPTHF   = 0x15,
    KILL     = 0x16,
    SMBO     = 0x17,
    PHAS     = 0x1E,
    HALT     = 0x1F
};

// -----------------------------------------------------------------------------
// USSE Predicate, Modifier, Swizzle & Register Bank Encodings
// -----------------------------------------------------------------------------
enum class ExtPredicate : uint8_t {
    None  = 0,
    P0    = 1,
    P1    = 2,
    P2    = 3,
    P3    = 4,
    NegP0 = 5,
    NegP1 = 6,
    PN    = 7
};

// Predicate field used specifically by V32NMAD / V16NMAD / VDP instructions
enum class ExtVecPredicate : uint8_t {
    None  = 0,
    P0    = 1,
    P1    = 2,
    P2    = 3,
    NegP0 = 4,
    NegP1 = 5,
    NegP2 = 6,
    PN    = 7
};

constexpr ExtVecPredicate to_ext_vec_pred(ExtPredicate p) {
    switch (p) {
        case ExtPredicate::P0:    return ExtVecPredicate::P0;
        case ExtPredicate::P1:    return ExtVecPredicate::P1;
        case ExtPredicate::P2:    return ExtVecPredicate::P2;
        case ExtPredicate::NegP0: return ExtVecPredicate::NegP0;
        case ExtPredicate::NegP1: return ExtVecPredicate::NegP1;
        case ExtPredicate::PN:    return ExtVecPredicate::PN;
        default:                  return ExtVecPredicate::None;
    }
}

enum class ShortPredicate : uint8_t {
    None  = 0,
    P0    = 1,
    P1    = 2,
    NegP0 = 3
};

// 3-bit per-channel swizzle used in V32NMAD src1_swiz (12-bit 4-component swizzle)
enum class SwizzleChannel : uint8_t {
    X    = 0,
    Y    = 1,
    Z    = 2,
    W    = 3,
    Zero = 4,
    One  = 5,
    Two  = 6,
    Half = 7
};

constexpr uint16_t pack_swizzle12(
    SwizzleChannel c0,
    SwizzleChannel c1,
    SwizzleChannel c2,
    SwizzleChannel c3
) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(c0) & 0x7u) |
        ((static_cast<uint16_t>(c1) & 0x7u) << 3u) |
        ((static_cast<uint16_t>(c2) & 0x7u) << 6u) |
        ((static_cast<uint16_t>(c3) & 0x7u) << 9u)
    );
}

constexpr uint16_t kSwiz12XYZW = pack_swizzle12(SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W);
constexpr uint16_t kSwiz12XXXX = pack_swizzle12(SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X, SwizzleChannel::X);
constexpr uint16_t kSwiz12YYYY = pack_swizzle12(SwizzleChannel::Y, SwizzleChannel::Y, SwizzleChannel::Y, SwizzleChannel::Y);
constexpr uint16_t kSwiz12ZZZZ = pack_swizzle12(SwizzleChannel::Z, SwizzleChannel::Z, SwizzleChannel::Z, SwizzleChannel::Z);
constexpr uint16_t kSwiz12WWWW = pack_swizzle12(SwizzleChannel::W, SwizzleChannel::W, SwizzleChannel::W, SwizzleChannel::W);
constexpr uint16_t kSwiz12Zero = pack_swizzle12(SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero, SwizzleChannel::Zero);
constexpr uint16_t kSwiz12One  = pack_swizzle12(SwizzleChannel::One, SwizzleChannel::One, SwizzleChannel::One, SwizzleChannel::One);

// Standard 4-bit vec4 swizzle table indices (public SGX/USSE vec4 swizzle encoding)
enum class Vec4Swiz : uint8_t {
    XXXX = 0,
    YYYY = 1,
    ZZZZ = 2,
    WWWW = 3,
    XYZW = 4,
    YZWW = 5,
    XYZZ = 6,
    XXYZ = 7,
    XYXY = 8,
    XYWZ = 9,
    ZXYW = 10,
    ZWZW = 11,
    YZXZ = 12,
    XXYY = 13,
    XZWW = 14,
    XYZ1 = 15
};

enum class SrcModifier : uint8_t {
    None   = 0,
    Negate = 1,
    Abs    = 2,
    NegAbs = 3
};

// VTST comparison sign/zero test modes
enum class VtstSignTest : uint8_t {
    NeOrEq = 0, // != (with ExcludeZero) or == (with IncludeZero)
    LtOrLe = 1, // <  (with ExcludeZero) or <= (with IncludeZero)
    GtOrGe = 2  // >  (with ExcludeZero) or >= (with IncludeZero)
};

enum class VtstZeroTest : uint8_t {
    Always      = 0,
    IncludeZero = 1, // ==, <=, >=
    ExcludeZero = 2  // !=, <,  >
};

enum class RegisterClass : uint8_t {
    Temporary      = 0, // 'r'  / TEMP bank
    Output         = 1, // 'o'  / OUTPUT bank
    InputAttr      = 2, // 'v'  / PRIMATTR bank
    SecAttr        = 3, // 'sa' / SECATTR bank
    Constant       = 4, // 'c'  / Uniform constant (SECATTR bank)
    TextureSampler = 5  // 's'  / Sampler descriptor (SECATTR bank)
};

// Destination bank encoding (dest_bank_ext == 0): 0=TEMP, 1=OUTPUT, 2=PRIMATTR
constexpr uint64_t encode_dest_bank(RegisterClass rc) {
    switch (rc) {
        case RegisterClass::Temporary:      return 0ULL;
        case RegisterClass::Output:         return 1ULL;
        case RegisterClass::InputAttr:      return 2ULL;
        case RegisterClass::SecAttr:
        case RegisterClass::Constant:
        case RegisterClass::TextureSampler: return 0ULL; // with dest_bank_ext = 1
    }
    return 0ULL;
}

constexpr bool dest_requires_bank_ext(RegisterClass rc) {
    return rc == RegisterClass::SecAttr ||
           rc == RegisterClass::Constant ||
           rc == RegisterClass::TextureSampler;
}

// Src1/Src2 bank encoding (src_bank_ext == 0): 0=TEMP, 1=OUTPUT, 2=PRIMATTR, 3=SECATTR
constexpr uint64_t encode_src12_bank(RegisterClass rc) {
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

// Src0 bank encoding (src0_bank_ext == 0): 0=TEMP, 1=PRIMATTR; (src0_bank_ext == 1): 0=OUTPUT, 1=SECATTR
constexpr uint64_t encode_src0_bank_bit(RegisterClass rc) {
    switch (rc) {
        case RegisterClass::Temporary:
        case RegisterClass::Output:         return 0ULL;
        case RegisterClass::InputAttr:
        case RegisterClass::SecAttr:
        case RegisterClass::Constant:
        case RegisterClass::TextureSampler: return 1ULL;
    }
    return 0ULL;
}

constexpr bool src0_requires_bank_ext(RegisterClass rc) {
    return rc == RegisterClass::Output ||
           rc == RegisterClass::SecAttr ||
           rc == RegisterClass::Constant ||
           rc == RegisterClass::TextureSampler;
}

struct HardwareRegister {
    RegisterClass reg_class = RegisterClass::Temporary;
    uint8_t index = 0;       // Register word offset (even-aligned for vector ops)
    uint8_t sub_index = 0;   // Primary scalar lane (0=x, 1=y, 2=z, 3=w)
    std::array<SwizzleChannel, 4> swizzle = {
        SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W
    };
    bool is_negated = false;
    bool is_absolute = false;

    [[nodiscard]] SrcModifier modifier() const {
        if (is_negated && is_absolute) return SrcModifier::NegAbs;
        if (is_absolute) return SrcModifier::Abs;
        if (is_negated) return SrcModifier::Negate;
        return SrcModifier::None;
    }

    [[nodiscard]] uint16_t swizzle12() const {
        return pack_swizzle12(swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
    }

    [[nodiscard]] std::string to_string() const {
        std::ostringstream ss;
        if (is_negated) ss << "-";
        if (is_absolute) ss << "|";
        switch (reg_class) {
            case RegisterClass::Temporary:      ss << "r" << static_cast<int>(index); break;
            case RegisterClass::Output:         ss << "o" << static_cast<int>(index); break;
            case RegisterClass::InputAttr:      ss << "pa" << static_cast<int>(index); break;
            case RegisterClass::SecAttr:        ss << "sa" << static_cast<int>(index); break;
            case RegisterClass::Constant:       ss << "c" << static_cast<int>(index); break;
            case RegisterClass::TextureSampler: ss << "s" << static_cast<int>(index); break;
        }
        static constexpr char kComponents[] = {'x', 'y', 'z', 'w', '0', '1', '2', 'h'};
        if (sub_index < 4) {
            ss << "." << kComponents[sub_index];
        }
        if (is_absolute) ss << "|";
        return ss.str();
    }
};

// -----------------------------------------------------------------------------
// 64-bit USSE Microcode Instruction Word Builder
// Every bit position is explicitly derived from public USSE instruction formats.
// -----------------------------------------------------------------------------
struct MicrocodeWord {
    uint64_t raw_word = 0;

    MicrocodeWord() = default;
    explicit MicrocodeWord(uint64_t val) : raw_word(val) {}

    // PHAS: "11111010s100eirc--matwwwppppppppbbnn--------xxxxxxoooooooddddddd"
    static MicrocodeWord encode_phas(bool end = true) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 58, 56, 0b010);   // op2 = Phase
        set_bits(w, 54, 52, 0b100);   // PHAS discriminator
        set_bit(w, 51, end);          // End of program / phase flag
        return MicrocodeWord(w);
    }

    // NOP: "11111----000-----------101--------------------------------------"
    static MicrocodeWord encode_nop() {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 54, 52, 0b000);   // Flow/misc category
        set_bits(w, 40, 38, 0b101);   // NOP discriminator
        return MicrocodeWord(w);
    }

    // KILL: "11111001--11000000000pp0000001101111----------------------------"
    static MicrocodeWord encode_kill(ShortPredicate kpred = ShortPredicate::None) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 58, 56, 0b001);   // op2
        set_bits(w, 53, 52, 0b11);    // opcat = 3 (fragment control)
        set_bits(w, 51, 43, 0ULL);    // kill = 0
        set_bits(w, 42, 41, static_cast<uint64_t>(kpred));
        set_bits(w, 40, 28, 0b0000001101111ULL); // kill2 discriminator (0x6F)
        return MicrocodeWord(w);
    }

    // SMBO: "11111011--01-n--ddddddddddddssssssssssssrrrrrrrrrrrrcccccccccccc"
    static MicrocodeWord encode_smbo(
        uint16_t dest_offset = 0,
        uint16_t src0_offset = 0,
        uint16_t src1_offset = 0,
        uint16_t src2_offset = 0
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 58, 56, 0b011);   // op2 = 3
        set_bits(w, 53, 52, 0b01);    // opcat = 1 (SMBO)
        set_bits(w, 47, 36, static_cast<uint64_t>(dest_offset & 0xFFFu));
        set_bits(w, 35, 24, static_cast<uint64_t>(src0_offset & 0xFFFu));
        set_bits(w, 23, 12, static_cast<uint64_t>(src1_offset & 0xFFFu));
        set_bits(w, 11, 0,  static_cast<uint64_t>(src2_offset & 0xFFFu));
        return MicrocodeWord(w);
    }

    // DEPTHF: "11111011s-11recb----npp---tffa--kkddggggggghhhhhhhiiiiiiijjjjjjj"
    static MicrocodeWord encode_depthf(
        HardwareRegister depth_src,
        ShortPredicate spred = ShortPredicate::None,
        bool feedback = true
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 58, 56, 0b011);   // op2 = 3 (DEPTHF)
        set_bits(w, 53, 52, 0b11);    // opcat = 3
        set_bit(w, 51, src0_requires_bank_ext(depth_src.reg_class));
        set_bits(w, 42, 41, static_cast<uint64_t>(spred));
        set_bit(w, 37, false);        // type = 0 (standard F32 depth)
        set_bits(w, 36, 35, feedback ? 1ULL : 0ULL); // ff = feedback mode
        set_bits(w, 34, 34, encode_src0_bank_bit(depth_src.reg_class)); // a = src0_bank
        set_bits(w, 20, 14, static_cast<uint64_t>(depth_src.index >> 1) & 0x7FULL); // doubled src0_n
        return MicrocodeWord(w);
    }

    // LIMM: "11111100sn10deiiiiiipppmmmmm--tt----uuuuuuuvvvvvvvvvvvvvvvvvvvvv"
    static MicrocodeWord encode_limm(
        HardwareRegister dst,
        uint32_t imm32,
        ExtPredicate pred = ExtPredicate::None
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 58, 56, 0b100);   // op2 = 4 (LIMM)
        set_bits(w, 53, 52, 0b10);    // opcat = 2
        set_bit(w, 51, dest_requires_bank_ext(dst.reg_class));
        set_bits(w, 49, 44, (imm32 >> 26) & 0x3FULL);
        set_bits(w, 43, 41, static_cast<uint64_t>(pred));
        set_bits(w, 40, 36, (imm32 >> 21) & 0x1FULL);
        set_bits(w, 33, 32, encode_dest_bank(dst.reg_class));
        set_bits(w, 27, 21, static_cast<uint64_t>(dst.index) & 0x7FULL); // un-doubled 7-bit reg index
        set_bits(w, 20, 0, imm32 & 0x1FFFFFULL);
        return MicrocodeWord(w);
    }

    // BR: "11111ppps000e-----wynba00r----------------iloooooooooooooooooooo"
    static MicrocodeWord encode_br(
        int32_t rel_word_offset,
        ExtPredicate pred = ExtPredicate::None,
        bool any_inst = true,
        bool all_inst = false
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11111); // Special instruction primary opcode
        set_bits(w, 58, 56, static_cast<uint64_t>(pred));
        set_bit(w, 54, false);        // opcat_extra = 0
        set_bits(w, 53, 52, 0b00);    // op2_flow_ctrl = 0 (BR)
        set_bits(w, 40, 39, 0b00);    // br_op = 0 (BR)
        set_bit(w, 38, true);         // br_type = 1 (signed PC-relative word offset)
        set_bit(w, 21, any_inst);
        set_bit(w, 20, all_inst);
        set_bits(w, 19, 0, static_cast<uint32_t>(rel_word_offset) & 0xFFFFFU);
        return MicrocodeWord(w);
    }

    // VMOV: "00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv"
    // Note: F32 TEMP/OUTPUT moves mask low 2 channels (.xy) on an even-aligned pair.
    static MicrocodeWord encode_vmov(
        HardwareRegister dst,
        HardwareRegister src1,
        uint8_t mask = 0b0011,
        Vec4Swiz src1_swiz = Vec4Swiz::XYZW,
        ExtPredicate pred = ExtPredicate::None
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b00111); // op1 = VMOV
        set_bits(w, 58, 56, static_cast<uint64_t>(pred));
        set_bit(w, 51, dest_requires_bank_ext(dst.reg_class));
        set_bits(w, 47, 46, 0ULL);    // move_type = UNCONDITIONAL
        set_bits(w, 42, 40, 5ULL);    // move_data_type = F32
        set_bits(w, 38, 35, static_cast<uint64_t>(src1_swiz));
        set_bits(w, 33, 32, encode_dest_bank(dst.reg_class));
        set_bits(w, 31, 30, encode_src12_bank(src1.reg_class));
        set_bits(w, 27, 24, static_cast<uint64_t>(mask & 0x3u));
        set_bits(w, 23, 18, static_cast<uint64_t>(dst.index >> 1) & 0x3FULL);
        set_bits(w, 11, 6, static_cast<uint64_t>(src1.index >> 1) & 0x3FULL);
        return MicrocodeWord(w);
    }

    // V32NMAD: "00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj"
    // Bit 59 is implicitly 1 (op1 = 00001 -> F32 32-bit vector ALU operations).
    static MicrocodeWord encode_v32nmad(
        UsseOpcode op,
        HardwareRegister dst,
        HardwareRegister src1,
        HardwareRegister src2,
        uint8_t dest_mask = 0b1111,
        uint16_t src1_swiz12 = kSwiz12XYZW,
        Vec4Swiz src2_swiz4 = Vec4Swiz::XYZW,
        ExtPredicate pred = ExtPredicate::None
    ) {
        using namespace detail;
        uint64_t w = 0;

        uint8_t op2 = 1; // 0=VMUL, 1=VADD, 2=VFRC, 3=VDSX, 4=VDSY, 5=VMIN, 6=VMAX, 7=VDP
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
            default:               op2 = 1; break;
        }

        // VSUB(a, b) is encoded in V32NMAD as VADD with src1 negated: (-src1) + src2.
        // Note: in V32NMAD, 'o' (bit 36) is src2 Absolute, while 'mm' (bits 38..37) is src1 Negate/Abs.
        SrcModifier s1_mod = src1.modifier();
        if (op == UsseOpcode::VSUB) {
            // Codegen orders operands so src1 is the subtracted term when emitting VSUB,
            // or flips Negate on src1 here.
            if (s1_mod == SrcModifier::None) s1_mod = SrcModifier::Negate;
            else if (s1_mod == SrcModifier::Negate) s1_mod = SrcModifier::None;
            else if (s1_mod == SrcModifier::Abs) s1_mod = SrcModifier::NegAbs;
            else if (s1_mod == SrcModifier::NegAbs) s1_mod = SrcModifier::Abs;
        }

        set_bits(w, 63, 59, 0b00001); // op1 = V32NMAD (F32)
        set_bits(w, 58, 56, static_cast<uint64_t>(to_ext_vec_pred(pred)));
        set_bits(w, 54, 53, (src1_swiz12 >> 10) & 0x3u); // src1_swiz_10_11
        set_bit(w, 51, dest_requires_bank_ext(dst.reg_class));
        set_bit(w, 50, ((src1_swiz12 >> 9) & 0x1u) != 0); // src1_swiz_9
        set_bits(w, 47, 44, static_cast<uint64_t>(src2_swiz4));
        set_bits(w, 42, 39, static_cast<uint64_t>(dest_mask & 0xFu));
        set_bits(w, 38, 37, static_cast<uint64_t>(s1_mod));
        set_bit(w, 36, src2.is_absolute); // src2_mod: 1 = Absolute
        set_bits(w, 35, 34, (src1_swiz12 >> 7) & 0x3u); // src1_swiz_7_8
        set_bits(w, 33, 32, encode_dest_bank(dst.reg_class));
        set_bits(w, 31, 30, encode_src12_bank(src1.reg_class));
        set_bits(w, 29, 28, encode_src12_bank(src2.reg_class));
        set_bits(w, 27, 22, static_cast<uint64_t>(dst.index >> 1) & 0x3FULL);
        set_bits(w, 21, 15, src1_swiz12 & 0x7Fu); // src1_swiz_0_6
        set_bits(w, 14, 12, op2);
        set_bits(w, 11, 6, static_cast<uint64_t>(src1.index >> 1) & 0x3FULL);
        set_bits(w, 5, 0, static_cast<uint64_t>(src2.index >> 1) & 0x3FULL);
        return MicrocodeWord(w);
    }

    // VCOMP: "00110pppsddyenr-aaaaobbccmmff-ttkk--ggggggg-------hhhhhhh---wwww"
    static MicrocodeWord encode_vcomp(
        UsseOpcode op,
        HardwareRegister dst,
        HardwareRegister src1,
        uint8_t src_comp = 0,
        uint8_t write_mask = 0b0001,
        ExtPredicate pred = ExtPredicate::None
    ) {
        using namespace detail;
        uint64_t w = 0;

        uint8_t op2 = 0; // 0=VRCP, 1=VRSQ, 2=VLOG, 3=VEXP
        switch (op) {
            case UsseOpcode::VRCP: op2 = 0; break;
            case UsseOpcode::VRSQ: op2 = 1; break;
            case UsseOpcode::VLOG: op2 = 2; break;
            case UsseOpcode::VEXP: op2 = 3; break;
            default:               op2 = 0; break;
        }

        set_bits(w, 63, 59, 0b00110); // op1 = VCOMP
        set_bits(w, 58, 56, static_cast<uint64_t>(pred));
        set_bits(w, 54, 53, 0ULL);    // dest_type = F32
        set_bit(w, 51, dest_requires_bank_ext(dst.reg_class));
        set_bit(w, 49, false);        // src1_bank_ext = 0
        set_bits(w, 42, 41, op2);
        set_bits(w, 40, 39, 0ULL);    // src_type = F32
        set_bits(w, 38, 37, static_cast<uint64_t>(src1.modifier()));
        set_bits(w, 36, 35, static_cast<uint64_t>(src_comp & 0x3u));
        set_bits(w, 33, 32, encode_dest_bank(dst.reg_class));
        set_bits(w, 31, 30, encode_src12_bank(src1.reg_class));
        set_bits(w, 27, 21, static_cast<uint64_t>(dst.index >> 1) & 0x7FULL);  // 7-bit doubled
        set_bits(w, 13, 7, static_cast<uint64_t>(src1.index >> 1) & 0x7FULL);  // 7-bit doubled
        set_bits(w, 3, 0, static_cast<uint64_t>(write_mask & 0xFu));
        return MicrocodeWord(w);
    }

    // VTST: "01001ppps-oydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq"
    // Emits F32 predicate comparison: p(pdst_n) = (src1.<chan> <op> src2.<chan>)
    static MicrocodeWord encode_vtst(
        uint8_t pdst_n,
        HardwareRegister src1,
        HardwareRegister src2,
        VtstSignTest sign_test,
        VtstZeroTest zero_test,
        uint8_t chan_cc = 0,
        bool src2_vscomp = false,
        ExtPredicate pred = ExtPredicate::None
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b01001); // op1 = VTST
        set_bits(w, 58, 56, static_cast<uint64_t>(pred));
        set_bit(w, 50, src1.is_negated);
        set_bit(w, 47, true);         // prec = 1 (F32)
        set_bit(w, 46, src2_vscomp);
        set_bits(w, 43, 42, static_cast<uint64_t>(sign_test));
        set_bits(w, 41, 40, static_cast<uint64_t>(zero_test));
        set_bits(w, 38, 36, static_cast<uint64_t>(chan_cc & 0x7u));
        set_bits(w, 35, 34, static_cast<uint64_t>(pdst_n & 0x3u));
        set_bits(w, 31, 30, encode_src12_bank(src1.reg_class));
        set_bits(w, 29, 28, encode_src12_bank(src2.reg_class));
        set_bit(w, 20, false);        // test_wben = 0 (write predicate only)
        set_bits(w, 19, 18, 0ULL);    // alu_sel = 0 (float compare)
        set_bits(w, 17, 14, 14ULL);   // alu_op = 14 (VSUB compare)
        set_bits(w, 13, 7, static_cast<uint64_t>(src1.index >> 1) & 0x7FULL); // doubled
        set_bits(w, 6, 0, static_cast<uint64_t>(src2.index >> 1) & 0x7FULL);  // doubled
        return MicrocodeWord(w);
    }

    // SMP: "11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv"
    // dst = sampler result (un-doubled TEMP/PA), src0 = UV (doubled TEMP/PA),
    // src1 = sampler SA offset (doubled SECATTR), src2 = optional LOD/bias (doubled).
    static MicrocodeWord encode_smp(
        HardwareRegister dst,
        HardwareRegister uv_src,
        uint16_t sampler_sa_offset,
        uint8_t dim = 1,          // 0=1D, 1=2D, 2=3D/Cube
        uint8_t lod_mode = 0,     // 0=none, 1=bias, 2=lod_replace, 3=grad
        HardwareRegister lod_src = {},
        ExtPredicate pred = ExtPredicate::None
    ) {
        using namespace detail;
        uint64_t w = 0;
        set_bits(w, 63, 59, 0b11100); // op1 = SMP
        set_bits(w, 58, 56, static_cast<uint64_t>(pred));
        set_bits(w, 47, 46, 3ULL);    // fconv_type = 3 (F32 result)
        set_bits(w, 43, 42, static_cast<uint64_t>(dim & 0x3u));
        set_bits(w, 41, 40, static_cast<uint64_t>(lod_mode & 0x3u));
        set_bit(w, 39, dst.reg_class == RegisterClass::InputAttr); // dest_use_pa
        set_bits(w, 38, 37, 0ULL);    // sb_mode = 0 (standard sample)
        set_bits(w, 36, 35, 0ULL);    // src0_type = F32
        set_bit(w, 34, uv_src.reg_class == RegisterClass::InputAttr); // src0_bank (0=TEMP, 1=PA)
        set_bits(w, 31, 30, 3ULL);    // src1_bank = 3 (SECATTR)
        set_bits(w, 29, 28, (lod_mode != 0) ? encode_src12_bank(lod_src.reg_class) : 0ULL);
        set_bits(w, 27, 21, static_cast<uint64_t>(dst.index) & 0x7FULL);            // un-doubled
        set_bits(w, 20, 14, static_cast<uint64_t>(uv_src.index >> 1) & 0x7FULL);    // doubled
        set_bits(w, 13, 7, static_cast<uint64_t>(sampler_sa_offset >> 1) & 0x7FULL);// doubled SA
        set_bits(w, 6, 0, (lod_mode != 0) ? (static_cast<uint64_t>(lod_src.index >> 1) & 0x7FULL) : 0ULL);
        return MicrocodeWord(w);
    }
};

struct UsseInstruction {
    UsseOpcode opcode = UsseOpcode::NOP;
    HardwareRegister dst;
    HardwareRegister src0;
    HardwareRegister src1;
    HardwareRegister src2;
    uint8_t write_mask = 0b1111;

    // Extended instruction parameters
    ExtPredicate pred = ExtPredicate::None;
    ShortPredicate short_pred = ShortPredicate::None;
    uint16_t src1_swiz12 = kSwiz12XYZW;
    Vec4Swiz src2_swiz4 = Vec4Swiz::XYZW;
    uint8_t src_comp = 0;        // VCOMP source lane (0..3)
    uint32_t imm32 = 0;          // LIMM immediate value
    int32_t branch_offset = 0;   // BR relative word offset
    uint16_t smbo_src1_offset = 0; // SMBO src1 doubled-register offset
    uint8_t pdst_n = 0;          // VTST destination predicate (0..2)
    VtstSignTest vtst_sign = VtstSignTest::NeOrEq;
    VtstZeroTest vtst_zero = VtstZeroTest::ExcludeZero;
    uint8_t vtst_chan = 0;       // VTST channel (0..3)
    bool vtst_src2_vscomp = false;
    uint8_t smp_dim = 1;         // 0=1D, 1=2D, 2=3D/Cube
    uint8_t smp_lod_mode = 0;    // 0=none, 1=bias, 2=lod
    uint16_t sampler_sa_offset = 0;

    std::string comment;

    [[nodiscard]] MicrocodeWord to_microcode() const {
        switch (opcode) {
            case UsseOpcode::NOP:
                return MicrocodeWord::encode_nop();
            case UsseOpcode::PHAS:
            case UsseOpcode::HALT:
                return MicrocodeWord::encode_phas(true);
            case UsseOpcode::KILL:
                return MicrocodeWord::encode_kill(short_pred);
            case UsseOpcode::SMBO:
                return MicrocodeWord::encode_smbo(0, 0, smbo_src1_offset, 0);
            case UsseOpcode::DEPTHF:
                return MicrocodeWord::encode_depthf(src0, short_pred, true);
            case UsseOpcode::LIMM:
                return MicrocodeWord::encode_limm(dst, imm32, pred);
            case UsseOpcode::BR:
                return MicrocodeWord::encode_br(branch_offset, pred, true, false);
            case UsseOpcode::VMOV:
                return MicrocodeWord::encode_vmov(dst, src0, write_mask, src2_swiz4, pred);
            case UsseOpcode::VRCP:
            case UsseOpcode::VRSQ:
            case UsseOpcode::VLOG:
            case UsseOpcode::VEXP:
                return MicrocodeWord::encode_vcomp(opcode, dst, src0, src_comp, write_mask, pred);
            case UsseOpcode::VTST:
                return MicrocodeWord::encode_vtst(pdst_n, src0, src1, vtst_sign, vtst_zero, vtst_chan, vtst_src2_vscomp, pred);
            case UsseOpcode::SMP:
                return MicrocodeWord::encode_smp(dst, src1, sampler_sa_offset, smp_dim, smp_lod_mode, src2, pred);
            default:
                return MicrocodeWord::encode_v32nmad(opcode, dst, src0, src1, write_mask, src1_swiz12, src2_swiz4, pred);
        }
    }

    [[nodiscard]] std::string disassemble() const {
        std::ostringstream ss;
        auto pred_prefix = [](ExtPredicate p) -> const char* {
            switch (p) {
                case ExtPredicate::P0:    return "(p0) ";
                case ExtPredicate::P1:    return "(p1) ";
                case ExtPredicate::P2:    return "(p2) ";
                case ExtPredicate::P3:    return "(p3) ";
                case ExtPredicate::NegP0: return "(!p0) ";
                case ExtPredicate::NegP1: return "(!p1) ";
                case ExtPredicate::PN:    return "(pn) ";
                default:                  return "";
            }
        };
        auto fmt_swiz12 = [](uint16_t sw) -> std::string {
            static constexpr char kC[] = {'x', 'y', 'z', 'w', '0', '1', '2', 'h'};
            std::string s = ".";
            for (int i = 0; i < 4; ++i) {
                s.push_back(kC[(sw >> (i * 3)) & 0x7u]);
            }
            return s;
        };

        ss << pred_prefix(pred);
        switch (opcode) {
            case UsseOpcode::NOP:  ss << "NOP"; break;
            case UsseOpcode::VMOV: ss << "VMOV " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VADD: ss << "VADD " << dst.to_string() << ", " << src0.to_string() << fmt_swiz12(src1_swiz12) << ", " << src1.to_string(); break;
            case UsseOpcode::VSUB: ss << "VSUB " << dst.to_string() << ", " << src1.to_string() << ", " << src0.to_string() << fmt_swiz12(src1_swiz12); break;
            case UsseOpcode::VMUL: ss << "VMUL " << dst.to_string() << ", " << src0.to_string() << fmt_swiz12(src1_swiz12) << ", " << src1.to_string(); break;
            case UsseOpcode::VMAD: ss << "VMAD " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string() << ", " << src2.to_string(); break;
            case UsseOpcode::VDP3: ss << "VDP3 " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VDP4: ss << "VDP4 " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VRCP: ss << "VRCP " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VRSQ: ss << "VRSQ " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VLOG: ss << "VLOG " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VEXP: ss << "VEXP " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VMIN: ss << "VMIN " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VMAX: ss << "VMAX " << dst.to_string() << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::VFRC: ss << "VFRC " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VDSX: ss << "VDSX " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::VDSY: ss << "VDSY " << dst.to_string() << ", " << src0.to_string(); break;
            case UsseOpcode::LIMM: ss << "LIMM " << dst.to_string() << ", 0x" << std::hex << std::uppercase << imm32 << std::dec; break;
            case UsseOpcode::VTST: ss << "VTST p" << static_cast<int>(pdst_n) << ", " << src0.to_string() << ", " << src1.to_string(); break;
            case UsseOpcode::BR:   ss << "BR " << std::showpos << branch_offset << std::noshowpos; break;
            case UsseOpcode::SMP:  ss << "SMP  " << dst.to_string() << ", " << src1.to_string() << ", sa" << sampler_sa_offset << " (s" << static_cast<int>(src0.index) << ")"; break;
            case UsseOpcode::DEPTHF: ss << "DEPTHF " << src0.to_string(); break;
            case UsseOpcode::KILL: ss << "KILL"; break;
            case UsseOpcode::SMBO: ss << "SMBO 0, 0, " << smbo_src1_offset << ", 0"; break;
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
