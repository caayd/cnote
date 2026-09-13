#include "PdfOverlay.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <objidl.h>
#include <gdiplus.h>

#include "PdfBackend.h"
#include "latex/LatexEngine.h"
#include "selection/StrokeSmoothing.h"
#include "text/TextGeometry.h"

#include "fpdf_edit.h"
#include "fpdfview.h"

namespace cnote {

namespace text = cnote::text;

namespace {

constexpr float kMinimumStrokeRenderWidth = 1.25f;
constexpr float kStrokeRenderWidthDivisor = 2.15f;
constexpr float kTextLayoutScale = 8.0f;

// GDI+ text drawing is temporary Win32 code; model UTF-16 maps one 16-bit code
// unit per wchar until the Qt renderer replaces it.
std::wstring WideFromUtf16(std::u16string_view value) {
    std::wstring wide;
    wide.reserve(value.size());
    for (char16_t unit : value) {
        wide.push_back(static_cast<wchar_t>(unit));
    }
    return wide;
}

using text::LayoutPageText;
using text::WrappedTextLayout;
using text::PageTextFontFace;
using text::ResolvedPageTextWidth;

void DrawPageImage(Gdiplus::Graphics& graphics, const PageImage& image, float renderScale) {
    if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0 || image.stride <= 0) {
        return;
    }

    Gdiplus::Bitmap bitmap(
        image.pixelWidth,
        image.pixelHeight,
        image.stride,
        PixelFormat32bppARGB,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));

    const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * renderScale);
    const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * renderScale);
    const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * renderScale);
    const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * renderScale);

    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.TranslateTransform(centerX, centerY);
    graphics.RotateTransform(static_cast<Gdiplus::REAL>(image.rotationDegrees));
    graphics.DrawImage(
        &bitmap,
        Gdiplus::RectF(-drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight));
    graphics.Restore(state);
}

void DrawPageLatex(Gdiplus::Graphics& graphics, const PageLatex& latex, float renderScale) {
    if (!cnote::latex::IsInitialized()) {
        return;
    }

    const cnote::latex::FormulaHandle formula = cnote::latex::Parse(latex);
    if (formula == nullptr) {
        return;
    }

    cnote::latex::Draw(
        formula,
        graphics,
        latex.x * renderScale,
        latex.y * renderScale,
        renderScale,
        latex.color);
}

void DrawStroke(Gdiplus::Graphics& graphics, const Stroke& stroke, float renderScale, float strokeSmoothing, float dashLength, float dashGap) {
    if (stroke.points.empty()) {
        return;
    }

    const float effectiveSmoothing = StrokeRenderSmoothing(stroke, strokeSmoothing);
    const std::vector<Vec2> smoothedPoints = BuildSmoothedPoints(stroke.points, effectiveSmoothing);
    std::vector<Gdiplus::PointF> screenPoints;
    screenPoints.reserve(smoothedPoints.size());
    for (const Vec2& point : smoothedPoints) {
        screenPoints.emplace_back(
            static_cast<Gdiplus::REAL>(point.x * renderScale),
            static_cast<Gdiplus::REAL>(point.y * renderScale));
    }

    const auto alpha = static_cast<BYTE>((stroke.style.color >> 24U) & 0xFFU);
    const auto red = static_cast<BYTE>((stroke.style.color >> 16U) & 0xFFU);
    const auto green = static_cast<BYTE>((stroke.style.color >> 8U) & 0xFFU);
    const auto blue = static_cast<BYTE>(stroke.style.color & 0xFFU);
    const Gdiplus::Color color(alpha, red, green, blue);
    const Gdiplus::REAL penWidth = static_cast<Gdiplus::REAL>(
        std::max(kMinimumStrokeRenderWidth, stroke.style.width * renderScale / kStrokeRenderWidthDivisor));

    Gdiplus::Pen pen(color, penWidth);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (stroke.style.dotted) {
        const Gdiplus::REAL dashPattern[] {
            static_cast<Gdiplus::REAL>(std::max(0.5f, dashLength)),
            static_cast<Gdiplus::REAL>(std::max(0.5f, dashGap)),
        };
        pen.SetDashPattern(dashPattern, 2);
        pen.SetDashCap(Gdiplus::DashCapRound);
    }

    if (screenPoints.size() == 1U) {
        Gdiplus::SolidBrush brush(color);
        const Gdiplus::REAL radius = penWidth * 0.5f;
        graphics.FillEllipse(
            &brush,
            screenPoints.front().X - radius,
            screenPoints.front().Y - radius,
            penWidth,
            penWidth);
        return;
    }

    if (screenPoints.size() == 2U) {
        graphics.DrawLines(&pen, screenPoints.data(), static_cast<INT>(screenPoints.size()));
        return;
    }

    Gdiplus::GraphicsPath path;
    path.AddCurve(screenPoints.data(), static_cast<INT>(screenPoints.size()), 0.1f + (effectiveSmoothing * 0.8f));
    graphics.DrawPath(&pen, &path);
}

void DrawPageText(Gdiplus::Graphics& graphics, const PageText& text, float pageWidth, float renderScale) {
    if (text.content.empty()) {
        return;
    }

    const WrappedTextLayout layout = LayoutPageText(text, pageWidth);
    const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(text.x * renderScale);
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(text.y * renderScale);
    const Gdiplus::REAL renderWidth = static_cast<Gdiplus::REAL>(
        std::max(1.0f, ResolvedPageTextWidth(text, pageWidth) * std::max(0.1f, renderScale)));
    const Gdiplus::REAL fontSize = static_cast<Gdiplus::REAL>(std::max(1.0f, text.fontSize * std::max(0.1f, renderScale)));
    const Gdiplus::REAL lineHeight = static_cast<Gdiplus::REAL>(std::max(
        fontSize,
        (static_cast<float>(layout.lineHeight) / kTextLayoutScale) * std::max(0.1f, renderScale)));
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentNear);
    format.SetLineAlignment(Gdiplus::StringAlignmentNear);
    format.SetFormatFlags(
        Gdiplus::StringFormatFlagsNoClip
        | Gdiplus::StringFormatFlagsNoWrap
        | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    format.SetTrimming(Gdiplus::StringTrimmingNone);

    const PageTextStyle defaultStyle {
        text.fontSize,
        text.color,
        text.fontFamily,
        text.bold,
        text.italic,
        text.underline,
    };
    const auto styleAt = [&](std::size_t sourceIndex) -> const PageTextStyle& {
        for (const PageTextStyleSpan& span : text.styleSpans) {
            const std::size_t spanStart = span.start;
            const std::size_t spanEnd = spanStart + span.length;
            if (sourceIndex >= spanStart && sourceIndex < spanEnd) {
                return span.style;
            }
        }
        return defaultStyle;
    };
    const auto fontStyleFor = [](const PageTextStyle& style) {
        Gdiplus::FontStyle result = Gdiplus::FontStyleRegular;
        if (style.bold) {
            result = static_cast<Gdiplus::FontStyle>(result | Gdiplus::FontStyleBold);
        }
        if (style.italic) {
            result = static_cast<Gdiplus::FontStyle>(result | Gdiplus::FontStyleItalic);
        }
        if (style.underline) {
            result = static_cast<Gdiplus::FontStyle>(result | Gdiplus::FontStyleUnderline);
        }
        return result;
    };
    const auto measureSegment = [&](std::u16string_view value, const PageTextStyle& style) {
        Gdiplus::FontFamily family(PageTextFontFace(style.fontFamily));
        Gdiplus::Font font(
            &family,
            static_cast<Gdiplus::REAL>(std::max(1.0f, style.fontSize * std::max(0.1f, renderScale))),
            fontStyleFor(style),
            Gdiplus::UnitPixel);
        Gdiplus::RectF bounds;
        const std::wstring wideValue = WideFromUtf16(value);
        graphics.MeasureString(wideValue.c_str(), static_cast<INT>(wideValue.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &format, &bounds);
        return bounds.Width;
    };

    for (std::size_t lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const std::u16string& line = layout.lines[lineIndex];
        const std::size_t sourceStart = lineIndex < layout.lineStarts.size() ? layout.lineStarts[lineIndex] : 0U;
        const Gdiplus::REAL lineTop = top + (lineHeight * static_cast<Gdiplus::REAL>(lineIndex));

        struct Segment {
            std::u16string value;
            PageTextStyle style;
            Gdiplus::REAL width = 0.0f;
        };
        std::vector<Segment> segments;
        for (std::size_t offset = 0; offset < line.size();) {
            const PageTextStyle& style = styleAt(sourceStart + offset);
            std::size_t end = offset + 1U;
            while (end < line.size() && PageTextStyleEqual(styleAt(sourceStart + end), style)) {
                ++end;
            }
            Segment segment { line.substr(offset, end - offset), style, 0.0f };
            segment.width = measureSegment(segment.value, segment.style);
            segments.push_back(std::move(segment));
            offset = end;
        }

        Gdiplus::REAL totalWidth = 0.0f;
        for (const Segment& segment : segments) {
            totalWidth += segment.width;
        }
        Gdiplus::REAL drawX = left;
        if (text.alignment == TextAlignment::Center) {
            drawX += std::max(0.0f, (renderWidth - totalWidth) * 0.5f);
        } else if (text.alignment == TextAlignment::Right) {
            drawX += std::max(0.0f, renderWidth - totalWidth);
        }

        for (const Segment& segment : segments) {
            const auto alpha = static_cast<BYTE>((segment.style.color >> 24U) & 0xFFU);
            const auto red = static_cast<BYTE>((segment.style.color >> 16U) & 0xFFU);
            const auto green = static_cast<BYTE>((segment.style.color >> 8U) & 0xFFU);
            const auto blue = static_cast<BYTE>(segment.style.color & 0xFFU);
            Gdiplus::FontFamily family(PageTextFontFace(segment.style.fontFamily));
            Gdiplus::Font font(
                &family,
                static_cast<Gdiplus::REAL>(std::max(1.0f, segment.style.fontSize * std::max(0.1f, renderScale))),
                fontStyleFor(segment.style),
                Gdiplus::UnitPixel);
            Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, red, green, blue));
            const std::wstring segmentWide = WideFromUtf16(segment.value);
            graphics.DrawString(
                segmentWide.c_str(),
                static_cast<INT>(segmentWide.size()),
                &font,
                Gdiplus::PointF(drawX, lineTop),
                &format,
                &brush);
            drawX += segment.width;
        }
    }
}

}  // namespace

std::optional<RasterImage> ComposeTextOverlayRaster(const Page& page, float renderScale) {
    bool hasText = false;
    for (const PageText& text : page.texts) {
        if (!text.content.empty()) {
            hasText = true;
            break;
        }
    }
    const bool hasLatex = cnote::latex::IsInitialized()
        && std::any_of(
            page.latexes.begin(),
            page.latexes.end(),
            [](const PageLatex& latex) { return !latex.content.empty(); });
    if (!hasText && !hasLatex) {
        return std::nullopt;
    }

    RasterImage raster = CreateTransparentRaster(page.width, page.height, renderScale);
    Gdiplus::Bitmap bitmap(
        raster.width,
        raster.height,
        raster.stride,
        PixelFormat32bppARGB,
        reinterpret_cast<BYTE*>(raster.pixels.data()));

    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    for (const PageText& text : page.texts) {
        DrawPageText(graphics, text, page.width, renderScale);
    }

    for (const PageLatex& latex : page.latexes) {
        DrawPageLatex(graphics, latex, renderScale);
    }

    return raster;
}

RasterImage ComposePageRaster(
    const PdfiumBackend& backend,
    const Document& document,
    std::size_t pageIndex,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    bool includeStrokes) {
    const Page& page = document.PageAt(pageIndex);
    std::optional<RasterImage> pdfRaster;
    if (const Document::PdfSource* pdfSource = document.PdfSourceAtIndex(page.sourcePdfSourceIndex);
        pdfSource != nullptr && page.sourcePdfPageIndex >= 0) {
        pdfRaster = backend.RenderPage(pdfSource->pdfData, static_cast<std::size_t>(page.sourcePdfPageIndex), renderScale);
    }

    const bool useStoredRaster = !pdfRaster.has_value()
        && !page.raster.pixels.empty()
        && page.raster.width > 0
        && page.raster.height > 0;
    RasterImage raster = pdfRaster.value_or(CreateBlankRaster(page.width, page.height, renderScale));

    Gdiplus::Bitmap bitmap(
        raster.width,
        raster.height,
        raster.stride,
        PixelFormat32bppARGB,
        reinterpret_cast<BYTE*>(raster.pixels.data()));

    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    if (useStoredRaster) {
        Gdiplus::Bitmap sourceBitmap(
            page.raster.width,
            page.raster.height,
            page.raster.stride,
            PixelFormat32bppARGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(page.raster.pixels.data())));
        graphics.DrawImage(
            &sourceBitmap,
            Gdiplus::RectF(
                0.0f,
                0.0f,
                static_cast<Gdiplus::REAL>(raster.width),
                static_cast<Gdiplus::REAL>(raster.height)));
    }

    for (const PageImage& image : page.images) {
        DrawPageImage(graphics, image, renderScale);
    }

    if (includeStrokes) {
        for (const Stroke& stroke : page.strokes) {
            DrawStroke(graphics, stroke, renderScale, strokeSmoothing, dashLength, dashGap);
        }
    }

    return raster;
}


bool PdfiumBackend::RenderPageToHdc(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
    std::size_t pageIndex,
    HDC hdc,
    int x,
    int y,
    int width,
    int height) const {
#ifdef _WIN32
    if (!initialized_ || !pdfBytes || pdfBytes->empty() || hdc == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return false;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        FPDF_CloseDocument(documentHandle);
        return false;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        FPDF_CloseDocument(documentHandle);
        return false;
    }

    SaveDC(hdc);
    const bool ok = FPDF_RenderPage(
        hdc,
        pageHandle,
        x,
        y,
        width,
        height,
        0,
        FPDF_ANNOT | FPDF_LCD_TEXT) != 0;
    RestoreDC(hdc, -1);

    FPDF_ClosePage(pageHandle);
    FPDF_CloseDocument(documentHandle);
    return ok;
#else
    static_cast<void>(pdfBytes);
    static_cast<void>(pageIndex);
    static_cast<void>(hdc);
    static_cast<void>(x);
    static_cast<void>(y);
    static_cast<void>(width);
    static_cast<void>(height);
    return false;
#endif
}

}  // namespace cnote
