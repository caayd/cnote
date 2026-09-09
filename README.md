# cnote

Native Windows PDF annotation app in C++.

`cnote` is built around fast pen input, simple page navigation, and a minimal UI. It opens PDFs, lets you annotate them with strokes and pasted images, saves native `.cnote` projects, and exports back to PDF.

## What It Does

- Open a PDF or start from a blank document.
- Draw, erase, select, move, and transform annotations.
- Paste images from the clipboard onto pages.
- Save editable `.cnote` projects.
- Export annotated pages as PDF.
- Customize palette, brush behavior, auto-save, zoom presets, and shortcuts.
- Open linked views of a project with independent navigation and shared edits.

## Linked Project Views

Choose **New view of this project** from the title menu to open another window
on the same project, including an unsaved project. Each view keeps its own page,
zoom, scroll, tool, and selection. Completed edits update all linked views.
Switching to another linked view commits an unfinished stroke, drag, text edit,
or equation edit before editing continues there.

Undo/redo belongs to the project: undo in either window reverses the latest
project edit. Saving updates the path and saved state in every view, and one
window coordinates autosave. Closing a view leaves the others open; closing the
last view uses the normal save behavior.

**New window** starts a separate blank project in the same app instance. Opening
an already-open file from that window joins its existing session. Choosing
**New** or opening another file detaches only that window from its old project.

Linked views currently work within one app instance. Separate executable or
Explorer launches do not join existing sessions. Window layouts are not restored
after restarting, and live interaction previews are not broadcast to other views.

## Default Controls

- `D` draw
- `E` erase
- `S` select
- `M` move
- `H` pan
- `L` latex equation (edit source with live preview; type `\` for command suggestions, `Tab` or `Enter` completes the selected command, `Ctrl+Enter` commits)
- LaTeX editing auto-closes `{}`, `()`, and `[]`; completing `\frac` inserts both argument slots
- `Ctrl+Z` undo
- `Ctrl+Y` redo
- `Ctrl+O` open
- `Ctrl+S` save `.cnote`
- `Ctrl+B` toggle PDF sidebar
- `Ctrl+W` quick popup: brush size for pen tools, text formatting while editing text
- `Enter` inserts a new line while editing text
- `Ctrl+Enter` commits the active text edit
- `Escape` cancels the active text edit
- `Ctrl+Q` toggle tool wheel
- `Ctrl+Mouse Wheel` zoom
- `PgUp` / `PgDn` previous / next page

Shortcuts are editable in the settings window.

## Build

Requirements:

- Windows
- CMake 3.25+
- MinGW-w64 g++
- `gendef` and `dlltool` if the bundled PDFium import library needs to be generated

Build:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

The repo already includes PDFium binaries under `third_party/pdfium`. The build copies `pdfium.dll` next to the executable automatically.

## Run

```powershell
.\build\cnote.exe
```

Open a file directly:

```powershell
.\build\cnote.exe path\to\document.pdf
```

## Notes

- `.cnote` is the native project format for editable work.
- Auto-save applies to saved `.cnote` documents.
- This project is Windows-only and uses raw Win32, GDI, and GDI+ rather than a cross-platform UI framework.
