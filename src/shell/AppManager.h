#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QTimer>

#include "Document.h"
#include "ProjectSession.h"

namespace cnote {

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

    // Called when a session's autosave timer fires; the handler saves and
    // clears dirty only on success.
    using AutoSaveHandler = std::function<void(const std::shared_ptr<ProjectSession>&, QObject*)>;
    void SetAutoSaveHandler(AutoSaveHandler handler);

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

private:
    struct Entry {
        QPointer<QObject> owner;
        SessionView* view = nullptr;
        std::shared_ptr<ProjectSession> session;
    };

    [[nodiscard]] Entry* FindEntry(QObject* owner);
    [[nodiscard]] bool AnyInteractionActive(const std::shared_ptr<ProjectSession>& session) const;
    void OnAutoSaveTimeout(ProjectSession* session);

    AutoSaveHandler autoSaveHandler_;
    std::vector<Entry> entries_;
    std::map<ProjectSession*, std::unique_ptr<QTimer>> timers_;
};

}  // namespace cnote
