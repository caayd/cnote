#include "latex/LatexCompletion.h"

#include <algorithm>
#include <array>

namespace cnote::latex {
namespace {

struct CatalogItem {
    const char16_t* command;
};

constexpr std::array<CatalogItem, 50> kCatalog {{
    {u"\\alpha"},
    {u"\\approx"},
    {u"\\begin"},
    {u"\\beta"},
    {u"\\cdot"},
    {u"\\cos"},
    {u"\\dfrac"},
    {u"\\delta"},
    {u"\\end"},
    {u"\\equiv"},
    {u"\\exp"},
    {u"\\frac"},
    {u"\\gamma"},
    {u"\\geq"},
    {u"\\in"},
    {u"\\infty"},
    {u"\\int"},
    {u"\\item"},
    {u"\\lambda"},
    {u"\\leq"},
    {u"\\left"},
    {u"\\lim"},
    {u"\\mathbb"},
    {u"\\mathbf"},
    {u"\\mathcal"},
    {u"\\mathrm"},
    {u"\\nabla"},
    {u"\\neq"},
    {u"\\omega"},
    {u"\\overline"},
    {u"\\partial"},
    {u"\\phi"},
    {u"\\pi"},
    {u"\\pm"},
    {u"\\prod"},
    {u"\\right"},
    {u"\\rho"},
    {u"\\sigma"},
    {u"\\sin"},
    {u"\\sqrt"},
    {u"\\sum"},
    {u"\\tan"},
    {u"\\text"},
    {u"\\theta"},
    {u"\\times"},
    {u"\\tilde"},
    {u"\\underline"},
    {u"\\vec"},
    {u"\\varepsilon"},
    {u"\\zeta"},
}};
bool IsCommandLetter(char16_t ch) {
    return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z');
}

CompletionItem MakeCompletionItem(std::u16string_view command) {
    CompletionItem item {
        std::u16string(command),
        std::u16string(command),
        command.size(),
    };
    if (command == u"\\frac" || command == u"\\dfrac") {
        item.insertion += u"{}{}";
        item.cursorOffset = command.size() + 1U;
    } else if (command == u"\\begin"
        || command == u"\\end"
        || command == u"\\sqrt"
        || command == u"\\mathbb"
        || command == u"\\mathbf"
        || command == u"\\mathcal"
        || command == u"\\mathrm"
        || command == u"\\overline"
        || command == u"\\text"
        || command == u"\\tilde"
        || command == u"\\underline"
        || command == u"\\vec") {
        item.insertion += u"{}";
        item.cursorOffset = command.size() + 1U;
    }
    return item;
}
}  // namespace


std::optional<CompletionContext> FindCompletionContext(std::u16string_view text, std::size_t cursor) {
    cursor = std::min(cursor, text.size());
    std::size_t commandStart = cursor;
    while (commandStart > 0 && IsCommandLetter(text[commandStart - 1U])) {
        --commandStart;
    }
    if (commandStart == 0 || text[commandStart - 1U] != u'\\') {
        return std::nullopt;
    }
    if (commandStart >= 2U && text[commandStart - 2U] == u'\\') {
        return std::nullopt;
    }

    return CompletionContext {
        commandStart - 1U,
        cursor,
        std::u16string(text.substr(commandStart - 1U, cursor - commandStart + 1U)),
    };
}

std::vector<CompletionItem> SuggestionsFor(std::u16string_view prefix) {
    std::vector<CompletionItem> suggestions;
    if (prefix.empty() || prefix.front() != u'\\') {
        return suggestions;
    }

    constexpr std::size_t kMaximumSuggestions = 8;
    for (const CatalogItem& item : kCatalog) {
        const std::u16string_view command(item.command);
        if (command.starts_with(prefix)) {
            suggestions.push_back(MakeCompletionItem(command));
            if (suggestions.size() == kMaximumSuggestions) {
                break;
            }
        }
    }
    return suggestions;
}

}  // namespace cnote::latex
