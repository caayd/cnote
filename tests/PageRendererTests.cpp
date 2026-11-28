// Offscreen Qt page-renderer test (migration plan steps 3.1/3.3/3.6).
//
// Renders a synthetic Document through cnote::render::PageRenderer into an
// offscreen QImage and defends the invariants a plausible bug would break:
// layer order, transparent text backgrounds over a colored page, span
// application, cache reuse without re-layout, and model stability.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QImage>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QPalette>
#include <QTextDocument>
#include <QTextOption>
#include <QFontMetricsF>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "render/PageRenderer.h"
#include "text/InlineMathQt.h"
#include "text/TextEditingQt.h"
#include "text/TextLayoutQt.h"
#include "Unicode.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

std::uint32_t PixelAt(const QImage& image, int x, int y) {
    return image.pixel(x, y);
}

std::string Hex(std::uint32_t argb) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%08X", argb);
    return buffer;
}

std::string Sha256(const QImage& image) {
    QByteArray bytes;
    for (int y = 0; y < image.height(); ++y) {
        bytes.append(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4);
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

std::vector<std::uint8_t> SolidBgra(int width, int height, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (std::size_t index = 0; index < pixels.size(); index += 4U) {
        pixels[index + 0U] = b;
        pixels[index + 1U] = g;
        pixels[index + 2U] = r;
        pixels[index + 3U] = a;
    }
    return pixels;
}

cnote::Document BuildDocument() {
    cnote::Document document = cnote::Document::CreateBlank();
    cnote::Page& page = document.CurrentPage();
    page.width = 200.0f;
    page.height = 200.0f;

    // Colored page background (straight-alpha BGRA).
    page.raster.width = 200;
    page.raster.height = 200;
    page.raster.stride = 200 * 4;
    page.raster.scale = 1.0f;
    page.raster.pixels = cnote::PixelBuffer(SolidBgra(200, 200, 0x60, 0x40, 0x20, 0xFF));

    // Translucent image over the background.
    cnote::PageImage image {};
    image.x = 10.0f;
    image.y = 10.0f;
    image.width = 40.0f;
    image.height = 40.0f;
    image.pixelWidth = 40;
    image.pixelHeight = 40;
    image.stride = 40 * 4;
    image.pixels = cnote::PixelBuffer(SolidBgra(40, 40, 0x00, 0x00, 0xFF, 0x80));
    page.images.push_back(image);

    // Styled multiline text with a surrogate pair; the first three code units
    // ("Red") carry a red span, the rest stays blue.
    cnote::PageText text {};
    text.id = 1;
    text.x = 10.0f;
    text.y = 60.0f;
    text.width = 180.0f;
    text.fontSize = 20.0f;
    text.color = 0xFF0000FFU;
    text.content = u"Red\U0001F600Blue\r\nsecond \u03B1 line";
    text.styleSpans.push_back({ 0U, 3U, { 20.0f, 0xFFFF0000U, cnote::TextFontFamily::Sans, false, false, false } });
    page.texts.push_back(text);

    cnote::PageLatex latex {};
    latex.id = 2;
    latex.x = 10.0f;
    latex.y = 130.0f;
    latex.fontSize = 24.0f;
    latex.color = 0xFF000000U;
    latex.content = u"x^2 + y^2";
    page.latexes.push_back(latex);

    // Green stroke crossing the translucent image (stroke layer is on top).
    cnote::Stroke stroke {};
    stroke.style.color = 0xFF00FF00U;
    stroke.style.width = 6.0f;
    stroke.points = { { 0.0f, 30.0f }, { 60.0f, 30.0f } };
    page.strokes.push_back(stroke);

    return document;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL: latex engine did not initialize\n");
        return 1;
    }

    cnote::Document document = BuildDocument();
    const cnote::Page& page = document.CurrentPage();
    const cnote::PageText& text = page.texts.front();

    cnote::render::PageRenderParams params;
    params.scale = 1.0f;
    params.devicePixelRatio = 1.0f;

    const QImage surface = cnote::render::RenderPageSurface(page, params);
    Check(!surface.isNull() && surface.width() == 200 && surface.height() == 200, "page surface has page dimensions");

    // Layer order: background, image over background, stroke over image.
    const std::uint32_t background = PixelAt(surface, 5, 5);
    Check(background == 0xFF204060U, "background pixel is the page raster color, got " + Hex(background));

    const std::uint32_t imagePixel = PixelAt(surface, 30, 20);
    const int imageRed = static_cast<int>((imagePixel >> 16U) & 0xFFU);
    const int imageBlue = static_cast<int>(imagePixel & 0xFFU);
    Check(imagePixel != background && imageRed > imageBlue + 40,
        "translucent image composites over the background, got " + Hex(imagePixel));

    const std::uint32_t strokePixel = PixelAt(surface, 30, 30);
    Check(strokePixel == 0xFF00FF00U, "stroke paints over the image, got " + Hex(strokePixel));

    // Transparent text background: most of the text box keeps the page color.
    int backgroundLike = 0;
    int textBoxPixels = 0;
    for (int y = 60; y < 120; ++y) {
        for (int x = 10; x < 190; ++x) {
            ++textBoxPixels;
            if (PixelAt(surface, x, y) == background) {
                ++backgroundLike;
            }
        }
    }
    Check(textBoxPixels > 0 && backgroundLike * 2 > textBoxPixels,
        "text box keeps the page background (no opaque rectangle): " + std::to_string(backgroundLike) + "/" + std::to_string(textBoxPixels));

    // Span application: red-dominant ink in the span, blue-dominant ink after it.
    int redInk = 0;
    int blueInk = 0;
    int firstRedX = surface.width();
    int lastBlueX = -1;
    for (int y = 60; y < 120; ++y) {
        for (int x = 10; x < 190; ++x) {
            const std::uint32_t pixel = PixelAt(surface, x, y);
            const int red = static_cast<int>((pixel >> 16U) & 0xFFU);
            const int blue = static_cast<int>(pixel & 0xFFU);
            if (red > blue + 40) {
                ++redInk;
                firstRedX = std::min(firstRedX, x);
            } else if (blue > red + 40) {
                ++blueInk;
                lastBlueX = std::max(lastBlueX, x);
            }
        }
    }
    Check(redInk > 0 && blueInk > 0, "span and default text colors both render");
    Check(firstRedX < lastBlueX, "span ink precedes default ink");

    // Cache reuse: a second EnsurePageSurface must not re-render or re-layout.
    cnote::render::PageSurfaceCache cache;
    cnote::render::EnsurePageSurface(cache, page, params);
    const std::uint64_t missesAfterFirst = cnote::text::TextLayoutCacheMisses();
    const uchar* firstBits = cache.image.constBits();
    const std::string firstHash = Sha256(cache.image);
    cnote::render::EnsurePageSurface(cache, page, params);
    Check(cache.image.constBits() == firstBits, "second EnsurePageSurface reuses the cached image");
    Check(Sha256(cache.image) == firstHash, "second EnsurePageSurface keeps identical pixels");
    Check(cnote::text::TextLayoutCacheMisses() == missesAfterFirst, "second render does not re-layout text");

    // Geometry stability: rendering never rewrites model values.
    Check(page.width == 200.0f && page.height == 200.0f
            && text.x == 10.0f && text.y == 60.0f && text.width == 180.0f
            && text.content == u"Red\U0001F600Blue\r\nsecond \u03B1 line"
            && text.styleSpans.size() == 1U && text.styleSpans.front().start == 0U && text.styleSpans.front().length == 3U,
        "model geometry and text are unchanged after rendering");

    // --- prepared draw document: a redraw reuses it, pixel for pixel --------
    {
        cnote::PageText drawn {};
        drawn.fontSize = 20.0f;
        drawn.fontFamily = cnote::TextFontFamily::Sans;
        drawn.color = 0xFF000000U;
        drawn.width = 320.0f;
        drawn.content = u"a $x^2$ b\r\nsecond line \u03B1";
        const auto drawInto = [&](float scale, float ratio) {
            QImage image(720, 300, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            cnote::text::DrawPageTextQt(painter, drawn, 320.0f, scale, ratio, 10.0f, 10.0f, 1.0f);
            return image;
        };

        const std::uint64_t buildsBeforeDraw = cnote::text::TextDocumentBuilds();
        const QImage coldImage = drawInto(2.0f, 2.0f);
        const std::uint64_t buildsAfterColdDraw = cnote::text::TextDocumentBuilds();
        Check(buildsAfterColdDraw > buildsBeforeDraw, "a first draw builds its formatted document");

        const QImage warmImage = drawInto(2.0f, 2.0f);
        Check(cnote::text::TextDocumentBuilds() == buildsAfterColdDraw,
            "a redraw at the same scale builds no document (cached prepared layout)");
        Check(warmImage == coldImage, "a cached draw keeps identical pixels");

        // A transform is a different layout scale: one new document, then that
        // scale is cached too.
        const QImage transformed = drawInto(3.0f, 2.0f);
        const std::uint64_t buildsAfterTransform = cnote::text::TextDocumentBuilds();
        Check(buildsAfterTransform == buildsAfterColdDraw + 1U,
            "a draw at a new scale builds exactly one document");
        const QImage transformedAgain = drawInto(3.0f, 2.0f);
        Check(cnote::text::TextDocumentBuilds() == buildsAfterTransform && transformedAgain == transformed,
            "the transformed scale is cached and pixel-identical");
    }

    // --- dense page: every prepared document survives to the next redraw ---
    // A page surface redraws text object by object.  If the cache is smaller
    // than the page, the second pass is a cyclic miss (and evicts what it
    // just built); 40 ordinary boxes and a deliberately dense 200 must both
    // be fully reused.
    {
        const auto makeBoxes = [](int count) {
            std::vector<cnote::PageText> boxes;
            boxes.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index) {
                cnote::PageText box {};
                box.fontSize = 12.0f;
                box.fontFamily = cnote::TextFontFamily::Sans;
                box.color = 0xFF000000U;
                box.width = 160.0f;
                const std::string label =
                    "box " + std::to_string(count) + "-" + std::to_string(index);
                box.content.assign(label.begin(), label.end());
                boxes.push_back(std::move(box));
            }
            return boxes;
        };
        const auto drawPass = [](const std::vector<cnote::PageText>& boxes) {
            QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            for (std::size_t index = 0; index < boxes.size(); ++index) {
                cnote::text::DrawPageTextQt(painter, boxes[index], 200.0f, 1.0f, 1.0f,
                    static_cast<float>(index % 10) * 18.0f, static_cast<float>(index / 10) * 14.0f, 1.0f);
            }
        };
        for (const int count : { 40, 200 }) {
            const std::vector<cnote::PageText> boxes = makeBoxes(count);
            const std::uint64_t buildsBeforeDense = cnote::text::TextDocumentBuilds();
            drawPass(boxes);
            const std::uint64_t buildsAfterDense = cnote::text::TextDocumentBuilds();
            Check(buildsAfterDense - buildsBeforeDense == static_cast<std::uint64_t>(count),
                "the first draw of a " + std::to_string(count) + "-box page builds one document per box");
            drawPass(boxes);
            Check(cnote::text::TextDocumentBuilds() == buildsAfterDense,
                "the second draw of a " + std::to_string(count)
                    + "-box page builds no documents (dense page fits the cache)");
        }
    }

    // --- dense formula page: placements survive a redraw -------------------
    {
        cnote::PageText formulas {};
        formulas.fontSize = 20.0f;
        formulas.fontFamily = cnote::TextFontFamily::Sans;
        formulas.color = 0xFF000000U;
        formulas.width = 860.0f;
        for (int index = 0; index < 200; ++index) {
            const std::string term = "x_" + std::to_string(index) + "^2";
            formulas.content += u"$";
            formulas.content.append(term.begin(), term.end());
            formulas.content += u"$ ";
        }
        const auto drawFormulas = [&formulas]() {
            QImage image(900, 200, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            cnote::text::DrawPageTextQt(painter, formulas, 900.0f, 1.0f, 1.0f, 5.0f, 5.0f, 1.0f);
        };

        const std::uint64_t buildsBeforeFormulas = cnote::text::TextDocumentBuilds();
        const std::uint64_t placementsBefore = cnote::text::InlineMathPlacementMisses();
        drawFormulas();
        const std::uint64_t buildsAfterFormulas = cnote::text::TextDocumentBuilds();
        const std::uint64_t placementsAfterFirst = cnote::text::InlineMathPlacementMisses();
        Check(buildsAfterFormulas - buildsBeforeFormulas == 1U,
            "the 200-formula page builds one formatted document");
        Check(placementsAfterFirst - placementsBefore == 200U,
            "the first draw measures every formula placement");

        drawFormulas();
        Check(cnote::text::TextDocumentBuilds() == buildsAfterFormulas,
            "a redrawn formula page reuses its prepared document");
        Check(cnote::text::InlineMathPlacementMisses() == placementsAfterFirst,
            "a redrawn formula page reuses every placement (dense page fits the cache)");
    }

    // A single-point stroke paints a filled dot whose diameter is the pen
    // width; a regression stroked the dot's outline on top of the fill and
    // doubled it.  Compare the dot's vertical extent against a two-point
    // stroke of the same width.
    {
        cnote::Page dotPage;
        dotPage.width = 300.0f;
        dotPage.height = 120.0f;
        cnote::Stroke dot;
        dot.style.width = 10.0f;
        dot.style.color = 0xFF000000U;
        dot.smoothing = 0.0f;
        dot.points = { { 60.0f, 60.0f } };
        cnote::Stroke line = dot;
        line.points = { { 120.0f, 60.0f }, { 240.0f, 60.0f } };
        dotPage.strokes = { dot, line };

        cnote::render::PageRenderParams dotParams;
        dotParams.scale = 2.0f;
        const QImage dotSurface = cnote::render::RenderPageSurface(dotPage, dotParams);
        const auto extentAt = [&dotSurface](int x) {
            int top = -1;
            int bottom = -1;
            for (int y = 0; y < dotSurface.height(); ++y) {
                if (qGray(dotSurface.pixel(x, y)) < 128) {
                    if (top < 0) {
                        top = y;
                    }
                    bottom = y;
                }
            }
            return bottom - top + 1;
        };
        const int dotExtent = extentAt(120);
        const int lineExtent = extentAt(360);
        Check(dotExtent > 0 && std::abs(dotExtent - lineExtent) <= 2,
            "single-point dot matches stroke thickness (dot=" + std::to_string(dotExtent)
                + " line=" + std::to_string(lineExtent) + ")");
    }

    // A page turn turns an image with the page: a wide image comes out tall
    // and unstretched.  The model used to swap the image box's sides and add
    // the 90 degrees as well, so the pixels were squeezed into the swapped
    // box and then turned back to the old shape.
    {
        cnote::Page turnPage;
        turnPage.width = 200.0f;
        turnPage.height = 100.0f;
        cnote::PageImage wide;
        wide.x = 20.0f;
        wide.y = 40.0f;
        wide.width = 60.0f;
        wide.height = 20.0f;
        wide.pixelWidth = 60;
        wide.pixelHeight = 20;
        wide.stride = 60 * 4;
        wide.pixels = cnote::PixelBuffer(SolidBgra(60, 20, 0x00, 0x00, 0xFF, 0xFF));
        turnPage.images.push_back(wide);
        cnote::RotatePageContents(turnPage, 1);

        const QImage turned = cnote::render::RenderPageSurface(turnPage, cnote::render::PageRenderParams {});
        int left = turned.width();
        int right = -1;
        int top = turned.height();
        int bottom = -1;
        for (int y = 0; y < turned.height(); ++y) {
            for (int x = 0; x < turned.width(); ++x) {
                const QRgb pixel = turned.pixel(x, y);
                if (qRed(pixel) > 200 && qGreen(pixel) < 80 && qBlue(pixel) < 80) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        const cnote::Vec2 centre = cnote::RotatePointQuarterTurn({ 50.0f, 50.0f }, 200.0f, 100.0f, 1);
        Check(right >= left && std::abs((right - left + 1) - 20) <= 2 && std::abs((bottom - top + 1) - 60) <= 2,
            "a turned wide image draws tall and unstretched (" + std::to_string(right - left + 1) + "x"
                + std::to_string(bottom - top + 1) + ")");
        Check(std::abs((left + right + 1) * 0.5f - centre.x) <= 1.5f && std::abs((top + bottom + 1) * 0.5f - centre.y) <= 1.5f,
            "a turned image stays centred where the page turn takes its centre");
    }

    // Pressure ink is a filled union of tapered quads, round caps and join
    // fans.  At half pressure (width factor 1) it must cover what the
    // constant-width round pen covers: a hole where pieces overlap at a sharp
    // turn, a wrong radius, or antialiased piece edges stacking up along the
    // outline (each a few percent of the ink) all show up as a difference.
    // More pressure must draw wider ink.
    {
        const auto renderInk = [](const cnote::Stroke& stroke) {
            cnote::Page page;
            page.width = 300.0f;
            page.height = 200.0f;
            page.strokes = { stroke };
            cnote::render::PageRenderParams params;
            params.scale = 2.0f;
            return cnote::render::RenderPageSurface(page, params);
        };
        const auto inkCount = [](const QImage& image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    count += qGray(image.pixel(x, y)) < 128 ? 1 : 0;
                }
            }
            return count;
        };

        // A hairpin V sampled every unit, so the smoothed centerline keeps a
        // tight turn where the pieces' overlap is most complex.
        cnote::Stroke pen;
        pen.style.width = 16.0f;
        pen.style.color = 0xFF000000U;
        for (int i = 0; i <= 100; ++i) {
            pen.points.push_back({ 50.0f + static_cast<float>(i), 170.0f - static_cast<float>(i) * 1.4f });
        }
        for (int i = 1; i <= 100; ++i) {
            pen.points.push_back({ 150.0f + static_cast<float>(i) * 0.3f, 30.0f + static_cast<float>(i) * 1.4f });
        }
        cnote::Stroke halfPressure = pen;
        halfPressure.pressures.assign(pen.points.size(), 0.5f);
        const int penInk = inkCount(renderInk(pen));
        const int outlineInk = inkCount(renderInk(halfPressure));
        Check(penInk > 0 && std::abs(outlineInk - penInk) <= penInk / 200,
            "half-pressure ink covers the constant-width pen (pen=" + std::to_string(penInk)
                + " outline=" + std::to_string(outlineInk) + ")");

        cnote::Stroke ramp;
        ramp.style = pen.style;
        for (int i = 0; i <= 200; ++i) {
            ramp.points.push_back({ 50.0f + static_cast<float>(i), 100.0f });
            ramp.pressures.push_back(static_cast<float>(i) / 200.0f);
        }
        const QImage rampSurface = renderInk(ramp);
        const auto thicknessAt = [&rampSurface](int x) {
            int count = 0;
            for (int y = 0; y < rampSurface.height(); ++y) {
                count += qGray(rampSurface.pixel(x, y)) < 128 ? 1 : 0;
            }
            return count;
        };
        const int light = thicknessAt(2 * 75);
        const int heavy = thicknessAt(2 * 225);
        Check(light > 0 && heavy > light * 2,
            "pressure widens the ink (light=" + std::to_string(light) + " heavy=" + std::to_string(heavy) + ")");

        // Where the width changes at a varying rate, neighbouring segments'
        // end edges sit at different offsets; the ink must still be solid
        // along its centerline (no light seams between the pieces).
        cnote::Stroke wave;
        wave.style = pen.style;
        wave.style.width = 24.0f;
        wave.smoothing = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            wave.points.push_back({ 50.0f + (2.0f * static_cast<float>(i)), 100.0f });
            wave.pressures.push_back(0.5f + (0.45f * std::sin(static_cast<float>(i) * 0.3f)));
        }
        const QImage waveSurface = renderInk(wave);
        int lightest = 0;
        for (int x = 2 * 52; x <= 2 * 248; ++x) {
            lightest = std::max(lightest, qGray(waveSurface.pixel(x, 2 * 100)));
        }
        Check(lightest < 40, "pressure ink is solid along its centerline (lightest=" + std::to_string(lightest) + ")");
    }

    // --- committed text is pixel-identical to the live editor ---------------
    // The editor (CanvasWidget::ShowTextOverlay) formats its document at the
    // view zoom; Qt rounds font pixel sizes, so a commit laid out in page
    // units and painter-scaled visibly re-spaced every glyph.  The canvas
    // surface must lay text out at the zoom and only scale by the DPR.
    {
        cnote::Page textPage {};
        textPage.width = 400.0f;
        textPage.height = 160.0f;
        // At the page origin so both sides share an exact (zero) translation;
        // the comparison then isolates glyph layout from origin rounding.
        cnote::PageText note {};
        note.fontSize = 15.0f;
        note.color = 0xFF000000U;
        note.content = u"The quick brown fox jumps over the lazy dog.\nIllicit WWW mmm iii 0123456789 wrapping text";
        note.styleSpans.push_back({ 4U, 5U, { 15.0f, 0xFF000000U, cnote::TextFontFamily::Sans, true, false, false } });
        for (const float width : { 0.0f, 230.0f }) {
            note.width = width;
            textPage.texts = { note };
            for (const float zoom : { 0.85f, 1.37f, 2.1f }) {
                for (const float dpr : { 1.0f, 1.5f }) {
                    cnote::render::PageRenderParams textParams;
                    textParams.scale = zoom;
                    textParams.devicePixelRatio = dpr;
                    textParams.textPixelRatio = 1.0f;
                    const QImage committedImage = cnote::render::RenderPageSurface(textPage, textParams);

                    QTextDocument editor;
                    editor.setDocumentMargin(0.0);
                    QTextOption option;
                    option.setWrapMode(width > 0.0f ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
                    editor.setDefaultTextOption(option);
                    editor.setPlainText(QString::fromUtf16(note.content.data(), static_cast<qsizetype>(note.content.size())));
                    cnote::text::ApplyPageTextFormatting(editor, note, zoom);
                    if (width > 0.0f) {
                        editor.setTextWidth(cnote::text::TextWrapWidthPx(note, zoom));
                    }
                    QImage editorImage(committedImage.size(), QImage::Format_ARGB32_Premultiplied);
                    editorImage.fill(Qt::white);
                    {
                        QPainter painter(&editorImage);
                        painter.setRenderHint(QPainter::Antialiasing, true);
                        painter.scale(dpr, dpr);
                        QAbstractTextDocumentLayout::PaintContext context;
                        context.palette.setColor(QPalette::Text, Qt::black);
                        editor.documentLayout()->draw(&painter, context);
                    }
                    // Compare inside the page only: the surface's last
                    // row/column is partially covered when page * scale is
                    // fractional.
                    int worst = 0;
                    for (int y = 0; y < committedImage.height(); ++y) {
                        for (int x = 0; x < committedImage.width(); ++x) {
                            const QRgb committedPixel = committedImage.pixel(x, y);
                            if (qAlpha(committedPixel) == 255) {
                                worst = std::max(worst, std::abs(qGray(committedPixel) - qGray(editorImage.pixel(x, y))));
                            }
                        }
                    }
                    Check(worst <= 8, "committed text matches the live editor pixel for pixel (width "
                        + std::to_string(width) + ", zoom " + std::to_string(zoom) + ", dpr " + std::to_string(dpr)
                        + ", worst gray delta " + std::to_string(worst) + ")");
                }
            }
        }
    }

    // --- inline $...$ math in committed text --------------------------------
    {
        cnote::PageText math {};
        math.fontSize = 20.0f;
        math.content = u"a $x^2$ b $5 and $10 $\\frac{}{}$";
        const cnote::text::TextOffsetMap map = cnote::text::BuildTextOffsetMap(math, cnote::text::InlineMathMode::Rendered);
        Check(map.normalized == u"a \uFFFC b $5 and $10 $\\frac{}{}$",
            "only the renderable formula collapses to a placeholder (prices and unparsable source stay text)");
        Check(map.math.size() == 1U && cnote::text::DocumentOffsetForModel(map, 2U) == 2U
                && cnote::text::DocumentOffsetForModel(map, 5U) == 2U
                && cnote::text::DocumentOffsetForModel(map, 7U) == 3U
                && cnote::text::ModelOffsetForDocument(map, 2U) == 2U
                && cnote::text::ModelOffsetForDocument(map, 3U) == 7U,
            "model offsets inside a formula map to its placeholder and back to the formula edges");
        const cnote::text::QtTextLayout mathLayout = cnote::text::LayoutPageTextQt(math, 400.0f);
        Check(mathLayout.lines.size() == 1U && mathLayout.lines.front().text == math.content,
            "layout lines still report the stored source");

        // Renders one line of Segoe UI text at 48px and finds its glyph
        // clusters (ink columns separated by blank columns).
        struct Cluster {
            int left = 0;
            int right = 0;
        };
        const auto renderLine = [](const std::u16string& content) {
            cnote::Page mathPage {};
            mathPage.width = 400.0f;
            mathPage.height = 200.0f;
            cnote::PageText line {};
            line.fontSize = 48.0f;
            line.fontFamily = cnote::TextFontFamily::Sans;
            line.color = 0xFF000000U;
            line.content = content;
            mathPage.texts = { line };
            cnote::render::PageRenderParams mathParams;
            mathParams.textPixelRatio = 1.0f;
            return cnote::render::RenderPageSurface(mathPage, mathParams);
        };
        const auto inkAt = [](const QImage& image, int x, int y) { return qGray(image.pixel(x, y)) < 128; };
        const auto clusters = [&](const QImage& image) {
            std::vector<Cluster> found;
            bool inside = false;
            for (int x = 0; x < image.width(); ++x) {
                bool ink = false;
                for (int y = 0; y < image.height() && !ink; ++y) {
                    ink = inkAt(image, x, y);
                }
                if (ink && !inside) {
                    found.push_back({ x, x });
                }
                if (ink) {
                    found.back().right = x;
                }
                inside = ink;
            }
            return found;
        };
        // Lowest ink row across the cluster's left half: the left stem of an
        // upright or italic "n" ends on the baseline.  Only the left half,
        // because an italic n's first columns are its entry hook, which sits
        // high (and crosses the ink threshold with some rasterisers, e.g.
        // CoreText).
        const auto stemBottom = [&](const QImage& image, const Cluster& cluster) {
            int bottom = -1;
            for (int x = cluster.left; x <= cluster.left + (cluster.right - cluster.left) / 2; ++x) {
                for (int y = 0; y < image.height(); ++y) {
                    if (inkAt(image, x, y)) {
                        bottom = std::max(bottom, y);
                    }
                }
            }
            return bottom;
        };
        const auto inkBottom = [&](const QImage& image, int left, int right) {
            int bottom = -1;
            for (int x = left; x <= right; ++x) {
                for (int y = 0; y < image.height(); ++y) {
                    if (inkAt(image, x, y)) {
                        bottom = std::max(bottom, y);
                    }
                }
            }
            return bottom;
        };
        const int fontDescent = static_cast<int>(std::ceil(QFontMetricsF(cnote::text::FontForStyle(
            { 48.0f, 0xFF000000U, cnote::TextFontFamily::Sans, false, false, false })).descent()));

        // Shallow formula (fits the font descent) and a deep one (a fraction
        // in the subscript) both put their base on the text baseline.  MicroTeX
        // metrics are whole layout pixels, so allow 2px at this 48px size.
        int deepBaseline = -1;
        int deepInkBottom = -1;
        for (const std::u16string& content : { std::u16string(u"n $n$ n"), std::u16string(u"n $n_{\\frac{n}{\\frac{n}{n}}}$ n") }) {
            const QImage image = renderLine(content);
            const std::vector<Cluster> found = clusters(image);
            Check(found.size() >= 3U, "inline math line renders separate glyph clusters for " + cnote::Utf16ToUtf8(content));
            if (found.size() < 3U) {
                continue;
            }
            const int baseline = stemBottom(image, found.front());
            const int formulaBase = stemBottom(image, found[1]);
            const int trailing = stemBottom(image, found.back());
            const int formulaInkBottom = inkBottom(image, found[1].left, found[found.size() - 2U].right);
            std::printf("     math baseline %s text=%d formula=%d trailing=%d formula ink bottom=%d descent=%d\n",
                cnote::Utf16ToUtf8(content).c_str(), baseline, formulaBase, trailing, formulaInkBottom, fontDescent);
            Check(std::abs(formulaBase - baseline) <= 2 && trailing == baseline,
                "formula base sits on the text baseline for " + cnote::Utf16ToUtf8(content));
            if (content.find(u"frac") != std::u16string::npos) {
                Check(formulaInkBottom - baseline > fontDescent,
                    "the deep probe hangs below the font descent (exercises the enlarged placeholder descent)");
                deepBaseline = baseline;
                deepInkBottom = formulaInkBottom;
            }
        }

        // A deep formula reserves its depth: the next line starts below it.
        // Probed with a capital H, whose ink stays inside the font's ascent;
        // accented capitals rise above it in some fonts (Helvetica on macOS).
        const QImage twoLines = renderLine(u"n $n_{\\frac{n}{\\frac{n}{n}}}$ n\nH");
        const std::vector<Cluster> twoLineClusters = clusters(twoLines);
        int secondLineTop = -1;
        if (!twoLineClusters.empty() && deepBaseline >= 0) {
            for (int y = deepBaseline + 1; y < twoLines.height() && secondLineTop < 0; ++y) {
                for (int x = twoLineClusters.front().left; x <= twoLineClusters.front().right; ++x) {
                    if (inkAt(twoLines, x, y)) {
                        secondLineTop = y;
                        break;
                    }
                }
            }
        }
        std::printf("     math next line top=%d formula ink bottom=%d\n", secondLineTop, deepInkBottom);
        Check(secondLineTop > deepInkBottom, "the line after a deep formula starts below its ink");
    }

    // --- Raster budget and tiled page surfaces --------------------------------
    // Above the per-surface budget a page renders as bounded tiles.  Tiling
    // must be pixel-exact and seam-free: compositing the tiles reproduces the
    // full RenderPageSurface raster byte for byte (the canvas blits those tiles
    // 1:1, so any seam would be visible at high zoom).
    {
        const auto tiledSurface = [&page](const cnote::render::PageRenderParams& params) {
            const QImage full = cnote::render::RenderPageSurface(page, params);
            Check(!full.isNull(), "tile probe: full surface renders");
            const int tileSize = cnote::render::PageTileSize(params);
            QImage composed(full.size(), QImage::Format_ARGB32_Premultiplied);
            composed.fill(Qt::transparent);
            QPainter painter(&composed);
            const int columns = (full.width() + tileSize - 1) / tileSize;
            const int rows = (full.height() + tileSize - 1) / tileSize;
            for (int row = 0; row < rows; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const QRect rect(
                        column * tileSize, row * tileSize,
                        std::min(tileSize, full.width() - (column * tileSize)),
                        std::min(tileSize, full.height() - (row * tileSize)));
                    const QImage tile =
                        cnote::render::RenderPageSurfaceRegionPixels(page, params, rect);
                    if (tile.isNull() || tile.size() != rect.size()) {
                        Check(false, "tile probe: every tile renders at its rect size");
                        continue;
                    }
                    painter.drawImage(rect.topLeft(), tile);
                }
            }
            painter.end();
            return std::make_pair(full, composed);
        };
        const auto mismatchedPixels = [](const QImage& full, const QImage& composed) {
            int mismatches = 0;
            if (full.size() != composed.size()) {
                return -1;
            }
            for (int y = 0; y < full.height(); ++y) {
                if (std::memcmp(full.constScanLine(y), composed.constScanLine(y),
                        static_cast<std::size_t>(full.width()) * 4U) == 0) {
                    continue;
                }
                for (int x = 0; x < full.width(); ++x) {
                    if (full.pixel(x, y) != composed.pixel(x, y)) {
                        ++mismatches;
                    }
                }
            }
            return mismatches;
        };

        cnote::render::PageRenderParams tileParams = params;
        tileParams.scale = 1.0f;
        tileParams.devicePixelRatio = 1.0f;
        tileParams.textPixelRatio = 1.0f;
        const auto [full, composed] = tiledSurface(tileParams);
        const int tileMismatches = mismatchedPixels(full, composed);
        std::printf("TILE-PARITY scale=%.2f dpr=%.2f size=%dx%d mismatchedPixels=%d\n",
            static_cast<double>(tileParams.scale), static_cast<double>(tileParams.devicePixelRatio),
            full.width(), full.height(), tileMismatches);
        Check(tileMismatches == 0, "tiles composite to the exact full-page surface (seam-free)");

        // Fractional device pixel ratio and zoom, with the canvas' text pixel
        // ratio: the tile grid must keep Qt's glyph subpixel phase identical to
        // the full raster, or text re-rasterizes a fraction of a pixel off.
        cnote::render::PageRenderParams dprParams = params;
        dprParams.scale = 1.35f;
        dprParams.devicePixelRatio = 1.75f;
        dprParams.textPixelRatio = 1.0f;
        Check(cnote::render::RegionAlignment(dprParams) == 7,
            "region alignment resolves a 1.75 device pixel ratio to 7 device pixels");
        const auto [fullDpr, composedDpr] = tiledSurface(dprParams);
        const int dprMismatches = mismatchedPixels(fullDpr, composedDpr);
        std::printf("TILE-PARITY dpr size=%dx%d tile=%d mismatchedPixels=%d\n",
            fullDpr.width(), fullDpr.height(), cnote::render::PageTileSize(dprParams), dprMismatches);
        Check(dprMismatches == 0, "tiles stay pixel-exact at a fractional device pixel ratio");

        // Refusals: the full size is checked before allocating, and a refused
        // render keeps the prior raster instead of retrying every paint.
        cnote::render::RasterBudget tiny;
        tiny.maxSurfaceBytes = 64U * 1024U;
        cnote::render::PageRenderParams bigger = params;
        bigger.scale = 1.5f;
        const QSize biggerSize = cnote::render::PageSurfacePixelSize(page, bigger);
        Check(!cnote::render::RasterFitsBudget(biggerSize.width(), biggerSize.height(), tiny),
            "budget probe size exceeds the tiny budget");
        Check(cnote::render::RenderPageSurface(page, bigger, tiny).isNull(),
            "a full surface above the budget is refused before allocating");
        Check(cnote::render::RenderPageSurfaceRegionPixels(
                  page, bigger, QRect(0, 0, biggerSize.width(), biggerSize.height()), tiny).isNull(),
            "a region above the budget is refused before allocating");

        cnote::render::PageSurfaceCache budgetCache;
        cnote::render::EnsurePageSurface(budgetCache, page, params);
        Check(!budgetCache.image.isNull() && !budgetCache.failed, "a surface inside the budget renders");
        const uchar* const priorBits = budgetCache.image.constBits();
        const std::uint64_t failedSignature = cnote::render::PageSurfaceSignature(page, bigger);
        cnote::render::EnsurePageSurface(budgetCache, page, bigger, tiny);
        Check(budgetCache.failed && budgetCache.failedSignature == failedSignature,
            "a refused render is recorded for its signature");
        Check(budgetCache.image.constBits() == priorBits && Sha256(budgetCache.image) == firstHash,
            "a refused render keeps the usable prior raster");
        cnote::render::EnsurePageSurface(budgetCache, page, bigger, tiny);
        Check(budgetCache.image.constBits() == priorBits && budgetCache.failedSignature == failedSignature,
            "a known-impossible allocation is not retried for the same signature");
    }

    std::printf("surface sha256: %s\n", firstHash.c_str());
    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL PAGE RENDERER CHECKS PASSED\n" : "%d PAGE RENDERER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
