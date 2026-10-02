#pragma once

#include <cstdint>
#include <span>

namespace mtpadb::gadget {

std::span<const std::uint8_t> build_functionfs_descriptors() noexcept;
std::span<const std::uint8_t> build_functionfs_strings() noexcept;

}  // namespace mtpadb::gadget
