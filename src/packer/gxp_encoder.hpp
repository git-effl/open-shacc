/**
 * @file gxp_encoder.hpp
 * @brief Serializer for packaging compiled shader microcode into GXP binary containers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef OPEN_SHACC_GXP_ENCODER_HPP
#define OPEN_SHACC_GXP_ENCODER_HPP

#include "gxp_format.hpp"
#include "../backend/usse_codegen.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace open_shacc::packer {

class GxpEncoder {
public:
    GxpEncoder() = default;

    /**
     * @brief Encode a compiled USSE shader program into a GXP binary container.
     *
     * @param program Compiled program containing microcode, symbols, and layout.
     * @param out_bytes Destination vector receiving the serialized 16-byte aligned GXP binary.
     * @return true on success, false on serialization error.
     */
    bool encode(const backend::CompiledProgram& program, std::vector<uint8_t>& out_bytes);

    [[nodiscard]] const std::string& get_error_message() const { return error_message_; }

    /**
     * @brief Validate and parse a GXP binary container.
     */
    static bool validate(const uint8_t* data, size_t size, std::string& out_error);

private:
    std::string error_message_;
};

} // namespace open_shacc::packer

#endif // OPEN_SHACC_GXP_ENCODER_HPP
