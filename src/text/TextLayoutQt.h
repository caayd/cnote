#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Document.h"

class QPainter;

namespace cnote::text {

// One wrapped line of a PageText. `start` is a UTF-16 code-unit offset into
// PageText::content; `text` is the line with trailing layout whitespace trimmed.
struct QtTextLine {
    std::u16string text;
    std::uint32_t start = 0;
};

struct QtTextLayout {
    std::vector<QtTextLine> lines;
    int lineHeight = 0;
    int height = 0;
};

// Lays out a PageText on the fixed 96-DPI reference device: point size is the
// stored pixel-like font size * 72/96, document margins are zero, the wrap
// width is explicit, and paragraph alignment/styles/spans are applied. Line
// starts stay UTF-16 code units and empty paragraphs are preserved. Never
// rewrites model values.
[[nodiscard]] QtTextLayout LayoutPageTextQt(const PageText& text, float pageWidth);

struct QtTextBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// Bounds in page units (same space as PageText::x/y).
[[nodiscard]] QtTextBounds MeasurePageTextBoundsQt(const PageText& text, float pageWidth);

// Draws the text with its top-left at (x, y) in the painter's current
// coordinate space, scaled by `scale`. Glyphs are painted transparently; no
// background rectangle is ever filled.
void DrawPageTextQt(QPainter& painter, const PageText& text, float pageWidth, float scale, float x, float y, float opacity);

// Diagnostics: number of layouts actually built (cache misses).
[[nodiscard]] std::uint64_t TextLayoutCacheMisses();

// Signature covering everything that affects the layout; used by render caches.
[[nodiscard]] std::uint64_t TextLayoutSignature(const PageText& text, float pageWidth);

}  // namespace cnote::text
