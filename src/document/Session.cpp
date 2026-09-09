#include "App.h"

#include <algorithm>
#include <filesystem>

namespace cnote {

std::vector<App*> App::openWindows_;
unsigned int App::appInstanceCount_ = 0;

void App::DetachSession() {
    std::erase(session_->views, this);
    for (App* view : session_->views) {
        view->UpdateAutoSaveTimer();
    }
}

void App::AttachSession(std::shared_ptr<ProjectSession> session) {
    DetachSession();
    session_ = std::move(session);
    session_->views.push_back(this);
    currentPageIndex_ = session_->document.CurrentPageIndex();
    settings_.autoSaveIntervalSeconds = session_->autoSaveIntervalSeconds;
}

std::size_t App::CurrentPageIndex() const {
    return std::min(currentPageIndex_, session_->document.PageCount() - 1U);
}

Page& App::CurrentPage() { return session_->document.PageAt(CurrentPageIndex()); }
const Page& App::CurrentPage() const { return session_->document.PageAt(CurrentPageIndex()); }

bool App::SetViewPageIndex(std::size_t index) {
    if (index >= session_->document.PageCount()) return false;
    currentPageIndex_ = index;
    return true;
}

std::vector<std::pair<App*, int>> App::CaptureViewPageAnchors() {
    std::vector<std::pair<App*, int>> anchors;
    for (App* view : session_->views) {
        if (view == this || view->window_ == nullptr) continue;
        view->StopSmoothScroll();
        RECT client {};
        GetClientRect(view->window_, &client);
        anchors.emplace_back(view, view->CalculatePageView(client, view->CurrentPageIndex()).rect.top);
    }
    return anchors;
}

void App::RestoreViewPageAnchors(const std::vector<std::pair<App*, int>>& anchors) {
    for (const auto& [view, top] : anchors) {
        RECT client {};
        GetClientRect(view->window_, &client);
        view->panY_ += static_cast<float>(top - view->CalculatePageView(client, view->CurrentPageIndex()).rect.top);
    }
}

std::size_t App::InsertDocumentPage(std::size_t index, Page page) {
    const auto anchors = CaptureViewPageAnchors();
    const auto inserted = session_->document.InsertPage(index, std::move(page));
    for (App* view : session_->views) {
        if (view->currentPageIndex_ >= inserted) ++view->currentPageIndex_;
    }
    RestoreViewPageAnchors(anchors);
    return inserted;
}

std::size_t App::RemoveDocumentPage(std::size_t index) {
    if (session_->document.PageCount() <= 1U || index >= session_->document.PageCount()) {
        return CurrentPageIndex();
    }
    const auto anchors = CaptureViewPageAnchors();
    session_->document.RemovePage(index);
    for (App* view : session_->views) {
        if (view->currentPageIndex_ > index) --view->currentPageIndex_;
        view->currentPageIndex_ = view->CurrentPageIndex();
    }
    RestoreViewPageAnchors(anchors);
    return CurrentPageIndex();
}

void App::FinishDocumentInteraction() {
    if (isDrawing_) EndStroke();
    if (isErasing_) EndErase();
    if (isMovingSelection_) EndMoveSelection();
    if (isTransformingImage_) EndImageTransform();
    if (isTransformingStroke_) EndStrokeTransform();
    if (isTransformingText_) EndTextTransform();
    if (isTransformingLatex_) EndLatexTransform();
    CommitTextEdit();
    CommitLatexEdit();
    pendingTextEditClick_ = false;
    if (isSelecting_) EndSelection();
    UpdateCaptureState();
}

void App::FinishOtherViewInteractions() {
    // One editing owner at a time. This also protects index-based selections
    // and whole-page history snapshots until object-level transactions exist.
    for (App* view : session_->views) {
        if (view != this) view->FinishDocumentInteraction();
    }
}

bool App::IsSessionInteractionActive() const {
    return std::any_of(session_->views.begin(), session_->views.end(), [](const App* view) {
        return view->IsDocumentInteractionActive() || view->isTransformingLatex_;
    });
}

App* App::AutoSaveOwner() const {
    for (App* view : session_->views) {
        if (view->window_ != nullptr) return view;
    }
    return nullptr;
}

void App::SetSessionAutoSaveInterval(unsigned int seconds) {
    session_->autoSaveIntervalSeconds = seconds;
    for (App* view : session_->views) {
        view->settings_.autoSaveIntervalSeconds = seconds;
        view->UpdateAutoSaveTimer();
    }
}

void App::BroadcastSessionChange(ProjectChange::Kind kind) {
    const ProjectChange change { kind, session_->revision };
    for (App* view : session_->views) {
        view->OnSessionChanged(change, view == this);
    }
}

void App::OnSessionChanged(const ProjectChange& change, bool originatingView) {
    if (change.kind == ProjectChange::Kind::Committed && !originatingView) {
        // Conservative invalidation for the initial version. This boundary can
        // carry affected pages/objects without changing the window lifecycle.
        ClearSelection();
        ClearInteractionPreview();
        ClearZoomSessionSnapshots();
        pageSurfaceCaches_.assign(session_->document.PageCount(), {});
        sidebarThumbnailCaches_.assign(session_->document.PageCount(), {});
        currentPageIndex_ = CurrentPageIndex();
        contentCompositionDirty_ = true;
        MarkPdfSidebarCompositionDirty();
        const auto& defaults = session_->document.NewPageDefaults();
        if (newPageSettings_.width != defaults.width || newPageSettings_.height != defaults.height
            || newPageSettings_.backgroundColor != defaults.backgroundColor
            || newPageSettings_.paletteProfile != defaults.paletteProfile) {
            newPageSettings_.width = defaults.width;
            newPageSettings_.height = defaults.height;
            newPageSettings_.backgroundColor = defaults.backgroundColor;
            newPageSettings_.paletteProfile = defaults.paletteProfile;
            RefreshNewPageSettingsWindow();
        }
    }
    UpdateAutoSaveTimer();
    if (window_ != nullptr) {
        UpdateWindowTitle();
        if (originatingView && change.kind == ProjectChange::Kind::Committed) {
            // The editing path already invalidates its precise canvas region.
            RECT toolbar {};
            GetClientRect(window_, &toolbar);
            toolbar.bottom = FullToolbarHeight();
            InvalidateRect(window_, &toolbar, FALSE);
        } else {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

std::shared_ptr<ProjectSession> App::FindOpenSession(const std::wstring& path) {
    if (path.empty()) return {};
    for (App* view : openWindows_) {
        const auto& existing = view->session_->document.FilePath();
        if (existing.empty()) continue;
        std::error_code error;
        if (std::filesystem::equivalent(path, existing, error) && !error) return view->session_;
        error.clear();
        const auto requestedPath = std::filesystem::absolute(path, error).lexically_normal();
        if (error) continue;
        const auto existingPath = std::filesystem::absolute(existing, error).lexically_normal();
        if (!error && CompareStringOrdinal(requestedPath.c_str(), -1, existingPath.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return view->session_;
        }
    }
    return {};
}

} // namespace cnote
