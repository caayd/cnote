#include "App.h"

#include <algorithm>
#include <commdlg.h>
#include <cwchar>
#include <cwctype>
#include <filesystem>

#include "ProjectFile.h"

namespace cnote {

using cnote::HasExtension;

namespace {

#ifdef _WIN32
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

bool PathsEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
    return Lowercase(a.native()) == Lowercase(b.native());
}
#else
bool PathsEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
    return a == b;
}
#endif

std::filesystem::path NormalizeFilePathForHistory(const std::filesystem::path& filePath) {
    if (filePath.empty()) {
        return {};
    }

    std::filesystem::path path = filePath;
    if (path.is_relative()) {
        std::error_code error;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
        if (!error) {
            path = absolutePath;
        }
    }

    return path.lexically_normal();
}

}  // namespace

std::filesystem::path App::SuggestedCnotePath() const {
    std::filesystem::path basePath = session_->document.FilePath();
    if (basePath.empty()) {
        basePath = session_->document.SourcePath();
    }

    std::filesystem::path suggestion;
    if (!basePath.empty()) {
        suggestion = basePath;
        suggestion.replace_extension(std::filesystem::path(kCnoteFileExtension));
    } else {
        suggestion = std::filesystem::path(L"Untitled").replace_extension(std::filesystem::path(kCnoteFileExtension));
    }

    return suggestion;
}

std::filesystem::path App::SuggestedPdfExportPath() const {
    std::filesystem::path basePath = session_->document.FilePath();
    if (basePath.empty()) {
        basePath = session_->document.SourcePath();
    }

    std::filesystem::path suggestion;
    if (!basePath.empty()) {
        suggestion = basePath;
        suggestion.replace_extension(L".pdf");
    } else {
        suggestion = std::filesystem::path(L"Untitled").replace_extension(L".pdf");
    }

    return suggestion;
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

bool App::OpenDocumentPath(const std::filesystem::path& filePath) {
    if (auto existing = FindOpenSession(filePath)) {
        if (existing == session_) return true;
        if (!ConfirmCloseWithUnsavedChanges()) return false;
        PrepareForDocumentSwitch();
        AttachSession(std::move(existing));
        FinishOtherViewInteractions();
        ResetDocumentView();
        newPageSettings_.width = session_->document.NewPageDefaults().width;
        newPageSettings_.height = session_->document.NewPageDefaults().height;
        newPageSettings_.backgroundColor = session_->document.NewPageDefaults().backgroundColor;
        newPageSettings_.paletteProfile = session_->document.NewPageDefaults().paletteProfile;
        RefreshNewPageSettingsWindow();
        RecordRecentDocumentPath(filePath);
        return true;
    }
    if (HasExtension(filePath, kCnoteFileExtension)) {
        return OpenProject(filePath);
    }

    if (HasExtension(filePath, u".pdf")) {
        return OpenPdf(filePath);
    }

    MessageBoxW(window_, L"Unsupported file type. Open a PDF or a .cnote file.", L"Open Document", MB_OK | MB_ICONERROR);
    return false;
}

bool App::OpenProject(const std::filesystem::path& filePath) {
    const auto opened = ProjectFile::Load(filePath, pdfBackend_);
    if (!opened.has_value()) {
        MessageBoxW(window_, L"Failed to open the cnote file.", L"Open Cnote", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!ConfirmCloseWithUnsavedChanges()) return false;
    PrepareForDocumentSwitch();
    ApplyDocument(*opened);
    newPageSettings_.width = session_->document.NewPageDefaults().width;
    newPageSettings_.height = session_->document.NewPageDefaults().height;
    newPageSettings_.backgroundColor = session_->document.NewPageDefaults().backgroundColor;
    newPageSettings_.paletteProfile = ResolvedPaletteProfile(session_->document.NewPageDefaults().paletteProfile);
    newPageSettings_.applyPaletteProfileToExistingStrokes = false;
    RefreshNewPageSettingsWindow();
    RecordRecentDocumentPath(filePath);
    return true;
}

bool App::OpenPdf(const std::filesystem::path& filePath) {
    const auto opened = pdfBackend_.Open(filePath);
    if (!opened.has_value()) {
        MessageBoxW(window_, L"Failed to open or render the PDF.", L"Open PDF", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!ConfirmCloseWithUnsavedChanges()) return false;
    PrepareForDocumentSwitch();
    ApplyDocument(*opened);
    session_->document.SetNewPageDefaults({
        newPageSettings_.width,
        newPageSettings_.height,
        newPageSettings_.backgroundColor,
        ResolvedPaletteProfile(newPageSettings_.paletteProfile),
    });
    RecordRecentDocumentPath(filePath);
    return true;
}

bool App::SaveCurrentCnote() {
    FinishOtherViewInteractions();
    FinishDocumentInteraction();
    if (HasSavedCnotePath()) {
        return SaveCnoteToPath(session_->document.FilePath(), true);
    }
    return SaveAsCnote();
}

bool App::SaveCnoteToPath(const std::filesystem::path& filePath, bool showError) {
    if (auto existing = FindOpenSession(filePath); existing && existing != session_) {
        if (showError) MessageBoxW(window_, L"This path is already open in another project. Choose a different filename.", L"Save Cnote", MB_OK | MB_ICONERROR);
        return false;
    }
    // The serialized page is a reopening hint, not shared navigation state.
    session_->document.SetCurrentPageIndex(CurrentPageIndex());
    if (!ProjectFile::Save(session_->document, filePath)) {
        if (showError) {
            MessageBoxW(window_, L"Failed to save the cnote file.", L"Save Cnote", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    session_->document.SetFilePath(filePath);
    MarkDocumentSaved();
    RecordRecentDocumentPath(filePath);
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void App::RecordRecentDocumentPath(const std::filesystem::path& filePath, bool saveSettings) {
    const std::filesystem::path normalizedPath = NormalizeFilePathForHistory(filePath);
    if (normalizedPath.empty()
        || (!HasExtension(normalizedPath, kCnoteFileExtension) && !HasExtension(normalizedPath, u".pdf"))) {
        return;
    }

    const auto existing = std::find_if(recentDocumentPaths_.begin(), recentDocumentPaths_.end(), [&](const std::filesystem::path& recentPath) {
        return PathsEqual(recentPath, normalizedPath);
    });

    if (existing != recentDocumentPaths_.end() && existing == recentDocumentPaths_.begin()) {
        if (*existing != normalizedPath) {
            *existing = normalizedPath;
            if (saveSettings) {
                SaveAppSettings();
            }
        }
        return;
    }

    if (existing != recentDocumentPaths_.end()) {
        recentDocumentPaths_.erase(existing);
    }

    recentDocumentPaths_.insert(recentDocumentPaths_.begin(), normalizedPath);
    if (recentDocumentPaths_.size() > kMaxRecentDocuments) {
        recentDocumentPaths_.resize(kMaxRecentDocuments);
    }

    if (saveSettings) {
        SaveAppSettings();
    }
}

bool App::OpenRecentDocument(std::size_t index) {
    if (index >= recentDocumentPaths_.size()) {
        return false;
    }

    const std::filesystem::path filePath = recentDocumentPaths_[index];
    std::error_code error;
    if (!std::filesystem::exists(filePath, error)) {
        MessageBoxW(window_, L"That recent file was not found.", L"Open", MB_OK | MB_ICONERROR);
        recentDocumentPaths_.erase(recentDocumentPaths_.begin() + static_cast<std::ptrdiff_t>(index));
        SaveAppSettings();
        InvalidateRect(window_, nullptr, FALSE);
        return false;
    }

    return OpenDocumentPath(filePath);
}

bool App::SaveAsCnote() {
    wchar_t fileBuffer[MAX_PATH] = {};
    const std::filesystem::path suggested = SuggestedCnotePath();
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

    return SaveCnoteToPath(fileBuffer, true);
}

bool App::ExportAsPdf() {
    CommitTextEdit();
    CommitLatexEdit();
    constexpr float kPdfExportFallbackRenderScale = 4.0f;

    wchar_t fileBuffer[MAX_PATH] = {};
    const std::filesystem::path suggested = SuggestedPdfExportPath();
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

    if (!pdfBackend_.ExportPdf(
            session_->document,
            fileBuffer,
            kPdfExportFallbackRenderScale,
            settings_.strokeSmoothing,
            settings_.dashLength,
            settings_.dashGap,
            settings_.embedProjectInExportedPdfs)) {
        MessageBoxW(window_, L"Failed to export the PDF.", L"Export as PDF", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

}  // namespace cnote
