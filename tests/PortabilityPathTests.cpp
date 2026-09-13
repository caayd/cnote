// Cross-platform path and file-name behavior (migration plan 7.1/7.6 and the
// assumptions section).
//
// These cases cannot be caught by the Windows build alone:
//   * a path that differs from another only by case must NOT name the same file
//     on Linux (two projects may coexist) but MUST on Windows;
//   * .cnote/.pdf extension checks stay case-insensitive on every platform;
//   * a non-ASCII project name must survive save/open and PDF export;
//   * a destination directory created after the application started must work;
//   * the settings location follows the platform rule (APPDATA on Windows,
//     XDG_CONFIG_HOME elsewhere).
//
// Path construction itself is checked because the Windows toolchain decides the
// narrow encoding: MinGW libstdc++ decodes a narrow std::filesystem::path source
// as UTF-8 and *throws* filesystem_error on bytes that are not valid UTF-8.
// That is why every conversion here goes through cnote::Utf8ToPath/PathToUtf8
// (and why the shell must build paths from Qt's UTF-16 text, not from a
// code-page-encoded argv).

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <QGuiApplication>
#include <QString>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"
#include "Unicode.h"
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        std::printf("ok   %s\n", message.c_str());
    } else {
        std::printf("FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

std::filesystem::path MakeTempDir(const std::string& name) {
    std::error_code error;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(error) / cnote::Utf8ToPath("cnote-portability-" + name);
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

// A project whose only text identifies it, so two files can be told apart.
cnote::Document MakeProject(std::u16string_view marker) {
    cnote::Document document = cnote::Document::CreateBlank();
    cnote::PageText text {};
    text.id = 1;
    text.x = 12.0f;
    text.y = 24.0f;
    text.width = 200.0f;
    text.height = 32.0f;
    text.fontSize = 15.0f;
    text.content = std::u16string(marker);
    document.CurrentPage().texts.push_back(text);
    return document;
}

std::u16string ProjectMarker(const cnote::Document& document) {
    if (document.PageCount() == 0 || document.PageAt(0).texts.empty()) {
        return {};
    }
    return document.PageAt(0).texts.front().content;
}

// --- non-ASCII file names -----------------------------------------------------

void NonAsciiProjectRoundTrip() {
    const std::filesystem::path dir = MakeTempDir("non-ascii-project");
    // "nöte-α-日本.cnote" as UTF-8 bytes, written explicitly so the test does not
    // depend on the compiler's source charset.
    const std::string utf8Name = "n\xC3\xB6" "te-" "\xCE\xB1" "-\xE6\x97\xA5\xE6\x9C\xAC" ".cnote";
    const std::filesystem::path projectPath = dir / cnote::Utf8ToPath(utf8Name);

    Check(cnote::ProjectFile::Save(MakeProject(u"unicode"), projectPath),
        "save a project to a non-ASCII path");
    Check(std::filesystem::is_regular_file(projectPath), "the non-ASCII project file exists");
    const std::optional<cnote::Document> loaded = cnote::ProjectFile::Load(projectPath);
    Check(loaded.has_value(), "reopen the non-ASCII project");
    Check(loaded.has_value() && ProjectMarker(*loaded) == u"unicode", "content survives the non-ASCII path");

    Check(cnote::PathToUtf8(projectPath.filename()) == utf8Name, "PathToUtf8 reproduces the exact UTF-8 bytes");
    Check(cnote::Utf8ToPath(utf8Name) == projectPath.filename(), "Utf8ToPath reconstructs the same path");

    // The file dialogs hand over Qt UTF-16 text; the conversion used for it must
    // address the same file as the UTF-8 helpers.
    const QString dialogName = QString::fromUtf8(utf8Name.data(), static_cast<qsizetype>(utf8Name.size()));
    const std::filesystem::path fromDialog =
        dir / cnote::Utf8ToPath(dialogName.toUtf8().toStdString());
    std::error_code error;
    Check(std::filesystem::equivalent(projectPath, fromDialog, error) && !error,
        "a QFileDialog-style conversion names the same file as the UTF-8 helper");
}

void NonAsciiPdfExport() {
    const std::filesystem::path dir = MakeTempDir("non-ascii-export");
    const std::string utf8Name = "\xCE\xB5\xCE\xBE\xCE\xB1\xCE\xB3\xCF\x89\xCE\xB3\xCE\xAE" ".pdf";  // εξαγωγή.pdf
    const std::filesystem::path exportPath = dir / cnote::Utf8ToPath(utf8Name);

    cnote::PdfiumBackend backend;
    Check(backend.ExportPdf(MakeProject(u"exported"), exportPath, 1.0f, 0.55f, 3.0f, 2.0f,
              std::span<const std::uint8_t> {}),
        "export a PDF to a non-ASCII path");
    Check(std::filesystem::is_regular_file(exportPath), "the non-ASCII PDF exists");

    std::ifstream stream(exportPath, std::ios::binary);
    char magic[5] = {};
    stream.read(magic, 4);
    Check(std::strncmp(magic, "%PDF", 4) == 0, "the non-ASCII PDF has a PDF header");
}

// --- case sensitivity ---------------------------------------------------------

void CaseOnlyDifferentPath() {
    const std::filesystem::path dir = MakeTempDir("case");
    const std::filesystem::path upper = dir / "Notes.cnote";
    const std::filesystem::path lower = dir / "notes.cnote";

    Check(cnote::ProjectFile::Save(MakeProject(u"upper"), upper), "save Notes.cnote");
#ifdef _WIN32
    Check(cnote::AppManager::PathsReferToSameFile(upper, lower),
        "Windows treats a case-only path difference as the same file");
#else
    Check(!cnote::AppManager::PathsReferToSameFile(upper, lower),
        "Linux treats a case-only path difference as a different file");
#endif

#ifdef _WIN32
    // Windows: both spellings name the same file, so a session must be joined.
    std::error_code error;
    Check(std::filesystem::equivalent(upper, lower, error) && !error,
        "Windows reports the two spellings as the same file");
    const std::optional<cnote::Document> viaLower = cnote::ProjectFile::Load(lower);
    Check(viaLower.has_value() && ProjectMarker(*viaLower) == u"upper",
        "Windows opens the lower-case spelling as the same project");
#else
    std::error_code error;
    // The lower-case spelling does not exist yet, so equivalent() reports an
    // error rather than "same file"; the independent-projects checks below are
    // what proves the two spellings are different files.
    Check(!std::filesystem::equivalent(upper, lower, error),
        "Linux reports the two spellings as different files");
    // Two projects differing only by case coexist and stay independent.
    Check(cnote::ProjectFile::Save(MakeProject(u"lower"), lower), "save notes.cnote next to Notes.cnote");
    const std::optional<cnote::Document> viaLower = cnote::ProjectFile::Load(lower);
    const std::optional<cnote::Document> viaUpper = cnote::ProjectFile::Load(upper);
    Check(viaLower.has_value() && viaUpper.has_value(), "both case-differing projects load");
    Check(ProjectMarker(*viaLower) == u"lower" && ProjectMarker(*viaUpper) == u"upper",
        "case-differing projects keep their own contents");
#endif
}

void ExtensionCaseRules() {
    Check(cnote::HasExtension("Report.CNOTE", u".cnote"), "project extension check is case-insensitive");
    Check(cnote::HasExtension("Doc.PdF", u".pdf"), "PDF extension check is case-insensitive");
    Check(!cnote::HasExtension("Notes.txt", u".cnote"), "other extensions are rejected");
    Check(cnote::AppManager::HasCnoteExtension("Mixed.CnOtE"), "session lookup accepts a mixed-case .cnote");
    Check(cnote::AppManager::HasPdfExtension("Mixed.PdF"), "session lookup accepts a mixed-case .pdf");

    const std::filesystem::path dir = MakeTempDir("extension");
    const std::filesystem::path mixed = dir / "Mixed.CnOtE";
    Check(cnote::ProjectFile::Save(MakeProject(u"mixed"), mixed), "save with a mixed-case .CnOtE extension");
    const std::optional<cnote::Document> loaded = cnote::ProjectFile::Load(mixed);
    Check(loaded.has_value() && ProjectMarker(*loaded) == u"mixed",
        "a mixed-case extension still opens on every platform");
}

// --- directories created after startup ---------------------------------------

void DirectoryCreatedAfterStartup() {
    // The services exist before the directory does: mirrors a user creating a new
    // folder in the file dialog after the application started.
    const std::filesystem::path base = MakeTempDir("late-directory");
    cnote::Document document = MakeProject(u"late");
    cnote::PdfiumBackend backend;
    cnote::settings::SettingsStoreQt store;

    const std::filesystem::path freshDir = base / "created" / "after" / "startup";
    std::error_code error;
    std::filesystem::create_directories(freshDir, error);
    Check(!error, "create a directory after the services were constructed");

    const std::filesystem::path projectPath = freshDir / "late.cnote";
    Check(cnote::ProjectFile::Save(document, projectPath), "save a project into the new directory");
    const std::optional<cnote::Document> loaded = cnote::ProjectFile::Load(projectPath);
    Check(loaded.has_value() && ProjectMarker(*loaded) == u"late", "reopen the project from the new directory");

    Check(backend.ExportPdf(document, freshDir / "late.pdf", 1.0f, 0.55f, 3.0f, 2.0f,
              std::span<const std::uint8_t> {}),
        "export a PDF into the new directory");
    Check(!store.path().empty(), "the settings store resolves a location");
}

// --- settings location --------------------------------------------------------

void SettingsLocationRule() {
#ifdef _WIN32
    const QByteArray previous = qgetenv("APPDATA");
    const std::filesystem::path scratch = MakeTempDir("settings-appdata");
    qputenv("APPDATA", QByteArray(reinterpret_cast<const char*>(scratch.u8string().data()),
                            static_cast<qsizetype>(scratch.u8string().size())));
    const std::filesystem::path resolved = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    Check(resolved == scratch / "cnote" / "settings.ini", "Windows resolves %APPDATA%/cnote/settings.ini");

    qputenv("APPDATA", QByteArray());
    const std::filesystem::path fallback = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    Check(fallback == std::filesystem::current_path() / "cnote.settings.ini",
        "Windows falls back to the working-directory settings file when APPDATA is unset");

    if (previous.isEmpty()) {
        qunsetenv("APPDATA");
    } else {
        qputenv("APPDATA", previous);
    }
#else
    const QByteArray previous = qgetenv("XDG_CONFIG_HOME");
    const std::filesystem::path scratch = MakeTempDir("settings-xdg");
    const QByteArray scratchBytes(reinterpret_cast<const char*>(scratch.u8string().data()),
        static_cast<qsizetype>(scratch.u8string().size()));
    qputenv("XDG_CONFIG_HOME", scratchBytes);
    const std::filesystem::path resolved = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    Check(resolved == scratch / "cnote" / "settings.ini", "Linux resolves XDG_CONFIG_HOME/cnote/settings.ini");

    if (previous.isEmpty()) {
        qunsetenv("XDG_CONFIG_HOME");
    } else {
        qputenv("XDG_CONFIG_HOME", previous);
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));

    NonAsciiProjectRoundTrip();
    NonAsciiPdfExport();
    CaseOnlyDifferentPath();
    ExtensionCaseRules();
    DirectoryCreatedAfterStartup();
    SettingsLocationRule();

    if (gFailures != 0) {
        std::printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
