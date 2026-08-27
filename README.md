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

## Default Controls

- `D` draw
- `E` erase
- `S` select
- `M` move
- `H` pan
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
