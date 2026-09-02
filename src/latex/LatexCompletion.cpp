#include "latex/LatexCompletion.h"

#include <algorithm>
#include <array>

namespace cnote::latex {
namespace {

struct CatalogItem {
    const wchar_t* command;
    const wchar_t* description;
};

constexpr std::array<CatalogItem, 50> kCatalog {{
    {L"\\alpha", L"Greek letter alpha"},
    {L"\\approx", L"Approximately equal"},
    {L"\\begin", L"Begin an environment"},
    {L"\\beta", L"Greek letter beta"},
    {L"\\cdot", L"Centered multiplication dot"},
    {L"\\cos", L"Cosine function"},
    {L"\\dfrac", L"Display-style fraction"},
    {L"\\delta", L"Greek letter delta"},
    {L"\\end", L"End an environment"},
    {L"\\equiv", L"Equivalent"},
    {L"\\exp", L"Exponential function"},
    {L"\\frac", L"Fraction"},
    {L"\\gamma", L"Greek letter gamma"},
    {L"\\geq", L"Greater than or equal"},
    {L"\\in", L"Element of set"},
    {L"\\infty", L"Infinity"},
    {L"\\int", L"Integral"},
    {L"\\item", L"Environment list item"},
    {L"\\lambda", L"Greek letter lambda"},
    {L"\\leq", L"Less than or equal"},
    {L"\\left", L"Left delimiter"},
    {L"\\lim", L"Limit operator"},
    {L"\\mathbb", L"Blackboard-bold letters"},
    {L"\\mathbf", L"Bold math letters"},
    {L"\\mathcal", L"Calligraphic letters"},
    {L"\\mathrm", L"Roman math letters"},
    {L"\\nabla", L"Nabla operator"},
    {L"\\neq", L"Not equal"},
    {L"\\omega", L"Greek letter omega"},
    {L"\\overline", L"Overline"},
    {L"\\partial", L"Partial derivative"},
    {L"\\phi", L"Greek letter phi"},
    {L"\\pi", L"Greek letter pi"},
    {L"\\pm", L"Plus or minus"},
    {L"\\prod", L"Product"},
    {L"\\right", L"Right delimiter"},
    {L"\\rho", L"Greek letter rho"},
    {L"\\sigma", L"Greek letter sigma"},
    {L"\\sin", L"Sine function"},
    {L"\\sqrt", L"Square root"},
    {L"\\sum", L"Summation"},
    {L"\\tan", L"Tangent function"},
    {L"\\text", L"Text inside a formula"},
    {L"\\theta", L"Greek letter theta"},
    {L"\\times", L"Multiplication sign"},
    {L"\\tilde", L"Tilde accent"},
    {L"\\underline", L"Underline"},
    {L"\\vec", L"Vector accent"},
    {L"\\varepsilon", L"Variant epsilon"},
    {L"\\zeta", L"Greek letter zeta"},
}};

bool IsCommandLetter(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
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
            suggestions.push_back({std::wstring(command), item.description});
            if (suggestions.size() == kMaximumSuggestions) {
                break;
            }
        }
    }
    return suggestions;
}

}  // namespace cnote::latex
