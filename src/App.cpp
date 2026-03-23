#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>

#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

namespace ctrainer {

namespace {

constexpr wchar_t kWindowClassName[] = L"CTrainerPrototypeWindow";
constexpr wchar_t kWindowTitle[] = L"ctrainer - PDF Drawing Prototype";

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

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points) {
    if (points.size() <= 2U) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * 0.2f) + (current.x * 0.6f) + (next.x * 0.2f),
            (previous.y * 0.2f) + (current.y * 0.6f) + (next.y * 0.2f),
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

}  // namespace

App::App(HINSTANCE instance) : instance_(instance) {
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) == Gdiplus::Ok) {
        gdiplusToken_ = token;
    }
}

App::~App() {
    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
    }
}

int App::Run(int showCommand) {
    if (!CreateMainWindow(showCommand)) {
        return 1;
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
    windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
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

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

void App::CreateBlankDocument() {
    document_.ResetToBlank();
    brushWidth_ = 3.0f;
    ResetView();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ResetView() {
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
}

void App::BeginStroke(POINT cursor) {
    if (isPanning_ || !IsCursorInsidePage(cursor)) {
        return;
    }

    activeStroke_ = Stroke {};
    activeStroke_->style.color = palette_.at(colorIndex_);
    activeStroke_->style.width = brushWidth_;
    activeStroke_->points.push_back(ScreenToPage(cursor));
    isDrawing_ = true;
    UpdateCaptureState();
}

void App::ExtendStroke(POINT cursor) {
    if (!isDrawing_ || isPanning_ || !activeStroke_.has_value()) {
        return;
    }

    const Vec2 rawPoint = ScreenToPage(cursor);
    Vec2 filteredPoint = rawPoint;

    if (!activeStroke_->points.empty()) {
        filteredPoint = Lerp(activeStroke_->points.back(), rawPoint, kInputSmoothing);
        if (DistanceSquared(filteredPoint, activeStroke_->points.back()) < kInputSpacing) {
            return;
        }
    }

    activeStroke_->points.push_back(filteredPoint);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::EndStroke() {
    if (isDrawing_ && activeStroke_.has_value() && !activeStroke_->points.empty()) {
        Stroke finishedStroke = std::move(*activeStroke_);
        finishedStroke.points = BuildSmoothedPoints(finishedStroke.points);
        document_.CurrentPage().strokes.push_back(std::move(finishedStroke));
    }

    activeStroke_.reset();
    isDrawing_ = false;
    UpdateCaptureState();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::BeginPan(POINT cursor) {
    if (isDrawing_) {
        EndStroke();
    }

    isPanning_ = true;
    lastPanCursor_ = cursor;
    UpdateCaptureState();
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
    UpdateCaptureState();
}

void App::UpdateCaptureState() {
    if (isDrawing_ || isPanning_) {
        SetCapture(window_);
        return;
    }

    if (GetCapture() == window_) {
        ReleaseCapture();
    }
}

void App::PanBy(float dx, float dy) {
    panX_ += dx;
    panY_ += dy;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ZoomAt(int delta, POINT cursor) {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView before = CalculatePageView(clientRect);
    if (!before.valid) {
        return;
    }

    const bool cursorOnPage = IsCursorInsidePage(cursor);
    const Vec2 anchorPage = cursorOnPage ? ScreenToPage(cursor) : Vec2 { before.scale, before.scale };
    const float factor = delta > 0 ? 1.1f : 0.9f;
    const float newZoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);

    if (std::abs(newZoom - zoom_) < 0.0001f) {
        return;
    }

    zoom_ = newZoom;

    const PageView after = CalculatePageView(clientRect);
    if (cursorOnPage && after.valid) {
        const POINT anchoredScreen = PageToScreen(anchorPage);
        panX_ += static_cast<float>(cursor.x - anchoredScreen.x);
        panY_ += static_cast<float>(cursor.y - anchoredScreen.y);
    }

    InvalidateRect(window_, nullptr, FALSE);
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
    InvalidateRect(window_, nullptr, FALSE);
}

void App::OpenPdf() {
    wchar_t fileBuffer[MAX_PATH] = {};
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    dialog.lpstrFile = fileBuffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"pdf";

    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    const auto opened = pdfBackend_.Open(fileBuffer);
    if (!opened.has_value()) {
        MessageBoxW(window_, L"Failed to open PDF placeholder session.", L"Open PDF", MB_OK | MB_ICONERROR);
        return;
    }

    document_ = *opened;
    ResetView();
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::IsCursorInsidePage(POINT cursor) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect);
    if (!pageView.valid) {
        return false;
    }

    return PtInRect(&pageView.rect, cursor) != 0;
}

Vec2 App::ScreenToPage(POINT cursor) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect);
    const Page& page = document_.CurrentPage();
    if (!pageView.valid || pageView.scale <= 0.0f) {
        return {};
    }

    const float pageX = (static_cast<float>(cursor.x - pageView.rect.left)) / pageView.scale;
    const float pageY = (static_cast<float>(cursor.y - pageView.rect.top)) / pageView.scale;
    return {
        std::clamp(pageX, 0.0f, page.width),
        std::clamp(pageY, 0.0f, page.height),
    };
}

POINT App::PageToScreen(Vec2 point) const {
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect);
    POINT output {};
    output.x = pageView.rect.left + static_cast<LONG>(std::lround(point.x * pageView.scale));
    output.y = pageView.rect.top + static_cast<LONG>(std::lround(point.y * pageView.scale));
    return output;
}

App::PageView App::CalculatePageView(RECT clientRect) const {
    const Page& page = document_.CurrentPage();
    const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
    const int clientHeight = static_cast<int>(clientRect.bottom - clientRect.top);
    const int canvasWidth = std::max(1, clientWidth);
    const int canvasHeight = std::max(1, clientHeight - kToolbarHeight);
    const float fitScale = std::min(
        static_cast<float>(canvasWidth - 120) / page.width,
        static_cast<float>(canvasHeight - 80) / page.height);
    const float scale = std::max(0.1f, fitScale * zoom_);

    const int pageWidth = static_cast<int>(std::lround(page.width * scale));
    const int pageHeight = static_cast<int>(std::lround(page.height * scale));
    const int canvasCenterX = canvasWidth / 2;
    const int canvasCenterY = kToolbarHeight + (canvasHeight / 2);
    const int left = static_cast<int>(std::lround(canvasCenterX - (pageWidth / 2.0f) + panX_));
    const int top = static_cast<int>(std::lround(canvasCenterY - (pageHeight / 2.0f) + panY_));

    return {
        MakeRect(left, top, left + pageWidth, top + pageHeight),
        scale,
        pageWidth > 0 && pageHeight > 0,
    };
}

COLORREF App::ToColorRef(std::uint32_t argb) const {
    const auto red = static_cast<unsigned char>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<unsigned char>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<unsigned char>(argb & 0xFFU);
    return RGB(red, green, blue);
}

std::wstring App::BuildStatusLine() const {
    wchar_t buffer[512] = {};
    const wchar_t* mode = document_.IsPdfPlaceholder() ? L"PDF session" : L"Blank page";
    const std::wstring fileName = document_.SourcePath().empty() ? L"(unsaved)" : BaseName(document_.SourcePath());
    const wchar_t* activeTool = isPanning_ ? L"pan" : L"draw";
    std::swprintf(
        buffer,
        std::size(buffer),
        L"%ls | file: %ls | tool: %ls | zoom: %.0f%% | brush: %.1f px | strokes: %zu",
        mode,
        fileName.c_str(),
        activeTool,
        zoom_ * 100.0f,
        brushWidth_,
        document_.CurrentPage().strokes.size());
    return buffer;
}

void App::Paint() {
    PAINTSTRUCT paint {};
    HDC windowDc = BeginPaint(window_, &paint);

    RECT clientRect {};
    GetClientRect(window_, &clientRect);

    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, clientRect.right, clientRect.bottom);
    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(236, 239, 244));
    FillRect(memoryDc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    PaintToolbar(memoryDc, clientRect);
    PaintCanvas(memoryDc, clientRect);

    BitBlt(windowDc, 0, 0, clientRect.right, clientRect.bottom, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);

    EndPaint(window_, &paint);
}

void App::PaintToolbar(HDC hdc, RECT clientRect) const {
    RECT toolbarRect = clientRect;
    toolbarRect.bottom = kToolbarHeight;

    HBRUSH toolbarBrush = CreateSolidBrush(RGB(23, 37, 84));
    FillRect(hdc, &toolbarRect, toolbarBrush);
    DeleteObject(toolbarBrush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(248, 250, 252));

    HFONT titleFont = CreateFontW(28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT bodyFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    HGDIOBJ previousFont = SelectObject(hdc, titleFont);
    TextOutW(hdc, 20, 12, L"ctrainer", 8);

    SelectObject(hdc, bodyFont);
    const std::wstring status = BuildStatusLine();
    TextOutW(hdc, 22, 42, status.c_str(), static_cast<int>(status.size()));

    const wchar_t* controls = L"Ctrl+O open PDF | Ctrl+N new | middle-drag pan | wheel zoom | [ ] brush | 1-5 colors";
    TextOutW(hdc, 700, 42, controls, static_cast<int>(std::wcslen(controls)));

    SelectObject(hdc, previousFont);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
}

void App::PaintCanvas(HDC hdc, RECT clientRect) const {
    RECT canvasRect = clientRect;
    canvasRect.top = kToolbarHeight;

    HBRUSH canvasBrush = CreateSolidBrush(RGB(226, 232, 240));
    FillRect(hdc, &canvasRect, canvasBrush);
    DeleteObject(canvasBrush);

    const PageView pageView = CalculatePageView(clientRect);
    if (!pageView.valid) {
        return;
    }

    PaintPage(hdc, pageView);
}

void App::PaintPage(HDC hdc, const PageView& pageView) const {
    RECT shadow = pageView.rect;
    OffsetRect(&shadow, 8, 10);
    HBRUSH shadowBrush = CreateSolidBrush(RGB(184, 196, 210));
    FillRect(hdc, &shadow, shadowBrush);
    DeleteObject(shadowBrush);

    HBRUSH pageBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &pageView.rect, pageBrush);
    DeleteObject(pageBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(148, 163, 184));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    SaveDC(hdc);
    IntersectClipRect(hdc, pageView.rect.left, pageView.rect.top, pageView.rect.right, pageView.rect.bottom);

    if (document_.IsPdfPlaceholder()) {
        PaintPlaceholderContent(hdc, pageView);
    }

    for (const Stroke& stroke : document_.CurrentPage().strokes) {
        PaintStroke(hdc, stroke, pageView);
    }

    if (activeStroke_.has_value()) {
        PaintStroke(hdc, *activeStroke_, pageView);
    }

    RestoreDC(hdc, -1);
}

void App::PaintPlaceholderContent(HDC hdc, const PageView& pageView) const {
    RECT banner = pageView.rect;
    banner.left += 18;
    banner.top += 18;
    banner.right -= 18;
    banner.bottom = banner.top + 110;

    HBRUSH bannerBrush = CreateSolidBrush(RGB(239, 246, 255));
    FillRect(hdc, &banner, bannerBrush);
    DeleteObject(bannerBrush);

    HPEN framePen = CreatePen(PS_SOLID, 1, RGB(96, 165, 250));
    HGDIOBJ oldPen = SelectObject(hdc, framePen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, banner.left, banner.top, banner.right, banner.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(framePen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(30, 41, 59));

    HFONT titleFont = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT bodyFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    HGDIOBJ previousFont = SelectObject(hdc, titleFont);
    const wchar_t* headline = L"PDF backend placeholder";
    TextOutW(hdc, banner.left + 18, banner.top + 18, headline, static_cast<int>(std::wcslen(headline)));

    SelectObject(hdc, bodyFont);
    const std::wstring openedFile = L"Opened: " + BaseName(document_.SourcePath());
    const wchar_t* detail = L"This page accepts ink annotations now. Real PDF raster/render support plugs into IPdfBackend next.";
    TextOutW(hdc, banner.left + 18, banner.top + 50, openedFile.c_str(), static_cast<int>(openedFile.size()));
    TextOutW(hdc, banner.left + 18, banner.top + 76, detail, static_cast<int>(std::wcslen(detail)));

    SelectObject(hdc, previousFont);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
}

void App::PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView) const {
    if (stroke.points.empty()) {
        return;
    }

    const std::vector<Vec2> smoothedPoints = BuildSmoothedPoints(stroke.points);
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
            path.AddCurve(screenPoints.data(), static_cast<INT>(screenPoints.size()), 0.5f);
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
        BeginStroke(cursor);
        return 0;
    }
    case WM_MBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BeginPan(cursor);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (isPanning_ || (wParam & MK_MBUTTON) != 0) {
            UpdatePan(cursor);
        } else if ((wParam & MK_LBUTTON) != 0) {
            ExtendStroke(cursor);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        EndStroke();
        return 0;
    case WM_MBUTTONUP:
        EndPan();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window_, &cursor);
        ZoomAt(GET_WHEEL_DELTA_WPARAM(wParam), cursor);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, isPanning_ ? IDC_SIZEALL : IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (wParam == 'N') {
                CreateBlankDocument();
                return 0;
            }
            if (wParam == 'O') {
                OpenPdf();
                return 0;
            }
        }

        switch (wParam) {
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
            ResetView();
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
    case WM_DESTROY:
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

}  // namespace ctrainer
