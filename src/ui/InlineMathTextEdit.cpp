#include "ui/InlineMathTextEdit.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <QAction>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QTextCursor>
#include <QTextDocument>

#include "text/InlineMathQt.h"
#include "text/TextEditingQt.h"

namespace cnote::ui {

namespace {

// Typing pauses longer than this start a new undo step.
constexpr qint64 kCoalesceWindowMs = 2000;

PageTextStyle DefaultStyle(const PageText& text) {
    return { text.fontSize, text.color, text.fontFamily, text.bold, text.italic, text.underline };
}

bool IsBlank(char16_t character) {
    return character == u' ' || character == u'\t' || character == u'\n';
}

bool SpansEqual(const std::vector<PageTextStyleSpan>& a, const std::vector<PageTextStyleSpan>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const PageTextStyleSpan& left, const PageTextStyleSpan& right) {
        return left.start == right.start && left.length == right.length && PageTextStyleEqual(left.style, right.style);
    });
}

// The format a formula's source characters take: the placeholder's style
// without the object properties (or a source character's, normalized).
QTextCharFormat PlainFormat(const QTextCharFormat& format, const PageTextStyle& fallback, float scale) {
    return text::CharFormatForStyle(text::StyleForCharFormat(format, fallback, scale), scale);
}

}  // namespace

InlineMathTextEdit::InlineMathTextEdit(QWidget* parent)
    : QTextEdit(parent) {
    document()->setUndoRedoEnabled(false);
    connect(document(), &QTextDocument::contentsChanged, this, &InlineMathTextEdit::OnDocumentChanged);
    connect(this, &QTextEdit::cursorPositionChanged, this, &InlineMathTextEdit::OnCursorMoved);
    lastChange_.start();
}

void InlineMathTextEdit::LoadPageText(const PageText& text, float displayScale) {
    base_ = text;
    scale_ = displayScale;
    undo_.clear();
    redo_.clear();
    lastKind_ = ChangeKind::Other;
    // Characters without an explicit colour draw in the palette's text
    // colour, which the app's dark chrome palette makes white: on a white
    // page that is invisible.  Fall back to the object's colour instead.
    QPalette textPalette = palette();
    textPalette.setColor(QPalette::Text, QColor::fromRgba(text.color));
    setPalette(textPalette);
    const PageText normalized = text::NormalizeCommittedText(text);
    const auto end = static_cast<std::uint32_t>(normalized.content.size());
    Rebuild(normalized, end, end);
    current_ = CurrentSnapshot();
}

void InlineMathTextEdit::SetDisplayScale(float displayScale) {
    if (displayScale == scale_) {
        return;
    }
    const Snapshot snapshot = CurrentSnapshot();
    scale_ = displayScale;
    Rebuild(snapshot.text, snapshot.anchor, snapshot.cursor);
    current_ = CurrentSnapshot();
}

PageText InlineMathTextEdit::CapturePageText() const {
    return CurrentSnapshot().text;
}

bool InlineMathTextEdit::UndoEdit() {
    if (undo_.empty()) {
        return false;
    }
    const Snapshot target = undo_.back();
    undo_.pop_back();
    redo_.push_back(CurrentSnapshot());
    lastKind_ = ChangeKind::Other;
    Rebuild(target.text, target.anchor, target.cursor);
    current_ = CurrentSnapshot();
    return true;
}

bool InlineMathTextEdit::RedoEdit() {
    if (redo_.empty()) {
        return false;
    }
    const Snapshot target = redo_.back();
    redo_.pop_back();
    undo_.push_back(CurrentSnapshot());
    lastKind_ = ChangeKind::Other;
    Rebuild(target.text, target.anchor, target.cursor);
    current_ = CurrentSnapshot();
    return true;
}

void InlineMathTextEdit::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Undo)) {
        UndoEdit();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        RedoEdit();
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

void InlineMathTextEdit::contextMenuEvent(QContextMenuEvent* event) {
    QMenu* menu = createStandardContextMenu(event->pos());
    // The standard Undo/Redo entries drive the (disabled) document stack;
    // point them at the source-level history.
    for (QAction* action : menu->actions()) {
        if (action->objectName() == QStringLiteral("edit-undo")) {
            disconnect(action, &QAction::triggered, nullptr, nullptr);
            connect(action, &QAction::triggered, this, &InlineMathTextEdit::UndoEdit);
            action->setEnabled(CanUndoEdit());
        } else if (action->objectName() == QStringLiteral("edit-redo")) {
            disconnect(action, &QAction::triggered, nullptr, nullptr);
            connect(action, &QAction::triggered, this, &InlineMathTextEdit::RedoEdit);
            action->setEnabled(CanRedoEdit());
        }
    }
    menu->exec(event->globalPos());
    delete menu;
}

QMimeData* InlineMathTextEdit::createMimeDataFromSelection() const {
    const QTextCursor cursor = textCursor();
    const text::DocumentContent read = text::ReadDocumentContent(*document());
    const std::uint32_t start = text::ModelOffsetForDocument(read.map, static_cast<std::uint32_t>(cursor.selectionStart()));
    const std::uint32_t end = text::ModelOffsetForDocument(read.map, static_cast<std::uint32_t>(cursor.selectionEnd()));
    auto* data = new QMimeData();
    data->setText(QString::fromUtf16(read.content.data() + start, static_cast<qsizetype>(end - start)));
    return data;
}

void InlineMathTextEdit::OnDocumentChanged() {
    if (internalEdit_ > 0) {
        return;
    }
    const Snapshot after = CurrentSnapshot();
    const std::u16string& before = current_.text.content;
    const std::u16string& now = after.text.content;
    if (before == now && SpansEqual(current_.text.styleSpans, after.text.styleSpans)) {
        current_ = after;
        SyncFormulas();
        return;
    }

    std::size_t prefix = 0;
    const std::size_t shorter = std::min(before.size(), now.size());
    while (prefix < shorter && before[prefix] == now[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < shorter - prefix && before[before.size() - 1U - suffix] == now[now.size() - 1U - suffix]) {
        ++suffix;
    }
    const std::size_t removed = before.size() - prefix - suffix;
    const std::size_t inserted = now.size() - prefix - suffix;
    const ChangeKind kind = removed == 0U && inserted > 0U
        ? ChangeKind::Insert
        : (inserted == 0U && removed > 0U ? ChangeKind::Delete : ChangeKind::Other);

    bool coalesce = false;
    if (kind == lastKind_ && lastChange_.elapsed() < kCoalesceWindowMs) {
        if (kind == ChangeKind::Insert) {
            // A blank typed after a word starts a new step, like QTextEdit.
            const bool wordBoundary = IsBlank(now[prefix]) && prefix > 0U && !IsBlank(now[prefix - 1U]);
            coalesce = inserted == 1U && prefix == lastPosition_ && !wordBoundary;
        } else if (kind == ChangeKind::Delete) {
            // Backspace walks left; Delete stays put.
            coalesce = removed == 1U && (prefix + 1U == lastPosition_ || prefix == lastPosition_);
        }
    }
    if (!coalesce) {
        undo_.push_back(current_);
    }
    redo_.clear();
    lastKind_ = kind;
    lastPosition_ = static_cast<std::uint32_t>(kind == ChangeKind::Insert ? prefix + inserted : prefix);
    lastChange_.restart();

    current_ = after;
    SyncFormulas();
    current_ = CurrentSnapshot();
}

void InlineMathTextEdit::OnCursorMoved() {
    if (internalEdit_ > 0) {
        return;
    }
    const Snapshot now = CurrentSnapshot();
    if (now.text.content != current_.text.content || !SpansEqual(now.text.styleSpans, current_.text.styleSpans)) {
        // The edit's cursor signal arrived before its contents signal.
        OnDocumentChanged();
        return;
    }
    current_.anchor = now.anchor;
    current_.cursor = now.cursor;
    SyncFormulas();
}

void InlineMathTextEdit::SyncFormulas() {
    ++internalEdit_;
    QTextDocument& doc = *document();
    // Registered before any placeholder is inserted: Qt sizes an inline
    // object when it is laid out, and one laid out without a handler stays
    // zero-width for line breaking.
    text::RegisterInlineMathHandler(doc);
    const PageTextStyle fallback = DefaultStyle(base_);
    text::DocumentContent read = text::ReadDocumentContent(doc);
    const QTextCursor caret = textCursor();
    const std::uint32_t cursorModel = text::ModelOffsetForDocument(read.map, static_cast<std::uint32_t>(caret.position()));
    const std::uint32_t anchorModel = text::ModelOffsetForDocument(read.map, static_cast<std::uint32_t>(caret.anchor()));

    // Every renderable formula renders, except the one the caret touches.
    std::vector<text::InlineMathRange> desired = text::RenderableInlineMath(read.content, base_.fontSize);
    std::erase_if(desired, [&](const text::InlineMathRange& range) {
        return range.start <= cursorModel && cursorModel <= range.end;
    });
    const auto isDesired = [&](const text::InlineMathRange& range) {
        return std::find(desired.begin(), desired.end(), range) != desired.end();
    };

    bool changed = false;
    // Placeholders that should show source expand back to `$...$`.
    for (auto range = read.map.math.rbegin(); range != read.map.math.rend(); ++range) {
        if (isDesired(*range)) {
            continue;
        }
        const int position = static_cast<int>(text::DocumentOffsetForModel(read.map, range->start));
        QTextCursor edit(&doc);
        edit.setPosition(position);
        edit.setPosition(position + 1, QTextCursor::KeepAnchor);
        edit.insertText(
            QString::fromUtf16(read.content.data() + range->start, static_cast<qsizetype>(range->end - range->start)),
            PlainFormat(edit.charFormat(), fallback, scale_));
        changed = true;
    }
    if (changed) {
        read = text::ReadDocumentContent(doc);
    }

    // Source that should render collapses; placeholders whose style changed
    // (toolbar formatting) are re-made.  Back to front keeps offsets valid.
    const std::vector<text::InlineMathRange>& placeholders = read.map.math;
    for (auto range = desired.rbegin(); range != desired.rend(); ++range) {
        const std::u16string_view source = std::u16string_view(read.content).substr(range->SourceStart(), range->SourceLength());
        const int start = static_cast<int>(text::DocumentOffsetForModel(read.map, range->start));
        QTextCursor edit(&doc);
        if (std::find(placeholders.begin(), placeholders.end(), *range) != placeholders.end()) {
            edit.setPosition(start);
            edit.setPosition(start + 1, QTextCursor::KeepAnchor);
            const QTextCharFormat current = edit.charFormat();
            QTextCharFormat expected = PlainFormat(current, fallback, scale_);
            text::MakeInlineMathFormat(expected, source, scale_);
            if (expected != current) {
                edit.setCharFormat(expected);
                changed = true;
            }
            continue;
        }
        // The opening `$` carries the run's style.
        edit.setPosition(start + 1);
        QTextCharFormat format = PlainFormat(edit.charFormat(), fallback, scale_);
        text::MakeInlineMathFormat(format, source, scale_);
        edit.setPosition(start);
        edit.setPosition(static_cast<int>(text::DocumentOffsetForModel(read.map, range->end)), QTextCursor::KeepAnchor);
        edit.insertText(QString(QChar::ObjectReplacementCharacter), format);
        changed = true;
    }

    if (changed) {
        const text::TextOffsetMap after = text::ReadDocumentContent(doc).map;
        QTextCursor restored(&doc);
        restored.setPosition(static_cast<int>(text::DocumentOffsetForModel(after, anchorModel)));
        restored.setPosition(static_cast<int>(text::DocumentOffsetForModel(after, cursorModel)), QTextCursor::KeepAnchor);
        setTextCursor(restored);
    }
    --internalEdit_;
}

void InlineMathTextEdit::Rebuild(const PageText& text, std::uint32_t anchor, std::uint32_t cursor) {
    ++internalEdit_;
    // `text` is normalized (LF only), so source documents map 1:1 to the model.
    setPlainText(QString::fromUtf16(text.content.data(), static_cast<qsizetype>(text.content.size())));
    text::ApplyPageTextFormatting(*document(), text, scale_);
    const int size = static_cast<int>(text.content.size());
    QTextCursor placed(document());
    placed.setPosition(std::min(static_cast<int>(anchor), size));
    placed.setPosition(std::min(static_cast<int>(cursor), size), QTextCursor::KeepAnchor);
    setTextCursor(placed);
    SyncFormulas();
    --internalEdit_;
}

InlineMathTextEdit::Snapshot InlineMathTextEdit::CurrentSnapshot() const {
    const QTextDocument& doc = *document();
    const text::DocumentContent read = text::ReadDocumentContent(doc);
    Snapshot snapshot;
    snapshot.text = base_;
    snapshot.text.content = read.content;
    snapshot.text.styleSpans = text::CapturePageTextFormatting(doc, snapshot.text, scale_);
    const QTextCursor cursor = textCursor();
    snapshot.anchor = text::ModelOffsetForDocument(read.map, static_cast<std::uint32_t>(cursor.anchor()));
    snapshot.cursor = text::ModelOffsetForDocument(read.map, static_cast<std::uint32_t>(cursor.position()));
    return snapshot;
}

}  // namespace cnote::ui
