#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QtCore/qnamespace.h>

#include "Document.h"
#include "latex/LatexCompletion.h"

class QFont;
class QString;
class QTextCharFormat;
class QTextDocument;

namespace cnote::text {

// The stored font size is pixel-like at the 96-DPI reference scale; Qt wants
// points.  Page zoom and DPR are applied by the callers, never baked into the
// model.  Exported so callers that reason in the popup's point units (the
// text-format popup's pending size) can convert both ways.
inline constexpr float kPointsPerPageUnit = 72.0f / 96.0f;

// --- style conversion (authoritative for live editing and rendering) --------
// The Qt family name for a model font family; the single mapping shared by
// FontForStyle and the toolbar's format application.  The bundled families
// (Shantell Sans) are registered with the application font database on first
// use.
[[nodiscard]] QString TextFontFamilyName(TextFontFamily family);

// displayScale multiplies font sizes for on-screen editing at a zoomed view;
// the model always stores unscaled (page-unit) sizes, so capture divides the
// same factor back out.  Rendering code passes the default 1.0.
[[nodiscard]] QFont FontForStyle(const PageTextStyle& style, float displayScale = 1.0f);
[[nodiscard]] QTextCharFormat CharFormatForStyle(const PageTextStyle& style, float displayScale = 1.0f);
[[nodiscard]] PageTextStyle StyleForCharFormat(const QTextCharFormat& format, const PageTextStyle& fallback, float displayScale = 1.0f);

// Applies the model's default style and every span to the document.
void ApplyPageTextFormatting(QTextDocument& document, const PageText& text, float displayScale = 1.0f);

// Captures the document's character formats back into spans in model UTF-16
// offsets.  displayScale must match the scale the document was formatted with.
[[nodiscard]] std::vector<PageTextStyleSpan> CapturePageTextFormatting(const QTextDocument& document, const PageText& text, float displayScale = 1.0f);

// --- offset mapping ---------------------------------------------------------

// Maps model UTF-16 offsets to QTextDocument offsets (a CRLF pair becomes one
// paragraph separator) and back.
struct TextOffsetMap {
    std::u16string normalized;
    std::vector<std::uint32_t> modelToDocument;
    std::vector<std::uint32_t> documentToModel;
};

[[nodiscard]] TextOffsetMap BuildTextOffsetMap(std::u16string_view content);
[[nodiscard]] std::uint32_t DocumentOffsetForModel(const TextOffsetMap& map, std::uint32_t modelOffset);
[[nodiscard]] std::uint32_t ModelOffsetForDocument(const TextOffsetMap& map, std::uint32_t documentOffset);

// --- commit normalization ---------------------------------------------------

// Normalizes CRLF/CR to LF and remaps every span into the normalized offsets.
[[nodiscard]] PageText NormalizeCommittedText(const PageText& text);

// --- editing decisions ------------------------------------------------------

// True when committing an empty object should delete it.
[[nodiscard]] bool ShouldDeleteEmptyObject(std::u16string_view content);

struct TextRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

// Ctrl+Backspace range: from the cursor back over trailing whitespace and the
// preceding word.
[[nodiscard]] TextRange WordDeletionRange(std::u16string_view content, std::uint32_t cursor);

// --- LaTeX editing ----------------------------------------------------------

struct LatexEditResult {
    std::u16string content;
    std::uint32_t cursor = 0;
};

// Wraps the selection in a matching pair, or skips over an existing closing
// character. Returns nullopt when the typed character is not a pair character.
[[nodiscard]] std::optional<LatexEditResult> ApplyLatexAutoPair(
    std::u16string_view content,
    std::uint32_t selectionStart,
    std::uint32_t selectionEnd,
    char16_t typed);

enum class LatexSuggestionKeyAction {
    None,
    Accept,
    SelectPrevious,
    SelectNext,
    PageUp,
    PageDown,
    Dismiss,
};

[[nodiscard]] LatexSuggestionKeyAction LatexSuggestionActionForKey(Qt::Key key);

struct LatexCompletionDecision {
    bool show = false;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::vector<latex::CompletionItem> suggestions;
};

[[nodiscard]] LatexCompletionDecision LatexCompletionFor(std::u16string_view content, std::uint32_t cursor);

[[nodiscard]] std::optional<LatexEditResult> ApplyLatexCompletion(
    std::u16string_view content,
    std::uint32_t cursor,
    const latex::CompletionItem& item);

}  // namespace cnote::text
