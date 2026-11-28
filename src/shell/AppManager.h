#pragma once

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

    void SetSessionAutoSaveInterval(const std::shared_ptr<ProjectSession>& session, unsigned int seconds);
    void UpdateAutoSaveTimer(const std::shared_ptr<ProjectSession>& session);
    void NotifySessionSaved(const std::shared_ptr<ProjectSession>& session);

    // Last-view-close decision: true when the shell should save before closing.
    [[nodiscard]] bool ShouldSaveOnLastViewClose(const std::shared_ptr<ProjectSession>& session) const;

    // Diagnostics for the session-lifetime evidence.
    [[nodiscard]] std::size_t AutoSaveTimerCount() const noexcept { return timers_.size(); }
    [[nodiscard]] bool IsAutoSaveTimerActive(const std::shared_ptr<ProjectSession>& session) const;

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

    [[nodiscard]] bool AnyInteractionActive(const std::shared_ptr<ProjectSession>& session) const;
    void OnAutoSaveTimeout(ProjectSession* session);

    bool servicesInitialized_ = false;
    AutoSaveHandler autoSaveHandler_;
    std::unique_ptr<ProjectSaveQueue> projectSaves_;
    std::vector<Entry> entries_;
    std::map<ProjectSession*, std::unique_ptr<QTimer>> timers_;
    std::vector<Page> clipboardPages_;
};

}  // namespace cnote
