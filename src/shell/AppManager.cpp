#include "shell/AppManager.h"

#include "Unicode.h"
#include "latex/LatexEngine.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace cnote {

namespace {

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

AppManager::~AppManager() = default;

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
        timers_.erase(session.get());
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

void AppManager::SetSessionAutoSaveInterval(const std::shared_ptr<ProjectSession>& session, unsigned int seconds) {
    if (!session) {
        return;
    }
    session->autoSaveIntervalSeconds = seconds;
    UpdateAutoSaveTimer(session);
}

void AppManager::UpdateAutoSaveTimer(const std::shared_ptr<ProjectSession>& session) {
    if (!session) {
        return;
    }
    const bool eligible = session->dirty
        && IsAutoSaveEligible(session->document)
        && session->autoSaveIntervalSeconds > 0U;

    const auto existing = timers_.find(session.get());
    if (!eligible) {
        if (existing != timers_.end() && existing->second) {
            existing->second->stop();
        }
        return;
    }

    QTimer* timer = existing != timers_.end() ? existing->second.get() : nullptr;
    if (timer == nullptr) {
        auto owned = std::make_unique<QTimer>(this);
        owned->setSingleShot(true);
        QObject::connect(owned.get(), &QTimer::timeout, this, [this, raw = session.get()]() {
            OnAutoSaveTimeout(raw);
        });
        timer = owned.get();
        timers_[session.get()] = std::move(owned);
    }
    timer->start(static_cast<int>(session->autoSaveIntervalSeconds) * 1000);
}

void AppManager::NotifySessionSaved(const std::shared_ptr<ProjectSession>& session) {
    if (!session) {
        return;
    }
    const auto existing = timers_.find(session.get());
    if (existing != timers_.end() && existing->second) {
        existing->second->stop();
    }
}

bool AppManager::ShouldSaveOnLastViewClose(const std::shared_ptr<ProjectSession>& session) const {
    return session && session->dirty && IsAutoSaveEligible(session->document);
}

bool AppManager::IsAutoSaveTimerActive(const std::shared_ptr<ProjectSession>& session) const {
    if (!session) {
        return false;
    }
    const auto existing = timers_.find(session.get());
    return existing != timers_.end() && existing->second && existing->second->isActive();
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
    if (!shared) {
        timers_.erase(session);
        return;
    }
    if (AnyInteractionActive(shared)) {
        // Postpone while any attached view is mid-interaction.
        UpdateAutoSaveTimer(shared);
        return;
    }
    if (autoSaveHandler_) {
        autoSaveHandler_(shared, AutoSaveOwner(shared));
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
