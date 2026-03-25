#include "App.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include <commdlg.h>

#include "NewPageSettingsWindow.h"
#include "SettingsWindow.h"
#include "ShortcutConfig.h"

namespace cnote {

namespace {

std::filesystem::path AppSettingsPath() {
    const wchar_t* appData = _wgetenv(L"APPDATA");
    if (appData != nullptr && *appData != L'\0') {
        return std::filesystem::path(appData) / L"cnote" / L"settings.ini";
    }
    return std::filesystem::current_path() / L"cnote.settings.ini";
}

const std::vector<std::uint32_t>& DefaultPalette() {
    static const std::vector<std::uint32_t> palette {
        0xFF000000,
        0xFF2563EB,
        0xFFDC2626,
        0xFF059669,
        0xFFF59E0B,
    };
    return palette;
}

const std::vector<std::uint32_t>& LegacyDefaultPalette() {
    static const std::vector<std::uint32_t> palette {
        0xFF1F2937,
        0xFF2563EB,
        0xFFDC2626,
        0xFF059669,
        0xFFF59E0B,
    };
    return palette;
}

std::string TrimAscii(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::vector<std::string> SplitAscii(std::string_view value, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.push_back(TrimAscii(value.substr(start)));
            break;
        }
        parts.push_back(TrimAscii(value.substr(start, end - start)));
        start = end + 1U;
    }
    return parts;
}

std::optional<std::uint32_t> ParseHexColor(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }), value.end());

    if (!value.empty() && value.front() == L'#') {
        value.erase(value.begin());
    }
    if (value.rfind(L"0x", 0) == 0 || value.rfind(L"0X", 0) == 0) {
        value.erase(0, 2);
    }

    if (value.size() != 6U && value.size() != 8U) {
        return std::nullopt;
    }
    if (!std::all_of(value.begin(), value.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; })) {
        return std::nullopt;
    }

    try {
        const std::uint32_t parsed = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        if (value.size() == 6U) {
            return 0xFF000000U | parsed;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<unsigned int> ParseUnsignedInt(std::string_view value, int base = 10) {
    const std::string trimmed = TrimAscii(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(trimmed, &consumed, base);
        if (consumed != trimmed.size()) {
            return std::nullopt;
        }
        return static_cast<unsigned int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<float> ParseFloat(std::string_view value) {
    const std::string trimmed = TrimAscii(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(trimmed, &consumed);
        if (consumed != trimmed.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

const char* ToolConfigName(App::Tool tool) {
    switch (tool) {
    case App::Tool::Select:
        return "Select";
    case App::Tool::Move:
        return "Move";
    case App::Tool::Draw:
        return "Draw";
    case App::Tool::Pan:
        return "Pan";
    case App::Tool::Erase:
        return "Erase";
    default:
        return "";
    }
}

std::optional<App::Tool> ParseToolConfigName(std::string_view name) {
    if (name == "Select") {
        return App::Tool::Select;
    }
    if (name == "Move") {
        return App::Tool::Move;
    }
    if (name == "Draw") {
        return App::Tool::Draw;
    }
    if (name == "Pan") {
        return App::Tool::Pan;
    }
    if (name == "Erase") {
        return App::Tool::Erase;
    }
    return std::nullopt;
}

bool HasExtension(const std::wstring& path, const wchar_t* extension) {
    const std::filesystem::path filePath(path);
    return filePath.has_extension() && _wcsicmp(filePath.extension().c_str(), extension) == 0;
}

RasterImage CreateSolidRaster(float pageWidth, float pageHeight, float renderScale, std::uint32_t argb) {
    const int bitmapWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageWidth) * renderScale)));
    const int bitmapHeight = std::max(1, static_cast<int>(std::lround(std::max(1.0f, pageHeight) * renderScale)));
    const int stride = bitmapWidth * 4;

    RasterImage raster {};
    raster.width = bitmapWidth;
    raster.height = bitmapHeight;
    raster.stride = stride;
    raster.scale = renderScale;
    raster.pixels.resize(static_cast<std::size_t>(stride) * static_cast<std::size_t>(bitmapHeight));

    const auto alpha = static_cast<std::uint8_t>((argb >> 24U) & 0xFFU);
    const auto red = static_cast<std::uint8_t>((argb >> 16U) & 0xFFU);
    const auto green = static_cast<std::uint8_t>((argb >> 8U) & 0xFFU);
    const auto blue = static_cast<std::uint8_t>(argb & 0xFFU);
    for (std::size_t offset = 0; offset + 3U < raster.pixels.size(); offset += 4U) {
        raster.pixels[offset] = blue;
        raster.pixels[offset + 1U] = green;
        raster.pixels[offset + 2U] = red;
        raster.pixels[offset + 3U] = alpha;
    }

    return raster;
}

void FlattenImportedPage(Page& page) {
    page.sourcePdfPageIndex = -1;
}

}  // namespace

void App::ToggleBrushSettings() {
    if (settings_.window == nullptr) {
        settings_.window = new SettingsWindow(*this);
    }
    settings_.window->Toggle(window_);
}

void App::ToggleNewPageSettings() {
    if (newPageSettings_.window == nullptr) {
        newPageSettings_.window = new NewPageSettingsWindow(*this);
    }
    newPageSettings_.window->Toggle(window_);
}

void App::TogglePdfSidebar() {
    if (document_.PageCount() == 0U) {
        pdfSidebarOpen_ = false;
        pdfSidebarScrollY_ = 0;
        CancelPageJumpInput();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    pdfSidebarOpen_ = !pdfSidebarOpen_;
    if (pdfSidebarOpen_) {
        EnsureSidebarCurrentPageVisible();
    } else {
        pdfSidebarScrollY_ = 0;
        CancelPageJumpInput();
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::RefreshSettingsWindow() {
    if (settings_.window && settings_.window->IsOpen()) {
        settings_.window->Refresh();
    }
}

bool App::IsSettingsWindowOpen() const {
    return settings_.window && settings_.window->IsOpen();
}

void App::RefreshNewPageSettingsWindow() {
    if (newPageSettings_.window && newPageSettings_.window->IsOpen()) {
        newPageSettings_.window->Refresh();
    }
}

void App::ResetNewPageSettings() {
    newPageSettings_.width = 612.0f;
    newPageSettings_.height = 792.0f;
    newPageSettings_.backgroundColor = 0xFFFFFFFFU;
    RefreshNewPageSettingsWindow();
}

Page App::BuildConfiguredNewPage() const {
    Page page {};
    page.width = std::max(72.0f, newPageSettings_.width);
    page.height = std::max(72.0f, newPageSettings_.height);
    page.raster = CreateSolidRaster(page.width, page.height, 1.5f, newPageSettings_.backgroundColor);
    return page;
}

void App::ApplyConfiguredSettingsToCurrentPage(float width, float height, std::uint32_t backgroundColor) {
    if (document_.PageCount() == 0U) {
        return;
    }

    ResetPendingRasterWork();

    Page& page = document_.CurrentPage();
    page.width = std::max(72.0f, width);
    page.height = std::max(72.0f, height);
    page.sourcePdfPageIndex = -1;
    page.raster = CreateSolidRaster(page.width, page.height, 1.5f, backgroundColor);

    const std::size_t pageIndex = document_.CurrentPageIndex();
    InvalidatePageSurfaceCache(pageIndex);
    InvalidateSidebarThumbnailCache(pageIndex);
    EnsureVisiblePageRasterQuality();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ApplyConfiguredSettingsToAllPages(float width, float height, std::uint32_t backgroundColor) {
    if (document_.PageCount() == 0U) {
        return;
    }

    ResetPendingRasterWork();

    const float clampedWidth = std::max(72.0f, width);
    const float clampedHeight = std::max(72.0f, height);
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        Page& page = document_.PageAt(pageIndex);
        page.width = clampedWidth;
        page.height = clampedHeight;
        page.sourcePdfPageIndex = -1;
        page.raster = CreateSolidRaster(page.width, page.height, 1.5f, backgroundColor);
    }

    pageSurfaceCaches_.assign(document_.PageCount(), {});
    sidebarThumbnailCaches_.assign(document_.PageCount(), {});
    EnsureVisiblePageRasterQuality();
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::AppendPagesFromFile() {
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
        return false;
    }

    return AppendPagesFromDocumentPath(fileBuffer);
}

bool App::AppendPagesFromDocumentPath(const std::wstring& filePath) {
    std::optional<Document> opened;
    if (HasExtension(filePath, kCnoteFileExtension)) {
        opened = ProjectFile::Load(filePath, pdfBackend_);
        if (!opened.has_value()) {
            MessageBoxW(window_, L"Failed to open the cnote file for page import.", L"Append Pages", MB_OK | MB_ICONERROR);
            return false;
        }
    } else if (HasExtension(filePath, L".pdf")) {
        opened = pdfBackend_.Open(filePath);
        if (!opened.has_value()) {
            MessageBoxW(window_, L"Failed to open or render the PDF for page import.", L"Append Pages", MB_OK | MB_ICONERROR);
            return false;
        }
    } else {
        MessageBoxW(window_, L"Unsupported file type. Choose a PDF or a .cnote file.", L"Append Pages", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!opened.has_value() || opened->PageCount() == 0U) {
        MessageBoxW(window_, L"The selected file did not contain any pages to append.", L"Append Pages", MB_OK | MB_ICONERROR);
        return false;
    }

    ResetPendingRasterWork();
    ClearSelection();
    ClearHistory();

    const std::size_t insertIndex = document_.PageCount();
    for (std::size_t pageIndex = 0; pageIndex < opened->PageCount(); ++pageIndex) {
        Page importedPage = opened->PageAt(pageIndex);
        FlattenImportedPage(importedPage);
        document_.InsertPage(document_.PageCount(), std::move(importedPage));
    }

    pageSurfaceCaches_.assign(document_.PageCount(), {});
    sidebarThumbnailCaches_.assign(document_.PageCount(), {});
    document_.SetCurrentPageIndex(insertIndex);
    EnsureSidebarCurrentPageVisible();
    EnsureVisiblePageRasterQuality();
    FocusPage(insertIndex);
    return true;
}

void App::AddPaletteColor() {
    if (palette_.size() >= 12U) {
        return;
    }

    const std::uint32_t sourceColor = palette_.at(settings_.paletteIndex);
    palette_.push_back(sourceColor);
    settings_.paletteIndex = palette_.size() - 1U;
    colorIndex_ = std::min(colorIndex_, palette_.size() - 1U);
    SaveAppSettings();
    RefreshSettingsWindow();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::RemovePaletteColor() {
    if (palette_.size() <= 1U || settings_.paletteIndex >= palette_.size()) {
        return;
    }

    palette_.erase(palette_.begin() + static_cast<std::ptrdiff_t>(settings_.paletteIndex));
    if (settings_.paletteIndex >= palette_.size()) {
        settings_.paletteIndex = palette_.size() - 1U;
    }
    if (colorIndex_ >= palette_.size()) {
        colorIndex_ = palette_.size() - 1U;
    }
    SaveAppSettings();
    RefreshSettingsWindow();
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
            if (const auto parsed = ParseHexColor(rawText); parsed.has_value() && settings_.paletteIndex < palette_.size()) {
                palette_[settings_.paletteIndex] = *parsed;
                applied = true;
            }
            GlobalUnlock(clipboardData);
        }
    }

    CloseClipboard();
    if (applied) {
        SaveAppSettings();
        RefreshSettingsWindow();
        InvalidateRect(window_, nullptr, FALSE);
    }
    return applied;
}

void App::LoadAppSettings() {
    std::ifstream input(AppSettingsPath(), std::ios::binary);
    if (!input) {
        return;
    }

    std::optional<std::size_t> loadedColorIndex;
    std::optional<std::size_t> loadedSettingsPaletteIndex;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }

        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = TrimAscii(std::string_view(trimmed).substr(0, separator));
        const std::string value = TrimAscii(std::string_view(trimmed).substr(separator + 1U));

        if (key == "palette") {
            std::vector<std::uint32_t> loadedPalette;
            for (const std::string& token : SplitAscii(value, ',')) {
                const auto color = ParseUnsignedInt(token, 16);
                if (!color.has_value()) {
                    loadedPalette.clear();
                    break;
                }
                loadedPalette.push_back(*color);
                if (loadedPalette.size() >= 12U) {
                    break;
                }
            }
            if (!loadedPalette.empty()) {
                palette_ = std::move(loadedPalette);
            }
            continue;
        }

        if (key == "active_color") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                loadedColorIndex = static_cast<std::size_t>(*parsed);
            }
            continue;
        }

        if (key == "settings_palette_index") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                loadedSettingsPaletteIndex = static_cast<std::size_t>(*parsed);
            }
            continue;
        }

        if (key == "input_smoothing") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.inputSmoothing = std::clamp(*parsed, 0.05f, 0.95f);
            }
            continue;
        }

        if (key == "draw_brush_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.drawBrushSize = std::clamp(*parsed, 1.0f, 20.0f);
            }
            continue;
        }

        if (key == "erase_brush_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.eraseBrushSize = std::clamp(*parsed, 1.0f, 20.0f);
            }
            continue;
        }

        if (key == "input_spacing") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.inputSpacing = std::clamp(*parsed, 0.05f, 4.05f);
            }
            continue;
        }

        if (key == "stroke_smoothing") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.strokeSmoothing = std::clamp(*parsed, 0.0f, 1.0f);
            }
            continue;
        }

        if (key == "wheel_tools") {
            std::vector<Tool> loadedTools;
            for (const std::string& token : SplitAscii(value, ',')) {
                const auto tool = ParseToolConfigName(token);
                if (!tool.has_value()) {
                    continue;
                }
                if (std::find(loadedTools.begin(), loadedTools.end(), *tool) == loadedTools.end()) {
                    loadedTools.push_back(*tool);
                }
            }
            wheel_.tools = std::move(loadedTools);
            continue;
        }

        if (key.starts_with("shortcut.")) {
            const auto action = ParseShortcutActionConfigName(std::string_view(key).substr(9));
            if (!action.has_value()) {
                continue;
            }

            const std::vector<std::string> parts = SplitAscii(value, ',');
            if (parts.size() != 4U) {
                continue;
            }

            const auto parsedKey = ParseUnsignedInt(parts[0]);
            const auto parsedCtrl = ParseUnsignedInt(parts[1]);
            const auto parsedShift = ParseUnsignedInt(parts[2]);
            const auto parsedAlt = ParseUnsignedInt(parts[3]);
            if (!parsedKey.has_value() || !parsedCtrl.has_value() || !parsedShift.has_value() || !parsedAlt.has_value()) {
                continue;
            }

            const auto bindingIt = std::find_if(shortcuts_.bindings.begin(), shortcuts_.bindings.end(), [&](auto& binding) {
                return binding.first == *action;
            });
            if (bindingIt != shortcuts_.bindings.end()) {
                bindingIt->second = {
                    *parsedKey,
                    *parsedCtrl != 0U,
                    *parsedShift != 0U,
                    *parsedAlt != 0U,
                };
            }
        }
    }

    if (palette_ == LegacyDefaultPalette()) {
        palette_ = DefaultPalette();
    }
    if (palette_.empty()) {
        palette_ = DefaultPalette();
    }
    colorIndex_ = std::min(loadedColorIndex.value_or(colorIndex_), palette_.size() - 1U);
    settings_.paletteIndex = std::min(loadedSettingsPaletteIndex.value_or(settings_.paletteIndex), palette_.size() - 1U);
    SyncBrushWidthFromActiveTool();
}

void App::SaveAppSettings() const {
    if (palette_.empty()) {
        return;
    }

    const std::filesystem::path path = AppSettingsPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }

    output << "palette=";
    for (std::size_t index = 0; index < palette_.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }
        std::ostringstream colorValue;
        colorValue << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << palette_[index];
        output << colorValue.str();
    }
    output << '\n';
    output << "active_color=" << colorIndex_ << '\n';
    output << "settings_palette_index=" << settings_.paletteIndex << '\n';
    output << "draw_brush_size=" << settings_.drawBrushSize << '\n';
    output << "erase_brush_size=" << settings_.eraseBrushSize << '\n';
    output << "input_smoothing=" << settings_.inputSmoothing << '\n';
    output << "input_spacing=" << settings_.inputSpacing << '\n';
    output << "stroke_smoothing=" << settings_.strokeSmoothing << '\n';

    output << "wheel_tools=";
    for (std::size_t index = 0; index < wheel_.tools.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }
        output << ToolConfigName(wheel_.tools[index]);
    }
    output << '\n';

    for (const auto& shortcut : shortcuts_.bindings) {
        output << "shortcut." << ShortcutActionConfigName(shortcut.first) << '=';
        output << shortcut.second.key << ','
               << (shortcut.second.ctrl ? 1 : 0) << ','
               << (shortcut.second.shift ? 1 : 0) << ','
               << (shortcut.second.alt ? 1 : 0) << '\n';
    }
}

std::wstring App::FormatKeyBinding(const KeyBinding& binding) const {
    if (binding.key == 0U) {
        return L"Unbound";
    }

    std::wstring result;
    if (binding.ctrl) {
        result += L"Ctrl+";
    }
    if (binding.shift) {
        result += L"Shift+";
    }
    if (binding.alt) {
        result += L"Alt+";
    }

    auto appendKeyName = [&](const wchar_t* name) {
        result += name;
    };

    if ((binding.key >= 'A' && binding.key <= 'Z') || (binding.key >= '0' && binding.key <= '9')) {
        result.push_back(static_cast<wchar_t>(binding.key));
        return result;
    }

    switch (binding.key) {
    case VK_PRIOR:
        appendKeyName(L"PageUp");
        return result;
    case VK_NEXT:
        appendKeyName(L"PageDown");
        return result;
    case VK_LEFT:
        appendKeyName(L"Left");
        return result;
    case VK_RIGHT:
        appendKeyName(L"Right");
        return result;
    case VK_UP:
        appendKeyName(L"Up");
        return result;
    case VK_DOWN:
        appendKeyName(L"Down");
        return result;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        appendKeyName(L"-");
        return result;
    case VK_OEM_PLUS:
    case VK_ADD:
        appendKeyName(L"+");
        return result;
    default:
        break;
    }

    UINT scanCode = MapVirtualKeyW(binding.key, MAPVK_VK_TO_VSC) << 16;
    wchar_t buffer[64] = {};
    if (GetKeyNameTextW(static_cast<LONG>(scanCode), buffer, static_cast<int>(std::size(buffer))) > 0) {
        result += buffer;
    } else {
        result += L"Key";
    }
    return result;
}

void App::ExecuteShortcutAction(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToolDraw:
        SetActiveTool(Tool::Draw);
        break;
    case ShortcutAction::ToolSelect:
        selectionMode_ = SelectionMode::Lasso;
        SetActiveTool(Tool::Select);
        break;
    case ShortcutAction::ToolMove:
        SetActiveTool(Tool::Move);
        break;
    case ShortcutAction::ToolPan:
        SetActiveTool(Tool::Pan);
        break;
    case ShortcutAction::ToolErase:
        SetActiveTool(Tool::Erase);
        break;
    case ShortcutAction::New:
        CreateBlankDocument();
        break;
    case ShortcutAction::Undo:
        Undo();
        break;
    case ShortcutAction::Redo:
        Redo();
        break;
    case ShortcutAction::Open:
        OpenDocument();
        break;
    case ShortcutAction::Save:
        static_cast<void>(SaveAsCnote());
        break;
    case ShortcutAction::ExportPdf:
        static_cast<void>(ExportAsPdf());
        break;
    case ShortcutAction::Copy:
        if (SidebarOwnsKeyboardShortcuts()) {
            CopyCurrentPageToSidebarBuffer();
            InvalidateRect(window_, nullptr, FALSE);
        } else {
            static_cast<void>(CopySelectionToClipboard());
        }
        break;
    case ShortcutAction::Paste:
        if (SidebarOwnsKeyboardShortcuts()) {
            static_cast<void>(PasteSidebarPageAfterCurrent());
        } else if (!PasteSelectionFromClipboard()) {
            static_cast<void>(PasteImageFromClipboard());
        }
        break;
    case ShortcutAction::ResetView:
        CancelDeferredRasterRefresh();
        ResetView();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        break;
    case ShortcutAction::ToggleSettings:
        ToggleBrushSettings();
        break;
    case ShortcutAction::TogglePdfSidebar:
        TogglePdfSidebar();
        break;
    case ShortcutAction::ToggleQuickSize:
        ToggleQuickSizePopup();
        break;
    case ShortcutAction::ToggleWheel:
        TogglePaletteWheel();
        break;
    case ShortcutAction::PreviousPage:
        PreviousPage();
        break;
    case ShortcutAction::NextPage:
        NextPage();
        break;
    default:
        break;
    }
}

bool App::HandleShortcutKey(UINT key, bool ctrl, bool shift, bool alt) {
    for (const auto& shortcut : shortcuts_.bindings) {
        const KeyBinding& binding = shortcut.second;
        if (binding.key == key && binding.ctrl == ctrl && binding.shift == shift && binding.alt == alt) {
            ExecuteShortcutAction(shortcut.first);
            return true;
        }
    }
    return false;
}

}  // namespace cnote
