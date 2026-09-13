#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "Document.h"

namespace cnote {

// CTS selection payload: value collections plus the content bounds used for
// paste centering. Carries no platform handles.
struct SelectionClipboardData {
    std::vector<Stroke> strokes;
    std::vector<PageText> texts;
    std::vector<PageLatex> latexes;
    std::vector<PageImage> images;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

// CTS8 writer order, explicit little-endian fields and two-byte UTF-16LE.
[[nodiscard]] std::vector<std::uint8_t> EncodeSelectionClipboard(const SelectionClipboardData& data);

// Accepts CTS1..CTS8. Returns nullopt for unknown magic or truncated/overflowing
// records, so callers can leave the document untouched.
[[nodiscard]] std::optional<SelectionClipboardData> DecodeSelectionClipboard(std::span<const std::uint8_t> bytes);

}  // namespace cnote
