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
    interactionPageSnapshots_.assign(document_.PageCount(), {});
    interactionPreviewStrokes_.assign(document_.PageCount(), {});
    interactionPreviewTexts_.assign(document_.PageCount(), {});
    interactionPreviewLatexes_.assign(document_.PageCount(), {});
    interactionPreviewImages_.assign(document_.PageCount(), {});
}

}  // namespace cnote
