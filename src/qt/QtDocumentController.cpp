#include "qt/QtDocumentController.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "PdfBackend.h"
#include "ProjectFile.h"
#include "document/HistoryReducer.h"
#include "qt/QtDocumentIo.h"
#include "render/PdfiumBackgroundProvider.h"
#include "text/TextEditingQt.h"
#include "text/TextLayoutQt.h"
#include "ui/SelectionMath.h"

namespace cnote::qt {

namespace {

// Reference App helpers (src/App.cpp:1958-2055): pointer samples are filtered
// towards the raw pointer, deduplicated by a screen-relative spacing and long
// segments are resampled so rendering keeps the same curve.
constexpr float kMinScreenSpacing = 1.75f;
constexpr float kMaxScreenSpacing = 3.0f;
constexpr int kMaxInterpolatedPoints = 64;

// Lazily rendered PDF page rasters kept alive by the background provider.
constexpr std::size_t kPdfBackgroundCapacity = 8;

// Reference App::ExportPdf fallback scale for pages without a raster.
constexpr float kPdfExportRenderScale = 4.0f;

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

float Distance(Vec2 a, Vec2 b) {
    return std::sqrt(DistanceSquared(a, b));
}

Vec2 Lerp(Vec2 from, Vec2 to, float amount) {
    return Vec2 {
        from.x + ((to.x - from.x) * amount),
        from.y + ((to.y - from.y) * amount),
    };
}

float InputSpacingPageUnits(float pageSpacing, float pageScale) {
    const float scale = std::max(0.1f, pageScale);
    const float screenSpacing = std::clamp(
        std::max(0.05f, pageSpacing) * scale, kMinScreenSpacing, kMaxScreenSpacing);
    return screenSpacing / scale;
}

void AppendStrokePointWithResampling(Stroke& stroke, Vec2 point, float spacing) {
    if (stroke.points.empty()) {
        stroke.points.push_back(point);
        return;
    }

    const float minSpacing = std::max(0.05f, spacing);
    const Vec2 from = stroke.points.back();
    const float distance = Distance(from, point);
    if (distance < minSpacing) {
        return;
    }

    const int segmentCount = std::min(
        kMaxInterpolatedPoints + 1,
        std::max(1, static_cast<int>(std::ceil(distance / minSpacing))));
    for (int segment = 1; segment <= segmentCount; ++segment) {
        const float t = static_cast<float>(segment) / static_cast<float>(segmentCount);
        stroke.points.push_back(Lerp(from, point, t));
    }
}

}  // namespace

QtDocumentController::QtDocumentController(AppManager& manager, QObject* parent)
    : QObject(parent),
      manager_(manager),
      pdfBackend_(std::make_unique<PdfiumBackend>()) {}

QtDocumentController::~QtDocumentController() {
    manager_.UnregisterView(this);
}

// --- session ---------------------------------------------------------------

void QtDocumentController::AttachSession(std::shared_ptr<ProjectSession> session) {
    CancelStroke();
    CancelTextEdit();
    CancelLatexEdit();
    manager_.UnregisterView(this);
    session_ = std::move(session);
    if (session_ != nullptr) {
        manager_.RegisterView(this, this, session_);
    }
    currentPage_ = 0;
    RebuildPdfBackground();
    emit SessionChanged();
    emit EditStateChanged();
}

bool QtDocumentController::OpenFile(const std::filesystem::path& path, std::string* error) {
    // Same-file joins are the shell's decision (AppManager::FindOpenSession);
    // this entry point always loads a fresh session for the view.
    std::optional<Document> document = LoadDocument(path, *pdfBackend_, error);
    if (!document.has_value()) {
        return false;
    }

    auto session = std::make_shared<ProjectSession>();
    session->document = std::move(*document);
    AttachSession(std::move(session));
    return true;
}

void QtDocumentController::NewBlankDocument() {
    auto session = std::make_shared<ProjectSession>();
    AttachSession(std::move(session));
}

bool QtDocumentController::SaveTo(const std::filesystem::path& path, std::string* error) {
    if (session_ == nullptr || path.empty()) {
        return false;
    }
    if (!ProjectFile::Save(session_->document, path)) {
        if (error != nullptr) {
            *error = "cannot write " + path.string();
        }
        return false;
    }

    session_->document.SetFilePath(path);
    session_->dirty = false;
    manager_.NotifySessionSaved(session_);
    NotifySessionChanged(true);
    return true;
}

bool QtDocumentController::ExportPdf(const std::filesystem::path& path, bool embedProject, std::string* error) {
    if (session_ == nullptr || path.empty()) {
        return false;
    }

    const std::vector<std::uint8_t> serialized =
        embedProject ? ProjectFile::Serialize(session_->document) : std::vector<std::uint8_t> {};

    // Write to a temporary destination and only replace the requested file
    // after a successful export (plan step 6.6).
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::error_code removeError;
    std::filesystem::remove(temporary, removeError);

    if (!pdfBackend_->ExportPdf(
            session_->document,
            temporary,
            kPdfExportRenderScale,
            input_.strokeSmoothing,
            3.0f,
            2.0f,
            std::span<const std::uint8_t>(serialized))) {
        std::filesystem::remove(temporary, removeError);
        if (error != nullptr) {
            *error = "cannot export PDF " + path.string();
        }
        return false;
    }

    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError) {
        std::filesystem::remove(temporary, removeError);
        if (error != nullptr) {
            *error = "cannot replace " + path.string();
        }
        return false;
    }
    return true;
}

bool QtDocumentController::HasSavedPath() const {
    return session_ != nullptr && !session_->document.FilePath().empty();
}

const std::filesystem::path& QtDocumentController::FilePath() const {
    static const std::filesystem::path kEmpty {};
    return session_ != nullptr ? session_->document.FilePath() : kEmpty;
}

void QtDocumentController::RebuildPdfBackground() {
    pdfBackground_.reset();
    if (session_ == nullptr) {
        return;
    }
    pdfBackground_ = std::make_unique<render::PdfiumBackgroundProvider>(
        *pdfBackend_, session_->document, kPdfBackgroundCapacity);
}

void QtDocumentController::NotifySessionChanged(bool originating) {
    if (session_ == nullptr) {
        return;
    }
    ProjectChange change;
    change.kind = ProjectChange::Kind::Committed;
    change.revision = session_->revision;
    for (QObject* owner : manager_.ViewsForSession(session_)) {
        if (auto* view = qobject_cast<QtDocumentController*>(owner); view != nullptr) {
            view->OnSessionChanged(change, view == this && originating);
        }
    }
    manager_.UpdateAutoSaveTimer(session_);
    if (originating) {
        emit SessionChanged();
    }
}

// --- SessionView ------------------------------------------------------------

bool QtDocumentController::IsInteractionActive() const {
    return pendingStroke_.has_value() || EditingText() || EditingLatex();
}

void QtDocumentController::FinishInteraction() {
    if (pendingStroke_.has_value()) {
        EndStroke();
    }
    if (EditingText()) {
        CommitTextEdit(editContent_ != nullptr ? editContent_->CaptureEditedText() : PageText {});
    }
    if (EditingLatex()) {
        CommitLatexEdit(editContent_ != nullptr ? editContent_->CaptureEditedLatexSource() : QString());
    }
}

void QtDocumentController::OnSessionChanged(const ProjectChange&, bool originating) {
    if (!originating) {
        // Another view committed: drop the pending preview and refresh caches.
        pendingStroke_.reset();
        pendingStrokePage_.reset();
        emit SessionChanged();
    }
}

void QtDocumentController::SetCurrentPageIndex(std::size_t index) {
    if (session_ != nullptr && index < session_->document.PageCount()) {
        currentPage_ = index;
    }
}

const Document& QtDocumentController::DocumentRef() const {
    static const Document kEmpty {};
    return session_ != nullptr ? session_->document : kEmpty;
}

std::uint64_t QtDocumentController::Revision() const {
    return session_ != nullptr ? session_->revision : 0U;
}

Page* QtDocumentController::PageAt(std::size_t pageIndex) {
    if (session_ == nullptr || pageIndex >= session_->document.PageCount()) {
        return nullptr;
    }
    return &session_->document.PageAt(pageIndex);
}

const Page* QtDocumentController::PageAt(std::size_t pageIndex) const {
    if (session_ == nullptr || pageIndex >= session_->document.PageCount()) {
        return nullptr;
    }
    return &session_->document.PageAt(pageIndex);
}

std::uint64_t QtDocumentController::NextObjectId() const {
    std::uint64_t next = 1;
    if (session_ != nullptr) {
        for (std::size_t pageIndex = 0; pageIndex < session_->document.PageCount(); ++pageIndex) {
            const Page& page = session_->document.PageAt(pageIndex);
            for (const PageText& text : page.texts) {
                next = std::max(next, text.id + 1U);
            }
            for (const PageLatex& latex : page.latexes) {
                next = std::max(next, latex.id + 1U);
            }
        }
    }
    return next;
}

// --- strokes ----------------------------------------------------------------

void QtDocumentController::BeginStroke(std::size_t pageIndex, Vec2 point, float pageScale) {
    (void)pageScale;
    FinishInteraction();
    pendingStroke_.reset();
    pendingStrokePage_.reset();
    if (PageAt(pageIndex) == nullptr) {
        return;
    }

    Stroke stroke;
    stroke.style = strokeStyle_;
    stroke.smoothing = input_.strokeSmoothing;
    stroke.points.push_back(point);
    pendingStroke_ = std::move(stroke);
    pendingStrokePage_ = pageIndex;
    currentPage_ = pageIndex;
}

void QtDocumentController::ExtendStroke(Vec2 point, float pageScale) {
    if (!pendingStroke_.has_value()) {
        return;
    }

    const float spacing = InputSpacingPageUnits(input_.inputSpacing, pageScale);
    Vec2 filtered = point;
    if (!pendingStroke_->points.empty()) {
        const float followAmount = std::clamp(1.0f - input_.inputSmoothing, 0.05f, 0.95f);
        filtered = Lerp(pendingStroke_->points.back(), point, followAmount);
        if (Distance(filtered, pendingStroke_->points.back()) < spacing) {
            return;
        }
    }
    AppendStrokePointWithResampling(*pendingStroke_, filtered, spacing);
}

void QtDocumentController::EndStroke() {
    if (!pendingStroke_.has_value() || !pendingStrokePage_.has_value() || session_ == nullptr) {
        CancelStroke();
        return;
    }

    Stroke finished = std::move(*pendingStroke_);
    const std::size_t pageIndex = *pendingStrokePage_;
    pendingStroke_.reset();
    pendingStrokePage_.reset();

    Page* page = PageAt(pageIndex);
    if (page == nullptr || finished.points.empty()) {
        return;
    }

    // Same commit shape as the reference (App::EndStroke): append, push an
    // AddStroke action with its insert index, clear the redo stack, bump the
    // revision once and broadcast once.
    HistoryAction action {};
    action.type = HistoryActionType::AddStroke;
    action.stroke = finished;
    action.index = page->strokes.size();
    action.pageIndex = pageIndex;
    page->strokes.push_back(std::move(finished));
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    currentPage_ = pageIndex;
    NotifySessionChanged(true);
}

void QtDocumentController::CancelStroke() {
    pendingStroke_.reset();
    pendingStrokePage_.reset();
}

const Stroke* QtDocumentController::PendingStroke() const {
    return pendingStroke_.has_value() ? &*pendingStroke_ : nullptr;
}

std::size_t QtDocumentController::PendingStrokePage() const {
    return pendingStrokePage_.value_or(0U);
}

// --- history ----------------------------------------------------------------

bool QtDocumentController::CanUndo() const {
    return session_ != nullptr && !session_->undoActions.empty();
}

bool QtDocumentController::CanRedo() const {
    return session_ != nullptr && !session_->redoActions.empty();
}

bool QtDocumentController::Undo() {
    if (session_ == nullptr || session_->undoActions.empty()) {
        return false;
    }
    FinishInteraction();

    HistoryAction action = std::move(session_->undoActions.back());
    session_->undoActions.pop_back();
    if (!ApplyHistoryAction(session_->document, action, true)) {
        session_->undoActions.push_back(std::move(action));
        return false;
    }

    session_->redoActions.push_back(std::move(action));
    session_->dirty = true;
    ++session_->revision;
    currentPage_ = std::min(action.pageIndex, session_->document.PageCount() > 0 ? session_->document.PageCount() - 1U : 0U);
    NotifySessionChanged(true);
    return true;
}

bool QtDocumentController::Redo() {
    if (session_ == nullptr || session_->redoActions.empty()) {
        return false;
    }
    FinishInteraction();

    HistoryAction action = std::move(session_->redoActions.back());
    session_->redoActions.pop_back();
    if (!ApplyHistoryAction(session_->document, action, false)) {
        session_->redoActions.push_back(std::move(action));
        return false;
    }

    session_->undoActions.push_back(std::move(action));
    session_->dirty = true;
    ++session_->revision;
    currentPage_ = std::min(action.pageIndex, session_->document.PageCount() > 0 ? session_->document.PageCount() - 1U : 0U);
    NotifySessionChanged(true);
    return true;
}

// --- object creation and text editing ---------------------------------------

std::size_t QtDocumentController::AddTextObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) {
    Page* page = PageAt(pageIndex);
    if (page == nullptr) {
        return 0U;
    }
    FinishInteraction();

    PageText text;
    text.id = NextObjectId();
    text.x = topLeft.x;
    text.y = topLeft.y;
    text.fontSize = fontSize;
    page->texts.push_back(text);
    const std::size_t index = page->texts.size() - 1U;
    if (BeginTextEdit(pageIndex, index)) {
        editingTextCreates_ = true;
    }
    return index;
}

std::size_t QtDocumentController::AddLatexObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) {
    Page* page = PageAt(pageIndex);
    if (page == nullptr) {
        return 0U;
    }
    FinishInteraction();

    PageLatex latex;
    latex.id = NextObjectId();
    latex.x = topLeft.x;
    latex.y = topLeft.y;
    latex.fontSize = fontSize;
    page->latexes.push_back(latex);
    const std::size_t index = page->latexes.size() - 1U;
    if (BeginLatexEdit(pageIndex, index)) {
        editingLatexCreates_ = true;
    }
    return index;
}

bool QtDocumentController::BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) {
    FinishInteraction();
    Page* page = PageAt(pageIndex);
    if (page == nullptr || textIndex >= page->texts.size()) {
        return false;
    }

    editingTextBefore_ = page->texts[textIndex];
    editingTextPage_ = pageIndex;
    editingTextIndex_ = textIndex;
    editingTextCreates_ = false;
    currentPage_ = pageIndex;
    emit EditStateChanged();
    return true;
}

const PageText* QtDocumentController::EditingTextObject() const {
    if (!editingTextIndex_.has_value()) {
        return nullptr;
    }
    return &editingTextBefore_;
}

bool QtDocumentController::BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) {
    FinishInteraction();
    Page* page = PageAt(pageIndex);
    if (page == nullptr || latexIndex >= page->latexes.size()) {
        return false;
    }

    editingLatexBefore_ = page->latexes[latexIndex];
    editingLatexPage_ = pageIndex;
    editingLatexIndex_ = latexIndex;
    editingLatexCreates_ = false;
    currentPage_ = pageIndex;
    emit EditStateChanged();
    return true;
}

const PageLatex* QtDocumentController::EditingLatexObject() const {
    if (!editingLatexPage_.has_value() || !editingLatexIndex_.has_value()) {
        return nullptr;
    }
    const Page* page = PageAt(*editingLatexPage_);
    if (page == nullptr || *editingLatexIndex_ >= page->latexes.size()) {
        return nullptr;
    }
    return &page->latexes[*editingLatexIndex_];
}

std::size_t QtDocumentController::EditingPageIndex() const {
    if (editingTextPage_.has_value()) {
        return *editingTextPage_;
    }
    if (editingLatexPage_.has_value()) {
        return *editingLatexPage_;
    }
    return currentPage_;
}

void QtDocumentController::CommitTextEdit(PageText edited) {
    if (!editingTextPage_.has_value() || !editingTextIndex_.has_value() || session_ == nullptr) {
        CancelTextEdit();
        return;
    }

    const std::size_t pageIndex = *editingTextPage_;
    const std::size_t textIndex = *editingTextIndex_;
    const PageText before = editingTextBefore_;
    const bool creates = editingTextCreates_;
    editingTextPage_.reset();
    editingTextIndex_.reset();
    editingTextCreates_ = false;

    Page* page = PageAt(pageIndex);
    if (page == nullptr || textIndex >= page->texts.size()) {
        emit EditStateChanged();
        return;
    }

    // The overlay owns the model's identity and position; only content and spans
    // come from the edit.
    PageText& stored = page->texts[textIndex];
    if (edited.id == 0U) {
        edited.id = stored.id;
    }
    edited.x = stored.x;
    edited.y = stored.y;

    HistoryAction action {};
    bool changed = false;
    if (edited.content.empty()) {
        page->texts.erase(page->texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
        if (!creates) {
            action.type = HistoryActionType::RemoveText;
            action.pageIndex = pageIndex;
            action.index = textIndex;
            action.text = before;
            changed = true;
        }
    } else {
        edited = text::NormalizeCommittedText(edited);
        page->texts[textIndex] = edited;
        if (creates) {
            action.type = HistoryActionType::AddText;
            action.pageIndex = pageIndex;
            action.index = textIndex;
            action.text = edited;
            changed = true;
        } else if (!PageTextEqual(before, edited)) {
            action.type = HistoryActionType::UpdateText;
            action.pageIndex = pageIndex;
            action.index = textIndex;
            action.text = before;
            action.replacementText = edited;
            changed = true;
        }
    }

    if (changed) {
        session_->undoActions.push_back(std::move(action));
        session_->redoActions.clear();
        session_->dirty = true;
        ++session_->revision;
        NotifySessionChanged(true);
    }
    emit EditStateChanged();
}

void QtDocumentController::CancelTextEdit() {
    if (!editingTextIndex_.has_value()) {
        return;
    }
    // A cancelled creation removes its placeholder again (the reference erases
    // an object created by the cancelled edit); nothing was recorded in history.
    if (editingTextCreates_ && editingTextPage_.has_value() && editingTextIndex_.has_value()) {
        if (Page* page = PageAt(*editingTextPage_); page != nullptr && *editingTextIndex_ < page->texts.size()) {
            page->texts.erase(page->texts.begin() + static_cast<std::ptrdiff_t>(*editingTextIndex_));
        }
    }
    editingTextPage_.reset();
    editingTextIndex_.reset();
    editingTextCreates_ = false;
    emit EditStateChanged();
}

void QtDocumentController::CommitLatexEdit(const QString& source) {
    if (!editingLatexPage_.has_value() || !editingLatexIndex_.has_value() || session_ == nullptr) {
        CancelLatexEdit();
        return;
    }

    const std::size_t pageIndex = *editingLatexPage_;
    const std::size_t latexIndex = *editingLatexIndex_;
    const PageLatex before = editingLatexBefore_;
    const bool creates = editingLatexCreates_;
    editingLatexPage_.reset();
    editingLatexIndex_.reset();
    editingLatexCreates_ = false;

    Page* page = PageAt(pageIndex);
    if (page == nullptr || latexIndex >= page->latexes.size()) {
        emit EditStateChanged();
        return;
    }

    PageLatex edited = page->latexes[latexIndex];
    edited.content = source.toStdU16String();

    HistoryAction action {};
    bool changed = false;
    if (edited.content.empty()) {
        page->latexes.erase(page->latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
        if (!creates) {
            action.type = HistoryActionType::RemoveLatex;
            action.pageIndex = pageIndex;
            action.index = latexIndex;
            action.latex = before;
            changed = true;
        }
    } else {
        page->latexes[latexIndex] = edited;
        if (creates) {
            action.type = HistoryActionType::AddLatex;
            action.pageIndex = pageIndex;
            action.index = latexIndex;
            action.latex = edited;
            changed = true;
        } else if (!PageLatexEqual(before, edited)) {
            action.type = HistoryActionType::UpdateLatex;
            action.pageIndex = pageIndex;
            action.index = latexIndex;
            action.latex = before;
            action.replacementLatex = edited;
            changed = true;
        }
    }

    if (changed) {
        session_->undoActions.push_back(std::move(action));
        session_->redoActions.clear();
        session_->dirty = true;
        ++session_->revision;
        NotifySessionChanged(true);
    }
    emit EditStateChanged();
}

void QtDocumentController::CancelLatexEdit() {
    if (!editingLatexIndex_.has_value()) {
        return;
    }
    if (editingLatexCreates_ && editingLatexPage_.has_value() && editingLatexIndex_.has_value()) {
        if (Page* page = PageAt(*editingLatexPage_); page != nullptr && *editingLatexIndex_ < page->latexes.size()) {
            page->latexes.erase(page->latexes.begin() + static_cast<std::ptrdiff_t>(*editingLatexIndex_));
        }
    }
    editingLatexPage_.reset();
    editingLatexIndex_.reset();
    editingLatexCreates_ = false;
    emit EditStateChanged();
}

// --- selection, page gestures and erase -------------------------------------

bool QtDocumentController::HasSelection() const {
    for (const auto& entry : selection_) {
        if (!entry.second.Empty()) {
            return true;
        }
    }
    return false;
}

const ui::PageSelection& QtDocumentController::SelectionFor(std::size_t pageIndex) const {
    static const ui::PageSelection kEmpty {};
    const auto entry = selection_.find(pageIndex);
    return entry != selection_.end() ? entry->second : kEmpty;
}

void QtDocumentController::ClearSelection() {
    selection_.clear();
}

void QtDocumentController::AddObjectToSelection(const ui::ObjectRef& object) {
    ui::PageSelection& selection = selection_[object.pageIndex];
    switch (object.kind) {
    case ui::ObjectKind::Stroke:
        selection.strokes.push_back(object.objectIndex);
        break;
    case ui::ObjectKind::Image:
        selection.images.push_back(object.objectIndex);
        break;
    case ui::ObjectKind::Text:
        selection.texts.push_back(object.objectIndex);
        break;
    case ui::ObjectKind::Latex:
        selection.latexes.push_back(object.objectIndex);
        break;
    }
}

void QtDocumentController::SelectObjects(std::size_t pageIndex, ui::PageSelection selection) {
    if (selection.Empty()) {
        selection_.erase(pageIndex);
        return;
    }
    selection_[pageIndex] = std::move(selection);
}

void QtDocumentController::BeginPageGesture(std::size_t pageIndex) {
    CancelPageGesture();
    const Page* page = PageAt(pageIndex);
    if (page == nullptr) {
        return;
    }
    gesturePage_ = pageIndex;
    gestureBefore_ = *page;
    gestureChanged_ = false;
}

Page* QtDocumentController::GesturePage() {
    return gesturePage_.has_value() ? PageAt(*gesturePage_) : nullptr;
}

void QtDocumentController::CommitPageGesture() {
    if (!gesturePage_.has_value()) {
        return;
    }
    const std::size_t pageIndex = *gesturePage_;
    Page before = gestureBefore_;
    gesturePage_.reset();
    gestureBefore_ = Page {};
    const bool changed = gestureChanged_;
    gestureChanged_ = false;

    Page* page = PageAt(pageIndex);
    if (!changed || page == nullptr || session_ == nullptr || PageEqual(before, *page)) {
        return;
    }

    // One transaction per gesture, the reference's PushReplacePagesHistoryAction.
    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pageIndices = { pageIndex };
    action.pages = { std::move(before) };
    action.replacementPages = { *page };
    action.focusPageIndex = pageIndex;
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    NotifySessionChanged(true);
}

void QtDocumentController::CancelPageGesture() {
    if (!gesturePage_.has_value()) {
        gestureChanged_ = false;
        return;
    }
    const std::size_t pageIndex = *gesturePage_;
    Page before = std::move(gestureBefore_);
    gesturePage_.reset();
    gestureBefore_ = Page {};
    gestureChanged_ = false;
    if (Page* page = PageAt(pageIndex); page != nullptr) {
        *page = std::move(before);
    }
}

void QtDocumentController::MoveSelectionBy(std::size_t pageIndex, Vec2 delta) {
    Page* page = GesturePage();
    if (page == nullptr || (delta.x == 0.0f && delta.y == 0.0f)) {
        return;
    }
    const ui::PageSelection& selection = SelectionFor(pageIndex);

    for (const std::size_t index : selection.strokes) {
        if (index >= page->strokes.size()) {
            continue;
        }
        const Vec2 clamped = ui::ClampStrokeDeltaToPage(page->strokes[index], *page, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        ui::TranslateStrokePoints(page->strokes[index], clamped);
        gestureChanged_ = true;
    }
    for (const std::size_t index : selection.images) {
        if (index >= page->images.size()) {
            continue;
        }
        PageImage& image = page->images[index];
        const Vec2 clamped = ui::ClampImageDeltaToPage(image, *page, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        image.x += clamped.x;
        image.y += clamped.y;
        gestureChanged_ = true;
    }
    for (const std::size_t index : selection.texts) {
        if (index >= page->texts.size()) {
            continue;
        }
        PageText& text = page->texts[index];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page->width);
        const Vec2 clamped {
            std::clamp(delta.x, -bounds.left, page->width - bounds.right),
            std::clamp(delta.y, -bounds.top, page->height - bounds.bottom),
        };
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        text.x += clamped.x;
        text.y += clamped.y;
        gestureChanged_ = true;
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page->latexes.size()) {
            continue;
        }
        PageLatex& object = page->latexes[index];
        const Vec2 clamped = ui::ClampLatexDeltaToPage(object, *page, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        object.x += clamped.x;
        object.y += clamped.y;
        gestureChanged_ = true;
    }
}

void QtDocumentController::CommitErase(std::size_t pageIndex, const std::vector<ui::ObjectRef>& targets) {
    Page* page = PageAt(pageIndex);
    if (page == nullptr || targets.empty()) {
        return;
    }

    std::vector<std::size_t> strokes;
    std::vector<std::size_t> images;
    std::vector<std::size_t> texts;
    std::vector<std::size_t> latexes;
    for (const ui::ObjectRef& target : targets) {
        if (target.pageIndex != pageIndex) {
            continue;
        }
        switch (target.kind) {
        case ui::ObjectKind::Stroke: strokes.push_back(target.objectIndex); break;
        case ui::ObjectKind::Image: images.push_back(target.objectIndex); break;
        case ui::ObjectKind::Text: texts.push_back(target.objectIndex); break;
        case ui::ObjectKind::Latex: latexes.push_back(target.objectIndex); break;
        }
    }

    BeginPageGesture(pageIndex);
    const auto removeDescending = [](auto& objects, std::vector<std::size_t>& indices) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
            if (*it < objects.size()) {
                objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(*it));
            }
        }
    };
    // Reference order (App::EndErase): texts, latexes, images, strokes.
    removeDescending(page->texts, texts);
    removeDescending(page->latexes, latexes);
    removeDescending(page->images, images);
    removeDescending(page->strokes, strokes);

    // The erased objects leave the selection as well.
    ui::PageSelection remaining = SelectionFor(pageIndex);
    const auto drop = [](std::vector<std::size_t>& selected, const std::vector<std::size_t>& removed) {
        selected.erase(
            std::remove_if(selected.begin(), selected.end(), [&](std::size_t index) {
                return std::find(removed.begin(), removed.end(), index) != removed.end();
            }),
            selected.end());
    };
    drop(remaining.strokes, strokes);
    drop(remaining.latexes, latexes);
    drop(remaining.images, images);
    drop(remaining.texts, texts);
    SelectObjects(pageIndex, std::move(remaining));

    gestureChanged_ = true;
    CommitPageGesture();
}

void QtDocumentController::SetPendingStrokeDotted(bool dotted) {
    if (pendingStroke_.has_value()) {
        pendingStroke_->style.dotted = dotted;
    }
}

void QtDocumentController::ReplacePendingStrokePoints(const std::vector<Vec2>& points, bool dotted) {
    if (!pendingStroke_.has_value()) {
        return;
    }
    pendingStroke_->points = points;
    pendingStroke_->style.dotted = dotted;
}

}  // namespace cnote::qt
