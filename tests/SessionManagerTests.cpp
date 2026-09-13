// Self-contained Qt session-lifetime / autosave manager test (plan 4.2/4.5/4.6).
//
// Uses stub views (no windows) to prove: one autosave timer per shared session,
// owner reassignment on close, postponement during an active interaction, dirty
// cleared only on a successful save, the same-file lookup rule, extension
// case-insensitivity, and the last-view-close decision.

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
            manager.NotifySessionSaved(session);
        } else {
            manager.UpdateAutoSaveTimer(session);
        }
    });

    // --- two views on one session share one autosave timer ------------------
    auto session = MakeSession(projectPath);
    session->dirty = true;
    session->autoSaveIntervalSeconds = 1;
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
    Check(manager.FindOpenSession(scratch / "PROJECT.cnote") == std::shared_ptr<cnote::ProjectSession> {},
        "POSIX exact spelling does not join a case-variant path");
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

    std::filesystem::remove_all(scratch, error);
    std::printf(g_failures == 0 ? "ALL SESSION MANAGER CHECKS PASSED\n" : "%d SESSION MANAGER CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
