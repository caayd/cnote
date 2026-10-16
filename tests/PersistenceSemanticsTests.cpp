// Persistence and error semantics of the Qt application (migration plan 6.6,
// with 4.5/7.2 for the close and autosave interplay).
//
// Drives the real services (QtDocumentController, AppManager, QtMainWindow,
// SettingsStoreQt) against a temporary directory, HOME/APPDATA-style settings
// location and project files:
//
//   * a successful project save writes the file, clears shared dirty state and
//     broadcasts exactly one Saved change;
//   * a failed save (read-only destination file, read-only parent directory on
//     POSIX, destination that is a directory) keeps the previous destination
//     bytes, keeps dirty set, keeps the saved path and reports an error;
//   * PDF export replaces the requested file only after successful generation,
//     for both the attachment-bearing and attachment-free variants, and leaves
//     no temporary file behind on success or failure;
//   * recent-document entries parsed from settings are normalised, filtered to
//     .cnote/.pdf, deduplicated (per platform), ordered newest-first and capped;
//   * closing the last dirty view auto-saves a saved project, keeps the window
//     open and dirty when that save fails, and a cancelled unsaved-changes
//     prompt keeps the window alive.
//
// What cannot be produced here: a genuine disk-full or interrupted-write
// failure.  Those are covered by the code-path guarantee (the destination is
// replaced only after a successful write) and are called out in the report.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <QApplication>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QString>
#include <QTimer>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"
#include "ProjectSession.h"
#include "Unicode.h"
#include "qt/QtDocumentController.h"
#include "qt/QtMainWindow.h"
#include "settings/SettingsStoreQt.h"
#include "render/PageRenderer.h"
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
        std::filesystem::temp_directory_path(error) / cnote::Utf8ToPath("cnote-persistence-" + name);
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void MakeWritable(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
}

// Read-only file: opening it for truncation must fail on Windows and POSIX.
bool MakeReadOnlyFile(const std::filesystem::path& path, std::string_view contents) {
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream << contents;
    }
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace, error);
    return !error;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool StartsWithPdfMagic(const std::filesystem::path& path) {
    const std::string bytes = ReadFile(path);
    return bytes.size() >= 4U && std::strncmp(bytes.data(), "%PDF", 4) == 0;
}

std::vector<std::filesystem::path> DirectoryEntries(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> entries;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, error)) {
        entries.push_back(entry.path().filename());
    }
    if (error) {
        return {};
    }
    return entries;
}

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

bool ProjectContains(const std::filesystem::path& path, std::u16string_view marker) {
    const std::optional<cnote::Document> document = cnote::ProjectFile::Load(path);
    return document.has_value() && document->PageCount() > 0
        && !document->PageAt(0).texts.empty() && document->PageAt(0).texts.front().content == marker;
}

// --- project save -------------------------------------------------------------

void SuccessfulSaveClearsDirtyAndStopsAutoSave() {
    const std::filesystem::path dir = MakeTempDir("save-success");
    const std::filesystem::path target = dir / "project.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());

    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"saved");
    session->document.SetFilePath(target);
    session->dirty = true;
    session->autoSaveIntervalSeconds = 60U;
    manager.SetSessionAutoSaveInterval(session, 60U);
    manager.UpdateAutoSaveTimer(session);
    Check(manager.IsAutoSaveTimerActive(session), "a dirty saved session arms the autosave timer");

    std::string error;
    Check(controller.SaveTo(target, &error), "save a dirty session to a destination");
    Check(std::filesystem::is_regular_file(target), "the destination file exists after the save");
    Check(!session->dirty, "a successful save clears the shared dirty state");
    Check(session->document.FilePath() == target, "a successful save records the destination path");
    Check(error.empty(), "a successful save reports no error");
    Check(ProjectContains(target, u"saved"), "the saved file reloads with the committed content");
    // The save commits the file and the dirty state before it tells the session
    // manager: the autosave timer of the saved session is disarmed and exactly
    // one ProjectChange::Kind::Saved change is broadcast (plan 6.6).
    Check(!manager.IsAutoSaveTimerActive(session), "a successful save disarms the session autosave timer");
    Check(controller.SavedChangeCount() == 1U, "a successful save broadcasts exactly one Saved change");
}

void RotatedPdfGeometrySurvivesReopen() {
    const std::filesystem::path dir = MakeTempDir("rotated-pdf");
    const std::filesystem::path source = dir / "background.pdf";
    {
        QPdfWriter writer(QString::fromStdString(source.string()));
        writer.setResolution(72);
        writer.setPageSize(QPageSize(QSizeF(120, 200), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter painter(&writer);
        painter.fillRect(QRect(0, 0, 60, 100), Qt::red);
        painter.fillRect(QRect(60, 0, 60, 100), Qt::green);
        painter.fillRect(QRect(0, 100, 60, 100), Qt::blue);
        painter.fillRect(QRect(60, 100, 60, 100), Qt::yellow);
    }

    for (const int turns : { 0, 1, 2, 3, -1 }) {
        cnote::AppManager manager;
        cnote::qt::QtDocumentController controller(manager);
        std::string error;
        if (!controller.OpenFile(source, &error)) {
            Check(false, "open the asymmetric PDF background: " + error);
            return;
        }
        if (turns != 0) {
            Check(controller.RotatePage(0, turns), "rotate the PDF-backed page");
        }
        const cnote::Page& expected = controller.DocumentRef().PageAt(0);
        cnote::render::PageRenderParams params;
        params.pdfBackground = controller.PdfBackground();
        const QImage before = cnote::render::RenderPageSurface(expected, params);
        Check(before.size() == (turns % 2 == 0 ? QSize(120, 200) : QSize(200, 120)),
            "rotated PDF surface uses the page orientation");

        for (const bool embeddedPdf : { false, true }) {
            const std::filesystem::path target = dir / (std::to_string(turns)
                + (embeddedPdf ? ".pdf" : ".cnote"));
            const bool saved = embeddedPdf ? controller.ExportPdf(target, true, &error)
                                          : controller.SaveTo(target, &error);
            Check(saved, "save rotated project: " + target.filename().string());
            if (!saved) {
                continue;
            }
            // A separate manager forces a disk reload instead of joining the
            // already-open session.
            cnote::AppManager reopenedManager;
            cnote::qt::QtDocumentController reopened(reopenedManager);
            if (!reopened.OpenFile(target, &error)) {
                Check(false, "reopen rotated project: " + error);
                continue;
            }
            const cnote::Page& actual = reopened.DocumentRef().PageAt(0);
            Check(actual.width == expected.width && actual.height == expected.height
                    && actual.rotationDegrees == expected.rotationDegrees,
                "reopened PDF-backed page preserves frame and rotation: " + target.filename().string());
            Check(actual.sourcePdfSourceIndex == expected.sourcePdfSourceIndex
                    && actual.sourcePdfPageIndex == expected.sourcePdfPageIndex,
                "reopened page retains its PDF background source");
            params.pdfBackground = reopened.PdfBackground();
            Check(cnote::render::RenderPageSurface(actual, params) == before,
                "reopened PDF background renders without clipping: " + target.filename().string());
        }
    }
}

void FailedSaveKeepsDestinationDirtyAndPath() {
    const std::filesystem::path dir = MakeTempDir("save-readonly-file");
    const std::filesystem::path target = dir / "readonly.cnote";
    const std::filesystem::path previous = dir / "previous.cnote";

    Check(MakeReadOnlyFile(target, "PREVIOUS-BYTES"), "prepare a read-only destination file");
    Check(cnote::ProjectFile::Save(MakeProject(u"previous"), previous), "prepare the previous destination");

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());

    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"replacement");
    session->document.SetFilePath(previous);
    session->dirty = true;
    session->autoSaveIntervalSeconds = 60U;
    manager.SetSessionAutoSaveInterval(session, 60U);
    manager.UpdateAutoSaveTimer(session);
    Check(manager.IsAutoSaveTimerActive(session), "the dirty session has an armed autosave timer before the failed save");

    std::string error;
    Check(!controller.SaveTo(target, &error), "a read-only destination file makes the save fail");
    Check(!error.empty(), "the failed save reports an error");
    Check(session->dirty, "a failed save keeps the shared dirty state");
    Check(session->document.FilePath() == previous, "a failed save keeps the previous destination path");
    Check(ReadFile(target) == "PREVIOUS-BYTES", "the read-only destination is byte-identical after the failed save");
    Check(ProjectContains(previous, u"previous"), "the previous destination file is untouched");
    Check(manager.IsAutoSaveTimerActive(session), "a failed save keeps the autosave timer armed (nothing was committed)");
    Check(controller.SavedChangeCount() == 0U, "a failed save broadcasts no Saved change");
    Check(DirectoryEntries(dir).size() == 2U, "a failed save leaves no temporary file behind");

    MakeWritable(target);
}

#ifndef _WIN32
void FailedSaveKeepsDestinationInReadOnlyDirectory() {
    const std::filesystem::path dir = MakeTempDir("save-readonly-dir");
    const std::filesystem::path locked = dir / "locked";
    std::error_code error;
    std::filesystem::create_directories(locked, error);

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"locked");
    session->dirty = true;

    std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, error);
    Check(!error, "make the destination directory read-only");

    std::string saveError;
    Check(!controller.SaveTo(locked / "new.cnote", &saveError), "a read-only parent directory makes the save fail");
    Check(!saveError.empty(), "the read-only-directory save reports an error");
    Check(session->dirty, "the read-only-directory failure keeps dirty state");
    Check(DirectoryEntries(locked).empty(), "the read-only directory gained no file");

    MakeWritable(locked);
}
#endif

void FailedSaveToDirectoryDestination() {
    const std::filesystem::path dir = MakeTempDir("save-directory-destination");
    const std::filesystem::path target = dir / "a-directory";
    std::error_code error;
    std::filesystem::create_directories(target, error);

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"directory");
    session->dirty = true;

    std::string saveError;
    Check(!controller.SaveTo(target, &saveError), "saving onto an existing directory fails");
    Check(!saveError.empty(), "the directory-destination failure reports an error");
    Check(std::filesystem::is_directory(target), "the directory destination is still a directory");
    Check(session->dirty, "the directory-destination failure keeps dirty state");
    Check(DirectoryEntries(target).empty(), "the directory destination stayed empty");
}

void SaveAfterDestinationDisappears() {
    const std::filesystem::path dir = MakeTempDir("save-vanished");
    const std::filesystem::path target = dir / "vanishing.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"first");
    session->dirty = true;

    std::string error;
    Check(controller.SaveTo(target, &error), "first save creates the destination");
    std::error_code removeError;
    std::filesystem::remove(target, removeError);
    Check(!std::filesystem::exists(target), "the destination disappears before the next save");

    session->document = MakeProject(u"second");
    session->dirty = true;
    Check(controller.SaveTo(target, &error), "saving again recreates the vanished destination");
    Check(!session->dirty, "the recreated save clears dirty state");
    Check(ProjectContains(target, u"second"), "the recreated file holds the new content");
}

void SaveAsToNonAsciiAndLongPaths() {
    const std::filesystem::path dir = MakeTempDir("save-odd-paths");

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();

    const std::filesystem::path nonAscii = dir / cnote::Utf8ToPath("n\xC3\xB6" "te-" "\xCE\xB1" ".cnote");
    session->document = MakeProject(u"non-ascii");
    session->dirty = true;
    std::string error;
    Check(controller.SaveTo(nonAscii, &error), "Save As to a non-ASCII path succeeds");
    Check(!session->dirty, "the non-ASCII save clears dirty state");
    Check(ProjectContains(nonAscii, u"non-ascii"), "the non-ASCII project reloads with its content");

    std::filesystem::path longDir = dir;
    while (longDir.string().size() < 190U) {
        longDir /= "segment";
    }
    std::error_code createError;
    std::filesystem::create_directories(longDir, createError);
    const std::filesystem::path longPath = longDir / "long-name.cnote";
    session->document = MakeProject(u"long");
    session->dirty = true;
    const bool savedLong = controller.SaveTo(longPath, &error);
    if (savedLong) {
        Check(!session->dirty && ProjectContains(longPath, u"long"), "a long path saves and reloads");
    } else {
        Check(session->dirty, "a long path that cannot be saved keeps dirty state");
        Check(!std::filesystem::exists(longPath), "a refused long path leaves no partial file");
    }
    Check(DirectoryEntries(longDir).size() <= 1U, "no temporary file is left in the long-path directory");
}

// A save replaces the destination instead of writing through it: an existing hard
// link to the previous file must keep the previous content (plan 6.6, QSaveFile /
// temp + replace).
void SuccessfulSaveReplacesTheDestination() {
    const std::filesystem::path dir = MakeTempDir("save-replace");
    const std::filesystem::path target = dir / "replace.cnote";
    const std::filesystem::path hardLink = dir / "replace-link.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"first");
    session->dirty = true;
    std::string error;
    Check(controller.SaveTo(target, &error), "first save creates the project");

    std::error_code linkError;
    std::filesystem::create_hard_link(target, hardLink, linkError);
    if (linkError) {
        std::printf("skip a hard link to the project file is not supported here (%s)\n", linkError.message().c_str());
        return;
    }

    session->document = MakeProject(u"second");
    session->dirty = true;
    Check(controller.SaveTo(target, &error), "second save replaces the project");
    Check(ProjectContains(target, u"second"), "the requested destination holds the new content");
    Check(ProjectContains(hardLink, u"first"),
        "the save replaced the destination instead of writing through it (the other link keeps the old content)");
    Check(DirectoryEntries(dir).size() == 2U, "no temporary file is left after the replacing save");
}

// --- PDF export ---------------------------------------------------------------

void PdfExportReplacesOnlyOnSuccess() {
    const std::filesystem::path dir = MakeTempDir("export-success");

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"exported");
    session->dirty = true;

    const std::filesystem::path embedded = dir / "embedded.pdf";
    const std::filesystem::path flat = dir / "flat.pdf";
    std::string error;
    Check(controller.ExportPdf(embedded, true, &error), "export a PDF with the embedded project");
    Check(StartsWithPdfMagic(embedded), "the attachment-bearing export is a PDF");
    Check(controller.ExportPdf(flat, false, &error), "export a PDF without the embedded project");
    Check(StartsWithPdfMagic(flat), "the attachment-free export is a PDF");
    Check(session->dirty, "exporting does not clear the project's dirty state");

    std::vector<std::filesystem::path> entries = DirectoryEntries(dir);
    bool leftover = false;
    for (const std::filesystem::path& entry : entries) {
        if (entry.extension() == ".tmp") {
            leftover = true;
        }
    }
    Check(!leftover, "a successful export leaves no temporary file behind");
}

void PdfExportFailureKeepsDestination() {
    const std::filesystem::path dir = MakeTempDir("export-failure");
    const std::filesystem::path target = dir / "readonly.pdf";
    Check(MakeReadOnlyFile(target, "PREVIOUS-PDF-BYTES"), "prepare a read-only PDF destination");

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"export-failure");
    session->dirty = true;

    std::string error;
#ifdef _WIN32
    // Windows cannot replace a read-only destination (the atomic replace fails),
    // so the previous bytes survive and the caller sees the error.
    Check(!controller.ExportPdf(target, true, &error), "a read-only PDF destination makes the export fail");
    Check(!error.empty(), "the failed export reports an error");
    Check(ReadFile(target) == "PREVIOUS-PDF-BYTES", "the read-only PDF destination is byte-identical");
#else
    // POSIX allows renaming over a read-only file, so the export succeeds and
    // replaces the destination atomically instead of leaving it partial.
    Check(controller.ExportPdf(target, true, &error), "POSIX replaces a read-only PDF destination atomically");
    Check(StartsWithPdfMagic(target), "the replaced destination is a complete PDF");
#endif
    Check(session->dirty, "a failed export keeps the dirty state");

    std::vector<std::filesystem::path> entries = DirectoryEntries(dir);
    bool leftover = false;
    for (const std::filesystem::path& entry : entries) {
        if (entry.extension() == ".tmp") {
            leftover = true;
        }
    }
    Check(!leftover, "a failed export leaves no temporary file behind");

    // A directory destination exercises the replace step: generation succeeds
    // into the temporary file, the replace fails, and the temporary file must be
    // cleaned up again.
    const std::filesystem::path directoryTarget = dir / "directory.pdf";
    std::error_code createError;
    std::filesystem::create_directories(directoryTarget, createError);
    Check(!controller.ExportPdf(directoryTarget, false, &error),
        "exporting onto an existing directory fails at the replace step");
    Check(std::filesystem::is_directory(directoryTarget), "the directory destination is still a directory");
    Check(DirectoryEntries(dir).size() == 2U, "no temporary file survives the failed replace");

    MakeWritable(target);

#ifndef _WIN32
    // POSIX failure case for the export path: the temporary file cannot even be
    // created in a read-only parent directory.
    const std::filesystem::path exportDir = MakeTempDir("export-readonly-dir");
    const std::filesystem::path locked = exportDir / "locked";
    std::error_code lockedError;
    std::filesystem::create_directories(locked, lockedError);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, lockedError);
    Check(!lockedError, "make the export directory read-only");
    Check(!controller.ExportPdf(locked / "out.pdf", false, &error),
        "an export into a read-only directory fails");
    Check(!error.empty(), "the read-only-directory export reports an error");
    Check(DirectoryEntries(locked).empty(), "the read-only export directory gained no file");
    MakeWritable(locked);
#endif
}

// --- recent documents ---------------------------------------------------------

const cnote::settings::Settings& DefaultSettings() {
    static const cnote::settings::Settings settings = [] {
        cnote::settings::Settings value;
        value.palette = cnote::settings::DefaultPalette();
        return value;
    }();
    return settings;
}

void RecentDocumentsRoundTripAndRules() {
    const std::filesystem::path dir = MakeTempDir("recent");

    cnote::settings::Settings settings = DefaultSettings();
    settings.recentDocumentPaths = {
        (dir / "one.cnote").lexically_normal(),
        (dir / "two.pdf").lexically_normal(),
        (dir / "three.cnote").lexically_normal(),
    };
    const std::string text = cnote::settings::SettingsStoreQt::Serialize(settings);
    const cnote::settings::Settings parsed = cnote::settings::SettingsStoreQt::ParseText(text);
    Check(parsed.recentDocumentPaths == settings.recentDocumentPaths,
        "recent document paths survive a settings round trip in order");

    // The parsed list applies the legacy rules: normalise, keep .cnote/.pdf,
    // deduplicate, newest first, cap at ten.
    std::string payload = "palette=0x000000,0xFFFFFF\n";
    for (std::size_t index = 1; index <= 12U; ++index) {
        payload += "recent_document_" + std::to_string(index) + "=" + cnote::PathToUtf8(dir / ("file" + std::to_string(index) + ".cnote")) + "\n";
    }
    payload += "recent_document_2=" + cnote::PathToUtf8(dir / "file2.cnote") + "\n";  // duplicate
    payload += "recent_document_13=" + cnote::PathToUtf8(dir / "ignored.txt") + "\n";
    const cnote::settings::Settings parsedDuplicates = cnote::settings::SettingsStoreQt::ParseText(payload);
    Check(parsedDuplicates.recentDocumentPaths.size() <= cnote::settings::kMaxRecentDocuments,
        "the parsed recent list is capped at ten entries");
    Check(parsedDuplicates.recentDocumentPaths.front() == (dir / "file1.cnote").lexically_normal(),
        "the newest parsed entry stays first");
    for (const std::filesystem::path& path : parsedDuplicates.recentDocumentPaths) {
        Check(path.is_absolute(), "a relative recent entry is normalised to an absolute path");
        Check(path.extension() == ".cnote", "only .cnote/.pdf entries are kept");
    }
    const auto file2 = std::find(parsedDuplicates.recentDocumentPaths.begin(),
        parsedDuplicates.recentDocumentPaths.end(), (dir / "file2.cnote").lexically_normal());
    Check(file2 != parsedDuplicates.recentDocumentPaths.end(), "the deduplicated entry is still present");

    // A settings write goes through QSaveFile: the destination is replaced only
    // after the text was written and committed, and a refused replacement leaves
    // the previous file and no temporary file behind.
    const std::filesystem::path settingsDir = dir / "settings-destination";
    std::error_code mkdirError;
    std::filesystem::create_directories(settingsDir, mkdirError);
    cnote::settings::SettingsStoreQt store(settingsDir);
    cnote::settings::Settings changed = settings;
    changed.drawBrushSize = 9.0f;
    Check(!store.Save(changed), "saving settings onto an existing directory fails at the commit step");
    Check(std::filesystem::is_directory(settingsDir), "the settings destination is still a directory");
    Check(DirectoryEntries(settingsDir).empty(), "a failed settings save leaves no temporary file behind");

    const std::filesystem::path settingsPath = dir / "settings.ini";
    cnote::settings::SettingsStoreQt fileStore(settingsPath);
    Check(fileStore.Save(settings), "save settings to a fresh destination");
    Check(std::filesystem::is_regular_file(settingsPath), "the settings file exists after the save");
    const cnote::settings::Settings reloaded = cnote::settings::SettingsStoreQt::ParseText(ReadFile(settingsPath));
    Check(reloaded.recentDocumentPaths == settings.recentDocumentPaths,
        "the persisted settings keep the recent document list");

    // QSaveFile refuses to replace a destination it cannot write, on both
    // platforms, and the previous file survives untouched.
    const std::string beforeFailure = ReadFile(settingsPath);
    Check(MakeReadOnlyFile(settingsPath, beforeFailure), "make the settings destination read-only");
    Check(!fileStore.Save(changed), "saving settings over a read-only destination is refused");
    Check(ReadFile(settingsPath) == beforeFailure, "the read-only settings file is unchanged");
    MakeWritable(settingsPath);
    Check(DirectoryEntries(dir).size() == 2U, "no temporary settings file is left behind");
}

// --- close and autosave interplay --------------------------------------------

// A modal dialog raised from a close handler must be answered from the test.
class MessageAnswerer {
public:
    MessageAnswerer(QMessageBox::StandardButton button, int maximumAnswers)
        : button_(button), remaining_(maximumAnswers) {
        timer_.setInterval(0);
        QObject::connect(&timer_, &QTimer::timeout, [this]() { Answer(); });
    }

    void Start() { timer_.start(); }
    void Stop() { timer_.stop(); }

    [[nodiscard]] int Answers() const { return answers_; }

private:
    void Answer() {
        if (remaining_ <= 0) {
            timer_.stop();
            return;
        }
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        --remaining_;
        ++answers_;
        if (QAbstractButton* target = box->button(button_)) {
            target->click();
        } else {
            box->reject();
        }
    }

    QMessageBox::StandardButton button_;
    int remaining_;
    int answers_ = 0;
    QTimer timer_;
};

void FailedCloseSaveKeepsWindowAlive() {
    const std::filesystem::path dir = MakeTempDir("close-failure");
    const std::filesystem::path target = dir / "close.cnote";
    Check(cnote::ProjectFile::Save(MakeProject(u"before"), target), "prepare the close target");
    Check(MakeReadOnlyFile(target, ReadFile(target)), "make the close target read-only");

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    Check(window.OpenPath(target), "open the read-only project in a window");
    const std::shared_ptr<cnote::ProjectSession> session = window.Controller().Session();
    session->document.CurrentPage().texts.front().content = u"unsaved-change";
    session->dirty = true;

    MessageAnswerer answerer(QMessageBox::Ok, 2);
    answerer.Start();
    const bool closed = window.close();
    answerer.Stop();

    Check(!closed, "closing the last view is refused when its auto-save fails");
    Check(answerer.Answers() >= 1, "the failed close-save showed an error dialog");
    Check(session->dirty, "the refused close keeps the session dirty");
    Check(!ProjectContains(target, u"unsaved-change"), "the read-only project file was not modified");

    MakeWritable(target);
}

void SuccessfulCloseSavesAndCloses() {
    const std::filesystem::path dir = MakeTempDir("close-success");
    const std::filesystem::path target = dir / "close.cnote";
    Check(cnote::ProjectFile::Save(MakeProject(u"before"), target), "prepare the writable close target");

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    Check(window.OpenPath(target), "open the project in a window");
    const std::shared_ptr<cnote::ProjectSession> session = window.Controller().Session();
    session->document.CurrentPage().texts.front().content = u"changed";
    session->dirty = true;

    const bool closed = window.close();
    Check(closed, "closing the last view succeeds when the auto-save succeeds");
    Check(!session->dirty, "the successful close-save clears dirty state");
    Check(ProjectContains(target, u"changed"), "the successful close-save wrote the new content");
}

void CancelledUnsavedPromptKeepsWindowAlive() {
    const std::filesystem::path dir = MakeTempDir("close-cancel");
    const std::filesystem::path target = dir / "cancel.cnote";
    Check(cnote::ProjectFile::Save(MakeProject(u"before"), target), "prepare the project");

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    Check(window.OpenPath(target), "open the project in a window");
    const std::shared_ptr<cnote::ProjectSession> session = window.Controller().Session();
    session->document.SetFilePath({});  // untitled: the close prompt applies
    session->dirty = true;

    {
        MessageAnswerer answerer(QMessageBox::Cancel, 1);
        answerer.Start();
        const bool closed = window.close();
        answerer.Stop();
        Check(!closed, "cancelling the unsaved-changes prompt keeps the window open");
        Check(answerer.Answers() >= 1, "the unsaved-changes prompt was shown");
    }
    Check(session->dirty, "the cancelled prompt keeps the session dirty");

    {
        MessageAnswerer answerer(QMessageBox::Discard, 1);
        answerer.Start();
        const bool closed = window.close();
        answerer.Stop();
        Check(closed, "choosing Discard closes the window");
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    if (argc == 2 && std::string_view(argv[1]) == "--rotated-pdf") {
        RotatedPdfGeometrySurvivesReopen();
        return gFailures == 0 ? 0 : 1;
    }

    SuccessfulSaveClearsDirtyAndStopsAutoSave();
    FailedSaveKeepsDestinationDirtyAndPath();
#ifndef _WIN32
    FailedSaveKeepsDestinationInReadOnlyDirectory();
#endif
    FailedSaveToDirectoryDestination();
    SaveAfterDestinationDisappears();
    SaveAsToNonAsciiAndLongPaths();
    SuccessfulSaveReplacesTheDestination();

    PdfExportReplacesOnlyOnSuccess();
    PdfExportFailureKeepsDestination();

    RecentDocumentsRoundTripAndRules();

    FailedCloseSaveKeepsWindowAlive();
    SuccessfulCloseSavesAndCloses();
    CancelledUnsavedPromptKeepsWindowAlive();

    if (gFailures != 0) {
        std::printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
