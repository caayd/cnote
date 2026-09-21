#pragma once

#include <QImage>
#include <QString>

#include "document/SelectionClipboardCodec.h"

namespace cnote::clipboard {

// Qt MIME type carrying the CTS selection payload (the codec's bytes) on every
// platform.  Another cnote instance - or any future Qt frontend - reads the
// selection back through this name.
inline constexpr char kSelectionMimeType[] = "application/x-cnote-selection";

// Windows-only companion flavor: Qt's Windows clipboard converter maps the
// mime type naming a custom CF format to that registered format in both
// directions, so the legacy frontend's `RegisterClipboardFormatW(
// L"cnote.selection.v1")` data is produced and consumed without either
// frontend knowing about the other's representation.  The name is spelled out
// here because it is the legacy on-the-wire contract, not an implementation
// detail (src/document/Clipboard.cpp registers the same string).
inline constexpr char kWindowsSelectionMimeType[] =
    "application/x-qt-windows-mime;value=\"cnote.selection.v1\"";

// The three clipboard flavors the Qt frontend knows, in paste priority order.
enum class ClipboardKind {
    None,
    Selection,
    Image,
    Text,
};

// Highest-priority clipboard content.  Exactly one payload field matches
// `kind`; `kind == None` means nothing usable is on the clipboard.
struct ClipboardContent {
    ClipboardKind kind = ClipboardKind::None;
    SelectionClipboardData selection {};
    QImage image {};
    QString text {};
};

// Writes the structured selection: the CTS codec bytes under
// kSelectionMimeType, plus the same bytes under the Windows bridge name on
// Windows.  A selection consisting of exactly one image object also publishes
// that image as ordinary image data, so non-cnote applications can paste the
// picture; the structured flavor comes first and Read() still prefers it.
//
// Returns false - leaving the clipboard untouched - when `data` holds no
// objects, mirroring the legacy copy path's refusal to publish an empty
// selection.  Selecting the document's selection state, paste centering and
// object identity are the document side's business and are not done here.
[[nodiscard]] bool WriteSelection(const SelectionClipboardData& data);

// Writes ordinary image data (the paste fallback for pictures from other
// applications).  Returns false for a null image.
[[nodiscard]] bool WriteImage(const QImage& image);

// Writes plain text - what copying selected PDF text publishes.  Returns false
// for empty text.
[[nodiscard]] bool WritePlainText(const QString& text);

// Reads the highest-priority flavor: the structured CTS MIME type (native name
// first, then the Windows bridge for legacy writers), otherwise an ordinary
// image, otherwise plain text.
//
// A structured flavor that is present but cannot be decoded - or that decodes
// to zero objects - yields ClipboardKind::None instead of falling through to
// image/text: corrupt custom bytes must never be silently replaced by another
// flavor and mutate the document.
[[nodiscard]] ClipboardContent Read();

// Converts a codec image object to a QImage.  The model stores top-down
// straight-alpha BGRA rows, which maps to QImage::Format_ARGB32 with the
// object's stride; an object whose dimensions/stride/pixels are inconsistent
// yields a null image.
[[nodiscard]] QImage QImageFromPageImage(const PageImage& image);

// Inverse of QImageFromPageImage: the model's BGRA pixel payload for a QImage.
// Position/size are left at zero for the document side to place.
[[nodiscard]] PageImage PageImageFromQImage(const QImage& image);

}  // namespace cnote::clipboard
