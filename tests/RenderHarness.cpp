// Throwaway Qt canvas-render harness (migration plan steps 3.1/3.3/3.6).
//
// Renders a synthetic Document through cnote::render::PageRenderer into an
// offscreen QImage and checks layer order, transparent text backgrounds, span
// application, clipping, cache reuse and geometry stability. Writes PNGs and a
// log under CNOTE_RENDER_LOG_DIR (default: the system temp dir).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "render/PageRenderer.h"
#include "text/TextLayoutQt.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    } else {
        std::printf("ok:   %s\n", message.c_str());
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
    page.raster.pixels = SolidBgra(200, 200, 0x60, 0x40, 0x20, 0xFF);

    // Translucent image over the background.
    cnote::PageImage image {};
    image.x = 10.0f;
    image.y = 10.0f;
    image.width = 40.0f;
    image.height = 40.0f;
    image.pixelWidth = 40;
    image.pixelHeight = 40;
    image.stride = 40 * 4;
    image.pixels = SolidBgra(40, 40, 0x00, 0x00, 0xFF, 0x80);
    page.images.push_back(image);

    // Styled multiline text with a surrogate pair; the first three code units
    // ("Red") carry a red span, the rest stays blue.
    cnote::PageText text {};
    text.id = 1;
    text.x = 10.0f;
    text.y = 60.0f;
    text.width = 180.0f;
    text.height = 60.0f;
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

    const std::filesystem::path logDir = std::filesystem::path(
        std::getenv("CNOTE_RENDER_LOG_DIR") != nullptr
            ? std::getenv("CNOTE_RENDER_LOG_DIR")
            : ".");
    std::error_code error;
    std::filesystem::create_directories(logDir, error);

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
    surface.save(QString::fromStdString((logDir / "page.png").string()));

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

    // No clipping: text ink stays inside the measured bounds.
    const cnote::text::QtTextBounds bounds = cnote::text::MeasurePageTextBoundsQt(text, page.width);
    int inkMinX = surface.width();
    int inkMinY = surface.height();
    int inkMaxX = -1;
    int inkMaxY = -1;
    for (int y = 60; y < 120; ++y) {
        for (int x = 10; x < 190; ++x) {
            const std::uint32_t pixel = PixelAt(surface, x, y);
            const int red = static_cast<int>((pixel >> 16U) & 0xFFU);
            const int blue = static_cast<int>(pixel & 0xFFU);
            if (red > blue + 40 || blue > red + 40) {
                inkMinX = std::min(inkMinX, x);
                inkMinY = std::min(inkMinY, y);
                inkMaxX = std::max(inkMaxX, x);
                inkMaxY = std::max(inkMaxY, y);
            }
        }
    }
    Check(inkMaxX >= 0, "text ink was rendered");
    Check(static_cast<float>(inkMinX) >= bounds.left - 2.0f
            && static_cast<float>(inkMinY) >= bounds.top - 2.0f
            && static_cast<float>(inkMaxX) <= bounds.right + 2.0f
            && static_cast<float>(inkMaxY) <= bounds.bottom + 2.0f,
        "text ink stays inside the measured bounds");

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
            && text.x == 10.0f && text.y == 60.0f && text.width == 180.0f && text.height == 60.0f
            && text.content == u"Red\U0001F600Blue\r\nsecond \u03B1 line"
            && text.styleSpans.size() == 1U && text.styleSpans.front().start == 0U && text.styleSpans.front().length == 3U,
        "model geometry and text are unchanged after rendering");

    std::printf("surface sha256: %s\n", firstHash.c_str());
    std::printf("text layout lines: %zu, lineHeight: %d, height: %d\n",
        cnote::text::LayoutPageTextQt(text, page.width).lines.size(),
        cnote::text::LayoutPageTextQt(text, page.width).lineHeight,
        cnote::text::LayoutPageTextQt(text, page.width).height);

    std::FILE* log = std::fopen((logDir / "render-harness.log").string().c_str(), "w");
    if (log != nullptr) {
        std::fprintf(log, "surface sha256: %s\n", firstHash.c_str());
        std::fprintf(log, "background=%s image=%s stroke=%s\n",
            Hex(background).c_str(), Hex(imagePixel).c_str(), Hex(strokePixel).c_str());
        std::fprintf(log, "textBoxBackgroundLike=%d/%d redInk=%d blueInk=%d\n",
            backgroundLike, textBoxPixels, redInk, blueInk);
        std::fprintf(log, "failures=%d\n", g_failures);
        std::fclose(log);
    }

    cnote::latex::Shutdown();
    std::printf(g_failures == 0 ? "ALL RENDER CHECKS PASSED\n" : "%d RENDER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
