#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QTimer>

#include "Document.h"
#include "ProjectSession.h"

namespace cnote {

class ProjectSaveQueue;

// Per-view hooks the manager calls. The Qt view implements them; the manager
// never touches widgets.
class SessionView {
public:
    virtual ~SessionView() = default;
    [[nodiscard]] virtual bool IsInteractionActive() const = 0;
    virtual void FinishInteraction() = 0;
    virtual void OnSessionChanged(const ProjectChange& change, bool originating) = 0;
    [[nodiscard]] virtual std::size_t CurrentPageIndex() const = 0;
    virtual void SetCurrentPageIndex(std::size_t index) = 0;
};

// Owns process-global service lifetime plus the per-session view registry and
// autosave timers. Qt Core only; no widgets.
class AppManager : public QObject {
public:
    explicit AppManager(QObject* parent = nullptr);
    ~AppManager() override;

    // Process-global service lifetime (plan step 4.1).  The manager owns the
    // MicroTeX/LaTeX services: InitializeServices must run after the
    // QApplication exists (the Qt adapter uses QFontDatabase/QPainter) and
    // ShutdownServices after the last window is destroyed but before the
    // QApplication object is.  PDFium is reference counted inside
    // PdfiumBackend, so the first backend to exist initializes it and the last
    // one destroys it.
    [[nodiscard]] bool InitializeServices(const std::filesystem::path& latexResourceRoot, std::string* error);
    void ShutdownServices();
    [[nodiscard]] bool ServicesInitialized() const { return servicesInitialized_; }

    // Called when a session's autosave timer fires; the handler saves and
    // clears dirty only on success.
    using AutoSaveHandler = std::function<void(const std::shared_ptr<ProjectSession>&, QObject*)>;
    void SetAutoSaveHandler(AutoSaveHandler handler);

    // Called once per failure streak with the view that should report it.  A
    // completion that lands while any view of the session is interacting is
    // held until the interaction ends; a successful write in between drops it.
    using AutoSaveFailureHandler = std::function<void(QObject* owner, const std::string& error)>;
    void SetAutoSaveFailureHandler(AutoSaveFailureHandler handler);

    // The process's one background project writer.  Every view and the
    // autosave timer submit through it, so writes run in request order and off
    // the GUI thread; the manager drains it before it is destroyed, so a quit
    // cannot outrun a requested save.
    [[nodiscard]] ProjectSaveQueue& ProjectSaves();

    void RegisterView(QObject* owner, SessionView* view, std::shared_ptr<ProjectSession> session);
    void UnregisterView(QObject* owner);

    [[nodiscard]] std::vector<QObject*> ViewsForSession(const std::shared_ptr<ProjectSession>& session) const;
    [[nodiscard]] std::shared_ptr<ProjectSession> FindOpenSession(const std::filesystem::path& path) const;
    [[nodiscard]] QObject* AutoSaveOwner(const std::shared_ptr<ProjectSession>& session) const;

    // App-wide auto-save interval in seconds, 0 = off.  The interval belongs
    // to the application setting, never to a project: every registered session
    // follows it, a session registered later inherits it, and there is no
    // per-session override.
    void SetAutoSaveInterval(unsigned int seconds);
    [[nodiscard]] unsigned int AutoSaveInterval() const noexcept { return autoSaveIntervalSeconds_; }

    // Commits and registration call this.  The interval is the maximum age of
    // the dirty state: the first unpersisted edit starts the deadline, and a
    // later commit does not push it back.
    void UpdateAutoSaveTimer(const std::shared_ptr<ProjectSession>& session);
    // A snapshot of the session's current revision was queued for writing.
    // The pending write covers every edit up to now, so the next edit starts a
    // new maximum dirty age.
    void NotifySaveRequested(const std::shared_ptr<ProjectSession>& session);
    // A queued write finished.  dirty itself is the controller's bookkeeping;
    // a failure re-arms the retry at one interval, a success keeps the deadline
    // of the edits that landed after its snapshot and drops a held failure.
    void NotifySaveFinished(const std::shared_ptr<ProjectSession>& session, bool success);
    // A background auto-save write failed.  Marks or holds the report (see
    // SetAutoSaveFailureHandler); never called for a refused request.
    void NotifyAutoSaveFailed(const std::shared_ptr<ProjectSession>& session, const std::string& error);

    // Last-view-close decision: true when the shell should save before closing.
    [[nodiscard]] bool ShouldSaveOnLastViewClose(const std::shared_ptr<ProjectSession>& session) const;

    // Diagnostics for the session-lifetime evidence.
    [[nodiscard]] std::size_t AutoSaveTimerCount() const noexcept { return autoSave_.size(); }
    [[nodiscard]] bool IsAutoSaveTimerActive(const std::shared_ptr<ProjectSession>& session) const;

    // Test-only: moves the session's maximum-age clock back by `age` so a
    // deadline can be made due without sleeping for the configured interval.
    // Production code never calls this.
    void BackdateAutoSaveDirtySinceForTesting(
        const std::shared_ptr<ProjectSession>& session,
        std::chrono::steady_clock::duration age);

    // Same-file lookup rule: filesystem equivalence for existing paths, then
    // normalized absolute paths; Windows-only case-insensitive fallback, exact
    // spelling on POSIX.
    [[nodiscard]] static bool PathsReferToSameFile(const std::filesystem::path& a, const std::filesystem::path& b);
    [[nodiscard]] static bool HasCnoteExtension(const std::filesystem::path& path);
    [[nodiscard]] static bool HasPdfExtension(const std::filesystem::path& path);
    [[nodiscard]] static bool IsAutoSaveEligible(const Document& document);

    // In-process page clipboard for the sidebar's copy/cut/paste page
    // management, holding the copied pages in document order.  Process-global
    // like the OS selection clipboard, so pages copied in one window paste
    // into another view of the same or a different document.
    void SetClipboardPages(std::vector<Page> pages) { clipboardPages_ = std::move(pages); }
    [[nodiscard]] const std::vector<Page>& ClipboardPages() const { return clipboardPages_; }
    [[nodiscard]] bool HasClipboardPages() const { return !clipboardPages_.empty(); }

private:
    struct Entry {
        QPointer<QObject> owner;
        SessionView* view = nullptr;
        std::shared_ptr<ProjectSession> session;
    };

    // One auto-save clock per session.  `dirtySince` is the first edit that is
    // not covered by a queued write; `pendingWrites` counts those writes, and
    // their completions re-arm from `dirtySince` (or retire it when the write
    // was current).
    struct AutoSaveState {
        std::unique_ptr<QTimer> timer;
        std::optional<std::chrono::steady_clock::time_point> dirtySince;
        unsigned int pendingWrites = 0;
        // A failure completion that arrived mid-interaction: shown when the
        // interaction ends, unless a successful write drops it first.
        std::optional<std::string> pendingFailure;
    };

    [[nodiscard]] bool AnyInteractionActive(const std::shared_ptr<ProjectSession>& session) const;
    void OnAutoSaveTimeout(ProjectSession* session);
    void ReportDeferredFailures();
    void ScheduleDeferredFailures();
    void ReportAutoSaveFailure(const std::shared_ptr<ProjectSession>& session, const std::string& error);
    [[nodiscard]] std::shared_ptr<ProjectSession> SessionFor(ProjectSession* raw) const;
    [[nodiscard]] AutoSaveState& AutoSaveStateFor(const std::shared_ptr<ProjectSession>& session);
    [[nodiscard]] long long IntervalMs() const;
    [[nodiscard]] long long RemainingMs(const AutoSaveState& state) const;
    void StartDeadline(AutoSaveState& state, long long milliseconds);
    void ApplyAutoSaveIntervalTo(const std::shared_ptr<ProjectSession>& session);

    bool servicesInitialized_ = false;
    AutoSaveHandler autoSaveHandler_;
    AutoSaveFailureHandler autoSaveFailureHandler_;
    std::unique_ptr<ProjectSaveQueue> projectSaves_;
    // Single-shot sweep that shows held failure reports once every view of a
    // session has stopped interacting.
    std::unique_ptr<QTimer> failureReportTimer_;
    std::vector<Entry> entries_;
    std::map<ProjectSession*, AutoSaveState> autoSave_;
    unsigned int autoSaveIntervalSeconds_ = 60U;
    std::vector<Page> clipboardPages_;
};

}  // namespace cnote
