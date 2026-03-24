#include "App.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

namespace cnote {

namespace {

constexpr wchar_t kWindowClassName[] = L"CnoteWindow";
constexpr wchar_t kWindowTitle[] = L"cnote";
constexpr int kToolbarPadding = 18;
constexpr int kIconButtonSize = 34;
constexpr int kColorSwatchSize = 26;
constexpr int kColorSwatchGap = 10;
constexpr unsigned int kCommandFileNew = 1001;
constexpr unsigned int kCommandFileOpen = 1002;
constexpr unsigned int kCommandFileSaveCnote = 1003;
constexpr unsigned int kCommandFileExportPdf = 1004;
constexpr unsigned int kCommandFileExit = 1005;
constexpr unsigned int kCommandEditUndo = 1101;
constexpr unsigned int kCommandEditRedo = 1102;
constexpr unsigned int kCommandEditBrushSettings = 1103;
constexpr unsigned int kCommandEditCopy = 1104;
constexpr unsigned int kCommandEditPaste = 1105;
constexpr unsigned int kCommandSelectLasso = 1111;
constexpr unsigned int kCommandViewReset = 1201;
constexpr unsigned int kCommandViewPreviousPage = 1202;
constexpr unsigned int kCommandViewNextPage = 1203;

struct MenuLayout {
    RECT fileRect {};
    RECT editRect {};
    RECT viewRect {};
};

struct ToolbarLayout {
    MenuLayout menu {};
    RECT previousPageRect {};
    RECT nextPageRect {};
    RECT pageLabelRect {};
    RECT undoRect {};
    RECT redoRect {};
    RECT selectToolRect {};
    RECT moveToolRect {};
    RECT drawToolRect {};
    RECT panToolRect {};
    RECT eraseToolRect {};
    RECT currentColorRect {};
};

struct PaletteMenuLayout {
    RECT panelRect {};
    std::vector<RECT> swatchRects;
};

struct PaletteWheelLayout {
    POINT center {};
    int innerRadius = 0;
    int outerRadius = 0;
};

struct SettingsLayout {
    RECT panelRect {};
    std::vector<RECT> paletteRects;
    RECT pasteHexRect {};
    RECT addColorRect {};
    RECT removeColorRect {};
    RECT redTrack {};
    RECT greenTrack {};
    RECT blueTrack {};
    RECT brushSizeTrack {};
    RECT stabilizationTrack {};
    RECT spacingTrack {};
    RECT smoothingTrack {};
};

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }

    return path.substr(slash + 1);
}

std::wstring Lowercase(std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
    return value;
}

bool HasExtension(const std::wstring& path, const wchar_t* extension) {
    const std::wstring normalizedPath = Lowercase(path);
    const std::wstring normalizedExtension = Lowercase(extension == nullptr ? L"" : extension);
    if (normalizedExtension.empty() || normalizedPath.size() < normalizedExtension.size()) {
        return false;
    }
    return normalizedPath.ends_with(normalizedExtension);
}

struct SvgPathStyle {
    bool fill = true;
    bool stroke = false;
    float strokeWidth = 1.0f;
    Gdiplus::LineCap lineCap = Gdiplus::LineCapRound;
    Gdiplus::LineJoin lineJoin = Gdiplus::LineJoinRound;
};

struct SvgPathShape {
    std::unique_ptr<Gdiplus::GraphicsPath> path;
    SvgPathStyle style {};
};

struct SvgIconData {
    float viewBoxX = 0.0f;
    float viewBoxY = 0.0f;
    float viewBoxWidth = 24.0f;
    float viewBoxHeight = 24.0f;
    std::vector<SvgPathShape> shapes;
    bool loaded = false;
};

std::optional<std::string> ReadUtf8TextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);

    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!contents.empty()) {
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!input) {
            return std::nullopt;
        }
    }

    return contents;
}

std::optional<std::string> ExtractXmlAttribute(std::string_view tag, std::string_view attribute) {
    const std::string needle = std::string(attribute) + "=\"";
    const std::size_t start = tag.find(needle);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }

    const std::size_t valueStart = start + needle.size();
    const std::size_t valueEnd = tag.find('"', valueStart);
    if (valueEnd == std::string_view::npos) {
        return std::nullopt;
    }

    return std::string(tag.substr(valueStart, valueEnd - valueStart));
}

void SkipSvgSeparators(const char*& cursor) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n' || *cursor == ',') {
        ++cursor;
    }
}

bool ConsumeSvgFloat(const char*& cursor, float& value) {
    SkipSvgSeparators(cursor);
    if (*cursor == '\0') {
        return false;
    }

    char* end = nullptr;
    value = std::strtof(cursor, &end);
    if (end == cursor) {
        return false;
    }
    cursor = end;
    SkipSvgSeparators(cursor);
    return true;
}

bool IsSvgCommandLetter(char ch) {
    switch (ch) {
    case 'M':
    case 'm':
    case 'L':
    case 'l':
    case 'H':
    case 'h':
    case 'V':
    case 'v':
    case 'C':
    case 'c':
    case 'Z':
    case 'z':
        return true;
    default:
        return false;
    }
}

std::unique_ptr<Gdiplus::GraphicsPath> ParseSvgPathData(const std::string& pathData) {
    auto path = std::make_unique<Gdiplus::GraphicsPath>();
    const char* cursor = pathData.c_str();
    char command = '\0';
    Gdiplus::PointF current(0.0f, 0.0f);
    Gdiplus::PointF subpathStart(0.0f, 0.0f);
    bool hasCurrentPoint = false;

    while (*cursor != '\0') {
        SkipSvgSeparators(cursor);
        if (*cursor == '\0') {
            break;
        }

        if (IsSvgCommandLetter(*cursor)) {
            command = *cursor++;
        } else if (command == '\0') {
            return {};
        }

        switch (command) {
        case 'M':
        case 'm': {
            bool firstPoint = true;
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x = 0.0f;
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, x) || !ConsumeSvgFloat(cursor, y)) {
                    return {};
                }
                if (command == 'm' && hasCurrentPoint) {
                    x += current.X;
                    y += current.Y;
                }

                if (firstPoint) {
                    path->StartFigure();
                    current = Gdiplus::PointF(x, y);
                    subpathStart = current;
                    hasCurrentPoint = true;
                    firstPoint = false;
                } else {
                    path->AddLine(current, Gdiplus::PointF(x, y));
                    current = Gdiplus::PointF(x, y);
                }
            }
            break;
        }
        case 'L':
        case 'l':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x = 0.0f;
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, x) || !ConsumeSvgFloat(cursor, y) || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'l') {
                    x += current.X;
                    y += current.Y;
                }
                path->AddLine(current, Gdiplus::PointF(x, y));
                current = Gdiplus::PointF(x, y);
            }
            break;
        case 'H':
        case 'h':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x = 0.0f;
                if (!ConsumeSvgFloat(cursor, x) || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'h') {
                    x += current.X;
                }
                path->AddLine(current, Gdiplus::PointF(x, current.Y));
                current = Gdiplus::PointF(x, current.Y);
            }
            break;
        case 'V':
        case 'v':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float y = 0.0f;
                if (!ConsumeSvgFloat(cursor, y) || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'v') {
                    y += current.Y;
                }
                path->AddLine(current, Gdiplus::PointF(current.X, y));
                current = Gdiplus::PointF(current.X, y);
            }
            break;
        case 'C':
        case 'c':
            while (*cursor != '\0' && !IsSvgCommandLetter(*cursor)) {
                float x1 = 0.0f;
                float y1 = 0.0f;
                float x2 = 0.0f;
                float y2 = 0.0f;
                float x3 = 0.0f;
                float y3 = 0.0f;
                if (!ConsumeSvgFloat(cursor, x1) || !ConsumeSvgFloat(cursor, y1)
                    || !ConsumeSvgFloat(cursor, x2) || !ConsumeSvgFloat(cursor, y2)
                    || !ConsumeSvgFloat(cursor, x3) || !ConsumeSvgFloat(cursor, y3)
                    || !hasCurrentPoint) {
                    return {};
                }
                if (command == 'c') {
                    x1 += current.X;
                    y1 += current.Y;
                    x2 += current.X;
                    y2 += current.Y;
                    x3 += current.X;
                    y3 += current.Y;
                }
                path->AddBezier(current, Gdiplus::PointF(x1, y1), Gdiplus::PointF(x2, y2), Gdiplus::PointF(x3, y3));
                current = Gdiplus::PointF(x3, y3);
            }
            break;
        case 'Z':
        case 'z':
            if (hasCurrentPoint) {
                path->CloseFigure();
                current = subpathStart;
            }
            break;
        default:
            return {};
        }
    }

    return path;
}

std::optional<std::array<float, 4>> ParseViewBox(const std::string& viewBox) {
    const char* cursor = viewBox.c_str();
    std::array<float, 4> values {};
    for (float& value : values) {
        if (!ConsumeSvgFloat(cursor, value)) {
            return std::nullopt;
        }
    }
    return values;
}

Gdiplus::LineCap ParseSvgLineCap(const std::string& value) {
    if (value == "butt") {
        return Gdiplus::LineCapFlat;
    }
    if (value == "square") {
        return Gdiplus::LineCapSquare;
    }
    return Gdiplus::LineCapRound;
}

Gdiplus::LineJoin ParseSvgLineJoin(const std::string& value) {
    if (value == "bevel") {
        return Gdiplus::LineJoinBevel;
    }
    if (value == "miter") {
        return Gdiplus::LineJoinMiter;
    }
    return Gdiplus::LineJoinRound;
}

std::optional<SvgIconData> LoadSvgIcon(const std::wstring& fileName) {
    const std::array<std::filesystem::path, 3> basePaths {
        std::filesystem::current_path(),
        std::filesystem::path([] {
            wchar_t buffer[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return std::filesystem::path(buffer).parent_path();
        }()),
        std::filesystem::path([] {
            wchar_t buffer[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return std::filesystem::path(buffer).parent_path().parent_path();
        }()),
    };

    std::optional<std::string> svgContents;
    for (const auto& basePath : basePaths) {
        const auto candidate = basePath / "assets" / "icons" / fileName;
        svgContents = ReadUtf8TextFile(candidate);
        if (svgContents.has_value()) {
            break;
        }
    }

    if (!svgContents.has_value()) {
        return std::nullopt;
    }

    SvgIconData icon {};
    const std::size_t svgStart = svgContents->find("<svg");
    if (svgStart == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t svgEnd = svgContents->find('>', svgStart);
    if (svgEnd == std::string::npos) {
        return std::nullopt;
    }

    const std::string_view svgTag(svgContents->data() + svgStart, (svgEnd - svgStart) + 1U);
    if (const auto viewBox = ExtractXmlAttribute(svgTag, "viewBox"); viewBox.has_value()) {
        if (const auto parsed = ParseViewBox(*viewBox); parsed.has_value()) {
            icon.viewBoxX = (*parsed)[0];
            icon.viewBoxY = (*parsed)[1];
            icon.viewBoxWidth = (*parsed)[2];
            icon.viewBoxHeight = (*parsed)[3];
        }
    }

    std::size_t searchStart = 0;
    while (true) {
        const std::size_t pathStart = svgContents->find("<path", searchStart);
        if (pathStart == std::string::npos) {
            break;
        }
        const std::size_t pathEnd = svgContents->find('>', pathStart);
        if (pathEnd == std::string::npos) {
            return std::nullopt;
        }

        const std::string_view pathTag(svgContents->data() + pathStart, (pathEnd - pathStart) + 1U);
        const auto pathData = ExtractXmlAttribute(pathTag, "d");
        if (!pathData.has_value()) {
            searchStart = pathEnd + 1U;
            continue;
        }

        SvgPathShape shape {};
        shape.path = ParseSvgPathData(*pathData);
        if (!shape.path) {
            return std::nullopt;
        }

        if (const auto fill = ExtractXmlAttribute(pathTag, "fill"); fill.has_value()) {
            shape.style.fill = *fill != "none";
        }
        if (const auto stroke = ExtractXmlAttribute(pathTag, "stroke"); stroke.has_value()) {
            shape.style.stroke = *stroke != "none";
        }
        if (const auto strokeWidth = ExtractXmlAttribute(pathTag, "stroke-width"); strokeWidth.has_value()) {
            const char* cursor = strokeWidth->c_str();
            float parsedWidth = 1.0f;
            if (ConsumeSvgFloat(cursor, parsedWidth)) {
                shape.style.strokeWidth = parsedWidth;
            }
        }
        if (const auto lineCap = ExtractXmlAttribute(pathTag, "stroke-linecap"); lineCap.has_value()) {
            shape.style.lineCap = ParseSvgLineCap(*lineCap);
        }
        if (const auto lineJoin = ExtractXmlAttribute(pathTag, "stroke-linejoin"); lineJoin.has_value()) {
            shape.style.lineJoin = ParseSvgLineJoin(*lineJoin);
        }

        icon.shapes.push_back(std::move(shape));
        searchStart = pathEnd + 1U;
    }

    if (icon.shapes.empty()) {
        return std::nullopt;
    }

    icon.loaded = true;
    return icon;
}

const SvgIconData* ToolbarSvgIcon(std::wstring_view fileName) {
    struct CacheEntry {
        std::wstring name;
        std::optional<SvgIconData> icon;
    };

    static std::vector<CacheEntry> cache;
    const auto existing = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry& entry) {
        return entry.name == fileName;
    });
    if (existing != cache.end()) {
        return existing->icon.has_value() ? &*existing->icon : nullptr;
    }

    cache.push_back({ std::wstring(fileName), LoadSvgIcon(std::wstring(fileName)) });
    return cache.back().icon.has_value() ? &*cache.back().icon : nullptr;
}

void PaintSvgIcon(HDC hdc, RECT rect, const SvgIconData& icon, COLORREF color) {
    if (!icon.loaded || icon.shapes.empty() || icon.viewBoxWidth <= 0.0f || icon.viewBoxHeight <= 0.0f) {
        return;
    }

    const float availableWidth = static_cast<float>((rect.right - rect.left) - 8);
    const float availableHeight = static_cast<float>((rect.bottom - rect.top) - 8);
    const float scale = std::max(0.01f, std::min(availableWidth / icon.viewBoxWidth, availableHeight / icon.viewBoxHeight));
    const float drawWidth = icon.viewBoxWidth * scale;
    const float drawHeight = icon.viewBoxHeight * scale;
    const float offsetX = static_cast<float>(rect.left) + ((rect.right - rect.left) - drawWidth) * 0.5f;
    const float offsetY = static_cast<float>(rect.top) + ((rect.bottom - rect.top) - drawHeight) * 0.5f;
    const auto alpha = static_cast<BYTE>(0xFF);
    const auto red = GetRValue(color);
    const auto green = GetGValue(color);
    const auto blue = GetBValue(color);
    const Gdiplus::Color gdipColor(alpha, red, green, blue);

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    for (const SvgPathShape& shape : icon.shapes) {
        std::unique_ptr<Gdiplus::GraphicsPath> transformed(shape.path->Clone());
        if (!transformed) {
            continue;
        }

        Gdiplus::Matrix matrix(
            scale,
            0.0f,
            0.0f,
            scale,
            offsetX - (icon.viewBoxX * scale),
            offsetY - (icon.viewBoxY * scale));
        transformed->Transform(&matrix);

        if (shape.style.fill) {
            Gdiplus::SolidBrush brush(gdipColor);
            graphics.FillPath(&brush, transformed.get());
        }

        if (shape.style.stroke) {
            Gdiplus::Pen pen(gdipColor, std::max(1.0f, shape.style.strokeWidth * scale));
            pen.SetLineCap(shape.style.lineCap, shape.style.lineCap, Gdiplus::DashCapRound);
            pen.SetLineJoin(shape.style.lineJoin);
            graphics.DrawPath(&pen, transformed.get());
        }
    }
}

ToolbarLayout BuildToolbarLayout(RECT clientRect, std::size_t colorCount) {
    ToolbarLayout layout;
    static_cast<void>(colorCount);

    const int rightEdge = static_cast<int>(clientRect.right) - kToolbarPadding;
    const int toolbarTop = 10;
    const int colorButtonSize = kIconButtonSize;
    const int colorsLeft = rightEdge - colorButtonSize;

    layout.menu.fileRect = MakeRect(14, toolbarTop + 4, 58, toolbarTop + 28);
    layout.menu.editRect = MakeRect(layout.menu.fileRect.right + 8, toolbarTop + 4, layout.menu.fileRect.right + 52, toolbarTop + 28);
    layout.menu.viewRect = MakeRect(layout.menu.editRect.right + 8, toolbarTop + 4, layout.menu.editRect.right + 54, toolbarTop + 28);
    layout.previousPageRect = MakeRect(layout.menu.viewRect.right + 20, toolbarTop, layout.menu.viewRect.right + 20 + kIconButtonSize, toolbarTop + kIconButtonSize);
    layout.pageLabelRect = MakeRect(layout.previousPageRect.right + 8, toolbarTop + 3, layout.previousPageRect.right + 92, toolbarTop + kIconButtonSize - 3);
    layout.nextPageRect = MakeRect(layout.pageLabelRect.right + 8, toolbarTop, layout.pageLabelRect.right + 8 + kIconButtonSize, toolbarTop + kIconButtonSize);
    layout.currentColorRect = MakeRect(colorsLeft, toolbarTop, colorsLeft + colorButtonSize, toolbarTop + colorButtonSize);

    const int toolBandRight = colorsLeft - 24;
    layout.panToolRect = MakeRect(
        toolBandRight - kIconButtonSize,
        toolbarTop,
        toolBandRight,
        toolbarTop + kIconButtonSize);
    layout.drawToolRect = MakeRect(
        layout.panToolRect.left - 10 - kIconButtonSize,
        toolbarTop,
        layout.panToolRect.left - 10,
        toolbarTop + kIconButtonSize);
    layout.eraseToolRect = MakeRect(
        layout.drawToolRect.left - 10 - kIconButtonSize,
        toolbarTop,
        layout.drawToolRect.left - 10,
        toolbarTop + kIconButtonSize);
    layout.selectToolRect = MakeRect(
        layout.eraseToolRect.left - 10 - kIconButtonSize,
        toolbarTop,
        layout.eraseToolRect.left - 10,
        toolbarTop + kIconButtonSize);
    layout.moveToolRect = MakeRect(
        layout.selectToolRect.left - 10 - kIconButtonSize,
        toolbarTop,
        layout.selectToolRect.left - 10,
        toolbarTop + kIconButtonSize);
    layout.redoRect = MakeRect(
        layout.moveToolRect.left - 22 - kIconButtonSize,
        toolbarTop,
        layout.moveToolRect.left - 22,
        toolbarTop + kIconButtonSize);
    layout.undoRect = MakeRect(
        layout.redoRect.left - 10 - kIconButtonSize,
        toolbarTop,
        layout.redoRect.left - 10,
        toolbarTop + kIconButtonSize);

    return layout;
}

PaletteMenuLayout BuildPaletteMenuLayout(const ToolbarLayout& toolbarLayout, std::size_t colorCount) {
    PaletteMenuLayout layout;
    const int columns = std::min(4, std::max(1, static_cast<int>(colorCount)));
    const int swatchSize = 28;
    const int gap = 8;
    const int rows = std::max(1, static_cast<int>((colorCount + static_cast<std::size_t>(columns) - 1U) / static_cast<std::size_t>(columns)));
    const int panelPadding = 12;
    const int panelWidth = (columns * swatchSize) + ((columns - 1) * gap) + (panelPadding * 2);
    const int panelHeight = (rows * swatchSize) + ((rows - 1) * gap) + (panelPadding * 2);
    layout.panelRect = MakeRect(
        toolbarLayout.currentColorRect.right - panelWidth,
        toolbarLayout.currentColorRect.bottom + 8,
        toolbarLayout.currentColorRect.right,
        toolbarLayout.currentColorRect.bottom + 8 + panelHeight);

    layout.swatchRects.reserve(colorCount);
    for (std::size_t index = 0; index < colorCount; ++index) {
        const int column = static_cast<int>(index % static_cast<std::size_t>(columns));
        const int row = static_cast<int>(index / static_cast<std::size_t>(columns));
        const int left = layout.panelRect.left + panelPadding + (column * (swatchSize + gap));
        const int top = layout.panelRect.top + panelPadding + (row * (swatchSize + gap));
        layout.swatchRects.push_back(MakeRect(left, top, left + swatchSize, top + swatchSize));
    }

    return layout;
}

PaletteWheelLayout BuildPaletteWheelLayout(RECT clientRect, POINT requestedCenter, std::size_t colorCount) {
    PaletteWheelLayout layout;
    constexpr int kToolbarBottom = 54;
    layout.innerRadius = 20;
    layout.outerRadius = colorCount <= 1U ? 40 : 86;
    const int margin = layout.outerRadius + 12;
    layout.center.x = std::clamp(static_cast<int>(requestedCenter.x), margin, static_cast<int>(clientRect.right) - margin);
    layout.center.y = std::clamp(static_cast<int>(requestedCenter.y), kToolbarBottom + margin, static_cast<int>(clientRect.bottom) - margin);
    return layout;
}

std::optional<std::size_t> PaletteWheelIndexAtPoint(const PaletteWheelLayout& layout, POINT cursor, std::size_t colorCount) {
    if (colorCount == 0U) {
        return std::nullopt;
    }

    const float dx = static_cast<float>(cursor.x - layout.center.x);
    const float dy = static_cast<float>(cursor.y - layout.center.y);
    const float distanceSquared = (dx * dx) + (dy * dy);
    const float innerSquared = static_cast<float>(layout.innerRadius * layout.innerRadius);
    const float outerSquared = static_cast<float>(layout.outerRadius * layout.outerRadius);

    if (distanceSquared < innerSquared || distanceSquared > outerSquared) {
        return std::nullopt;
    }

    if (colorCount == 1U) {
        return 0U;
    }

    constexpr float kWheelPi = 3.14159265358979323846f;
    float angle = std::atan2(dy, dx);
    angle += kWheelPi / 2.0f;
    if (angle < 0.0f) {
        angle += 2.0f * kWheelPi;
    }

    const float normalized = angle / (2.0f * kWheelPi);
    const std::size_t index = std::min(
        colorCount - 1U,
        static_cast<std::size_t>(std::floor(normalized * static_cast<float>(colorCount))));
    return index;
}

SettingsLayout BuildSettingsLayout(RECT clientRect, std::size_t colorCount) {
    SettingsLayout layout;
    const int panelWidth = 320;
    const int panelLeft = std::max(24, static_cast<int>(clientRect.right) - panelWidth - 24);
    const int panelTop = 54 + 10;
    layout.panelRect = MakeRect(panelLeft, panelTop, panelLeft + panelWidth, panelTop + 418);

    const int contentLeft = panelLeft + 18;
    const int sliderLeft = contentLeft;
    const int sliderRight = layout.panelRect.right - 18;
    const int sliderHeight = 10;

    layout.paletteRects.reserve(colorCount);
    const int paletteTop = panelTop + 48;
    for (std::size_t index = 0; index < colorCount; ++index) {
        const int column = static_cast<int>(index % 6U);
        const int row = static_cast<int>(index / 6U);
        const int left = contentLeft + (column * 44);
        const int top = paletteTop + (row * 34);
        layout.paletteRects.push_back(MakeRect(left, top, left + 36, top + 26));
    }

    const int paletteRows = static_cast<int>((colorCount + 5U) / 6U);
    const int actionsTop = paletteTop + (std::max(1, paletteRows) * 34) + 4;
    layout.pasteHexRect = MakeRect(contentLeft, actionsTop, contentLeft + 118, actionsTop + 28);
    layout.addColorRect = MakeRect(layout.pasteHexRect.right + 10, actionsTop, layout.pasteHexRect.right + 54, actionsTop + 28);
    layout.removeColorRect = MakeRect(layout.addColorRect.right + 10, actionsTop, layout.addColorRect.right + 54, actionsTop + 28);

    const int sliderStart = actionsTop + 54;
    layout.redTrack = MakeRect(sliderLeft, sliderStart, sliderRight, sliderStart + sliderHeight);
    layout.greenTrack = MakeRect(sliderLeft, sliderStart + 36, sliderRight, sliderStart + 36 + sliderHeight);
    layout.blueTrack = MakeRect(sliderLeft, sliderStart + 72, sliderRight, sliderStart + 72 + sliderHeight);
    layout.brushSizeTrack = MakeRect(sliderLeft, sliderStart + 118, sliderRight, sliderStart + 118 + sliderHeight);
    layout.stabilizationTrack = MakeRect(sliderLeft, sliderStart + 164, sliderRight, sliderStart + 164 + sliderHeight);
    layout.spacingTrack = MakeRect(sliderLeft, sliderStart + 200, sliderRight, sliderStart + 200 + sliderHeight);
    layout.smoothingTrack = MakeRect(sliderLeft, sliderStart + 236, sliderRight, sliderStart + 236 + sliderHeight);

    layout.panelRect.bottom = layout.smoothingTrack.bottom + 24;

    const int maxBottom = static_cast<int>(clientRect.bottom) - 12;
    if (layout.panelRect.bottom > maxBottom) {
        const int minTop = 62;
        const int availableShift = std::max(0, static_cast<int>(layout.panelRect.top) - minTop);
        const int overflow = static_cast<int>(layout.panelRect.bottom) - maxBottom;
        const int shift = std::min(overflow, availableShift);
        OffsetRect(&layout.panelRect, 0, -shift);
        OffsetRect(&layout.pasteHexRect, 0, -shift);
        OffsetRect(&layout.addColorRect, 0, -shift);
        OffsetRect(&layout.removeColorRect, 0, -shift);
        OffsetRect(&layout.redTrack, 0, -shift);
        OffsetRect(&layout.greenTrack, 0, -shift);
        OffsetRect(&layout.blueTrack, 0, -shift);
        OffsetRect(&layout.brushSizeTrack, 0, -shift);
        OffsetRect(&layout.stabilizationTrack, 0, -shift);
        OffsetRect(&layout.spacingTrack, 0, -shift);
        OffsetRect(&layout.smoothingTrack, 0, -shift);
        for (RECT& rect : layout.paletteRects) {
            OffsetRect(&rect, 0, -shift);
        }
    }

    return layout;
}

void FillRoundedRect(HDC hdc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void StrokeRoundedRect(HDC hdc, RECT rect, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void FillSolidRect(HDC hdc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

template <std::size_t N>
void FillSolidPolygon(HDC hdc, const std::array<POINT, N>& points, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, points.data(), static_cast<int>(points.size()));
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void PaintSelectToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const RECT box = MakeRect(centerX - 8, centerY - 7, centerX + 8, centerY + 7);
    const int thickness = 2;
    const int handle = 3;

    FillSolidRect(hdc, MakeRect(box.left, box.top, box.right, box.top + thickness), color);
    FillSolidRect(hdc, MakeRect(box.left, box.bottom - thickness, box.right, box.bottom), color);
    FillSolidRect(hdc, MakeRect(box.left, box.top, box.left + thickness, box.bottom), color);
    FillSolidRect(hdc, MakeRect(box.right - thickness, box.top, box.right, box.bottom), color);

    FillSolidRect(hdc, MakeRect(box.left - 1, box.top - 1, box.left - 1 + handle, box.top - 1 + handle), color);
    FillSolidRect(hdc, MakeRect(box.right - handle + 1, box.top - 1, box.right + 1, box.top - 1 + handle), color);
    FillSolidRect(hdc, MakeRect(box.left - 1, box.bottom - handle + 1, box.left - 1 + handle, box.bottom + 1), color);
    FillSolidRect(hdc, MakeRect(box.right - handle + 1, box.bottom - handle + 1, box.right + 1, box.bottom + 1), color);
}

void PaintMoveToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const std::array<POINT, 7> arrow {
        POINT { centerX - 8, centerY - 9 },
        POINT { centerX - 8, centerY + 8 },
        POINT { centerX - 4, centerY + 4 },
        POINT { centerX - 1, centerY + 11 },
        POINT { centerX + 3, centerY + 9 },
        POINT { centerX, centerY + 2 },
        POINT { centerX + 7, centerY + 2 },
    };
    FillSolidPolygon(hdc, arrow, color);
}

void PaintDrawToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const std::array<POINT, 4> body {
        POINT { centerX - 7, centerY + 5 },
        POINT { centerX + 2, centerY - 8 },
        POINT { centerX + 7, centerY - 3 },
        POINT { centerX - 2, centerY + 10 },
    };
    const std::array<POINT, 3> tip {
        POINT { centerX - 4, centerY + 8 },
        POINT { centerX - 1, centerY + 12 },
        POINT { centerX - 7, centerY + 11 },
    };
    FillSolidPolygon(hdc, body, color);
    FillSolidPolygon(hdc, tip, color);
}

void PaintPanToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    FillSolidRect(hdc, MakeRect(centerX - 5, centerY - 1, centerX + 5, centerY + 8), color);
    FillSolidRect(hdc, MakeRect(centerX - 7, centerY - 7, centerX - 4, centerY + 1), color);
    FillSolidRect(hdc, MakeRect(centerX - 3, centerY - 10, centerX, centerY + 1), color);
    FillSolidRect(hdc, MakeRect(centerX + 1, centerY - 9, centerX + 4, centerY + 1), color);
    FillSolidRect(hdc, MakeRect(centerX + 5, centerY - 5, centerX + 8, centerY + 2), color);
    const std::array<POINT, 4> thumb {
        POINT { centerX - 5, centerY + 1 },
        POINT { centerX - 11, centerY - 2 },
        POINT { centerX - 9, centerY + 4 },
        POINT { centerX - 5, centerY + 6 },
    };
    FillSolidPolygon(hdc, thumb, color);
}

void PaintEraseToolIcon(HDC hdc, RECT rect, COLORREF color) {
    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    const std::array<POINT, 4> eraser {
        POINT { centerX - 7, centerY + 4 },
        POINT { centerX - 1, centerY - 8 },
        POINT { centerX + 8, centerY - 1 },
        POINT { centerX + 2, centerY + 10 },
    };
    FillSolidPolygon(hdc, eraser, color);
    FillSolidRect(hdc, MakeRect(centerX - 1, centerY + 5, centerX + 7, centerY + 8), RGB(15, 18, 24));
}

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return {
        from.x + ((to.x - from.x) * amount),
        from.y + ((to.y - from.y) * amount),
    };
}

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength) {
    if (points.size() <= 2U || strength <= 0.001f) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    const float sideWeight = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    const float centerWeight = 1.0f - (sideWeight * 2.0f);

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * sideWeight) + (current.x * centerWeight) + (next.x * sideWeight),
            (previous.y * sideWeight) + (current.y * centerWeight) + (next.y * sideWeight),
        };

        if (DistanceSquared(filtered, smoothed.back()) >= 0.05f) {
            smoothed.push_back(filtered);
        }
    }

    if (DistanceSquared(points.back(), smoothed.back()) >= 0.01f) {
        smoothed.push_back(points.back());
    }

    return smoothed;
}

std::wstring Trimmed(std::wstring value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](wchar_t ch) { return !iswspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](wchar_t ch) { return !iswspace(ch); }).base(), value.end());
    return value;
}

std::optional<std::uint32_t> ParseHexColor(std::wstring value) {
    value = Trimmed(std::move(value));
    if (value.empty()) {
        return std::nullopt;
    }

    if (value.starts_with(L"#")) {
        value.erase(0, 1);
    }
    if (value.starts_with(L"0x") || value.starts_with(L"0X")) {
        value.erase(0, 2);
    }

    if (value.size() != 6U && value.size() != 8U) {
        return std::nullopt;
    }

    for (wchar_t ch : value) {
        if (!iswxdigit(ch)) {
            return std::nullopt;
        }
    }

    const unsigned long parsed = std::wcstoul(value.c_str(), nullptr, 16);
    if (value.size() == 6U) {
        return 0xFF000000U | static_cast<std::uint32_t>(parsed);
    }

    return static_cast<std::uint32_t>(parsed);
}

bool PointInRectF(Vec2 point, float left, float top, float right, float bottom) {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
}

float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = (dx * dx) + (dy * dy);
    if (lengthSquared <= 0.0001f) {
        return DistanceSquared(point, a);
    }

    float t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vec2 projection {
        a.x + (dx * t),
        a.y + (dy * t),
    };
    return DistanceSquared(point, projection);
}

float CrossProduct(Vec2 a, Vec2 b, Vec2 c) {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

bool OnSegment(Vec2 a, Vec2 b, Vec2 point) {
    return point.x >= std::min(a.x, b.x) - 0.001f
        && point.x <= std::max(a.x, b.x) + 0.001f
        && point.y >= std::min(a.y, b.y) - 0.001f
        && point.y <= std::max(a.y, b.y) + 0.001f;
}

bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2) {
    const float d1 = CrossProduct(a1, a2, b1);
    const float d2 = CrossProduct(a1, a2, b2);
    const float d3 = CrossProduct(b1, b2, a1);
    const float d4 = CrossProduct(b1, b2, a2);

    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f))
        && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }

    if (std::abs(d1) <= 0.001f && OnSegment(a1, a2, b1)) {
        return true;
    }
    if (std::abs(d2) <= 0.001f && OnSegment(a1, a2, b2)) {
        return true;
    }
    if (std::abs(d3) <= 0.001f && OnSegment(b1, b2, a1)) {
        return true;
    }
    if (std::abs(d4) <= 0.001f && OnSegment(b1, b2, a2)) {
        return true;
    }
    return false;
}

bool SegmentIntersectsRectF(Vec2 a, Vec2 b, float left, float top, float right, float bottom) {
    if (PointInRectF(a, left, top, right, bottom) || PointInRectF(b, left, top, right, bottom)) {
        return true;
    }

    return SegmentsIntersect(a, b, { left, top }, { right, top })
        || SegmentsIntersect(a, b, { right, top }, { right, bottom })
        || SegmentsIntersect(a, b, { right, bottom }, { left, bottom })
        || SegmentsIntersect(a, b, { left, bottom }, { left, top });
}

bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1U; i < polygon.size(); j = i++) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[j];
        const bool intersects = ((a.y > point.y) != (b.y > point.y))
            && (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 0.00001f)) + a.x);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius) {
    const float radiusSquared = radius * radius;
    if (stroke.points.empty()) {
        return false;
    }

    if (DistanceSquared(stroke.points.front(), point) <= radiusSquared) {
        return true;
    }

    for (std::size_t index = 1; index < stroke.points.size(); ++index) {
        if (DistancePointToSegmentSquared(point, stroke.points[index - 1], stroke.points[index]) <= radiusSquared) {
            return true;
        }
    }

    return false;
}

bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b) {
    if (stroke.points.empty()) {
        return false;
    }

    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x, b.x);
    const float bottom = std::max(a.y, b.y);

    for (const Vec2& point : stroke.points) {
        if (PointInRectF(point, left, top, right, bottom)) {
            return true;
        }
    }

    for (std::size_t index = 1; index < stroke.points.size(); ++index) {
        if (SegmentIntersectsRectF(stroke.points[index - 1], stroke.points[index], left, top, right, bottom)) {
            return true;
        }
    }

    return false;
}

bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon) {
    if (stroke.points.empty() || polygon.size() < 3U) {
        return false;
    }

    for (const Vec2& point : stroke.points) {
        if (PointInPolygon(point, polygon)) {
            return true;
        }
    }

    for (std::size_t index = 1; index < stroke.points.size(); ++index) {
        const Vec2 strokeA = stroke.points[index - 1];
        const Vec2 strokeB = stroke.points[index];
        for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
            const Vec2 polyA = polygon[edgeIndex];
            const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
            if (SegmentsIntersect(strokeA, strokeB, polyA, polyB)) {
                return true;
            }
        }
    }

    return false;
}

constexpr float kPi = 3.14159265358979323846f;

float DegreesToRadians(float degrees) {
    return degrees * (kPi / 180.0f);
}

Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees) {
    const float radians = DegreesToRadians(degrees);
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const float translatedX = point.x - center.x;
    const float translatedY = point.y - center.y;
    return {
        center.x + (translatedX * cosine) - (translatedY * sine),
        center.y + (translatedX * sine) + (translatedY * cosine),
    };
}

Vec2 ImageCenter(const PageImage& image) {
    return {
        image.x + (image.width * 0.5f),
        image.y + (image.height * 0.5f),
    };
}

std::array<Vec2, 4> ImageCorners(const PageImage& image) {
    const Vec2 center = ImageCenter(image);
    std::array<Vec2, 4> corners {
        Vec2 { image.x, image.y },
        Vec2 { image.x + image.width, image.y },
        Vec2 { image.x + image.width, image.y + image.height },
        Vec2 { image.x, image.y + image.height },
    };
    for (Vec2& corner : corners) {
        corner = RotatePointAround(corner, center, image.rotationDegrees);
    }
    return corners;
}

Vec2 ScreenPointFromPagePoint(Vec2 point, RECT pageRect, float pageScale) {
    return {
        static_cast<float>(pageRect.left) + (point.x * pageScale),
        static_cast<float>(pageRect.top) + (point.y * pageScale),
    };
}

Vec2 ToImageLocalPoint(const PageImage& image, Vec2 point) {
    const Vec2 center = ImageCenter(image);
    return RotatePointAround(point, center, -image.rotationDegrees);
}

bool ImageHitsPoint(const PageImage& image, Vec2 point) {
    const Vec2 local = ToImageLocalPoint(image, point);
    return PointInRectF(local, image.x, image.y, image.x + image.width, image.y + image.height);
}

bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b) {
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x, b.x);
    const float bottom = std::max(a.y, b.y);
    const auto corners = ImageCorners(image);
    for (const Vec2& corner : corners) {
        if (PointInRectF(corner, left, top, right, bottom)) {
            return true;
        }
    }
    if (ImageHitsPoint(image, { left, top }) || ImageHitsPoint(image, { right, top })
        || ImageHitsPoint(image, { right, bottom }) || ImageHitsPoint(image, { left, bottom })) {
        return true;
    }
    for (std::size_t index = 0; index < corners.size(); ++index) {
        if (SegmentIntersectsRectF(corners[index], corners[(index + 1U) % corners.size()], left, top, right, bottom)) {
            return true;
        }
    }
    return false;
}

bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    const auto corners = ImageCorners(image);

    for (const Vec2& corner : corners) {
        if (PointInPolygon(corner, polygon)) {
            return true;
        }
    }

    for (const Vec2& point : polygon) {
        if (ImageHitsPoint(image, point)) {
            return true;
        }
    }

    for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
        const Vec2 polyA = polygon[edgeIndex];
        const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
        for (std::size_t imageEdgeIndex = 0; imageEdgeIndex < corners.size(); ++imageEdgeIndex) {
            if (SegmentsIntersect(polyA, polyB, corners[imageEdgeIndex], corners[(imageEdgeIndex + 1U) % corners.size()])) {
                return true;
            }
        }
    }

    return false;
}

UINT SelectionClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"cnote.selection.v1");
    return format;
}

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok) {
        gdiplusToken_ = token;
    }
    ClearSelection();
    StartRenderWorker();
}

App::~App() {
    StopRenderWorker();
    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
    }
}

void App::StartRenderWorker() {
    renderWorkerRunning_ = true;
    renderThread_ = std::thread([this]() {
        PdfiumBackend workerBackend {};
        for (;;) {
            RenderRequest request {};
            {
                std::unique_lock lock(renderMutex_);
                renderCv_.wait(lock, [this]() { return !renderWorkerRunning_ || !renderRequests_.empty(); });
                if (!renderWorkerRunning_ && renderRequests_.empty()) {
                    break;
                }
                request = std::move(renderRequests_.front());
                renderRequests_.pop_front();
            }

            RenderResult result {};
            result.pageIndex = request.pageIndex;
            result.renderScale = request.renderScale;
            result.generation = request.generation;
            result.raster = workerBackend.RenderPage(request.pdfData, request.pageIndex, request.renderScale);

            {
                std::lock_guard lock(renderMutex_);
                renderResults_.push_back(std::move(result));
            }

            if (window_ != nullptr) {
                PostMessageW(window_, kRenderResultsMessage, 0, 0);
            }
        }
    });
}

void App::StopRenderWorker() {
    {
        std::lock_guard lock(renderMutex_);
        renderWorkerRunning_ = false;
        renderRequests_.clear();
    }
    renderCv_.notify_all();
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
}

int App::Run(int showCommand, std::optional<std::wstring> startupPath) {
    if (!CreateMainWindow(showCommand)) {
        return 1;
    }

    if (startupPath.has_value() && !startupPath->empty()) {
        static_cast<void>(OpenDocumentPath(*startupPath));
    }

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}

bool App::CreateMainWindow(int showCommand) {
    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &App::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        return false;
    }

    window_ = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1400,
        960,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr) {
        return false;
    }

    constexpr DWORD kDwmDarkModeAttribute = 20;
    constexpr DWORD kDwmBorderColorAttribute = 34;
    constexpr DWORD kDwmCaptionColorAttribute = 35;
    constexpr DWORD kDwmTextColorAttribute = 36;
    const BOOL useDarkMode = TRUE;
    const COLORREF chromeColor = RGB(15, 18, 24);
    const COLORREF chromeText = RGB(236, 240, 245);
    DwmSetWindowAttribute(window_, kDwmDarkModeAttribute, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(window_, kDwmBorderColorAttribute, &chromeColor, sizeof(chromeColor));
    DwmSetWindowAttribute(window_, kDwmCaptionColorAttribute, &chromeColor, sizeof(chromeColor));
    DwmSetWindowAttribute(window_, kDwmTextColorAttribute, &chromeText, sizeof(chromeText));
    UpdateWindowTitle();

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

void App::CreateBlankDocument() {
    PrepareForDocumentSwitch();
    ApplyDocument(Document::CreateBlank(), true);
}

void App::PrepareForDocumentSwitch() {
    CancelDeferredRasterRefresh();
    KillTimer(window_, kZoomSessionTimerId);
    zoomSessionActive_ = false;
    zoomAnchorValid_ = false;
    ++renderGeneration_;
    pendingRenderScales_.clear();
    {
        std::lock_guard lock(renderMutex_);
        renderRequests_.clear();
        renderResults_.clear();
    }
}

void App::ApplyDocument(Document document, bool resetBrushWidth) {
    document_ = std::move(document);
    pageSurfaceCaches_.assign(document_.PageCount(), {});
    ClearSelection();
    ClosePaletteOverlays();
    selectionPageIndex_.reset();
    selectionLassoPoints_.clear();
    isSelecting_ = false;
    ClearHistory();
    if (resetBrushWidth) {
        brushWidth_ = 3.0f;
    }
    ResetView();
    EnsureVisiblePageRasterQuality();
    UpdateWindowTitle();
    InvalidateRect(window_, nullptr, FALSE);
}

std::wstring App::SuggestedCnotePath() const {
    std::wstring basePath = document_.FilePath();
    if (basePath.empty()) {
        basePath = document_.SourcePath();
    }

    std::filesystem::path suggestion;
    if (!basePath.empty()) {
        suggestion = std::filesystem::path(basePath);
        suggestion.replace_extension(kCnoteFileExtension);
    } else {
        suggestion = std::filesystem::path(L"Untitled").replace_extension(kCnoteFileExtension);
    }

    return suggestion.native();
}

std::wstring App::SuggestedPdfExportPath() const {
    std::wstring basePath = document_.FilePath();
    if (basePath.empty()) {
        basePath = document_.SourcePath();
    }

    std::filesystem::path suggestion;
    if (!basePath.empty()) {
        suggestion = std::filesystem::path(basePath);
        suggestion.replace_extension(L".pdf");
    } else {
        suggestion = std::filesystem::path(L"Untitled").replace_extension(L".pdf");
    }

    return suggestion.native();
}

void App::ClearHistory() {
    undoActions_.clear();
    redoActions_.clear();
}

void App::ClosePaletteOverlays() {
    paletteMenuOpen_ = false;
    paletteWheelOpen_ = false;
}

void App::TogglePaletteWheel() {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    POINT cursor {};
    GetCursorPos(&cursor);
    ScreenToClient(window_, &cursor);

    if (cursor.x < 0 || cursor.y < 0 || cursor.x >= clientRect.right || cursor.y >= clientRect.bottom) {
        const ToolbarLayout toolbarLayout = BuildToolbarLayout(clientRect, palette_.size());
        cursor.x = (toolbarLayout.currentColorRect.left + toolbarLayout.currentColorRect.right) / 2;
        cursor.y = toolbarLayout.currentColorRect.bottom + 72;
    }

    paletteWheelCenter_ = cursor;
    paletteWheelOpen_ = !paletteWheelOpen_;
    paletteMenuOpen_ = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::InvalidateCanvas() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    clientRect.top = kToolbarHeight;
    InvalidateRect(window_, &clientRect, FALSE);
}

void App::InvalidatePageSurfaceCache(std::size_t pageIndex) {
    if (pageIndex >= pageSurfaceCaches_.size()) {
        return;
    }

    PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    cache.valid = false;
    cache.pixels.clear();
    cache.width = 0;
    cache.height = 0;
    cache.stride = 0;
    cache.imageCount = 0;
    cache.rasterWidth = 0;
    cache.rasterHeight = 0;
    cache.rasterScale = 0.0f;
    cache.pageScale = 0.0f;
    cache.strokeCount = 0;
}

void App::InvalidateAllPageSurfaceCaches() {
    for (std::size_t pageIndex = 0; pageIndex < pageSurfaceCaches_.size(); ++pageIndex) {
        InvalidatePageSurfaceCache(pageIndex);
    }
}

void App::InvalidateVisiblePageSurfaceCaches() {
    if (window_ == nullptr) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int visibleTop = kToolbarHeight;
    const int visibleBottom = static_cast<int>(clientRect.bottom);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < visibleTop || pageView.rect.top > visibleBottom) {
            continue;
        }
        InvalidatePageSurfaceCache(pageView.pageIndex);
    }
}

void App::InvalidateStrokeRegion(const Stroke& stroke, std::size_t pageIndex, bool smoothLiveStroke) {
    if (window_ == nullptr || stroke.points.empty()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    if (!pageView.valid) {
        return;
    }

    const std::vector<Vec2> renderPoints = smoothLiveStroke
        ? BuildSmoothedPoints(stroke.points, strokeSmoothing_)
        : stroke.points;
    if (renderPoints.empty()) {
        return;
    }

    float minX = renderPoints.front().x;
    float minY = renderPoints.front().y;
    float maxX = renderPoints.front().x;
    float maxY = renderPoints.front().y;
    for (const Vec2& point : renderPoints) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }

    const float penWidth = std::max(1.25f, stroke.style.width * pageView.scale / 2.15f);
    const int padding = static_cast<int>(std::ceil(penWidth)) + 12;
    RECT dirtyRect = MakeRect(
        pageView.rect.left + static_cast<int>(std::floor(minX * pageView.scale)) - padding,
        pageView.rect.top + static_cast<int>(std::floor(minY * pageView.scale)) - padding,
        pageView.rect.left + static_cast<int>(std::ceil(maxX * pageView.scale)) + padding + 1,
        pageView.rect.top + static_cast<int>(std::ceil(maxY * pageView.scale)) + padding + 1);
    InvalidateRect(window_, &dirtyRect, FALSE);
}

void App::ResetView() {
    zoom_ = 1.0f;
    targetZoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
}

void App::SetActiveTool(Tool tool) {
    if (tool != Tool::Select) {
        isSelecting_ = false;
        selectionPageIndex_.reset();
        selectionLassoPoints_.clear();
        selectionBoxOverride_ = false;
        UpdateCaptureState();
    }
    if (tool != Tool::Move) {
        isMovingSelection_ = false;
        moveSelectionPageIndex_.reset();
        isTransformingImage_ = false;
        transformingImagePageIndex_.reset();
        transformingImageIndex_.reset();
        imageTransformHandle_ = ImageTransformHandle::None;
        isTransformingStroke_ = false;
        transformingStrokePageIndex_.reset();
        strokeTransformHandle_ = ImageTransformHandle::None;
        strokeTransformInitials_.clear();
    }
    activeTool_ = tool;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::PreviousPage() {
    if (CanGoToPreviousPage()) {
        FocusPage(document_.CurrentPageIndex() - 1U);
    }
}

void App::NextPage() {
    if (CanGoToNextPage()) {
        FocusPage(document_.CurrentPageIndex() + 1U);
    }
}

void App::SetCurrentPageIndex(std::size_t index) {
    if (document_.SetCurrentPageIndex(index)) {
        CancelDeferredRasterRefresh();
        targetZoom_ = zoom_;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::FocusPage(std::size_t index) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, index);
    if (!pageView.valid) {
        return;
    }

    document_.SetCurrentPageIndex(index);
    const int canvasTop = kToolbarHeight;
    const int canvasHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top) - kToolbarHeight);
    const int pageCenterY = (pageView.rect.top + pageView.rect.bottom) / 2;
    const int desiredCenterY = canvasTop + (canvasHeight / 2);
    panY_ += static_cast<float>(desiredCenterY - pageCenterY);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ToggleBrushSettings() {
    settingsOpen_ = !settingsOpen_;
    activeSlider_ = SliderTarget::None;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ClearSelection() {
    selectedImageIndices_.assign(document_.PageCount(), {});
    selectedStrokeIndices_.assign(document_.PageCount(), {});
}

bool App::HasSelection() const {
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if ((pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty())
            || (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty())) {
            return true;
        }
    }

    return false;
}

bool App::HasSelectedStrokes() const {
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty()) {
            return true;
        }
    }

    return false;
}

void App::DeleteSelection() {
    if (!HasSelection()) {
        return;
    }

    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty()) {
            auto imageIndices = selectedImageIndices_[pageIndex];
            std::sort(imageIndices.rbegin(), imageIndices.rend());
            auto& images = document_.PageAt(pageIndex).images;
            for (std::size_t imageIndex : imageIndices) {
                if (imageIndex < images.size()) {
                    images.erase(images.begin() + static_cast<std::ptrdiff_t>(imageIndex));
                }
            }
            InvalidatePageSurfaceCache(pageIndex);
        }

        if (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty()) {
            auto strokeIndices = selectedStrokeIndices_[pageIndex];
            std::sort(strokeIndices.rbegin(), strokeIndices.rend());
            auto& strokes = document_.PageAt(pageIndex).strokes;
            for (std::size_t strokeIndex : strokeIndices) {
                if (strokeIndex < strokes.size()) {
                    strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(strokeIndex));
                }
            }
            InvalidatePageSurfaceCache(pageIndex);
        }
    }

    ClearSelection();
    isMovingSelection_ = false;
    isTransformingImage_ = false;
    isTransformingStroke_ = false;
    transformingImagePageIndex_.reset();
    transformingImageIndex_.reset();
    transformingStrokePageIndex_.reset();
    imageTransformHandle_ = ImageTransformHandle::None;
    strokeTransformHandle_ = ImageTransformHandle::None;
    strokeTransformInitials_.clear();
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::AdjustSelectionForRemoval(std::size_t pageIndex, std::size_t strokeIndex) {
    if (pageIndex >= selectedStrokeIndices_.size()) {
        return;
    }

    auto& selected = selectedStrokeIndices_[pageIndex];
    selected.erase(
        std::remove_if(
            selected.begin(),
            selected.end(),
            [&](std::size_t index) {
                if (index == strokeIndex) {
                    return true;
                }
                if (index > strokeIndex) {
                    --index;
                }
                return false;
            }),
        selected.end());

    for (std::size_t& index : selected) {
        if (index > strokeIndex) {
            --index;
        }
    }
}

void App::AdjustSelectionForInsertion(std::size_t pageIndex, std::size_t strokeIndex) {
    if (pageIndex >= selectedStrokeIndices_.size()) {
        return;
    }

    for (std::size_t& index : selectedStrokeIndices_[pageIndex]) {
        if (index >= strokeIndex) {
            ++index;
        }
    }
}

bool App::IsStrokeSelected(std::size_t pageIndex, std::size_t strokeIndex) const {
    if (pageIndex >= selectedStrokeIndices_.size()) {
        return false;
    }

    const auto& selected = selectedStrokeIndices_[pageIndex];
    return std::find(selected.begin(), selected.end(), strokeIndex) != selected.end();
}

bool App::IsImageSelected(std::size_t pageIndex, std::size_t imageIndex) const {
    if (pageIndex >= selectedImageIndices_.size()) {
        return false;
    }

    const auto& selected = selectedImageIndices_[pageIndex];
    return std::find(selected.begin(), selected.end(), imageIndex) != selected.end();
}

std::optional<Vec2> App::DefaultPasteOrigin(std::size_t pageIndex) const {
    if (pageIndex >= document_.PageCount()) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(pageIndex);
    return Vec2 {
        page.width * 0.5f,
        page.height * 0.5f,
    };
}

std::optional<std::pair<std::size_t, std::size_t>> App::SingleSelectedImage() const {
    std::optional<std::pair<std::size_t, std::size_t>> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageIndex].empty()) {
            return std::nullopt;
        }
        if (pageIndex >= selectedImageIndices_.size()) {
            continue;
        }
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (result.has_value()) {
                return std::nullopt;
            }
            result = std::make_pair(pageIndex, imageIndex);
        }
    }
    return result;
}

std::optional<std::size_t> App::SingleSelectedStrokePage() const {
    std::optional<std::size_t> result;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex < selectedImageIndices_.size() && !selectedImageIndices_[pageIndex].empty()) {
            return std::nullopt;
        }
        if (pageIndex >= selectedStrokeIndices_.size() || selectedStrokeIndices_[pageIndex].empty()) {
            continue;
        }
        if (result.has_value()) {
            return std::nullopt;
        }
        result = pageIndex;
    }
    return result;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedImageHandle(POINT cursor, std::size_t* pageIndex, std::size_t* imageIndex) const {
    const auto singleImage = SingleSelectedImage();
    if (!singleImage.has_value() || window_ == nullptr) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, singleImage->first);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(singleImage->first);
    if (singleImage->second >= page.images.size()) {
        return std::nullopt;
    }

    const PageImage& image = page.images[singleImage->second];
    const auto corners = ImageCorners(image);
    const Vec2 topMid {
        (corners[0].x + corners[1].x) * 0.5f,
        (corners[0].y + corners[1].y) * 0.5f,
    };
    const Vec2 bottomMid {
        (corners[2].x + corners[3].x) * 0.5f,
        (corners[2].y + corners[3].y) * 0.5f,
    };
    const Vec2 leftMid {
        (corners[0].x + corners[3].x) * 0.5f,
        (corners[0].y + corners[3].y) * 0.5f,
    };
    const Vec2 rightMid {
        (corners[1].x + corners[2].x) * 0.5f,
        (corners[1].y + corners[2].y) * 0.5f,
    };
    const Vec2 center = ImageCenter(image);
    const Vec2 rotateHandle = RotatePointAround({ center.x, image.y - 18.0f / pageView.scale }, center, image.rotationDegrees);

    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, ScreenPointFromPagePoint(corners[0], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorth, ScreenPointFromPagePoint(topMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorthEast, ScreenPointFromPagePoint(corners[1], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeEast, ScreenPointFromPagePoint(rightMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthEast, ScreenPointFromPagePoint(corners[2], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouth, ScreenPointFromPagePoint(bottomMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthWest, ScreenPointFromPagePoint(corners[3], pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeWest, ScreenPointFromPagePoint(leftMid, pageView.rect, pageView.scale) },
        { ImageTransformHandle::Rotate, ScreenPointFromPagePoint(rotateHandle, pageView.rect, pageView.scale) },
    };
    constexpr float kHandleRadiusPixels = 11.0f;
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (kHandleRadiusPixels * kHandleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = singleImage->first;
            }
            if (imageIndex != nullptr) {
                *imageIndex = singleImage->second;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

std::optional<App::ImageTransformHandle> App::HitTestSelectedStrokeHandle(POINT cursor, std::size_t* pageIndex) const {
    const auto strokePage = SingleSelectedStrokePage();
    if (!strokePage.has_value() || window_ == nullptr || *strokePage >= selectedStrokeIndices_.size() || selectedStrokeIndices_[*strokePage].empty()) {
        return std::nullopt;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *strokePage);
    if (!pageView.valid) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(*strokePage);
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (std::size_t strokeIndex : selectedStrokeIndices_[*strokePage]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    const Vec2 center { (minX + maxX) * 0.5f, (minY + maxY) * 0.5f };
    const struct {
        ImageTransformHandle handle;
        Vec2 point;
    } handlePoints[] {
        { ImageTransformHandle::ResizeNorthWest, ScreenPointFromPagePoint({ minX, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorth, ScreenPointFromPagePoint({ center.x, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeNorthEast, ScreenPointFromPagePoint({ maxX, minY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeEast, ScreenPointFromPagePoint({ maxX, center.y }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthEast, ScreenPointFromPagePoint({ maxX, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouth, ScreenPointFromPagePoint({ center.x, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeSouthWest, ScreenPointFromPagePoint({ minX, maxY }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::ResizeWest, ScreenPointFromPagePoint({ minX, center.y }, pageView.rect, pageView.scale) },
        { ImageTransformHandle::Rotate, ScreenPointFromPagePoint({ center.x, minY - (18.0f / pageView.scale) }, pageView.rect, pageView.scale) },
    };
    constexpr float kHandleRadiusPixels = 11.0f;
    const Vec2 cursorPoint { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };
    for (const auto& handlePoint : handlePoints) {
        if (DistanceSquared(handlePoint.point, cursorPoint) <= (kHandleRadiusPixels * kHandleRadiusPixels)) {
            if (pageIndex != nullptr) {
                *pageIndex = *strokePage;
            }
            return handlePoint.handle;
        }
    }

    return std::nullopt;
}

void App::TranslateSelectedContent(std::size_t pageIndex, Vec2 delta) {
    if (pageIndex >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(pageIndex);
    if (pageIndex >= selectedStrokeIndices_.size() || pageIndex >= selectedImageIndices_.size()) {
        return;
    }

    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool hasSelection = false;

    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
            hasSelection = true;
        }
    }

    for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        const PageImage& image = page.images[imageIndex];
        const auto corners = ImageCorners(image);
        for (const Vec2& corner : corners) {
            minX = std::min(minX, corner.x);
            minY = std::min(minY, corner.y);
            maxX = std::max(maxX, corner.x);
            maxY = std::max(maxY, corner.y);
        }
        hasSelection = true;
    }

    if (!hasSelection) {
        return;
    }

    delta.x = std::clamp(delta.x, -minX, page.width - maxX);
    delta.y = std::clamp(delta.y, -minY, page.height - maxY);
    if (std::abs(delta.x) < 0.001f && std::abs(delta.y) < 0.001f) {
        return;
    }

    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        for (Vec2& point : page.strokes[strokeIndex].points) {
            point.x += delta.x;
            point.y += delta.y;
        }
    }

    for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        page.images[imageIndex].x += delta.x;
        page.images[imageIndex].y += delta.y;
    }

    InvalidatePageSurfaceCache(pageIndex);
    InvalidateCanvas();
}

void App::Undo() {
    if (isDrawing_ || isPanning_ || !CanUndo()) {
        return;
    }

    HistoryAction action = undoActions_.back();
    undoActions_.pop_back();
    auto& strokes = document_.PageAt(action.pageIndex).strokes;
    document_.SetCurrentPageIndex(action.pageIndex);
    bool cacheUpdated = false;

    if (action.type == HistoryActionType::AddStroke) {
        if (action.index < strokes.size()) {
            strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
            AdjustSelectionForRemoval(action.pageIndex, action.index);
        }
    } else if (action.index <= strokes.size()) {
        AdjustSelectionForInsertion(action.pageIndex, action.index);
        strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
        if (action.index + 1U == strokes.size()) {
            cacheUpdated = TryAppendStrokeToPageSurfaceCache(action.pageIndex, action.stroke);
        }
    }

    redoActions_.push_back(std::move(action));
    if (!cacheUpdated) {
        InvalidatePageSurfaceCache(redoActions_.back().pageIndex);
    }
    InvalidateStrokeRegion(redoActions_.back().stroke, redoActions_.back().pageIndex, false);
}

void App::Redo() {
    if (isDrawing_ || isPanning_ || !CanRedo()) {
        return;
    }

    HistoryAction action = redoActions_.back();
    redoActions_.pop_back();
    auto& strokes = document_.PageAt(action.pageIndex).strokes;
    document_.SetCurrentPageIndex(action.pageIndex);
    bool cacheUpdated = false;

    if (action.type == HistoryActionType::AddStroke) {
        if (action.index <= strokes.size()) {
            AdjustSelectionForInsertion(action.pageIndex, action.index);
            strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
            if (action.index + 1U == strokes.size()) {
                cacheUpdated = TryAppendStrokeToPageSurfaceCache(action.pageIndex, action.stroke);
            }
        }
    } else if (action.index < strokes.size()) {
        strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
        AdjustSelectionForRemoval(action.pageIndex, action.index);
    }

    undoActions_.push_back(std::move(action));
    if (!cacheUpdated) {
        InvalidatePageSurfaceCache(undoActions_.back().pageIndex);
    }
    InvalidateStrokeRegion(undoActions_.back().stroke, undoActions_.back().pageIndex, false);
}

bool App::CanUndo() const {
    return !undoActions_.empty();
}

bool App::CanRedo() const {
    return !redoActions_.empty();
}

bool App::CanGoToPreviousPage() const {
    return document_.CurrentPageIndex() > 0U;
}

bool App::CanGoToNextPage() const {
    return document_.CurrentPageIndex() + 1U < document_.PageCount();
}

void App::AddPaletteColor() {
    if (palette_.size() >= 12U) {
        return;
    }

    const std::uint32_t sourceColor = palette_.at(settingsPaletteIndex_);
    palette_.push_back(sourceColor);
    settingsPaletteIndex_ = palette_.size() - 1U;
    colorIndex_ = std::min(colorIndex_, palette_.size() - 1U);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::RemovePaletteColor() {
    if (palette_.size() <= 1U || settingsPaletteIndex_ >= palette_.size()) {
        return;
    }

    palette_.erase(palette_.begin() + static_cast<std::ptrdiff_t>(settingsPaletteIndex_));
    if (settingsPaletteIndex_ >= palette_.size()) {
        settingsPaletteIndex_ = palette_.size() - 1U;
    }
    if (colorIndex_ >= palette_.size()) {
        colorIndex_ = palette_.size() - 1U;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::ApplyHexColorFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    bool applied = false;
    HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
    if (clipboardData != nullptr) {
        const auto* rawText = static_cast<const wchar_t*>(GlobalLock(clipboardData));
        if (rawText != nullptr) {
            if (const auto parsed = ParseHexColor(rawText); parsed.has_value() && settingsPaletteIndex_ < palette_.size()) {
                palette_[settingsPaletteIndex_] = *parsed;
                applied = true;
            }
            GlobalUnlock(clipboardData);
        }
    }

    CloseClipboard();
    if (applied) {
        InvalidateRect(window_, nullptr, FALSE);
    }
    return applied;
}

std::wstring App::CurrentWindowTitle() const {
    if (!document_.FilePath().empty()) {
        return BaseName(document_.FilePath());
    }

    if (!document_.SourcePath().empty()) {
        return BaseName(document_.SourcePath());
    }

    return L"Untitled";
}

std::wstring App::PageIndicatorLabel() const {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%zu / %zu", document_.CurrentPageIndex() + 1U, document_.PageCount());
    return buffer;
}

void App::UpdateWindowTitle() {
    if (window_ != nullptr) {
        const std::wstring title = CurrentWindowTitle();
        SetWindowTextW(window_, title.c_str());
    }
}

void App::BeginStroke(POINT cursor) {
    if (isPanning_ || isErasing_) {
        return;
    }

    CancelDeferredRasterRefresh(false);
    std::size_t pageIndex = 0;
    const Vec2 pagePoint = ScreenToPage(cursor, &pageIndex);
    if (!HitTestPage(cursor).has_value()) {
        return;
    }

    document_.SetCurrentPageIndex(pageIndex);
    activeStroke_ = Stroke {};
    activeStrokePageIndex_ = pageIndex;
    activeStroke_->style.color = palette_.at(colorIndex_);
    activeStroke_->style.width = brushWidth_;
    activeStroke_->points.push_back(pagePoint);
    isDrawing_ = true;
    UpdateCaptureState();
    InvalidateStrokeRegion(*activeStroke_, pageIndex, true);
}

void App::ExtendStroke(POINT cursor) {
    if (!isDrawing_ || isPanning_ || !activeStroke_.has_value()) {
        return;
    }

    if (!activeStrokePageIndex_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *activeStrokePageIndex_);
    const Page& page = document_.PageAt(*activeStrokePageIndex_);
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return;
    }

    const float pageX = (static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale;
    const float pageY = (static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale;
    const Vec2 rawPoint {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
    Vec2 filteredPoint = rawPoint;

    if (!activeStroke_->points.empty()) {
        filteredPoint = Lerp(activeStroke_->points.back(), rawPoint, inputSmoothing_);
        if (DistanceSquared(filteredPoint, activeStroke_->points.back()) < inputSpacing_) {
            return;
        }
    }

    activeStroke_->points.push_back(filteredPoint);
    InvalidateStrokeRegion(*activeStroke_, *activeStrokePageIndex_, true);
}

void App::EndStroke() {
    std::optional<Stroke> committedStroke;
    std::optional<std::size_t> committedPageIndex;
    if (isDrawing_ && activeStroke_.has_value() && activeStrokePageIndex_.has_value() && !activeStroke_->points.empty()) {
        Stroke finishedStroke = std::move(*activeStroke_);
        finishedStroke.points = BuildSmoothedPoints(finishedStroke.points, strokeSmoothing_);
        auto& strokes = document_.PageAt(*activeStrokePageIndex_).strokes;
        const std::size_t insertIndex = strokes.size();
        strokes.push_back(finishedStroke);
        undoActions_.push_back({ HistoryActionType::AddStroke, std::move(finishedStroke), insertIndex, *activeStrokePageIndex_ });
        redoActions_.clear();
        committedStroke = undoActions_.back().stroke;
        committedPageIndex = *activeStrokePageIndex_;
        if (!TryAppendStrokeToPageSurfaceCache(*committedPageIndex, *committedStroke)) {
            InvalidatePageSurfaceCache(*committedPageIndex);
        }
    }

    activeStroke_.reset();
    activeStrokePageIndex_.reset();
    isDrawing_ = false;
    UpdateCaptureState();
    if (rasterRefreshPending_) {
        ScheduleDeferredRasterRefresh();
    }
    if (committedStroke.has_value() && committedPageIndex.has_value()) {
        InvalidateStrokeRegion(*committedStroke, *committedPageIndex, false);
    } else {
        InvalidateCanvas();
    }
}

void App::BeginSelection(POINT cursor) {
    if (isPanning_) {
        return;
    }

    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        ClearSelection();
        InvalidateCanvas();
        return;
    }

    const Page& page = document_.PageAt(hitPage->pageIndex);
    const Vec2 pagePoint = ScreenToPage(cursor, nullptr);
    selectionPageIndex_ = hitPage->pageIndex;
    selectionAnchorPage_ = {
        std::clamp(pagePoint.x, 0.0f, page.width),
        std::clamp(pagePoint.y, 0.0f, page.height),
    };
    selectionCurrentPage_ = selectionAnchorPage_;
    selectionLassoPoints_.clear();
    selectionLassoPoints_.push_back(selectionAnchorPage_);
    isSelecting_ = true;
    document_.SetCurrentPageIndex(hitPage->pageIndex);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::ExtendSelection(POINT cursor) {
    if (!isSelecting_ || !selectionPageIndex_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *selectionPageIndex_);
    if (!pageView.valid) {
        return;
    }

    const Page& page = document_.PageAt(*selectionPageIndex_);
    const float pageX = (static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale;
    const float pageY = (static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale;
    selectionCurrentPage_ = {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };

    if (selectionMode_ == SelectionMode::Lasso) {
        if (selectionLassoPoints_.empty()
            || DistanceSquared(selectionCurrentPage_, selectionLassoPoints_.back()) >= std::max(2.0f, 6.0f / pageView.scale)) {
            selectionLassoPoints_.push_back(selectionCurrentPage_);
        }
    }

    InvalidateCanvas();
}

void App::EndSelection() {
    if (!isSelecting_) {
        return;
    }

    std::vector<std::size_t> selected;
    std::vector<std::size_t> selectedImages;
    if (selectionPageIndex_.has_value()) {
        const std::size_t pageIndex = *selectionPageIndex_;
        const Page& page = document_.PageAt(pageIndex);
        const bool clickSelection = DistanceSquared(selectionAnchorPage_, selectionCurrentPage_) <= 18.0f;
        const SelectionMode effectiveSelectionMode = selectionBoxOverride_ ? SelectionMode::Box : selectionMode_;

        for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
            const Stroke& stroke = page.strokes[strokeIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? StrokeHitsPoint(stroke, selectionCurrentPage_, 10.0f)
                    : StrokeIntersectsRectSelection(stroke, selectionAnchorPage_, selectionCurrentPage_);
            } else {
                hit = (selectionLassoPoints_.size() < 3U)
                    ? StrokeHitsPoint(stroke, selectionCurrentPage_, 10.0f)
                    : StrokeIntersectsPolygonSelection(stroke, selectionLassoPoints_);
            }

            if (hit) {
                selected.push_back(strokeIndex);
            }
        }

        for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
            const PageImage& image = page.images[imageIndex];
            bool hit = false;
            if (effectiveSelectionMode == SelectionMode::Box) {
                hit = clickSelection
                    ? ImageHitsPoint(image, selectionCurrentPage_)
                    : ImageIntersectsRectSelection(image, selectionAnchorPage_, selectionCurrentPage_);
            } else {
                hit = (selectionLassoPoints_.size() < 3U)
                    ? ImageHitsPoint(image, selectionCurrentPage_)
                    : ImageIntersectsPolygonSelection(image, selectionLassoPoints_);
            }
            if (hit) {
                selectedImages.push_back(imageIndex);
            }
        }

        ClearSelection();
        if (pageIndex < selectedStrokeIndices_.size()) {
            selectedStrokeIndices_[pageIndex] = std::move(selected);
        }
        if (pageIndex < selectedImageIndices_.size()) {
            selectedImageIndices_[pageIndex] = std::move(selectedImages);
        }
    } else {
        ClearSelection();
    }

    isSelecting_ = false;
    selectionPageIndex_.reset();
    selectionLassoPoints_.clear();
    selectionBoxOverride_ = false;
    UpdateCaptureState();
    if (HasSelection()) {
        SetActiveTool(Tool::Move);
    } else {
        InvalidateCanvas();
    }
}

void App::BeginImageTransform(std::size_t pageIndex, std::size_t imageIndex, ImageTransformHandle handle, POINT cursor) {
    if (pageIndex >= document_.PageCount() || imageIndex >= document_.PageAt(pageIndex).images.size()) {
        return;
    }

    CancelDeferredRasterRefresh(false);
    isTransformingImage_ = true;
    transformingImagePageIndex_ = pageIndex;
    transformingImageIndex_ = imageIndex;
    imageTransformHandle_ = handle;
    imageTransformInitial_ = document_.PageAt(pageIndex).images[imageIndex];
    imageTransformStartPagePoint_ = ScreenToPage(cursor, nullptr);
    const Vec2 center = ImageCenter(imageTransformInitial_);
    imageTransformStartAngleDegrees_ = std::atan2(
        imageTransformStartPagePoint_.y - center.y,
        imageTransformStartPagePoint_.x - center.x) * (180.0f / kPi);
    UpdateCaptureState();
}

void App::BeginStrokeTransform(std::size_t pageIndex, ImageTransformHandle handle, POINT cursor) {
    if (pageIndex >= document_.PageCount() || pageIndex >= selectedStrokeIndices_.size() || selectedStrokeIndices_[pageIndex].empty()) {
        return;
    }

    CancelDeferredRasterRefresh(false);
    isTransformingStroke_ = true;
    transformingStrokePageIndex_ = pageIndex;
    strokeTransformHandle_ = handle;
    strokeTransformStartPagePoint_ = ScreenToPage(cursor, nullptr);
    strokeTransformInitials_.clear();

    const Page& page = document_.PageAt(pageIndex);
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        strokeTransformInitials_.push_back({ strokeIndex, page.strokes[strokeIndex] });
        for (const Vec2& point : page.strokes[strokeIndex].points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    strokeTransformBoundsMin_ = { minX, minY };
    strokeTransformBoundsMax_ = { maxX, maxY };
    const Vec2 center {
        (minX + maxX) * 0.5f,
        (minY + maxY) * 0.5f,
    };
    strokeTransformStartAngleDegrees_ = std::atan2(
        strokeTransformStartPagePoint_.y - center.y,
        strokeTransformStartPagePoint_.x - center.x) * (180.0f / kPi);
    UpdateCaptureState();
}

void App::BeginMoveSelection(POINT cursor) {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        if (HasSelection()) {
            ClearSelection();
            InvalidateCanvas();
        }
        return;
    }

    const std::size_t pageIndex = hitPage->pageIndex;
    const Vec2 pagePoint = ScreenToPage(cursor, nullptr);
    const float hitRadius = std::max(8.0f, brushWidth_ * 2.5f) / std::max(0.1f, hitPage->scale);
    const Page& page = document_.PageAt(pageIndex);
    std::size_t strokeHandlePageIndex = 0;
    const auto strokeHandle = HitTestSelectedStrokeHandle(cursor, &strokeHandlePageIndex);
    if (strokeHandle.has_value() && strokeHandlePageIndex == pageIndex) {
        BeginStrokeTransform(pageIndex, *strokeHandle, cursor);
        return;
    }

    std::size_t imageHandlePageIndex = 0;
    std::size_t imageHandleImageIndex = 0;
    const auto imageHandle = HitTestSelectedImageHandle(cursor, &imageHandlePageIndex, &imageHandleImageIndex);
    if (imageHandle.has_value() && imageHandlePageIndex == pageIndex) {
        BeginImageTransform(pageIndex, imageHandleImageIndex, *imageHandle, cursor);
        return;
    }

    std::optional<std::size_t> clickedSelectedStroke {};
    std::optional<std::size_t> clickedSelectedImage {};
    std::optional<std::size_t> clickedStroke {};
    std::optional<std::size_t> clickedImage {};

    if (pageIndex < selectedStrokeIndices_.size()) {
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size() && StrokeHitsPoint(page.strokes[strokeIndex], pagePoint, hitRadius)) {
                clickedSelectedStroke = strokeIndex;
                break;
            }
        }
    }
    if (!clickedSelectedStroke.has_value() && pageIndex < selectedImageIndices_.size()) {
        for (std::size_t imageIndex : selectedImageIndices_[pageIndex]) {
            if (imageIndex < page.images.size() && ImageHitsPoint(page.images[imageIndex], pagePoint)) {
                clickedSelectedImage = imageIndex;
                break;
            }
        }
    }

    if (!clickedSelectedStroke.has_value() && !clickedSelectedImage.has_value()) {
        for (std::size_t strokeIndex = page.strokes.size(); strokeIndex > 0; --strokeIndex) {
            const std::size_t actualIndex = strokeIndex - 1U;
            if (StrokeHitsPoint(page.strokes[actualIndex], pagePoint, hitRadius)) {
                clickedStroke = actualIndex;
                break;
            }
        }
    }
    if (!clickedSelectedStroke.has_value() && !clickedSelectedImage.has_value() && !clickedStroke.has_value()) {
        for (std::size_t imageIndex = page.images.size(); imageIndex > 0; --imageIndex) {
            const std::size_t actualIndex = imageIndex - 1U;
            if (ImageHitsPoint(page.images[actualIndex], pagePoint)) {
                clickedImage = actualIndex;
                break;
            }
        }
    }

    if (!clickedSelectedStroke.has_value() && !clickedSelectedImage.has_value()
        && !clickedStroke.has_value() && !clickedImage.has_value()) {
        ClearSelection();
        selectionBoxOverride_ = true;
        BeginSelection(cursor);
        return;
    }

    if (!clickedSelectedStroke.has_value() && !clickedSelectedImage.has_value()) {
        ClearSelection();
        if (clickedStroke.has_value() && pageIndex < selectedStrokeIndices_.size()) {
            selectedStrokeIndices_[pageIndex].push_back(*clickedStroke);
        }
        if (clickedImage.has_value() && pageIndex < selectedImageIndices_.size()) {
            selectedImageIndices_[pageIndex].push_back(*clickedImage);
        }
    }

    CancelDeferredRasterRefresh(false);
    isMovingSelection_ = true;
    moveSelectionPageIndex_ = pageIndex;
    lastMovePagePoint_ = pagePoint;
    document_.SetCurrentPageIndex(pageIndex);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::UpdateImageTransform(POINT cursor) {
    if (!isTransformingImage_ || !transformingImagePageIndex_.has_value() || !transformingImageIndex_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *transformingImagePageIndex_);
    if (!pageView.valid) {
        return;
    }

    Page& page = document_.PageAt(*transformingImagePageIndex_);
    if (*transformingImageIndex_ >= page.images.size()) {
        return;
    }

    const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const Vec2 currentPoint {
        std::clamp((static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale, 0.0f, page.width),
        std::clamp((static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale, 0.0f, page.height),
    };
    PageImage updated = imageTransformInitial_;
    const Vec2 center = ImageCenter(imageTransformInitial_);

    if (imageTransformHandle_ == ImageTransformHandle::Rotate) {
        const float currentAngle = std::atan2(currentPoint.y - center.y, currentPoint.x - center.x) * (180.0f / kPi);
        updated.rotationDegrees = imageTransformInitial_.rotationDegrees + (currentAngle - imageTransformStartAngleDegrees_);
        if (shiftPressed) {
            updated.rotationDegrees = std::round(updated.rotationDegrees / 15.0f) * 15.0f;
        }
    } else {
        const Vec2 localPoint = ToImageLocalPoint(imageTransformInitial_, currentPoint);
        float left = -imageTransformInitial_.width * 0.5f;
        float right = imageTransformInitial_.width * 0.5f;
        float top = -imageTransformInitial_.height * 0.5f;
        float bottom = imageTransformInitial_.height * 0.5f;
        const float localX = localPoint.x - center.x;
        const float localY = localPoint.y - center.y;
        constexpr float kMinImageSize = 18.0f;

        switch (imageTransformHandle_) {
        case ImageTransformHandle::ResizeNorthWest:
            left = std::min(localX, right - kMinImageSize);
            top = std::min(localY, bottom - kMinImageSize);
            break;
        case ImageTransformHandle::ResizeNorth:
            top = std::min(localY, bottom - kMinImageSize);
            break;
        case ImageTransformHandle::ResizeNorthEast:
            right = std::max(localX, left + kMinImageSize);
            top = std::min(localY, bottom - kMinImageSize);
            break;
        case ImageTransformHandle::ResizeEast:
            right = std::max(localX, left + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeSouthEast:
            right = std::max(localX, left + kMinImageSize);
            bottom = std::max(localY, top + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeSouth:
            bottom = std::max(localY, top + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeSouthWest:
            left = std::min(localX, right - kMinImageSize);
            bottom = std::max(localY, top + kMinImageSize);
            break;
        case ImageTransformHandle::ResizeWest:
            left = std::min(localX, right - kMinImageSize);
            break;
        default:
            break;
        }

        if (shiftPressed) {
            const float initialWidth = std::max(kMinImageSize, imageTransformInitial_.width);
            const float initialHeight = std::max(kMinImageSize, imageTransformInitial_.height);
            const float aspect = initialWidth / initialHeight;
            switch (imageTransformHandle_) {
            case ImageTransformHandle::ResizeNorthWest:
            case ImageTransformHandle::ResizeNorthEast:
            case ImageTransformHandle::ResizeSouthEast:
            case ImageTransformHandle::ResizeSouthWest: {
                const float widthScale = std::abs(right - left) / initialWidth;
                const float heightScale = std::abs(bottom - top) / initialHeight;
                const float scale = std::max(widthScale, heightScale);
                const float newWidth = std::max(kMinImageSize, initialWidth * scale);
                const float newHeight = std::max(kMinImageSize, initialHeight * scale);
                if (imageTransformHandle_ == ImageTransformHandle::ResizeNorthWest) {
                    left = right - newWidth;
                    top = bottom - newHeight;
                } else if (imageTransformHandle_ == ImageTransformHandle::ResizeNorthEast) {
                    right = left + newWidth;
                    top = bottom - newHeight;
                } else if (imageTransformHandle_ == ImageTransformHandle::ResizeSouthEast) {
                    right = left + newWidth;
                    bottom = top + newHeight;
                } else {
                    left = right - newWidth;
                    bottom = top + newHeight;
                }
                break;
            }
            case ImageTransformHandle::ResizeEast:
            case ImageTransformHandle::ResizeWest: {
                const float newWidth = std::max(kMinImageSize, std::abs(right - left));
                const float newHeight = std::max(kMinImageSize, newWidth / aspect);
                if (imageTransformHandle_ == ImageTransformHandle::ResizeEast) {
                    right = left + newWidth;
                } else {
                    left = right - newWidth;
                }
                top = -newHeight * 0.5f;
                bottom = newHeight * 0.5f;
                break;
            }
            case ImageTransformHandle::ResizeNorth:
            case ImageTransformHandle::ResizeSouth: {
                const float newHeight = std::max(kMinImageSize, std::abs(bottom - top));
                const float newWidth = std::max(kMinImageSize, newHeight * aspect);
                if (imageTransformHandle_ == ImageTransformHandle::ResizeNorth) {
                    top = bottom - newHeight;
                } else {
                    bottom = top + newHeight;
                }
                left = -newWidth * 0.5f;
                right = newWidth * 0.5f;
                break;
            }
            default:
                break;
            }
        }

        const Vec2 localCenterOffset {
            (left + right) * 0.5f,
            (top + bottom) * 0.5f,
        };
        const Vec2 newCenter = RotatePointAround(
            { center.x + localCenterOffset.x, center.y + localCenterOffset.y },
            center,
            imageTransformInitial_.rotationDegrees);
        updated.width = std::max(kMinImageSize, right - left);
        updated.height = std::max(kMinImageSize, bottom - top);
        updated.x = newCenter.x - (updated.width * 0.5f);
        updated.y = newCenter.y - (updated.height * 0.5f);
    }

    page.images[*transformingImageIndex_] = updated;
    InvalidatePageSurfaceCache(*transformingImagePageIndex_);
    InvalidateCanvas();
}

void App::UpdateStrokeTransform(POINT cursor) {
    if (!isTransformingStroke_ || !transformingStrokePageIndex_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *transformingStrokePageIndex_);
    if (!pageView.valid) {
        return;
    }

    Page& page = document_.PageAt(*transformingStrokePageIndex_);
    const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const Vec2 currentPoint {
        std::clamp((static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale, 0.0f, page.width),
        std::clamp((static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale, 0.0f, page.height),
    };
    const Vec2 center {
        (strokeTransformBoundsMin_.x + strokeTransformBoundsMax_.x) * 0.5f,
        (strokeTransformBoundsMin_.y + strokeTransformBoundsMax_.y) * 0.5f,
    };

    if (strokeTransformHandle_ == ImageTransformHandle::Rotate) {
        float angleDelta = (std::atan2(currentPoint.y - center.y, currentPoint.x - center.x) * (180.0f / kPi)) - strokeTransformStartAngleDegrees_;
        if (shiftPressed) {
            angleDelta = std::round(angleDelta / 15.0f) * 15.0f;
        }
        for (const auto& [strokeIndex, originalStroke] : strokeTransformInitials_) {
            if (strokeIndex >= page.strokes.size()) {
                continue;
            }
            Stroke updated = originalStroke;
            for (Vec2& point : updated.points) {
                point = RotatePointAround(point, center, angleDelta);
            }
            page.strokes[strokeIndex] = std::move(updated);
        }
    } else {
        float left = strokeTransformBoundsMin_.x - center.x;
        float right = strokeTransformBoundsMax_.x - center.x;
        float top = strokeTransformBoundsMin_.y - center.y;
        float bottom = strokeTransformBoundsMax_.y - center.y;
        const float localX = currentPoint.x - center.x;
        const float localY = currentPoint.y - center.y;
        constexpr float kMinStrokeSize = 12.0f;

        switch (strokeTransformHandle_) {
        case ImageTransformHandle::ResizeNorthWest:
            left = std::min(localX, right - kMinStrokeSize);
            top = std::min(localY, bottom - kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeNorth:
            top = std::min(localY, bottom - kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeNorthEast:
            right = std::max(localX, left + kMinStrokeSize);
            top = std::min(localY, bottom - kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeEast:
            right = std::max(localX, left + kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeSouthEast:
            right = std::max(localX, left + kMinStrokeSize);
            bottom = std::max(localY, top + kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeSouth:
            bottom = std::max(localY, top + kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeSouthWest:
            left = std::min(localX, right - kMinStrokeSize);
            bottom = std::max(localY, top + kMinStrokeSize);
            break;
        case ImageTransformHandle::ResizeWest:
            left = std::min(localX, right - kMinStrokeSize);
            break;
        default:
            break;
        }

        const float initialWidth = std::max(kMinStrokeSize, strokeTransformBoundsMax_.x - strokeTransformBoundsMin_.x);
        const float initialHeight = std::max(kMinStrokeSize, strokeTransformBoundsMax_.y - strokeTransformBoundsMin_.y);
        if (shiftPressed) {
            const float aspect = initialWidth / initialHeight;
            switch (strokeTransformHandle_) {
            case ImageTransformHandle::ResizeNorthWest:
            case ImageTransformHandle::ResizeNorthEast:
            case ImageTransformHandle::ResizeSouthEast:
            case ImageTransformHandle::ResizeSouthWest: {
                const float widthScale = std::abs(right - left) / initialWidth;
                const float heightScale = std::abs(bottom - top) / initialHeight;
                const float scale = std::max(widthScale, heightScale);
                const float newWidth = std::max(kMinStrokeSize, initialWidth * scale);
                const float newHeight = std::max(kMinStrokeSize, initialHeight * scale);
                if (strokeTransformHandle_ == ImageTransformHandle::ResizeNorthWest) {
                    left = right - newWidth;
                    top = bottom - newHeight;
                } else if (strokeTransformHandle_ == ImageTransformHandle::ResizeNorthEast) {
                    right = left + newWidth;
                    top = bottom - newHeight;
                } else if (strokeTransformHandle_ == ImageTransformHandle::ResizeSouthEast) {
                    right = left + newWidth;
                    bottom = top + newHeight;
                } else {
                    left = right - newWidth;
                    bottom = top + newHeight;
                }
                break;
            }
            case ImageTransformHandle::ResizeEast:
            case ImageTransformHandle::ResizeWest: {
                const float newWidth = std::max(kMinStrokeSize, std::abs(right - left));
                const float newHeight = std::max(kMinStrokeSize, newWidth / aspect);
                if (strokeTransformHandle_ == ImageTransformHandle::ResizeEast) {
                    right = left + newWidth;
                } else {
                    left = right - newWidth;
                }
                top = -newHeight * 0.5f;
                bottom = newHeight * 0.5f;
                break;
            }
            case ImageTransformHandle::ResizeNorth:
            case ImageTransformHandle::ResizeSouth: {
                const float newHeight = std::max(kMinStrokeSize, std::abs(bottom - top));
                const float newWidth = std::max(kMinStrokeSize, newHeight * aspect);
                if (strokeTransformHandle_ == ImageTransformHandle::ResizeNorth) {
                    top = bottom - newHeight;
                } else {
                    bottom = top + newHeight;
                }
                left = -newWidth * 0.5f;
                right = newWidth * 0.5f;
                break;
            }
            default:
                break;
            }
        }

        const Vec2 localCenterOffset { (left + right) * 0.5f, (top + bottom) * 0.5f };
        const float scaleX = std::max(kMinStrokeSize, right - left) / initialWidth;
        const float scaleY = std::max(kMinStrokeSize, bottom - top) / initialHeight;
        for (const auto& [strokeIndex, originalStroke] : strokeTransformInitials_) {
            if (strokeIndex >= page.strokes.size()) {
                continue;
            }
            Stroke updated = originalStroke;
            for (Vec2& point : updated.points) {
                const float localXPoint = point.x - center.x;
                const float localYPoint = point.y - center.y;
                point.x = center.x + (localXPoint * scaleX) + localCenterOffset.x;
                point.y = center.y + (localYPoint * scaleY) + localCenterOffset.y;
            }
            page.strokes[strokeIndex] = std::move(updated);
        }
    }

    InvalidatePageSurfaceCache(*transformingStrokePageIndex_);
    InvalidateCanvas();
}

void App::EndStrokeTransform() {
    if (!isTransformingStroke_) {
        return;
    }

    isTransformingStroke_ = false;
    transformingStrokePageIndex_.reset();
    strokeTransformHandle_ = ImageTransformHandle::None;
    strokeTransformInitials_.clear();
    UpdateCaptureState();
    if (rasterRefreshPending_) {
        ScheduleDeferredRasterRefresh();
    }
}

void App::UpdateMoveSelection(POINT cursor) {
    if (!isMovingSelection_ || !moveSelectionPageIndex_.has_value()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *moveSelectionPageIndex_);
    if (!pageView.valid) {
        return;
    }

    const Page& page = document_.PageAt(*moveSelectionPageIndex_);
    const Vec2 pagePoint {
        std::clamp((static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale, 0.0f, page.width),
        std::clamp((static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale, 0.0f, page.height),
    };
    const Vec2 delta {
        pagePoint.x - lastMovePagePoint_.x,
        pagePoint.y - lastMovePagePoint_.y,
    };
    TranslateSelectedContent(*moveSelectionPageIndex_, delta);
    lastMovePagePoint_ = pagePoint;
}

void App::EndImageTransform() {
    if (!isTransformingImage_) {
        return;
    }

    isTransformingImage_ = false;
    transformingImagePageIndex_.reset();
    transformingImageIndex_.reset();
    imageTransformHandle_ = ImageTransformHandle::None;
    UpdateCaptureState();
    if (rasterRefreshPending_) {
        ScheduleDeferredRasterRefresh();
    }
}

void App::EndMoveSelection() {
    if (!isMovingSelection_) {
        return;
    }

    isMovingSelection_ = false;
    moveSelectionPageIndex_.reset();
    UpdateCaptureState();
    if (rasterRefreshPending_) {
        ScheduleDeferredRasterRefresh();
    }
}

bool App::CopySelectionToClipboard() {
    std::optional<std::size_t> pageIndex;
    for (std::size_t index = 0; index < document_.PageCount(); ++index) {
        const bool hasStrokes = index < selectedStrokeIndices_.size() && !selectedStrokeIndices_[index].empty();
        const bool hasImages = index < selectedImageIndices_.size() && !selectedImageIndices_[index].empty();
        if (hasStrokes || hasImages) {
            pageIndex = index;
            break;
        }
    }
    if (!pageIndex.has_value()) {
        return false;
    }

    const Page& page = document_.PageAt(*pageIndex);
    std::vector<Stroke> strokes;
    std::vector<PageImage> images;
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool hasContent = false;

    for (std::size_t strokeIndex : selectedStrokeIndices_[*pageIndex]) {
        if (strokeIndex >= page.strokes.size()) {
            continue;
        }
        const Stroke& stroke = page.strokes[strokeIndex];
        strokes.push_back(stroke);
        for (const Vec2& point : stroke.points) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
            hasContent = true;
        }
    }

    for (std::size_t imageIndex : selectedImageIndices_[*pageIndex]) {
        if (imageIndex >= page.images.size()) {
            continue;
        }
        const PageImage& image = page.images[imageIndex];
        images.push_back(image);
        minX = std::min(minX, image.x);
        minY = std::min(minY, image.y);
        maxX = std::max(maxX, image.x + image.width);
        maxY = std::max(maxY, image.y + image.height);
        hasContent = true;
    }

    if (!hasContent || !OpenClipboard(window_)) {
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const auto appendBytes = [&](const void* data, std::size_t size) {
        const auto* source = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), source, source + size);
    };
    const auto appendUint32 = [&](std::uint32_t value) { appendBytes(&value, sizeof(value)); };
    const auto appendFloat = [&](float value) { appendBytes(&value, sizeof(value)); };

    const char magic[] = { 'C', 'T', 'S', '1' };
    appendBytes(magic, sizeof(magic));
    appendUint32(static_cast<std::uint32_t>(strokes.size()));
    appendUint32(static_cast<std::uint32_t>(images.size()));
    appendFloat(minX);
    appendFloat(minY);
    appendFloat(maxX);
    appendFloat(maxY);

    for (const Stroke& stroke : strokes) {
        appendUint32(stroke.style.color);
        appendFloat(stroke.style.width);
        appendUint32(static_cast<std::uint32_t>(stroke.points.size()));
        for (const Vec2& point : stroke.points) {
            appendFloat(point.x);
            appendFloat(point.y);
        }
    }

    for (const PageImage& image : images) {
        appendFloat(image.x);
        appendFloat(image.y);
        appendFloat(image.width);
        appendFloat(image.height);
        appendFloat(image.rotationDegrees);
        appendUint32(static_cast<std::uint32_t>(image.pixelWidth));
        appendUint32(static_cast<std::uint32_t>(image.pixelHeight));
        appendUint32(static_cast<std::uint32_t>(image.stride));
        appendUint32(static_cast<std::uint32_t>(image.pixels.size()));
        if (!image.pixels.empty()) {
            appendBytes(image.pixels.data(), image.pixels.size());
        }
    }

    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }

    void* destination = GlobalLock(memory);
    std::copy(bytes.begin(), bytes.end(), static_cast<std::uint8_t*>(destination));
    GlobalUnlock(memory);
    SetClipboardData(SelectionClipboardFormat(), memory);
    CloseClipboard();
    return true;
}

bool App::PasteSelectionFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    HANDLE clipboardData = GetClipboardData(SelectionClipboardFormat());
    if (clipboardData == nullptr) {
        CloseClipboard();
        return false;
    }

    const auto* data = static_cast<const std::uint8_t*>(GlobalLock(clipboardData));
    if (data == nullptr) {
        CloseClipboard();
        return false;
    }

    SIZE_T size = GlobalSize(clipboardData);
    const auto readExact = [&](std::size_t offset, void* output, std::size_t length) -> bool {
        if (offset + length > static_cast<std::size_t>(size)) {
            return false;
        }
        std::memcpy(output, data + offset, length);
        return true;
    };

    std::size_t offset = 0;
    char magic[4] {};
    if (!readExact(offset, magic, sizeof(magic)) || std::memcmp(magic, "CTS1", 4) != 0) {
        GlobalUnlock(clipboardData);
        CloseClipboard();
        return false;
    }
    offset += sizeof(magic);

    auto readUint32 = [&](std::uint32_t& value) -> bool {
        const bool ok = readExact(offset, &value, sizeof(value));
        offset += ok ? sizeof(value) : 0U;
        return ok;
    };
    auto readFloat = [&](float& value) -> bool {
        const bool ok = readExact(offset, &value, sizeof(value));
        offset += ok ? sizeof(value) : 0U;
        return ok;
    };

    std::uint32_t strokeCount = 0;
    std::uint32_t imageCount = 0;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    const bool headerOk = readUint32(strokeCount) && readUint32(imageCount)
        && readFloat(minX) && readFloat(minY) && readFloat(maxX) && readFloat(maxY);
    if (!headerOk) {
        GlobalUnlock(clipboardData);
        CloseClipboard();
        return false;
    }

    std::vector<Stroke> strokes;
    std::vector<PageImage> images;
    strokes.reserve(strokeCount);
    images.reserve(imageCount);

    bool ok = true;
    for (std::uint32_t strokeIndex = 0; ok && strokeIndex < strokeCount; ++strokeIndex) {
        Stroke stroke {};
        std::uint32_t pointCount = 0;
        ok = readUint32(stroke.style.color) && readFloat(stroke.style.width) && readUint32(pointCount);
        stroke.points.reserve(pointCount);
        for (std::uint32_t pointIndex = 0; ok && pointIndex < pointCount; ++pointIndex) {
            Vec2 point {};
            ok = readFloat(point.x) && readFloat(point.y);
            if (ok) {
                stroke.points.push_back(point);
            }
        }
        if (ok) {
            strokes.push_back(std::move(stroke));
        }
    }

    for (std::uint32_t imageIndex = 0; ok && imageIndex < imageCount; ++imageIndex) {
        PageImage image {};
        std::uint32_t pixelWidth = 0;
        std::uint32_t pixelHeight = 0;
        std::uint32_t stride = 0;
        std::uint32_t dataSize = 0;
        ok = readFloat(image.x) && readFloat(image.y) && readFloat(image.width) && readFloat(image.height)
            && readFloat(image.rotationDegrees)
            && readUint32(pixelWidth) && readUint32(pixelHeight) && readUint32(stride) && readUint32(dataSize);
        image.pixelWidth = static_cast<int>(pixelWidth);
        image.pixelHeight = static_cast<int>(pixelHeight);
        image.stride = static_cast<int>(stride);
        if (ok) {
            image.pixels.resize(dataSize);
            ok = readExact(offset, image.pixels.data(), dataSize);
            offset += ok ? dataSize : 0U;
        }
        if (ok) {
            images.push_back(std::move(image));
        }
    }

    GlobalUnlock(clipboardData);
    CloseClipboard();
    if (!ok) {
        return false;
    }

    const std::size_t targetPageIndex = document_.CurrentPageIndex();
    if (targetPageIndex >= document_.PageCount()) {
        return false;
    }

    const auto pasteCenter = DefaultPasteOrigin(targetPageIndex);
    if (!pasteCenter.has_value()) {
        return false;
    }

    const Vec2 pasteTopLeft {
        pasteCenter->x - ((maxX - minX) * 0.5f),
        pasteCenter->y - ((maxY - minY) * 0.5f),
    };
    const Vec2 delta {
        pasteTopLeft.x - minX,
        pasteTopLeft.y - minY,
    };

    Page& page = document_.PageAt(targetPageIndex);
    ClearSelection();
    for (Stroke& stroke : strokes) {
        for (Vec2& point : stroke.points) {
            point.x += delta.x;
            point.y += delta.y;
        }
        page.strokes.push_back(stroke);
        selectedStrokeIndices_[targetPageIndex].push_back(page.strokes.size() - 1U);
    }

    for (PageImage& image : images) {
        image.x += delta.x;
        image.y += delta.y;
        page.images.push_back(image);
        selectedImageIndices_[targetPageIndex].push_back(page.images.size() - 1U);
    }

    document_.SetCurrentPageIndex(targetPageIndex);
    InvalidatePageSurfaceCache(targetPageIndex);
    InvalidateCanvas();
    return true;
}

bool App::PasteImageFromClipboard() {
    if (!OpenClipboard(window_)) {
        return false;
    }

    HBITMAP bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (bitmap == nullptr) {
        CloseClipboard();
        return false;
    }

    BITMAP sourceInfo {};
    if (GetObjectW(bitmap, sizeof(sourceInfo), &sourceInfo) == 0 || sourceInfo.bmWidth <= 0 || sourceInfo.bmHeight <= 0) {
        CloseClipboard();
        return false;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = sourceInfo.bmWidth;
    bitmapInfo.bmiHeader.biHeight = -sourceInfo.bmHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    const int stride = sourceInfo.bmWidth * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * static_cast<std::size_t>(sourceInfo.bmHeight));
    HDC screenDc = GetDC(nullptr);
    const int rows = GetDIBits(screenDc, bitmap, 0, static_cast<UINT>(sourceInfo.bmHeight), pixels.data(), &bitmapInfo, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screenDc);
    CloseClipboard();
    if (rows == 0) {
        return false;
    }

    const std::size_t targetPageIndex = document_.CurrentPageIndex();
    if (targetPageIndex >= document_.PageCount()) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, targetPageIndex);
    const Page& page = document_.PageAt(targetPageIndex);
    const float pageScale = std::max(0.1f, pageView.valid ? pageView.scale : 1.0f);

    const float pageWidth = std::min(page.width * 0.75f, static_cast<float>(sourceInfo.bmWidth) / pageScale);
    const float aspect = static_cast<float>(sourceInfo.bmHeight) / std::max(1.0f, static_cast<float>(sourceInfo.bmWidth));
    const float pageHeight = pageWidth * aspect;
    const auto pasteCenter = DefaultPasteOrigin(targetPageIndex);
    if (!pasteCenter.has_value()) {
        return false;
    }

    PageImage image {};
    image.width = pageWidth;
    image.height = pageHeight;
    image.x = std::clamp(pasteCenter->x - (pageWidth * 0.5f), 0.0f, std::max(0.0f, page.width - pageWidth));
    image.y = std::clamp(pasteCenter->y - (pageHeight * 0.5f), 0.0f, std::max(0.0f, page.height - pageHeight));
    image.pixelWidth = sourceInfo.bmWidth;
    image.pixelHeight = sourceInfo.bmHeight;
    image.stride = stride;
    image.pixels = std::move(pixels);

    Page& mutablePage = document_.PageAt(targetPageIndex);
    mutablePage.images.push_back(std::move(image));
    ClearSelection();
    selectedImageIndices_[targetPageIndex].push_back(mutablePage.images.size() - 1U);
    document_.SetCurrentPageIndex(targetPageIndex);
    InvalidatePageSurfaceCache(targetPageIndex);
    InvalidateCanvas();
    return true;
}

void App::BeginErase(POINT cursor) {
    if (isPanning_) {
        return;
    }

    CancelDeferredRasterRefresh(false);
    isErasing_ = true;
    UpdateCaptureState();
    static_cast<void>(EraseAt(cursor));
}

void App::ExtendErase(POINT cursor) {
    if (!isErasing_) {
        return;
    }

    static_cast<void>(EraseAt(cursor));
}

bool App::EraseAt(POINT cursor) {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        return false;
    }

    const PageView pageView = *hitPage;
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return false;
    }

    const Vec2 center = ScreenToPage(cursor, nullptr);
    const float eraseRadiusPage = std::max(10.0f, brushWidth_ * 4.0f) / pageView.scale;
    const float eraseRadiusSquared = eraseRadiusPage * eraseRadiusPage;
    auto& strokes = document_.PageAt(pageView.pageIndex).strokes;
    document_.SetCurrentPageIndex(pageView.pageIndex);

    for (std::size_t strokeIndex = 0; strokeIndex < strokes.size(); ++strokeIndex) {
        const Stroke& stroke = strokes[strokeIndex];
        if (stroke.points.empty()) {
            continue;
        }

        bool hit = DistanceSquared(stroke.points.front(), center) <= eraseRadiusSquared;
        for (std::size_t index = 1; !hit && index < stroke.points.size(); ++index) {
            const Vec2 a = stroke.points[index - 1];
            const Vec2 b = stroke.points[index];
            const float segmentDx = b.x - a.x;
            const float segmentDy = b.y - a.y;
            const float lengthSquared = (segmentDx * segmentDx) + (segmentDy * segmentDy);

            float t = 0.0f;
            if (lengthSquared > 0.0001f) {
                t = ((center.x - a.x) * segmentDx + (center.y - a.y) * segmentDy) / lengthSquared;
                t = std::clamp(t, 0.0f, 1.0f);
            }

            const Vec2 closest {
                a.x + (segmentDx * t),
                a.y + (segmentDy * t),
            };
            hit = DistanceSquared(closest, center) <= eraseRadiusSquared;
        }

        if (hit) {
            Stroke removed = strokes[strokeIndex];
            strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(strokeIndex));
            AdjustSelectionForRemoval(pageView.pageIndex, strokeIndex);
            undoActions_.push_back({ HistoryActionType::RemoveStroke, std::move(removed), strokeIndex, pageView.pageIndex });
            redoActions_.clear();
            InvalidatePageSurfaceCache(pageView.pageIndex);
            InvalidateRect(window_, nullptr, FALSE);
            return true;
        }
    }

    return false;
}

void App::BeginPan(POINT cursor, bool fromMiddleButton) {
    if (isDrawing_) {
        EndStroke();
    }

    CancelDeferredRasterRefresh(false);
    isPanning_ = true;
    panFromMiddleButton_ = fromMiddleButton;
    lastPanCursor_ = cursor;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::UpdatePan(POINT cursor) {
    if (!isPanning_) {
        return;
    }

    const float dx = static_cast<float>(cursor.x - lastPanCursor_.x);
    const float dy = static_cast<float>(cursor.y - lastPanCursor_.y);
    lastPanCursor_ = cursor;
    PanBy(dx, dy);
}

void App::EndPan() {
    isPanning_ = false;
    panFromMiddleButton_ = false;
    UpdateCaptureState();
    if (rasterRefreshPending_) {
        ScheduleDeferredRasterRefresh();
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::UpdateCaptureState() {
    if (isDrawing_ || isPanning_ || isSelecting_ || isMovingSelection_ || isTransformingImage_ || isTransformingStroke_) {
        SetCapture(window_);
        return;
    }

    if (isErasing_) {
        SetCapture(window_);
        return;
    }

    if (GetCapture() == window_) {
        ReleaseCapture();
    }
}

void App::ExecuteCommand(unsigned int commandId) {
    switch (commandId) {
    case kCommandFileNew:
        CreateBlankDocument();
        break;
    case kCommandFileOpen:
        OpenDocument();
        break;
    case kCommandFileSaveCnote:
        static_cast<void>(SaveAsCnote());
        break;
    case kCommandFileExportPdf:
        static_cast<void>(ExportAsPdf());
        break;
    case kCommandFileExit:
        PostMessageW(window_, WM_CLOSE, 0, 0);
        break;
    case kCommandEditUndo:
        Undo();
        break;
    case kCommandEditRedo:
        Redo();
        break;
    case kCommandEditCopy:
        static_cast<void>(CopySelectionToClipboard());
        break;
    case kCommandEditPaste:
        if (!PasteSelectionFromClipboard()) {
            static_cast<void>(PasteImageFromClipboard());
        }
        break;
    case kCommandEditBrushSettings:
        ToggleBrushSettings();
        break;
    case kCommandSelectLasso:
        selectionMode_ = SelectionMode::Lasso;
        SetActiveTool(Tool::Select);
        break;
    case kCommandViewReset:
        CancelDeferredRasterRefresh();
        ResetView();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        break;
    case kCommandViewPreviousPage:
        PreviousPage();
        break;
    case kCommandViewNextPage:
        NextPage();
        break;
    default:
        break;
    }
}

bool App::HandleToolbarClick(POINT cursor) {
    if (cursor.y >= kToolbarHeight) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size());

    const auto openMenu = [&](RECT rect, unsigned int kind) {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) {
            return;
        }

        switch (kind) {
        case 0:
            AppendMenuW(menu, MF_STRING, kCommandFileNew, L"New");
            AppendMenuW(menu, MF_STRING, kCommandFileOpen, L"Open...");
            AppendMenuW(menu, MF_STRING, kCommandFileSaveCnote, L"Save as Cnote...");
            AppendMenuW(menu, MF_STRING, kCommandFileExportPdf, L"Export as PDF...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCommandFileExit, L"Exit");
            break;
        case 1:
            AppendMenuW(menu, MF_STRING | (CanUndo() ? 0 : MF_GRAYED), kCommandEditUndo, L"Undo");
            AppendMenuW(menu, MF_STRING | (CanRedo() ? 0 : MF_GRAYED), kCommandEditRedo, L"Redo");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCommandEditCopy, L"Copy");
            AppendMenuW(menu, MF_STRING, kCommandEditPaste, L"Paste");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kCommandEditBrushSettings, settingsOpen_ ? L"Hide Brush Settings" : L"Brush Settings");
            break;
        case 2:
            AppendMenuW(menu, MF_STRING, kCommandViewReset, L"Reset View");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING | (CanGoToPreviousPage() ? 0 : MF_GRAYED), kCommandViewPreviousPage, L"Previous Page");
            AppendMenuW(menu, MF_STRING | (CanGoToNextPage() ? 0 : MF_GRAYED), kCommandViewNextPage, L"Next Page");
            break;
        case 3:
            AppendMenuW(menu, MF_STRING | (selectionMode_ == SelectionMode::Lasso ? MF_CHECKED : 0), kCommandSelectLasso, L"Lasso Select");
            break;
        default:
            DestroyMenu(menu);
            return;
        }

        POINT anchor { rect.left, rect.bottom + 2 };
        ClientToScreen(window_, &anchor);
        const unsigned int selected = TrackPopupMenu(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            anchor.x,
            anchor.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);

        if (selected != 0U) {
            ExecuteCommand(selected);
        }
    };

    if (PtInRect(&layout.menu.fileRect, cursor) != 0) {
        ClosePaletteOverlays();
        openMenu(layout.menu.fileRect, 0);
        return true;
    }

    if (PtInRect(&layout.menu.editRect, cursor) != 0) {
        ClosePaletteOverlays();
        openMenu(layout.menu.editRect, 1);
        return true;
    }

    if (PtInRect(&layout.menu.viewRect, cursor) != 0) {
        ClosePaletteOverlays();
        openMenu(layout.menu.viewRect, 2);
        return true;
    }

    if (PtInRect(&layout.previousPageRect, cursor) != 0) {
        ClosePaletteOverlays();
        PreviousPage();
        return true;
    }

    if (PtInRect(&layout.nextPageRect, cursor) != 0) {
        ClosePaletteOverlays();
        NextPage();
        return true;
    }

    if (PtInRect(&layout.drawToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Draw);
        return true;
    }

    if (PtInRect(&layout.selectToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        selectionMode_ = SelectionMode::Lasso;
        SetActiveTool(Tool::Select);
        return true;
    }

    if (PtInRect(&layout.moveToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Move);
        return true;
    }

    if (PtInRect(&layout.panToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Pan);
        return true;
    }

    if (PtInRect(&layout.eraseToolRect, cursor) != 0) {
        ClosePaletteOverlays();
        SetActiveTool(Tool::Erase);
        return true;
    }

    if (PtInRect(&layout.undoRect, cursor) != 0) {
        ClosePaletteOverlays();
        Undo();
        return true;
    }

    if (PtInRect(&layout.redoRect, cursor) != 0) {
        ClosePaletteOverlays();
        Redo();
        return true;
    }

    return true;
}

bool App::HandlePaletteClick(POINT cursor) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const ToolbarLayout toolbarLayout = BuildToolbarLayout(clientRect, palette_.size());

    if (PtInRect(&toolbarLayout.currentColorRect, cursor) != 0) {
        paletteMenuOpen_ = !paletteMenuOpen_;
        paletteWheelOpen_ = false;
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    if (paletteMenuOpen_) {
        const PaletteMenuLayout menuLayout = BuildPaletteMenuLayout(toolbarLayout, palette_.size());
        for (std::size_t index = 0; index < menuLayout.swatchRects.size(); ++index) {
            if (PtInRect(&menuLayout.swatchRects[index], cursor) != 0) {
                SetColorIndex(index);
                return true;
            }
        }

        if (PtInRect(&menuLayout.panelRect, cursor) != 0) {
            return true;
        }

        ClosePaletteOverlays();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    if (paletteWheelOpen_) {
        const PaletteWheelLayout wheelLayout = BuildPaletteWheelLayout(clientRect, paletteWheelCenter_, palette_.size());
        if (const auto index = PaletteWheelIndexAtPoint(wheelLayout, cursor, palette_.size()); index.has_value()) {
            SetColorIndex(*index);
            return true;
        }

        ClosePaletteOverlays();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    return false;
}

bool App::HandleSettingsClick(POINT cursor) {
    if (!settingsOpen_) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());
    if (PtInRect(&layout.panelRect, cursor) == 0) {
        return false;
    }

    for (std::size_t index = 0; index < layout.paletteRects.size(); ++index) {
        if (PtInRect(&layout.paletteRects[index], cursor) != 0) {
            settingsPaletteIndex_ = index;
            InvalidateRect(window_, nullptr, FALSE);
            return true;
        }
    }

    if (PtInRect(&layout.pasteHexRect, cursor) != 0) {
        static_cast<void>(ApplyHexColorFromClipboard());
        return true;
    }

    if (PtInRect(&layout.addColorRect, cursor) != 0) {
        AddPaletteColor();
        return true;
    }

    if (PtInRect(&layout.removeColorRect, cursor) != 0) {
        RemovePaletteColor();
        return true;
    }

    const auto beginSlider = [&](SliderTarget target, const RECT& track) -> bool {
        RECT expanded = track;
        expanded.top -= 8;
        expanded.bottom += 8;
        if (PtInRect(&expanded, cursor) != 0) {
            activeSlider_ = target;
            UpdateActiveSlider(cursor);
            return true;
        }
        return false;
    };

    if (beginSlider(SliderTarget::Red, layout.redTrack)
        || beginSlider(SliderTarget::Green, layout.greenTrack)
        || beginSlider(SliderTarget::Blue, layout.blueTrack)
        || beginSlider(SliderTarget::BrushSize, layout.brushSizeTrack)
        || beginSlider(SliderTarget::Stabilization, layout.stabilizationTrack)
        || beginSlider(SliderTarget::Spacing, layout.spacingTrack)
        || beginSlider(SliderTarget::Smoothing, layout.smoothingTrack)) {
        return true;
    }

    return true;
}

void App::UpdateActiveSlider(POINT cursor) {
    if (activeSlider_ == SliderTarget::None) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());

    const auto sliderValue = [&](const RECT& track) {
        const float width = static_cast<float>(std::max(1L, track.right - track.left));
        return std::clamp((static_cast<float>(cursor.x - track.left) / width), 0.0f, 1.0f);
    };

    std::uint32_t color = palette_.at(settingsPaletteIndex_);
    unsigned char red = GetColorChannel(color, 16);
    unsigned char green = GetColorChannel(color, 8);
    unsigned char blue = GetColorChannel(color, 0);

    switch (activeSlider_) {
    case SliderTarget::Red:
        red = static_cast<unsigned char>(std::lround(sliderValue(layout.redTrack) * 255.0f));
        palette_[settingsPaletteIndex_] = ComposeColor(red, green, blue);
        break;
    case SliderTarget::Green:
        green = static_cast<unsigned char>(std::lround(sliderValue(layout.greenTrack) * 255.0f));
        palette_[settingsPaletteIndex_] = ComposeColor(red, green, blue);
        break;
    case SliderTarget::Blue:
        blue = static_cast<unsigned char>(std::lround(sliderValue(layout.blueTrack) * 255.0f));
        palette_[settingsPaletteIndex_] = ComposeColor(red, green, blue);
        break;
    case SliderTarget::BrushSize:
        brushWidth_ = 1.0f + (sliderValue(layout.brushSizeTrack) * 23.0f);
        break;
    case SliderTarget::Stabilization:
        inputSmoothing_ = std::clamp(sliderValue(layout.stabilizationTrack), 0.05f, 0.95f);
        break;
    case SliderTarget::Spacing:
        inputSpacing_ = 0.05f + (sliderValue(layout.spacingTrack) * 4.0f);
        break;
    case SliderTarget::Smoothing:
        strokeSmoothing_ = sliderValue(layout.smoothingTrack);
        break;
    case SliderTarget::None:
        break;
    }

    InvalidateRect(window_, nullptr, FALSE);
}

bool App::IsCursorInsideSettingsPanel(POINT cursor) const {
    if (!settingsOpen_) {
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());
    return PtInRect(&layout.panelRect, cursor) != 0;
}

void App::PanBy(float dx, float dy) {
    panX_ += dx;
    panY_ += dy;
    InvalidateCanvas();
}

void App::BeginZoomSession(POINT cursor) {
    zoomSessionActive_ = true;
    zoomAnchorValid_ = false;

    if (const auto hitPage = HitTestPage(cursor); hitPage.has_value()) {
        zoomAnchorValid_ = true;
        zoomAnchorPageIndex_ = hitPage->pageIndex;
        zoomAnchorPage_ = ScreenToPage(cursor, nullptr);
        zoomAnchorCursor_ = cursor;
    }

    SetTimer(window_, kZoomSessionTimerId, kZoomSessionDelayMs, nullptr);
}

void App::ZoomAt(int delta, POINT cursor) {
    const float factor = delta > 0 ? 1.1f : 0.9f;
    const float newZoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);

    if (std::abs(newZoom - zoom_) < 0.0001f) {
        return;
    }

    CancelDeferredRasterRefresh();
    BeginZoomSession(cursor);
    zoom_ = newZoom;
    targetZoom_ = newZoom;

    if (zoomAnchorValid_) {
        const POINT anchoredScreen = PageToScreen(zoomAnchorPageIndex_, zoomAnchorPage_);
        panX_ += static_cast<float>(zoomAnchorCursor_.x - anchoredScreen.x);
        panY_ += static_cast<float>(zoomAnchorCursor_.y - anchoredScreen.y);
    }

    InvalidateCanvas();
}

void App::EndZoomSession() {
    if (!zoomSessionActive_) {
        return;
    }

    zoomSessionActive_ = false;
    zoomAnchorValid_ = false;
    KillTimer(window_, kZoomSessionTimerId);
    HandleDeferredRasterRefresh();
}

void App::ScheduleDeferredRasterRefresh() {
    if (window_ == nullptr || zoomSessionActive_) {
        return;
    }

    rasterRefreshPending_ = true;
    SetTimer(window_, kRasterRefreshTimerId, kRasterRefreshDelayMs, nullptr);
}

void App::CancelDeferredRasterRefresh(bool clearPending) {
    if (window_ == nullptr) {
        return;
    }

    KillTimer(window_, kRasterRefreshTimerId);
    if (clearPending) {
        rasterRefreshPending_ = false;
    }
}

void App::HandleDeferredRasterRefresh() {
    if (isDrawing_ || isPanning_ || isErasing_ || isMovingSelection_ || isTransformingImage_ || isTransformingStroke_ || activeSlider_ != SliderTarget::None || zoomSessionActive_) {
        SetTimer(window_, kRasterRefreshTimerId, kRasterRefreshDelayMs, nullptr);
        return;
    }

    CancelDeferredRasterRefresh();
    InvalidateVisiblePageSurfaceCaches();
    EnsureVisiblePageRasterQuality();
    InvalidateCanvas();
}

void App::HandleCompletedRenderResults() {
    std::deque<RenderResult> results;
    {
        std::lock_guard lock(renderMutex_);
        results.swap(renderResults_);
    }

    bool updated = false;
    while (!results.empty()) {
        RenderResult result = std::move(results.front());
        results.pop_front();

        if (result.pageIndex < document_.PageCount()) {
            const auto pending = pendingRenderScales_.find(result.pageIndex);
            if (pending != pendingRenderScales_.end() && pending->second <= result.renderScale + 0.001f) {
                pendingRenderScales_.erase(pending);
            }
        }

        const auto pending = pendingRenderScales_.find(result.pageIndex);
        if (pending != pendingRenderScales_.end() && pending->second > result.renderScale + 0.001f) {
            continue;
        }

        if (result.generation != renderGeneration_ || !result.raster.has_value() || result.pageIndex >= document_.PageCount()) {
            continue;
        }

        Page& page = document_.PageAt(result.pageIndex);
        if (result.renderScale + 0.001f < page.raster.scale) {
            continue;
        }

        page.raster = std::move(*result.raster);
        InvalidatePageSurfaceCache(result.pageIndex);
        updated = true;
    }

    if (updated) {
        InvalidateCanvas();
    }
}

void App::QueuePageRasterRequest(const PageView& pageView) {
    if (!document_.IsPdfDocument() || !document_.HasPdfData() || !pageView.valid) {
        return;
    }

    Page& page = document_.PageAt(pageView.pageIndex);
    const float desiredScale = DesiredRenderScale(pageView);
    const float currentScale = page.raster.scale;
    const bool missingRaster = page.raster.pixels.empty() || page.raster.width <= 0 || page.raster.height <= 0;
    const bool needsUpgrade = missingRaster || currentScale <= 0.0f || desiredScale > (currentScale * 1.2f);
    if (!needsUpgrade) {
        return;
    }

    const auto pending = pendingRenderScales_.find(pageView.pageIndex);
    if (pending != pendingRenderScales_.end() && pending->second >= desiredScale - 0.001f) {
        return;
    }

    pendingRenderScales_[pageView.pageIndex] = desiredScale;
    {
        std::lock_guard lock(renderMutex_);
        renderRequests_.erase(
            std::remove_if(
                renderRequests_.begin(),
                renderRequests_.end(),
                [&](const RenderRequest& request) {
                    return request.generation == renderGeneration_
                        && request.pageIndex == pageView.pageIndex
                        && request.renderScale <= desiredScale + 0.001f;
                }),
            renderRequests_.end());
        renderRequests_.push_back({ document_.PdfData(), pageView.pageIndex, desiredScale, renderGeneration_ });
    }
    renderCv_.notify_one();
}

void App::ChangeBrushSize(float delta) {
    brushWidth_ = std::clamp(brushWidth_ + delta, 1.0f, 24.0f);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::CycleColor() {
    SetColorIndex((colorIndex_ + 1U) % palette_.size());
}

void App::SetColorIndex(std::size_t index) {
    if (index >= palette_.size()) {
        return;
    }

    colorIndex_ = index;
    ClosePaletteOverlays();
    bool recoloredSelection = false;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        if (pageIndex >= selectedStrokeIndices_.size() || selectedStrokeIndices_[pageIndex].empty()) {
            continue;
        }
        Page& page = document_.PageAt(pageIndex);
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageIndex]) {
            if (strokeIndex < page.strokes.size()) {
                page.strokes[strokeIndex].style.color = palette_[index];
                recoloredSelection = true;
            }
        }
        if (recoloredSelection) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }

    if (recoloredSelection) {
        InvalidateCanvas();
        return;
    }

    InvalidateRect(window_, nullptr, FALSE);
}

void App::OpenDocument() {
    wchar_t fileBuffer[MAX_PATH] = {};
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter =
        L"All Supported Files\0*.cnote;*.pdf\0"
        L"Cnote Files\0*.cnote\0"
        L"PDF Files\0*.pdf\0"
        L"All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"cnote";

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    static_cast<void>(OpenDocumentPath(fileBuffer));
}

bool App::OpenDocumentPath(const std::wstring& filePath) {
    if (HasExtension(filePath, kCnoteFileExtension)) {
        return OpenProject(filePath);
    }

    if (HasExtension(filePath, L".pdf")) {
        return OpenPdf(filePath);
    }

    MessageBoxW(window_, L"Unsupported file type. Open a PDF or a .cnote file.", L"Open Document", MB_OK | MB_ICONERROR);
    return false;
}

bool App::OpenProject(const std::wstring& filePath) {
    const auto opened = ProjectFile::Load(filePath, pdfBackend_);
    if (!opened.has_value()) {
        MessageBoxW(window_, L"Failed to open the cnote file.", L"Open Cnote", MB_OK | MB_ICONERROR);
        return false;
    }

    PrepareForDocumentSwitch();
    ApplyDocument(*opened);
    return true;
}

bool App::OpenPdf(const std::wstring& filePath) {
    const auto opened = pdfBackend_.Open(filePath);
    if (!opened.has_value()) {
        MessageBoxW(window_, L"Failed to open or render the PDF.", L"Open PDF", MB_OK | MB_ICONERROR);
        return false;
    }

    PrepareForDocumentSwitch();
    ApplyDocument(*opened);
    return true;
}

bool App::SaveAsCnote() {
    wchar_t fileBuffer[MAX_PATH] = {};
    const std::wstring suggested = SuggestedCnotePath();
    std::wcsncpy(fileBuffer, suggested.c_str(), std::size(fileBuffer) - 1U);

    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"Cnote Files\0*.cnote\0All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"cnote";

    if (!GetSaveFileNameW(&dialog)) {
        return false;
    }

    if (!ProjectFile::Save(document_, fileBuffer)) {
        MessageBoxW(window_, L"Failed to save the cnote file.", L"Save as Cnote", MB_OK | MB_ICONERROR);
        return false;
    }

    document_.SetFilePath(fileBuffer);
    UpdateWindowTitle();
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool App::ExportAsPdf() {
    wchar_t fileBuffer[MAX_PATH] = {};
    const std::wstring suggested = SuggestedPdfExportPath();
    std::wcsncpy(fileBuffer, suggested.c_str(), std::size(fileBuffer) - 1U);

    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"pdf";

    if (!GetSaveFileNameW(&dialog)) {
        return false;
    }

    if (!pdfBackend_.ExportPdf(document_, fileBuffer, 2.0f, strokeSmoothing_)) {
        MessageBoxW(window_, L"Failed to export the PDF.", L"Export as PDF", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

void App::EnsureVisiblePageRasterQuality() {
    if (!document_.IsPdfDocument() || !document_.HasPdfData()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int visibleTop = kToolbarHeight - 200;
    const int visibleBottom = static_cast<int>(clientRect.bottom) + 200;
    std::vector<bool> keepCache(document_.PageCount(), false);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom < visibleTop || pageView.rect.top > visibleBottom) {
            continue;
        }
        keepCache[pageView.pageIndex] = true;
        QueuePageRasterRequest(pageView);
    }

    const int cacheKeepTop = visibleTop - 1200;
    const int cacheKeepBottom = visibleBottom + 1200;
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        if (pageView.rect.bottom >= cacheKeepTop && pageView.rect.top <= cacheKeepBottom) {
            keepCache[pageView.pageIndex] = true;
        }
    }

    for (std::size_t pageIndex = 0; pageIndex < keepCache.size(); ++pageIndex) {
        if (!keepCache[pageIndex]) {
            InvalidatePageSurfaceCache(pageIndex);
        }
    }
}

void App::EnsurePageRasterQuality(const PageView& pageView) {
    QueuePageRasterRequest(pageView);
}

bool App::IsCursorInsidePage(POINT cursor) const {
    return HitTestPage(cursor).has_value();
}

std::optional<App::PageView> App::HitTestPage(POINT cursor) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (pageView.valid && PtInRect(&pageView.rect, cursor) != 0) {
            return pageView;
        }
    }
    return std::nullopt;
}

Vec2 App::ScreenToPage(POINT cursor, std::size_t* pageIndex) const {
    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value() || hitPage->scale <= 0.0f) {
        if (pageIndex != nullptr) {
            *pageIndex = document_.CurrentPageIndex();
        }
        return {};
    }

    if (pageIndex != nullptr) {
        *pageIndex = hitPage->pageIndex;
    }

    const Page& page = document_.PageAt(hitPage->pageIndex);
    const float pageX = (static_cast<float>(cursor.x - hitPage->rect.left)) / hitPage->scale;
    const float pageY = (static_cast<float>(cursor.y - hitPage->rect.top)) / hitPage->scale;
    return {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
}

POINT App::PageToScreen(std::size_t pageIndex, Vec2 point) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, pageIndex);
    POINT output {};
    output.x = pageView.rect.left + static_cast<LONG>(std::lround(point.x * pageView.scale));
    output.y = pageView.rect.top + static_cast<LONG>(std::lround(point.y * pageView.scale));
    return output;
}

App::PageView App::CalculatePageView(RECT clientRect, std::size_t pageIndex) const {
    const std::vector<PageView> pageViews = CalculatePageViews(clientRect);
    if (pageIndex >= pageViews.size()) {
        return {};
    }

    return pageViews[pageIndex];
}

std::vector<App::PageView> App::CalculatePageViews(RECT clientRect) const {
    const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
    const int canvasWidth = std::max(1, clientWidth);
    float widestPage = 1.0f;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        widestPage = std::max(widestPage, document_.PageAt(pageIndex).width);
    }

    const float fitScale = static_cast<float>(canvasWidth - 120) / widestPage;
    const float scale = std::max(0.1f, fitScale * zoom_);
    const int canvasCenterX = canvasWidth / 2;
    int top = kToolbarHeight + 40 + static_cast<int>(std::lround(panY_));

    std::vector<PageView> pageViews;
    pageViews.reserve(document_.PageCount());
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        const Page& page = document_.PageAt(pageIndex);
        const int pageWidth = static_cast<int>(std::lround(page.width * scale));
        const int pageHeight = static_cast<int>(std::lround(page.height * scale));
        const int left = static_cast<int>(std::lround(canvasCenterX - (pageWidth / 2.0f) + panX_));
        pageViews.push_back({
            MakeRect(left, top, left + pageWidth, top + pageHeight),
            pageIndex,
            scale,
            pageWidth > 0 && pageHeight > 0,
        });
        top += pageHeight + kPageGap;
    }

    return pageViews;
}

float App::DesiredRenderScale(const PageView& pageView) const {
    if (!pageView.valid) {
        return 0.0f;
    }

    constexpr float kRenderOverscan = 1.2f;
    constexpr float kMinRenderScale = 1.5f;
    constexpr float kMaxRenderScale = 6.0f;
    return std::clamp(pageView.scale * kRenderOverscan, kMinRenderScale, kMaxRenderScale);
}

COLORREF App::ToColorRef(std::uint32_t argb) const {
    const auto red = static_cast<unsigned char>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned char>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned char>(argb & 0xFFU);
    return RGB(red, green, blue);
}

std::uint32_t App::ComposeColor(unsigned char red, unsigned char green, unsigned char blue) const {
    return 0xFF000000U | (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(green) << 8U)
        | static_cast<std::uint32_t>(blue);
}

unsigned char App::GetColorChannel(std::uint32_t color, int shift) const {
    return static_cast<unsigned char>((color >> shift) & 0xFFU);
}

void App::Paint() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, clientRect.right, clientRect.bottom);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    SaveDC(memoryDc);
    IntersectClipRect(memoryDc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right, paint.rcPaint.bottom);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 22, 29));
    FillRect(memoryDc, &paint.rcPaint, backgroundBrush);
    DeleteObject(backgroundBrush);

    PaintToolbar(memoryDc, clientRect);
    PaintCanvas(memoryDc, clientRect);
    PaintSettingsPanel(memoryDc, clientRect);
    PaintPaletteMenu(memoryDc, clientRect);
    PaintPaletteWheel(memoryDc, clientRect);

    BitBlt(
        windowDc,
        paint.rcPaint.left,
        paint.rcPaint.top,
        paint.rcPaint.right - paint.rcPaint.left,
        paint.rcPaint.bottom - paint.rcPaint.top,
        memoryDc,
        paint.rcPaint.left,
        paint.rcPaint.top,
        SRCCOPY);

    RestoreDC(memoryDc, -1);
    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);

    EndPaint(window_, &paint);
}

void App::PaintToolbar(HDC hdc, RECT clientRect) const {
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = kToolbarHeight;

    const COLORREF toolbarBackground = RGB(15, 18, 24);
    const COLORREF panelBorder = RGB(43, 52, 64);
    const COLORREF primaryText = RGB(236, 240, 245);
    const COLORREF selectedFill = RGB(48, 68, 94);
    const COLORREF idleFill = RGB(28, 34, 43);

    HBRUSH toolbarBrush = CreateSolidBrush(toolbarBackground);
    FillRect(hdc, &toolbarRect, toolbarBrush);
    DeleteObject(toolbarBrush);

    HPEN dividerPen = CreatePen(PS_SOLID, 1, panelBorder);
    HGDIOBJ oldPen = SelectObject(hdc, dividerPen);
    MoveToEx(hdc, 0, kToolbarHeight - 1, nullptr);
    LineTo(hdc, clientRect.right, kToolbarHeight - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(dividerPen);

    SetBkMode(hdc, TRANSPARENT);

    HFONT bodyFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT labelFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size());

    HGDIOBJ previousFont = SelectObject(hdc, labelFont);
    SetTextColor(hdc, primaryText);
    RECT fileRect = layout.menu.fileRect;
    RECT editRect = layout.menu.editRect;
    RECT viewRect = layout.menu.viewRect;
    DrawTextW(hdc, L"File", -1, &fileRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdc, L"Edit", -1, &editRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdc, L"View", -1, &viewRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const auto paintToolButton = [&](RECT rect, const wchar_t* label, bool selected, bool enabled = true) {
        FillRoundedRect(hdc, rect, selected ? selectedFill : idleFill);
        StrokeRoundedRect(hdc, rect, selected ? RGB(92, 137, 187) : panelBorder, 1);
        SetTextColor(hdc, enabled ? (selected ? RGB(240, 247, 255) : primaryText) : RGB(102, 111, 124));
        SelectObject(hdc, bodyFont);
        RECT labelRect = rect;
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    const auto paintIconToolButton = [&](RECT rect, auto&& paintFallbackIcon, const wchar_t* svgFileName, bool selected, bool enabled = true) {
        FillRoundedRect(hdc, rect, selected ? selectedFill : idleFill);
        StrokeRoundedRect(hdc, rect, selected ? RGB(92, 137, 187) : panelBorder, 1);
        const COLORREF iconColor = enabled
            ? (selected ? RGB(240, 247, 255) : primaryText)
            : RGB(102, 111, 124);
        if (const SvgIconData* icon = ToolbarSvgIcon(svgFileName); icon != nullptr) {
            PaintSvgIcon(hdc, rect, *icon, iconColor);
        } else {
            paintFallbackIcon(hdc, rect, iconColor);
        }
    };

    const auto paintSvgActionButton = [&](RECT rect, const wchar_t* svgFileName, const wchar_t* label, bool enabled) {
        FillRoundedRect(hdc, rect, idleFill);
        StrokeRoundedRect(hdc, rect, panelBorder, 1);
        const COLORREF iconColor = enabled ? primaryText : RGB(102, 111, 124);
        if (const SvgIconData* icon = ToolbarSvgIcon(svgFileName); icon != nullptr) {
            PaintSvgIcon(hdc, rect, *icon, iconColor);
        } else {
            SetTextColor(hdc, iconColor);
            SelectObject(hdc, bodyFont);
            RECT labelRect = rect;
            DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    };

    paintSvgActionButton(layout.undoRect, L"undo.svg", L"Undo", CanUndo());
    paintSvgActionButton(layout.redoRect, L"redo.svg", L"Redo", CanRedo());
    paintToolButton(layout.previousPageRect, L"<", false, CanGoToPreviousPage());
    paintToolButton(layout.nextPageRect, L">", false, CanGoToNextPage());
    paintIconToolButton(layout.selectToolRect, PaintSelectToolIcon, L"lasso.svg", activeTool_ == Tool::Select);
    paintIconToolButton(layout.moveToolRect, PaintMoveToolIcon, L"cursor.svg", activeTool_ == Tool::Move || isMovingSelection_ || isTransformingImage_ || isTransformingStroke_);
    paintIconToolButton(layout.eraseToolRect, PaintEraseToolIcon, L"erase.svg", activeTool_ == Tool::Erase);
    paintIconToolButton(layout.drawToolRect, PaintDrawToolIcon, L"draw.svg", activeTool_ == Tool::Draw && !isPanning_);
    paintIconToolButton(layout.panToolRect, PaintPanToolIcon, L"pan.svg", activeTool_ == Tool::Pan || isPanning_);

    FillRoundedRect(hdc, layout.pageLabelRect, RGB(22, 28, 36));
    StrokeRoundedRect(hdc, layout.pageLabelRect, panelBorder, 1);
    SetTextColor(hdc, primaryText);
    SelectObject(hdc, labelFont);
    RECT pageLabelRect = layout.pageLabelRect;
    const std::wstring pageLabel = PageIndicatorLabel();
    DrawTextW(hdc, pageLabel.c_str(), -1, &pageLabelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    FillRoundedRect(hdc, layout.currentColorRect, ToColorRef(palette_[colorIndex_]));
    StrokeRoundedRect(hdc, layout.currentColorRect, paletteMenuOpen_ ? RGB(244, 247, 252) : panelBorder, paletteMenuOpen_ ? 2 : 1);

    SelectObject(hdc, previousFont);
    DeleteObject(bodyFont);
    DeleteObject(labelFont);
}

void App::PaintPaletteMenu(HDC hdc, RECT clientRect) const {
    if (!paletteMenuOpen_) {
        return;
    }

    const ToolbarLayout toolbarLayout = BuildToolbarLayout(clientRect, palette_.size());
    const PaletteMenuLayout layout = BuildPaletteMenuLayout(toolbarLayout, palette_.size());
    const COLORREF panelFill = RGB(18, 24, 32);
    const COLORREF panelBorder = RGB(46, 58, 74);

    FillRoundedRect(hdc, layout.panelRect, panelFill);
    StrokeRoundedRect(hdc, layout.panelRect, panelBorder, 1);

    for (std::size_t index = 0; index < layout.swatchRects.size(); ++index) {
        const RECT swatchRect = layout.swatchRects[index];
        FillRoundedRect(hdc, swatchRect, ToColorRef(palette_[index]));
        StrokeRoundedRect(hdc, swatchRect, index == colorIndex_ ? RGB(244, 247, 252) : panelBorder, index == colorIndex_ ? 2 : 1);
    }
}

void App::PaintPaletteWheel(HDC hdc, RECT clientRect) const {
    if (!paletteWheelOpen_) {
        return;
    }

    const PaletteWheelLayout layout = BuildPaletteWheelLayout(clientRect, paletteWheelCenter_, palette_.size());
    if (palette_.empty()) {
        return;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const int outerDiameter = layout.outerRadius * 2;
    const int innerDiameter = layout.innerRadius * 2;
    Gdiplus::Rect outerRect(
        layout.center.x - layout.outerRadius,
        layout.center.y - layout.outerRadius,
        outerDiameter,
        outerDiameter);

    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(210, 18, 24, 32));
    graphics.FillEllipse(
        &shadowBrush,
        static_cast<Gdiplus::REAL>(layout.center.x - (layout.outerRadius + 6)),
        static_cast<Gdiplus::REAL>(layout.center.y - (layout.outerRadius + 6)),
        static_cast<Gdiplus::REAL>((layout.outerRadius + 6) * 2),
        static_cast<Gdiplus::REAL>((layout.outerRadius + 6) * 2));

    const float sweep = 360.0f / static_cast<float>(palette_.size());
    float start = -90.0f;
    for (std::size_t index = 0; index < palette_.size(); ++index) {
        const COLORREF color = ToColorRef(palette_[index]);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPie(&brush, outerRect, start, sweep);

        Gdiplus::Pen separatorPen(Gdiplus::Color(index == colorIndex_ ? 255 : 120, 244, 247, 252), index == colorIndex_ ? 3.0f : 1.2f);
        graphics.DrawPie(&separatorPen, outerRect, start, sweep);
        start += sweep;
    }

    Gdiplus::SolidBrush cutoutBrush(Gdiplus::Color(245, 18, 24, 32));
    graphics.FillEllipse(
        &cutoutBrush,
        static_cast<Gdiplus::REAL>(layout.center.x - layout.innerRadius),
        static_cast<Gdiplus::REAL>(layout.center.y - layout.innerRadius),
        static_cast<Gdiplus::REAL>(innerDiameter),
        static_cast<Gdiplus::REAL>(innerDiameter));

    Gdiplus::Pen ringBorder(Gdiplus::Color(180, 244, 247, 252), 1.5f);
    graphics.DrawEllipse(
        &ringBorder,
        static_cast<Gdiplus::REAL>(layout.center.x - layout.outerRadius),
        static_cast<Gdiplus::REAL>(layout.center.y - layout.outerRadius),
        static_cast<Gdiplus::REAL>(outerDiameter),
        static_cast<Gdiplus::REAL>(outerDiameter));
    graphics.DrawEllipse(
        &ringBorder,
        static_cast<Gdiplus::REAL>(layout.center.x - layout.innerRadius),
        static_cast<Gdiplus::REAL>(layout.center.y - layout.innerRadius),
        static_cast<Gdiplus::REAL>(innerDiameter),
        static_cast<Gdiplus::REAL>(innerDiameter));
}

void App::PaintSettingsPanel(HDC hdc, RECT clientRect) const {
    if (!settingsOpen_) {
        return;
    }

    const SettingsLayout layout = BuildSettingsLayout(clientRect, palette_.size());
    const COLORREF panelFill = RGB(18, 24, 32);
    const COLORREF panelBorder = RGB(46, 58, 74);
    const COLORREF textColor = RGB(235, 240, 246);
    const COLORREF mutedText = RGB(141, 152, 167);

    FillRoundedRect(hdc, layout.panelRect, panelFill);
    StrokeRoundedRect(hdc, layout.panelRect, panelBorder, 1);

    SetBkMode(hdc, TRANSPARENT);
    HFONT titleFont = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT bodyFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(hdc, titleFont);
    SetTextColor(hdc, textColor);
    TextOutW(hdc, layout.panelRect.left + 18, layout.panelRect.top + 18, L"Brush Settings", 14);

    SelectObject(hdc, bodyFont);
    SetTextColor(hdc, mutedText);
    TextOutW(hdc, layout.panelRect.left + 18, layout.panelRect.top + 96, L"Palette", 7);

    for (std::size_t index = 0; index < layout.paletteRects.size(); ++index) {
        RECT slotRect = layout.paletteRects[index];
        FillRoundedRect(hdc, slotRect, ToColorRef(palette_[index]));
        StrokeRoundedRect(hdc, slotRect, index == settingsPaletteIndex_ ? RGB(244, 247, 252) : panelBorder, index == settingsPaletteIndex_ ? 2 : 1);
    }

    const auto paintButton = [&](RECT rect, const wchar_t* label, bool enabled = true) {
        FillRoundedRect(hdc, rect, enabled ? RGB(28, 34, 43) : RGB(24, 28, 35));
        StrokeRoundedRect(hdc, rect, panelBorder, 1);
        SetTextColor(hdc, enabled ? textColor : RGB(102, 111, 124));
        RECT labelRect = rect;
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    paintButton(layout.pasteHexRect, L"Paste Hex");
    paintButton(layout.addColorRect, L"+", palette_.size() < 12U);
    paintButton(layout.removeColorRect, L"-", palette_.size() > 1U);

    const auto paintSlider = [&](const wchar_t* label, const RECT& track, float normalized, COLORREF accent, int value) {
        RECT labelRect = MakeRect(track.left, track.top - 24, track.right, track.top - 6);
        SetTextColor(hdc, mutedText);
        DrawTextW(hdc, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        wchar_t valueBuffer[32] = {};
        std::swprintf(valueBuffer, std::size(valueBuffer), L"%d", value);
        SetTextColor(hdc, textColor);
        DrawTextW(hdc, valueBuffer, -1, &labelRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        FillRoundedRect(hdc, track, RGB(33, 40, 52));
        RECT fillRect = track;
        fillRect.right = track.left + static_cast<LONG>(std::lround((track.right - track.left) * std::clamp(normalized, 0.0f, 1.0f)));
        FillRoundedRect(hdc, fillRect, accent);

        RECT knob = MakeRect(fillRect.right - 6, track.top - 5, fillRect.right + 6, track.bottom + 5);
        FillRoundedRect(hdc, knob, RGB(245, 247, 250));
    };

    const std::uint32_t activeColor = palette_[settingsPaletteIndex_];
    paintSlider(L"Red", layout.redTrack, GetColorChannel(activeColor, 16) / 255.0f, RGB(220, 76, 70), GetColorChannel(activeColor, 16));
    paintSlider(L"Green", layout.greenTrack, GetColorChannel(activeColor, 8) / 255.0f, RGB(70, 190, 118), GetColorChannel(activeColor, 8));
    paintSlider(L"Blue", layout.blueTrack, GetColorChannel(activeColor, 0) / 255.0f, RGB(74, 132, 225), GetColorChannel(activeColor, 0));
    paintSlider(L"Brush Size", layout.brushSizeTrack, (brushWidth_ - 1.0f) / 23.0f, RGB(56, 189, 248), static_cast<int>(std::lround(brushWidth_)));
    paintSlider(L"Stabilization", layout.stabilizationTrack, inputSmoothing_, RGB(129, 140, 248), static_cast<int>(std::lround(inputSmoothing_ * 100.0f)));
    paintSlider(L"Point Spacing", layout.spacingTrack, (inputSpacing_ - 0.05f) / 4.0f, RGB(244, 114, 182), static_cast<int>(std::lround(inputSpacing_ * 100.0f)));
    paintSlider(L"Stroke Smoothing", layout.smoothingTrack, strokeSmoothing_, RGB(245, 158, 11), static_cast<int>(std::lround(strokeSmoothing_ * 100.0f)));

    SelectObject(hdc, previousFont);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
}

void App::PaintCanvas(HDC hdc, RECT clientRect) {
    RECT canvasRect = clientRect;
    canvasRect.top = kToolbarHeight;

    HBRUSH canvasBrush = CreateSolidBrush(RGB(22, 26, 33));
    FillRect(hdc, &canvasRect, canvasBrush);
    DeleteObject(canvasBrush);

    SaveDC(hdc);
    IntersectClipRect(hdc, canvasRect.left, canvasRect.top, canvasRect.right, canvasRect.bottom);
    RECT clipRect {};
    GetClipBox(hdc, &clipRect);
    for (const PageView& pageView : CalculatePageViews(clientRect)) {
        if (!pageView.valid) {
            continue;
        }
        const int paintPadding = zoomSessionActive_ ? 0 : 20;
        if (pageView.rect.bottom < canvasRect.top - paintPadding || pageView.rect.top > canvasRect.bottom + paintPadding) {
            continue;
        }
        RECT visibleRect {};
        if (IntersectRect(&visibleRect, &pageView.rect, &clipRect) == 0) {
            continue;
        }
        PaintPage(hdc, pageView);
    }
    RestoreDC(hdc, -1);
}

bool App::TryAppendStrokeToPageSurfaceCache(std::size_t pageIndex, const Stroke& stroke) {
    if (pageIndex >= pageSurfaceCaches_.size() || stroke.points.empty()) {
        return false;
    }

    const Page& page = document_.PageAt(pageIndex);
    PageSurfaceCache& cache = pageSurfaceCaches_[pageIndex];
    if (!cache.valid || cache.pixels.empty() || cache.width <= 0 || cache.height <= 0) {
        return false;
    }

    if (cache.pageIndex != pageIndex
        || cache.strokeCount + 1U != page.strokes.size()
        || cache.imageCount != page.images.size()
        || cache.rasterWidth != page.raster.width
        || cache.rasterHeight != page.raster.height
        || std::abs(cache.rasterScale - page.raster.scale) >= 0.001f) {
        return false;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = cache.width;
    bitmapInfo.bmiHeader.biHeight = -cache.height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return false;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return false;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);
    std::copy(cache.pixels.begin(), cache.pixels.end(), static_cast<std::uint8_t*>(bits));

    const float cacheScaleX = cache.width > 0 ? static_cast<float>(cache.width) / std::max(1.0f, page.width) : 1.0f;
    const float cacheScaleY = cache.height > 0 ? static_cast<float>(cache.height) / std::max(1.0f, page.height) : 1.0f;
    const PageView cacheView {
        MakeRect(0, 0, cache.width, cache.height),
        pageIndex,
        std::min(cacheScaleX, cacheScaleY),
        true,
    };
    PaintStroke(cacheDc, stroke, cacheView, false);

    std::copy_n(static_cast<const std::uint8_t*>(bits), cache.pixels.size(), cache.pixels.data());
    cache.strokeCount = page.strokes.size();
    cache.pageScale = cacheView.scale;

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
    return true;
}

void App::EnsurePageSurfaceCache(const PageView& pageView) {
    if (pageView.pageIndex >= pageSurfaceCaches_.size()) {
        pageSurfaceCaches_.resize(document_.PageCount());
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
    const int width = pageView.rect.right - pageView.rect.left;
    const int height = pageView.rect.bottom - pageView.rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const bool cacheMatches = cache.valid
        && cache.pageIndex == pageView.pageIndex
        && cache.strokeCount == page.strokes.size()
        && cache.imageCount == page.images.size()
        && cache.rasterWidth == page.raster.width
        && cache.rasterHeight == page.raster.height
        && std::abs(cache.rasterScale - page.raster.scale) < 0.001f
        && !cache.pixels.empty();
    if (cacheMatches) {
        return;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC cacheDc = CreateCompatibleDC(nullptr);
    if (cacheDc == nullptr) {
        return;
    }

    void* bits = nullptr;
    HBITMAP cacheBitmap = CreateDIBSection(cacheDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (cacheBitmap == nullptr || bits == nullptr) {
        if (cacheBitmap != nullptr) {
            DeleteObject(cacheBitmap);
        }
        DeleteDC(cacheDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(cacheDc, cacheBitmap);

    RECT cacheRect = MakeRect(0, 0, width, height);
    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(cacheDc, &cacheRect, pageBrush);
    DeleteObject(pageBrush);

    if (!page.raster.pixels.empty() && page.raster.width > 0 && page.raster.height > 0) {
        BITMAPINFO pageBitmapInfo {};
        pageBitmapInfo.bmiHeader.biSize = sizeof(pageBitmapInfo.bmiHeader);
        pageBitmapInfo.bmiHeader.biWidth = page.raster.width;
        pageBitmapInfo.bmiHeader.biHeight = -page.raster.height;
        pageBitmapInfo.bmiHeader.biPlanes = 1;
        pageBitmapInfo.bmiHeader.biBitCount = 32;
        pageBitmapInfo.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(cacheDc, HALFTONE);
        StretchDIBits(
            cacheDc,
            0,
            0,
            width,
            height,
            0,
            0,
            page.raster.width,
            page.raster.height,
            page.raster.pixels.data(),
            &pageBitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);
    }

    const PageView cacheView {
        MakeRect(0, 0, width, height),
        pageView.pageIndex,
        pageView.scale,
        true,
    };
    Gdiplus::Graphics imageGraphics(cacheDc);
    imageGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    imageGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    imageGraphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    for (const PageImage& image : page.images) {
        if (image.pixels.empty() || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
            continue;
        }

        Gdiplus::Bitmap bitmap(
            image.pixelWidth,
            image.pixelHeight,
            image.stride,
            PixelFormat32bppRGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(image.pixels.data())));
        const Gdiplus::REAL centerX = static_cast<Gdiplus::REAL>((image.x + (image.width * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL centerY = static_cast<Gdiplus::REAL>((image.y + (image.height * 0.5f)) * cacheView.scale);
        const Gdiplus::REAL drawWidth = static_cast<Gdiplus::REAL>(image.width * cacheView.scale);
        const Gdiplus::REAL drawHeight = static_cast<Gdiplus::REAL>(image.height * cacheView.scale);

        imageGraphics.ResetTransform();
        imageGraphics.TranslateTransform(centerX, centerY);
        imageGraphics.RotateTransform(image.rotationDegrees);
        imageGraphics.DrawImage(&bitmap, -drawWidth * 0.5f, -drawHeight * 0.5f, drawWidth, drawHeight);
    }
    imageGraphics.ResetTransform();
    for (const Stroke& stroke : page.strokes) {
        PaintStroke(cacheDc, stroke, cacheView, false);
    }

    const int stride = width * 4;
    const std::size_t totalBytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    cache.pixels.resize(totalBytes);
    std::copy_n(static_cast<const std::uint8_t*>(bits), totalBytes, cache.pixels.data());
    cache.width = width;
    cache.height = height;
    cache.stride = stride;
    cache.pageIndex = pageView.pageIndex;
    cache.strokeCount = page.strokes.size();
    cache.imageCount = page.images.size();
    cache.rasterWidth = page.raster.width;
    cache.rasterHeight = page.raster.height;
    cache.rasterScale = page.raster.scale;
    cache.pageScale = pageView.scale;
    cache.valid = true;

    SelectObject(cacheDc, previousBitmap);
    DeleteObject(cacheBitmap);
    DeleteDC(cacheDc);
}

void App::PaintPage(HDC hdc, const PageView& pageView) {
    if (!zoomSessionActive_) {
        RECT shadow = pageView.rect;
        OffsetRect(&shadow, 8, 10);
        HBRUSH shadowBrush = CreateSolidBrush(RGB(7, 10, 15));
        FillRect(hdc, &shadow, shadowBrush);
        DeleteObject(shadowBrush);
    }

    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &pageView.rect, pageBrush);
    DeleteObject(pageBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(94, 105, 121));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SaveDC(hdc);
    IntersectClipRect(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);

    EnsurePageSurfaceCache(pageView);
    if (pageView.pageIndex < pageSurfaceCaches_.size()) {
        const PageSurfaceCache& cache = pageSurfaceCaches_[pageView.pageIndex];
        if (cache.valid && !cache.pixels.empty()) {
            BITMAPINFO bitmapInfo {};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = cache.width;
            bitmapInfo.bmiHeader.biHeight = -cache.height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(hdc, (isPanning_ || isDrawing_ || isErasing_ || zoomSessionActive_) ? COLORONCOLOR : HALFTONE);
            StretchDIBits(
                hdc,
                pageView.rect.left,
                pageView.rect.top,
                pageView.rect.right - pageView.rect.left,
                pageView.rect.bottom - pageView.rect.top,
                0,
                0,
                cache.width,
                cache.height,
                cache.pixels.data(),
                &bitmapInfo,
                DIB_RGB_COLORS,
                SRCCOPY);
        }
    }

    const Page& page = document_.PageAt(pageView.pageIndex);
    const auto singleImage = SingleSelectedImage();
    const auto singleStrokePage = SingleSelectedStrokePage();
    for (std::size_t imageIndex = 0; imageIndex < page.images.size(); ++imageIndex) {
        if (!IsImageSelected(pageView.pageIndex, imageIndex)) {
            continue;
        }

        const PageImage& image = page.images[imageIndex];
        const auto corners = ImageCorners(image);
        POINT polygon[5] {};
        for (std::size_t index = 0; index < corners.size(); ++index) {
            const Vec2 screenPoint = ScreenPointFromPagePoint(corners[index], pageView.rect, pageView.scale);
            polygon[index].x = static_cast<LONG>(std::lround(screenPoint.x));
            polygon[index].y = static_cast<LONG>(std::lround(screenPoint.y));
        }
        polygon[4] = polygon[0];
        HPEN selectionPen = CreatePen(PS_DASH, 2, RGB(110, 204, 255));
        HGDIOBJ oldImagePen = SelectObject(hdc, selectionPen);
        HGDIOBJ oldImageBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Polyline(hdc, polygon, 5);
        SelectObject(hdc, oldImageBrush);
        SelectObject(hdc, oldImagePen);
        DeleteObject(selectionPen);

        if (singleImage.has_value() && singleImage->first == pageView.pageIndex && singleImage->second == imageIndex
            && (activeTool_ == Tool::Move || isMovingSelection_ || isTransformingImage_)) {
            const int handleHalf = 4;
            const auto paintHandle = [&](int centerX, int centerY) {
                RECT handleRect = MakeRect(centerX - handleHalf, centerY - handleHalf, centerX + handleHalf + 1, centerY + handleHalf + 1);
                FillRoundedRect(hdc, handleRect, RGB(242, 247, 252));
                StrokeRoundedRect(hdc, handleRect, RGB(83, 112, 145), 1);
            };

            const Vec2 topMid {
                (corners[0].x + corners[1].x) * 0.5f,
                (corners[0].y + corners[1].y) * 0.5f,
            };
            const Vec2 bottomMid {
                (corners[2].x + corners[3].x) * 0.5f,
                (corners[2].y + corners[3].y) * 0.5f,
            };
            const Vec2 leftMid {
                (corners[0].x + corners[3].x) * 0.5f,
                (corners[0].y + corners[3].y) * 0.5f,
            };
            const Vec2 rightMid {
                (corners[1].x + corners[2].x) * 0.5f,
                (corners[1].y + corners[2].y) * 0.5f,
            };
            const Vec2 center = ImageCenter(image);
            const Vec2 rotatePagePoint = RotatePointAround({ center.x, image.y - 18.0f / pageView.scale }, center, image.rotationDegrees);
            const auto toScreenPoint = [&](Vec2 point) {
                return ScreenPointFromPagePoint(point, pageView.rect, pageView.scale);
            };
            const Vec2 nw = toScreenPoint(corners[0]);
            const Vec2 n = toScreenPoint(topMid);
            const Vec2 ne = toScreenPoint(corners[1]);
            const Vec2 e = toScreenPoint(rightMid);
            const Vec2 se = toScreenPoint(corners[2]);
            const Vec2 s = toScreenPoint(bottomMid);
            const Vec2 sw = toScreenPoint(corners[3]);
            const Vec2 w = toScreenPoint(leftMid);
            const Vec2 rotateScreenPoint = toScreenPoint(rotatePagePoint);

            paintHandle(static_cast<int>(std::lround(nw.x)), static_cast<int>(std::lround(nw.y)));
            paintHandle(static_cast<int>(std::lround(n.x)), static_cast<int>(std::lround(n.y)));
            paintHandle(static_cast<int>(std::lround(ne.x)), static_cast<int>(std::lround(ne.y)));
            paintHandle(static_cast<int>(std::lround(e.x)), static_cast<int>(std::lround(e.y)));
            paintHandle(static_cast<int>(std::lround(se.x)), static_cast<int>(std::lround(se.y)));
            paintHandle(static_cast<int>(std::lround(s.x)), static_cast<int>(std::lround(s.y)));
            paintHandle(static_cast<int>(std::lround(sw.x)), static_cast<int>(std::lround(sw.y)));
            paintHandle(static_cast<int>(std::lround(w.x)), static_cast<int>(std::lround(w.y)));

            HPEN stemPen = CreatePen(PS_SOLID, 1, RGB(110, 204, 255));
            HGDIOBJ oldStemPen = SelectObject(hdc, stemPen);
            MoveToEx(hdc, static_cast<int>(std::lround(n.x)), static_cast<int>(std::lround(n.y)), nullptr);
            LineTo(hdc, static_cast<int>(std::lround(rotateScreenPoint.x)), static_cast<int>(std::lround(rotateScreenPoint.y)));
            SelectObject(hdc, oldStemPen);
            DeleteObject(stemPen);
            paintHandle(static_cast<int>(std::lround(rotateScreenPoint.x)), static_cast<int>(std::lround(rotateScreenPoint.y)));
        }
    }

    for (std::size_t strokeIndex = 0; strokeIndex < page.strokes.size(); ++strokeIndex) {
        if (!IsStrokeSelected(pageView.pageIndex, strokeIndex)) {
            continue;
        }

        Stroke highlight = page.strokes[strokeIndex];
        highlight.style.color = 0xB06ECFFF;
        highlight.style.width += 4.0f;
        PaintStroke(hdc, highlight, pageView, false);
    }

    if (singleStrokePage.has_value() && *singleStrokePage == pageView.pageIndex
        && pageView.pageIndex < selectedStrokeIndices_.size() && !selectedStrokeIndices_[pageView.pageIndex].empty()
        && (activeTool_ == Tool::Move || isMovingSelection_ || isTransformingStroke_)) {
        float minX = page.width;
        float minY = page.height;
        float maxX = 0.0f;
        float maxY = 0.0f;
        for (std::size_t strokeIndex : selectedStrokeIndices_[pageView.pageIndex]) {
            if (strokeIndex >= page.strokes.size()) {
                continue;
            }
            for (const Vec2& point : page.strokes[strokeIndex].points) {
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
            }
        }

        const int left = pageView.rect.left + static_cast<int>(std::lround(minX * pageView.scale));
        const int top = pageView.rect.top + static_cast<int>(std::lround(minY * pageView.scale));
        const int right = pageView.rect.left + static_cast<int>(std::lround(maxX * pageView.scale));
        const int bottom = pageView.rect.top + static_cast<int>(std::lround(maxY * pageView.scale));
        HPEN selectionPen = CreatePen(PS_DASH, 2, RGB(110, 204, 255));
        HGDIOBJ oldStrokePen = SelectObject(hdc, selectionPen);
        HGDIOBJ oldStrokeBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, left, top, right, bottom);
        SelectObject(hdc, oldStrokeBrush);
        SelectObject(hdc, oldStrokePen);
        DeleteObject(selectionPen);

        const int handleHalf = 4;
        const auto paintHandle = [&](int centerX, int centerY) {
            RECT handleRect = MakeRect(centerX - handleHalf, centerY - handleHalf, centerX + handleHalf + 1, centerY + handleHalf + 1);
            FillRoundedRect(hdc, handleRect, RGB(242, 247, 252));
            StrokeRoundedRect(hdc, handleRect, RGB(83, 112, 145), 1);
        };

        const int centerX = (left + right) / 2;
        const int centerY = (top + bottom) / 2;
        paintHandle(left, top);
        paintHandle(centerX, top);
        paintHandle(right, top);
        paintHandle(right, centerY);
        paintHandle(right, bottom);
        paintHandle(centerX, bottom);
        paintHandle(left, bottom);
        paintHandle(left, centerY);

        const int rotateY = top - 18;
        HPEN stemPen = CreatePen(PS_SOLID, 1, RGB(110, 204, 255));
        HGDIOBJ oldStemPen = SelectObject(hdc, stemPen);
        MoveToEx(hdc, centerX, top, nullptr);
        LineTo(hdc, centerX, rotateY);
        SelectObject(hdc, oldStemPen);
        DeleteObject(stemPen);
        paintHandle(centerX, rotateY);
    }

    if (activeStroke_.has_value() && activeStrokePageIndex_.has_value() && *activeStrokePageIndex_ == pageView.pageIndex) {
        PaintStroke(hdc, *activeStroke_, pageView, true);
    }

    if (isSelecting_ && selectionPageIndex_.has_value() && *selectionPageIndex_ == pageView.pageIndex) {
        HPEN marqueePen = CreatePen(PS_DASH, 1, RGB(124, 211, 255));
        HGDIOBJ oldPenSelection = SelectObject(hdc, marqueePen);
        HGDIOBJ oldBrushSelection = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        if (selectionBoxOverride_ || selectionMode_ == SelectionMode::Box) {
            const int left = pageView.rect.left + static_cast<int>(std::lround(std::min(selectionAnchorPage_.x, selectionCurrentPage_.x) * pageView.scale));
            const int top = pageView.rect.top + static_cast<int>(std::lround(std::min(selectionAnchorPage_.y, selectionCurrentPage_.y) * pageView.scale));
            const int right = pageView.rect.left + static_cast<int>(std::lround(std::max(selectionAnchorPage_.x, selectionCurrentPage_.x) * pageView.scale));
            const int bottom = pageView.rect.top + static_cast<int>(std::lround(std::max(selectionAnchorPage_.y, selectionCurrentPage_.y) * pageView.scale));
            Rectangle(hdc, left, top, right, bottom);
        } else if (!selectionLassoPoints_.empty()) {
            std::vector<POINT> points;
            points.reserve(selectionLassoPoints_.size() + 1U);
            for (const Vec2& point : selectionLassoPoints_) {
                points.push_back({
                    pageView.rect.left + static_cast<LONG>(std::lround(point.x * pageView.scale)),
                    pageView.rect.top + static_cast<LONG>(std::lround(point.y * pageView.scale)),
                });
            }
            points.push_back({
                pageView.rect.left + static_cast<LONG>(std::lround(selectionCurrentPage_.x * pageView.scale)),
                pageView.rect.top + static_cast<LONG>(std::lround(selectionCurrentPage_.y * pageView.scale)),
            });
            if (points.size() >= 2U) {
                Polyline(hdc, points.data(), static_cast<int>(points.size()));
            }
        }

        SelectObject(hdc, oldBrushSelection);
        SelectObject(hdc, oldPenSelection);
        DeleteObject(marqueePen);
    }

    RestoreDC(hdc, -1);
}

void App::PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView, bool smoothLiveStroke) const {
    if (stroke.points.empty()) {
        return;
    }

    const std::vector<Vec2> smoothedPoints = smoothLiveStroke
        ? BuildSmoothedPoints(stroke.points, strokeSmoothing_)
        : stroke.points;
    const float penWidth = std::max(1.25f, stroke.style.width * pageView.scale / 2.15f);

    if (gdiplusToken_ != 0) {
        std::vector<Gdiplus::PointF> screenPoints;
        screenPoints.reserve(smoothedPoints.size());
        for (const Vec2& point : smoothedPoints) {
            screenPoints.emplace_back(
                static_cast<Gdiplus::REAL>(pageView.rect.left + (point.x * pageView.scale)),
                static_cast<Gdiplus::REAL>(pageView.rect.top + (point.y * pageView.scale)));
        }

        const auto alpha = static_cast<BYTE>((stroke.style.color >> 24U) & 0xFFU);
        const auto red = static_cast<BYTE>((stroke.style.color >> 16U) & 0xFFU);
        const auto green = static_cast<BYTE>((stroke.style.color >> 8U) & 0xFFU);
        const auto blue = static_cast<BYTE>(stroke.style.color & 0xFFU);

        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        Gdiplus::Color color(alpha, red, green, blue);
        Gdiplus::Pen pen(color, penWidth);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);

        if (screenPoints.size() == 1U) {
            Gdiplus::SolidBrush brush(color);
            const Gdiplus::REAL radius = penWidth * 0.5f;
            graphics.FillEllipse(
                &brush,
                screenPoints.front().X - radius,
                screenPoints.front().Y - radius,
                penWidth,
                penWidth);
        } else if (screenPoints.size() == 2U) {
            graphics.DrawLines(&pen, screenPoints.data(), static_cast<INT>(screenPoints.size()));
        } else {
            Gdiplus::GraphicsPath path;
            path.AddCurve(screenPoints.data(), static_cast<INT>(screenPoints.size()), 0.1f + (strokeSmoothing_ * 0.8f));
            graphics.DrawPath(&pen, &path);
        }

        return;
    }

    const int fallbackWidth = std::max(1, static_cast<int>(std::lround(penWidth)));
    HPEN pen = CreatePen(PS_SOLID, fallbackWidth, ToColorRef(stroke.style.color));
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    POINT first {
        pageView.rect.left + static_cast<LONG>(std::lround(smoothedPoints.front().x * pageView.scale)),
        pageView.rect.top + static_cast<LONG>(std::lround(smoothedPoints.front().y * pageView.scale)),
    };
    MoveToEx(hdc, first.x, first.y, nullptr);

    if (smoothedPoints.size() == 1U) {
        LineTo(hdc, first.x + 1, first.y + 1);
    } else {
        for (std::size_t index = 1; index < smoothedPoints.size(); ++index) {
            const POINT next {
                pageView.rect.left + static_cast<LONG>(std::lround(smoothedPoints[index].x * pageView.scale)),
                pageView.rect.top + static_cast<LONG>(std::lround(smoothedPoints[index].y * pageView.scale)),
            };
            LineTo(hdc, next.x, next.y);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (HandlePaletteClick(cursor)) {
            return 0;
        }
        if (HandleToolbarClick(cursor)) {
            return 0;
        }

        if (HandleSettingsClick(cursor)) {
            return 0;
        }

        ClosePaletteOverlays();

        if (activeTool_ == Tool::Pan) {
            BeginPan(cursor, false);
        } else if (activeTool_ == Tool::Move) {
            BeginMoveSelection(cursor);
        } else if (activeTool_ == Tool::Select) {
            BeginSelection(cursor);
        } else if (activeTool_ == Tool::Erase) {
            BeginErase(cursor);
        } else {
            BeginStroke(cursor);
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BeginPan(cursor, true);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (activeSlider_ != SliderTarget::None && (wParam & MK_LBUTTON) != 0) {
            UpdateActiveSlider(cursor);
        } else if (isSelecting_) {
            ExtendSelection(cursor);
        } else if (isTransformingStroke_) {
            UpdateStrokeTransform(cursor);
        } else if (isTransformingImage_) {
            UpdateImageTransform(cursor);
        } else if (isPanning_ || (wParam & MK_MBUTTON) != 0) {
            UpdatePan(cursor);
        } else if (isMovingSelection_ || ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Move)) {
            UpdateMoveSelection(cursor);
        } else if ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Select) {
            ExtendSelection(cursor);
        } else if (isErasing_ || ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Erase)) {
            ExtendErase(cursor);
        } else if ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Draw) {
            ExtendStroke(cursor);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (isDrawing_) {
            EndStroke();
        } else if (isTransformingStroke_) {
            EndStrokeTransform();
        } else if (isTransformingImage_) {
            EndImageTransform();
        } else if (isMovingSelection_) {
            EndMoveSelection();
        } else if (isSelecting_) {
            EndSelection();
        } else if (isErasing_) {
            isErasing_ = false;
            UpdateCaptureState();
            if (rasterRefreshPending_) {
                ScheduleDeferredRasterRefresh();
            }
        } else if (isPanning_ && !panFromMiddleButton_) {
            EndPan();
        }
        activeSlider_ = SliderTarget::None;
        return 0;
    case WM_MBUTTONUP:
        if (isPanning_ && panFromMiddleButton_) {
            EndPan();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window_, &cursor);
        ZoomAt(GET_WHEEL_DELTA_WPARAM(wParam), cursor);
        return 0;
    }
    case kRenderResultsMessage:
        HandleCompletedRenderResults();
        return 0;
    case WM_TIMER:
        if (wParam == kRasterRefreshTimerId) {
            HandleDeferredRasterRefresh();
            return 0;
        }
        if (wParam == kZoomSessionTimerId) {
            EndZoomSession();
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor {};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            const auto cursorForHandle = [](ImageTransformHandle handle) -> LPCWSTR {
                switch (handle) {
                case ImageTransformHandle::ResizeNorth:
                case ImageTransformHandle::ResizeSouth:
                    return IDC_SIZENS;
                case ImageTransformHandle::ResizeEast:
                case ImageTransformHandle::ResizeWest:
                    return IDC_SIZEWE;
                case ImageTransformHandle::ResizeNorthWest:
                case ImageTransformHandle::ResizeSouthEast:
                    return IDC_SIZENWSE;
                case ImageTransformHandle::ResizeNorthEast:
                case ImageTransformHandle::ResizeSouthWest:
                    return IDC_SIZENESW;
                case ImageTransformHandle::Rotate:
                    return IDC_HAND;
                case ImageTransformHandle::None:
                default:
                    return IDC_ARROW;
                }
            };
            if (cursor.y < kToolbarHeight) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (activeSlider_ != SliderTarget::None || IsCursorInsideSettingsPanel(cursor)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            } else if (isPanning_ || activeTool_ == Tool::Pan) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            } else if (isTransformingStroke_) {
                SetCursor(LoadCursorW(nullptr, cursorForHandle(strokeTransformHandle_)));
            } else if (isTransformingImage_) {
                SetCursor(LoadCursorW(nullptr, cursorForHandle(imageTransformHandle_)));
            } else if (isMovingSelection_) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            } else if (activeTool_ == Tool::Move) {
                if (const auto strokeHandle = HitTestSelectedStrokeHandle(cursor); strokeHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*strokeHandle)));
                } else if (const auto imageHandle = HitTestSelectedImageHandle(cursor); imageHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*imageHandle)));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
            } else if (activeTool_ == Tool::Select) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            } else if (activeTool_ == Tool::Erase) {
                SetCursor(LoadCursorW(nullptr, IDC_NO));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            }
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (wParam == 'Z') {
                if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                    Redo();
                } else {
                    Undo();
                }
                return 0;
            }
            if (wParam == 'Y') {
                Redo();
                return 0;
            }
            if (wParam == 'C') {
                static_cast<void>(CopySelectionToClipboard());
                return 0;
            }
            if (wParam == 'V') {
                if (!PasteSelectionFromClipboard()) {
                    static_cast<void>(PasteImageFromClipboard());
                }
                return 0;
            }
            if (wParam == 'N') {
                CreateBlankDocument();
                return 0;
            }
            if (wParam == 'O') {
                OpenDocument();
                return 0;
            }
            if (wParam == 'Q') {
                TogglePaletteWheel();
                return 0;
            }
            if (wParam == 'S') {
                static_cast<void>(SaveAsCnote());
                return 0;
            }
            if (wParam == 'B') {
                ToggleBrushSettings();
                return 0;
            }
        }

        if (wParam == VK_DELETE) {
            DeleteSelection();
            return 0;
        }

        if (wParam == VK_ESCAPE && (paletteMenuOpen_ || paletteWheelOpen_)) {
            ClosePaletteOverlays();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        switch (wParam) {
        case VK_PRIOR:
            PreviousPage();
            return 0;
        case VK_NEXT:
            NextPage();
            return 0;
        case VK_LEFT:
            PanBy(20.0f, 0.0f);
            return 0;
        case VK_RIGHT:
            PanBy(-20.0f, 0.0f);
            return 0;
        case VK_UP:
            PanBy(0.0f, 20.0f);
            return 0;
        case VK_DOWN:
            PanBy(0.0f, -20.0f);
            return 0;
        case VK_OEM_4:
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            ChangeBrushSize(-1.0f);
            return 0;
        case VK_OEM_6:
        case VK_ADD:
        case VK_OEM_PLUS:
            ChangeBrushSize(1.0f);
            return 0;
        case '0':
            CancelDeferredRasterRefresh();
            ResetView();
            EnsureVisiblePageRasterQuality();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
            SetColorIndex(static_cast<std::size_t>(wParam - '1'));
            return 0;
        case 'C':
            CycleColor();
            return 0;
        default:
            break;
        }
        break;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        CancelDeferredRasterRefresh();
        KillTimer(window_, kZoomSessionTimerId);
        zoomSessionActive_ = false;
        zoomAnchorValid_ = false;
        targetZoom_ = zoom_;
        InvalidateAllPageSurfaceCaches();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        CancelDeferredRasterRefresh();
        KillTimer(window_, kZoomSessionTimerId);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window_ = window;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (app != nullptr) {
        return app->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace cnote
