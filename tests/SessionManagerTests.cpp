// Self-contained Qt session-lifetime / autosave manager test (plan 4.2/4.5/4.6).
//
// Uses stub views (no windows) to prove: one autosave timer per shared session,
// owner reassignment on close, postponement during an active interaction, dirty
// cleared only on a successful save, the same-file lookup rule, extension
// case-insensitivity, the last-view-close decision, and the document kind each
// factory and load path reports.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include "Document.h"
#include "ProjectFile.h"
#include "ProjectSession.h"
#include "shell/AppManager.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

void SpinFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

class StubView : public QObject, public cnote::SessionView {
public:
    bool interactionActive = false;

    [[nodiscard]] bool IsInteractionActive() const override { return interactionActive; }
    void FinishInteraction() override {}
    void OnSessionChanged(const cnote::ProjectChange&, bool) override {}
    [[nodiscard]] std::size_t CurrentPageIndex() const override { return 0; }
    void SetCurrentPageIndex(std::size_t) override {}
};

std::shared_ptr<cnote::ProjectSession> MakeSession(const std::filesystem::path& path) {
    auto session = std::make_shared<cnote::ProjectSession>();
    session->document.SetFilePath(path);
    return session;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "cnote-session-manager-tests";
    std::error_code error;
    std::filesystem::create_directories(scratch, error);
    const std::filesystem::path projectPath = scratch / "project.cnote";
    {
        std::ofstream(projectPath) << "placeholder";
    }

    cnote::AppManager manager;
    int autoSaveCalls = 0;
    bool autoSaveSucceeds = true;
    manager.SetAutoSaveHandler([&](const std::shared_ptr<cnote::ProjectSession>& session, QObject*) {
        ++autoSaveCalls;
        if (autoSaveSucceeds) {
            session->dirty = false;
            manager.NotifySaveFinished(session, true);
        }
        // A failure leaves the session dirty; the manager re-arms the retry at
        // the interval itself.
    });

    // --- two views on one session share one autosave timer ------------------
    manager.SetAutoSaveInterval(1);
    auto session = MakeSession(projectPath);
    session->dirty = true;
    StubView first;
    StubView second;
    manager.RegisterView(&first, &first, session);
    manager.RegisterView(&second, &second, session);
    Check(manager.AutoSaveTimerCount() == 1U, "two views on one session share one autosave timer");
    Check(manager.IsAutoSaveTimerActive(session), "dirty saved session arms the autosave timer");
    Check(manager.AutoSaveOwner(session) == &first, "the first registered view owns autosave");
    Check(manager.ViewsForSession(session).size() == 2U, "both views are registered for the session");

    // --- postpone while an interaction is active ----------------------------
    first.interactionActive = true;
    SpinFor(1400);
    Check(autoSaveCalls == 0, "autosave is postponed during an active interaction");
    Check(manager.IsAutoSaveTimerActive(session), "the postponed timer stays armed");
    first.interactionActive = false;
    SpinFor(1400);
    Check(autoSaveCalls == 1, "autosave fires once the interaction ends");
    Check(!session->dirty, "a successful autosave clears dirty");
    Check(!manager.IsAutoSaveTimerActive(session), "a clean session disarms the timer");

    // --- dirty clears only on a successful save -----------------------------
    session->dirty = true;
    manager.UpdateAutoSaveTimer(session);
    autoSaveSucceeds = false;
    SpinFor(1400);
    Check(autoSaveCalls == 2 && session->dirty, "a failed autosave keeps dirty");
    Check(manager.IsAutoSaveTimerActive(session), "a failed autosave keeps the timer armed");
    autoSaveSucceeds = true;
    SpinFor(1400);
    Check(autoSaveCalls == 3 && !session->dirty, "a later successful autosave clears dirty");

    // --- the interval is a maximum dirty age, not an idle delay -------------
    // The commit halfway to the deadline must not move it: continuous editing
    // still autosaves one interval after the first unsaved edit.
    session->revision = 10U;
    session->dirty = true;
    manager.SetAutoSaveInterval(2);
    manager.UpdateAutoSaveTimer(session);
    SpinFor(1200);
    session->revision = 11U;
    session->dirty = true;
    manager.UpdateAutoSaveTimer(session);
    SpinFor(1400);
    Check(autoSaveCalls == 4 && !session->dirty, "a commit does not push the autosave deadline back");

    // --- shortening the interval applies to a dirty open session -----------
    session->revision = 12U;
    session->dirty = true;
    manager.SetAutoSaveInterval(10);
    manager.UpdateAutoSaveTimer(session);
    SpinFor(300);
    manager.SetAutoSaveInterval(1);
    Check(manager.IsAutoSaveTimerActive(session), "a shortened interval keeps a dirty session armed");
    SpinFor(1200);
    Check(autoSaveCalls == 5 && !session->dirty, "shortening the interval applies to an open dirty session");

    // --- Off disables auto-save; turning it back on re-arms -----------------
    session->revision = 13U;
    session->dirty = true;
    manager.UpdateAutoSaveTimer(session);
    manager.SetAutoSaveInterval(0);
    Check(!manager.IsAutoSaveTimerActive(session), "Off disarms the autosave timer");
    SpinFor(1200);
    Check(autoSaveCalls == 5, "Off never autosaves a dirty session");
    manager.SetAutoSaveInterval(1);
    Check(manager.IsAutoSaveTimerActive(session), "turning auto-save back on re-arms a dirty session");
    SpinFor(1200);
    Check(autoSaveCalls == 6 && !session->dirty, "the re-enabled interval autosaves the dirty session");

    // --- the app interval reaches every open session ------------------------
    session->revision = 14U;
    session->dirty = true;
    manager.UpdateAutoSaveTimer(session);
    auto otherSession = MakeSession(scratch / "other.cnote");
    otherSession->dirty = true;
    StubView otherView;
    manager.RegisterView(&otherView, &otherView, otherSession);
    Check(manager.IsAutoSaveTimerActive(otherSession), "a session opened while auto-save is on is armed");
    manager.SetAutoSaveInterval(0);
    Check(!manager.IsAutoSaveTimerActive(session) && !manager.IsAutoSaveTimerActive(otherSession),
        "Off reaches every open session");
    manager.SetAutoSaveInterval(1);
    Check(manager.IsAutoSaveTimerActive(session) && manager.IsAutoSaveTimerActive(otherSession),
        "an interval change reaches every open session");
    manager.SetAutoSaveInterval(0);
    manager.UnregisterView(&otherView);

    // --- owner reassignment on close ----------------------------------------
    manager.UnregisterView(&first);
    Check(manager.AutoSaveOwner(session) == &second, "autosave ownership reassigns when the owner closes");
    Check(manager.AutoSaveTimerCount() == 1U, "the shared timer survives one view closing");
    manager.UnregisterView(&second);
    Check(manager.AutoSaveTimerCount() == 0U, "the timer is released when the last view closes");

    // --- last-view-close decision -------------------------------------------
    session->dirty = false;
    Check(!manager.ShouldSaveOnLastViewClose(session), "a clean session needs no close save");
    session->dirty = true;
    Check(manager.ShouldSaveOnLastViewClose(session), "a dirty saved session is saved on last close");
    session->document.SetFilePath({});
    Check(!manager.ShouldSaveOnLastViewClose(session), "a dirty unsaved session has no close save");

    // --- same-file lookup ---------------------------------------------------
    auto lookupSession = MakeSession(projectPath);
    StubView lookupView;
    manager.RegisterView(&lookupView, &lookupView, lookupSession);
    Check(manager.FindOpenSession(projectPath) == lookupSession, "same path joins the session");
    Check(manager.FindOpenSession(scratch / "." / "project.cnote") == lookupSession,
        "normalized absolute path joins the session");
#ifdef _WIN32
    std::wstring upper = projectPath.native();
    for (wchar_t& character : upper) {
        character = static_cast<wchar_t>(std::towupper(character));
    }
    Check(manager.FindOpenSession(std::filesystem::path(upper)) == lookupSession,
        "Windows case-variant path joins the session");
#else
    // POSIX compares exact spelling, but a case variant is still the same file
    // on a case-insensitive filesystem (the macOS default), which joins through
    // filesystem equivalence.
    std::error_code caseError;
    const bool caseInsensitiveFs = std::filesystem::equivalent(projectPath, scratch / "PROJECT.cnote", caseError);
    Check((manager.FindOpenSession(scratch / "PROJECT.cnote") == lookupSession) == caseInsensitiveFs,
        "a case-variant path joins the session only when it is the same file");
#endif
    Check(manager.FindOpenSession(scratch / "other.cnote") == std::shared_ptr<cnote::ProjectSession> {},
        "a different path does not join the session");

    // --- extension case-insensitivity ---------------------------------------
    Check(cnote::AppManager::HasCnoteExtension("Note.CNOTE"), "cnote extension check is case-insensitive");
    Check(cnote::AppManager::HasPdfExtension("Doc.PdF"), "pdf extension check is case-insensitive");
    Check(!cnote::AppManager::HasCnoteExtension("Note.txt"), "other extensions are rejected");
    cnote::Document eligible = cnote::Document::CreateBlank();
    eligible.SetFilePath("saved.CNOTE");
    Check(cnote::AppManager::IsAutoSaveEligible(eligible), "autosave eligibility is case-insensitive");
    eligible.SetFilePath("saved.pdf");
    Check(!cnote::AppManager::IsAutoSaveEligible(eligible), "PDF documents are not autosave eligible");

    // --- document kind ------------------------------------------------------
    // Save/close policy keys off the kind, so the factories and the two load
    // paths must classify every document they hand out.
    Check(cnote::Document::CreateBlank().Kind() == cnote::DocumentKind::Blank,
        "a blank document reports the blank kind");
    const cnote::Document editable = cnote::Document::CreateEditable(
        std::filesystem::path("kind.cnote"), { cnote::Page {} }, 0, cnote::Document::PageDefaults {});
    Check(editable.Kind() == cnote::DocumentKind::Project, "an editable document is a project");
    const cnote::Document pdfContainer = cnote::Document::CreatePdfDocument(
        std::filesystem::path("kind.pdf"), nullptr, { cnote::Page {} }, 0, cnote::Document::PageDefaults {});
    Check(pdfContainer.Kind() == cnote::DocumentKind::PdfContainer,
        "a PDF container reports the PDF kind");
    const std::vector<std::uint8_t> kindBytes =
        cnote::ProjectFile::Serialize(cnote::Document::CreateBlank());
    const auto projectLoad = cnote::ProjectFile::LoadFromBytes(kindBytes, scratch / "kind.cnote");
    Check(projectLoad.has_value() && projectLoad->Kind() == cnote::DocumentKind::Project,
        "a project decoded from a .cnote path is a project");
    const auto embeddedLoad = cnote::ProjectFile::LoadFromBytes(kindBytes, scratch / "kind.pdf");
    Check(embeddedLoad.has_value() && embeddedLoad->Kind() == cnote::DocumentKind::EmbeddedProject,
        "a project decoded from a PDF path is an embedded project");

    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL SESSION MANAGER CHECKS PASSED\n" : "%d SESSION MANAGER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
