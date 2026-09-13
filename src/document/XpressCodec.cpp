#include "document/XpressCodec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <new>
#include <queue>
#include <stdexcept>
#include <utility>
namespace cnote {
namespace {

constexpr std::uint32_t kMagic = 0xC0E5510AU;
constexpr std::uint16_t kHeaderSize = 24U;
constexpr std::uint8_t kModeXpressHuffman = 4U;
constexpr std::size_t kHeaderBytes = 28U;
constexpr std::size_t kTableBytes = 256U;
constexpr std::size_t kSymbolCount = 512U;
// Windows Compression API buffer framing is an observed 24-byte header followed
// by a body at offset 28: magic@0, header size@4, low byte of standard
// reflected CRC32@6, mode@7, uncompressed size@8, unknown zero@12, chunk
// size@16, unknown zero@20, body size@24. The body uses one 256-byte
// nibble-length table per 64 KiB decoded output block.

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned byte = 0; byte != 4; ++byte) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

std::uint8_t HeaderCrc(std::span<const std::uint8_t> header) {
    // Cabinet's buffer wrapper uses the low CRC-32 byte over the header with
    // the CRC byte itself omitted. This is framing, not an XPRESS payload CRC.
    std::uint32_t crc = 0xFFFFFFFFU;
    auto update = [&crc](std::uint8_t value) {
        crc ^= value;
        for (unsigned bit = 0; bit != 8; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    };
    for (std::size_t i = 0; i != 6; ++i) update(header[i]);
    for (std::size_t i = 7; i != 24; ++i) update(header[i]);
    return static_cast<std::uint8_t>(crc ^ 0xFFFFFFFFU);
}

struct Code {
    std::uint32_t bits = 0;
    std::uint8_t length = 0;
};

std::optional<std::array<Code, kSymbolCount>> BuildCodes(
    const std::array<std::uint64_t, kSymbolCount>& frequencies,
    std::array<std::uint8_t, kSymbolCount>& lengths) {
    struct Node {
        std::uint64_t frequency;
        int symbol;
        int left;
        int right;
    };
    struct QueueEntry {
        std::uint64_t frequency;
        int node;
        bool operator>(const QueueEntry& other) const noexcept {
            return frequency > other.frequency
                || (frequency == other.frequency && node > other.node);
        }
    };
    std::vector<Node> nodes;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> queue;
    for (std::size_t symbol = 0; symbol != kSymbolCount; ++symbol) {
        if (frequencies[symbol] != 0) {
            queue.push({frequencies[symbol], static_cast<int>(nodes.size())});
            nodes.push_back({frequencies[symbol], static_cast<int>(symbol), -1, -1});
        }
    }
    lengths.fill(0);
    if (queue.empty()) return std::nullopt;
    if (queue.size() == 1) {
        lengths[static_cast<std::size_t>(nodes[queue.top().node].symbol)] = 1;
    } else {
        while (queue.size() > 1) {
            const auto first = queue.top(); queue.pop();
            const auto second = queue.top(); queue.pop();
            const int node = static_cast<int>(nodes.size());
            nodes.push_back({first.frequency + second.frequency, -1, first.node, second.node});
            queue.push({nodes.back().frequency, node});
        }
        const int root = queue.top().node;
        bool tooDeep = false;
        const auto visit = [&nodes, &lengths, &tooDeep](auto&& self, int node, unsigned depth) -> void {
            const Node& current = nodes[static_cast<std::size_t>(node)];
            if (current.symbol >= 0) {
                if (depth > 15U) tooDeep = true;
                else lengths[static_cast<std::size_t>(current.symbol)] = static_cast<std::uint8_t>(depth);
                return;
            }
            self(self, current.left, depth + 1U);
            self(self, current.right, depth + 1U);
        };
        visit(visit, root, 0);
        if (tooDeep) return std::nullopt;
    }
    std::array<std::uint16_t, 16> count{};
    for (std::uint8_t length : lengths) if (length != 0) ++count[length];
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (unsigned length = 1; length <= 15; ++length) {
        code = (code + count[length - 1]) << 1U;
        next[length] = code;
    }
    std::array<Code, kSymbolCount> result{};
    for (std::size_t symbol = 0; symbol != kSymbolCount; ++symbol) {
        const std::uint8_t length = lengths[symbol];
        if (length != 0) result[symbol] = {next[length]++, length};
    }
    return result;
}

class BitWriter {
public:
    void Write(std::uint32_t value, unsigned bits) {
        if (!primed_) Prime();
        if (bits == 0) return;
        acc_ |= (value & ((std::uint32_t{1} << bits) - 1U)) << (32U - pendingBits_ - bits);
        pendingBits_ += bits;
        cumBits_ += bits;
        while (pendingBits_ >= 16U) {
            const std::uint16_t word = static_cast<std::uint16_t>(acc_ >> 16U);
            acc_ <<= 16U; pendingBits_ -= 16U;
            if (nextSlot_ == slots_.size()) {
                slots_.push_back(bytes_.size()); bytes_.push_back(0); bytes_.push_back(0);
            }
            Patch(slots_[nextSlot_++], word);
        }
    }
    void WriteRawByte(std::uint8_t value) {
        if (!primed_) Prime();
        const std::size_t required = cumBits_ <= 16U ? 2U : 2U + (cumBits_ - 16U + 15U) / 16U;
        while (slots_.size() < required) {
            slots_.push_back(bytes_.size());
            bytes_.push_back(0);
            bytes_.push_back(0);
        }
        bytes_.push_back(value);
    }
    void WriteRaw16(std::uint32_t value) {
        WriteRawByte(static_cast<std::uint8_t>(value));
        WriteRawByte(static_cast<std::uint8_t>(value >> 8U));
    }
    void WriteRaw32(std::uint32_t value) {
        WriteRaw16(value); WriteRaw16(value >> 16U);
    }
    [[nodiscard]] std::vector<std::uint8_t> Finish() {
        if (!primed_) Prime();
        if (pendingBits_ != 0) {
            if (nextSlot_ == slots_.size()) {
                slots_.push_back(bytes_.size()); bytes_.push_back(0); bytes_.push_back(0);
            }
            Patch(slots_[nextSlot_++], static_cast<std::uint16_t>(acc_ >> 16U));
        }
        const std::size_t required = cumBits_ <= 16U ? 2U : 2U + (cumBits_ - 16U + 15U) / 16U;
        while (slots_.size() < required) {
            slots_.push_back(bytes_.size());
            bytes_.push_back(0);
            bytes_.push_back(0);
        }
        return std::move(bytes_);
    }
    void Prime() {
        primed_ = true;
        for (unsigned i = 0; i != 2; ++i) {
            slots_.push_back(bytes_.size()); bytes_.push_back(0); bytes_.push_back(0);
        }
    }
    void Patch(std::size_t position, std::uint16_t word) {
        bytes_[position] = static_cast<std::uint8_t>(word);
        bytes_[position + 1] = static_cast<std::uint8_t>(word >> 8U);
    }
    std::vector<std::uint8_t> bytes_;
    std::vector<std::size_t> slots_;
    std::uint32_t acc_ = 0;
    unsigned pendingBits_ = 0;
    std::uint32_t cumBits_ = 0;
    std::size_t nextSlot_ = 0;
    bool primed_ = false;
};
struct Token {
    std::uint16_t symbol;
    std::uint32_t length = 1;
    std::uint32_t offset = 0;
};

std::optional<std::vector<std::uint8_t>> EncodeLiteralStream(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> result;
    constexpr std::size_t kBlockSize = 65536U;
    constexpr std::size_t kHashSize = 65536U;
    constexpr std::size_t kMissing = std::numeric_limits<std::size_t>::max();
    for (std::size_t blockOffset = 0; blockOffset < input.size();) {
        const std::size_t blockSize = std::min(kBlockSize, input.size() - blockOffset);
        const auto block = input.subspan(blockOffset, blockSize);
        std::vector<Token> tokens;
        tokens.reserve(blockSize);
        std::array<std::size_t, kHashSize> last;
        last.fill(kMissing);
        const auto hash = [](std::uint32_t key) {
            return static_cast<std::size_t>((key * 2654435761U) >> 16U);
        };
        for (std::size_t position = 0; position < block.size();) {
            std::size_t matchLength = 0;
            std::size_t matchOffset = 0;
            if (position + 3 <= block.size()) {
                const std::uint32_t key = (static_cast<std::uint32_t>(block[position]) << 16U)
                    | (static_cast<std::uint32_t>(block[position + 1]) << 8U) | block[position + 2];
                const std::size_t slot = hash(key);
                const std::size_t candidate = last[slot];
                if (candidate != kMissing && position - candidate <= 65535U) {
                    while (matchLength < block.size() - position
                        && block[candidate + matchLength] == block[position + matchLength]) ++matchLength;
                    if (matchLength >= 3) matchOffset = position - candidate;
                }
                last[slot] = position;
            }
            if (matchLength >= 3) {
                const std::size_t limited = std::min<std::size_t>(matchLength, 0xFFFFFFFFU - 3U);
                const unsigned offsetBits = std::bit_width(static_cast<unsigned>(matchOffset)) - 1U;
                tokens.push_back({static_cast<std::uint16_t>(256U + (offsetBits << 4U)
                    + std::min<std::size_t>(limited - 3U, 15U)), static_cast<std::uint32_t>(limited),
                    static_cast<std::uint32_t>(matchOffset)});
                for (std::size_t i = 1; i < limited && position + i + 2 < block.size(); ++i) {
                    const auto key = (static_cast<std::uint32_t>(block[position + i]) << 16U)
                        | (static_cast<std::uint32_t>(block[position + i + 1]) << 8U)
                        | block[position + i + 2];
                    last[hash(key)] = position + i;
                }
                position += limited;
            } else {
                tokens.push_back({block[position], 1, 0});
                ++position;
            }
        }
        std::array<std::uint64_t, kSymbolCount> frequencies{};
        for (const Token& token : tokens) ++frequencies[token.symbol];
        std::array<std::uint8_t, kSymbolCount> lengths{};
        auto codes = BuildCodes(frequencies, lengths);
        if (!codes) {
            tokens.clear();
            tokens.reserve(block.size());
            frequencies.fill(0);
            for (const std::uint8_t value : block) {
                tokens.push_back({value, 1, 0});
                ++frequencies[value];
            }
            lengths.fill(0);
            codes = BuildCodes(frequencies, lengths);
        }
        if (!codes) return std::nullopt;
        for (std::size_t symbol = 0; symbol != kSymbolCount; symbol += 2)
            result.push_back(static_cast<std::uint8_t>(lengths[symbol] | (lengths[symbol + 1] << 4U)));
        BitWriter writer;
        for (const Token& token : tokens) {
            writer.Write((*codes)[token.symbol].bits, (*codes)[token.symbol].length);
            if (token.symbol >= 256U) {
                const std::uint32_t length = token.length - 3U;
                if ((token.symbol & 15U) == 15U) {
                    if (length < 270U) writer.WriteRawByte(static_cast<std::uint8_t>(length - 15U));
                    else if (length <= 65535U) { writer.WriteRawByte(255); writer.WriteRaw16(length); }
                    else { writer.WriteRawByte(255); writer.WriteRaw16(0); writer.WriteRaw32(length); }
                }
                const unsigned offsetBits = (token.symbol - 256U) >> 4U;
                if (offsetBits != 0) writer.Write(token.offset - (1U << offsetBits), offsetBits);
            }
        }
        const auto encodedBlock = writer.Finish();
        result.insert(result.end(), encodedBlock.begin(), encodedBlock.end());
        blockOffset += blockSize;
    }
    return result;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool Read16(std::uint32_t& value) {
        if (position_ >= bytes_.size()) return false;
        const std::size_t available = bytes_.size() - position_;
        value = static_cast<std::uint32_t>(bytes_[position_]);
        if (available >= 2) value |= static_cast<std::uint32_t>(bytes_[position_ + 1]) << 8U;
        position_ += std::min<std::size_t>(available, 2);
        return true;
    }
    bool Init() {
        std::uint32_t first = 0, second = 0;
        if (!Read16(first) || !Read16(second)) return false;
        mask_ = (first << 16U) + second;
        bits_ = 32;
        return true;
    }
    bool ReadRaw16(std::uint32_t& value) {
        if (bytes_.size() - position_ < 2U) return false;
        value = static_cast<std::uint32_t>(bytes_[position_])
            | (static_cast<std::uint32_t>(bytes_[position_ + 1]) << 8U);
        position_ += 2U;
        return true;
    }
    bool ReadBits(unsigned count, std::uint32_t& value) {
        if (count > 16U) return false;
        value = count == 0 ? 0 : mask_ >> (32U - count);
        mask_ <<= count;
        bits_ -= static_cast<int>(count);
        if (bits_ < 16) {
            std::uint32_t next = 0;
            if (!Read16(next)) return false;
            mask_ += next << static_cast<unsigned>(16 - bits_);
            bits_ += 16;
        }
        return true;
    }
    bool PeekBits(unsigned count, std::uint32_t& value) const {
        if (count > 16U || bits_ < static_cast<int>(count)) return false;
        value = count == 0 ? 0 : mask_ >> (32U - count);
        return true;
    }
    bool ReadByte(std::uint8_t& value) {
        if (position_ >= bytes_.size()) return false;
        value = bytes_[position_++];
        return true;
    }
    [[nodiscard]] std::size_t Position() const noexcept { return position_; }
    [[nodiscard]] std::size_t BoundaryPosition() const noexcept {
        return position_;
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
    std::uint32_t mask_ = 0;
    int bits_ = 0;
};

struct DecodeNode { int child[2] = {-1, -1}; int symbol = -1; };

std::optional<std::vector<DecodeNode>> BuildTree(std::span<const std::uint8_t> table) {
    if (table.size() < kTableBytes) return std::nullopt;
    std::array<std::uint8_t, kSymbolCount> lengths{};
    for (std::size_t i = 0; i != kTableBytes; ++i) {
        lengths[i * 2] = table[i] & 0x0FU;
        lengths[i * 2 + 1] = table[i] >> 4U;
    }
    std::array<std::uint16_t, 16> count{};
    for (std::uint8_t length : lengths) {
        if (length != 0) ++count[length];
    }
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (unsigned length = 1; length <= 15; ++length) {
        code = (code + count[length - 1]) << 1U;
        next[length] = code;
    }
    std::vector<DecodeNode> tree(1);
    for (std::size_t symbol = 0; symbol != kSymbolCount; ++symbol) {
        const unsigned length = lengths[symbol];
        if (length == 0) continue;
        if (length > 15U || next[length] >= (1U << length)) return std::nullopt;
        int node = 0;
        const std::uint32_t symbolCode = next[length]++;
        for (unsigned bit = length; bit != 0; --bit) {
            const int branch = static_cast<int>((symbolCode >> (bit - 1U)) & 1U);
            if (tree[node].symbol >= 0) return std::nullopt;
            if (tree[node].child[branch] < 0) {
                tree[node].child[branch] = static_cast<int>(tree.size());
                tree.push_back({});
            }
            node = tree[node].child[branch];
        }
        if (tree[node].symbol >= 0 || tree[node].child[0] >= 0 || tree[node].child[1] >= 0) return std::nullopt;
        tree[node].symbol = static_cast<int>(symbol);
    }
    return tree;
}
std::optional<std::vector<std::uint8_t>> DecodeStream(
    std::span<const std::uint8_t> stream,
    std::uint64_t expectedSize) {
    if (expectedSize > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    std::vector<std::uint8_t> output;
    if (expectedSize > output.max_size()) return std::nullopt;
    std::size_t tableOffset = 0;
    while (output.size() < expectedSize) {
        if (stream.size() < tableOffset || stream.size() - tableOffset < kTableBytes) return std::nullopt;
        const auto tree = BuildTree(stream.subspan(tableOffset, kTableBytes));
        if (!tree) return std::nullopt;
        tableOffset += kTableBytes;
        BitReader bits(stream.subspan(tableOffset));
        if (!bits.Init()) return std::nullopt;
        std::size_t producedInBlock = 0;
        while (output.size() < expectedSize && producedInBlock < 65536U) {
            std::uint32_t value = 0;
            int node = 0;
            while ((*tree)[node].symbol < 0) {
                if (!bits.ReadBits(1, value)) return std::nullopt;
                const int next = (*tree)[node].child[value & 1U];
                if (next < 0) return std::nullopt;
                node = next;
            }
            const int symbol = (*tree)[node].symbol;
            if (symbol < 256) {
                output.push_back(static_cast<std::uint8_t>(symbol));
                ++producedInBlock;
                continue;
            }
            const unsigned offsetBits = static_cast<unsigned>((symbol - 256) >> 4);
            if (offsetBits > 15U || !bits.PeekBits(offsetBits, value)) return std::nullopt;
            const std::size_t offset = (std::size_t{1} << offsetBits) + value;
            unsigned lengthCode = static_cast<unsigned>((symbol - 256) & 15);
            if (lengthCode == 15U) {
                std::uint8_t extra = 0;
                if (!bits.ReadByte(extra)) return std::nullopt;
                lengthCode = extra + 15U;
                if (lengthCode == 270U) {
                    std::uint32_t low = 0;
                    if (!bits.ReadRaw16(low)) return std::nullopt;
                    lengthCode = low;
                    if (lengthCode == 0) {
                        std::uint32_t high = 0;
                        if (!bits.ReadRaw16(low) || !bits.ReadRaw16(high)) return std::nullopt;
                        lengthCode = (high << 16U) | low;
                    }
                }
            }
            const std::uint64_t length = static_cast<std::uint64_t>(lengthCode) + 3U;
            if (offset == 0 || offset > output.size()
                || length > expectedSize - output.size()) return std::nullopt;
            if (!bits.ReadBits(offsetBits, value)) return std::nullopt;
            for (std::uint64_t i = 0; i != length; ++i) output.push_back(output[output.size() - offset]);
            producedInBlock += static_cast<std::size_t>(length);
        }
        tableOffset += bits.BoundaryPosition();
    }
    return output;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> CompressXpressBuffer(std::span<const std::uint8_t> input) {
    if (input.empty()) return std::vector<std::uint8_t>{};
    try {
        if (input.size() > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        const auto stream = EncodeLiteralStream(input);
        const bool useRaw = !stream || stream->size() + 4U >= input.size();
        const std::size_t bodySize = useRaw ? input.size() : stream->size();
        if (bodySize > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        std::vector<std::uint8_t> result;
        result.reserve(kHeaderBytes + bodySize);
        AppendU32(result, kMagic);
        result.push_back(static_cast<std::uint8_t>(kHeaderSize));
        result.push_back(static_cast<std::uint8_t>(kHeaderSize >> 8U));
        result.push_back(0);
        result.push_back(kModeXpressHuffman);
        AppendU32(result, static_cast<std::uint32_t>(input.size()));
        AppendU32(result, 0);
        AppendU32(result, static_cast<std::uint32_t>(input.size()));
        AppendU32(result, 0);
        AppendU32(result, static_cast<std::uint32_t>(bodySize));
        if (useRaw) result.insert(result.end(), input.begin(), input.end());
        else result.insert(result.end(), stream->begin(), stream->end());
        result[6] = HeaderCrc(result);
        return result;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

std::optional<std::vector<std::uint8_t>> DecompressXpressBuffer(
    std::span<const std::uint8_t> input,
    std::uint64_t expectedSize) {
    if (expectedSize == 0) {
        return input.empty()
            ? std::optional<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{})
            : std::nullopt;
    }
    if (expectedSize > std::numeric_limits<std::uint32_t>::max()
        || input.size() < kHeaderBytes
        || ReadU32(input, 0) != kMagic
        || static_cast<std::uint16_t>(input[4] | (static_cast<std::uint16_t>(input[5]) << 8U)) != kHeaderSize
        || input[7] != kModeXpressHuffman
        || input[6] != HeaderCrc(input)) return std::nullopt;
    const std::uint32_t size = ReadU32(input, 8);
    const std::uint32_t chunkSize = ReadU32(input, 16);
    const std::uint32_t bodySize = ReadU32(input, 24);
    if (size != expectedSize || chunkSize != size || ReadU32(input, 12) != 0
        || ReadU32(input, 20) != 0 || bodySize != input.size() - kHeaderBytes) return std::nullopt;
    const auto body = input.subspan(kHeaderBytes);
    try {
        if (bodySize == size) return std::vector<std::uint8_t>(body.begin(), body.end());
        return DecodeStream(body, expectedSize);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

}  // namespace cnote
