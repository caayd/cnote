#include "ui/InlineMathTextEdit.h"

#include <algorithm>
#include <cstdint>
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

#include "latex/LatexEngine.h"
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

bool SameRange(const text::InlineMathRange& a, const text::InlineMathRange& b) {
    return a.start == b.start && a.end == b.end;
}

const text::InlineMathRange* FindRange(const std::vector<text::InlineMathRange>& ranges,
    const text::InlineMathRange& wanted) {
    const auto it = std::lower_bound(ranges.begin(), ranges.end(), wanted.start,
        [](const text::InlineMathRange& range, std::uint32_t start) { return range.start < start; });
    return it != ranges.end() && SameRange(*it, wanted) ? &*it : nullptr;
}

void InsertRange(std::vector<text::InlineMathRange>& ranges, const text::InlineMathRange& range) {
    const auto it = std::lower_bound(ranges.begin(), ranges.end(), range.start,
        [](const text::InlineMathRange& existing, std::uint32_t start) { return existing.start < start; });
    ranges.insert(it, range);
}

void AppendSpan(std::vector<PageTextStyleSpan>& spans, PageTextStyleSpan span) {
    if (span.length == 0U) {
        return;
    }
    if (!spans.empty()) {
        PageTextStyleSpan& last = spans.back();
        if (last.start + last.length == span.start && PageTextStyleEqual(last.style, span.style)) {
            last.length += span.length;
            return;
        }
    }
    spans.push_back(span);
}

// Moves the cached spans through a content edit [start, start+removed) that
// inserted `inserted` characters.  Spans are ordered and non-overlapping; a
// span that straddles the edit keeps its left part and its (shifted) right
// part.  The inserted characters' spans are captured separately and merged.
std::vector<PageTextStyleSpan> TransformSpans(const std::vector<PageTextStyleSpan>& spans,
    std::uint32_t start, std::uint32_t removed, std::uint32_t inserted) {
    std::vector<PageTextStyleSpan> result;
    result.reserve(spans.size() + 1U);
    const std::uint32_t oldEnd = start + removed;
    for (const PageTextStyleSpan& span : spans) {
        if (span.length == 0U) {
            continue;
        }
        const std::uint32_t end = span.start + span.length;
        if (end <= start) {
            result.push_back(span);
            continue;
        }
        if (span.start >= oldEnd) {
            result.push_back({ span.start - removed + inserted, span.length, span.style });
            continue;
        }
        if (span.start < start) {
            result.push_back({ span.start, start - span.start, span.style });
        }
        if (end > oldEnd) {
            const std::uint32_t rightStart = std::max(span.start, oldEnd);
            result.push_back({ rightStart - removed + inserted, end - rightStart, span.style });
        }
    }
    return result;
}

}  // namespace

InlineMathTextEdit::InlineMathTextEdit(QWidget* parent)
    : QTextEdit(parent) {
    document()->setUndoRedoEnabled(false);
    connect(document(), &QTextDocument::contentsChange, this, &InlineMathTextEdit::OnContentsChange);
    connect(document(), &QTextDocument::contentsChanged, this, &InlineMathTextEdit::OnDocumentChanged);
    connect(this, &QTextEdit::cursorPositionChanged, this, &InlineMathTextEdit::OnCursorMoved);
    documentLength_ = document()->characterCount();
    lastChange_.start();
}

void InlineMathTextEdit::LoadPageText(const PageText& text, float displayScale) {
    base_ = text;
    scale_ = displayScale;
    undo_.clear();
    redo_.clear();
    pendingChange_.reset();
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
}

void InlineMathTextEdit::SetDisplayScale(float displayScale) {
    if (displayScale == scale_) {
        return;
    }
    const PageText text = CurrentText();
    scale_ = displayScale;
    Rebuild(text, anchor_, cursor_);
}

PageText InlineMathTextEdit::CapturePageText() const {
    return CurrentText();
}

bool InlineMathTextEdit::UndoEdit() {
    if (undo_.empty()) {
        return false;
    }
    EditStep step = std::move(undo_.back());
    undo_.pop_back();
    std::u16string target = content_;
    target.replace(step.start, step.inserted.size(), step.removed);
    PageText text = base_;
    text.content = std::move(target);
    text.styleSpans = std::move(step.spansBefore);
    lastKind_ = ChangeKind::Other;
    Rebuild(text, step.anchorBefore, step.cursorBefore);
    redo_.push_back(std::move(step));
    emit ContentEdited();
    return true;
}

bool InlineMathTextEdit::RedoEdit() {
    if (redo_.empty()) {
        return false;
    }
    EditStep step = std::move(redo_.back());
    redo_.pop_back();
    std::u16string target = content_;
    target.replace(step.start, step.removed.size(), step.inserted);
    PageText text = base_;
    text.content = std::move(target);
    text.styleSpans = std::move(step.spansAfter);
    lastKind_ = ChangeKind::Other;
    Rebuild(text, step.anchorAfter, step.cursorAfter);
    undo_.push_back(std::move(step));
    emit ContentEdited();
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
    // The selection is mapped through the cached map; only the selected
    // characters are copied, never the whole document.
    const QTextCursor cursor = textCursor();
    const std::uint32_t start = text::ModelOffsetForDocument(map_,
        static_cast<std::uint32_t>(cursor.selectionStart()));
    const std::uint32_t end = text::ModelOffsetForDocument(map_,
        static_cast<std::uint32_t>(cursor.selectionEnd()));
    const std::uint32_t size = static_cast<std::uint32_t>(content_.size());
    const std::uint32_t from = std::min(start, size);
    const std::uint32_t to = std::min(std::max(start, end), size);
    auto* data = new QMimeData();
    data->setText(QString::fromUtf16(content_.data() + from, static_cast<qsizetype>(to - from)));
    return data;
}

void InlineMathTextEdit::OnContentsChange(int position, int charsRemoved, int charsAdded) {
    if (internalEdit_ > 0) {
        return;
    }
    pendingChange_ = PendingChange { position, charsRemoved, charsAdded };
}

void InlineMathTextEdit::OnDocumentChanged() {
    if (internalEdit_ > 0) {
        return;
    }
    const std::optional<PendingChange> pending = pendingChange_;
    pendingChange_.reset();
    if (pending.has_value()) {
        // The delta is only usable if it accounts for the whole document
        // change: QTextEdit::setPlainText clears the text before inserting,
        // and Qt does not always report that clear as its own contentsChange.
        const auto expectedCharacters = static_cast<int>(map_.normalized.size())
            + pending->added - pending->removed + 1;
        if (document()->characterCount() != expectedCharacters) {
            ResyncAll();
            return;
        }
        // Qt reports a formatting change through contentsChange with
        // charsRemoved == charsAdded == the formatted range's length, and a
        // real replacement can have equal counts too: the text decides.
        if (ContentChanged(*pending)) {
            ApplyContentChange(*pending);
            return;
        }
    }
    if (!pending.has_value() && document()->characterCount() != documentLength_) {
        // A content edit whose contentsChange we did not observe: recover
        // from the document rather than trusting a stale map.
        ResyncAll();
        return;
    }
    ApplyStyleChange();
}

void InlineMathTextEdit::OnCursorMoved() {
    if (internalEdit_ > 0) {
        return;
    }
    if (pendingChange_.has_value()) {
        // The edit's cursor signal arrived before its contents change; the
        // content handler places the caret once the map is current.
        return;
    }
    if (document()->characterCount() != documentLength_) {
        return;
    }
    SyncCaretFromDocument();
    if (SyncFormulas()) {
        // A formula expanded or collapsed around the caret.  The model
        // content did not change, so the canvas only needs new overlay
        // geometry, not a box refit.
        emit FormulaLayoutChanged();
    }
}

bool InlineMathTextEdit::ContentChanged(const PendingChange& change) const {
    if (change.removed == 0 && change.added == 0) {
        return false;
    }
    const std::uint32_t start = text::ModelOffsetForDocument(map_,
        static_cast<std::uint32_t>(change.position));
    const std::uint32_t oldEnd = text::ModelOffsetForDocument(map_,
        static_cast<std::uint32_t>(change.position + change.removed));
    if (oldEnd < start) {
        return true;
    }
    const std::u16string inserted = text::ReadDocumentTextRange(*document(),
        static_cast<std::uint32_t>(change.position),
        static_cast<std::uint32_t>(change.position + change.added));
    const std::uint32_t oldLength = oldEnd - start;
    if (inserted.size() != oldLength) {
        return true;
    }
    return std::u16string_view(content_).substr(start, oldLength) != inserted;
}

void InlineMathTextEdit::ApplyContentChange(const PendingChange& change) {
    const auto docStart = static_cast<std::uint32_t>(change.position);
    const auto docOldEnd = static_cast<std::uint32_t>(change.position + change.removed);
    const auto docNewEnd = static_cast<std::uint32_t>(change.position + change.added);
    const std::uint32_t start = text::ModelOffsetForDocument(map_, docStart);
    const std::uint32_t oldEnd = text::ModelOffsetForDocument(map_, docOldEnd);
    if (oldEnd < start) {
        ResyncAll();
        return;
    }
    const std::uint32_t removed = oldEnd - start;

    // A rendered placeholder is one document character standing for its whole
    // `$...$` source, so an edit may only remove it whole.  Anything else
    // (which the UI prevents by expanding the caret's formula first) cannot be
    // expressed as a model delta.
    for (const text::InlineMathRange& range : map_.math) {
        const bool insertedInside = removed == 0U && range.start < start && start < range.end;
        const bool partialRemoval = removed > 0U && range.start < oldEnd && range.end > start
            && !(range.start >= start && range.end <= oldEnd);
        if (insertedInside || partialRemoval) {
            ResyncAll();
            return;
        }
    }

    const std::u16string inserted = text::ReadDocumentTextRange(*document(), docStart, docNewEnd);
    const auto insertedSize = static_cast<std::uint32_t>(inserted.size());
    std::u16string removedText = content_.substr(start, removed);
    content_.replace(start, removed, inserted);

    // Rendered placeholders that survive the edit (a removed one is dropped).
    std::vector<text::InlineMathRange> math;
    math.reserve(map_.math.size());
    for (const text::InlineMathRange& range : map_.math) {
        if (range.end <= start) {
            math.push_back(range);
        } else if (range.start >= oldEnd) {
            math.push_back({ range.start - removed + insertedSize, range.end - removed + insertedSize });
        }
    }

    std::vector<PageTextStyleSpan> newSpans = TransformSpans(spans_, start, removed, insertedSize);
    text::TextOffsetMap provisional = text::BuildTextOffsetMap(content_, math);
    if (!inserted.empty()) {
        const std::uint32_t capturedStart = text::DocumentOffsetForModel(provisional, start);
        const std::uint32_t capturedEnd = text::DocumentOffsetForModel(provisional, start + insertedSize);
        std::vector<PageTextStyleSpan> captured = text::CapturePageTextFormattingRange(
            *document(), base_, provisional, capturedStart, capturedEnd, scale_);
        newSpans.insert(newSpans.end(), captured.begin(), captured.end());
    }
    std::sort(newSpans.begin(), newSpans.end(),
        [](const PageTextStyleSpan& a, const PageTextStyleSpan& b) { return a.start < b.start; });
    std::vector<PageTextStyleSpan> mergedSpans;
    mergedSpans.reserve(newSpans.size());
    for (const PageTextStyleSpan& span : newSpans) {
        AppendSpan(mergedSpans, span);
    }

    const ChangeKind kind = removed == 0U && insertedSize > 0U
        ? ChangeKind::Insert
        : (insertedSize == 0U && removed > 0U ? ChangeKind::Delete : ChangeKind::Other);
    bool coalesce = false;
    if (kind == lastKind_ && lastChange_.elapsed() < kCoalesceWindowMs) {
        if (kind == ChangeKind::Insert) {
            // A blank typed after a word starts a new step, like QTextEdit.
            const bool wordBoundary = start < content_.size() && IsBlank(content_[start])
                && start > 0U && !IsBlank(content_[start - 1U]);
            coalesce = insertedSize == 1U && start == lastPosition_ && !wordBoundary;
        } else if (kind == ChangeKind::Delete) {
            // Backspace walks left; Delete stays put.
            coalesce = removed == 1U && (start + 1U == lastPosition_ || start == lastPosition_);
        }
    }

    const std::uint32_t oldAnchor = anchor_;
    const std::uint32_t oldCursor = cursor_;
    const std::vector<PageTextStyleSpan> spansBefore = spans_;
    spans_ = std::move(mergedSpans);
    map_ = std::move(provisional);
    UpdateRenderableForEdit(content_, start, removed, insertedSize);
    // The caret decides which formula stays source; the document already has
    // the edit, so read it before reconciling.
    SyncCaretFromDocument();
    SyncFormulas();
    SyncCaretFromDocument();

    if (coalesce && !undo_.empty()) {
        EditStep& last = undo_.back();
        if (kind == ChangeKind::Insert) {
            last.inserted += inserted;
        } else if (start < last.start) {
            last.removed.insert(0, removedText);
            last.start = start;
        } else {
            last.removed += removedText;
        }
        last.spansAfter = spans_;
        last.anchorAfter = anchor_;
        last.cursorAfter = cursor_;
    } else {
        undo_.push_back(EditStep { start, std::move(removedText), inserted, spansBefore, spans_,
            oldAnchor, oldCursor, anchor_, cursor_ });
    }
    redo_.clear();
    lastKind_ = kind;
    lastPosition_ = kind == ChangeKind::Insert ? start + insertedSize : start;
    lastChange_.restart();
    emit ContentEdited();
}

void InlineMathTextEdit::ApplyStyleChange() {
    if (document()->characterCount() != documentLength_) {
        ResyncAll();
        return;
    }
    SyncCaretFromDocument();
    // Re-style rendered placeholders first: a mergeCharFormat leaves their
    // char formats non-canonical, and the capture below must see the same
    // normalised formats a full capture after reconciliation would.
    styleDirty_ = true;
    SyncFormulas();
    ++fullFormattingCaptures_;
    std::vector<PageTextStyleSpan> captured = text::CapturePageTextFormatting(
        *document(), base_, map_, scale_);
    const bool changed = !SpansEqual(spans_, captured);
    if (!changed) {
        return;
    }
    const std::uint32_t oldAnchor = anchor_;
    const std::uint32_t oldCursor = cursor_;
    EditStep step;
    step.spansBefore = spans_;
    step.spansAfter = std::move(captured);
    step.anchorBefore = oldAnchor;
    step.cursorBefore = oldCursor;
    spans_ = step.spansAfter;
    SyncCaretFromDocument();
    step.anchorAfter = anchor_;
    step.cursorAfter = cursor_;
    undo_.push_back(std::move(step));
    redo_.clear();
    lastKind_ = ChangeKind::Other;
    lastChange_.restart();
    emit ContentEdited();
}

void InlineMathTextEdit::ResyncAll() {
    ++fullDocumentReads_;
    text::DocumentContent read = text::ReadDocumentContent(*document());
    ++fullFormattingCaptures_;
    std::vector<PageTextStyleSpan> captured = text::CapturePageTextFormatting(
        *document(), base_, read.map, scale_);

    // A contiguous replacement for the source-level history.
    std::size_t prefix = 0;
    const std::size_t shorter = std::min(content_.size(), read.content.size());
    while (prefix < shorter && content_[prefix] == read.content[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < shorter - prefix
        && content_[content_.size() - 1U - suffix] == read.content[read.content.size() - 1U - suffix]) {
        ++suffix;
    }
    const auto removed = static_cast<std::uint32_t>(content_.size() - prefix - suffix);
    const auto inserted = static_cast<std::uint32_t>(read.content.size() - prefix - suffix);
    std::u16string removedText = content_.substr(prefix, removed);
    std::u16string insertedText = read.content.substr(prefix, inserted);
    const bool changed = content_ != read.content || !SpansEqual(spans_, captured);
    const std::uint32_t oldAnchor = anchor_;
    const std::uint32_t oldCursor = cursor_;
    const std::vector<PageTextStyleSpan> spansBefore = spans_;

    content_ = std::move(read.content);
    spans_ = std::move(captured);
    map_ = std::move(read.map);
    ++fullFormulaScans_;
    renderable_ = text::RenderableInlineMath(content_, base_.fontSize);
    engineGeneration_ = latex::EngineGeneration();
    styleDirty_ = true;
    SyncCaretFromDocument();
    SyncFormulas();
    SyncCaretFromDocument();
    if (changed) {
        undo_.push_back(EditStep { static_cast<std::uint32_t>(prefix), std::move(removedText),
            std::move(insertedText), spansBefore, spans_, oldAnchor, oldCursor, anchor_, cursor_ });
        redo_.clear();
        emit ContentEdited();
    }
    lastKind_ = ChangeKind::Other;
    lastChange_.restart();
}

void InlineMathTextEdit::UpdateRenderableForEdit(const std::u16string& content,
    std::uint32_t start, std::uint32_t removed, std::uint32_t inserted) {
    const std::uint32_t oldEnd = start + removed;
    std::vector<text::InlineMathRange> next;
    next.reserve(renderable_.size() + 1U);
    for (const text::InlineMathRange& range : renderable_) {
        if (range.end <= start) {
            next.push_back(range);
        } else if (range.start >= oldEnd) {
            next.push_back({ range.start - removed + inserted, range.end - removed + inserted });
        }
        // Ranges inside the edited text are recomputed below.
    }

    const auto size = static_cast<std::uint32_t>(content.size());
    const std::uint32_t editEnd = std::min(start + inserted, size);
    std::uint32_t lineStart = 0;
    if (start > 0 && start <= size) {
        const std::size_t newline = content.rfind(u'\n', start - 1U);
        lineStart = newline == std::u16string::npos ? 0U : static_cast<std::uint32_t>(newline) + 1U;
    }
    std::uint32_t lineEnd = size;
    if (const std::size_t newline = content.find(u'\n', editEnd); newline != std::u16string::npos) {
        lineEnd = static_cast<std::uint32_t>(newline);
    }
    next.erase(std::remove_if(next.begin(), next.end(), [&](const text::InlineMathRange& range) {
        return range.end > lineStart && range.start < lineEnd;
    }), next.end());
    // Formulas never span a line break, so the touched line(s) are the only
    // place the edit can create or destroy one.
    const std::u16string_view line(content.data() + lineStart, lineEnd - lineStart);
    std::vector<text::InlineMathRange> found = text::RenderableInlineMath(line, base_.fontSize);
    for (text::InlineMathRange& range : found) {
        range.start += lineStart;
        range.end += lineStart;
        next.push_back(range);
    }
    std::sort(next.begin(), next.end(),
        [](const text::InlineMathRange& a, const text::InlineMathRange& b) { return a.start < b.start; });
    renderable_ = std::move(next);
}

void InlineMathTextEdit::SyncCaretFromDocument() {
    const QTextCursor caret = textCursor();
    anchor_ = text::ModelOffsetForDocument(map_, static_cast<std::uint32_t>(caret.anchor()));
    cursor_ = text::ModelOffsetForDocument(map_, static_cast<std::uint32_t>(caret.position()));
}

PageText InlineMathTextEdit::CurrentText() const {
    PageText text = base_;
    text.content = content_;
    text.styleSpans = spans_;
    return text;
}

bool InlineMathTextEdit::SyncFormulas() {
    QTextDocument& doc = *document();
    // Registered before any placeholder is inserted: Qt sizes an inline
    // object when it is laid out, and one laid out without a handler stays
    // zero-width for line breaking.
    text::RegisterInlineMathHandler(doc);
    const std::uint64_t generation = latex::EngineGeneration();
    if (generation != engineGeneration_) {
        engineGeneration_ = generation;
        ++fullFormulaScans_;
        renderable_ = text::RenderableInlineMath(content_, base_.fontSize);
    }
    const auto touchesCaret = [&](const text::InlineMathRange& range) {
        return range.start <= cursor_ && cursor_ <= range.end;
    };
    const PageTextStyle fallback = DefaultStyle(base_);
    const bool refreshStyles = styleDirty_;
    styleDirty_ = false;
    ++internalEdit_;
    bool changed = false;

    // Placeholders that should show source expand back to `$...$`.  Back to
    // front keeps the cached offsets of the earlier ranges valid.
    std::vector<text::InlineMathRange> stillRendered;
    stillRendered.reserve(map_.math.size());
    for (auto it = map_.math.rbegin(); it != map_.math.rend(); ++it) {
        if (FindRange(renderable_, *it) != nullptr && !touchesCaret(*it)) {
            stillRendered.push_back(*it);
            continue;
        }
        const std::uint32_t start = text::DocumentOffsetForModel(map_, it->start);
        QTextCursor edit(&doc);
        edit.setPosition(static_cast<int>(start));
        edit.setPosition(static_cast<int>(start) + 1, QTextCursor::KeepAnchor);
        edit.insertText(
            QString::fromUtf16(content_.data() + it->start, static_cast<qsizetype>(it->end - it->start)),
            PlainFormat(edit.charFormat(), fallback, scale_));
        changed = true;
    }
    std::reverse(stillRendered.begin(), stillRendered.end());
    if (changed) {
        map_ = text::BuildTextOffsetMap(content_, std::move(stillRendered));
    }

    // Source that should render collapses; placeholders whose style changed
    // (toolbar formatting) are re-made.  Back to front keeps offsets valid.
    std::vector<text::InlineMathRange> math = map_.math;
    for (auto it = renderable_.rbegin(); it != renderable_.rend(); ++it) {
        if (touchesCaret(*it)) {
            continue;
        }
        const std::u16string_view source = std::u16string_view(content_).substr(it->SourceStart(), it->SourceLength());
        const std::uint32_t start = text::DocumentOffsetForModel(map_, it->start);
        if (FindRange(math, *it) != nullptr) {
            if (!refreshStyles) {
                continue;
            }
            QTextCursor edit(&doc);
            edit.setPosition(static_cast<int>(start));
            edit.setPosition(static_cast<int>(start) + 1, QTextCursor::KeepAnchor);
            const QTextCharFormat current = edit.charFormat();
            QTextCharFormat expected = PlainFormat(current, fallback, scale_);
            text::MakeInlineMathFormat(expected, source, scale_);
            if (expected.objectType() == text::kInlineMathObjectType && expected != current) {
                edit.setCharFormat(expected);
                changed = true;
            }
            continue;
        }
        const std::uint32_t end = text::DocumentOffsetForModel(map_, it->end);
        QTextCursor edit(&doc);
        // The opening `$` carries the run's style.
        edit.setPosition(static_cast<int>(start) + 1);
        QTextCharFormat format = PlainFormat(edit.charFormat(), fallback, scale_);
        text::MakeInlineMathFormat(format, source, scale_);
        if (format.objectType() != text::kInlineMathObjectType) {
            // The engine no longer parses this source; leave it as source.
            continue;
        }
        edit.setPosition(static_cast<int>(start));
        edit.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
        edit.insertText(QString(QChar::ObjectReplacementCharacter), format);
        InsertRange(math, *it);
        changed = true;
    }

    if (changed) {
        map_ = text::BuildTextOffsetMap(content_, std::move(math));
        QTextCursor restored(&doc);
        restored.setPosition(static_cast<int>(text::DocumentOffsetForModel(map_, anchor_)));
        restored.setPosition(static_cast<int>(text::DocumentOffsetForModel(map_, cursor_)), QTextCursor::KeepAnchor);
        setTextCursor(restored);
    }
    --internalEdit_;
    documentLength_ = doc.characterCount();
    return changed;
}

void InlineMathTextEdit::Rebuild(const PageText& text, std::uint32_t anchor, std::uint32_t cursor) {
    ++internalEdit_;
    pendingChange_.reset();
    styleDirty_ = false;
    // `text` is normalized (LF only), so source documents map 1:1 to the model.
    setPlainText(QString::fromUtf16(text.content.data(), static_cast<qsizetype>(text.content.size())));
    text::ApplyPageTextFormatting(*document(), text, scale_);
    const int size = static_cast<int>(text.content.size());
    QTextCursor placed(document());
    placed.setPosition(std::min(static_cast<int>(anchor), size));
    placed.setPosition(std::min(static_cast<int>(cursor), size), QTextCursor::KeepAnchor);
    setTextCursor(placed);

    content_ = text.content;
    spans_ = text.styleSpans;
    map_ = text::BuildTextOffsetMap(content_, {});
    engineGeneration_ = latex::EngineGeneration();
    ++fullFormulaScans_;
    renderable_ = text::RenderableInlineMath(content_, base_.fontSize);
    anchor_ = std::min(anchor, static_cast<std::uint32_t>(content_.size()));
    cursor_ = std::min(cursor, static_cast<std::uint32_t>(content_.size()));
    SyncFormulas();
    documentLength_ = document()->characterCount();
    --internalEdit_;
}

}  // namespace cnote::ui
