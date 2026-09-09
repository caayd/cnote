#include "App.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include <commdlg.h>

#include "NewPageSettingsWindow.h"
#include "ProjectFile.h"
#include "SettingsWindow.h"
#include "ShortcutConfig.h"
#include "ui/UiUtil.h"

using cnote::HasExtension;
using cnote::ui::ParseHexColor;
using cnote::ui::TrimAscii;
using cnote::ui::TrimWide;

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





std::uint32_t RemapPaletteColor(
    std::uint32_t color,
    const std::vector<std::uint32_t>& sourcePaletteProfile,
    const std::vector<std::uint32_t>& targetPaletteProfile) {
    const std::size_t remapCount = std::min(sourcePaletteProfile.size(), targetPaletteProfile.size());
    for (std::size_t index = 0; index < remapCount; ++index) {
        if (color == sourcePaletteProfile[index]) {
            return targetPaletteProfile[index];
        }
    }
    return color;
}

bool ApplyPaletteProfileToPage(Page& page, const std::vector<std::uint32_t>& targetPaletteProfile) {
    bool changed = page.paletteProfile != targetPaletteProfile;
    for (Stroke& stroke : page.strokes) {
        const std::uint32_t remappedColor = RemapPaletteColor(stroke.style.color, page.paletteProfile, targetPaletteProfile);
        if (remappedColor != stroke.style.color) {
            stroke.style.color = remappedColor;
            changed = true;
        }
    }
    page.paletteProfile = targetPaletteProfile;
    return changed;
}

bool ApplyConfiguredSettingsToPage(
    Page& page,
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    bool changed = false;
    const float clampedWidth = std::max(72.0f, width);
    const float clampedHeight = std::max(72.0f, height);
    if (page.width != clampedWidth) {
        page.width = clampedWidth;
        changed = true;
    }
    if (page.height != clampedHeight) {
        page.height = clampedHeight;
        changed = true;
    }

    RasterImage raster = CreateSolidRaster(page.width, page.height, 1.5f, backgroundColor);
    if (page.sourcePdfSourceIndex != -1
        || page.sourcePdfPageIndex != -1
        || !RasterImageEqual(page.raster, raster)) {
        page.sourcePdfSourceIndex = -1;
        page.sourcePdfPageIndex = -1;
        page.raster = std::move(raster);
        changed = true;
    }

    if (applyPaletteProfileToExistingStrokes) {
        changed = ApplyPaletteProfileToPage(page, paletteProfile) || changed;
    }

    return changed;
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

std::string DrawCursorKindConfigName(App::DrawCursorKind kind) {
    switch (kind) {
    case App::DrawCursorKind::PrecisionCrosshair:
        return "precision_crosshair";
    case App::DrawCursorKind::Dot:
    default:
        return "dot";
    }
}

std::optional<App::DrawCursorKind> ParseDrawCursorKindConfigName(std::string_view name) {
    if (name == "dot") {
        return App::DrawCursorKind::Dot;
    }
    if (name == "precision_crosshair") {
        return App::DrawCursorKind::PrecisionCrosshair;
    }
    return std::nullopt;
}

std::optional<unsigned int> ParseUnsignedInt(std::string_view value, int base = 10);

std::optional<std::vector<std::uint32_t>> ParsePaletteColors(std::string_view value) {
    std::vector<std::uint32_t> colors;
    for (const std::string& token : SplitAscii(value, ',')) {
        const auto color = ParseUnsignedInt(token, 16);
        if (!color.has_value()) {
            return std::nullopt;
        }
        colors.push_back(*color);
        if (colors.size() >= 12U) {
            break;
        }
    }

    if (colors.empty()) {
        return std::nullopt;
    }
    return colors;
}

void WritePaletteColors(std::ostream& output, const std::vector<std::uint32_t>& colors) {
    for (std::size_t index = 0; index < colors.size(); ++index) {
        if (index > 0U) {
            output << ',';
        }
        std::ostringstream colorValue;
        colorValue << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << colors[index];
        output << colorValue.str();
    }
}

bool EqualPaletteNames(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}



std::optional<unsigned int> ParseUnsignedInt(std::string_view value, int base) {
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



std::wstring FormatBrushSizePresetValue(float size) {
    wchar_t buffer[16] = {};
    std::swprintf(buffer, std::size(buffer), L"%.2f", App::SnapBrushSize(size));
    std::wstring value = buffer;
    while (!value.empty() && value.back() == L'0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == L'.') {
        value.pop_back();
    }
    return value;
}

std::vector<float> NormalizeBrushSizePresets(std::vector<float> presets) {
    for (float& preset : presets) {
        preset = App::SnapBrushSize(preset);
    }

    std::sort(presets.begin(), presets.end());
    presets.erase(std::unique(presets.begin(), presets.end(), [](float left, float right) {
        return std::abs(left - right) < 0.001f;
    }), presets.end());
    if (presets.size() > 16U) {
        presets.resize(16U);
    }
    return presets;
}

const char* ToolConfigName(App::Tool tool) {
    switch (tool) {
    case App::Tool::Select:
        return "Select";
    case App::Tool::Move:
        return "Move";
    case App::Tool::Text:
        return "Text";
    case App::Tool::Latex:
        return "Latex";
    case App::Tool::Draw:
        return "Draw";
    case App::Tool::Pan:
        return "Pan";
    case App::Tool::Erase:
        return "Erase";
    case App::Tool::Zoom:
        return "Zoom";
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
    if (name == "Text") {
        return App::Tool::Text;
    }
    if (name == "Latex") {
        return App::Tool::Latex;
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
    if (name == "Zoom") {
        return App::Tool::Zoom;
    }
    return std::nullopt;
}

void FlattenImportedPage(Page& page) {
    page.sourcePdfSourceIndex = -1;
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
    if (session_->document.PageCount() == 0U) {
        pdfSidebarOpen_ = false;
        pdfSidebarSlideProgress_ = 0.0f;
        pdfSidebarSlideAnimationStartProgress_ = 0.0f;
        pdfSidebarSlideAnimationTargetProgress_ = 0.0f;
        pdfSidebarScrollY_ = 0;
        MarkPdfSidebarCompositionDirty();
        CancelPageJumpInput();
        StopPdfSidebarSlideAnimationTimer();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    pdfSidebarOpen_ = !pdfSidebarOpen_;
    MarkPdfSidebarCompositionDirty();
    if (pdfSidebarOpen_) {
        EnsureSidebarCurrentPageVisible();
        StartPdfSidebarSlideAnimation(1.0f);
    } else {
        pdfSidebarScrollY_ = 0;
        CancelPageJumpInput();
        StartPdfSidebarSlideAnimation(0.0f);
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
    newPageSettings_.backgroundColor = settings_.defaultPageColor;
    newPageSettings_.paletteProfile = ResolvedPaletteProfile({});
    newPageSettings_.applyPaletteProfileToExistingStrokes = false;
    RefreshNewPageSettingsWindow();
}

std::vector<std::uint32_t> App::ResolvedPaletteProfile(const std::vector<std::uint32_t>& paletteProfile) const {
    if (!paletteProfile.empty()) {
        return paletteProfile;
    }
    if (!palette_.empty()) {
        return palette_;
    }
    return DefaultPalette();
}

void App::NormalizeDocumentPaletteProfiles() {
    session_->document.SetNewPageDefaults({
        session_->document.NewPageDefaults().width,
        session_->document.NewPageDefaults().height,
        session_->document.NewPageDefaults().backgroundColor,
        ResolvedPaletteProfile(session_->document.NewPageDefaults().paletteProfile),
    });
    for (std::size_t pageIndex = 0; pageIndex < session_->document.PageCount(); ++pageIndex) {
        Page& page = session_->document.PageAt(pageIndex);
        page.paletteProfile = ResolvedPaletteProfile(page.paletteProfile.empty() ? session_->document.NewPageDefaults().paletteProfile : page.paletteProfile);
    }
}

Page App::BuildConfiguredNewPage() const {
    Page page {};
    page.width = std::max(72.0f, newPageSettings_.width);
    page.height = std::max(72.0f, newPageSettings_.height);
    page.paletteProfile = ResolvedPaletteProfile(newPageSettings_.paletteProfile);
    page.raster = CreateSolidRaster(page.width, page.height, 1.5f, newPageSettings_.backgroundColor);
    return page;
}

void App::ApplyConfiguredSettingsToCurrentPage(
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    if (session_->document.PageCount() == 0U) {
        return;
    }

    const std::vector<std::uint32_t> resolvedPaletteProfile = ResolvedPaletteProfile(paletteProfile);
    const std::size_t pageIndex = CurrentPageIndex();
    const Page beforePage = CurrentPage();
    Page& page = CurrentPage();
    if (!ApplyConfiguredSettingsToPage(page, width, height, backgroundColor, resolvedPaletteProfile, applyPaletteProfileToExistingStrokes)) {
        return;
    }

    PushReplacePagesHistoryAction({ pageIndex }, { beforePage }, pageIndex);
    NotifyDocumentMutated();
    InvalidatePageSurfaceCache(pageIndex);
    InvalidateSidebarThumbnailCache(pageIndex);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::ApplyConfiguredSettingsToAllPages(
    float width,
    float height,
    std::uint32_t backgroundColor,
    const std::vector<std::uint32_t>& paletteProfile,
    bool applyPaletteProfileToExistingStrokes) {
    if (session_->document.PageCount() == 0U) {
        return;
    }

    const std::vector<std::uint32_t> resolvedPaletteProfile = ResolvedPaletteProfile(paletteProfile);
    std::vector<std::size_t> affectedPageIndices;
    std::vector<Page> beforePages;
    affectedPageIndices.reserve(session_->document.PageCount());
    beforePages.reserve(session_->document.PageCount());
    bool changed = false;
    for (std::size_t pageIndex = 0; pageIndex < session_->document.PageCount(); ++pageIndex) {
        beforePages.push_back(session_->document.PageAt(pageIndex));
        affectedPageIndices.push_back(pageIndex);
        Page& page = session_->document.PageAt(pageIndex);
        changed = ApplyConfiguredSettingsToPage(
            page,
            width,
            height,
            backgroundColor,
            resolvedPaletteProfile,
            applyPaletteProfileToExistingStrokes)
            || changed;
    }

    if (!changed) {
        return;
    }

    PushReplacePagesHistoryAction(std::move(affectedPageIndices), std::move(beforePages), CurrentPageIndex());
    pageSurfaceCaches_.assign(session_->document.PageCount(), {});
    sidebarThumbnailCaches_.assign(session_->document.PageCount(), {});
    NotifyDocumentMutated();
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

    ClearSelection();

    const std::size_t insertIndex = session_->document.PageCount();
    const std::size_t previousPageIndex = CurrentPageIndex();
    std::vector<Page> importedPages;
    importedPages.reserve(opened->PageCount());
    std::vector<std::int32_t> sourceIndexRemap(opened->PdfSourceCount(), -1);
    for (std::size_t pageIndex = 0; pageIndex < opened->PageCount(); ++pageIndex) {
        Page importedPage = opened->PageAt(pageIndex);
        importedPage.paletteProfile = ResolvedPaletteProfile(
            importedPage.paletteProfile.empty() ? opened->NewPageDefaults().paletteProfile : importedPage.paletteProfile);
        if (importedPage.sourcePdfSourceIndex >= 0 && importedPage.sourcePdfPageIndex >= 0) {
            const std::size_t sourceIndex = static_cast<std::size_t>(importedPage.sourcePdfSourceIndex);
            if (sourceIndex < sourceIndexRemap.size()) {
                if (sourceIndexRemap[sourceIndex] < 0) {
                    const Document::PdfSource& source = opened->PdfSourceAt(sourceIndex);
                    sourceIndexRemap[sourceIndex] = session_->document.AppendPdfSource(source.sourcePath, source.pdfData);
                }
                if (sourceIndexRemap[sourceIndex] >= 0) {
                    importedPage.sourcePdfSourceIndex = sourceIndexRemap[sourceIndex];
                } else {
                    FlattenImportedPage(importedPage);
                }
            } else {
                FlattenImportedPage(importedPage);
            }
        }
        importedPages.push_back(importedPage);
        InsertDocumentPage(session_->document.PageCount(), std::move(importedPage));
    }

    PushPageHistoryAction(HistoryActionType::AddPages, insertIndex, std::move(importedPages), insertIndex, previousPageIndex);
    RestoreAfterPageHistoryChange(insertIndex);
    NotifyDocumentMutated();
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
    ReleaseDrawCursor();
    SaveAppSettings();
    RefreshSettingsWindow();
    RefreshNewPageSettingsWindow();
    InvalidateRect(window_, nullptr, FALSE);
    RefreshCursor();
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
    ReleaseDrawCursor();
    SaveAppSettings();
    RefreshSettingsWindow();
    RefreshNewPageSettingsWindow();
    InvalidateRect(window_, nullptr, FALSE);
    RefreshCursor();
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
        ReleaseDrawCursor();
        SaveAppSettings();
        RefreshSettingsWindow();
        RefreshNewPageSettingsWindow();
        InvalidateRect(window_, nullptr, FALSE);
        RefreshCursor();
    }
    return applied;
}

bool App::TryParseBrushSizePresets(std::wstring_view text, std::vector<float>& presets) {
    const std::wstring trimmed = TrimWide(text);
    if (trimmed.empty()) {
        presets.clear();
        return true;
    }

    std::vector<float> parsedPresets;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t end = trimmed.find(L',', start);
        const std::wstring token = TrimWide(trimmed.substr(
            start,
            end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        if (token.empty()) {
            return false;
        }

        wchar_t* parseEnd = nullptr;
        const float parsedValue = std::wcstof(token.c_str(), &parseEnd);
        if (parseEnd == token.c_str() || *parseEnd != L'\0' || parsedValue <= 0.0f) {
            return false;
        }

        parsedPresets.push_back(parsedValue);
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1U;
    }

    presets = NormalizeBrushSizePresets(std::move(parsedPresets));
    return true;
}

std::wstring App::FormatBrushSizePresets(const std::vector<float>& presets) {
    std::wstring formatted;
    const std::vector<float> normalized = NormalizeBrushSizePresets(presets);
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        if (index > 0U) {
            formatted += L", ";
        }
        formatted += FormatBrushSizePresetValue(normalized[index]);
    }
    return formatted;
}

void App::LoadAppSettings() {
    std::ifstream input(AppSettingsPath(), std::ios::binary);
    if (!input) {
        return;
    }

    struct PendingNamedPalette {
        std::wstring name {};
        std::vector<std::uint32_t> colors {};
        bool hasName = false;
        bool hasColors = false;
    };

    std::optional<std::size_t> loadedColorIndex;
    std::optional<std::size_t> loadedSettingsPaletteIndex;
    std::map<unsigned int, PendingNamedPalette> pendingNamedPalettes;
    std::map<unsigned int, std::wstring> pendingRecentDocumentPaths;
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
            if (const auto loadedPalette = ParsePaletteColors(value); loadedPalette.has_value()) {
                palette_ = std::move(*loadedPalette);
            }
            continue;
        }

        if (key.starts_with("saved_palette_")) {
            const std::string_view suffix(key.c_str() + 14, key.size() - 14U);
            const std::size_t fieldSeparator = suffix.find('_');
            if (fieldSeparator == std::string_view::npos) {
                continue;
            }

            const auto paletteIndex = ParseUnsignedInt(suffix.substr(0, fieldSeparator));
            if (!paletteIndex.has_value() || *paletteIndex == 0U) {
                continue;
            }

            PendingNamedPalette& pendingPalette = pendingNamedPalettes[*paletteIndex];
            const std::string_view field = suffix.substr(fieldSeparator + 1U);
            if (field == "name") {
                pendingPalette.name = TrimWide(Utf8ToWide(value));
                pendingPalette.hasName = !pendingPalette.name.empty();
            } else if (field == "colors") {
                if (const auto colors = ParsePaletteColors(value); colors.has_value()) {
                    pendingPalette.colors = std::move(*colors);
                    pendingPalette.hasColors = true;
                }
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

        if (key == "default_page_color") {
            if (const auto parsed = ParseUnsignedInt(value, 16); parsed.has_value()) {
                settings_.defaultPageColor = *parsed;
            }
            continue;
        }

        if (key.starts_with("recent_document_")) {
            if (const auto index = ParseUnsignedInt(std::string_view(key).substr(16)); index.has_value()
                && *index >= 1U
                && *index <= kMaxRecentDocuments) {
                std::wstring recentPath = TrimWide(Utf8ToWide(value));
                if (!recentPath.empty()) {
                    pendingRecentDocumentPaths[*index] = std::move(recentPath);
                }
            }
            continue;
        }

        if (key.starts_with("recent_cnote_")) {
            if (const auto index = ParseUnsignedInt(std::string_view(key).substr(13)); index.has_value()
                && *index >= 1U
                && *index <= kMaxRecentDocuments) {
                std::wstring recentPath = TrimWide(Utf8ToWide(value));
                if (!recentPath.empty() && pendingRecentDocumentPaths.find(*index) == pendingRecentDocumentPaths.end()) {
                    pendingRecentDocumentPaths[*index] = std::move(recentPath);
                }
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
                settings_.drawBrushSize = std::clamp(*parsed, 0.5f, 20.0f);
            }
            continue;
        }

        if (key == "erase_brush_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.eraseBrushSize = std::clamp(*parsed, 0.5f, 20.0f);
            }
            continue;
        }

        if (key == "quick_size_slider_max") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.quickSizeSliderMax = std::clamp(*parsed, 1.0f, 20.0f);
            }
            continue;
        }

        if (key == "brush_size_presets") {
            std::vector<float> parsedPresets;
            if (TryParseBrushSizePresets(Utf8ToWide(value), parsedPresets)) {
                settings_.brushSizePresets = std::move(parsedPresets);
            }
            continue;
        }

        if (key == "draw_cursor_size") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.drawCursorSize = std::clamp(*parsed, 4.0f, 24.0f);
            }
            continue;
        }

        if (key == "draw_cursor_kind") {
            if (const auto parsed = ParseDrawCursorKindConfigName(value); parsed.has_value()) {
                settings_.drawCursorKind = *parsed;
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

        if (key == "dash_length") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.dashLength = std::clamp(*parsed, 0.5f, 8.0f);
            }
            continue;
        }

        if (key == "dash_gap") {
            if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                settings_.dashGap = std::clamp(*parsed, 0.5f, 8.0f);
            }
            continue;
        }

        if (key == "auto_save_interval_seconds") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                settings_.autoSaveIntervalSeconds = std::min(*parsed, 300U);
            }
            continue;
        }

        if (key == "auto_hide_toolbar") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                settings_.autoHideToolbar = *parsed != 0U;
            }
            continue;
        }

        if (key == "embed_project_in_exported_pdfs") {
            if (const auto parsed = ParseUnsignedInt(value); parsed.has_value()) {
                settings_.embedProjectInExportedPdfs = *parsed != 0U;
            }
            continue;
        }

        if (key.starts_with("zoom_preset_")) {
            if (const auto index = ParseUnsignedInt(std::string_view(key).substr(12)); index.has_value()
                && *index >= 1U
                && *index <= settings_.zoomPresets.size()) {
                if (const auto parsed = ParseFloat(value); parsed.has_value()) {
                    settings_.zoomPresets[*index - 1U] = *parsed <= 0.0f ? 0.0f : std::clamp(*parsed, kMinZoom, kMaxZoom);
                }
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

    savedPalettes_.clear();
    for (auto& [unusedIndex, pendingPalette] : pendingNamedPalettes) {
        static_cast<void>(unusedIndex);
        if (!pendingPalette.hasName || !pendingPalette.hasColors || pendingPalette.colors.empty()) {
            continue;
        }

        const std::wstring trimmedName = TrimWide(pendingPalette.name);
        if (trimmedName.empty()) {
            continue;
        }

        const auto existingPalette = std::find_if(savedPalettes_.begin(), savedPalettes_.end(), [&](const NamedPalette& palette) {
            return EqualPaletteNames(palette.name, trimmedName);
        });
        if (existingPalette != savedPalettes_.end()) {
            existingPalette->name = trimmedName;
            existingPalette->colors = pendingPalette.colors;
        } else {
            savedPalettes_.push_back({ trimmedName, pendingPalette.colors });
        }
    }

    recentDocumentPaths_.clear();
    for (auto it = pendingRecentDocumentPaths.rbegin(); it != pendingRecentDocumentPaths.rend(); ++it) {
        RecordRecentDocumentPath(it->second, false);
    }

    if (palette_ == LegacyDefaultPalette()) {
        palette_ = DefaultPalette();
    }
    if (palette_.empty()) {
        palette_ = DefaultPalette();
    }
    settings_.quickSizeSliderMax = std::clamp(settings_.quickSizeSliderMax, 1.0f, kMaxBrushSize);
    settings_.drawBrushSize = std::min(settings_.drawBrushSize, settings_.quickSizeSliderMax);
    settings_.eraseBrushSize = std::min(settings_.eraseBrushSize, settings_.quickSizeSliderMax);
    colorIndex_ = std::min(loadedColorIndex.value_or(colorIndex_), palette_.size() - 1U);
    settings_.paletteIndex = std::min(loadedSettingsPaletteIndex.value_or(settings_.paletteIndex), palette_.size() - 1U);
    newPageSettings_.backgroundColor = settings_.defaultPageColor;
    newPageSettings_.paletteProfile = ResolvedPaletteProfile(newPageSettings_.paletteProfile);
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
    WritePaletteColors(output, palette_);
    output << '\n';
    for (std::size_t index = 0; index < savedPalettes_.size(); ++index) {
        const NamedPalette& palette = savedPalettes_[index];
        if (palette.name.empty() || palette.colors.empty()) {
            continue;
        }
        output << "saved_palette_" << (index + 1U) << "_name=" << WideToUtf8(palette.name) << '\n';
        output << "saved_palette_" << (index + 1U) << "_colors=";
        WritePaletteColors(output, palette.colors);
        output << '\n';
    }
    output << "active_color=" << colorIndex_ << '\n';
    output << "settings_palette_index=" << settings_.paletteIndex << '\n';
    output << "default_page_color=" << std::hex << std::uppercase << settings_.defaultPageColor << std::dec << std::nouppercase << '\n';
    for (std::size_t index = 0; index < recentDocumentPaths_.size(); ++index) {
        output << "recent_document_" << (index + 1U) << '=' << WideToUtf8(recentDocumentPaths_[index]) << '\n';
    }
    output << "draw_brush_size=" << settings_.drawBrushSize << '\n';
    output << "erase_brush_size=" << settings_.eraseBrushSize << '\n';
    output << "quick_size_slider_max=" << settings_.quickSizeSliderMax << '\n';
    output << "brush_size_presets=" << WideToUtf8(FormatBrushSizePresets(settings_.brushSizePresets)) << '\n';
    output << "draw_cursor_kind=" << DrawCursorKindConfigName(settings_.drawCursorKind) << '\n';
    output << "draw_cursor_size=" << settings_.drawCursorSize << '\n';
    output << "input_smoothing=" << settings_.inputSmoothing << '\n';
    output << "input_spacing=" << settings_.inputSpacing << '\n';
    output << "stroke_smoothing=" << settings_.strokeSmoothing << '\n';
    output << "dash_length=" << settings_.dashLength << '\n';
    output << "dash_gap=" << settings_.dashGap << '\n';
    output << "auto_save_interval_seconds=" << settings_.autoSaveIntervalSeconds << '\n';
    output << "auto_hide_toolbar=" << (settings_.autoHideToolbar ? 1 : 0) << '\n';
    output << "embed_project_in_exported_pdfs=" << (settings_.embedProjectInExportedPdfs ? 1 : 0) << '\n';
    for (std::size_t index = 0; index < settings_.zoomPresets.size(); ++index) {
        output << "zoom_preset_" << (index + 1U) << '=' << settings_.zoomPresets[index] << '\n';
    }

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

bool App::SaveNamedPalette(const std::wstring& name) {
    const std::wstring trimmedName = TrimWide(name);
    if (trimmedName.empty() || palette_.empty()) {
        return false;
    }

    const auto existingPalette = std::find_if(savedPalettes_.begin(), savedPalettes_.end(), [&](const NamedPalette& palette) {
        return EqualPaletteNames(palette.name, trimmedName);
    });
    if (existingPalette != savedPalettes_.end()) {
        existingPalette->name = trimmedName;
        existingPalette->colors = palette_;
    } else {
        savedPalettes_.push_back({ trimmedName, palette_ });
    }

    SaveAppSettings();
    RefreshSettingsWindow();
    RefreshNewPageSettingsWindow();
    return true;
}

bool App::ApplyNamedPalette(std::size_t index) {
    if (index >= savedPalettes_.size() || savedPalettes_[index].colors.empty()) {
        return false;
    }

    palette_ = savedPalettes_[index].colors;
    settings_.paletteIndex = std::min(settings_.paletteIndex, palette_.size() - 1U);
    colorIndex_ = std::min(colorIndex_, palette_.size() - 1U);
    ReleaseDrawCursor();
    SaveAppSettings();
    RefreshSettingsWindow();
    RefreshNewPageSettingsWindow();
    ClosePaletteOverlays();
    InvalidateRect(window_, nullptr, FALSE);
    RefreshCursor();
    return true;
}

bool App::DeleteNamedPalette(std::size_t index) {
    if (index >= savedPalettes_.size()) {
        return false;
    }

    savedPalettes_.erase(savedPalettes_.begin() + static_cast<std::ptrdiff_t>(index));
    SaveAppSettings();
    RefreshSettingsWindow();
    RefreshNewPageSettingsWindow();
    return true;
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
    case ShortcutAction::ToolText:
        SetActiveTool(Tool::Text);
        break;
    case ShortcutAction::ToolLatex:
        SetActiveTool(Tool::Latex);
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
        static_cast<void>(SaveCurrentCnote());
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
    case ShortcutAction::Cut:
        if (SidebarOwnsKeyboardShortcuts()) {
            static_cast<void>(CutCurrentPageToSidebarBuffer());
        } else {
            static_cast<void>(CutSelectionToClipboard());
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
        ResetView();
        InvalidateRect(window_, nullptr, FALSE);
        break;
    case ShortcutAction::ZoomPreset1:
        static_cast<void>(ApplyZoomPreset(0));
        break;
    case ShortcutAction::ZoomPreset2:
        static_cast<void>(ApplyZoomPreset(1));
        break;
    case ShortcutAction::ZoomPreset3:
        static_cast<void>(ApplyZoomPreset(2));
        break;
    case ShortcutAction::ZoomPreset4:
        static_cast<void>(ApplyZoomPreset(3));
        break;
    case ShortcutAction::ZoomPreset5:
        static_cast<void>(ApplyZoomPreset(4));
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
