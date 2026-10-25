#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace cnote::text {

// One `$...$` inline formula in a text object's content (UTF-16 offsets).
// [start, end) covers both delimiters; the LaTeX source is the inside.
struct InlineMathRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;

    [[nodiscard]] std::uint32_t SourceStart() const { return start + 1U; }
    [[nodiscard]] std::uint32_t SourceLength() const { return end - start - 2U; }
    friend bool operator==(const InlineMathRange&, const InlineMathRange&) = default;
};

// Finds inline math with Pandoc's tex_math_dollars rules, so ordinary prose
// is never captured:
//   * the opening `$` must be followed by a non-space character;
//   * the closing `$` must follow a non-space character and must not be
//     followed by a digit ("$5 and $10" stays text);
//   * a backslash escapes the next character (`\$` is a literal dollar);
//   * formulas never span a line break;
//   * runs of two or more `$` are literal text (display math is not
//     supported) and end any formula search that reaches them.
// Ranges are returned in order and never overlap.
[[nodiscard]] std::vector<InlineMathRange> FindInlineMath(std::u16string_view content);

}  // namespace cnote::text
