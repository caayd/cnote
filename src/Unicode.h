#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cnote {

// UTF-8 <-> UTF-16 conversion for persisted/editor text. Both directions are
// length-aware: embedded NUL is preserved and malformed input is replaced with
// U+FFFD rather than treated as a terminator or dropped.
[[nodiscard]] std::string Utf16ToUtf8(std::u16string_view value);
[[nodiscard]] std::u16string Utf8ToUtf16(std::string_view value);

// Path metadata crosses the wire as UTF-8; in memory it is a native path.
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path Utf8ToPath(std::string_view value);

}  // namespace cnote
