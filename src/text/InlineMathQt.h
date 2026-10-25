#pragma once

#include <string_view>
#include <vector>

#include <QTextFormat>

#include "text/InlineMath.h"

class QTextCharFormat;
class QTextDocument;

namespace cnote::text {

// A `$...$` formula rendered inside a QTextDocument is one U+FFFC character
// whose char format carries this object type, the LaTeX source, the display
// scale the document was formatted at and the surrounding text's font size
// (page units).  The placeholder keeps the run's colour; its font size is
// enlarged for deep formulas to reserve line space, so the text size lives
// in its own property.
inline constexpr int kInlineMathObjectType = QTextFormat::UserObject + 1;
inline constexpr int kInlineMathSourceProperty = QTextFormat::UserProperty + 1;
inline constexpr int kInlineMathScaleProperty = QTextFormat::UserProperty + 2;
inline constexpr int kInlineMathFontSizeProperty = QTextFormat::UserProperty + 3;
// The point size MakeInlineMathFormat gave the placeholder.  A different
// font size on the placeholder means the user restyled it (toolbar size),
// which then wins over kInlineMathFontSizeProperty.
inline constexpr int kInlineMathPlaceholderPointSizeProperty = QTextFormat::UserProperty + 4;

// The formulas in `content` that will render: FindInlineMath ranges whose
// source parses at `fontSize`.  Source that does not parse (or a missing
// LaTeX engine) stays plain text, so a typo never makes words disappear.
[[nodiscard]] std::vector<InlineMathRange> RenderableInlineMath(std::u16string_view content, float fontSize);

// Installs the measuring/drawing handler for kInlineMathObjectType on the
// document's layout.  Safe to call repeatedly.
void RegisterInlineMathHandler(QTextDocument& document);

// Turns the placeholder character's (already styled) format into an inline
// math object for `source`, choosing the vertical alignment that puts the
// formula baseline on the text baseline.
void MakeInlineMathFormat(QTextCharFormat& format, std::u16string_view source, float displayScale);

}  // namespace cnote::text
