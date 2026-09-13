// Self-contained Qt clipboard-bridge test (migration plan step 2.6).
//
// Exercises the real OS clipboard through the Qt bridge: the structured CTS8
// round trip, flavor priority (structured > image > text), invalid/empty
// payload handling, and - on Windows - the Qt Windows MIME bridge that exposes
// the same bytes under the legacy registered format `cnote.selection.v1` in
// both directions.
//
// The OS clipboard is shared with every other process, so every publish/verify
// pair retries for a bounded time instead of assuming an uninterrupted
// clipboard.  Cross-application interop with the reference executable is not
// driven here (that needs GUI automation); the Windows section proves the
// equivalent property through the same OS clipboard and the codec's own
// decoder.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QString>

#include "document/SelectionClipboardCodec.h"
#include "document/SelectionClipboardQt.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using cnote::PageImage;
using cnote::PageLatex;
using cnote::PageText;
using cnote::SelectionClipboardData;
using cnote::Stroke;
using cnote::clipboard::ClipboardKind;

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

// Publishes and verifies clipboard content, retrying while another process
// holds or replaces the OS clipboard.
template <typename Publish, typename Verify>
bool PublishAndVerify(Publish publish, Verify verify) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        publish();
        if (verify()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return false;
}

const QMimeData* ClipboardMime() {
    return QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
}

void PublishMime(QMimeData* mime) {
    QGuiApplication::clipboard()->setMimeData(mime, QClipboard::Clipboard);
}

bool SameFloat(float a, float b) {
    return a == b;
}

// The pixel payload only; PageImageFromQImage deliberately leaves placement
// (x/y/width/height/rotation) at zero for the document side to fill in.
bool SameImagePixels(const PageImage& a, const PageImage& b) {
    return a.pixelWidth == b.pixelWidth && a.pixelHeight == b.pixelHeight && a.stride == b.stride
        && a.pixels == b.pixels;
}

bool SameImage(const PageImage& a, const PageImage& b) {
    return SameFloat(a.x, b.x) && SameFloat(a.y, b.y) && SameFloat(a.width, b.width)
        && SameFloat(a.height, b.height) && SameFloat(a.rotationDegrees, b.rotationDegrees)
        && SameImagePixels(a, b);
}

bool SameSelection(const SelectionClipboardData& a, const SelectionClipboardData& b) {
    if (!SameFloat(a.minX, b.minX) || !SameFloat(a.minY, b.minY)
        || !SameFloat(a.maxX, b.maxX) || !SameFloat(a.maxY, b.maxY)) {
        return false;
    }
    if (a.strokes.size() != b.strokes.size() || a.texts.size() != b.texts.size()
        || a.latexes.size() != b.latexes.size() || a.images.size() != b.images.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.strokes.size(); ++i) {
        const Stroke& left = a.strokes[i];
        const Stroke& right = b.strokes[i];
        if (left.style.color != right.style.color || !SameFloat(left.style.width, right.style.width)
            || left.style.dotted != right.style.dotted || !SameFloat(left.smoothing, right.smoothing)
            || left.points.size() != right.points.size()) {
            return false;
        }
        for (std::size_t p = 0; p < left.points.size(); ++p) {
            if (!SameFloat(left.points[p].x, right.points[p].x)
                || !SameFloat(left.points[p].y, right.points[p].y)) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < a.texts.size(); ++i) {
        const PageText& left = a.texts[i];
        const PageText& right = b.texts[i];
        if (!SameFloat(left.x, right.x) || !SameFloat(left.y, right.y) || !SameFloat(left.width, right.width)
            || !SameFloat(left.height, right.height) || !SameFloat(left.fontSize, right.fontSize)
            || left.color != right.color || left.alignment != right.alignment
            || left.fontFamily != right.fontFamily || left.bold != right.bold || left.italic != right.italic
            || left.underline != right.underline || left.content != right.content
            || left.styleSpans.size() != right.styleSpans.size()) {
            return false;
        }
        for (std::size_t s = 0; s < left.styleSpans.size(); ++s) {
            const cnote::PageTextStyleSpan& leftSpan = left.styleSpans[s];
            const cnote::PageTextStyleSpan& rightSpan = right.styleSpans[s];
            if (leftSpan.start != rightSpan.start || leftSpan.length != rightSpan.length
                || !SameFloat(leftSpan.style.fontSize, rightSpan.style.fontSize)
                || leftSpan.style.color != rightSpan.style.color
                || leftSpan.style.fontFamily != rightSpan.style.fontFamily
                || leftSpan.style.bold != rightSpan.style.bold
                || leftSpan.style.italic != rightSpan.style.italic
                || leftSpan.style.underline != rightSpan.style.underline) {
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < a.latexes.size(); ++i) {
        const PageLatex& left = a.latexes[i];
        const PageLatex& right = b.latexes[i];
        if (!SameFloat(left.x, right.x) || !SameFloat(left.y, right.y)
            || !SameFloat(left.fontSize, right.fontSize) || left.color != right.color
            || left.content != right.content) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.images.size(); ++i) {
        if (!SameImage(a.images[i], b.images[i])) {
            return false;
        }
    }
    return true;
}

PageImage MakeImage(std::uint8_t alpha) {
    PageImage image;
    image.x = 5.0f;
    image.y = 6.0f;
    image.width = 30.0f;
    image.height = 20.0f;
    image.rotationDegrees = 15.0f;
    image.pixelWidth = 3;
    image.pixelHeight = 2;
    image.stride = 12;
    image.pixels.assign(24U, 0U);
    for (int i = 0; i < 6; ++i) {
        image.pixels[static_cast<std::size_t>(i) * 4U + 0U] = static_cast<std::uint8_t>(1 + i);   // blue
        image.pixels[static_cast<std::size_t>(i) * 4U + 1U] = static_cast<std::uint8_t>(11 + i);  // green
        image.pixels[static_cast<std::size_t>(i) * 4U + 2U] = static_cast<std::uint8_t>(21 + i);  // red
        image.pixels[static_cast<std::size_t>(i) * 4U + 3U] = alpha;
    }
    return image;
}

SelectionClipboardData MakeSelection() {
    SelectionClipboardData data;
    Stroke stroke;
    stroke.style.color = 0xFF3366AAU;
    stroke.style.width = 4.5f;
    stroke.style.dotted = true;
    stroke.smoothing = 0.25f;
    stroke.points = { { 1.5f, 2.5f }, { -3.25f, 8.0f } };
    data.strokes.push_back(stroke);

    PageText text;
    text.id = 42;
    text.x = 10.0f;
    text.y = 20.0f;
    text.width = 180.0f;
    text.height = 60.0f;
    text.fontSize = 18.5f;
    text.color = 0xFF102030U;
    text.alignment = cnote::TextAlignment::Center;
    text.fontFamily = cnote::TextFontFamily::Serif;
    text.bold = true;
    text.italic = true;
    text.underline = true;
    text.content = u"A\U0001F600B\r\n\u03B1";
    text.styleSpans.push_back({ 3U, 1U, { 20.0f, 0xFFFF0000U, cnote::TextFontFamily::Sans, false, false, false } });
    data.texts.push_back(text);

    PageLatex latex;
    latex.id = 7;
    latex.x = 30.0f;
    latex.y = 40.0f;
    latex.fontSize = 22.0f;
    latex.color = 0xFF00FF00U;
    latex.content = u"\\frac{a}{b}";
    data.latexes.push_back(latex);

    data.images.push_back(MakeImage(255U));
    data.minX = -3.25f;
    data.minY = 2.5f;
    data.maxX = 200.0f;
    data.maxY = 120.0f;
    return data;
}

QByteArray BytesOf(const std::vector<std::uint8_t>& bytes) {
    return QByteArray(
        reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

#ifdef _WIN32

UINT SelectionFormat() {
    return RegisterClipboardFormatW(L"cnote.selection.v1");
}

std::vector<std::uint8_t> ReadRegisteredFormat(UINT format) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::vector<std::uint8_t> bytes;
        if (OpenClipboard(nullptr) != 0) {
            if (HANDLE handle = GetClipboardData(format)) {
                const SIZE_T size = GlobalSize(handle);
                if (const void* data = GlobalLock(handle)) {
                    const auto* begin = static_cast<const std::uint8_t*>(data);
                    bytes.assign(begin, begin + size);
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
            if (!bytes.empty()) {
                return bytes;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

bool WriteRegisteredFormat(UINT format, const std::vector<std::uint8_t>& bytes) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(nullptr) != 0) {
            EmptyClipboard();
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
            if (memory == nullptr) {
                CloseClipboard();
                return false;
            }
            void* destination = GlobalLock(memory);
            if (destination == nullptr) {
                GlobalFree(memory);
                CloseClipboard();
                return false;
            }
            std::copy(bytes.begin(), bytes.end(), static_cast<std::uint8_t*>(destination));
            GlobalUnlock(memory);
            const bool stored = SetClipboardData(format, memory) != nullptr;
            CloseClipboard();
            return stored;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    // --- the Qt MIME name is the inter-instance contract --------------------
    Check(QString::fromLatin1(cnote::clipboard::kSelectionMimeType) == QStringLiteral("application/x-cnote-selection"),
        "the structured flavor is published as application/x-cnote-selection");

    // --- structured round trip ----------------------------------------------
    const SelectionClipboardData original = MakeSelection();
    const std::vector<std::uint8_t> originalBytes = cnote::EncodeSelectionClipboard(original);
    const auto publishOriginal = [&] { static_cast<void>(cnote::clipboard::WriteSelection(original)); };

    Check(cnote::clipboard::WriteSelection(original), "a non-empty selection is published");
    Check(PublishAndVerify(publishOriginal, [&] {
            return cnote::clipboard::Read().kind == ClipboardKind::Selection;
        }),
        "the structured MIME type wins the paste priority");
    Check(PublishAndVerify(publishOriginal, [&] {
            return SameSelection(cnote::clipboard::Read().selection, original);
        }),
        "every decoded value collection matches the published one");
    Check(PublishAndVerify(publishOriginal, [&] {
            return cnote::EncodeSelectionClipboard(cnote::clipboard::Read().selection) == originalBytes;
        }),
        "the decoded selection re-encodes to identical bytes");
    Check(PublishAndVerify(publishOriginal, [&] {
            const QMimeData* mime = ClipboardMime();
            return mime != nullptr
                && mime->data(QLatin1String(cnote::clipboard::kSelectionMimeType)) == BytesOf(originalBytes);
        }),
        "the clipboard carries exactly the codec bytes under the Qt MIME type");

    // --- priority: structured > image > text --------------------------------
    Check(PublishAndVerify(
            [&] {
                auto* mime = new QMimeData;
                mime->setData(QLatin1String(cnote::clipboard::kSelectionMimeType), BytesOf(originalBytes));
                mime->setImageData(QImage(4, 4, QImage::Format_ARGB32));
                mime->setText(QStringLiteral("plain"));
                PublishMime(mime);
            },
            [&] { return cnote::clipboard::Read().kind == ClipboardKind::Selection; }),
        "structured objects are preferred over image and text flavors");
    Check(PublishAndVerify(
            [&] {
                auto* mime = new QMimeData;
                QImage image(4, 4, QImage::Format_ARGB32);
                image.fill(0xFF204060U);
                mime->setImageData(image);
                mime->setText(QStringLiteral("plain"));
                PublishMime(mime);
            },
            [&] {
                const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
                return content.kind == ClipboardKind::Image && content.image.size() == QSize(4, 4);
            }),
        "an ordinary image is preferred over plain text");
    Check(cnote::clipboard::WritePlainText(QStringLiteral("PDF selection text")),
        "plain text is published for a PDF text copy");
    Check(PublishAndVerify(
            [&] { static_cast<void>(cnote::clipboard::WritePlainText(QStringLiteral("PDF selection text"))); },
            [&] {
                const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
                return content.kind == ClipboardKind::Text
                    && content.text == QStringLiteral("PDF selection text");
            }),
        "plain text is the last-resort paste flavor");

    // --- invalid and empty payloads -----------------------------------------
    Check(PublishAndVerify(
            [&] {
                auto* mime = new QMimeData;
                mime->setData(QLatin1String(cnote::clipboard::kSelectionMimeType), QByteArrayLiteral("CTS8 truncated"));
                PublishMime(mime);
            },
            [&] {
                const QMimeData* mime = ClipboardMime();
                return mime != nullptr && mime->hasFormat(QLatin1String(cnote::clipboard::kSelectionMimeType))
                    && cnote::clipboard::Read().kind == ClipboardKind::None;
            }),
        "truncated structured bytes produce no paste content");
    Check(PublishAndVerify(
            [&] {
                auto* mime = new QMimeData;
                mime->setData(QLatin1String(cnote::clipboard::kSelectionMimeType), QByteArrayLiteral("nope"));
                mime->setText(QStringLiteral("must not be pasted"));
                PublishMime(mime);
            },
            [&] {
                const QMimeData* mime = ClipboardMime();
                return mime != nullptr && mime->hasText()
                    && cnote::clipboard::Read().kind == ClipboardKind::None;
            }),
        "corrupt structured bytes do not silently fall back to plain text");
    Check(PublishAndVerify(
            [&] {
                auto* mime = new QMimeData;
                mime->setData(QLatin1String(cnote::clipboard::kSelectionMimeType),
                    BytesOf(cnote::EncodeSelectionClipboard(SelectionClipboardData {})));
                PublishMime(mime);
            },
            [&] {
                const QMimeData* mime = ClipboardMime();
                return mime != nullptr && mime->hasFormat(QLatin1String(cnote::clipboard::kSelectionMimeType))
                    && cnote::clipboard::Read().kind == ClipboardKind::None;
            }),
        "a valid but object-less CTS8 payload produces no paste content");

    Check(PublishAndVerify(
            [&] { static_cast<void>(cnote::clipboard::WritePlainText(QStringLiteral("clipboard survivor"))); },
            [&] {
                const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
                return content.kind == ClipboardKind::Text && content.text == QStringLiteral("clipboard survivor");
            }),
        "the clipboard holds pre-existing content before the refused copies");
    SelectionClipboardData empty;
    empty.minX = 1.0f;
    Check(!cnote::clipboard::WriteSelection(empty), "an object-less selection publishes nothing");
    Check(!cnote::clipboard::WritePlainText(QString {}), "empty text publishes nothing");
    Check(!cnote::clipboard::WriteImage(QImage {}), "a null image publishes nothing");
    Check(PublishAndVerify(
            [&] { static_cast<void>(cnote::clipboard::WriteSelection(empty)); },
            [&] {
                const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
                return content.kind == ClipboardKind::Text && content.text == QStringLiteral("clipboard survivor");
            }),
        "a refused copy leaves the existing clipboard content untouched");

    // --- a single image object also publishes ordinary image data ------------
    SelectionClipboardData singleImage;
    singleImage.images.push_back(MakeImage(255U));
    singleImage.minX = 5.0f;
    singleImage.minY = 6.0f;
    singleImage.maxX = 35.0f;
    singleImage.maxY = 26.0f;
    const auto publishSingleImage = [&] { static_cast<void>(cnote::clipboard::WriteSelection(singleImage)); };

    Check(PublishAndVerify(publishSingleImage, [&] {
            const QMimeData* mime = ClipboardMime();
            return mime != nullptr && mime->hasFormat(QLatin1String(cnote::clipboard::kSelectionMimeType));
        }),
        "the structured flavor is still published for a single-image selection");
    Check(PublishAndVerify(publishSingleImage, [&] {
            const QMimeData* mime = ClipboardMime();
            return mime != nullptr && mime->hasImage();
        }),
        "a single-image selection also publishes ordinary image data");
    Check(PublishAndVerify(publishSingleImage, [&] {
            const QMimeData* mime = ClipboardMime();
            if (mime == nullptr || !mime->hasImage()) {
                return false;
            }
            const QImage image = qvariant_cast<QImage>(mime->imageData());
            return image.size() == QSize(3, 2) && image.pixel(0, 0) == qRgba(21, 11, 1, 255)
                && image.pixel(2, 1) == qRgba(26, 16, 6, 255);
        }),
        "the published ordinary image carries the object's pixels");
    Check(PublishAndVerify(publishSingleImage, [&] {
            const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
            return content.kind == ClipboardKind::Selection && content.selection.images.size() == 1U;
        }),
        "structured objects still win over the ordinary image flavor");

    // --- BGRA <-> QImage conversion keeps straight alpha ---------------------
    {
        const PageImage source = MakeImage(128U);
        const QImage image = cnote::clipboard::QImageFromPageImage(source);
        Check(!image.isNull() && image.size() == QSize(3, 2), "the model image converts to a QImage");
        Check(image.pixel(0, 0) == qRgba(21, 11, 1, 128), "BGRA bytes map to ARGB with straight alpha");
        Check(image.pixel(2, 1) == qRgba(26, 16, 6, 128), "the last pixel maps too");
        const PageImage restored = cnote::clipboard::PageImageFromQImage(image);
        Check(SameImagePixels(source, restored), "the QImage converts back to identical BGRA bytes");
        Check(cnote::clipboard::QImageFromPageImage(PageImage {}).isNull(),
            "an empty model image yields a null QImage");
    }

    // --- Windows: the legacy registered format bridge ------------------------
#ifdef _WIN32
    {
        const UINT format = SelectionFormat();
        Check(format != 0, "cnote.selection.v1 registers as a Windows clipboard format");

        // Qt -> raw Win32: exactly what the legacy frontend reads.
        const SelectionClipboardData published = MakeSelection();
        const std::vector<std::uint8_t> expected = cnote::EncodeSelectionClipboard(published);
        Check(PublishAndVerify(
                [&] { static_cast<void>(cnote::clipboard::WriteSelection(published)); },
                [&] { return ReadRegisteredFormat(format) == expected; }),
            "the Qt-published bytes are reachable through the legacy registered format");
        Check(PublishAndVerify(
                [&] { static_cast<void>(cnote::clipboard::WriteSelection(published)); },
                [&] {
                    const std::optional<SelectionClipboardData> decoded =
                        cnote::DecodeSelectionClipboard(ReadRegisteredFormat(format));
                    return decoded.has_value() && SameSelection(*decoded, published);
                }),
            "the legacy decoder reads the Qt-published selection");

        // raw Win32 -> Qt: exactly what the legacy frontend writes.
        const std::vector<std::uint8_t> legacyBytes = cnote::EncodeSelectionClipboard(MakeSelection());
        Check(WriteRegisteredFormat(format, legacyBytes),
            "the legacy registered format accepts raw bytes");
        Check(PublishAndVerify(
                [&] { WriteRegisteredFormat(format, legacyBytes); },
                [&] {
                    const QMimeData* mime = ClipboardMime();
                    return mime != nullptr
                        && mime->hasFormat(QLatin1String(cnote::clipboard::kWindowsSelectionMimeType))
                        && mime->data(QLatin1String(cnote::clipboard::kWindowsSelectionMimeType))
                            == BytesOf(legacyBytes);
                }),
            "Qt exposes the legacy format through its Windows MIME bridge with identical bytes");
        Check(PublishAndVerify(
                [&] { WriteRegisteredFormat(format, legacyBytes); },
                [&] {
                    const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
                    return content.kind == ClipboardKind::Selection && SameSelection(content.selection, MakeSelection());
                }),
            "the Qt MIME path decodes legacy-written bytes");
    }
#endif

    std::printf(g_failures == 0 ? "ALL CLIPBOARD CHECKS PASSED\n" : "%d CLIPBOARD CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
