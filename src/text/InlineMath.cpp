#include "text/InlineMath.h"

#include <cstddef>
#include <vector>

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

// Positions that end a formula search: a valid closing `$`, or something that
// aborts the search (an unescaped line break or a two-dollar run).  The probe
// walks forward from an opening `$` until it meets one; remembering the next
// one for every position makes the whole scan linear instead of rescanning
// the tail of the line for every unmatched opening dollar.
enum class ScanEvent : unsigned char {
    None,
    Closing,
    Abort,
};

}  // namespace

std::vector<InlineMathRange> FindInlineMath(std::u16string_view content) {
    std::vector<InlineMathRange> ranges;
    const std::size_t size = content.size();
    if (size < 2U) {
        return ranges;
    }

    // A unit is escaped when the unit before it is a backslash that is itself
    // unescaped: exactly the pairs the probe skips.  Escaped units are never
    // events (an escaped dollar is literal text and an escaped line break does
    // not end the search).
    std::vector<char> escaped(size, 0);
    std::vector<ScanEvent> event(size, ScanEvent::None);
    for (std::size_t index = 0; index < size; ++index) {
        if (index > 0U && content[index - 1U] == u'\\' && escaped[index - 1U] == 0) {
            escaped[index] = 1;
            continue;
        }
        const char16_t character = content[index];
        if (IsLineBreak(character)) {
            event[index] = ScanEvent::Abort;
            continue;
        }
        if (character != u'$') {
            continue;
        }
        const bool hasNext = index + 1U < size;
        if (hasNext && content[index + 1U] == u'$') {
            // A two-dollar run ends any search that reaches it.
            event[index] = ScanEvent::Abort;
            continue;
        }
        // The closing dollar must follow a non-space and must not be followed
        // by a digit ("$5 and $10" stays text).
        if (index > 0U && !IsMathSpace(content[index - 1U])
            && !(hasNext && IsAsciiDigit(content[index + 1U]))) {
            event[index] = ScanEvent::Closing;
        }
    }

    // nextEvent[index] is the first position >= index carrying an event, or
    // `size` when the rest of the content has none.
    std::vector<std::size_t> nextEvent(size + 1U, size);
    for (std::size_t index = size; index-- > 0U;) {
        nextEvent[index] = event[index] != ScanEvent::None ? index : nextEvent[index + 1U];
    }

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

        const std::size_t closing = nextEvent[index + 1U];
        if (closing < size && event[closing] == ScanEvent::Closing) {
            ranges.push_back({ static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(closing + 1U) });
            index = closing + 1U;
        } else {
            ++index;
        }
    }
    return ranges;
}

}  // namespace cnote::text
