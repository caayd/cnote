#include "App.h"

namespace cnote {

void App::CreateBlankDocument() {
    if (!ConfirmCloseWithUnsavedChanges()) return;
    PrepareForDocumentSwitch();
    newPageSettings_.backgroundColor = settings_.defaultPageColor;
    const std::vector<std::uint32_t> resolvedPaletteProfile = ResolvedPaletteProfile(newPageSettings_.paletteProfile);
    Document blankDocument = Document::CreateBlank({
        newPageSettings_.width,
        newPageSettings_.height,
        newPageSettings_.backgroundColor,
        resolvedPaletteProfile,
    });
    if (blankDocument.PageCount() > 0U) {
        blankDocument.PageAt(0) = BuildConfiguredNewPage();
    }
    ApplyDocument(std::move(blankDocument), true);
}

void App::PrepareForDocumentSwitch() {
    FinishDocumentInteraction();
    KillTimer(window_, kZoomSessionTimerId);
    StopPdfSidebarSlideAnimationTimer();
    CancelQuickSizePopup();
    ClearInteractionPreview();
    zoomSessionActive_ = false;
    zoomAnchorValid_ = false;
    contentCompositionDirty_ = true;
    ClearZoomSessionSnapshots();
}

void App::ApplyDocument(Document document, bool resetBrushWidth) {
    auto session = std::make_shared<ProjectSession>();
    session->document = std::move(document);
    session->autoSaveIntervalSeconds = settings_.autoSaveIntervalSeconds;
    AttachSession(std::move(session));
    NormalizeDocumentPaletteProfiles();
    ResetDocumentView(resetBrushWidth);
}

void App::ResetDocumentView(bool resetBrushWidth) {
    pdfSidebarOpen_ = pdfSidebarOpen_ && session_->document.PageCount() > 0U;
    pdfSidebarSlideProgress_ = pdfSidebarOpen_ ? 1.0f : 0.0f;
    pdfSidebarSlideAnimationStartProgress_ = pdfSidebarSlideProgress_;
    pdfSidebarSlideAnimationTargetProgress_ = pdfSidebarSlideProgress_;
    pdfSidebarScrollY_ = 0;
    MarkPdfSidebarCompositionDirty();
    copiedSidebarPage_.reset();
    CancelPageJumpInput();
    CancelToolbarZoomInput();
    EndTextEditControl();
    editingTextPageIndex_.reset();
    editingTextIndex_.reset();
    editingTextBefore_.reset();
    editingTextCreatesText_ = false;
    pageSurfaceCaches_.assign(session_->document.PageCount(), {});
    sidebarThumbnailCaches_.assign(session_->document.PageCount(), {});
    contentCompositionDirty_ = true;
    ClearInteractionPreview();
    ClearSelection();
    ClosePaletteOverlays();
    selectionLassoCursorPoints_.clear();
    isSelecting_ = false;
    if (resetBrushWidth) {
        SyncBrushWidthFromActiveTool();
    }
    ResetView();
    UpdateAutoSaveTimer();
    EnsureSidebarCurrentPageVisible();
    UpdateWindowTitle();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::AddBlankPageAfterCurrent() {
    if (session_->document.PageCount() == 0U) {
        return;
    }

    ClearSelection();
    const std::size_t previousPageIndex = CurrentPageIndex();
    Page newPage = BuildConfiguredNewPage();
    const std::size_t insertIndex = InsertDocumentPage(CurrentPageIndex() + 1U, newPage);
    PushPageHistoryAction(HistoryActionType::AddPages, insertIndex, { std::move(newPage) }, insertIndex, previousPageIndex);
    RestoreAfterPageHistoryChange(insertIndex);
    NotifyDocumentMutated();
}

void App::CopyCurrentPageToSidebarBuffer() {
    if (session_->document.PageCount() == 0U) {
        copiedSidebarPage_.reset();
        return;
    }

    copiedSidebarPage_ = CurrentPage();
}

bool App::CutCurrentPageToSidebarBuffer() {
    if (session_->document.PageCount() <= 1U) {
        return false;
    }

    CopyCurrentPageToSidebarBuffer();
    return DeleteCurrentPage();
}

bool App::PasteSidebarPageAfterCurrent() {
    if (!copiedSidebarPage_.has_value() || session_->document.PageCount() == 0U) {
        return false;
    }

    ClearSelection();
    const std::size_t previousPageIndex = CurrentPageIndex();
    const Page insertedPage = *copiedSidebarPage_;
    const std::size_t insertIndex = InsertDocumentPage(CurrentPageIndex() + 1U, insertedPage);
    PushPageHistoryAction(HistoryActionType::AddPages, insertIndex, { insertedPage }, insertIndex, previousPageIndex);
    RestoreAfterPageHistoryChange(insertIndex);
    NotifyDocumentMutated();
    return true;
}

bool App::DeleteCurrentPage() {
    if (session_->document.PageCount() <= 1U) {
        return false;
    }

    ClearSelection();
    const std::size_t currentIndex = CurrentPageIndex();
    Page removedPage = session_->document.PageAt(currentIndex);
    const std::size_t focusIndex = RemoveDocumentPage(currentIndex);
    PushPageHistoryAction(HistoryActionType::RemovePages, currentIndex, { std::move(removedPage) }, focusIndex, currentIndex);
    RestoreAfterPageHistoryChange(focusIndex);
    NotifyDocumentMutated();
    return true;
}

}  // namespace cnote
