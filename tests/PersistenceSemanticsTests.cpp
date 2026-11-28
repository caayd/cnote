// Persistence and error semantics of the Qt application (migration plan 6.6,
// with 4.5/7.2 for the close and autosave interplay).
//
// Drives the real services (QtDocumentController, AppManager, QtMainWindow,
// SettingsStoreQt) against a temporary directory, a temporary settings file
// (SettingsStoreQt::OverrideSettingsPath) and project files:
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
//     prompt keeps the window alive;
//   * an edit session commits the captured overlay content (and cancels, never
//     erases, without a content source), undo during an edit keeps the object,
//     and a remote commit cancels a stale edit/gesture;
//   * saves serialize an immutable snapshot on the background writer, so an
//     edit made while a save is in flight stays dirty for the next save, a
//     Save As during an in-flight save commits both destinations, a failed
//     asynchronous save keeps the old bytes and reports through the
//     completion, a close waits for the queued write, destroying the manager
//     still commits a requested save, a save started from inside a completion
//     makes delivery progress, and Save/close settle a queued Save As before
//     deciding whether they already have a destination;
//   * SaveTo refuses to overwrite a PDF container (and the save policy routes
//     PDF-backed documents through Save As), and the viewed page a controller
//     sets is what ProjectFile serializes.
//
// What cannot be produced here: a genuine disk-full or interrupted-write
// failure.  Those are covered by the code-path guarantee (the destination is
// replaced only after a successful write) and are called out in the report.

#include <algorithm>
#include <chrono>
#include <cmath>
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
#include <QComboBox>
#include <QEventLoop>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QSlider>
#include <QString>
#include <QTimer>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"
#include "ProjectSaveQueue.h"
#include "ProjectSession.h"
#include "Unicode.h"
#include "document/HistoryReducer.h"
#include "qt/QtDocumentController.h"
#include "latex/LatexEngine.h"
#include "qt/QtMainWindow.h"
#include "settings/SettingsStoreQt.h"
#include "render/PageRenderer.h"
#include "shell/AppManager.h"
#include "text/TextLayoutQt.h"
#include "ui/SelectionMath.h"
#include "ui/NewPageDialogQt.h"
#include "ui/SettingsDialogQt.h"

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

void DefaultPageSizePrecedence() {
    const std::filesystem::path dir = MakeTempDir("page-size");
    cnote::settings::SettingsStoreQt::OverrideSettingsPath(dir / "settings.ini");
    cnote::qt::QtMainWindow::SetHeadlessModals(true);
    cnote::settings::Settings settings;
    settings.defaultPageSizePreset = 1; // Letter Landscape.
    cnote::settings::SettingsStoreQt store;
    Check(store.Save(settings), "persist the app page-size preset before opening windows");

    const auto hasSize = [](const cnote::Page& page, float width, float height) {
        return page.width == width && page.height == height;
    };
    cnote::Document::PageDefaults modelDefaults;
    modelDefaults.width = 400.0f;
    modelDefaults.height = 700.0f;
    Check(hasSize(cnote::Document::CreateBlank(modelDefaults).CurrentPage(), 400, 700),
        "blank document applies its defaults to the first page");

    // Version 15 has the same payload layout but no inheritance metadata.
    // Its stored defaults must remain authoritative when opened by this app.
    modelDefaults.sizeOverridden = true;
    auto legacyBytes = cnote::ProjectFile::Serialize(cnote::Document::CreateBlank(modelDefaults));
    const std::uint32_t legacyVersion = 15;
    std::memcpy(legacyBytes.data() + 8, &legacyVersion, sizeof(legacyVersion));
    auto legacy = cnote::ProjectFile::LoadFromBytes(legacyBytes, dir / "legacy.cnote");
    Check(legacy.has_value() && legacy->NewPageDefaults().sizeOverridden
            && legacy->NewPageDefaults().width == 400 && legacy->NewPageDefaults().height == 700,
        "legacy projects retain their stored future-page size");

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    cnote::qt::QtMainWindow other(manager);
    Check(hasSize(window.Controller().DocumentRef().PageAt(0), 792, 612),
        "startup project uses the saved app preset for its first page");
    window.AddPage();
    Check(hasSize(window.Controller().DocumentRef().PageAt(1), 792, 612),
        "new pages inherit the app preset");

    window.ToggleSettingsDialog();
    auto* combo = window.findChild<QComboBox*>(QStringLiteral("defaultPageSizeCombo"));
    Check(combo != nullptr, "the drawing settings expose the page-size preset");
    if (combo == nullptr) {
        return;
    }
    combo->setCurrentIndex(2); // Legal Portrait.
    window.AddPage();
    other.AddPage();
    Check(hasSize(window.Controller().DocumentRef().PageAt(2), 612, 1008)
            && hasSize(other.Controller().DocumentRef().PageAt(1), 612, 1008),
        "changing the app preset affects future pages in every open window");
    Check(hasSize(window.Controller().DocumentRef().PageAt(0), 792, 612),
        "changing the app preset never resizes existing pages");

    const auto inheritedPath = dir / "inherited.cnote";
    std::string error;
    Check(window.Controller().SaveTo(inheritedPath, &error), "save an inheriting project");
    combo->setCurrentIndex(4); // A4 Landscape.
    cnote::qt::QtMainWindow reopened(manager);
    // Controller open forces a disk load rather than joining the live session.
    Check(reopened.Controller().OpenFile(inheritedPath, &error), "reload an inheriting project from disk");
    reopened.AddPage();
    Check(hasSize(reopened.Controller().DocumentRef().PageAt(reopened.Controller().CurrentPageIndex()), 842, 595),
        "a reopened inheriting project follows the current app preset");

    // Explicitly choosing even the current effective size must pin the project.
    window.Controller().SetNewPageDefaults(window.Controller().NewPageDefaults());
    combo->setCurrentIndex(0); // Letter Portrait.
    window.AddPage();
    Check(hasSize(window.Controller().DocumentRef().PageAt(3), 842, 595),
        "explicit project defaults override later app preset changes");
    window.Controller().SetNewPageDefaults({ 410, 730, 0xFFFFFFFFU, {} });
    const auto customPath = dir / "custom.cnote";
    const auto embeddedPath = dir / "custom.pdf";
    Check(window.Controller().SaveTo(customPath, &error), "save custom project page defaults");
    Check(window.Controller().ExportPdf(embeddedPath, true, &error), "embed custom project page defaults in PDF");
    for (const auto& path : { customPath, embeddedPath }) {
        Check(reopened.Controller().OpenFile(path, &error), "reload an explicit project page-size override");
        reopened.AddPage();
        const auto& document = reopened.Controller().DocumentRef();
        Check(hasSize(document.PageAt(reopened.Controller().CurrentPageIndex()), 410, 730),
            "custom future-page size survives native and embedded-project round trips");
    }
    window.NewDocument();
    Check(hasSize(window.Controller().DocumentRef().PageAt(0), 612, 792),
        "New Project discards the old project override and uses the app preset");

    // Plain PDF import must retain source geometry but inherit the app preset
    // for appended blank pages.
    const auto plainPdf = dir / "plain.pdf";
    {
        QPdfWriter writer(QString::fromStdString(plainPdf.string()));
        writer.setResolution(72);
        writer.setPageSize(QPageSize(QSizeF(300, 500), QPageSize::Point));
        QPainter painter(&writer);
        painter.fillRect(QRect(0, 0, 300, 500), Qt::white);
    }
    Check(window.OpenPath(plainPdf), "import a PDF without project defaults");
    window.AddPage();
    Check(hasSize(window.Controller().DocumentRef().PageAt(0), 300, 500)
            && hasSize(window.Controller().DocumentRef().PageAt(1), 612, 792),
        "PDF import preserves source geometry and uses app defaults for blank pages");

    // A discarded dialog draft must not leak into another project's Add Page.
    window.OpenNewPageSettings();
    auto* pageDialog = window.findChild<cnote::ui::NewPageDialogQt*>();
    pageDialog->setDraft({ 900, 900, 0xFFFFFFFFU, {} });
    pageDialog->hide();
    window.NewDocument();
    window.AddPage();
    Check(hasSize(window.Controller().DocumentRef().PageAt(1), 612, 792),
        "a hidden old page-settings draft cannot override a new project's preset");
}

void AutoSaveIntervalFollowsSettings() {
    const std::filesystem::path dir = MakeTempDir("autosave-settings");
    cnote::settings::SettingsStoreQt::OverrideSettingsPath(dir / "settings.ini");
    cnote::settings::Settings settings;
    settings.autoSaveIntervalSeconds = 0U;
    cnote::settings::SettingsStoreQt store;
    Check(store.Save(settings), "persist Off before opening the windows");
    // Restore the process-wide override: the close-failure tests below need a
    // real modal box to answer.
    const bool headless = cnote::qt::QtMainWindow::HeadlessModals();
    cnote::qt::QtMainWindow::SetHeadlessModals(true);

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    cnote::qt::QtMainWindow other(manager);
    const std::shared_ptr<cnote::ProjectSession> windowSession = window.Controller().Session();
    const std::shared_ptr<cnote::ProjectSession> otherSession = other.Controller().Session();
    Check(windowSession != nullptr && otherSession != nullptr && windowSession != otherSession,
        "each window has its own session");

    std::string error;
    Check(window.Controller().SaveTo(dir / "window.cnote", &error), "save the first window's session");
    Check(other.Controller().SaveTo(dir / "other.cnote", &error), "save the second window's session");
    window.AddPage();
    other.AddPage();
    Check(windowSession->dirty && otherSession->dirty, "both sessions have unsaved edits");
    Check(!manager.IsAutoSaveTimerActive(windowSession) && !manager.IsAutoSaveTimerActive(otherSession),
        "the stored Off setting disables auto-save in every open window");

    window.ToggleSettingsDialog();
    auto* slider = window.findChild<QSlider*>(QStringLiteral("autoSaveSlider"));
    Check(slider != nullptr, "the settings dialog exposes the auto-save slider");
    if (slider == nullptr) {
        cnote::qt::QtMainWindow::SetHeadlessModals(headless);
        return;
    }
    slider->setValue(15);
    Check(store.Load().autoSaveIntervalSeconds == 15U, "the selected auto-save interval persists");
    Check(manager.AutoSaveInterval() == 15U, "the selected auto-save interval reaches the manager");
    Check(manager.IsAutoSaveTimerActive(windowSession) && manager.IsAutoSaveTimerActive(otherSession),
        "an interval change arms auto-save in every open window");
    slider->setValue(0);
    Check(!manager.IsAutoSaveTimerActive(windowSession) && !manager.IsAutoSaveTimerActive(otherSession),
        "Off reaches every open window again");
    cnote::qt::QtMainWindow::SetHeadlessModals(headless);
}

void AutoSaveReportsFailureOncePerStreak() {
    const std::filesystem::path dir = MakeTempDir("autosave-failure");
    const std::filesystem::path unwritable = dir / "missing" / "note.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"autosave");
    session->document.SetFilePath(unwritable);
    session->dirty = true;

    int reports = 0;
    std::string reportedError;
    controller.SetAutoSaveErrorHandler([&](const QString& reason) {
        ++reports;
        reportedError = reason.toStdString();
    });
    // The shell's production dispatcher: the manager owns the streak and the
    // interaction deferral, the view only shows the report.
    manager.SetAutoSaveFailureHandler([&](QObject* owner, const std::string& error) {
        if (auto* view = qobject_cast<cnote::qt::QtDocumentController*>(owner)) {
            view->ShowAutoSaveFailure(error);
        }
    });

    controller.AutoSave();
    manager.ProjectSaves().WaitForIdle();
    Check(reports == 1, "an auto-save failure is reported");
    Check(!reportedError.empty(), "the auto-save failure carries a reason");
    Check(session->dirty, "a failed auto-save keeps the session dirty");
    Check(session->autoSaveFailureReported, "the failure streak is marked");

    controller.AutoSave();
    manager.ProjectSaves().WaitForIdle();
    Check(reports == 1, "a repeated failure in the same streak is not reported again");

    // A successful write ends the streak, so the next failure reports again.
    std::string error;
    Check(controller.SaveTo(dir / "note.cnote", &error), "a writable destination saves");
    Check(!session->autoSaveFailureReported, "a successful write clears the failure streak");
    session->document.SetFilePath(unwritable);
    session->dirty = true;
    controller.AutoSave();
    manager.ProjectSaves().WaitForIdle();
    Check(reports == 2 && session->dirty, "a failure after a successful write is reported again");

    // A refused auto-save (a PDF-backed document saved in place) is not an
    // error and must not raise a dialog.
    session->document.SetFilePath(dir / "scan.pdf");
    session->document.SetKind(cnote::DocumentKind::PdfContainer);
    session->dirty = true;
    controller.AutoSave();
    manager.ProjectSaves().WaitForIdle();
    Check(reports == 2, "a refused auto-save is not reported as an error");
}

// The whole timer-to-disk path: the manager's deadline calls the handler, the
// handler queues the write, and the successful completion clears dirty.
void AutoSaveWritesTheDirtyRevision() {
    const std::filesystem::path dir = MakeTempDir("autosave-writes");
    const std::filesystem::path target = dir / "auto.cnote";
    Check(cnote::ProjectFile::Save(MakeProject(u"before"), target), "prepare the autosave target");

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    std::string error;
    Check(controller.OpenFile(target, &error), "open the autosave target");
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    manager.SetAutoSaveHandler([&controller](const std::shared_ptr<cnote::ProjectSession>&, QObject*) {
        controller.AutoSave();
    });
    manager.SetAutoSaveInterval(1);

    session->document.CurrentPage().texts.front().content = u"written-by-autosave";
    ++session->revision;
    session->dirty = true;
    manager.UpdateAutoSaveTimer(session);
    Check(manager.IsAutoSaveTimerActive(session), "a dirty saved session arms auto-save");

    QEventLoop loop;
    QTimer::singleShot(1800, &loop, &QEventLoop::quit);
    loop.exec();
    manager.ProjectSaves().WaitForIdle();

    Check(!session->dirty, "the autosave cleared dirty after writing the current revision");
    Check(!manager.IsAutoSaveTimerActive(session), "the successful autosave disarmed the timer");
    Check(ProjectContains(target, u"written-by-autosave"), "the autosave wrote the committed edit");
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

// --- background save ----------------------------------------------------------

void EditDuringSaveKeepsSessionDirty() {
    const std::filesystem::path dir = MakeTempDir("save-in-flight-edit");
    const std::filesystem::path target = dir / "in-flight.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"before-save");
    session->document.SetFilePath(target);
    session->revision = 1U;
    session->dirty = true;
    manager.UpdateAutoSaveTimer(session);

    Check(controller.RequestSave(target, nullptr), "an asynchronous save request starts");
    Check(manager.ProjectSaves().PendingCount() > 0U,
        "the save request returns while its write is still queued");

    // The edit lands after the request took its snapshot and before the
    // completion applies it.
    session->document.CurrentPage().texts.front().content = u"after-save";
    session->revision = 2U;
    session->dirty = true;
    manager.ProjectSaves().WaitForIdle();

    Check(ProjectContains(target, u"before-save"), "the save wrote the snapshot taken when it started");
    Check(session->dirty, "an edit made during the save keeps the session dirty");
    Check(session->document.FilePath() == target, "the committed write records its destination");
    Check(manager.IsAutoSaveTimerActive(session),
        "the autosave timer stays armed for the revision written after the save");
    Check(controller.SavedChangeCount() == 1U, "the committed save broadcasts one Saved change");

    std::string error;
    Check(controller.SaveTo(target, &error), "saving again writes the current revision");
    Check(!session->dirty && ProjectContains(target, u"after-save"),
        "the next save commits the edit and clears dirty");
}

void SaveAsDuringInFlightSaveCommitsBothDestinations() {
    const std::filesystem::path dir = MakeTempDir("save-as-in-flight");
    const std::filesystem::path first = dir / "first.cnote";
    const std::filesystem::path second = dir / "second.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"first");
    session->document.SetFilePath(first);
    session->revision = 1U;
    session->dirty = true;

    Check(controller.RequestSave(first, nullptr), "start a save of the current revision");
    session->document.CurrentPage().texts.front().content = u"second";
    session->revision = 2U;
    Check(controller.RequestSave(second, nullptr), "start Save As while the first write is queued");
    manager.ProjectSaves().WaitForIdle();

    Check(ProjectContains(first, u"first"), "the queued save wrote its own snapshot");
    Check(ProjectContains(second, u"second"), "the Save As wrote the later snapshot");
    Check(session->document.FilePath() == second, "the newest destination is the session path");
    Check(!session->dirty, "the newest revision's save clears dirty");
    Check(controller.SavedChangeCount() == 2U, "both committed writes broadcast a Saved change");
}

void FailedAsynchronousSaveKeepsDirtyAndReports() {
    const std::filesystem::path dir = MakeTempDir("save-async-failure");
    const std::filesystem::path target = dir / "locked.cnote";
    Check(MakeReadOnlyFile(target, "PREVIOUS-BYTES"), "prepare the read-only destination");

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"new-content");
    session->revision = 1U;
    session->dirty = true;

    bool completed = false;
    bool failed = false;
    std::string reported;
    Check(controller.RequestSave(target, nullptr,
              [&](const cnote::qt::QtDocumentController::SaveOutcome& outcome) {
                  completed = true;
                  failed = !outcome.success;
                  reported = outcome.error;
              }),
        "the asynchronous save request starts");
    Check(!completed, "the request returns before the completion runs");
    manager.ProjectSaves().WaitForIdle();

    Check(completed && failed, "the completion reports the write failure");
    Check(!reported.empty(), "the asynchronous failure carries an error message");
    Check(session->dirty, "a failed asynchronous save keeps dirty");
    Check(ReadFile(target) == "PREVIOUS-BYTES", "the read-only destination is unchanged");
    Check(controller.SavedChangeCount() == 0U, "a failed save broadcasts no Saved change");
    MakeWritable(target);
}

void DestroyingManagerDrainsAPendingSave() {
    const std::filesystem::path dir = MakeTempDir("save-quit-drain");
    const std::filesystem::path target = dir / "quit.cnote";
    {
        cnote::AppManager manager;
        cnote::qt::QtDocumentController controller(manager);
        controller.AttachSession(std::make_shared<cnote::ProjectSession>());
        const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
        session->document = MakeProject(u"quit");
        session->document.SetFilePath(target);
        session->revision = 1U;
        session->dirty = true;
        Check(controller.RequestSave(target, nullptr), "queue a save and quit before it completes");
    }
    Check(ProjectContains(target, u"quit"), "destroying the manager finished the queued write");
}

// A completion may run a nested event loop - ReportError shows a modal dialog
// on failure - and that loop can reach a second save while another written
// result is still undelivered.  Delivery must make progress from the nested
// call instead of waiting on itself.
void SaveFromInsideACompletionDeliversQueuedResults() {
    const std::filesystem::path dir = MakeTempDir("save-reentrant");
    const std::filesystem::path first = dir / "first.cnote";
    const std::filesystem::path second = dir / "second.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document = MakeProject(u"snapshot");
    session->revision = 1U;
    session->dirty = true;

    Check(controller.RequestSave(first, nullptr), "queue the first save");
    bool innerRan = false;
    bool innerSaved = false;
    std::string innerError;
    Check(controller.RequestSave(second, nullptr,
              [&](const cnote::qt::QtDocumentController::SaveOutcome&) {
                  innerRan = true;
                  innerSaved = controller.SaveTo(first, &innerError);
              }),
        "queue a save whose completion saves again");
    manager.ProjectSaves().WaitForIdle();

    Check(innerRan, "the completion ran");
    Check(innerSaved, "a save requested from inside a completion commits: " + innerError);
    Check(ProjectContains(first, u"snapshot") && ProjectContains(second, u"snapshot"),
        "the nested save and the queued save both wrote their snapshots");
    Check(!session->dirty, "the nested save cleared dirty for the saved revision");
    Check(session->document.FilePath() == first, "the nested save recorded its destination");
    Check(controller.SavedChangeCount() == 3U, "every committed write reported a Saved change");
}

// Save As queues its write and only then records the destination.  A Save that
// follows immediately must settle that write and save in place instead of
// reopening the file dialog.
void SaveAfterSaveAsUsesTheChosenPath() {
    const std::filesystem::path dir = MakeTempDir("save-after-save-as");
    const std::filesystem::path target = dir / "chosen.cnote";

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    const std::shared_ptr<cnote::ProjectSession> session = window.Controller().Session();
    session->document = MakeProject(u"first");
    session->revision = 1U;
    session->dirty = true;

    // What Save As does after the dialog: queue the write to the chosen path.
    Check(window.Controller().RequestSave(target, nullptr), "queue the Save As write");
    session->document.CurrentPage().texts.front().content = u"second";
    session->revision = 2U;
    session->dirty = true;

    const bool headless = cnote::qt::QtMainWindow::HeadlessModals();
    cnote::qt::QtMainWindow::SetHeadlessModals(true);
    window.Save();
    cnote::qt::QtMainWindow::SetHeadlessModals(headless);
    manager.ProjectSaves().WaitForIdle();

    Check(window.Controller().FilePath() == target, "Save keeps the Save As destination");
    Check(ProjectContains(target, u"second"), "Save wrote the current revision to the chosen path");
    Check(!session->dirty, "the in-place save cleared dirty");
    Check(window.Controller().SavedChangeCount() == 2U, "both writes reported a Saved change");
}

// The same stale state in the close decision: the queued Save As already chose
// a path, so the last view must save in place, not ask "save changes?" again.
void CloseAfterSaveAsUsesTheChosenPath() {
    const std::filesystem::path dir = MakeTempDir("close-after-save-as");
    const std::filesystem::path target = dir / "close-chosen.cnote";

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    const std::shared_ptr<cnote::ProjectSession> session = window.Controller().Session();
    session->document = MakeProject(u"first");
    session->revision = 1U;
    session->dirty = true;

    Check(window.Controller().RequestSave(target, nullptr), "queue the Save As write");
    session->document.CurrentPage().texts.front().content = u"second";
    session->revision = 2U;
    session->dirty = true;

    bool prompted = false;
    window.SetCloseDecisionProvider([&prompted]() {
        prompted = true;
        return cnote::qt::CloseDecision::Cancel;
    });
    Check(window.close(), "closing the last view after Save As succeeds");
    Check(!prompted, "the close used the chosen Save As destination instead of prompting");
    Check(ProjectContains(target, u"second"), "the close save wrote the current revision to the chosen path");
    Check(!session->dirty, "the close save cleared dirty");
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

// A manual save returns to the event loop before its write commits; closing
// the last view while that write is queued must still finish it, so the window
// never goes away ahead of its own save.
void CloseWaitsForInFlightSave() {
    const std::filesystem::path dir = MakeTempDir("close-in-flight");
    const std::filesystem::path target = dir / "close-in-flight.cnote";
    Check(cnote::ProjectFile::Save(MakeProject(u"before"), target), "prepare the close-during-save target");

    cnote::AppManager manager;
    cnote::qt::QtMainWindow window(manager);
    Check(window.OpenPath(target), "open the project in a window");
    const std::shared_ptr<cnote::ProjectSession> session = window.Controller().Session();
    session->document.CurrentPage().texts.front().content = u"during-save";
    ++session->revision;
    session->dirty = true;

    window.Save();
    Check(manager.ProjectSaves().PendingCount() > 0U,
        "the manual save is still queued when the window closes");
    Check(window.close(), "closing the last view during an in-flight save succeeds");
    Check(ProjectContains(target, u"during-save"), "the close waited for the queued write to commit");
    Check(!session->dirty, "the successful close save cleared dirty");
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

// --- edit session, save policy and viewed-page persistence --------------------

// Stub overlay content: the controller captures through EditContentSource when
// a manager-driven finish ends an edit.
class StubEditContentSource final : public cnote::ui::EditContentSource {
public:
    cnote::PageText text {};
    QString latex {};

    [[nodiscard]] cnote::PageText CaptureEditedText() const override { return text; }
    [[nodiscard]] QString CaptureEditedLatexSource() const override { return latex; }
};

cnote::PageText MakeText(std::uint64_t id, std::u16string_view content) {
    cnote::PageText text {};
    text.id = id;
    text.x = 10.0f;
    text.y = 20.0f;
    text.width = 120.0f;
    text.content = std::u16string(content);
    return text;
}

cnote::Stroke MakeStrokeAt(float x, float y) {
    cnote::Stroke stroke;
    stroke.points = { { x, y }, { x + 10.0f, y + 10.0f } };
    return stroke;
}

void EditSessionCommitsAndUndoKeepsTheObject() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    session->document.CurrentPage().texts.push_back(MakeText(7, u"original"));

    StubEditContentSource source;
    controller.SetEditContentSource(&source);

    // Manager-driven finish: the open editor's content must be committed, not
    // the old empty fallback (which erased the object).
    Check(controller.BeginTextEdit(0, 0), "begin editing the existing text");
    Check(controller.IsInteractionActive(), "an open edit is an active interaction");
    source.text = *controller.EditingTextObject();
    source.text.content = u"typed";
    controller.FinishInteraction();
    Check(!controller.EditingText(), "finishing closes the edit session");
    Check(!controller.IsInteractionActive(), "finishing leaves no transient state");
    Check(session->document.CurrentPage().texts.size() == 1U
            && session->document.CurrentPage().texts.front().content == u"typed",
        "a controller-initiated finish preserves the typed content");
    Check(session->undoActions.size() == 1U
            && session->undoActions.back().type == cnote::HistoryActionType::UpdateText,
        "the finished edit is one UpdateText history step");

    // Undo during an edit must keep the object: the old path committed an empty
    // PageText before undoing, which erased it without a history step.
    Check(controller.BeginTextEdit(0, 0), "reopen the text for undo");
    source.text = *controller.EditingTextObject();
    Check(controller.Undo(), "undo during an edit applies the history step");
    Check(session->document.CurrentPage().texts.size() == 1U,
        "undo during an edit does not delete the object");
    Check(session->document.CurrentPage().texts.front().content == u"original",
        "undo during an edit reverts the committed text");

    // Without a content source the edit is cancelled, never committed empty.
    controller.SetEditContentSource(nullptr);
    Check(controller.BeginTextEdit(0, 0), "reopen the text without a content source");
    controller.FinishInteraction();
    Check(!controller.EditingText() && session->document.CurrentPage().texts.size() == 1U
            && session->document.CurrentPage().texts.front().content == u"original",
        "finishing without a content source cancels instead of erasing");
}

// A page turn carries every object with the page: its box (a text's stored
// width and laid-out height, a formula's measured size, an image's stored
// size) keeps its size and is re-centred on the turned centre, and the
// object turns about that centre.  LaTeX used to be offset by half the
// difference of its sides, and images had their sides swapped (stretched).
void RotatedPageKeepsObjectBoxes() {
    Check(cnote::latex::IsInitialized()
            || cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT)),
        "the LaTeX engine starts for the page-turn check");
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    cnote::Page& page = controller.Session()->document.CurrentPage();
    page.texts.push_back(MakeText(7, u"a text that wraps over lines"));
    cnote::PageLatex latex {};
    latex.id = 8;
    latex.x = 300.0f;
    latex.y = 400.0f;
    latex.content = u"x^2 + y^2 + z^2 = r^2";
    page.latexes.push_back(latex);
    cnote::PageImage image {};
    image.x = 50.0f;
    image.y = 600.0f;
    image.width = 160.0f;
    image.height = 40.0f;
    page.images.push_back(image);
    const float pageWidth = page.width;
    const float pageHeight = page.height;

    const auto turnedCentre = [&](float left, float top, float right, float bottom) {
        return cnote::RotatePointQuarterTurn({ (left + right) * 0.5f, (top + bottom) * 0.5f }, pageWidth, pageHeight, 1);
    };
    const auto same = [](float a, float b) { return std::abs(a - b) < 0.01f; };
    const cnote::text::QtTextBounds textBefore = cnote::text::MeasurePageTextBoundsQt(page.texts.front(), pageWidth);
    const cnote::Vec2 textExpected = turnedCentre(textBefore.left, textBefore.top, textBefore.right, textBefore.bottom);
    const cnote::ui::LatexBounds latexBefore = cnote::ui::MeasurePageLatexBounds(page.latexes.front(), pageWidth);
    Check(latexBefore.right - latexBefore.left > 2.0f * (latexBefore.bottom - latexBefore.top),
        "the formula is much wider than tall, so a swapped box would show");
    const cnote::Vec2 latexExpected = turnedCentre(latexBefore.left, latexBefore.top, latexBefore.right, latexBefore.bottom);
    const cnote::Vec2 imageExpected = turnedCentre(image.x, image.y, image.x + image.width, image.y + image.height);

    Check(controller.RotatePage(0, 1), "turn the page a quarter clockwise");
    const cnote::Page& turnedPage = controller.DocumentRef().PageAt(0);

    const cnote::PageText& text = turnedPage.texts.front();
    const cnote::text::QtTextBounds textAfter = cnote::text::MeasurePageTextBoundsQt(text, pageHeight);
    Check(text.width == 120.0f, "a turned page keeps the text's wrap width");
    Check(same((textAfter.left + textAfter.right) * 0.5f, textExpected.x)
            && same((textAfter.top + textAfter.bottom) * 0.5f, textExpected.y),
        "the text's box centre turns with the page");
    Check(text.rotationDegrees == 90.0f, "the text turns with the page");

    const cnote::ui::LatexBounds latexAfter = cnote::ui::MeasurePageLatexBounds(turnedPage.latexes.front(), pageHeight);
    Check(same((latexAfter.left + latexAfter.right) * 0.5f, latexExpected.x)
            && same((latexAfter.top + latexAfter.bottom) * 0.5f, latexExpected.y),
        "the formula's box centre turns with the page");
    Check(turnedPage.latexes.front().rotationDegrees == 90.0f, "the formula turns with the page");

    const cnote::PageImage& turnedImage = turnedPage.images.front();
    Check(turnedImage.width == 160.0f && turnedImage.height == 40.0f,
        "the image keeps its box, so its pixels are not stretched");
    Check(same(turnedImage.x + turnedImage.width * 0.5f, imageExpected.x)
            && same(turnedImage.y + turnedImage.height * 0.5f, imageExpected.y),
        "the image's centre turns with the page");
    Check(turnedImage.rotationDegrees == 90.0f, "the image turns with the page");
}

void RemoteCommitCancelsTransientState() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController first(manager);
    cnote::qt::QtDocumentController second(manager);
    auto session = std::make_shared<cnote::ProjectSession>();
    first.AttachSession(session);
    second.AttachSession(session);
    session->document.CurrentPage().texts.push_back(MakeText(9, u"keep"));

    StubEditContentSource source;
    source.text = MakeText(9, u"stale");
    first.SetEditContentSource(&source);
    Check(first.BeginTextEdit(0, 0), "the first view starts editing");
    first.BeginPageGesture(0);

    // A commit from the other view must drop the first view's transient state
    // without writing its (now stale) edit back by index.
    Check(second.InsertPages(0, 1, cnote::ui::NewPageDraft { 200.0f, 300.0f, 0xFFFFFFFFU, {} }) == 0U,
        "the second view commits a page insert");
    Check(!first.EditingText(), "a remote commit cancels the stale edit");
    Check(!first.GesturePageIndex().has_value(), "a remote commit cancels the stale gesture");
    Check(first.DocumentRef().PageCount() == 2U, "the remote commit's page insert is kept");
    Check(first.DocumentRef().PageAt(1).texts.size() == 1U
            && first.DocumentRef().PageAt(1).texts.front().content == u"keep",
        "the stale edit was never committed by index");
}

// One session, two views: every commit one view makes must leave the other
// view's selection naming the same objects (or cleared where no exact mapping
// exists), never the index positions of the objects it used to name.  The
// committed object ids and marker coordinates prove the named objects, not just
// that some index survived.
void RemoteCommitCarriesTheOtherViewsSelection() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController first(manager);
    cnote::qt::QtDocumentController second(manager);
    auto session = std::make_shared<cnote::ProjectSession>();
    first.AttachSession(session);
    second.AttachSession(session);

    std::size_t secondSelectionChanges = 0U;
    QObject::connect(&second, &cnote::qt::QtDocumentController::SelectionChanged, &second,
        [&secondSelectionChanges]() { ++secondSelectionChanges; });

    // Three pages with distinct widths so a page key can be checked by content.
    first.InsertPages(1, 1, cnote::ui::NewPageDraft { 600.0f, 500.0f, 0xFFFFFFFFU, {} });
    first.InsertPages(2, 1, cnote::ui::NewPageDraft { 700.0f, 500.0f, 0xFFFFFFFFU, {} });
    cnote::Document& document = session->document;
    cnote::Page& page = document.PageAt(0);
    page.strokes = { MakeStrokeAt(0.0f, 0.0f), MakeStrokeAt(10.0f, 10.0f), MakeStrokeAt(20.0f, 20.0f),
        MakeStrokeAt(30.0f, 30.0f), MakeStrokeAt(40.0f, 40.0f) };
    page.texts = { MakeText(100, u"zero"), MakeText(101, u"one"), MakeText(102, u"two") };
    document.PageAt(1).strokes.push_back(MakeStrokeAt(0.0f, 1000.0f));
    document.PageAt(2).strokes.push_back(MakeStrokeAt(0.0f, 2000.0f));

    cnote::ui::PageSelection selection;
    selection.strokes = { 1U, 3U };
    selection.texts = { 2U };
    second.SelectObjects(0, selection);
    cnote::ui::PageSelection markerSelection;
    markerSelection.strokes = { 0U };
    second.SelectObjects(2, markerSelection);
    cnote::ui::PageSelection firstSelection;
    firstSelection.strokes = { 4U };
    first.SelectObjects(0, firstSelection);
    Check(secondSelectionChanges == 2U, "the second view's own selections announce themselves");

    const auto selectedStrokeY = [&](std::size_t pageIndex, std::size_t selectionIndex) {
        const cnote::ui::PageSelection& selected = second.SelectionFor(pageIndex);
        return document.PageAt(pageIndex).strokes[selected.strokes[selectionIndex]].points.front().y;
    };
    const auto selectedTextId = [&](std::size_t pageIndex, std::size_t selectionIndex) {
        const cnote::ui::PageSelection& selected = second.SelectionFor(pageIndex);
        return document.PageAt(pageIndex).texts[selected.texts[selectionIndex]].id;
    };

    // Erase (a ReplaceObjects removal) from the first view.
    first.CommitErase({ cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Stroke, 0U },
        cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Text, 0U } });
    Check(document.PageAt(0).strokes.size() == 4U && document.PageAt(0).texts.size() == 2U,
        "the first view erases a stroke and a text");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 2U },
        "the remote selection's stroke indices shift down with the erase");
    Check(second.SelectionFor(0).texts == std::vector<std::size_t> { 1U },
        "the remote selection's text index shifts down with the erase");
    Check(selectedStrokeY(0, 0U) == 10.0f && selectedStrokeY(0, 1U) == 30.0f,
        "the shifted stroke indices still name the originally selected strokes");
    Check(selectedTextId(0, 0U) == 102U, "the shifted text index still names the originally selected text");
    Check(second.SelectionFor(2).strokes == std::vector<std::size_t> { 0U },
        "a page the erase did not touch keeps its selection");
    Check(first.SelectionFor(0).strokes == std::vector<std::size_t> { 3U },
        "the erasing view's own selection still shifts as before");
    Check(secondSelectionChanges == 3U, "the remapped selection announces one change");

    // Paste (a ReplaceObjects insertion appended at the end) from the first view.
    cnote::SelectionClipboardData payload;
    cnote::Stroke pastedStroke = MakeStrokeAt(200.0f, 200.0f);
    payload.strokes.push_back(pastedStroke);
    payload.minX = 200.0f;
    payload.minY = 200.0f;
    payload.maxX = 210.0f;
    payload.maxY = 210.0f;
    Check(first.PastePayload(0, cnote::Vec2 { 300.0f, 300.0f }, payload), "the first view pastes a stroke");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 2U },
        "an appended paste leaves the remote selection on the same objects");
    Check(selectedStrokeY(0, 0U) == 10.0f && selectedStrokeY(0, 1U) == 30.0f,
        "the remote selection still names its strokes after the paste");
    Check(secondSelectionChanges == 3U, "a selection the paste did not move needs no announcement");

    // Delete from the first view: its stroke 1 is the second view's neighbor.
    cnote::ui::PageSelection deleteSelection;
    deleteSelection.strokes = { 1U };
    first.SelectObjects(0, deleteSelection);
    Check(first.RemoveSelection(), "the first view deletes a stroke");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 1U },
        "the remote selection's survivor shifts into the deleted object's place");
    Check(selectedStrokeY(0, 0U) == 10.0f && selectedStrokeY(0, 1U) == 30.0f,
        "the surviving remote selection still names its strokes after the delete");
    Check(selectedTextId(0, 0U) == 102U, "an untouched kind keeps its remote selection after the delete");
    Check(secondSelectionChanges == 4U, "the delete announces the remote selection change");

    // Undo and redo of that delete from the other view.
    Check(first.Undo(), "the first view undoes its delete");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 2U },
        "undo reinserts the object before the remote selection's survivor");
    Check(selectedStrokeY(0, 1U) == 30.0f, "the remote selection still names its stroke after undo");
    Check(first.Redo(), "the first view redoes its delete");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 1U },
        "redo shifts the remote selection back with the object");
    Check(selectedStrokeY(0, 1U) == 30.0f, "the remote selection still names its stroke after redo");

    // Page reorder from the first view: both selected pages must follow theirs.
    const std::size_t changesBeforeReorder = secondSelectionChanges;
    Check(first.MovePages({ 0U }, 3U).has_value(), "the first view moves page 0 to the end");
    Check(second.SelectionFor(0).Empty(), "the page-key remap leaves no selection on the old key");
    Check(second.SelectionFor(2).strokes == std::vector<std::size_t> { 0U, 1U },
        "the moved page's selection follows it to its new key");
    Check(second.SelectionFor(2).texts == std::vector<std::size_t> { 1U },
        "the moved page's text selection follows it too");
    Check(selectedStrokeY(2, 0U) == 10.0f && selectedStrokeY(2, 1U) == 30.0f,
        "the moved key still names the same objects");
    Check(second.SelectionFor(1).strokes == std::vector<std::size_t> { 0U }
            && document.PageAt(1).strokes[0].points.front().y == 2000.0f,
        "the other selected page followed its own move");
    Check(secondSelectionChanges == changesBeforeReorder + 1U,
        "the page reorder announces one remote selection change");

    Check(first.Undo(), "the page reorder undoes from the other view");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 1U }
            && second.SelectionFor(2).strokes == std::vector<std::size_t> { 0U },
        "undoing the reorder carries the remote selection keys back");
    Check(selectedStrokeY(0, 0U) == 10.0f && selectedStrokeY(0, 1U) == 30.0f,
        "the restored key still names the same objects");

    // Page delete from the first view shifts the selected later page down.
    Check(first.RemovePages({ 1U }), "the first view deletes an unselected page");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U, 1U },
        "deleting an earlier page keeps the first page's selection");
    Check(second.SelectionFor(1).strokes == std::vector<std::size_t> { 0U }
            && document.PageAt(1).strokes[0].points.front().y == 2000.0f,
        "the later selected page's key shifts down with its page");
    Check(first.Undo(), "the page delete undoes from the other view");
    Check(second.SelectionFor(2).strokes == std::vector<std::size_t> { 0U },
        "undoing the page delete shifts the later selection back up");
    Check(first.Redo(), "the page delete redoes from the other view");
    Check(second.SelectionFor(1).strokes == std::vector<std::size_t> { 0U },
        "redoing the page delete shifts the later selection down again");

    // Deleting a page the other view has a selection on drops it; surviving
    // later pages still move.
    Check(first.RemovePages({ 0U }), "the first view deletes the selected page");
    Check(second.SelectionFor(0).strokes == std::vector<std::size_t> { 0U }
            && document.PageAt(0).strokes[0].points.front().y == 2000.0f,
        "the deleted page's selection is gone and the survivor moved to its key");
    Check(second.SelectionFor(0).texts.empty(), "the deleted page's text selection is gone too");

    // A whole-page replacement cannot be remapped: clear rather than guess.
    Check(first.RotatePage(0, 1), "the first view rotates the remaining page");
    Check(second.SelectionFor(0).Empty(), "a whole-page change clears the remote selection");

    const auto selectionIsInRange = [&]() {
        for (std::size_t pageIndex = 0; pageIndex < document.PageCount(); ++pageIndex) {
            const cnote::ui::PageSelection& selected = second.SelectionFor(pageIndex);
            const cnote::Page& current = document.PageAt(pageIndex);
            for (const std::size_t index : selected.strokes) {
                if (index >= current.strokes.size()) {
                    return false;
                }
            }
            for (const std::size_t index : selected.images) {
                if (index >= current.images.size()) {
                    return false;
                }
            }
            for (const std::size_t index : selected.texts) {
                if (index >= current.texts.size()) {
                    return false;
                }
            }
            for (const std::size_t index : selected.latexes) {
                if (index >= current.latexes.size()) {
                    return false;
                }
            }
        }
        return true;
    };
    Check(selectionIsInRange(), "no remote selection index ever points past its page");
}

void SaveToRefusesPdfContainer() {
    const std::filesystem::path dir = MakeTempDir("pdf-save");
    const std::filesystem::path pdfPath = dir / "background.pdf";
    {
        QPdfWriter writer(QString::fromStdString(pdfPath.string()));
        writer.setResolution(72);
        writer.setPageSize(QPageSize(QSizeF(300, 500), QPageSize::Point));
        QPainter painter(&writer);
        painter.fillRect(QRect(0, 0, 300, 500), Qt::white);
    }

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    std::string error;
    Check(controller.OpenFile(pdfPath, &error), "open a PDF container: " + error);
    Check(controller.DocumentRef().Kind() == cnote::DocumentKind::PdfContainer,
        "an opened PDF is a PDF container");
    const std::string before = ReadFile(pdfPath);
    error.clear();
    Check(!controller.SaveTo(pdfPath, &error), "saving a PDF container over the PDF is refused");
    Check(!error.empty(), "the refused PDF save reports an error");
    Check(ReadFile(pdfPath) == before, "the PDF is byte-identical after the refused save");
    Check(controller.DocumentRef().FilePath() == pdfPath, "the refused save keeps the PDF path");
    Check(controller.DocumentRef().Kind() == cnote::DocumentKind::PdfContainer,
        "the refused save keeps the PDF kind");

    const std::filesystem::path projectPath = dir / "annotations.cnote";
    error.clear();
    Check(controller.SaveTo(projectPath, &error), "saving the PDF document as a project works");
    Check(controller.DocumentRef().Kind() == cnote::DocumentKind::Project,
        "the project save turns the document into a project");
    Check(cnote::ProjectFile::Load(projectPath).has_value(), "the saved project reloads");
    error.clear();
    Check(controller.SaveTo(projectPath, &error), "a later in-place project save succeeds");
}

void SetCurrentPageIndexIsSerialized() {
    const std::filesystem::path dir = MakeTempDir("viewed-page");
    const std::filesystem::path target = dir / "pages.cnote";

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    controller.InsertPages(1, 2, cnote::ui::NewPageDraft { 200.0f, 300.0f, 0xFFFFFFFFU, {} });
    Check(controller.DocumentRef().PageCount() == 3U, "two pages were appended");
    controller.SetCurrentPageIndex(2);
    Check(controller.CurrentPageIndex() == 2U && controller.DocumentRef().CurrentPageIndex() == 2U,
        "the viewed page reaches the document");

    std::string error;
    Check(controller.SaveTo(target, &error), "save the viewed page: " + error);
    const std::optional<cnote::Document> reloaded = cnote::ProjectFile::Load(target);
    Check(reloaded.has_value() && reloaded->CurrentPageIndex() == 2U,
        "the viewed page is what the project serializes");
}

void PageClipboardActsOnSeveralPages() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    controller.InsertPages(1, 1, cnote::ui::NewPageDraft { 200.0f, 300.0f, 0xFFFFFFFFU, {} });
    controller.InsertPages(2, 1, cnote::ui::NewPageDraft { 400.0f, 500.0f, 0xFFFFFFFFU, {} });
    controller.InsertPages(3, 1, cnote::ui::NewPageDraft { 600.0f, 700.0f, 0xFFFFFFFFU, {} });
    const auto widths = [&]() {
        std::vector<float> result;
        for (std::size_t index = 0; index < controller.DocumentRef().PageCount(); ++index) {
            result.push_back(controller.DocumentRef().PageAt(index).width);
        }
        return result;
    };
    const std::vector<float> original = widths();

    // Deleting two non-adjacent pages is one undo step.
    Check(controller.RemovePages({ 0, 2 }), "two non-adjacent pages delete");
    Check(widths() == std::vector<float> { original[1], original[3] }, "pages 0 and 2 are gone");
    Check(controller.Undo(), "the delete undoes in one step");
    Check(widths() == original, "undo puts both pages back where they were");
    Check(controller.Redo(), "the delete redoes");
    Check(widths() == std::vector<float> { original[1], original[3] }, "redo deletes both again");
    Check(controller.Undo(), "back to four pages");

    // Copy two pages, paste them after page 0: they land in order, as one step.
    Check(controller.CopyPages({ 1, 3 }), "two pages copy");
    Check(manager.ClipboardPages().size() == 2U, "the clipboard holds both pages");
    Check(controller.PastePages(0) == std::optional<std::size_t>(1U), "the pasted block starts after page 0");
    Check(widths() == std::vector<float> { original[0], original[1], original[3], original[1], original[2], original[3] },
        "both copies are inserted after page 0, in order");
    Check(controller.Undo(), "the paste undoes in one step");
    Check(widths() == original, "undo removes both pasted pages");

    // Cut needs a page left behind; a refused cut leaves the clipboard alone.
    Check(!controller.CutPages({ 0, 1, 2, 3 }), "cutting every page is refused");
    Check(manager.ClipboardPages().size() == 2U && widths() == original, "a refused cut changes nothing");
    Check(!controller.RemovePages({ 0, 1, 2, 3 }), "deleting every page is refused");
    Check(controller.CutPages({ 2, 3 }), "two trailing pages cut");
    Check(widths() == std::vector<float> { original[0], original[1] } && manager.ClipboardPages().size() == 2U
            && manager.ClipboardPages()[0].width == original[2],
        "cut removes both pages and puts them on the clipboard in order");
    Check(!controller.RemovePages({ 1, 0 }), "unsorted pages are refused");
}

void MovePagesUndoRedoKeepsOrderAndViewedPage() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    controller.InsertPages(1, 1, cnote::ui::NewPageDraft { 200.0f, 300.0f, 0xFFFFFFFFU, {} });
    controller.InsertPages(2, 1, cnote::ui::NewPageDraft { 400.0f, 500.0f, 0xFFFFFFFFU, {} });
    controller.InsertPages(3, 1, cnote::ui::NewPageDraft { 600.0f, 700.0f, 0xFFFFFFFFU, {} });
    const auto widths = [&]() {
        std::vector<float> result;
        for (std::size_t index = 0; index < controller.DocumentRef().PageCount(); ++index) {
            result.push_back(controller.DocumentRef().PageAt(index).width);
        }
        return result;
    };
    const std::vector<float> original = widths();
    const std::vector<float> moved { original[1], original[3], original[0], original[2] };
    controller.SetCurrentPageIndex(1);  // the 200-wide page

    // Two non-adjacent pages dragged to the end land there together, in order.
    const std::optional<std::size_t> landed = controller.MovePages({ 0, 2 }, 4);
    Check(landed == std::optional<std::size_t>(2U), "the moved block starts at index 2");
    Check(widths() == moved, "pages 0 and 2 now follow the unmoved pages, in their old order");
    Check(controller.CurrentPageIndex() == 0U, "the viewed page follows its page (index 1 -> 0)");
    Check(controller.Dirty(), "a move dirties the session");

    Check(controller.Undo(), "the move undoes");
    Check(widths() == original, "undo restores the original order");
    Check(controller.CurrentPageIndex() == 1U, "undo keeps the viewed page on the same page");
    Check(controller.Redo(), "the move redoes");
    Check(widths() == moved, "redo reapplies the move");

    Check(!controller.MovePages({ 1, 2 }, 2).has_value(), "dropping a contiguous block inside itself is refused");
    Check(!controller.MovePages({ 0 }, 1).has_value(), "dropping a page into its own gap is refused");
    Check(!controller.MovePages({ 4 }, 0).has_value(), "moving a page past the last one is refused");
    Check(!controller.MovePages({ 2, 1 }, 0).has_value(), "unsorted pages are refused");
    Check(widths() == moved, "refused moves change nothing");
}

// Every object gesture must record only its affected objects and reproduce the
// exact page on undo and redo (page epochs included), with no whole-page
// snapshot left in history.
void ObjectGesturesUndoRedoExactly() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    cnote::Page& page = controller.Session()->document.CurrentPage();
    cnote::Stroke stroke;
    stroke.points = { { 10.0f, 10.0f }, { 60.0f, 40.0f } };
    page.strokes.push_back(stroke);
    cnote::PageImage image;
    image.x = 100.0f;
    image.y = 100.0f;
    image.width = 40.0f;
    image.height = 20.0f;
    page.images.push_back(image);
    page.texts.push_back(MakeText(7, u"gesture text"));
    cnote::PageLatex latex;
    latex.id = 8;
    latex.x = 200.0f;
    latex.y = 200.0f;
    latex.content = u"x^2 + y^2";
    page.latexes.push_back(latex);

    const auto runGesture = [&](const std::string& what, auto&& gesture) {
        const cnote::Page before = controller.Session()->document.CurrentPage();
        const std::uint64_t epochBefore = controller.Session()->document.PageEpoch(0);
        gesture();
        const cnote::Page after = controller.Session()->document.CurrentPage();
        Check(!cnote::PageEqual(before, after), what + " changes the page");
        Check(controller.Session()->undoActions.size() == 1U
                && controller.Session()->undoActions.back().type == cnote::HistoryActionType::ReplaceObjects,
            what + " records one object delta");
        const cnote::HistoryAction& action = controller.Session()->undoActions.back();
        Check(action.pages.empty() && action.replacementPages.empty(),
            what + " keeps no whole-page snapshot");
        Check(controller.Session()->document.PageEpoch(0) != epochBefore,
            what + " bumps the page epoch");
        Check(controller.Undo(), what + " undoes");
        Check(cnote::PageEqual(controller.Session()->document.CurrentPage(), before),
            what + " undo restores the exact page");
        Check(controller.Redo(), what + " redoes");
        Check(cnote::PageEqual(controller.Session()->document.CurrentPage(), after),
            what + " redo restores the exact page");
        Check(controller.Undo(), what + " returns to the start state");
    };

    runGesture("move", [&]() {
        cnote::ui::PageSelection selection;
        selection.strokes.push_back(0);
        controller.SelectObjects(0, selection);
        controller.BeginPageGesture(0);
        controller.MoveSelectionBy(0, cnote::Vec2 { 5.0f, 4.0f });
        controller.CommitPageGesture();
    });
    runGesture("transform", [&]() {
        cnote::ui::PageSelection selection;
        selection.texts.push_back(0);
        controller.SelectObjects(0, selection);
        const cnote::text::QtTextBounds bounds =
            cnote::text::MeasurePageTextBoundsQt(page.texts[0], page.width);
        cnote::ui::SelectionTransform transform;
        transform.from = cnote::ui::SelectionBounds { bounds.left, bounds.top, bounds.right, bounds.bottom, true };
        transform.to = transform.from;
        transform.to.left += 15.0f;
        transform.to.right += 15.0f;
        transform.to.top += 10.0f;
        transform.to.bottom += 10.0f;
        controller.BeginPageGesture(0);
        controller.TransformSelection(0, transform);
        controller.CommitPageGesture();
    });
    runGesture("recolor", [&]() {
        cnote::ui::PageSelection selection;
        selection.strokes.push_back(0);
        selection.texts.push_back(0);
        selection.latexes.push_back(0);
        controller.SelectObjects(0, selection);
        static_cast<void>(controller.RecolorSelection(0xFF00FF00U));
    });
    runGesture("erase", [&]() {
        controller.CommitErase({ cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Stroke, 0U },
            cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Text, 0U } });
    });
    runGesture("clone", [&]() {
        cnote::ui::PageSelection selection;
        selection.strokes.push_back(0);
        selection.texts.push_back(0);
        controller.SelectObjects(0, selection);
        controller.BeginPageGesture(0);
        Check(controller.CloneSelection(0), "the clone gesture duplicates the selection");
        controller.CommitPageGesture();
    });
    runGesture("delete selection", [&]() {
        cnote::ui::PageSelection selection;
        selection.strokes.push_back(0);
        selection.texts.push_back(0);
        controller.SelectObjects(0, selection);
        Check(controller.RemoveSelection(), "the delete selection removes objects");
    });
    runGesture("paste", [&]() {
        cnote::SelectionClipboardData payload;
        cnote::Stroke stroke;
        stroke.points = { { 300.0f, 300.0f }, { 320.0f, 310.0f } };
        payload.strokes.push_back(stroke);
        payload.texts.push_back(MakeText(70, u"pasted text"));
        cnote::PageLatex latex;
        latex.id = 80;
        latex.x = 300.0f;
        latex.y = 300.0f;
        latex.content = u"a^2 + b^2";
        payload.latexes.push_back(latex);
        cnote::PageImage image;
        image.x = 300.0f;
        image.y = 300.0f;
        image.width = 12.0f;
        image.height = 8.0f;
        payload.images.push_back(image);
        payload.minX = 300.0f;
        payload.minY = 300.0f;
        payload.maxX = 320.0f;
        payload.maxY = 310.0f;
        Check(controller.PastePayload(0, cnote::Vec2 { 400.0f, 400.0f }, payload),
            "the paste appends every payload kind");
    });
}

// Paste generates new text/LaTeX ids; undo must remove the pasted objects and
// redo must reinsert them with the same ids, not regenerate them.
void PasteIdsSurviveUndoRedo() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    cnote::SelectionClipboardData payload;
    cnote::Stroke stroke;
    stroke.points = { { 10.0f, 10.0f }, { 30.0f, 20.0f } };
    payload.strokes.push_back(stroke);
    payload.texts.push_back(MakeText(70, u"pasted text"));
    cnote::PageLatex latex;
    latex.id = 80;
    latex.x = 10.0f;
    latex.y = 10.0f;
    latex.content = u"a^2 + b^2";
    payload.latexes.push_back(latex);
    cnote::PageImage image;
    image.x = 10.0f;
    image.y = 10.0f;
    image.width = 12.0f;
    image.height = 8.0f;
    payload.images.push_back(image);
    payload.minX = 10.0f;
    payload.minY = 10.0f;
    payload.maxX = 30.0f;
    payload.maxY = 20.0f;

    Check(controller.PastePayload(0, cnote::Vec2 { 200.0f, 200.0f }, payload),
        "the cross-kind paste succeeds");
    const cnote::HistoryAction& action = controller.Session()->undoActions.back();
    Check(action.type == cnote::HistoryActionType::ReplaceObjects
            && action.pageIndices == std::vector<std::size_t> { 0U }
            && action.beforeObjects.size() == 1U && action.beforeObjects.front().Empty()
            && action.afterObjects.size() == 1U
            && action.afterObjects.front().strokes.size() == 1U
            && action.afterObjects.front().texts.size() == 1U
            && action.afterObjects.front().latexes.size() == 1U
            && action.afterObjects.front().images.size() == 1U,
        "the paste records one insertion delta for every kind");
    const cnote::Page pasted = controller.Session()->document.CurrentPage();
    const std::uint64_t textId = pasted.texts.back().id;
    const std::uint64_t latexId = pasted.latexes.back().id;
    Check(textId != 0U && latexId != 0U, "the paste assigned ids to text and LaTeX");

    Check(controller.Undo(), "the paste undoes");
    const cnote::Page& removed = controller.Session()->document.CurrentPage();
    Check(removed.strokes.empty() && removed.texts.empty() && removed.latexes.empty()
            && removed.images.empty(),
        "undo removes every pasted object");
    Check(controller.Redo(), "the paste redoes");
    const cnote::Page& restored = controller.Session()->document.CurrentPage();
    Check(cnote::PageEqual(restored, pasted), "redo restores the pasted page exactly");
    Check(restored.texts.back().id == textId && restored.latexes.back().id == latexId,
        "redo restores the pasted text and LaTeX ids");
}

std::size_t StrokeStorageBytes(const cnote::Stroke& stroke) {
    return sizeof(cnote::Stroke)
        + stroke.points.capacity() * sizeof(cnote::Vec2)
        + stroke.pressures.capacity() * sizeof(float);
}

std::size_t PageStorageBytes(const cnote::Page& page) {
    std::size_t bytes = sizeof(cnote::Page);
    for (const cnote::Stroke& stroke : page.strokes) {
        bytes += StrokeStorageBytes(stroke);
    }
    bytes += page.images.size() * sizeof(cnote::PageImage);
    for (const cnote::PageText& text : page.texts) {
        bytes += sizeof(cnote::PageText) + text.content.capacity() * sizeof(char16_t);
    }
    for (const cnote::PageLatex& latex : page.latexes) {
        bytes += sizeof(cnote::PageLatex) + latex.content.capacity() * sizeof(char16_t);
    }
    return bytes;
}

std::size_t ObjectSetStorageBytes(const cnote::PageObjectSet& objects) {
    std::size_t bytes = (objects.strokeIndices.capacity() + objects.imageIndices.capacity()
                            + objects.textIndices.capacity() + objects.latexIndices.capacity())
        * sizeof(std::size_t);
    for (const cnote::Stroke& stroke : objects.strokes) {
        bytes += StrokeStorageBytes(stroke);
    }
    bytes += objects.images.size() * sizeof(cnote::PageImage);
    for (const cnote::PageText& text : objects.texts) {
        bytes += sizeof(cnote::PageText) + text.content.capacity() * sizeof(char16_t);
    }
    for (const cnote::PageLatex& latex : objects.latexes) {
        bytes += sizeof(cnote::PageLatex) + latex.content.capacity() * sizeof(char16_t);
    }
    return bytes;
}

std::size_t RetainedHistoryBytes(const cnote::ProjectSession& session) {
    std::size_t bytes = 0;
    for (const cnote::HistoryAction& action : session.undoActions) {
        bytes += sizeof(cnote::HistoryAction);
        for (const cnote::Page& page : action.pages) {
            bytes += PageStorageBytes(page);
        }
        for (const cnote::Page& page : action.replacementPages) {
            bytes += PageStorageBytes(page);
        }
        for (const cnote::PageObjectSet& objects : action.beforeObjects) {
            bytes += ObjectSetStorageBytes(objects);
        }
        for (const cnote::PageObjectSet& objects : action.afterObjects) {
            bytes += ObjectSetStorageBytes(objects);
        }
    }
    return bytes;
}

// One moved object must retain that object per gesture, not the page it lives
// on: the same repeated move on an ink-heavy page used to keep two full Page
// snapshots per action, so retained bytes grew with the page.
void ObjectGestureHistoryDoesNotScaleWithPageSize() {
    const auto measure = [](std::size_t strokeCount) {
        cnote::AppManager manager;
        cnote::qt::QtDocumentController controller(manager);
        controller.AttachSession(std::make_shared<cnote::ProjectSession>());
        cnote::Page& page = controller.Session()->document.CurrentPage();
        for (std::size_t index = 0; index < strokeCount; ++index) {
            cnote::Stroke stroke;
            const float offset = static_cast<float>(index) * 4.0f;
            stroke.points = { { 10.0f + offset, 10.0f }, { 30.0f + offset, 25.0f } };
            page.strokes.push_back(stroke);
        }
        cnote::ui::PageSelection selection;
        selection.strokes.push_back(0);
        controller.SelectObjects(0, selection);
        for (int move = 0; move < 5; ++move) {
            controller.BeginPageGesture(0);
            controller.MoveSelectionBy(0, cnote::Vec2 { 2.0f, 1.0f });
            controller.CommitPageGesture();
        }
        for (const cnote::HistoryAction& action : controller.Session()->undoActions) {
            if (action.type != cnote::HistoryActionType::ReplaceObjects
                || action.beforeObjects.size() != 1U
                || action.afterObjects.size() != 1U
                || action.beforeObjects.front().strokes.size() != 1U
                || action.afterObjects.front().strokes.size() != 1U) {
                return std::size_t { 0 };
            }
        }
        return RetainedHistoryBytes(*controller.Session());
    };

    const std::size_t small = measure(8);
    const std::size_t large = measure(400);
    std::printf("HISTORY-BYTES small-page=%zu large-page=%zu\n", small, large);
    Check(small > 0U, "repeated one-object moves retain their affected stroke");
    Check(large == small, "retained history bytes do not grow with the page's object count");
}

// One erase drag crosses pages: the controller records a single ReplaceObjects
// action carrying one delta per page, bumps each touched page's epoch once and
// broadcasts once.  Undo restores both pages exactly.
void EraseDragIsOneTransactionAcrossPages() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    Check(controller.InsertPages(1, 1, cnote::ui::NewPageDraft { 200.0f, 300.0f, 0xFFFFFFFFU, {} }) == 1U,
        "the erase fixture gets a second page");

    cnote::Page& first = session->document.PageAt(0);
    cnote::Page& second = session->document.PageAt(1);
    const cnote::Stroke firstStroke0 = MakeStrokeAt(10.0f, 10.0f);
    const cnote::Stroke firstStroke1 = MakeStrokeAt(20.0f, 20.0f);
    const cnote::Stroke firstStroke2 = MakeStrokeAt(30.0f, 30.0f);
    first.strokes = { firstStroke0, firstStroke1, firstStroke2 };
    first.texts.push_back(MakeText(10, u"keep"));
    const cnote::Stroke secondStroke0 = MakeStrokeAt(40.0f, 40.0f);
    const cnote::Stroke secondStroke1 = MakeStrokeAt(50.0f, 50.0f);
    second.strokes = { secondStroke0, secondStroke1 };
    cnote::PageLatex latex;
    latex.id = 40;
    latex.x = 5.0f;
    latex.y = 5.0f;
    latex.content = u"x^2";
    second.latexes.push_back(latex);

    const cnote::Page firstBefore = first;
    const cnote::Page secondBefore = second;
    const std::uint64_t firstEpoch = session->document.PageEpoch(0);
    const std::uint64_t secondEpoch = session->document.PageEpoch(1);
    const std::size_t undoBefore = session->undoActions.size();
    int changes = 0;
    QObject::connect(&controller, &cnote::qt::QtDocumentController::SessionChanged, &controller,
        [&changes]() { ++changes; });

    controller.CommitErase({ cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Stroke, 0U },
        cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Text, 0U },
        cnote::ui::ObjectRef { 1U, cnote::ui::ObjectKind::Stroke, 1U } });

    Check(changes == 1, "the whole multi-page erase sends exactly one change notification");
    Check(session->undoActions.size() == undoBefore + 1U, "the whole multi-page erase is one history step");
    const cnote::HistoryAction& action = session->undoActions.back();
    Check(action.type == cnote::HistoryActionType::ReplaceObjects
            && action.pageIndices == std::vector<std::size_t> { 0U, 1U },
        "the action carries both pages");
    Check(action.beforeObjects.size() == 2U && action.afterObjects.size() == 2U,
        "each page has a before/after delta");
    Check(action.beforeObjects[0].strokes.size() == 1U && action.beforeObjects[0].texts.size() == 1U
            && action.beforeObjects[0].strokeIndices == std::vector<std::size_t> { 0U }
            && action.beforeObjects[0].textIndices == std::vector<std::size_t> { 0U },
        "the first page's delta holds the erased stroke and text");
    Check(action.beforeObjects[1].strokes.size() == 1U
            && action.beforeObjects[1].strokeIndices == std::vector<std::size_t> { 1U }
            && action.beforeObjects[1].latexes.empty(),
        "the second page's delta holds only the erased stroke");
    Check(action.afterObjects[0].Empty() && action.afterObjects[1].Empty(),
        "an erase only removes objects");
    Check(session->document.PageEpoch(0) != firstEpoch && session->document.PageEpoch(1) != secondEpoch,
        "both touched pages get one epoch bump");
    Check(first.strokes.size() == 2U && first.texts.empty()
            && second.strokes.size() == 1U && second.latexes.size() == 1U,
        "the erased objects are gone and the rest survive");

    Check(controller.Undo(), "one undo reverts the whole multi-page erase");
    Check(cnote::PageEqual(first, firstBefore) && cnote::PageEqual(second, secondBefore),
        "undo restores both pages exactly");
    Check(controller.Redo(), "the erase redoes");
    Check(first.strokes.size() == 2U && second.strokes.size() == 1U,
        "redo removes the same objects again");
    Check(controller.Undo(), "back to the pre-erase pages");
}

// Surviving selection indices move down with their objects; erased ones leave
// the selection.  The bug left them pointing at whatever slid into the hole.
void SelectionIndicesFollowErasedObjects() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    cnote::Page& page = controller.Session()->document.CurrentPage();
    const cnote::Stroke stroke1 = MakeStrokeAt(1.0f, 1.0f);
    const cnote::Stroke stroke3 = MakeStrokeAt(3.0f, 3.0f);
    page.strokes = { MakeStrokeAt(0.0f, 0.0f), stroke1, MakeStrokeAt(2.0f, 2.0f), stroke3,
        MakeStrokeAt(4.0f, 4.0f) };
    page.texts = { MakeText(100, u"zero"), MakeText(101, u"one"), MakeText(102, u"two") };

    cnote::ui::PageSelection selection;
    selection.strokes = { 1U, 3U };
    selection.texts = { 2U };
    controller.SelectObjects(0, selection);

    controller.CommitErase({ cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Stroke, 0U },
        cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Text, 0U } });

    const cnote::ui::PageSelection& after = controller.SelectionFor(0);
    Check(after.strokes == std::vector<std::size_t> { 0U, 2U },
        "surviving stroke selection indices shift down by the erased object");
    Check(after.texts == std::vector<std::size_t> { 1U },
        "surviving text selection indices shift down by the erased object");
    Check(page.strokes.size() == 4U && cnote::StrokeEqual(page.strokes[0], stroke1)
            && cnote::StrokeEqual(page.strokes[2], stroke3),
        "the remapped stroke indices still name the originally selected objects");
    Check(page.texts.size() == 2U && page.texts[1].id == 102U,
        "the remapped text index still names the originally selected object");

    // Erasing a selected object drops it; the other survivor shifts again.
    controller.CommitErase({ cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Stroke, 0U } });
    Check(controller.SelectionFor(0).strokes == std::vector<std::size_t> { 1U },
        "an erased selection member leaves and the survivor follows its object");
    Check(cnote::StrokeEqual(page.strokes[1], stroke3), "the selected survivor is still the same stroke");
    Check(controller.SelectionFor(0).texts == std::vector<std::size_t> { 1U },
        "an untouched kind keeps its remapped selection");
}

// A rejected history step must not invalidate renderer caches: the page epoch
// is for committed content only.
void RejectedHistoryActionsLeavePageEpochs() {
    cnote::Document document = cnote::Document::CreateBlank();
    const auto rejected = [&document](const std::string& what, cnote::HistoryAction action) {
        const std::uint64_t epoch = document.PageEpoch(0);
        Check(!cnote::ApplyHistoryAction(document, action, false), what + " is rejected");
        Check(document.PageEpoch(0) == epoch, what + " leaves the page epoch unchanged");
    };

    cnote::HistoryAction action {};
    action.type = cnote::HistoryActionType::AddStroke;
    action.pageIndex = 0;
    action.index = 3;
    rejected("an out-of-range stroke insert", action);
    action.index = 0;
    action.type = cnote::HistoryActionType::RemoveStroke;
    rejected("removing a stroke that is not there", action);

    cnote::HistoryAction textAction {};
    textAction.pageIndex = 0;
    textAction.type = cnote::HistoryActionType::AddText;
    textAction.index = 2;
    textAction.text = MakeText(1, u"x");
    rejected("an out-of-range text insert", textAction);
    textAction.index = 0;
    textAction.type = cnote::HistoryActionType::RemoveText;
    rejected("removing a text that is not there", textAction);
    textAction.type = cnote::HistoryActionType::UpdateText;
    textAction.replacementText = MakeText(2, u"y");
    rejected("updating a text that is not there", textAction);

    cnote::HistoryAction latexAction {};
    latexAction.pageIndex = 0;
    latexAction.type = cnote::HistoryActionType::AddLatex;
    latexAction.index = 1;
    latexAction.latex.id = 9;
    latexAction.latex.content = u"z";
    rejected("an out-of-range LaTeX insert", latexAction);
    latexAction.index = 0;
    latexAction.type = cnote::HistoryActionType::RemoveLatex;
    rejected("removing a LaTeX object that is not there", latexAction);
    latexAction.type = cnote::HistoryActionType::UpdateLatex;
    latexAction.replacementLatex.id = 10;
    latexAction.replacementLatex.content = u"w";
    rejected("updating a LaTeX object that is not there", latexAction);

    Check(document.CurrentPage().strokes.empty() && document.CurrentPage().texts.empty()
            && document.CurrentPage().latexes.empty(),
        "rejected actions leave the page untouched");

    cnote::HistoryAction valid {};
    valid.type = cnote::HistoryActionType::AddStroke;
    valid.pageIndex = 0;
    valid.index = 0;
    valid.stroke = MakeStrokeAt(7.0f, 8.0f);
    const std::uint64_t before = document.PageEpoch(0);
    Check(cnote::ApplyHistoryAction(document, valid, false), "a valid action applies");
    Check(document.PageEpoch(0) != before, "a committed action bumps the page epoch");
    Check(document.CurrentPage().strokes.size() == 1U, "the valid stroke is inserted");

    const std::uint64_t afterValid = document.PageEpoch(0);
    cnote::HistoryAction staleRemove {};
    staleRemove.type = cnote::HistoryActionType::RemoveStroke;
    staleRemove.pageIndex = 0;
    staleRemove.index = 1;
    Check(!cnote::ApplyHistoryAction(document, staleRemove, false),
        "a rejected removal after a commit is refused");
    Check(document.PageEpoch(0) == afterValid, "the rejected removal keeps the committed epoch");
}

// Multi-page add/remove actions go through one batched insert/remove.  The
// page order, the current page (which follows its page across the shift) and
// one-undo-per-delete must all match the old per-page loop.
void BatchPageHistoryKeepsOrderAndCurrentPage() {
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    const auto draft = [](float width) {
        return cnote::ui::NewPageDraft { width, 300.0f, 0xFFFFFFFFU, {} };
    };
    controller.InsertPages(1, 1, draft(200.0f));
    controller.InsertPages(2, 1, draft(300.0f));
    controller.InsertPages(3, 1, draft(400.0f));
    const auto widths = [&controller]() {
        std::vector<float> result;
        for (std::size_t index = 0; index < controller.DocumentRef().PageCount(); ++index) {
            result.push_back(controller.DocumentRef().PageAt(index).width);
        }
        return result;
    };
    const std::vector<float> original = widths();
    Check(original.size() == 4U, "the batch fixture has four pages");
    controller.SetCurrentPageIndex(3);

    Check(controller.InsertPages(1, 2, draft(250.0f)) == 1U, "two pages insert as one block");
    Check(widths() == std::vector<float> { original[0], 250.0f, 250.0f, original[1], original[2], original[3] },
        "the inserted block lands in order");
    Check(controller.CurrentPageIndex() == 5U, "the current page moves right past the inserted block");

    Check(controller.Undo(), "the multi-page insert undoes in one step");
    Check(widths() == original, "undo restores the page order");
    Check(controller.CurrentPageIndex() == 3U && controller.DocumentRef().CurrentPage().width == original[3],
        "undo keeps the current page on the same page");
    Check(controller.Redo(), "the multi-page insert redoes");
    Check(widths() == std::vector<float> { original[0], 250.0f, 250.0f, original[1], original[2], original[3] }
            && controller.CurrentPageIndex() == 5U,
        "redo restores the block and the current page");
    Check(controller.Undo(), "back to the four-page document");

    // Non-contiguous removal: the batch insert that undoes it must restore the
    // pages to their recorded positions, not as a contiguous block.
    Check(controller.RemovePages({ 0U, 2U }), "two non-adjacent pages delete");
    Check(widths() == std::vector<float> { original[1], original[3] }, "both pages are gone");
    Check(controller.CurrentPageIndex() == 1U
            && controller.DocumentRef().CurrentPage().width == original[3],
        "the current page follows its page across the removal");
    Check(controller.Undo(), "the multi-page delete undoes in one step");
    Check(widths() == original, "undo puts both pages back at their old indices");
    Check(controller.CurrentPageIndex() == 3U
            && controller.DocumentRef().CurrentPage().width == original[3],
        "undo leaves the current page on the same page");
    Check(controller.Redo(), "the delete redoes");
    Check(widths() == std::vector<float> { original[1], original[3] }
            && controller.CurrentPageIndex() == 1U,
        "redo deletes both pages and keeps the current page");
}

// The old erase erased each target index from the vector front and copied the
// removed objects twice (into the gesture map, then into the history delta).
// The compacting commit records the same delta and page with one pass; compare
// the exact output and measure both on the fixture below.
void EraseCommitCompactsOnce() {
    constexpr std::size_t kStrokeCount = 6000U;
    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.AttachSession(std::make_shared<cnote::ProjectSession>());
    cnote::Page& page = controller.Session()->document.CurrentPage();
    page.strokes.reserve(kStrokeCount);
    std::vector<cnote::ui::ObjectRef> targets;
    targets.reserve(kStrokeCount / 2U);
    for (std::size_t index = 0; index < kStrokeCount; ++index) {
        page.strokes.push_back(MakeStrokeAt(static_cast<float>(index) * 0.01f, 10.0f));
        if (index % 2U == 0U) {
            targets.push_back(cnote::ui::ObjectRef { 0U, cnote::ui::ObjectKind::Stroke, index });
        }
    }

    // Reference: the old commit's per-index erase and its two capture copies.
    std::vector<cnote::Stroke> legacy = page.strokes;
    std::vector<std::size_t> legacyIndices;
    for (const cnote::ui::ObjectRef& target : targets) {
        legacyIndices.push_back(target.objectIndex);
    }
    const auto legacyStart = std::chrono::steady_clock::now();
    std::sort(legacyIndices.begin(), legacyIndices.end());
    std::map<std::size_t, cnote::Stroke> removed;
    for (const std::size_t index : legacyIndices) {
        removed.emplace(index, legacy[index]);
    }
    for (auto index = legacyIndices.rbegin(); index != legacyIndices.rend(); ++index) {
        legacy.erase(legacy.begin() + static_cast<std::ptrdiff_t>(*index));
    }
    cnote::PageObjectSet legacyBefore {};
    for (const auto& [index, object] : removed) {
        legacyBefore.strokeIndices.push_back(index);
        legacyBefore.strokes.push_back(object);
    }
    const auto legacyEnd = std::chrono::steady_clock::now();

    const cnote::Page before = page;
    const auto commitStart = std::chrono::steady_clock::now();
    controller.CommitErase(targets);
    const auto commitEnd = std::chrono::steady_clock::now();

    const std::shared_ptr<cnote::ProjectSession> session = controller.Session();
    Check(session->undoActions.size() == 1U, "the dense erase records one history action");
    const cnote::HistoryAction& action = session->undoActions.back();
    Check(action.type == cnote::HistoryActionType::ReplaceObjects
            && action.pageIndices == std::vector<std::size_t> { 0U },
        "the dense erase is a one-page object delta");
    const cnote::PageObjectSet& recorded = action.beforeObjects.front();
    Check(recorded.strokeIndices == legacyBefore.strokeIndices,
        "the compacting commit records exactly the old pre-removal indices");
    Check(recorded.strokes.size() == legacyBefore.strokes.size(),
        "the compacting commit records every erased stroke");
    bool sameObjects = recorded.strokes.size() == legacyBefore.strokes.size();
    for (std::size_t index = 0; sameObjects && index < recorded.strokes.size(); ++index) {
        sameObjects = cnote::StrokeEqual(recorded.strokes[index], legacyBefore.strokes[index]);
    }
    Check(sameObjects, "the compacting commit records exactly the old erased strokes");
    bool samePage = page.strokes.size() == legacy.size();
    for (std::size_t index = 0; samePage && index < page.strokes.size(); ++index) {
        samePage = cnote::StrokeEqual(page.strokes[index], legacy[index]);
    }
    Check(samePage, "the compacting commit leaves exactly the old surviving strokes in order");
    Check(page.strokes.size() == kStrokeCount / 2U, "half of the dense page survives");

    const auto micros = [](std::chrono::steady_clock::time_point from,
                            std::chrono::steady_clock::time_point to) {
        return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
    };
    std::printf("ERASE-COMMIT strokes=%zu erased=%zu compact=%lldus per-index=%lldus\n",
        kStrokeCount, targets.size(), static_cast<long long>(micros(commitStart, commitEnd)),
        static_cast<long long>(micros(legacyStart, legacyEnd)));

    Check(controller.Undo(), "the dense erase undoes");
    Check(cnote::PageEqual(page, before), "undo restores the dense page exactly");
    Check(controller.Redo(), "the dense erase redoes");
    Check(page.strokes.size() == kStrokeCount / 2U, "redo removes the same objects");
    Check(controller.Undo(), "back to the dense pre-erase page");
}

// Version 18 strokes store raw samples, the StrokeSampling they render with
// and their pen pressures.  A version-17 stroke has no pressures (constant
// width); a version-16 one stored already-stabilized points, which must read
// back with the identity sampling so it renders exactly as before.
void StrokeFormatRoundTripsAndLegacyReads() {
    cnote::Document document = cnote::Document::CreateBlank();
    cnote::Stroke stroke;
    stroke.sampling = { 0.4f, 1.25f };
    stroke.points = { { 10.0f, 10.0f }, { 10.5f, 10.25f }, { 30.0f, 12.0f } };
    stroke.pressures = { 0.125f, 0.5f, 0.875f };
    document.CurrentPage().strokes.push_back(stroke);

    std::vector<std::uint8_t> bytes = cnote::ProjectFile::Serialize(document);
    const std::optional<cnote::Document> current = cnote::ProjectFile::LoadFromBytes(bytes, "current.cnote");
    Check(current.has_value() && current->CurrentPage().strokes.size() == 1U
            && cnote::StrokeEqual(current->CurrentPage().strokes.front(), stroke),
        "a stroke keeps its raw points, sampling and pressures through save/load");

    // Rewrites the file as an older version did.  The header is magic(8)
    // version(4) encoding(4) payloadSize(8); this tiny payload is stored raw
    // (encoding 0), and the lone stroke is the payload's last record.
    std::uint32_t encoding = 0;
    std::memcpy(&encoding, bytes.data() + 12, sizeof(encoding));
    Check(encoding == 0U, "the tiny test project is stored raw");
    if (encoding != 0U) {
        return;
    }
    const auto downgrade = [&bytes](std::uint32_t version, std::size_t removedBytes) {
        std::memcpy(bytes.data() + 8, &version, sizeof(version));
        std::uint64_t payloadSize = 0;
        std::memcpy(&payloadSize, bytes.data() + 16, sizeof(payloadSize));
        payloadSize -= removedBytes;
        std::memcpy(bytes.data() + 16, &payloadSize, sizeof(payloadSize));
    };

    // Version 17: no pressure count or pressures after the points.
    const std::size_t pressureBytes = 4U + (4U * stroke.pressures.size());
    bytes.resize(bytes.size() - pressureBytes);
    downgrade(17U, pressureBytes);
    const std::optional<cnote::Document> v17 = cnote::ProjectFile::LoadFromBytes(bytes, "v17.cnote");
    cnote::Stroke expected = stroke;
    expected.pressures.clear();
    Check(v17.has_value() && v17->CurrentPage().strokes.size() == 1U
            && cnote::StrokeEqual(v17->CurrentPage().strokes.front(), expected),
        "a version-17 stroke reads without pressures (constant width)");

    // Version 16: no sampling floats after the stroke's smoothing either.
    std::uint8_t sampling[8] {};
    std::memcpy(sampling, &stroke.sampling.streamline, 4);
    std::memcpy(sampling + 4, &stroke.sampling.spacing, 4);
    const auto found = std::search(bytes.begin(), bytes.end(), std::begin(sampling), std::end(sampling));
    Check(found != bytes.end(), "the raw serialized stroke carries its sampling");
    if (found == bytes.end()) {
        return;
    }
    bytes.erase(found, found + 8);
    downgrade(16U, 8U);
    const std::optional<cnote::Document> v16 = cnote::ProjectFile::LoadFromBytes(bytes, "v16.cnote");
    expected.sampling = {};
    Check(v16.has_value() && v16->CurrentPage().strokes.size() == 1U
            && cnote::StrokeEqual(v16->CurrentPage().strokes.front(), expected),
        "a version-16 stroke reads its stored points with the identity sampling");
}

// Saved bytes are pinned against the pre-PERF-057 writer: the header layout,
// the payload order and the compressed/raw decision must not change.  The hash
// was captured from the writer before the staging copies were removed, so a
// regression in the rewritten save path shows up as a mismatch here.
void SerializedProjectBytesAreStable() {
    const auto hashBytes = [](const std::vector<std::uint8_t>& bytes) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint8_t byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    const auto makeDocument = [](const bool compressiblePdf) {
        std::vector<std::uint8_t> pdfBytes(65536U, 0U);
        if (!compressiblePdf) {
            std::uint32_t state = 12345U;
            for (std::uint8_t& byte : pdfBytes) {
                state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
                byte = static_cast<std::uint8_t>(state & 255U);
            }
        }
        auto pdf = std::make_shared<std::vector<std::uint8_t>>(std::move(pdfBytes));
        std::vector<cnote::Document::PdfSource> sources;
        sources.push_back({ std::filesystem::path(u"golden.pdf"), pdf });
        std::vector<cnote::Page> pages(1);
        cnote::Page& page = pages.front();
        page.width = 612.0f;
        page.height = 792.0f;
        page.rotationDegrees = 90.0f;
        page.sourcePdfSourceIndex = 0;
        page.sourcePdfPageIndex = 2;
        cnote::Stroke stroke;
        stroke.style = { 0xFF204060U, 2.0f, false };
        stroke.sampling = { 0.25f, 0.75f };
        stroke.points = { { 1.0f, 2.0f }, { 3.5f, 4.25f } };
        stroke.pressures = { 0.5f, 0.75f };
        page.strokes.push_back(stroke);
        cnote::PageText text;
        text.id = 9;
        text.x = 5.0f;
        text.y = 6.0f;
        text.width = 100.0f;
        text.content = u"Golden";
        page.texts.push_back(text);
        cnote::Document::PageDefaults defaults;
        defaults.width = 612.0f;
        defaults.height = 792.0f;
        defaults.sizeOverridden = true;
        return cnote::Document::CreateProjectDocument(
            std::filesystem::path(u"golden.cnote"), std::move(sources), std::move(pages), 0, defaults);
    };

    struct Case {
        bool compressiblePdf;
        std::size_t size;
        std::uint64_t hash;
        std::uint32_t encoding;
        const char* name;
    };
    for (const Case& item : {
             Case { true, 648U, 0x1DA8DA4F57DFF01AULL, 1U, "compressed" },
             Case { false, 65765U, 0x4C8161D4E218DF58ULL, 0U, "raw" } }) {
        const cnote::Document document = makeDocument(item.compressiblePdf);
        const std::vector<std::uint8_t> bytes = cnote::ProjectFile::Serialize(document);
        std::uint32_t encoding = 0;
        if (bytes.size() >= 16U) {
            std::memcpy(&encoding, bytes.data() + 12, sizeof(encoding));
        }
        Check(bytes.size() == item.size && hashBytes(bytes) == item.hash && encoding == item.encoding,
            std::string(item.name) + " project bytes are unchanged");
        const std::optional<cnote::Document> loaded =
            cnote::ProjectFile::LoadFromBytes(bytes, "golden.cnote");
        Check(loaded.has_value() && cnote::ProjectFile::Serialize(*loaded) == bytes,
            std::string(item.name) + " project round-trips byte-for-byte");
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    // Every window here loads settings and records recent files: keep the
    // developer's real settings file out of it.
    cnote::settings::SettingsStoreQt::OverrideSettingsPath(MakeTempDir("settings") / "settings.ini");
    if (argc == 2 && std::string_view(argv[1]) == "--rotated-pdf") {
        RotatedPdfGeometrySurvivesReopen();
        return gFailures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--default-page-size") {
        DefaultPageSizePrecedence();
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
    AutoSaveReportsFailureOncePerStreak();
    AutoSaveWritesTheDirtyRevision();
    AutoSaveIntervalFollowsSettings();
    EditDuringSaveKeepsSessionDirty();
    SaveAsDuringInFlightSaveCommitsBothDestinations();
    FailedAsynchronousSaveKeepsDirtyAndReports();
    DestroyingManagerDrainsAPendingSave();
    SaveFromInsideACompletionDeliversQueuedResults();
    SaveAfterSaveAsUsesTheChosenPath();
    CloseAfterSaveAsUsesTheChosenPath();

    PdfExportReplacesOnlyOnSuccess();
    PdfExportFailureKeepsDestination();

    RecentDocumentsRoundTripAndRules();

    FailedCloseSaveKeepsWindowAlive();
    SuccessfulCloseSavesAndCloses();
    CloseWaitsForInFlightSave();
    CancelledUnsavedPromptKeepsWindowAlive();

    EditSessionCommitsAndUndoKeepsTheObject();
    RotatedPageKeepsObjectBoxes();
    RemoteCommitCancelsTransientState();
    RemoteCommitCarriesTheOtherViewsSelection();
    SaveToRefusesPdfContainer();
    SetCurrentPageIndexIsSerialized();
    PageClipboardActsOnSeveralPages();
    MovePagesUndoRedoKeepsOrderAndViewedPage();
    ObjectGesturesUndoRedoExactly();
    PasteIdsSurviveUndoRedo();
    ObjectGestureHistoryDoesNotScaleWithPageSize();
    EraseDragIsOneTransactionAcrossPages();
    SelectionIndicesFollowErasedObjects();
    RejectedHistoryActionsLeavePageEpochs();
    BatchPageHistoryKeepsOrderAndCurrentPage();
    EraseCommitCompactsOnce();
    StrokeFormatRoundTripsAndLegacyReads();
    SerializedProjectBytesAreStable();

    if (gFailures != 0) {
        std::printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
