#pragma once

#include <cstdint>
#include <vector>

#include <QElapsedTimer>
#include <QTextEdit>

#include "Document.h"

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
class InlineMathTextEdit final : public QTextEdit {
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

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    [[nodiscard]] QMimeData* createMimeDataFromSelection() const override;

private:
    struct Snapshot {
        PageText text {};
        std::uint32_t anchor = 0;
        std::uint32_t cursor = 0;
    };
    enum class ChangeKind {
        Insert,
        Delete,
        Other,
    };

    void OnDocumentChanged();
    void OnCursorMoved();
    // Puts every renderable formula except the caret's into placeholder form
    // and refreshes placeholders whose style changed.  Never recorded.
    void SyncFormulas();
    // Replaces the document with `text` and places the selection (model
    // offsets).  Never recorded.
    void Rebuild(const PageText& text, std::uint32_t anchor, std::uint32_t cursor);
    [[nodiscard]] Snapshot CurrentSnapshot() const;

    PageText base_ {};
    float scale_ = 1.0f;
    int internalEdit_ = 0;

    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    Snapshot current_ {};
    ChangeKind lastKind_ = ChangeKind::Other;
    std::uint32_t lastPosition_ = 0;
    QElapsedTimer lastChange_;
};

}  // namespace cnote::ui
