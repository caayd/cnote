#include "text/InlineMath.h"

namespace cnote::text {

namespace {

bool IsMathSpace(char16_t character) {
    switch (character) {
    case u' ':
    case u'\t':
    case u'\n':
    case u'\r':
    case u'\f':
    case u'\v':
    case u'\u00A0':
    case u'\u2028':
    case u'\u2029':
    case u'\u202F':
    case u'\u205F':
    case u'\u3000':
        return true;
    default:
        return character >= u'\u2000' && character <= u'\u200A';
    }
}

bool IsLineBreak(char16_t character) {
    return character == u'\n' || character == u'\r' || character == u'\u2028' || character == u'\u2029';
}

bool IsAsciiDigit(char16_t character) {
    return character >= u'0' && character <= u'9';
}

}  // namespace

std::vector<InlineMathRange> FindInlineMath(std::u16string_view content) {
    std::vector<InlineMathRange> ranges;
    const std::size_t size = content.size();
    std::size_t index = 0;
    while (index < size) {
        const char16_t character = content[index];
        if (character == u'\\') {
            index += 2U;
            continue;
        }
        if (character != u'$') {
            ++index;
            continue;
        }
        std::size_t runEnd = index + 1U;
        while (runEnd < size && content[runEnd] == u'$') {
            ++runEnd;
        }
        if (runEnd - index >= 2U) {
            index = runEnd;
            continue;
        }
        if (index + 1U >= size || IsMathSpace(content[index + 1U])) {
            ++index;
            continue;
        }

        std::size_t closing = size;
        for (std::size_t probe = index + 1U; probe < size;) {
            const char16_t candidate = content[probe];
            if (IsLineBreak(candidate)) {
                break;
            }
            if (candidate == u'\\') {
                probe += 2U;
                continue;
            }
            if (candidate == u'$') {
                const bool hasNext = probe + 1U < size;
                if (hasNext && content[probe + 1U] == u'$') {
                    break;
                }
                if (!IsMathSpace(content[probe - 1U]) && !(hasNext && IsAsciiDigit(content[probe + 1U]))) {
                    closing = probe;
                    break;
                }
            }
            ++probe;
        }
        if (closing == size) {
            ++index;
            continue;
        }
        ranges.push_back({ static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(closing + 1U) });
        index = closing + 1U;
    }
    return ranges;
}

}  // namespace cnote::text
