#include "App.h"
#include "latex/LatexEngine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>

namespace cnote {

struct ProjectSessionTests {
    static void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    static bool QuitPending() {
        MSG message {};
        // WM_QUIT is synthesized only after pending posted messages are drained.
        // A WM_QUIT-only filter can therefore miss an already requested exit.
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return false;
    }

    static void StrokeOn(App& view, std::size_t page) {
        view.StartStrokeSegment(page, { 20, 20 });
        view.activeStroke_->points.push_back({ 80, 80 });
        view.EndStroke();
    }

    static void Run(const std::filesystem::path& scratch) {
        App root(GetModuleHandleW(nullptr));
        Require(root.CreateMainWindow(SW_HIDE), "create original window");
        root.CreateBlankDocument();
        root.AddBlankPageAfterCurrent();
        root.SetViewPageIndex(0);
        root.zoom_ = root.targetZoom_ = 1.25f;
        Require(root.CreateAdditionalWindow(true, SW_HIDE), "create linked view");
        App& second = *root.childWindows_.back();
        Require(second.session_ == root.session_, "linked windows share a session");
        Require(root.session_->views.size() == 2, "two registered views");
        second.SetViewPageIndex(1);
        second.zoom_ = second.targetZoom_ = 0.75f;
        second.panY_ = -300;
        Require(root.CurrentPageIndex() == 0 && root.zoom_ == 1.25f, "independent navigation");

        second.pageSurfaceCaches_.assign(2, {});
        second.pageSurfaceCaches_[0].valid = true;
        second.selectedStrokeIndices_[0].push_back(0);
        const auto revision = root.session_->revision;
        StrokeOn(root, 0);
        Require(second.session_->document.PageAt(0).strokes.size() == 1, "committed stroke shared");
        Require(root.session_->revision == revision + 1, "one revision per commit");
        Require(!second.pageSurfaceCaches_[0].valid, "remote page cache invalidated");
        Require(second.selectedStrokeIndices_[0].empty(), "stale remote selection cleared");
        Require(second.CurrentPageIndex() == 1 && second.panY_ == -300 && second.zoom_ == 0.75f,
                "content notifications preserve remote viewport");
        Require(second.CurrentWindowTitle().ends_with(L"*"), "dirty title shared");
        second.Undo();
        Require(root.session_->document.PageAt(0).strokes.empty(), "undo from another view");
        Require(root.CanRedo(), "redo stack shared");
        root.Redo();
        Require(second.session_->document.PageAt(0).strokes.size() == 1, "redo shared");
        Require(second.CurrentPageIndex() == 0, "undo focuses initiating view");

        second.SetViewPageIndex(1);
        RECT client {};
        GetClientRect(second.window_, &client);
        const int oldTop = second.CalculatePageView(client, 1).rect.top;
        root.SetViewPageIndex(0);
        root.AddBlankPageAfterCurrent();
        Require(second.CurrentPageIndex() == 2, "insertion preserves remote page identity");
        Require(second.CalculatePageView(client, 2).rect.top == oldTop, "insertion preserves remote scroll anchor");
        root.Undo();
        Require(second.CurrentPageIndex() == 1, "undo insertion remaps remote page");
        Require(second.CalculatePageView(client, 1).rect.top == oldTop, "undo preserves remote scroll anchor");
        root.SetViewPageIndex(1);
        Require(root.DeleteCurrentPage(), "delete viewed page");
        Require(second.CurrentPageIndex() == 0, "deleted remote page clamps safely");
        root.Undo();

        // Switching to another view finishes an unfinished gesture before undo.
        root.StartStrokeSegment(0, { 30, 30 });
        Require(second.IsSessionInteractionActive(), "autosave sees another view's gesture");
        SendMessageW(second.window_, WM_ACTIVATE, WA_ACTIVE, 0);
        Require(!root.isDrawing_ && !second.IsSessionInteractionActive(), "activation finishes other view's gesture");
        second.Undo();
        Require(root.session_->document.PageAt(0).strokes.size() == 1, "undo committed gesture from another view");

        PageText text;
        text.id = 1;
        text.x = 30;
        text.y = 30;
        text.content = u"original";
        root.session_->document.PageAt(0).texts.push_back(text);
        root.BeginTextEdit(0, 0, text);
        Require(root.textEditWindow_ != nullptr, "begin text edit");
        SetWindowTextW(root.textEditWindow_, L"edited in first view");
        SendMessageW(second.window_, WM_ACTIVATE, WA_ACTIVE, 0);
        Require(root.textEditWindow_ == nullptr, "view switch commits text editor");
        Require(second.session_->document.PageAt(0).texts[0].content == u"edited in first view", "text commit shared");
        second.Undo();
        Require(root.session_->document.PageAt(0).texts[0].content == u"original", "cross-view text undo");
        root.Redo();

        const std::filesystem::path projectPath = scratch / L"linked.cnote";
        Require(second.SaveCnoteToPath(projectPath, false), "save shared project");
        Require(!root.session_->dirty && !root.CurrentWindowTitle().ends_with(L"*"), "save clears every dirty title");
        Require(root.session_->document.FilePath() == projectPath, "save path shared");
        Require(root.AutoSaveOwner() == &root && second.AutoSaveOwner() == &root, "single autosave owner");
        second.SetSessionAutoSaveInterval(15);
        Require(root.session_->autoSaveIntervalSeconds == 15 && root.settings_.autoSaveIntervalSeconds == 15,
                "autosave interval changes reach the owning view");
        auto saved = ProjectFile::Load(projectPath);
        Require(saved && saved->PageAt(0).texts[0].content == u"edited in first view", "shared edits round trip");
        Require(!second.SaveCnoteToPath(scratch / L"missing" / L"failure.cnote", false), "save failure reported");

        auto defaults = root.session_->document.NewPageDefaults();
        defaults.width = 420;
        root.session_->document.SetNewPageDefaults(defaults);
        root.newPageSettings_.width = defaults.width;
        root.NotifyDocumentMutated();
        Require(second.newPageSettings_.width == 420, "project page defaults shared");
        Require(!second.SaveCnoteToPath(scratch / L"missing" / L"failure.cnote", false)
                && root.session_->dirty, "failed save preserves shared dirty state");

        Require(root.CreateAdditionalWindow(false, SW_HIDE), "create independent window");
        App& third = *root.childWindows_.back();
        Require(third.session_ != root.session_, "new window starts independent project");
        Require(third.OpenDocumentPath(scratch / L"." / L"LINKED.cnote"), "open equivalent existing path");
        Require(third.session_ == root.session_, "opening existing file attaches to session");
        StrokeOn(third, 0);
        auto originalSession = root.session_;
        third.CreateBlankDocument();
        Require(third.session_ != originalSession, "new document detaches only its view");
        Require(originalSession->dirty && originalSession->document.PageAt(0).strokes.size() == 2,
                "switch preserves original unsaved edits");
        Require(!third.SaveCnoteToPath(projectPath, false), "cannot overwrite a different open session");
        SendMessageW(third.window_, WM_CLOSE, 0, 0);
        Require(!QuitPending(), "closing unrelated window keeps process alive");
        std::erase_if(root.childWindows_, [](const auto& view) { return view->window_ == nullptr; });

        // Closing the original host must neither prompt nor terminate the child.
        SendMessageW(root.window_, WM_CLOSE, 0, 0);
        Require(root.window_ == nullptr && IsWindow(second.window_), "original closes while linked view survives");
        Require(!QuitPending(), "original close does not post quit");
        Require(originalSession->views.size() == 1 && second.AutoSaveOwner() == &second, "autosave ownership transfers");
        Require(second.CanAutoSaveCurrentDocument(), "remaining view can autosave dirty project");
        Require(second.pdfBackend_.ExportPdf(second.session_->document, scratch / L"after-close.pdf",
                    1.0f, 0.55f, 3, 2, std::span<const std::uint8_t> {}), "PDF rendering survives another window's destruction");
        Require(latex::Parse(u"x^2", 24, true) != nullptr, "LaTeX survives another window's destruction");
        Require(second.CreateAdditionalWindow(true, SW_HIDE), "surviving view can create another linked view");
        App& fourth = *root.childWindows_.back();
        SendMessageW(fourth.window_, WM_CLOSE, 0, 0);
        std::erase_if(root.childWindows_, [](const auto& view) { return view->window_ == nullptr; });
        SendMessageW(second.window_, WM_CLOSE, 0, 0);
        Require(QuitPending(), "last window posts quit");
        Require(originalSession->views.empty() && !originalSession->dirty, "last close saves once and detaches");

        VerifyDeactivationAutoSave(scratch);
        VerifyStaleHistoryAction();
    }

    // A recorded action whose target no longer exists must not mutate the
    // document, move either history stack or bump the revision.
    static void VerifyStaleHistoryAction() {
        static_cast<void>(QuitPending());
        App probe(GetModuleHandleW(nullptr));
        Require(probe.CreateMainWindow(SW_HIDE), "create stale-history probe window");
        probe.CreateBlankDocument();
        probe.session_->undoActions.clear();
        probe.session_->redoActions.clear();

        HistoryAction staleStroke {};
        staleStroke.type = HistoryActionType::AddStroke;
        staleStroke.pageIndex = 0;
        staleStroke.index = 999;
        probe.session_->undoActions.push_back(staleStroke);
        const std::uint64_t revision = probe.session_->revision;
        Require(probe.CanUndo(), "stale action is on the undo stack");
        probe.Undo();
        Require(probe.session_->undoActions.size() == 1 && probe.session_->redoActions.empty(),
            "stale stroke index leaves both history stacks unchanged");
        Require(probe.session_->revision == revision, "stale stroke index leaves revision unchanged");
        Require(probe.session_->document.PageAt(0).strokes.empty(), "stale stroke index does not mutate document");

        probe.session_->undoActions.clear();
        HistoryAction staleText {};
        staleText.type = HistoryActionType::AddText;
        staleText.pageIndex = 99;
        probe.session_->undoActions.push_back(staleText);
        probe.Undo();
        Require(probe.session_->undoActions.size() == 1 && probe.session_->redoActions.empty(),
            "stale page index leaves both history stacks unchanged");
        Require(probe.session_->revision == revision, "stale page index leaves revision unchanged");

        // Positive control: a valid recorded action still applies and moves
        // exactly one action between the stacks.
        StrokeOn(probe, 0);
        const std::size_t strokeCount = probe.session_->document.PageAt(0).strokes.size();
        const std::size_t undoBefore = probe.session_->undoActions.size();
        probe.Undo();
        Require(probe.session_->document.PageAt(0).strokes.size() == strokeCount - 1
                && probe.session_->undoActions.size() == undoBefore - 1
                && probe.session_->redoActions.size() == 1,
            "valid action applies and moves one action to the redo stack");
        SendMessageW(probe.window_, WM_CLOSE, 0, 0);
    }

    // Positive control for the WM_ACTIVATEAPP guard in MessageLoop: a visible
    // window must still autosave on app deactivation, and a minimized window
    // (which keeps WS_VISIBLE) must keep doing so.
    static void VerifyDeactivationAutoSave(const std::filesystem::path& scratch) {
        App probe(GetModuleHandleW(nullptr));
        Require(probe.CreateMainWindow(SW_HIDE), "create auto-save probe window");
        ShowWindow(probe.window_, SW_SHOWNA);
        Require(IsWindowVisible(probe.window_) != 0, "probe window is visible");
        probe.CreateBlankDocument();
        const std::filesystem::path path = scratch / L"deactivation-autosave.cnote";
        Require(probe.SaveCnoteToPath(path, false), "save auto-save probe project");
        Require(!probe.session_->dirty, "probe project starts clean");
        StrokeOn(probe, 0);
        Require(probe.session_->dirty, "probe project dirty after stroke");
        SendMessageW(probe.window_, WM_ACTIVATEAPP, FALSE, 0);
        Require(!probe.session_->dirty, "deactivation autosave fires for a visible window");
        const auto reloaded = ProjectFile::Load(path);
        Require(reloaded && reloaded->PageAt(0).strokes.size() == 1,
            "deactivation autosave wrote the committed stroke");

        StrokeOn(probe, 0);
        Require(probe.session_->dirty, "probe project dirty again");
        ShowWindow(probe.window_, SW_MINIMIZE);
        Require(IsWindowVisible(probe.window_) != 0, "minimized window keeps WS_VISIBLE");
        SendMessageW(probe.window_, WM_ACTIVATEAPP, FALSE, 0);
        Require(!probe.session_->dirty, "deactivation autosave fires for a minimized window");
        SendMessageW(probe.window_, WM_CLOSE, 0, 0);
    }
};

} // namespace cnote

int main() {
    const auto scratch = std::filesystem::temp_directory_path() /
        (L"cnote-session-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(scratch);
    // Keep test preferences separate from the user's real cnote configuration.
    _wputenv_s(L"APPDATA", scratch.c_str());
    try {
        cnote::ProjectSessionTests::Run(scratch);
        std::cout << "Project session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
