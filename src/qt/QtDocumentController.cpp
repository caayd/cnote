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

// Same object-location rule as cnote::ApplyHistoryAction: an edit addresses its
// object by id, so a page insert, a rotation or a remote commit that moved the
// object can never make the commit write to whatever now sits at an old index.
std::optional<std::size_t> FindTextIndexById(const Page& page, std::uint64_t textId) {
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (page.texts[index].id == textId) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> FindLatexIndexById(const Page& page, std::uint64_t latexId) {
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (page.latexes[index].id == latexId) {
            return index;
        }
    }
    return std::nullopt;
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
    AbortTransientState();
    manager_.UnregisterView(this);
    session_ = std::move(session);
    if (session_ != nullptr) {
        manager_.RegisterView(this, this, session_);
    }
    currentPage_ = 0;
    if (session_ != nullptr) {
        // The session owns the viewed page (it is what ProjectFile serializes);
        // a view joining an existing session must not silently look at page 0.
        SetCurrentPageIndex(session_->document.CurrentPageIndex());
    }
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
    Document::PageDefaults defaults;
    defaults.width = defaultPageWidth_;
    defaults.height = defaultPageHeight_;
    session->document.SetNewPageDefaults(defaults);
    session->document.CurrentPage().width = defaults.width;
    session->document.CurrentPage().height = defaults.height;
    AttachSession(std::move(session));
}

void QtDocumentController::SetDefaultPageSize(float width, float height) {
    defaultPageWidth_ = width;
    defaultPageHeight_ = height;
}

void QtDocumentController::DetachForClose() {
    AbortTransientState();
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

    // A PDF-backed document's path is the PDF file.  Without this check the
    // first save wrote project bytes over the user's PDF (and the same for a
    // project recovered from a PDF attachment).  Saving to another destination
    // is legitimate - that is what Save As is for - and turns the document
    // into a project.
    const bool pdfBacked = !CanSaveInPlace();
    if (pdfBacked) {
        bool samePdfDestination = path == session_->document.FilePath();
        if (!samePdfDestination && !session_->document.FilePath().empty()) {
            // A relative FilePath (a command-line open) and the dialog's
            // absolute destination are the same file: compare the resolved
            // paths, not their spelling.
            std::error_code equivalentError;
            samePdfDestination = std::filesystem::equivalent(
                path, session_->document.FilePath(), equivalentError);
        }
        if (samePdfDestination) {
            if (error != nullptr) {
                *error = "cannot overwrite a PDF document: " + PathToUtf8(path) + " (use Save As)";
            }
            return false;
        }
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
    // The destination is a cnote project file now, so the document is one:
    // a PDF-backed document turns into a project (an in-place save is safe
    // from here on), and the save/close policy stops routing through Save As.
    session_->document.SetKind(DocumentKind::Project);
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
    // OnSessionChanged skips the originating view (it already knows), but the
    // save mutates observable state (FilePath, dirty), so the window still
    // needs the signal to refresh its chrome — title included.
    emit SessionChanged();
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

bool QtDocumentController::CanSaveInPlace() const {
    if (session_ == nullptr) {
        return false;
    }
    const DocumentKind kind = session_->document.Kind();
    return kind != DocumentKind::PdfContainer && kind != DocumentKind::EmbeddedProject;
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
    return pendingStroke_.has_value() || edit_.has_value() || gesture_.has_value();
}

void QtDocumentController::AbortTransientState(bool commitOpenEdit, bool restoreGesture) {
    // A half-drawn stroke is a preview, never a commit: undo/redo, a session
    // switch or a remote change ends it without writing to history.
    CancelStroke();
    if (gesture_.has_value()) {
        // The gesture snapshot was taken before this point in time.  Restore it
        // first, then finish the edit: a commit that landed before the restore
        // would be silently rolled back by the page snapshot.
        if (restoreGesture) {
            CancelPageGesture();
        } else {
            // The snapshot predates the other view's committed change: putting
            // it back would erase that change.  Just forget the gesture.
            gesture_.reset();
        }
    }
    if (commitOpenEdit) {
        FinishOpenEdit();
    } else {
        CancelTextEdit();
        CancelLatexEdit();
    }
}

void QtDocumentController::FinishOpenEdit() {
    if (!edit_.has_value()) {
        return;
    }
    // No source means no captured overlay content.  Committing an empty value
    // here is exactly the data-loss bug this session state exists to stop, so
    // cancel instead.
    if (editContent_ == nullptr) {
        CancelTextEdit();
        CancelLatexEdit();
        return;
    }
    if (edit_->kind == EditSession::Kind::Text) {
        CommitTextEdit(editContent_->CaptureEditedText());
    } else {
        CommitLatexEdit(editContent_->CaptureEditedLatexSource());
    }
}

void QtDocumentController::FinishInteraction() {
    if (pendingStroke_.has_value()) {
        EndStroke();
    }
    FinishOpenEdit();
    if (gesture_.has_value()) {
        CommitPageGesture();
    }
}

void QtDocumentController::OnSessionChanged(const ProjectChange& change, bool originating) {
    if (originating) {
        return;
    }
    if (change.kind == ProjectChange::Kind::Committed) {
        // Another view committed: any edit or gesture here points at a snapshot
        // (and, for the edit, an object) of a page this view no longer has the
        // last word on.  Drop it without writing anything back; never commit by
        // an index the other view may have shifted.
        AbortTransientState(false, false);
    }
    emit SessionChanged();
}

void QtDocumentController::SetCurrentPageIndex(std::size_t index) {
    if (session_ == nullptr || index >= session_->document.PageCount()) {
        return;
    }
    // The controller's page and the document's page are one state: the document
    // is what ProjectFile serializes, so a viewed-page change must reach it.
    session_->document.SetCurrentPageIndex(index);
    currentPage_ = index;
}

void QtDocumentController::NoteCommitted() {
    if (session_ == nullptr) {
        return;
    }
    session_->dirty = true;
    ++session_->revision;
    // The focused page follows the viewport (CanvasWidget::SyncFocusedPage), not
    // the edited page; the document keeps its index on the same page across
    // inserts and removals, so only resync to it here.
    SetCurrentPageIndex(session_->document.CurrentPageIndex());
    NotifySessionChanged(true);
}

void QtDocumentController::RecordCommit(HistoryAction action) {
    if (session_ == nullptr) {
        return;
    }
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
    NoteCommitted();
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

namespace {

float ClampPressure(float pressure) {
    return std::isfinite(pressure) ? std::clamp(pressure, 0.0f, 1.0f) : 0.5f;
}

}  // namespace

void QtDocumentController::BeginStroke(std::size_t pageIndex, Vec2 point, StrokeSampling sampling,
    std::optional<float> pressure) {
    FinishInteraction();
    pendingStroke_.reset();
    if (PageAt(pageIndex) == nullptr) {
        return;
    }

    PendingStrokeState pending;
    pending.pageIndex = pageIndex;
    pending.stroke.style = strokeStyle_;
    pending.stroke.smoothing = input_.strokeSmoothing;
    pending.stroke.sampling = sampling;
    pending.stroke.points.push_back(point);
    if (pressure.has_value()) {
        pending.stroke.pressures.push_back(ClampPressure(*pressure));
    }
    pendingStroke_ = std::move(pending);
}

void QtDocumentController::ExtendStroke(Vec2 point, std::optional<float> pressure) {
    if (!pendingStroke_.has_value()) {
        return;
    }
    Stroke& stroke = pendingStroke_->stroke;
    stroke.points.push_back(point);
    if (!stroke.pressures.empty()) {
        stroke.pressures.push_back(pressure.has_value() ? ClampPressure(*pressure) : stroke.pressures.back());
    }
}

void QtDocumentController::EndStroke() {
    if (!pendingStroke_.has_value() || session_ == nullptr) {
        CancelStroke();
        return;
    }

    PendingStrokeState finished = std::move(*pendingStroke_);
    pendingStroke_.reset();
    Page* page = PageAt(finished.pageIndex);
    if (page == nullptr || finished.stroke.points.empty()) {
        return;
    }

    // Same commit shape as the reference (App::EndStroke): append, push an
    // AddStroke action with its insert index, then the shared commit tail.
    session_->document.TouchPage(finished.pageIndex);
    HistoryAction action {};
    action.type = HistoryActionType::AddStroke;
    action.index = page->strokes.size();
    action.pageIndex = finished.pageIndex;
    action.stroke = std::move(finished.stroke);
    page->strokes.push_back(action.stroke);
    RecordCommit(std::move(action));
}

void QtDocumentController::CancelStroke() {
    pendingStroke_.reset();
}

const Stroke* QtDocumentController::PendingStroke() const {
    return pendingStroke_.has_value() ? &pendingStroke_->stroke : nullptr;
}

std::size_t QtDocumentController::PendingStrokePage() const {
    return pendingStroke_.has_value() ? pendingStroke_->pageIndex : 0U;
}

// --- history ----------------------------------------------------------------

bool QtDocumentController::CanUndo() const {
    return session_ != nullptr && !session_->undoActions.empty();
}

bool QtDocumentController::CanRedo() const {
    return session_ != nullptr && !session_->redoActions.empty();
}

bool QtDocumentController::Undo() {
    // The edit is a transient preview: finish it first (committing captured
    // overlay content when a source exists, cancelling otherwise) so the undo
    // below is a history step and never an empty commit that erased the object.
    AbortTransientState();
    if (session_ == nullptr || session_->undoActions.empty()) {
        return false;
    }

    HistoryAction action = std::move(session_->undoActions.back());
    session_->undoActions.pop_back();
    if (!ApplyHistoryAction(session_->document, action, true)) {
        session_->undoActions.push_back(std::move(action));
        return false;
    }

    session_->redoActions.push_back(std::move(action));
    NoteCommitted();
    return true;
}

bool QtDocumentController::Redo() {
    AbortTransientState();
    if (session_ == nullptr || session_->redoActions.empty()) {
        return false;
    }

    HistoryAction action = std::move(session_->redoActions.back());
    session_->redoActions.pop_back();
    if (!ApplyHistoryAction(session_->document, action, false)) {
        session_->redoActions.push_back(std::move(action));
        return false;
    }

    session_->undoActions.push_back(std::move(action));
    NoteCommitted();
    return true;
}

// --- object creation and text editing ---------------------------------------

std::size_t QtDocumentController::AddTextObject(std::size_t pageIndex, Vec2 topLeft, float fontSize, float width) {
    AbortTransientState();
    Page* page = PageAt(pageIndex);
    if (page == nullptr) {
        return 0U;
    }

    PageText text;
    text.id = NextObjectId();
    text.x = topLeft.x;
    text.y = topLeft.y;
    text.width = width > 0.0f ? std::max(kMinTextWidth, width) : 0.0f;
    text.fontSize = fontSize;
    text.color = input_.objectColor;
    // Pending style from the text-format popup (CanvasInputSettings): every
    // attribute the popup exposes seeds the placeholder so the editor opens
    // already formatted.
    text.fontFamily = input_.textFontFamily;
    text.alignment = input_.textAlignment;
    text.bold = input_.textBold;
    text.italic = input_.textItalic;
    text.underline = input_.textUnderline;
    // The placeholder enters the page now (excluded from cached surfaces while
    // it is under edit); bump the epoch so a failed BeginTextEdit still leaves
    // consistent caches.
    session_->document.TouchPage(pageIndex);
    page->texts.push_back(text);
    const std::size_t index = page->texts.size() - 1U;
    if (BeginTextEdit(pageIndex, index)) {
        edit_->creates = true;
    }
    return index;
}

std::size_t QtDocumentController::AddLatexObject(std::size_t pageIndex, Vec2 topLeft, float fontSize) {
    AbortTransientState();
    Page* page = PageAt(pageIndex);
    if (page == nullptr) {
        return 0U;
    }

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
        edit_->creates = true;
    }
    return index;
}

bool QtDocumentController::BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) {
    AbortTransientState();
    Page* page = PageAt(pageIndex);
    if (page == nullptr || textIndex >= page->texts.size()) {
        return false;
    }

    EditSession edit;
    edit.kind = EditSession::Kind::Text;
    edit.pageIndex = pageIndex;
    edit.objectId = page->texts[textIndex].id;
    edit.textBefore = page->texts[textIndex];
    edit.text = edit.textBefore;
    edit_ = std::move(edit);
    emit EditStateChanged();
    return true;
}

const PageText* QtDocumentController::EditingTextObject() const {
    if (!EditingText()) {
        return nullptr;
    }
    return &edit_->text;
}

void QtDocumentController::FitEditingText(const PageText& edited, float maxAutoWidth) {
    if (!EditingText()) {
        return;
    }
    PageText next = edit_->text;
    next.content = edited.content;
    next.styleSpans = edited.styleSpans;
    edit_->text = text::FitTextBox(edit_->text, std::move(next), maxAutoWidth);
}

void QtDocumentController::SetEditingTextAlignment(TextAlignment alignment) {
    if (EditingText()) {
        edit_->text.alignment = alignment;
    }
}

bool QtDocumentController::BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) {
    AbortTransientState();
    Page* page = PageAt(pageIndex);
    if (page == nullptr || latexIndex >= page->latexes.size()) {
        return false;
    }

    EditSession edit;
    edit.kind = EditSession::Kind::LaTeX;
    edit.pageIndex = pageIndex;
    edit.objectId = page->latexes[latexIndex].id;
    edit.latexBefore = page->latexes[latexIndex];
    edit_ = std::move(edit);
    emit EditStateChanged();
    return true;
}

const PageLatex* QtDocumentController::EditingLatexObject() const {
    if (!EditingLatex()) {
        return nullptr;
    }
    return &edit_->latexBefore;
}

std::size_t QtDocumentController::EditingPageIndex() const {
    return edit_.has_value() ? edit_->pageIndex : currentPage_;
}

std::optional<std::size_t> QtDocumentController::EditingTextIndex() const {
    if (!EditingText()) {
        return std::nullopt;
    }
    const Page* page = PageAt(edit_->pageIndex);
    if (page == nullptr) {
        return std::nullopt;
    }
    return FindTextIndexById(*page, edit_->objectId);
}

std::optional<std::size_t> QtDocumentController::EditingLatexIndex() const {
    if (!EditingLatex()) {
        return std::nullopt;
    }
    const Page* page = PageAt(edit_->pageIndex);
    if (page == nullptr) {
        return std::nullopt;
    }
    return FindLatexIndexById(*page, edit_->objectId);
}

void QtDocumentController::CommitTextEdit(PageText edited) {
    if (!EditingText() || session_ == nullptr) {
        CancelTextEdit();
        return;
    }

    const EditSession session = std::move(*edit_);
    edit_.reset();

    Page* page = PageAt(session.pageIndex);
    const std::optional<std::size_t> located =
        page != nullptr ? FindTextIndexById(*page, session.objectId) : std::nullopt;
    if (page == nullptr || !located.has_value()) {
        emit EditStateChanged();
        return;
    }
    const std::size_t textIndex = *located;

    // The session owns the box (FitEditingText grew it while it was typed);
    // the edit brings content and spans, and the box is fitted to them once
    // more so content that changed since the last fit keeps the anchor.
    PageText& stored = page->texts[textIndex];
    if (edited.id == 0U) {
        edited.id = stored.id;
    }
    edited.x = session.text.x;
    edited.y = session.text.y;
    edited.width = session.text.width;

    HistoryAction action {};
    bool changed = false;
    if (edited.content.empty()) {
        session_->document.TouchPage(session.pageIndex);
        page->texts.erase(page->texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
        if (!session.creates) {
            action.type = HistoryActionType::RemoveText;
            action.pageIndex = session.pageIndex;
            action.index = textIndex;
            action.text = session.textBefore;
            changed = true;
        }
    } else {
        edited = text::FitTextBox(session.text, text::NormalizeCommittedText(edited), 0.0f);
        session_->document.TouchPage(session.pageIndex);
        page->texts[textIndex] = edited;
        if (session.creates) {
            action.type = HistoryActionType::AddText;
            action.pageIndex = session.pageIndex;
            action.index = textIndex;
            action.text = edited;
            changed = true;
        } else if (!PageTextEqual(session.textBefore, edited)) {
            action.type = HistoryActionType::UpdateText;
            action.pageIndex = session.pageIndex;
            action.index = textIndex;
            action.text = session.textBefore;
            action.replacementText = edited;
            changed = true;
        }
    }

    if (changed) {
        RecordCommit(std::move(action));
    }
    emit EditStateChanged();
}

void QtDocumentController::CancelTextEdit() {
    if (!EditingText()) {
        return;
    }
    const EditSession session = std::move(*edit_);
    edit_.reset();
    // A cancelled creation removes its placeholder again (the reference erases
    // an object created by the cancelled edit); nothing was recorded in history.
    if (session.creates && session_ != nullptr) {
        Page* page = PageAt(session.pageIndex);
        const std::optional<std::size_t> located =
            page != nullptr ? FindTextIndexById(*page, session.objectId) : std::nullopt;
        if (located.has_value()) {
            session_->document.TouchPage(session.pageIndex);
            page->texts.erase(page->texts.begin() + static_cast<std::ptrdiff_t>(*located));
        }
    }
    emit EditStateChanged();
}

void QtDocumentController::CommitLatexEdit(const QString& source) {
    if (!EditingLatex() || session_ == nullptr) {
        CancelLatexEdit();
        return;
    }

    const EditSession session = std::move(*edit_);
    edit_.reset();

    Page* page = PageAt(session.pageIndex);
    const std::optional<std::size_t> located =
        page != nullptr ? FindLatexIndexById(*page, session.objectId) : std::nullopt;
    if (page == nullptr || !located.has_value()) {
        emit EditStateChanged();
        return;
    }
    const std::size_t latexIndex = *located;

    PageLatex edited = page->latexes[latexIndex];
    edited.id = session.latexBefore.id;
    edited.content = source.toStdU16String();

    HistoryAction action {};
    bool changed = false;
    if (edited.content.empty()) {
        session_->document.TouchPage(session.pageIndex);
        page->latexes.erase(page->latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
        if (!session.creates) {
            action.type = HistoryActionType::RemoveLatex;
            action.pageIndex = session.pageIndex;
            action.index = latexIndex;
            action.latex = session.latexBefore;
            changed = true;
        }
    } else {
        session_->document.TouchPage(session.pageIndex);
        page->latexes[latexIndex] = edited;
        if (session.creates) {
            action.type = HistoryActionType::AddLatex;
            action.pageIndex = session.pageIndex;
            action.index = latexIndex;
            action.latex = edited;
            changed = true;
        } else if (!PageLatexEqual(session.latexBefore, edited)) {
            action.type = HistoryActionType::UpdateLatex;
            action.pageIndex = session.pageIndex;
            action.index = latexIndex;
            action.latex = session.latexBefore;
            action.replacementLatex = edited;
            changed = true;
        }
    }

    if (changed) {
        RecordCommit(std::move(action));
    }
    emit EditStateChanged();
}

void QtDocumentController::CancelLatexEdit() {
    if (!EditingLatex()) {
        return;
    }
    const EditSession session = std::move(*edit_);
    edit_.reset();
    if (session.creates && session_ != nullptr) {
        Page* page = PageAt(session.pageIndex);
        const std::optional<std::size_t> located =
            page != nullptr ? FindLatexIndexById(*page, session.objectId) : std::nullopt;
        if (located.has_value()) {
            session_->document.TouchPage(session.pageIndex);
            page->latexes.erase(page->latexes.begin() + static_cast<std::ptrdiff_t>(*located));
        }
    }
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
    PageGesture gesture;
    gesture.pageIndex = pageIndex;
    gesture.before = *page;
    gesture_ = std::move(gesture);
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
        static_cast<int>(std::lround(page->rotationDegrees / 90.0f)) & 3,
    };
    if (const auto cached = pdfTextCache_.find(key); cached != pdfTextCache_.end()) {
        return &cached->second;
    }
    const std::optional<std::vector<PdfTextCharBox>> extracted = pdfBackend_->ExtractTextCharBoxes(
        pdfSource->pdfData, static_cast<std::size_t>(page->sourcePdfPageIndex));
    if (!extracted.has_value()) {
        return nullptr;
    }
    std::vector<PdfTextCharBox> boxes = *extracted;
    if (key.quarterTurns != 0) {
        // Boxes come back in source-page space; the page already swapped its
        // frame, so the source dims are the swapped-back page dims.
        const float sourceWidth = (key.quarterTurns % 2 != 0) ? page->height : page->width;
        const float sourceHeight = (key.quarterTurns % 2 != 0) ? page->width : page->height;
        for (PdfTextCharBox& box : boxes) {
            const Vec2 tl = RotatePointQuarterTurn({ box.left, box.top }, sourceWidth, sourceHeight, key.quarterTurns);
            const Vec2 br = RotatePointQuarterTurn({ box.right, box.bottom }, sourceWidth, sourceHeight, key.quarterTurns);
            box.left = std::min(tl.x, br.x);
            box.right = std::max(tl.x, br.x);
            box.top = std::min(tl.y, br.y);
            box.bottom = std::max(tl.y, br.y);
        }
    }
    // Extraction reads the whole PDF text page (pdfium LoadMemDocument + text
    // page walk), so hit-testing without this cache cost ~10 ms per probe.
    if (pdfTextCache_.size() >= kPdfTextCacheCapacity) {
        pdfTextCache_.clear();
    }
    const auto inserted = pdfTextCache_.emplace(key, std::move(boxes));
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
    // are emitted in document order.  PDFium marks line ends with real \r/\n
    // chars; they normalize to a single '\n'.  Where a PDF supplies no line
    // markers at all, the geometric check stays as the fallback: a line break
    // is inserted when the next box sits well below the previous one or wraps
    // back to the left.
    const auto isLineBreak = [](std::uint32_t unicode) {
        return unicode == static_cast<std::uint32_t>(L'\r') || unicode == static_cast<std::uint32_t>(L'\n');
    };
    std::u16string text;
    const PdfTextCharBox* previousBox = nullptr;
    for (const auto& [pageIndex, selection] : selection_) {
        for (const PdfTextCharBox& box : selection.pdfText) {
            if (isLineBreak(box.unicode)) {
                // A \r\n pair collapses to one break.
                if (previousBox == nullptr || !isLineBreak(previousBox->unicode)) {
                    text.push_back(L'\n');
                }
            } else {
                // Whitespace carries a word gap at a degenerate box and is no
                // layout anchor: a zero-width space left of the previous
                // glyph would trip the wrap-back check as a phantom break.
                const bool whitespace = box.unicode == static_cast<std::uint32_t>(L' ')
                    || box.unicode == static_cast<std::uint32_t>(L'\t');
                if (previousBox != nullptr && !whitespace && !isLineBreak(previousBox->unicode)) {
                    const float previousHeight = previousBox->bottom - previousBox->top;
                    const float currentHeight = box.bottom - box.top;
                    const float lineBreakThreshold = std::max(previousHeight, currentHeight) * 0.5f;
                    if (box.top > previousBox->bottom + lineBreakThreshold
                        || box.left + (box.right - box.left) * 0.5f < previousBox->left) {
                        text.push_back(L'\n');
                    }
                }
                AppendPdfUnicode(text, box.unicode);
            }
            previousBox = &box;
        }
    }
    return QString::fromStdU16String(text);
}


Page* QtDocumentController::GesturePage() {
    return gesture_.has_value() ? PageAt(gesture_->pageIndex) : nullptr;
}

void QtDocumentController::CommitPageGesture() {
    if (!gesture_.has_value()) {
        return;
    }
    const std::size_t pageIndex = gesture_->pageIndex;
    Page before = std::move(gesture_->before);
    const bool changed = gesture_->changed;
    gesture_.reset();

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
    RecordCommit(std::move(action));
}

void QtDocumentController::CancelPageGesture() {
    if (!gesture_.has_value()) {
        return;
    }
    const std::size_t pageIndex = gesture_->pageIndex;
    Page before = std::move(gesture_->before);
    gesture_.reset();
    if (Page* page = PageAt(pageIndex); page != nullptr) {
        *page = std::move(before);
    }
}

void QtDocumentController::MoveSelectionBy(std::size_t pageIndex, Vec2 delta) {
    Page* page = GesturePage();
    if (page == nullptr || (delta.x == 0.0f && delta.y == 0.0f)) {
        return;
    }
    PageGesture& gesture = *gesture_;
    PageGesture::MoveBounds& moveBounds = gesture.moveBounds;
    const ui::PageSelection& selection = SelectionFor(pageIndex);

    // First move of the gesture measures every selected object's bounds once;
    // later samples reuse them and translate by the applied delta.  Without
    // this each pointer event re-scans every selected stroke's points and
    // re-runs text/LaTeX measurement just to clamp the delta.
    if (!moveBounds.built) {
        moveBounds.strokes.clear();
        moveBounds.images.clear();
        moveBounds.texts.clear();
        moveBounds.latexes.clear();
        for (const std::size_t index : selection.strokes) {
            ui::SelectionBounds bounds {};
            if (index < page->strokes.size()) {
                bounds.valid = ui::StrokeBounds(
                    page->strokes[index], &bounds.left, &bounds.top, &bounds.right, &bounds.bottom);
            }
            moveBounds.strokes.push_back(bounds);
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
            moveBounds.images.push_back(bounds);
        }
        for (const std::size_t index : selection.texts) {
            ui::SelectionBounds bounds {};
            if (index < page->texts.size()) {
                const PageText& text = page->texts[index];
                const text::QtTextBounds textBounds = text::MeasurePageTextBoundsQt(text, page->width);
                for (const Vec2& corner : ui::RotatedRectCorners(
                        textBounds.left, textBounds.top, textBounds.right, textBounds.bottom,
                        text.rotationDegrees)) {
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
            moveBounds.texts.push_back(bounds);
        }
        for (const std::size_t index : selection.latexes) {
            ui::SelectionBounds bounds {};
            if (index < page->latexes.size()) {
                const PageLatex& latex = page->latexes[index];
                const ui::LatexBounds latexBounds = ui::MeasurePageLatexBounds(latex, page->width);
                for (const Vec2& corner : ui::RotatedRectCorners(
                        latexBounds.left, latexBounds.top, latexBounds.right, latexBounds.bottom,
                        latex.rotationDegrees)) {
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
            moveBounds.latexes.push_back(bounds);
        }
        moveBounds.built = true;
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
        if (index >= page->strokes.size() || slot >= moveBounds.strokes.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds.strokes, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        ui::TranslateStrokePoints(page->strokes[index], clamped);
        advance(moveBounds.strokes[slot], clamped);
        MarkGestureChanged();
    }
    for (std::size_t slot = 0; slot < selection.images.size(); ++slot) {
        const std::size_t index = selection.images[slot];
        if (index >= page->images.size() || slot >= moveBounds.images.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds.images, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        page->images[index].x += clamped.x;
        page->images[index].y += clamped.y;
        advance(moveBounds.images[slot], clamped);
        MarkGestureChanged();
    }
    for (std::size_t slot = 0; slot < selection.texts.size(); ++slot) {
        const std::size_t index = selection.texts[slot];
        if (index >= page->texts.size() || slot >= moveBounds.texts.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds.texts, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        page->texts[index].x += clamped.x;
        page->texts[index].y += clamped.y;
        advance(moveBounds.texts[slot], clamped);
        MarkGestureChanged();
    }
    for (std::size_t slot = 0; slot < selection.latexes.size(); ++slot) {
        const std::size_t index = selection.latexes[slot];
        if (index >= page->latexes.size() || slot >= moveBounds.latexes.size()) {
            continue;
        }
        const Vec2 clamped = clampedDelta(slot, moveBounds.latexes, delta);
        if (clamped.x == 0.0f && clamped.y == 0.0f) {
            continue;
        }
        page->latexes[index].x += clamped.x;
        page->latexes[index].y += clamped.y;
        advance(moveBounds.latexes[slot], clamped);
        MarkGestureChanged();
    }
}

bool QtDocumentController::CloneSelection(std::size_t pageIndex) {
    Page* page = GesturePage();
    if (page == nullptr) {
        return false;
    }
    ui::PageSelection clones {};
    const ui::PageSelection& selection = SelectionFor(pageIndex);
    // Copy first, then append: a push_back of the source element straight out
    // of the same vector can invalidate the argument on reallocation.
    for (const std::size_t index : selection.strokes) {
        if (index >= page->strokes.size()) {
            continue;
        }
        Stroke clone = page->strokes[index];
        clones.strokes.push_back(page->strokes.size());
        page->strokes.push_back(std::move(clone));
    }
    for (const std::size_t index : selection.images) {
        if (index >= page->images.size()) {
            continue;
        }
        PageImage clone = page->images[index];
        clones.images.push_back(page->images.size());
        page->images.push_back(std::move(clone));
    }
    for (const std::size_t index : selection.texts) {
        if (index >= page->texts.size()) {
            continue;
        }
        PageText clone = page->texts[index];
        clone.id = NextObjectId();
        clones.texts.push_back(page->texts.size());
        page->texts.push_back(std::move(clone));
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page->latexes.size()) {
            continue;
        }
        PageLatex clone = page->latexes[index];
        clone.id = NextObjectId();
        clones.latexes.push_back(page->latexes.size());
        page->latexes.push_back(std::move(clone));
    }
    if (clones.Empty()) {
        return false;
    }
    selection_[pageIndex] = std::move(clones);
    // The clone is a change even if the pointer never moves afterwards, so
    // CommitPageGesture must not compare-and-skip it.
    MarkGestureChanged();
    return true;
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
        if (index < page->strokes.size() && index < gesture_->before.strokes.size()) {
            page->strokes[index] = gesture_->before.strokes[index];
        }
    }
    for (const std::size_t index : selection.images) {
        if (index < page->images.size() && index < gesture_->before.images.size()) {
            page->images[index] = gesture_->before.images[index];
        }
    }
    for (const std::size_t index : selection.texts) {
        if (index < page->texts.size() && index < gesture_->before.texts.size()) {
            page->texts[index] = gesture_->before.texts[index];
        }
    }
    for (const std::size_t index : selection.latexes) {
        if (index < page->latexes.size() && index < gesture_->before.latexes.size()) {
            page->latexes[index] = gesture_->before.latexes[index];
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
        Stroke& stroke = page->strokes[index];
        for (Vec2& point : stroke.points) {
            point = ui::MapSelectionTransformPoint(point, transform.from, transform.to, transform.rotateDegrees);
        }
        // The stabilization replays over the mapped raw points, so its spacing
        // scales with them (exact for uniform scales and rotations; the
        // geometric mean approximates a non-uniform resize).
        stroke.sampling.spacing *= std::sqrt(std::abs(scaleX * scaleY));
        MarkGestureChanged();
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
        MarkGestureChanged();
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
            // Already-rotated text instead tracks its bounds centre - the
            // reference had no rotated text, and a centre-anchor keeps the
            // accumulated object rotation meaningful.
            if (std::abs(text.rotationDegrees) > 0.0001f) {
                const Vec2 boundsCenter {
                    (bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f };
                const Vec2 mappedCenter = ui::MapSelectionTransformPoint(
                    boundsCenter, transform.from, transform.to, transform.rotateDegrees);
                text.x += mappedCenter.x - boundsCenter.x;
                text.y += mappedCenter.y - boundsCenter.y;
            } else {
                const Vec2 mapped = ui::MapSelectionTransformPoint(
                    { bounds.left, bounds.top }, transform.from, transform.to, transform.rotateDegrees);
                text.x += mapped.x - bounds.left;
                text.y += mapped.y - bounds.top;
            }
            text.rotationDegrees += transform.rotateDegrees;
            if (transform.scaleTextFont) {
                const float textScale = std::max(0.05f,
                    (transform.to.right - transform.to.left) / std::max(1.0f, transform.from.right - transform.from.left));
                text.fontSize = std::max(1.0f, text.fontSize * textScale);
                for (PageTextStyleSpan& span : text.styleSpans) {
                    span.style.fontSize = std::max(1.0f, span.style.fontSize * textScale);
                }
                if (text.width > 0.0f) {
                    text.width = std::max(kMinTextWidth, text.width * textScale);
                }
            }
            MarkGestureChanged();
            continue;
        }

        // Box resize (Excalidraw's e/w text handles): the box's left edge
        // follows the mapped bounds and a horizontal drag sets the width the
        // text wraps at (an auto-width text starts wrapping).  The height is
        // never set: it is always the wrapped content's, so a vertical drag
        // only carries the text along with the rest of the selection.
        const Vec2 mappedTopLeft = ui::MapSelectionTransformPoint(
            { bounds.left, bounds.top }, transform.from, transform.to, 0.0f);
        const Vec2 mappedBottomRight = ui::MapSelectionTransformPoint(
            { bounds.right, bounds.bottom }, transform.from, transform.to, 0.0f);
        const bool widthChanged = transform.to.left != transform.from.left
            || transform.to.right != transform.from.right;
        text.x += mappedTopLeft.x - bounds.left;
        text.y += mappedTopLeft.y - bounds.top;
        if (widthChanged) {
            text.width = std::max(kMinTextWidth, mappedBottomRight.x - mappedTopLeft.x);
        }
        MarkGestureChanged();
    }
    for (const std::size_t index : selection.latexes) {
        if (index >= page->latexes.size()) {
            continue;
        }
        PageLatex& object = page->latexes[index];
        const ui::LatexBounds bounds = ui::MeasurePageLatexBounds(object, page->width);
        // Same centre-anchor rule as text: rotated latex tracks its bounds
        // centre so the accumulated rotation stays meaningful.
        if (std::abs(object.rotationDegrees) > 0.0001f || transform.rotateDegrees != 0.0f) {
            const Vec2 boundsCenter {
                (bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f };
            const Vec2 mappedCenter = ui::MapSelectionTransformPoint(
                boundsCenter, transform.from, transform.to, transform.rotateDegrees);
            object.x += mappedCenter.x - boundsCenter.x;
            object.y += mappedCenter.y - boundsCenter.y;
            object.rotationDegrees += transform.rotateDegrees;
        } else {
            const Vec2 mapped = ui::MapSelectionTransformPoint(
                { bounds.left, bounds.top }, transform.from, transform.to, transform.rotateDegrees);
            object.x += mapped.x - bounds.left;
            object.y += mapped.y - bounds.top;
        }
        object.fontSize *= scaleY;
        MarkGestureChanged();
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

    MarkGestureChanged();
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
                MarkGestureChanged();
            }
        }
        for (const std::size_t index : selection.texts) {
            if (index >= page->texts.size()) {
                continue;
            }
            PageText& text = page->texts[index];
            if (text.color != color) {
                text.color = color;
                MarkGestureChanged();
            }
            for (PageTextStyleSpan& span : text.styleSpans) {
                if (span.style.color != color) {
                    span.style.color = color;
                    MarkGestureChanged();
                }
            }
        }
        for (const std::size_t index : selection.latexes) {
            if (index < page->latexes.size() && page->latexes[index].color != color) {
                page->latexes[index].color = color;
                MarkGestureChanged();
            }
        }
        recolored = recolored || (gesture_.has_value() && gesture_->changed);
        CommitPageGesture();
    }
    return recolored;
}



void QtDocumentController::SetPendingStrokeDotted(bool dotted) {
    if (pendingStroke_.has_value()) {
        pendingStroke_->stroke.style.dotted = dotted;
    }
}

void QtDocumentController::ReplacePendingStrokePoints(const std::vector<Vec2>& points, bool dotted) {
    if (!pendingStroke_.has_value()) {
        return;
    }
    pendingStroke_->stroke.points = points;
    pendingStroke_->stroke.sampling = {};
    pendingStroke_->stroke.pressures.clear();
    pendingStroke_->stroke.style.dotted = dotted;
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
    RecordCommit(std::move(action));
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
    emit SelectionChanged();
    RecordCommit(std::move(action));
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
    RecordCommit(std::move(action));
    return true;
}

bool QtDocumentController::RotatePage(std::size_t pageIndex, int quarterTurns) {
    const int turns = ((quarterTurns % 4) + 4) % 4;
    if (session_ == nullptr || turns == 0 || pageIndex >= session_->document.PageCount()) {
        return false;
    }
    Document& document = session_->document;
    Page& page = document.PageAt(pageIndex);
    const Page before = page;

    // Text and LaTeX boxes come from layout (no stored height), so each one's
    // measured bounds centre follows the page and the box - whose size a turn
    // does not change, since it rotates about that centre - is re-centred on
    // it after RotatePageContents accumulates the rotation.
    const float width = page.width;
    const float height = page.height;
    struct Box {
        Vec2 center;
        float width = 0.0f;
        float height = 0.0f;
    };
    const auto boxOf = [](float left, float top, float right, float bottom) {
        return Box { { (left + right) * 0.5f, (top + bottom) * 0.5f }, right - left, bottom - top };
    };
    std::vector<Box> textBoxes;
    textBoxes.reserve(page.texts.size());
    for (const PageText& text : page.texts) {
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, width);
        textBoxes.push_back(boxOf(bounds.left, bounds.top, bounds.right, bounds.bottom));
    }
    std::vector<Box> latexBoxes;
    latexBoxes.reserve(page.latexes.size());
    for (const PageLatex& latex : page.latexes) {
        const ui::LatexBounds bounds = ui::MeasurePageLatexBounds(latex, width);
        latexBoxes.push_back(boxOf(bounds.left, bounds.top, bounds.right, bounds.bottom));
    }

    RotatePageContents(page, turns);

    const auto recentre = [&](const Box& box, float& x, float& y) {
        const Vec2 mapped = RotatePointQuarterTurn(box.center, width, height, turns);
        x = mapped.x - box.width * 0.5f;
        y = mapped.y - box.height * 0.5f;
    };
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        recentre(textBoxes[index], page.texts[index].x, page.texts[index].y);
    }
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        recentre(latexBoxes[index], page.latexes[index].x, page.latexes[index].y);
    }

    document.TouchPage(pageIndex);
    pdfTextCache_.clear();
    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pageIndices = { pageIndex };
    action.pages = { std::move(before) };
    action.replacementPages = { page };
    RecordCommit(std::move(action));
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
    RecordCommit(std::move(action));
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
    pageDefaults.sizeOverridden = true;
    const Document::PageDefaults& stored = document.NewPageDefaults();
    const bool same = stored.width == pageDefaults.width && stored.height == pageDefaults.height
        && stored.backgroundColor == pageDefaults.backgroundColor
        && stored.paletteProfile == pageDefaults.paletteProfile
        && stored.sizeOverridden;
    if (same) {
        return;
    }
    document.SetNewPageDefaults(pageDefaults);
    NoteCommitted();
}

ui::NewPageDefaults QtDocumentController::NewPageDefaults() const {
    const Document& document = DocumentRef();
    const Document::PageDefaults& stored = document.NewPageDefaults();
    ui::NewPageDefaults defaults {};
    defaults.width = stored.sizeOverridden ? stored.width : defaultPageWidth_;
    defaults.height = stored.sizeOverridden ? stored.height : defaultPageHeight_;
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
            const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(text, page.width);
            growPoint(bounds.left, bounds.top);
            growPoint(bounds.right, bounds.bottom);
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
    selection_.clear();
    emit SelectionChanged();
    RecordCommit(std::move(action));
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

    selection_.clear();
    selection_[pageIndex] = std::move(pasted);
    emit SelectionChanged();
    RecordCommit(std::move(action));
    return true;
}

}  // namespace cnote::qt
