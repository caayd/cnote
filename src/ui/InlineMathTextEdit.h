#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QTextEdit>

#include "cnote_ui_export.h"
#include "Document.h"
#include "text/TextEditingQt.h"

class QContextMenuEvent;
class QKeyEvent;
class QMimeData;

namespace cnote::ui {

// The text tool's editor.  `$...$` formulas render inline exactly like the
// committed object (the same placeholders, text::ApplyPageTextFormatting),
// except the formula the caret touches, which shows as editable source.
//
// Rendering swaps characters in the document, so the document's own undo
// stack is disabled and this widget keeps a source-level history instead:
// Ctrl+Z / Ctrl+Y and the context menu undo what the user typed, never a
// formula expanding or collapsing.  Typing coalesces like QTextEdit: one step
// per contiguous run, broken at word boundaries and pauses.  Copy and drag
// export the `$...$` source, never placeholder characters.
//
// The document is not re-read for every signal: contentsChange locates the
// edit, the model content is spliced, and the offset map/formatting spans are
// updated for the affected range only.  Caret moves reuse the cached map.
class CNOTE_UI_EXPORT InlineMathTextEdit final : public QTextEdit {
    Q_OBJECT

public:
    explicit InlineMathTextEdit(QWidget* parent = nullptr);

    // Loads `text` (content, spans and default style) formatted at
    // displayScale, caret at the end, and starts a fresh history.
    void LoadPageText(const PageText& text, float displayScale);
    // Re-formats the current content at a new display scale, keeping the
    // caret, selection and history.
    void SetDisplayScale(float displayScale);
    // The loaded object with the edited content and style spans.
    [[nodiscard]] PageText CapturePageText() const;

    bool UndoEdit();
    bool RedoEdit();
    [[nodiscard]] bool CanUndoEdit() const { return !undo_.empty(); }
    [[nodiscard]] bool CanRedoEdit() const { return !redo_.empty(); }

    // Diagnostics for the audit: whole-document work only.  Caret moves and
    // keystrokes must leave these unchanged; loads, scale changes, undo/redo
    // and the defensive resync are allowed to bump them.
    [[nodiscard]] std::uint64_t FullDocumentReadCount() const { return fullDocumentReads_; }
    [[nodiscard]] std::uint64_t FullFormattingCaptureCount() const { return fullFormattingCaptures_; }
    [[nodiscard]] std::uint64_t FullFormulaScanCount() const { return fullFormulaScans_; }

signals:
    // An actual user content/style edit finished (never an internal formula
    // placeholder swap).  The canvas refits the edited box once per edit.
    void ContentEdited();
    // A formula expanded or collapsed around the caret, changing the
    // document's layout but not the model content.  The canvas repositions
    // (and resizes) the overlay without refitting the box.
    void FormulaLayoutChanged();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    [[nodiscard]] QMimeData* createMimeDataFromSelection() const override;

private:
    // One undo step as a text delta plus the span/caret states on both sides.
    // The document itself is rebuilt from the current content when applied.
    struct EditStep {
        std::uint32_t start = 0;
        std::u16string removed;
        std::u16string inserted;
        std::vector<PageTextStyleSpan> spansBefore;
        std::vector<PageTextStyleSpan> spansAfter;
        std::uint32_t anchorBefore = 0;
        std::uint32_t cursorBefore = 0;
        std::uint32_t anchorAfter = 0;
        std::uint32_t cursorAfter = 0;
    };
    struct PendingChange {
        int position = 0;
        int removed = 0;
        int added = 0;
    };
    enum class ChangeKind {
        Insert,
        Delete,
        Other,
    };

    void OnContentsChange(int position, int charsRemoved, int charsAdded);
    void OnDocumentChanged();
    void OnCursorMoved();
    // Puts every renderable formula except the caret's into placeholder form
    // and refreshes placeholders whose style changed.  Never recorded, and it
    // only touches the formulas whose desired state changed.  Returns whether
    // the document changed.
    bool SyncFormulas();
    // Replaces the document with `text` and places the selection (model
    // offsets).  Never recorded.
    void Rebuild(const PageText& text, std::uint32_t anchor, std::uint32_t cursor);
    // True when a reported change actually altered the document's text;
    // formatting changes arrive with charsRemoved == charsAdded.
    [[nodiscard]] bool ContentChanged(const PendingChange& change) const;
    // Applies one contentsChange delta to the cached content/map/spans.
    void ApplyContentChange(const PendingChange& change);
    // A formatting-only change: the spans are re-captured, content is not.
    void ApplyStyleChange();
    // Defensive whole-document resync when a change cannot be localized.
    void ResyncAll();
    // Recomputes the renderable ranges for the line(s) the edit touched.
    void UpdateRenderableForEdit(const std::u16string& content,
        std::uint32_t start, std::uint32_t removed, std::uint32_t inserted);
    void RecordStep(std::uint32_t start, std::u16string removed, std::u16string inserted,
        std::uint32_t oldAnchor, std::uint32_t oldCursor, ChangeKind kind);
    void SyncCaretFromDocument();
    [[nodiscard]] PageText CurrentText() const;

    PageText base_ {};
    float scale_ = 1.0f;
    int internalEdit_ = 0;

    // Cached model state: content is the expanded `$...$` source; map_ maps
    // it to the document's current placeholder/source form.
    std::u16string content_;
    std::vector<PageTextStyleSpan> spans_;
    text::TextOffsetMap map_ {};
    // Every formula in content_ whose source parses, sorted; the rendered
    // subset is map_.math.
    std::vector<text::InlineMathRange> renderable_;
    std::uint32_t anchor_ = 0;
    std::uint32_t cursor_ = 0;
    int documentLength_ = 0;
    std::uint64_t engineGeneration_ = 0;
    std::optional<PendingChange> pendingChange_;
    // A formatting edit touched the document; rendered placeholders need
    // their char formats re-checked once.
    bool styleDirty_ = false;

    std::vector<EditStep> undo_;
    std::vector<EditStep> redo_;
    ChangeKind lastKind_ = ChangeKind::Other;
    std::uint32_t lastPosition_ = 0;
    QElapsedTimer lastChange_;

    std::uint64_t fullDocumentReads_ = 0;
    std::uint64_t fullFormattingCaptures_ = 0;
    std::uint64_t fullFormulaScans_ = 0;
};

}  // namespace cnote::ui
