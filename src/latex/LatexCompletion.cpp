#include "latex/LatexCompletion.h"

#include <algorithm>
#include <array>

namespace cnote::latex {
namespace {

struct CatalogItem {
    const wchar_t* command;
};

constexpr std::array<CatalogItem, 50> kCatalog {{
    {L"\\alpha"},
    {L"\\approx"},
    {L"\\begin"},
    {L"\\beta"},
    {L"\\cdot"},
    {L"\\cos"},
    {L"\\dfrac"},
    {L"\\delta"},
    {L"\\end"},
    {L"\\equiv"},
    {L"\\exp"},
    {L"\\frac"},
    {L"\\gamma"},
    {L"\\geq"},
    {L"\\in"},
    {L"\\infty"},
    {L"\\int"},
    {L"\\item"},
    {L"\\lambda"},
    {L"\\leq"},
    {L"\\left"},
    {L"\\lim"},
    {L"\\mathbb"},
    {L"\\mathbf"},
    {L"\\mathcal"},
    {L"\\mathrm"},
    {L"\\nabla"},
    {L"\\neq"},
    {L"\\omega"},
    {L"\\overline"},
    {L"\\partial"},
    {L"\\phi"},
    {L"\\pi"},
    {L"\\pm"},
    {L"\\prod"},
    {L"\\right"},
    {L"\\rho"},
    {L"\\sigma"},
    {L"\\sin"},
    {L"\\sqrt"},
    {L"\\sum"},
    {L"\\tan"},
    {L"\\text"},
    {L"\\theta"},
    {L"\\times"},
    {L"\\tilde"},
    {L"\\underline"},
    {L"\\vec"},
    {L"\\varepsilon"},
    {L"\\zeta"},
}};
bool IsCommandLetter(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
}

CompletionItem MakeCompletionItem(std::wstring_view command) {
    CompletionItem item {
        std::wstring(command),
        std::wstring(command),
        command.size(),
    };
    if (command == L"\\frac" || command == L"\\dfrac") {
        item.insertion += L"{}{}";
        item.cursorOffset = command.size() + 1U;
    } else if (command == L"\\begin"
        || command == L"\\end"
        || command == L"\\sqrt"
        || command == L"\\mathbb"
        || command == L"\\mathbf"
        || command == L"\\mathcal"
        || command == L"\\mathrm"
        || command == L"\\overline"
        || command == L"\\text"
        || command == L"\\tilde"
        || command == L"\\underline"
        || command == L"\\vec") {
        item.insertion += L"{}";
        item.cursorOffset = command.size() + 1U;
    }
    return item;
}
}  // namespace


std::optional<CompletionContext> FindCompletionContext(std::wstring_view text, std::size_t cursor) {
    cursor = std::min(cursor, text.size());
    std::size_t commandStart = cursor;
    while (commandStart > 0 && IsCommandLetter(text[commandStart - 1U])) {
        --commandStart;
    }
    if (commandStart == 0 || text[commandStart - 1U] != L'\\') {
        return std::nullopt;
    }
    if (commandStart >= 2U && text[commandStart - 2U] == L'\\') {
        return std::nullopt;
    }

    return CompletionContext {
        commandStart - 1U,
        cursor,
        std::wstring(text.substr(commandStart - 1U, cursor - commandStart + 1U)),
    };
}

std::vector<CompletionItem> SuggestionsFor(std::wstring_view prefix) {
    std::vector<CompletionItem> suggestions;
    if (prefix.empty() || prefix.front() != L'\\') {
        return suggestions;
    }

    constexpr std::size_t kMaximumSuggestions = 8;
    for (const CatalogItem& item : kCatalog) {
        const std::wstring_view command(item.command);
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
