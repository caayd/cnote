#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cnote {

[[nodiscard]] std::optional<std::vector<std::uint8_t>> CompressXpressBuffer(
    std::span<const std::uint8_t> input);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> DecompressXpressBuffer(
    std::span<const std::uint8_t> input,
    std::uint64_t expectedSize);

}  // namespace cnote
