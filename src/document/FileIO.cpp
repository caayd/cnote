#include "App.h"

#include <algorithm>
#include <commdlg.h>
#include <cwchar>
#include <cwctype>
#include <filesystem>

namespace cnote {

namespace {

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

}  // namespace

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
    newPageSettings_.width = document_.NewPageDefaults().width;
    newPageSettings_.height = document_.NewPageDefaults().height;
    newPageSettings_.backgroundColor = document_.NewPageDefaults().backgroundColor;
    RefreshNewPageSettingsWindow();
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
    document_.SetNewPageDefaults({ newPageSettings_.width, newPageSettings_.height, newPageSettings_.backgroundColor });
    return true;
}

bool App::SaveCurrentCnote() {
    if (HasSavedCnotePath()) {
        return SaveCnoteToPath(document_.FilePath(), true);
    }
    return SaveAsCnote();
}

bool App::SaveCnoteToPath(const std::wstring& filePath, bool showError) {
    if (!ProjectFile::Save(document_, filePath)) {
        if (showError) {
            MessageBoxW(window_, L"Failed to save the cnote file.", L"Save Cnote", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    document_.SetFilePath(filePath);
    MarkDocumentSaved();
    InvalidateRect(window_, nullptr, FALSE);
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

    return SaveCnoteToPath(fileBuffer, true);
}

bool App::ExportAsPdf() {
    constexpr float kPdfExportFallbackRenderScale = 4.0f;

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

    if (!pdfBackend_.ExportPdf(document_, fileBuffer, kPdfExportFallbackRenderScale, settings_.strokeSmoothing)) {
        MessageBoxW(window_, L"Failed to export the PDF.", L"Export as PDF", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

}  // namespace cnote
