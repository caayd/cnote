#include "document/XpressCodec.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    const std::vector<std::uint8_t> independentFrame = {
        0x0A, 0x51, 0xE5, 0xC0, 0x18, 0x00, 0x17, 0x04,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00,
    };
    assert(cnote::DecompressXpressBuffer(independentFrame, 1).value() == std::vector<std::uint8_t>{0});

    std::vector<std::uint8_t> input;
    for (unsigned repeat = 0; repeat != 64; ++repeat) {
        for (unsigned value = 0; value != 256; ++value) input.push_back(static_cast<std::uint8_t>(value));
    }
    const auto encoded = cnote::CompressXpressBuffer(input);
    assert(encoded.has_value());
    assert(cnote::DecompressXpressBuffer(*encoded, input.size()).value() == input);

    auto truncated = *encoded;
    truncated.pop_back();
    assert(!cnote::DecompressXpressBuffer(truncated, input.size()));
    auto badCrc = *encoded;
    badCrc[6] ^= 1;
    assert(!cnote::DecompressXpressBuffer(badCrc, input.size()));
    assert(!cnote::DecompressXpressBuffer(*encoded, input.size() + 1));
    return 0;
}
