#include "PdfBackend.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include <objidl.h>
#include <gdiplus.h>

#include "fpdf_attachment.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include "fpdfview.h"

#include "ProjectFile.h"
#include "selection/Geometry.h"
#include "text/TextGeometry.h"

namespace cnote {

namespace geometry = cnote;
namespace text = cnote::text;

namespace {

using geometry::BuildSmoothedPoints;
using geometry::DistanceSquared;
using geometry::StrokeRenderSmoothing;
using text::CreatePageTextFont;
using text::LayoutPageText;
using text::PageTextFontFace;
using text::PageTextFontWeight;
using text::ResolvedPageTextWidth;
using text::TrimTrailingLayoutWhitespace;

constexpr float kMinimumStrokeRenderWidth = 1.25f;
constexpr float kStrokeRenderWidthDivisor = 2.15f;
constexpr float kTextLayoutScale = 8.0f;
constexpr wchar_t kEmbeddedCnoteAttachmentName[] = L"cnote-edit-data.cnote";



Gdiplus::StringAlignment GdiplusAlignmentForText(TextAlignment alignment) {
    switch (alignment) {
    case TextAlignment::Center:
        return Gdiplus::StringAlignmentCenter;
    case TextAlignment::Right:
        return Gdiplus::StringAlignmentFar;
    case TextAlignment::Left:
    default:
        return Gdiplus::StringAlignmentNear;
    }
}

struct WrappedTextLayout {
    std::vector<std::wstring> lines;
    int wrapWidth = 0;
    int lineHeight = 0;
    int height = 0;
};

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::wstring& filePath) {
    std::ifstream input(std::filesystem::path(filePath), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return std::nullopt;
        }
    }

    return bytes;
}

FPDF_WIDESTRING PdfWideString(const wchar_t* value) {
    static_assert(sizeof(wchar_t) == sizeof(FPDF_WCHAR));
    return reinterpret_cast<FPDF_WIDESTRING>(value);
}

std::optional<std::wstring> AttachmentName(FPDF_ATTACHMENT attachment) {
    const unsigned long byteLength = FPDFAttachment_GetName(attachment, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR) || byteLength % sizeof(FPDF_WCHAR) != 0) {
        return std::nullopt;
    }

    std::vector<FPDF_WCHAR> buffer(byteLength / sizeof(FPDF_WCHAR));
    const unsigned long copiedLength = FPDFAttachment_GetName(attachment, buffer.data(), byteLength);
    if (copiedLength != byteLength || buffer.empty()) {
        return std::nullopt;
    }

    const std::size_t charCount = buffer.back() == 0 ? buffer.size() - 1U : buffer.size();
    return std::wstring(
        reinterpret_cast<const wchar_t*>(buffer.data()),
        reinterpret_cast<const wchar_t*>(buffer.data()) + charCount);
}

std::optional<std::vector<std::uint8_t>> AttachmentFileBytes(FPDF_ATTACHMENT attachment) {
    unsigned long byteLength = 0;
    if (FPDFAttachment_GetFile(attachment, nullptr, 0, &byteLength) == 0 || byteLength == 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(byteLength);
    unsigned long copiedLength = 0;
    if (FPDFAttachment_GetFile(attachment, bytes.data(), byteLength, &copiedLength) == 0
        || copiedLength != byteLength) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::vector<std::uint8_t>> ExtractEmbeddedCnoteProject(
    const std::vector<std::uint8_t>& pdfBytes) {
    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes.data(), pdfBytes.size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    std::optional<std::vector<std::uint8_t>> projectBytes;
    const int attachmentCount = FPDFDoc_GetAttachmentCount(documentHandle);
    for (int attachmentIndex = 0; attachmentIndex < attachmentCount; ++attachmentIndex) {
        FPDF_ATTACHMENT attachment = FPDFDoc_GetAttachment(documentHandle, attachmentIndex);
        if (attachment == nullptr) {
            continue;
        }

        const std::optional<std::wstring> name = AttachmentName(attachment);
        if (name.has_value() && *name == kEmbeddedCnoteAttachmentName) {
            projectBytes = AttachmentFileBytes(attachment);
            break;
        }
    }

    FPDF_CloseDocument(documentHandle);
    return projectBytes;
}

bool AppendEmbeddedCnoteProject(
    FPDF_DOCUMENT documentHandle,
    const std::vector<std::uint8_t>& projectBytes) {
    if (documentHandle == nullptr || projectBytes.empty()
        || projectBytes.size() > std::numeric_limits<unsigned long>::max()) {
        return false;
    }

    FPDF_ATTACHMENT attachment = FPDFDoc_AddAttachment(documentHandle, PdfWideString(kEmbeddedCnoteAttachmentName));
    if (attachment == nullptr) {
        return false;
    }

    return FPDFAttachment_SetFile(
        attachment,
        documentHandle,
        projectBytes.data(),
        static_cast<unsigned long>(projectBytes.size())) != 0;
}

RasterImage CreateBlankRaster(float pageWidth, float pageHeight, float renderScale) {
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageWidth) * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageHeight) * renderScale)));
    const int stride = bitmapWidth * 4;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    raster.pixels.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight), 0xFF);
    return raster;
}

RasterImage CreateTransparentRaster(float pageWidth, float pageHeight, float renderScale) {
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageWidth) * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageHeight) * renderScale)));
    const int stride = bitmapWidth * 4;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    raster.pixels.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight), 0);
    return raster;
}

std::optional<RasterImage> RenderPageBitmap(FPDF_PAGE pageHandle, float renderScale) {
    if (pageHandle == nullptr || renderScale <= 0.0f) {
        return std::nullopt;
    }

    const float pageWidth = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
    const float pageHeight = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(pageWidth * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(pageHeight * renderScale)));

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(bitmapWidth, bitmapHeight, FPDFBitmap_BGRA, nullptr, 0);
    if (bitmap == nullptr) {
        return std::nullopt;
    }

    RasterImage raster {};
    FPDFBitmap_FillRect(bitmap, 0, 0, bitmapWidth, bitmapHeight, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, pageHandle, 0, 0, bitmapWidth, bitmapHeight, 0, FPDF_ANNOT);

    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = FPDFBitmap_GetStride(bitmap);
    raster.scale = renderScale;

    const auto* source = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    const std::size_t totalBytes = static_cast<std::size_t>(raster.stride) * static_cast<std::size_t>(bitmapHeight);
    raster.pixels.assign(source, source + totalBytes);
    FPDFBitmap_Destroy(bitmap);
    return raster;
}



int PageTextFontPixelHeight(const PageText& text, float scale) {
    return std::max(1, static_cast<int>(std::lround(std::max(1.0f, text.fontSize) * std::max(0.1f, scale))));
}

WrappedTextLayout LayoutPageTextFwd(const PageText& text, float pageWidth) {
    return LayoutPageText(text, pageWidth);
}

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
    const auto alpha = static_cast<BYTE>((text.color >> 24U) & 0xFFU);
    const auto red = static_cast<BYTE>((text.color >> 16U) & 0xFFU);
    const auto green = static_cast<BYTE>((text.color >> 8U) & 0xFFU);
    const auto blue = static_cast<BYTE>(text.color & 0xFFU);

    Gdiplus::FontStyle fontStyle = Gdiplus::FontStyleRegular;
    if (text.bold) {
        fontStyle = static_cast<Gdiplus::FontStyle>(fontStyle | Gdiplus::FontStyleBold);
    }
    if (text.italic) {
        fontStyle = static_cast<Gdiplus::FontStyle>(fontStyle | Gdiplus::FontStyleItalic);
    }
    if (text.underline) {
        fontStyle = static_cast<Gdiplus::FontStyle>(fontStyle | Gdiplus::FontStyleUnderline);
    }

    Gdiplus::FontFamily fontFamily(PageTextFontFace(text.fontFamily));
    Gdiplus::Font font(&fontFamily, fontSize, fontStyle, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, red, green, blue));
    Gdiplus::StringFormat format;
    format.SetAlignment(GdiplusAlignmentForText(text.alignment));
    format.SetLineAlignment(Gdiplus::StringAlignmentNear);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip | Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingNone);

    for (std::size_t lineIndex = 0; lineIndex < layout.lines.size(); ++lineIndex) {
        const std::wstring& line = layout.lines[lineIndex];
        const Gdiplus::REAL lineTop = top + (lineHeight * static_cast<Gdiplus::REAL>(lineIndex));
        const Gdiplus::RectF lineRect(left, lineTop, renderWidth + (fontSize * 0.5f), lineHeight * 1.25f);
        graphics.DrawString(line.c_str(), static_cast<INT>(line.size()), &font, lineRect, &format, &brush);
    }
}

std::optional<RasterImage> ComposeTextOverlayRaster(const Page& page, float renderScale) {
    bool hasText = false;
    for (const PageText& text : page.texts) {
        if (!text.content.empty()) {
            hasText = true;
            break;
        }
    }
    if (!hasText) {
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

float ToPdfY(float pageHeight, float pageY) {
    return pageHeight - pageY;
}

bool AppendVectorStroke(FPDF_PAGE pageHandle, const Page& page, const Stroke& stroke, float strokeSmoothing, float dashLength, float dashGap) {
    if (stroke.points.empty()) {
        return true;
    }

    const std::vector<Vec2> smoothedPoints = BuildSmoothedPoints(
        stroke.points,
        StrokeRenderSmoothing(stroke, strokeSmoothing));
    if (smoothedPoints.empty()) {
        return true;
    }

    const Vec2 firstPoint = smoothedPoints.front();
    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(firstPoint.x, ToPdfY(page.height, firstPoint.y));
    if (path == nullptr) {
        return false;
    }

    bool ok = true;
    if (smoothedPoints.size() == 1U) {
        ok = FPDFPath_LineTo(path, firstPoint.x + 0.01f, ToPdfY(page.height, firstPoint.y)) != 0;
    } else {
        for (std::size_t pointIndex = 1; ok && pointIndex < smoothedPoints.size(); ++pointIndex) {
            ok = FPDFPath_LineTo(
                path,
                smoothedPoints[pointIndex].x,
                ToPdfY(page.height, smoothedPoints[pointIndex].y)) != 0;
        }
    }

    const auto alpha = static_cast<unsigned int>((stroke.style.color >> 24U) & 0xFFU);
    const auto red = static_cast<unsigned int>((stroke.style.color >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned int>((stroke.style.color >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned int>(stroke.style.color & 0xFFU);
    const float pdfStrokeWidth = std::max(0.6f, stroke.style.width / kStrokeRenderWidthDivisor);

    ok = ok
        && FPDFPageObj_SetStrokeColor(path, red, green, blue, alpha) != 0
        && FPDFPageObj_SetStrokeWidth(path, pdfStrokeWidth) != 0
        && FPDFPageObj_SetLineJoin(path, FPDF_LINEJOIN_ROUND) != 0
        && FPDFPageObj_SetLineCap(path, FPDF_LINECAP_ROUND) != 0
        && FPDFPath_SetDrawMode(path, FPDF_FILLMODE_NONE, 1) != 0;
    if (ok && stroke.style.dotted) {
        const float dashArray[] {
            std::max(0.01f, pdfStrokeWidth * std::max(0.5f, dashLength)),
            std::max(0.01f, pdfStrokeWidth * std::max(0.5f, dashGap)),
        };
        ok = FPDFPageObj_SetDashArray(path, dashArray, 2, 0.0f) != 0;
    }
    if (!ok) {
        return false;
    }

    FPDFPage_InsertObject(pageHandle, path);
    return true;
}

bool AppendBitmapObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    int pixelWidth,
    int pixelHeight,
    int stride,
    std::uint8_t* pixels,
    double a,
    double b,
    double c,
    double d,
    double e,
    double f,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    if (documentHandle == nullptr || pageHandle == nullptr || pixels == nullptr
        || pixelWidth <= 0 || pixelHeight <= 0 || stride <= 0) {
        return false;
    }

    FPDF_BITMAP bitmapHandle = FPDFBitmap_CreateEx(
        pixelWidth,
        pixelHeight,
        FPDFBitmap_BGRA,
        pixels,
        stride);
    if (bitmapHandle == nullptr) {
        return false;
    }

    FPDF_PAGEOBJECT imageObject = FPDFPageObj_NewImageObj(documentHandle);
    if (imageObject == nullptr) {
        FPDFBitmap_Destroy(bitmapHandle);
        return false;
    }

    const bool ok = FPDFImageObj_SetBitmap(nullptr, 0, imageObject, bitmapHandle) != 0
        && FPDFImageObj_SetMatrix(imageObject, a, b, c, d, e, f) != 0;
    if (!ok) {
        FPDFPageObj_Destroy(imageObject);
        FPDFBitmap_Destroy(bitmapHandle);
        return false;
    }

    ownedBitmaps.push_back(bitmapHandle);
    FPDFPage_InsertObject(pageHandle, imageObject);
    return true;
}

bool AppendFullPageRasterObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    const Page& page,
    RasterImage& raster,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    return AppendBitmapObject(
        documentHandle,
        pageHandle,
        raster.width,
        raster.height,
        raster.stride,
        raster.pixels.data(),
        page.width,
        0.0,
        0.0,
        page.height,
        0.0,
        0.0,
        ownedBitmaps);
}

bool AppendPageImageObject(
    FPDF_DOCUMENT documentHandle,
    FPDF_PAGE pageHandle,
    const Page& page,
    const PageImage& image,
    std::vector<FPDF_BITMAP>& ownedBitmaps) {
    if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0 || image.stride <= 0) {
        return true;
    }

    const double rotationRadians = -static_cast<double>(image.rotationDegrees) * std::numbers::pi / 180.0;
    const double cosine = std::cos(rotationRadians);
    const double sine = std::sin(rotationRadians);
    const double width = static_cast<double>(image.width);
    const double height = static_cast<double>(image.height);
    const double centerX = static_cast<double>(image.x) + (width * 0.5);
    const double centerY = static_cast<double>(page.height) - static_cast<double>(image.y) - (height * 0.5);
    const double a = cosine * width;
    const double b = sine * width;
    const double c = -sine * height;
    const double d = cosine * height;
    const double e = centerX - ((a + c) * 0.5);
    const double f = centerY - ((b + d) * 0.5);

    return AppendBitmapObject(
        documentHandle,
        pageHandle,
        image.pixelWidth,
        image.pixelHeight,
        image.stride,
        const_cast<std::uint8_t*>(image.pixels.data()),
        a,
        b,
        c,
        d,
        e,
        f,
        ownedBitmaps);
}

struct PdfFileWriter {
    FPDF_FILEWRITE writer {};
    std::FILE* file = nullptr;

    static int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
        auto* fileWriter = reinterpret_cast<PdfFileWriter*>(self);
        return std::fwrite(data, 1, size, fileWriter->file) == size ? 1 : 0;
    }
};

}  // namespace

PdfiumBackend::PdfiumBackend() {
    FPDF_InitLibrary();
    initialized_ = true;
}

PdfiumBackend::~PdfiumBackend() {
    if (initialized_) {
        FPDF_DestroyLibrary();
    }
}

std::optional<Document> PdfiumBackend::Open(const std::wstring& filePath) {
    if (!initialized_ || filePath.empty()) {
        return std::nullopt;
    }

    const auto pdfBytes = ReadAllBytes(filePath);
    if (!pdfBytes.has_value()) {
        return std::nullopt;
    }

    if (const auto embeddedProject = ExtractEmbeddedCnoteProject(*pdfBytes); embeddedProject.has_value()) {
        if (auto projectDocument = ProjectFile::LoadFromBytes(*embeddedProject, filePath, *this);
            projectDocument.has_value()) {
            return projectDocument;
        }
    }

    return OpenFromMemory(filePath, std::make_shared<std::vector<std::uint8_t>>(std::move(*pdfBytes)));
}

std::optional<Document> PdfiumBackend::OpenFromMemory(
    const std::wstring& sourcePath,
    std::shared_ptr<std::vector<std::uint8_t>> pdfBytes) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty()) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    std::vector<Page> pages;
    pages.reserve(static_cast<std::size_t>(pageCount));

    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, pageIndex);
        if (pageHandle == nullptr) {
            continue;
        }

        Page page {};
        page.width = std::max(1.0f, FPDF_GetPageWidthF(pageHandle));
        page.height = std::max(1.0f, FPDF_GetPageHeightF(pageHandle));
        page.sourcePdfSourceIndex = 0;
        page.sourcePdfPageIndex = pageIndex;

        pages.push_back(std::move(page));
        FPDF_ClosePage(pageHandle);
    }

    FPDF_CloseDocument(documentHandle);

    if (pages.empty()) {
        return std::nullopt;
    }

    return Document::CreatePdfDocument(sourcePath, std::move(pdfBytes), std::move(pages), 0, {});
}

std::optional<RasterImage> PdfiumBackend::RenderPage(
    const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
    std::size_t pageIndex,
    float renderScale) const {
    if (!initialized_ || !pdfBytes || pdfBytes->empty() || renderScale <= 0.0f) {
        return std::nullopt;
    }

    FPDF_DOCUMENT documentHandle = FPDF_LoadMemDocument64(pdfBytes->data(), pdfBytes->size(), nullptr);
    if (documentHandle == nullptr) {
        return std::nullopt;
    }

    const int pageCount = FPDF_GetPageCount(documentHandle);
    if (pageCount <= 0 || pageIndex >= static_cast<std::size_t>(pageCount)) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    FPDF_PAGE pageHandle = FPDF_LoadPage(documentHandle, static_cast<int>(pageIndex));
    if (pageHandle == nullptr) {
        FPDF_CloseDocument(documentHandle);
        return std::nullopt;
    }

    const auto raster = RenderPageBitmap(pageHandle, renderScale);

    FPDF_ClosePage(pageHandle);
    FPDF_CloseDocument(documentHandle);
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

bool PdfiumBackend::ExportPdf(
    const Document& document,
    const std::wstring& filePath,
    float renderScale,
    float strokeSmoothing,
    float dashLength,
    float dashGap,
    bool embedProject) const {
    if (!initialized_ || filePath.empty() || document.PageCount() == 0U || renderScale <= 0.0f) {
        return false;
    }

    FPDF_DOCUMENT outputDocument = FPDF_CreateNewDocument();
    if (outputDocument == nullptr) {
        return false;
    }

    std::vector<RasterImage> flattenedPages;
    std::vector<FPDF_BITMAP> ownedBitmaps;
    std::vector<FPDF_DOCUMENT> sourceDocuments(document.PdfSourceCount(), nullptr);
    std::vector<FPDF_PAGE> createdPages;
    flattenedPages.reserve(document.PageCount());
    ownedBitmaps.reserve(document.PageCount());
    createdPages.reserve(document.PageCount());

    bool ok = true;
    for (std::size_t pageIndex = 0; ok && pageIndex < document.PageCount(); ++pageIndex) {
        const Page& sourcePage = document.PageAt(pageIndex);
        FPDF_PAGE pageHandle = nullptr;
        bool importedSourcePage = false;
        bool imagesAlreadyFlattened = false;

        if (const Document::PdfSource* pdfSource = document.PdfSourceAtIndex(sourcePage.sourcePdfSourceIndex);
            pdfSource != nullptr && sourcePage.sourcePdfPageIndex >= 0) {
            FPDF_DOCUMENT& sourceDocument = sourceDocuments[static_cast<std::size_t>(sourcePage.sourcePdfSourceIndex)];
            if (sourceDocument == nullptr && pdfSource->pdfData && !pdfSource->pdfData->empty()) {
                sourceDocument = FPDF_LoadMemDocument64(pdfSource->pdfData->data(), pdfSource->pdfData->size(), nullptr);
            }

            if (sourceDocument != nullptr) {
                const int sourcePageIndex = sourcePage.sourcePdfPageIndex;
                importedSourcePage = FPDF_ImportPagesByIndex(
                    outputDocument,
                    sourceDocument,
                    &sourcePageIndex,
                    1,
                    static_cast<int>(pageIndex)) != 0;
                if (importedSourcePage) {
                    pageHandle = FPDF_LoadPage(outputDocument, static_cast<int>(pageIndex));
                    if (pageHandle == nullptr) {
                        ok = false;
                        break;
                    }
                }
            }
        }

        if (pageHandle == nullptr) {
            pageHandle = FPDFPage_New(
                outputDocument,
                static_cast<int>(pageIndex),
                sourcePage.width,
                sourcePage.height);
            if (pageHandle == nullptr) {
                ok = false;
                break;
            }

            bool hasBackgroundRaster = false;
            if (!importedSourcePage && sourcePage.sourcePdfPageIndex >= 0) {
                flattenedPages.push_back(ComposePageRaster(*this, document, pageIndex, renderScale, strokeSmoothing, dashLength, dashGap, false));
                hasBackgroundRaster = true;
                imagesAlreadyFlattened = true;
            } else if (!sourcePage.raster.pixels.empty()
                && sourcePage.raster.width > 0
                && sourcePage.raster.height > 0
                && sourcePage.raster.stride > 0) {
                flattenedPages.push_back(sourcePage.raster);
                hasBackgroundRaster = true;
            }

            if (hasBackgroundRaster && !AppendFullPageRasterObject(
                    outputDocument,
                    pageHandle,
                    sourcePage,
                    flattenedPages.back(),
                    ownedBitmaps)) {
                ok = false;
                break;
            }
        }
        createdPages.push_back(pageHandle);

        if (!imagesAlreadyFlattened) {
            for (const PageImage& image : sourcePage.images) {
                ok = AppendPageImageObject(outputDocument, pageHandle, sourcePage, image, ownedBitmaps);
                if (!ok) {
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }

        if (const auto textOverlay = ComposeTextOverlayRaster(sourcePage, renderScale); textOverlay.has_value()) {
            flattenedPages.push_back(std::move(*textOverlay));
            ok = AppendFullPageRasterObject(
                outputDocument,
                pageHandle,
                sourcePage,
                flattenedPages.back(),
                ownedBitmaps);
            if (!ok) {
                break;
            }
        }

        for (const Stroke& stroke : sourcePage.strokes) {
            ok = AppendVectorStroke(pageHandle, sourcePage, stroke, strokeSmoothing, dashLength, dashGap);
            if (!ok) {
                break;
            }
        }
        ok = FPDFPage_GenerateContent(pageHandle) != 0;
    }

    if (ok && embedProject) {
        const std::vector<std::uint8_t> projectBytes = ProjectFile::Serialize(document);
        static_cast<void>(AppendEmbeddedCnoteProject(outputDocument, projectBytes));
    }

    if (ok) {
        std::FILE* file = _wfopen(filePath.c_str(), L"wb");
        if (file == nullptr) {
            ok = false;
        } else {
            PdfFileWriter fileWriter {};
            fileWriter.writer.version = 1;
            fileWriter.writer.WriteBlock = &PdfFileWriter::WriteBlock;
            fileWriter.file = file;
            ok = FPDF_SaveAsCopy(outputDocument, &fileWriter.writer, FPDF_NO_INCREMENTAL) != 0;
            std::fclose(file);
        }
    }

    for (FPDF_BITMAP bitmapHandle : ownedBitmaps) {
        FPDFBitmap_Destroy(bitmapHandle);
    }
    for (FPDF_PAGE pageHandle : createdPages) {
        FPDF_ClosePage(pageHandle);
    }
    for (FPDF_DOCUMENT sourceDocument : sourceDocuments) {
        if (sourceDocument != nullptr) {
            FPDF_CloseDocument(sourceDocument);
        }
    }
    FPDF_CloseDocument(outputDocument);
    return ok;
}

}  // namespace cnote
