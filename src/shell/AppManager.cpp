#include "shell/AppManager.h"

#include "ProjectSaveQueue.h"
#include "Unicode.h"
#include "latex/LatexEngine.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace cnote {

namespace {

// How soon an overdue deadline is re-checked while a view is mid-interaction.
// The maximum dirty age has already elapsed; waiting a full interval would
// postpone the write long after the interaction ends.
constexpr int kInteractionRecheckMs = 250;

using SteadyClock = std::chrono::steady_clock;

bool ExtensionEquals(const std::filesystem::path& path, std::string_view extension) {
    const std::u16string actual = path.extension().u16string();
    if (actual.size() != extension.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        char16_t left = actual[index];
        char16_t right = static_cast<char16_t>(extension[index]);
        if (left >= u'A' && left <= u'Z') {
            left = static_cast<char16_t>(left + 32);
        }
        if (right >= u'A' && right <= u'Z') {
            right = static_cast<char16_t>(right + 32);
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

}  // namespace

AppManager::AppManager(QObject* parent) : QObject(parent) {}

AppManager::~AppManager() {
    // A requested save must reach the disk even when the process quits without
    // a window close (a capture run, a harness): the queue's destructor drains
    // its jobs before the completing views are gone.
    projectSaves_.reset();
}

bool AppManager::InitializeServices(const std::filesystem::path& latexResourceRoot, std::string* error) {
    if (servicesInitialized_) {
        return true;
    }
    if (latex::Initialize(latexResourceRoot)) {
        servicesInitialized_ = true;
        return true;
    }
    if (error != nullptr) {
        *error = "MicroTeX resources not found at " + PathToUtf8(latexResourceRoot);
    }
    return false;
}

void AppManager::ShutdownServices() {
    if (!servicesInitialized_) {
        return;
    }
    latex::Shutdown();
    servicesInitialized_ = false;
}

void AppManager::SetAutoSaveHandler(AutoSaveHandler handler) {
    autoSaveHandler_ = std::move(handler);
}

ProjectSaveQueue& AppManager::ProjectSaves() {
    if (!projectSaves_) {
        projectSaves_ = std::make_unique<ProjectSaveQueue>();
    }
    return *projectSaves_;
}

void AppManager::RegisterView(QObject* owner, SessionView* view, std::shared_ptr<ProjectSession> session) {
    if (owner == nullptr || view == nullptr || !session) {
        return;
    }
    UnregisterView(owner);
    entries_.push_back({ QPointer<QObject>(owner), view, std::move(session) });
    UpdateAutoSaveTimer(entries_.back().session);
}

void AppManager::UnregisterView(QObject* owner) {
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& candidate) {
        return candidate.owner == owner;
    });
    if (entry == entries_.end()) {
        return;
    }
    const std::shared_ptr<ProjectSession> session = entry->session;
    entries_.erase(entry);

    const bool stillAttached = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& candidate) {
        return candidate.session == session;
    });
    if (stillAttached) {
        UpdateAutoSaveTimer(session);
    } else {
        autoSave_.erase(session.get());
    }
}

std::vector<QObject*> AppManager::ViewsForSession(const std::shared_ptr<ProjectSession>& session) const {
    std::vector<QObject*> views;
    for (const Entry& entry : entries_) {
        if (entry.session == session && !entry.owner.isNull()) {
            views.push_back(entry.owner);
        }
    }
    return views;
}

std::shared_ptr<ProjectSession> AppManager::FindOpenSession(const std::filesystem::path& path) const {
    if (path.empty()) {
        return {};
    }
    for (const Entry& entry : entries_) {
        if (entry.owner.isNull()) {
            continue;
        }
        const std::filesystem::path& existing = entry.session->document.FilePath();
        if (existing.empty()) {
            continue;
        }
        if (PathsReferToSameFile(path, existing)) {
            return entry.session;
        }
    }
    return {};
}

QObject* AppManager::AutoSaveOwner(const std::shared_ptr<ProjectSession>& session) const {
    for (const Entry& entry : entries_) {
        if (entry.session == session && !entry.owner.isNull()) {
            return entry.owner;
        }
    }
    return nullptr;
}

void AppManager::SetAutoSaveInterval(unsigned int seconds) {
    if (autoSaveIntervalSeconds_ == seconds) {
        return;
    }
    autoSaveIntervalSeconds_ = seconds;
    // App-wide: every session that is already open follows the new value.  A
    // session opened later reads it when it registers.
    std::vector<ProjectSession*> applied;
    for (const Entry& entry : entries_) {
        if (!entry.session) {
            continue;
        }
        ProjectSession* raw = entry.session.get();
        if (std::find(applied.begin(), applied.end(), raw) != applied.end()) {
            continue;
        }
        applied.push_back(raw);
        ApplyAutoSaveIntervalTo(entry.session);
    }
}

long long AppManager::IntervalMs() const {
    return static_cast<long long>(autoSaveIntervalSeconds_) * 1000LL;
}

long long AppManager::RemainingMs(const AutoSaveState& state) const {
    if (!state.dirtySince) {
        return IntervalMs();
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - *state.dirtySince).count();
    return std::max(0LL, IntervalMs() - elapsed);
}

void AppManager::StartDeadline(AutoSaveState& state, long long milliseconds) {
    state.timer->start(static_cast<int>(std::clamp<long long>(milliseconds, 0LL, 0x7FFFFFFFLL)));
}

AppManager::AutoSaveState& AppManager::AutoSaveStateFor(const std::shared_ptr<ProjectSession>& session) {
    const auto inserted = autoSave_.try_emplace(session.get());
    if (inserted.second) {
        auto owned = std::make_unique<QTimer>(this);
        owned->setSingleShot(true);
        ProjectSession* raw = session.get();
        QObject::connect(owned.get(), &QTimer::timeout, this, [this, raw]() { OnAutoSaveTimeout(raw); });
        inserted.first->second.timer = std::move(owned);
    }
    return inserted.first->second;
}

void AppManager::ApplyAutoSaveIntervalTo(const std::shared_ptr<ProjectSession>& session) {
    const bool eligible = session->dirty && IsAutoSaveEligible(session->document);
    const auto existing = autoSave_.find(session.get());
    if (!eligible || autoSaveIntervalSeconds_ == 0U) {
        if (existing != autoSave_.end()) {
            existing->second.timer->stop();
            existing->second.dirtySince.reset();
        }
        return;
    }
    if (existing == autoSave_.end()) {
        UpdateAutoSaveTimer(session);
        return;
    }
    AutoSaveState& state = existing->second;
    if (state.pendingWrites > 0U) {
        // A write is in flight; its completion arms the deadline with the new
        // interval.
        state.timer->stop();
        return;
    }
    if (!state.dirtySince) {
        // No pending write (checked above) and no live deadline: auto-save was
        // off while the session turned dirty, so the new interval opens a new
        // maximum age now.
        state.dirtySince = SteadyClock::now();
    }
    // The new interval re-derives the maximum age: shortening it can make the
    // deadline immediate, lengthening it moves it out.
    state.timer->stop();
    StartDeadline(state, RemainingMs(state));
}

void AppManager::UpdateAutoSaveTimer(const std::shared_ptr<ProjectSession>& session) {
    if (!session) {
        return;
    }
    const bool eligible = session->dirty && IsAutoSaveEligible(session->document);
    const auto existing = autoSave_.find(session.get());
    if (!eligible || autoSaveIntervalSeconds_ == 0U) {
        if (existing != autoSave_.end()) {
            existing->second.timer->stop();
            existing->second.dirtySince.reset();
        }
        return;
    }

    AutoSaveState& state = AutoSaveStateFor(session);
    if (!state.dirtySince) {
        // The first edit not covered by a queued snapshot starts the clock.
        state.dirtySince = SteadyClock::now();
    }
    if (state.pendingWrites > 0U) {
        // NotifySaveFinished re-arms from dirtySince when the write lands.
        return;
    }
    if (state.timer->isActive()) {
        // Never push a live maximum-age deadline back.
        return;
    }
    StartDeadline(state, RemainingMs(state));
}

void AppManager::NotifySaveRequested(const std::shared_ptr<ProjectSession>& session) {
    if (!session) {
        return;
    }
    // Every request is paired with its completion, even for a session that is
    // currently clean or has auto-save off.
    AutoSaveState& state = AutoSaveStateFor(session);
    ++state.pendingWrites;
    // The snapshot covers every edit made so far: a later edit starts a new
    // maximum dirty age.
    state.dirtySince.reset();
    state.timer->stop();
}

void AppManager::NotifySaveFinished(const std::shared_ptr<ProjectSession>& session, bool success) {
    if (!session) {
        return;
    }
    const auto existing = autoSave_.find(session.get());
    if (existing == autoSave_.end()) {
        return;
    }
    AutoSaveState& state = existing->second;
    if (state.pendingWrites > 0U) {
        --state.pendingWrites;
    }
    state.timer->stop();

    const bool eligible = session->dirty && IsAutoSaveEligible(session->document);
    if (!eligible || autoSaveIntervalSeconds_ == 0U) {
        state.dirtySince.reset();
        return;
    }
    if (!success) {
        // Retry at the interval in case the file is still unwritable; another
        // queued write still owns the current revision.
        if (state.pendingWrites == 0U) {
            state.dirtySince = SteadyClock::now();
            StartDeadline(state, IntervalMs());
        }
        return;
    }
    if (state.pendingWrites > 0U) {
        // A newer snapshot is queued; its completion arms the deadline.
        return;
    }
    if (!state.dirtySince) {
        // dirty without an unpersisted edit means bookkeeping updated the
        // revision without a commit callback (tests do this); start now.
        state.dirtySince = SteadyClock::now();
    }
    StartDeadline(state, RemainingMs(state));
}

bool AppManager::ShouldSaveOnLastViewClose(const std::shared_ptr<ProjectSession>& session) const {
    return session && session->dirty && IsAutoSaveEligible(session->document);
}

bool AppManager::IsAutoSaveTimerActive(const std::shared_ptr<ProjectSession>& session) const {
    if (!session) {
        return false;
    }
    const auto existing = autoSave_.find(session.get());
    return existing != autoSave_.end() && existing->second.timer && existing->second.timer->isActive();
}

bool AppManager::AnyInteractionActive(const std::shared_ptr<ProjectSession>& session) const {
    for (const Entry& entry : entries_) {
        if (entry.session == session && entry.view != nullptr && entry.view->IsInteractionActive()) {
            return true;
        }
    }
    return false;
}

void AppManager::OnAutoSaveTimeout(ProjectSession* session) {
    std::shared_ptr<ProjectSession> shared;
    for (const Entry& entry : entries_) {
        if (entry.session.get() == session) {
            shared = entry.session;
            break;
        }
    }
    const auto existing = autoSave_.find(session);
    if (!shared || existing == autoSave_.end()) {
        autoSave_.erase(session);
        return;
    }
    AutoSaveState& state = existing->second;
    const bool eligible = shared->dirty && IsAutoSaveEligible(shared->document);
    if (!eligible || autoSaveIntervalSeconds_ == 0U) {
        state.timer->stop();
        state.dirtySince.reset();
        return;
    }
    if (AnyInteractionActive(shared)) {
        // The maximum dirty age is already reached; re-check soon instead of
        // waiting another full interval.  dirtySince stays untouched so a
        // commit during the interaction cannot extend the deadline.
        state.timer->start(kInteractionRecheckMs);
        return;
    }
    const unsigned int pendingBefore = state.pendingWrites;
    if (autoSaveHandler_) {
        autoSaveHandler_(shared, AutoSaveOwner(shared));
    }
    // The handler may have closed the last view (erasing the state) or already
    // finished a synchronous write.
    const auto after = autoSave_.find(session);
    if (after == autoSave_.end()) {
        return;
    }
    AutoSaveState& remaining = after->second;
    if (remaining.pendingWrites == pendingBefore && shared->dirty) {
        // The handler did not queue a write (a refusal or a synchronous test
        // double): keep retrying at the interval rather than dropping the
        // session from auto-save until its next commit.
        remaining.dirtySince = SteadyClock::now();
        StartDeadline(remaining, IntervalMs());
    }
}

bool AppManager::PathsReferToSameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    std::error_code error;
    if (std::filesystem::equivalent(a, b, error) && !error) {
        return true;
    }
    error.clear();
    const std::filesystem::path normalizedA = std::filesystem::absolute(a, error).lexically_normal();
    if (error) {
        return false;
    }
    const std::filesystem::path normalizedB = std::filesystem::absolute(b, error).lexically_normal();
    if (error) {
        return false;
    }
#ifdef _WIN32
    const std::wstring left = normalizedA.native();
    const std::wstring right = normalizedB.native();
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }
    return true;
#else
    return normalizedA == normalizedB;
#endif
}

bool AppManager::HasCnoteExtension(const std::filesystem::path& path) {
    return ExtensionEquals(path, ".cnote");
}

bool AppManager::HasPdfExtension(const std::filesystem::path& path) {
    return ExtensionEquals(path, ".pdf");
}

bool AppManager::IsAutoSaveEligible(const Document& document) {
    return HasCnoteExtension(document.FilePath());
}

}  // namespace cnote
