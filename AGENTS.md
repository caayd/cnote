# AGENTS.md

Instructions for humans and coding agents working on cnote. Several agents may work in parallel, so this
workflow is mandatory.

## Rules

1. **One change = one git worktree = one branch = one PR.** Never edit the main checkout, never commit to `main`.
2. **Humans merge.** Agents open PRs; merge only when the person you work for says so (squash merge).
3. **Keep changes scoped.** No unrelated refactors; delete code you replace, no fallbacks or commented-out code.

## Worktree flow

From the main clone:

```sh
git fetch origin main                                    # never branch from a stale main
git worktree add ../cnote-worktrees/<topic> -b <github-login>/<topic> origin/main
cd ../cnote-worktrees/<topic>
```

Work, then open the PR:

```sh
git fetch origin && git rebase origin/main               # force-push your own branch only
git push -u origin HEAD                                  # or --force-with-lease after a rebase
gh pr create --fill                                      # body: why, and what you ran and saw
```

After the PR is merged, from the main clone:

```sh
gh pr view <github-login>/<topic> --json state -q .state   # must print MERGED (squash merges don't show in git log)
git worktree remove ../cnote-worktrees/<topic>             # refuses with uncommitted changes: check, don't --force
git branch -D <github-login>/<topic>
git pull --ff-only origin main
```

On Windows, if `git worktree remove` fails with `Directory not empty`, finish with
`cmd /c rmdir /s /q ..\cnote-worktrees\<topic>`.

## Build and test

`CNOTE_QT` (Qt kit, e.g. `C:/Qt/6.11.2/mingw_64`) and on Windows `CNOTE_CXX` (the kit's `g++.exe`) must be set;
PDFium comes from `third_party/pdfium`, and `ccache` must be on `PATH`. Every worktree builds in its own `build-dev/`:

```sh
cmake --preset dev
cmake --build --preset dev                     # or: --target cnote_chrome_tests
ctest --preset dev                             # or: -R chrome_qt
```

- `dev` builds shared libraries through ccache, so a new worktree's first build mostly hits the cache (about
  20 s, about 90 MB). Always build with `--preset dev`; it sets `CCACHE_BASEDIR`, without which nothing is shared.
- Never use a build directory configured in another checkout (the main clone's `build/` included): CMake build
  directories hard-code their source path, so they build the other checkout's code.
- `release` is the static build CI and packages use. Run it too when you change linkage, `CMakeLists.txt`
  install/deploy rules or packaging (`tools/verify-package.cmake`).
- A QObject class in `cnote_ui` that declares signals needs `CNOTE_UI_EXPORT` (`#include "cnote_ui_export.h"`),
  or `connect` to its signals from tests fails with "signal not found" in the `dev` build.
- A new test is an executable plus `add_test` in `CMakeLists.txt`; on Windows, also add the target to the
  test runtime staging list (`cnote_stage_test_runtime`) so it finds the Qt and PDFium DLLs.
- Don't set `QT_QPA_PLATFORM` globally; each test picks its platform (see "Test" in `README.md`).
- Agents never open a visible window: the suite runs offscreen or keeps its native windows off screen, and the
  app is exercised with `--capture`/`--script` (below). A new test that shows a window on the native platform
  must keep it off screen too (`cnote::shell::OffscreenWindows`, as `window_chrome_native_qt` does).

## Before opening a PR

- `ctest --preset dev` passes in full, with no new compiler warnings (`-Wall -Wextra -Wpedantic`).
- Exercise the change in the app when it affects behavior, headless: `build-dev/cnote.exe --script=steps.txt
  --capture=out.png [file]` (`build-dev/cnote` elsewhere; in PowerShell append `| Out-Host`). The script clicks,
  drags, types and captures; see "Headless runs" in `README.md`. Look at the PNGs, and say in the PR what you ran
  and what you saw.
- **Visual changes need screenshots in the PR:** use those captures (`--scale=1` unless DPI matters). Push them
  to an orphan branch `<github-login>/<topic>-screenshots` and embed
  `https://github.com/caayd/cnote/blob/<that branch>/<file>.png?raw=true`.
- CI (`.github/workflows/ci.yml`) runs on every PR; check it with `gh pr checks`.

## Repo map

Which library a file belongs to is set by the `add_library` source lists in `CMakeLists.txt`; directories mostly
follow the layers:

| Path | What |
|---|---|
| `src/Document.*`, `src/document/HistoryReducer.*` | Model (`cnote_model`): document and history. Standard C++ only, no Qt. |
| `src/document/`, `src/text/`, `src/render/`, `src/latex/`, `src/settings/`, `src/selection/`, `src/ProjectFile.*`, `src/PdfBackend.cpp` | Services (`cnote_services`): codecs, text layout, page rendering, PDF, MicroTeX, settings. Qt Core/Gui, no widgets. |
| `src/ui/`, `src/qt/`, `src/shell/`, `src/platform/windows/` | UI (`cnote_ui`): canvas, chrome widgets, dialogs, controller, headless capture driver, Windows touchpad adapter. |
| `src/main.cpp` | The `cnote` executable. |
| `tests/` | One executable per area, registered in `CMakeLists.txt`. |
| `third_party/` | Vendored PDFium SDK, MicroTeX, tinyxml2, fonts, MinGW headers. Don't modify without updating `THIRD-PARTY-NOTICES.md`. |

`README.md` covers the layer rules, build requirements, packaging and test details.
