#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cnote::latex {

struct CompletionContext {
    std::size_t start = 0;
    std::size_t end = 0;
    std::u16string prefix;
};

struct CompletionItem {
    std::u16string command;
    std::u16string insertion;
    std::size_t cursorOffset = 0;
};

// Returns the active \\command immediately before the cursor, if any.
[[nodiscard]] std::optional<CompletionContext> FindCompletionContext(std::u16string_view text, std::size_t cursor);

// Returns common MicroTeX commands whose names begin with prefix, in display order.
[[nodiscard]] std::vector<CompletionItem> SuggestionsFor(std::u16string_view prefix);

}  // namespace cnote::latex
