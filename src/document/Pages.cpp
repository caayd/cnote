#include "App.h"

namespace cnote {

void App::CreateBlankDocument() {
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
    CancelTextEdit();
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
    document_ = std::move(document);
    NormalizeDocumentPaletteProfiles();
    documentDirty_ = false;
    pdfSidebarOpen_ = pdfSidebarOpen_ && document_.PageCount() > 0U;
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
    editingTextBeforePage_.reset();
    pageSurfaceCaches_.assign(document_.PageCount(), {});
    sidebarThumbnailCaches_.assign(document_.PageCount(), {});
    contentCompositionDirty_ = true;
    ClearInteractionPreview();
    ClearSelection();
    ClosePaletteOverlays();
    selectionLassoCursorPoints_.clear();
    isSelecting_ = false;
    ClearHistory();
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
    if (document_.PageCount() == 0U) {
        return;
    }

    ClearSelection();
    const std::size_t previousPageIndex = document_.CurrentPageIndex();
    Page newPage = BuildConfiguredNewPage();
    const std::size_t insertIndex = document_.InsertPage(document_.CurrentPageIndex() + 1U, newPage);
    PushPageHistoryAction(HistoryActionType::AddPages, insertIndex, { std::move(newPage) }, insertIndex, previousPageIndex);
    RestoreAfterPageHistoryChange(insertIndex);
    NotifyDocumentMutated();
}

void App::CopyCurrentPageToSidebarBuffer() {
    if (document_.PageCount() == 0U) {
        copiedSidebarPage_.reset();
        return;
    }

    copiedSidebarPage_ = document_.CurrentPage();
}

bool App::CutCurrentPageToSidebarBuffer() {
    if (document_.PageCount() <= 1U) {
        return false;
    }

    CopyCurrentPageToSidebarBuffer();
    return DeleteCurrentPage();
}

bool App::PasteSidebarPageAfterCurrent() {
    if (!copiedSidebarPage_.has_value() || document_.PageCount() == 0U) {
        return false;
    }

    ClearSelection();
    const std::size_t previousPageIndex = document_.CurrentPageIndex();
    const Page insertedPage = *copiedSidebarPage_;
    const std::size_t insertIndex = document_.InsertPage(document_.CurrentPageIndex() + 1U, insertedPage);
    PushPageHistoryAction(HistoryActionType::AddPages, insertIndex, { insertedPage }, insertIndex, previousPageIndex);
    RestoreAfterPageHistoryChange(insertIndex);
    NotifyDocumentMutated();
    return true;
}

bool App::DeleteCurrentPage() {
    if (document_.PageCount() <= 1U) {
        return false;
    }

    ClearSelection();
    const std::size_t currentIndex = document_.CurrentPageIndex();
    Page removedPage = document_.PageAt(currentIndex);
    const std::size_t focusIndex = document_.RemovePage(currentIndex);
    PushPageHistoryAction(HistoryActionType::RemovePages, currentIndex, { std::move(removedPage) }, focusIndex, currentIndex);
    RestoreAfterPageHistoryChange(focusIndex);
    NotifyDocumentMutated();
    return true;
}

}  // namespace cnote
