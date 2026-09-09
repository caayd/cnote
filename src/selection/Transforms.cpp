#include "App.h"

namespace cnote {

void App::ClearInteractionPreview() {
    interactionPreviewMode_ = InteractionPreviewMode::None;
    interactionPageSnapshots_.clear();
    interactionPreviewStrokes_.clear();
    interactionPreviewTexts_.clear();
    interactionPreviewLatexes_.clear();
    interactionPreviewImages_.clear();
    moveSelectionTextCache_.clear();
}

void App::EnsureInteractionPreviewStorage() {
    interactionPageSnapshots_.assign(session_->document.PageCount(), {});
    interactionPreviewStrokes_.assign(session_->document.PageCount(), {});
    interactionPreviewTexts_.assign(session_->document.PageCount(), {});
    interactionPreviewLatexes_.assign(session_->document.PageCount(), {});
    interactionPreviewImages_.assign(session_->document.PageCount(), {});
}

}  // namespace cnote
