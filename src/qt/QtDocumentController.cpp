#include "qt/QtDocumentController.h"

#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "PdfBackend.h"
#include "Unicode.h"
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

// Upper bound on extracted PDF char-box pages kept per view; extraction is
// per-pointer-event work in the reference, cached here instead.
constexpr std::size_t kPdfTextCacheCapacity = 64;

// App::AppendPdfUnicode (Clipboard.cpp:27-37): code points beyond the BMP are
// stored as a UTF-16 surrogate pair.
void AppendPdfUnicode(std::u16string& text, std::uint32_t unicode) {
    if (unicode <= 0xFFFFU) {
        text.push_back(static_cast<char16_t>(unicode));
        return;
    }
    if (unicode <= 0x10FFFFU) {
        const std::uint32_t codePoint = unicode - 0x10000U;
        text.push_back(static_cast<char16_t>(0xD800U + (codePoint >> 10U)));
        text.push_back(static_cast<char16_t>(0xDC00U + (codePoint & 0x3FFU)));
    }
}

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

void QtDocumentController::DetachForClose() {
    CancelStroke();
    manager_.UnregisterView(this);
}

void QtDocumentController::ReattachForShow() {
    if (session_ != nullptr) {
        manager_.RegisterView(this, this, session_);
    }
}

bool QtDocumentController::SaveTo(const std::filesystem::path& path, std::string* error) {
    if (session_ == nullptr || path.empty()) {
        return false;
    }

    // Serialize once and commit through QSaveFile (plan 6.6): a failure must
    // never leave a truncated project behind, and dirty/FilePath only change
    // after the destination is committed.
    const std::vector<std::uint8_t> bytes = ProjectFile::Serialize(session_->document);
    QSaveFile file(QString::fromStdU16String(path.u16string()));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = "cannot write " + PathToUtf8(path);
        }
        return false;
    }
    const qint64 expected = static_cast<qint64>(bytes.size());
    const bool written = bytes.empty()
        || file.write(reinterpret_cast<const char*>(bytes.data()), expected) == expected;
    if (!written || !file.commit()) {
        file.cancelWriting();
        if (error != nullptr) {
            *error = "cannot write " + PathToUtf8(path);
        }
        return false;
    }

    session_->document.SetFilePath(path);
    session_->dirty = false;
    manager_.NotifySessionSaved(session_);
    BroadcastChange(ProjectChange::Kind::Saved);
    return true;
}

void QtDocumentController::BroadcastChange(ProjectChange::Kind kind) {
    if (session_ == nullptr) {
        return;
    }
    if (kind == ProjectChange::Kind::Saved) {
        ++savedChangeCount_;
    }
    ProjectChange change;
    change.kind = kind;
    for (QObject* owner : manager_.ViewsForSession(session_)) {
        if (auto* view = qobject_cast<QtDocumentController*>(owner); view != nullptr) {
            view->OnSessionChanged(change, view == this);
        }
    }
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
            input_.dashLength,
            input_.dashGap,
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

void QtDocumentController::OnSessionChanged(const ProjectChange& change, bool originating) {
    if (originating) {
        return;
    }
    if (change.kind == ProjectChange::Kind::Committed) {
        // Another view committed: drop the pending preview and refresh caches.
        pendingStroke_.reset();
        pendingStrokePage_.reset();
    }
    emit SessionChanged();
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
    session_->document.TouchPage(pageIndex);
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
    text.color = input_.objectColor;
    // The placeholder enters the page now (excluded from cached surfaces while
    // it is under edit); bump the epoch so a failed BeginTextEdit still leaves
    // consistent caches.
    session_->document.TouchPage(pageIndex);
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
    latex.color = input_.objectColor;
    session_->document.TouchPage(pageIndex);
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
        session_->document.TouchPage(pageIndex);
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
        session_->document.TouchPage(pageIndex);
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
            session_->document.TouchPage(*editingTextPage_);
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
        session_->document.TouchPage(pageIndex);
        page->latexes.erase(page->latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
        if (!creates) {
            action.type = HistoryActionType::RemoveLatex;
            action.pageIndex = pageIndex;
            action.index = latexIndex;
            action.latex = before;
            changed = true;
        }
    } else {
        session_->document.TouchPage(pageIndex);
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
            session_->document.TouchPage(*editingLatexPage_);
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
    emit SelectionChanged();
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
    emit SelectionChanged();
}

void QtDocumentController::SelectObjects(std::size_t pageIndex, ui::PageSelection selection) {
    if (selection.Empty()) {
        selection_.erase(pageIndex);
    } else {
        selection_[pageIndex] = std::move(selection);
    }
    emit SelectionChanged();
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
    moveBounds_.built = false;
}

const std::vector<PdfTextCharBox>* QtDocumentController::PdfTextCharBoxesFor(std::size_t pageIndex) const {
    const Page* page = PageAt(pageIndex);
    if (page == nullptr || pdfBackend_ == nullptr) {
        return nullptr;
    }
    const Document::PdfSource* pdfSource = DocumentRef().PdfSourceAtIndex(page->sourcePdfSourceIndex);
    if (pdfSource == nullptr || page->sourcePdfPageIndex < 0
        || !pdfSource->pdfData || pdfSource->pdfData->empty()) {
        return nullptr;
    }
    const PdfTextCacheKey key {
        pdfSource->pdfData,
        static_cast<std::size_t>(page->sourcePdfPageIndex),
    };
    if (const auto cached = pdfTextCache_.find(key); cached != pdfTextCache_.end()) {
        return &cached->second;
    }
    const std::optional<std::vector<PdfTextCharBox>> boxes = pdfBackend_->ExtractTextCharBoxes(
        pdfSource->pdfData, static_cast<std::size_t>(page->sourcePdfPageIndex));
    if (!boxes.has_value()) {
        return nullptr;
    }
    // Extraction reads the whole PDF text page (pdfium LoadMemDocument + text
    // page walk), so hit-testing without this cache cost ~10 ms per probe.
    if (pdfTextCache_.size() >= kPdfTextCacheCapacity) {
        pdfTextCache_.clear();
    }
    const auto inserted = pdfTextCache_.emplace(key, std::move(*boxes));
    return &inserted.first->second;
}

bool QtDocumentController::HasSelectedPdfText() const {
    for (const auto& [pageIndex, selection] : selection_) {
        if (!selection.pdfText.empty()) {
            return true;
        }
    }
    return false;
}

QString QtDocumentController::SelectedPdfText() const {
    // App::CopyPdfTextSelectionToClipboard (Clipboard.cpp:73-113): characters
    // are emitted in document order; a line break is inserted when the next
    // box sits well below the previous one or wraps back to the left.
    std::u16string text;
    const PdfTextCharBox* previousBox = nullptr;
    for (const auto& [pageIndex, selection] : selection_) {
        for (const PdfTextCharBox& box : selection.pdfText) {
            if (previousBox != nullptr) {
                const float previousHeight = previousBox->bottom - previousBox->top;
                const float currentHeight = box.bottom - box.top;
                const float lineBreakThreshold = std::max(previousHeight, currentHeight) * 0.5f;
                if (box.top > previousBox->bottom + lineBreakThreshold
                    || box.left + (box.right - box.left) * 0.5f < previousBox->left) {
                    text.push_back(L'\n');
                }
            }
            AppendPdfUnicode(text, box.unicode);
            previousBox = &box;
        }
    }
    return QString::fromStdU16String(text);
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

    // The page was mutated in place during the gesture (each sample touched
    // excluded objects only, so no epoch was needed mid-flight); the commit
    // makes the change visible to renderer caches.
    session_->document.TouchPage(pageIndex);
    // One transaction per gesture, the reference's PushReplacePagesHistoryAction.
    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pageIndices = { pageIndex };
    action.pages = { std::move(before) };
    action.replacementPages = { *page };
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

    // First move of the gesture measures every selected object's bounds once;
    // later samples reuse them and translate by the applied delta.  Without
    // this each pointer event re-scans every selected stroke's points and
    // re-runs text/LaTeX measurement just to clamp the delta.
    if (!moveBounds_.built) {
        moveBounds_.strokes.clear();
        moveBounds_.images.clear();
        moveBounds_.texts.clear();
        moveBounds_.latexes.clear();
        for (const std::size_t index : selection.strokes) {
            ui::SelectionBounds bounds {};
            if (index < page->strokes.size()) {
                bounds.valid = ui::StrokeBounds(
                    page->strokes[index], &bounds.left, &bounds.top, &bounds.right, &bounds.bottom);
            }
            moveBounds_.strokes.push_back(bounds);
        }
        for (const std::size_t index : selection.images) {
            ui::SelectionBounds bounds {};
            if (index < page->images.size()) {
                for (const Vec2& corner : ui::ImageCorners(page->images[index])) {
                    if (!bounds.valid) {
                        bounds = ui::SelectionBounds { corner.x, corner.y, corner.x, corner.y, true };
                        continue;
                    }
                    bounds.left = std::min(bounds.left, corner.x);
                    bounds.top = std::min(bounds.top, corner.y);
                    bounds.right = std::max(bounds.right, corner.x);
                    bounds.bottom = std::max(bounds.bottom, corner.y);
                }
            }
            moveBounds_.images.push_back(bounds);
        }
        for (const std::size_t index : selection.texts) {
            ui::SelectionBounds bounds {};
            if (index < page->texts.size()) {
                const text::QtTextBounds textBounds =
                    text::MeasurePageTextBoundsQt(page->texts[index], page->width);
                bounds = ui::SelectionBounds {
                    textBounds.left, textBounds.top, textBounds.right, textBounds.bottom, true };
            }
            moveBounds_.texts.push_back(bounds);
        }
        for (const std::size_t index : selection.latexes) {
            ui::SelectionBounds bounds {};
            if (index < page->latexes.size()) {
                const ui::LatexBounds latexBounds =
                    ui::MeasurePageLatexBounds(page->latexes[index], page->width);
                bounds = ui::SelectionBounds {
                    latexBounds.left, latexBounds.top, latexBounds.right, latexBounds.bottom, true };
            }
            moveBounds_.latexes.push_back(bounds);
        }
        moveBounds_.built = true;
    }

    // Clamps `delta` against the cached bounds and keeps them current by
    // translating with whatever was actually applied.
    const auto clampedDelta = [this, page](std::size_t slot, const std::vector<ui::SelectionBounds>& bounds, Vec2 delta) {
        const ui::SelectionBounds& b = bounds[slot];
        if (!b.valid) {
            return Vec2 {};
        }
        return Vec2 {
            std::clamp(delta.x, -b.left, page->width - b.right),
            std::clamp(delta.y, -b.top, page->height - b.bottom),
        };
    };
    const auto advance = [](ui::SelectionBounds& b, Vec2 applied) {
        b.left += applied.x;
        b.right += applied.x;
        b.top += applied.y;
        b.bottom += applied.y;
    };

    for (std::size_t slot = 0; slot < selection.strokes.size(); ++slot) {
        const std::size_t index = selection.strokes[slot];
        if (index >= page->strokes.size() || slot >= moveBounds_.strokes.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds_.strokes, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        ui::TranslateStrokePoints(page->strokes[index], clamped);
        advance(moveBounds_.strokes[slot], clamped);
        gestureChanged_ = true;
    }
    for (std::size_t slot = 0; slot < selection.images.size(); ++slot) {
        const std::size_t index = selection.images[slot];
        if (index >= page->images.size() || slot >= moveBounds_.images.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds_.images, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        page->images[index].x += clamped.x;
        page->images[index].y += clamped.y;
        advance(moveBounds_.images[slot], clamped);
        gestureChanged_ = true;
    }
    for (std::size_t slot = 0; slot < selection.texts.size(); ++slot) {
        const std::size_t index = selection.texts[slot];
        if (index >= page->texts.size() || slot >= moveBounds_.texts.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds_.texts, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        page->texts[index].x += clamped.x;
        page->texts[index].y += clamped.y;
        advance(moveBounds_.texts[slot], clamped);
        gestureChanged_ = true;
    }
    for (std::size_t slot = 0; slot < selection.latexes.size(); ++slot) {
        const std::size_t index = selection.latexes[slot];
        if (index >= page->latexes.size() || slot >= moveBounds_.latexes.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds_.latexes, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        page->latexes[index].x += clamped.x;
        page->latexes[index].y += clamped.y;
        advance(moveBounds_.latexes[slot], clamped);
        gestureChanged_ = true;
    }
}
void QtDocumentController::TransformSelection(std::size_t pageIndex, const ui::SelectionTransform& transform) {
    Page* page = GesturePage();
    if (page == nullptr || !transform.from.valid) {
        return;
    }

    // Absolute, not cumulative: every update restores the gesture snapshot and
    // re-applies the transform, so repeated UpdateTransform calls cannot drift.
    // Only the selected objects are restored — copying the whole page per
    // pointer event deep-copied every unselected stroke, image raster and text
    // run, which dominated the gesture on dense pages.
    const ui::PageSelection& selection = SelectionFor(pageIndex);
    for (const std::size_t index : selection.strokes) {
        if (index < page->strokes.size() && index < gestureBefore_.strokes.size()) {
            page->strokes[index] = gestureBefore_.strokes[index];
        }
    }
    for (const std::size_t index : selection.images) {
        if (index < page->images.size() && index < gestureBefore_.images.size()) {
            page->images[index] = gestureBefore_.images[index];
        }
    }
    for (const std::size_t index : selection.texts) {
        if (index < page->texts.size() && index < gestureBefore_.texts.size()) {
            page->texts[index] = gestureBefore_.texts[index];
        }
    }
    for (const std::size_t index : selection.latexes) {
        if (index < page->latexes.size() && index < gestureBefore_.latexes.size()) {
            page->latexes[index] = gestureBefore_.latexes[index];
        }
    }

    const float fromWidth = transform.from.right - transform.from.left;
    const float fromHeight = transform.from.bottom - transform.from.top;
    // Degenerate source extents keep the source scale (the canvas clamps a
    // resize to 1 page unit, but a selection can legitimately be flat).
    const float scaleX = fromWidth > 0.0001f ? (transform.to.right - transform.to.left) / fromWidth : 1.0f;
    const float scaleY = fromHeight > 0.0001f ? (transform.to.bottom - transform.to.top) / fromHeight : 1.0f;

    for (const std::size_t index : selection.strokes) {
        if (index >= page->strokes.size()) {
            continue;
        }
        for (Vec2& point : page->strokes[index].points) {
            point = ui::MapSelectionTransformPoint(point, transform.from, transform.to, transform.rotateDegrees);
        }
        gestureChanged_ = true;
    }
    for (const std::size_t index : selection.images) {
        if (index >= page->images.size()) {
            continue;
        }
        PageImage& image = page->images[index];
        // Scale about the mapped centre: the rotated frame is not axis aligned,
        // so the rotated image keeps its own rotation and accumulates the
        // gesture's.
        const Vec2 mappedCenter = ui::MapSelectionTransformPoint(
            ui::ImageCenter(image), transform.from, transform.to, transform.rotateDegrees);
        image.width *= scaleX;
        image.height *= scaleY;
        image.x = mappedCenter.x - (image.width * 0.5f);
        image.y = mappedCenter.y - (image.height * 0.5f);
        image.rotationDegrees += transform.rotateDegrees;
        gestureChanged_ = true;
    }
    for (const std::size_t index : selection.texts) {
        if (index >= page->texts.size()) {
            continue;
        }
        PageText& text = page->texts[index];
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page->width);
        if (transform.rotateDegrees != 0.0f || transform.scaleTextFont) {
            // Rotation and Shift-resize keep the reference behaviour: the
            // origin follows the mapped top-left and the font scales with the
            // frame (App::UpdateTextTransform shift path, App.cpp:5032-5038).
            const Vec2 mapped = ui::MapSelectionTransformPoint(
                { bounds.left, bounds.top }, transform.from, transform.to, transform.rotateDegrees);
            text.x += mapped.x - bounds.left;
            text.y += mapped.y - bounds.top;
            if (transform.scaleTextFont) {
                const float textScale = std::max(0.05f,
                    (transform.to.right - transform.to.left) / std::max(1.0f, transform.from.right - transform.from.left));
                text.fontSize = std::max(1.0f, text.fontSize * textScale);
                for (PageTextStyleSpan& span : text.styleSpans) {
                    span.style.fontSize = std::max(1.0f, span.style.fontSize * textScale);
                }
                if (text.width > 0.0f) {
                    text.width = std::max(1.0f, text.width * textScale);
                }
                if (text.height > 0.0f) {
                    text.height = std::max(1.0f, text.height * textScale);
                }
            }
            gestureChanged_ = true;
            continue;
        }

        // Box resize (App::UpdateTextTransform non-shift path, App.cpp:5045-5125):
        // the text box follows the mapped bounds so the user chooses where
        // wrapping happens. A pure vertical drag keeps the wrap width; a
        // horizontal drag rewrites text.width (auto-width text becomes
        // explicit) and resets the height to auto, exactly like the reference.
        const Vec2 mappedTopLeft = ui::MapSelectionTransformPoint(
            { bounds.left, bounds.top }, transform.from, transform.to, 0.0f);
        const Vec2 mappedBottomRight = ui::MapSelectionTransformPoint(
            { bounds.right, bounds.bottom }, transform.from, transform.to, 0.0f);
        const bool widthChanged = transform.to.left != transform.from.left
            || transform.to.right != transform.from.right;
        const bool heightChanged = transform.to.top != transform.from.top
            || transform.to.bottom != transform.from.bottom;
        text.x += mappedTopLeft.x - bounds.left;
        text.y += mappedTopLeft.y - bounds.top;
        if (widthChanged) {
            text.width = std::max(1.0f, mappedBottomRight.x - mappedTopLeft.x);
            text.height = 0.0f;
        }
        if (heightChanged) {
            text.height = std::max(1.0f, mappedBottomRight.y - mappedTopLeft.y);
        }
        gestureChanged_ = true;
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page->latexes.size()) {
            continue;
        }
        PageLatex& object = page->latexes[index];
        const ui::LatexBounds bounds = ui::MeasurePageLatexBounds(object, page->width);
        const Vec2 mapped = ui::MapSelectionTransformPoint(
            { bounds.left, bounds.top }, transform.from, transform.to, transform.rotateDegrees);
        object.x += mapped.x - bounds.left;
        object.y += mapped.y - bounds.top;
        object.fontSize *= scaleY;
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
bool QtDocumentController::RecolorSelection(std::uint32_t color) {
    // Reference App::SetColorIndex (ToolbarState.cpp:512-543) recolors selected
    // strokes; this port extends the same idea to text and LaTeX objects and
    // routes it through the page-gesture machinery so the recolor is one
    // undoable ReplacePages action instead of an unrecorded mutation.
    bool recolored = false;
    for (auto& [pageIndex, selection] : selection_) {
        if (selection.Empty()) {
            continue;
        }
        BeginPageGesture(pageIndex);
        Page* page = GesturePage();
        if (page == nullptr) {
            continue;
        }
        for (const std::size_t index : selection.strokes) {
            if (index < page->strokes.size() && page->strokes[index].style.color != color) {
                page->strokes[index].style.color = color;
                gestureChanged_ = true;
            }
        }
        for (const std::size_t index : selection.texts) {
            if (index >= page->texts.size()) {
                continue;
            }
            PageText& text = page->texts[index];
            if (text.color != color) {
                text.color = color;
                gestureChanged_ = true;
            }
            for (PageTextStyleSpan& span : text.styleSpans) {
                if (span.style.color != color) {
                    span.style.color = color;
                    gestureChanged_ = true;
                }
            }
        }
        for (const std::size_t index : selection.latexes) {
            if (index < page->latexes.size() && page->latexes[index].color != color) {
                page->latexes[index].color = color;
                gestureChanged_ = true;
            }
        }
        recolored = recolored || gestureChanged_;
        CommitPageGesture();
    }
    return recolored;
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

// --- page operations --------------------------------------------------------

namespace {

// Page snapshot from the new-page draft, exactly like the reference
// NewPageSettingsWindow apply path.
Page MakePageFromDraft(const ui::NewPageDraft& draft, const std::vector<std::uint32_t>& palette) {
    Page page {};
    ui::ApplyConfiguredSettingsToPage(
        page,
        std::max(ui::kMinPageSize, draft.width),
        std::max(ui::kMinPageSize, draft.height),
        draft.backgroundColor,
        ui::ResolvedPaletteProfile(draft.paletteProfile, palette),
        draft.applyPaletteProfileToExistingStrokes);
    return page;
}

}  // namespace

std::size_t QtDocumentController::InsertPages(std::size_t index, std::size_t count, const ui::NewPageDraft& draft) {
    if (session_ == nullptr || count == 0U) {
        return 0U;
    }
    Document& document = session_->document;
    const std::size_t insertIndex = std::min(index, document.PageCount());

    std::vector<Page> pages;
    pages.reserve(count);
    for (std::size_t offset = 0; offset < count; ++offset) {
        pages.push_back(MakePageFromDraft(draft, session_->document.NewPageDefaults().paletteProfile));
    }
    for (std::size_t offset = 0; offset < pages.size(); ++offset) {
        document.InsertPage(insertIndex + offset, pages[offset]);
    }
    document.SetCurrentPageIndex(insertIndex);

    // Selection indices are per-page positions: every selection at or after
    // the insertion point shifts by the inserted count.
    if (!selection_.empty()) {
        std::map<std::size_t, ui::PageSelection> remapped;
        for (auto& entry : selection_) {
            remapped[entry.first >= insertIndex ? entry.first + count : entry.first] = std::move(entry.second);
        }
        selection_ = std::move(remapped);
    }
    HistoryAction action {};
    action.type = HistoryActionType::AddPages;
    action.pages = std::move(pages);
    action.index = insertIndex;
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    currentPage_ = insertIndex;
    NotifySessionChanged(true);
    return insertIndex;
}

bool QtDocumentController::CopyPage(std::size_t index) {
    if (session_ == nullptr || index >= DocumentRef().PageCount()) {
        return false;
    }
    manager_.SetClipboardPage(DocumentRef().PageAt(index));
    return true;
}

bool QtDocumentController::CutPage(std::size_t index) {
    return CopyPage(index) && RemovePage(index);
}

bool QtDocumentController::RemovePage(std::size_t index) {
    if (session_ == nullptr) {
        return false;
    }
    Document& document = session_->document;
    if (index >= document.PageCount() || document.PageCount() <= 1U) {
        return false;
    }

    const Page removed = document.PageAt(index);
    document.RemovePage(index);

    // Selection indices are per-page positions: the removed page's selection
    // dies with it and every later selection shifts down one.
    if (!selection_.empty()) {
        std::map<std::size_t, ui::PageSelection> remapped;
        for (auto& entry : selection_) {
            if (entry.first < index) {
                remapped[entry.first] = std::move(entry.second);
            } else if (entry.first > index) {
                remapped[entry.first - 1U] = std::move(entry.second);
            }
        }
        selection_ = std::move(remapped);
    }

    HistoryAction action {};
    action.type = HistoryActionType::RemovePages;
    action.pages = { removed };
    action.index = index;
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    currentPage_ = document.CurrentPageIndex();
    emit SelectionChanged();
    NotifySessionChanged(true);
    return true;
}

bool QtDocumentController::PastePage(std::size_t index) {
    if (session_ == nullptr || !manager_.HasClipboardPage()) {
        return false;
    }
    Document& document = session_->document;
    const std::size_t insertIndex = std::min(index + 1U, document.PageCount());

    const Page pasted = *manager_.ClipboardPage();
    document.InsertPage(insertIndex, pasted);
    document.SetCurrentPageIndex(insertIndex);

    // Same per-page index shift as InsertPages.
    if (!selection_.empty()) {
        std::map<std::size_t, ui::PageSelection> remapped;
        for (auto& entry : selection_) {
            remapped[entry.first >= insertIndex ? entry.first + 1U : entry.first] = std::move(entry.second);
        }
        selection_ = std::move(remapped);
    }

    HistoryAction action {};
    action.type = HistoryActionType::AddPages;
    action.pages = { pasted };
    action.index = insertIndex;
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    currentPage_ = insertIndex;
    NotifySessionChanged(true);
    return true;
}

void QtDocumentController::ApplyNewPageDraft(const ui::NewPageDraft& draft, ui::NewPageApplyScope scope) {
    if (session_ == nullptr) {
        return;
    }
    Document& document = session_->document;
    const std::vector<std::uint32_t> palette = document.NewPageDefaults().paletteProfile;

    if (scope == ui::NewPageApplyScope::FuturePages) {
        SetNewPageDefaults(ui::NewPageDefaults {
            std::max(ui::kMinPageSize, draft.width),
            std::max(ui::kMinPageSize, draft.height),
            draft.backgroundColor,
            ui::ResolvedPaletteProfile(draft.paletteProfile, palette),
            draft.applyPaletteProfileToExistingStrokes,
        });
        return;
    }

    std::vector<std::size_t> indices;
    std::vector<Page> before;
    if (scope == ui::NewPageApplyScope::AllPages) {
        indices.reserve(document.PageCount());
        for (std::size_t page = 0; page < document.PageCount(); ++page) {
            indices.push_back(page);
        }
    } else if (document.PageCount() > 0U) {
        indices.push_back(std::min(currentPage_, document.PageCount() - 1U));
    }
    if (indices.empty()) {
        return;
    }

    bool changed = false;
    for (const std::size_t pageIndex : indices) {
        Page& page = document.PageAt(pageIndex);
        before.push_back(page);
        const bool pageChanged = ui::ApplyConfiguredSettingsToPage(
            page,
            std::max(ui::kMinPageSize, draft.width),
            std::max(ui::kMinPageSize, draft.height),
            draft.backgroundColor,
            ui::ResolvedPaletteProfile(draft.paletteProfile, palette),
            draft.applyPaletteProfileToExistingStrokes);
        if (pageChanged) {
            document.TouchPage(pageIndex);
        }
        changed = pageChanged || changed;
    }
    if (!changed) {
        return;
    }

    std::vector<Page> after;
    after.reserve(indices.size());
    for (const std::size_t pageIndex : indices) {
        after.push_back(document.PageAt(pageIndex));
    }

    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pageIndices = indices;
    action.pages = std::move(before);
    action.replacementPages = std::move(after);
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    NotifySessionChanged(true);
}

void QtDocumentController::SetNewPageDefaults(const ui::NewPageDefaults& defaults) {
    if (session_ == nullptr) {
        return;
    }
    Document& document = session_->document;
    Document::PageDefaults pageDefaults {};
    pageDefaults.width = std::max(ui::kMinPageSize, defaults.width);
    pageDefaults.height = std::max(ui::kMinPageSize, defaults.height);
    pageDefaults.backgroundColor = defaults.backgroundColor;
    pageDefaults.paletteProfile = defaults.paletteProfile;
    const Document::PageDefaults& stored = document.NewPageDefaults();
    const bool same = stored.width == pageDefaults.width && stored.height == pageDefaults.height
        && stored.backgroundColor == pageDefaults.backgroundColor
        && stored.paletteProfile == pageDefaults.paletteProfile;
    if (same) {
        return;
    }
    document.SetNewPageDefaults(pageDefaults);
    if (session_ != nullptr) {
        session_->dirty = true;
        ++session_->revision;
        NotifySessionChanged(true);
    }
}

ui::NewPageDefaults QtDocumentController::NewPageDefaults() const {
    const Document& document = DocumentRef();
    const Document::PageDefaults& stored = document.NewPageDefaults();
    ui::NewPageDefaults defaults {};
    defaults.width = stored.width;
    defaults.height = stored.height;
    defaults.backgroundColor = stored.backgroundColor;
    defaults.paletteProfile = stored.paletteProfile;
    return defaults;
}

// --- selection clipboard, document side -------------------------------------

bool QtDocumentController::BuildSelectionPayload(cnote::SelectionClipboardData& data) const {
    const Document& document = DocumentRef();
    data = cnote::SelectionClipboardData {};
    bool any = false;
    bool haveBounds = false;

    const auto growPoint = [&](float x, float y) {
        if (!haveBounds) {
            data.minX = data.maxX = x;
            data.minY = data.maxY = y;
            haveBounds = true;
            return;
        }
        data.minX = std::min(data.minX, x);
        data.minY = std::min(data.minY, y);
        data.maxX = std::max(data.maxX, x);
        data.maxY = std::max(data.maxY, y);
    };

    for (const auto& [pageIndex, selection] : selection_) {
        if (pageIndex >= document.PageCount() || selection.Empty()) {
            continue;
        }
        const Page& page = document.PageAt(pageIndex);
        for (const std::size_t index : selection.strokes) {
            if (index >= page.strokes.size()) {
                continue;
            }
            for (const Vec2& point : page.strokes[index].points) {
                growPoint(point.x, point.y);
            }
            data.strokes.push_back(page.strokes[index]);
            any = true;
        }
        for (const std::size_t index : selection.texts) {
            if (index >= page.texts.size()) {
                continue;
            }
            const PageText& text = page.texts[index];
            growPoint(text.x, text.y);
            growPoint(text.x + text.width, text.y + text.height);
            data.texts.push_back(text);
            any = true;
        }
        for (const std::size_t index : selection.latexes) {
            if (index >= page.latexes.size()) {
                continue;
            }
            growPoint(page.latexes[index].x, page.latexes[index].y);
            data.latexes.push_back(page.latexes[index]);
            any = true;
        }
        for (const std::size_t index : selection.images) {
            if (index >= page.images.size()) {
                continue;
            }
            const PageImage& image = page.images[index];
            growPoint(image.x, image.y);
            growPoint(image.x + image.width, image.y + image.height);
            data.images.push_back(image);
            any = true;
        }
    }
    return any;
}

bool QtDocumentController::RemoveSelection() {
    if (session_ == nullptr || selection_.empty()) {
        return false;
    }
    // PDF text is not document content: the reference's DeleteSelection just
    // clears it (Clipboard.cpp:225-235 cut still copies it first). Strip it
    // here; the object path below then handles any remaining real selection,
    // and a pdf-text-only selection ends as a clear, not a history step.
    for (auto& [pageIndex, selection] : selection_) {
        selection.pdfText.clear();
    }
    {
        bool hasObjects = false;
        for (const auto& [pageIndex, selection] : selection_) {
            if (pageIndex < session_->document.PageCount()
                && (!selection.strokes.empty() || !selection.images.empty()
                    || !selection.texts.empty() || !selection.latexes.empty())) {
                hasObjects = true;
                break;
            }
        }
        if (!hasObjects) {
            selection_.clear();
            emit SelectionChanged();
            return true;
        }
    }
    Document& document = session_->document;
    std::vector<std::size_t> indices;
    std::vector<Page> before;
    std::vector<Page> after;
    bool changed = false;

    for (const auto& [pageIndex, selection] : selection_) {
        if (pageIndex >= document.PageCount() || selection.Empty()) {
            continue;
        }
        indices.push_back(pageIndex);
        before.push_back(document.PageAt(pageIndex));
        document.TouchPage(pageIndex);
        Page& page = document.PageAt(pageIndex);

        const auto removeAt = [](auto& objects, std::vector<std::size_t> indices) {
            std::sort(indices.begin(), indices.end(), std::greater<> {});
            for (const std::size_t index : indices) {
                if (index < objects.size()) {
                    objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(index));
                }
            }
        };
        removeAt(page.strokes, selection.strokes);
        removeAt(page.texts, selection.texts);
        removeAt(page.latexes, selection.latexes);
        removeAt(page.images, selection.images);
        after.push_back(page);
        changed = true;
    }
    if (!changed) {
        return false;
    }

    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pageIndices = indices;
    action.pages = std::move(before);
    action.replacementPages = std::move(after);
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;
    selection_.clear();
    emit SelectionChanged();
    NotifySessionChanged(true);
    return true;
}

bool QtDocumentController::PastePayload(
    std::size_t pageIndex, Vec2 target, const cnote::SelectionClipboardData& data) {
    if (session_ == nullptr || DocumentRef().PageCount() == 0U) {
        return false;
    }
    if (data.strokes.empty() && data.texts.empty() && data.latexes.empty() && data.images.empty()) {
        return false;
    }
    pageIndex = std::min(pageIndex, DocumentRef().PageCount() - 1U);
    Document& document = session_->document;
    Page& page = document.PageAt(pageIndex);
    const Page before = page;

    // Center the payload on the target and keep it inside the page when it fits.
    const float width = data.maxX - data.minX;
    const float height = data.maxY - data.minY;
    float deltaX = target.x - (data.minX + width * 0.5f);
    float deltaY = target.y - (data.minY + height * 0.5f);
    if (width <= page.width) {
        deltaX = std::clamp(deltaX, -data.minX, page.width - data.maxX);
    }
    if (height <= page.height) {
        deltaY = std::clamp(deltaY, -data.minY, page.height - data.maxY);
    }

    ui::PageSelection pasted {};
    for (const Stroke& source : data.strokes) {
        Stroke stroke = source;
        ui::TranslateStrokePoints(stroke, Vec2 { deltaX, deltaY });
        pasted.strokes.push_back(page.strokes.size());
        page.strokes.push_back(std::move(stroke));
    }
    for (const PageText& source : data.texts) {
        PageText text = source;
        text.id = NextObjectId();
        text.x += deltaX;
        text.y += deltaY;
        pasted.texts.push_back(page.texts.size());
        page.texts.push_back(std::move(text));
    }
    for (const PageLatex& source : data.latexes) {
        PageLatex latex = source;
        latex.id = NextObjectId();
        latex.x += deltaX;
        latex.y += deltaY;
        pasted.latexes.push_back(page.latexes.size());
        page.latexes.push_back(std::move(latex));
    }
    for (const PageImage& source : data.images) {
        PageImage image = source;
        image.x += deltaX;
        image.y += deltaY;
        pasted.images.push_back(page.images.size());
        page.images.push_back(std::move(image));
    }

    document.TouchPage(pageIndex);

    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pageIndices = { pageIndex };
    action.pages = { before };
    action.replacementPages = { page };
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    session_->dirty = true;
    ++session_->revision;

    selection_.clear();
    selection_[pageIndex] = std::move(pasted);
    currentPage_ = pageIndex;
    emit SelectionChanged();
    NotifySessionChanged(true);
    return true;
}

}  // namespace cnote::qt
