# cnote

Native C++ prototype for a PDF-oriented drawing and annotation app.

## Current scope

This first pass is a dependency-light Windows desktop app built with:

- C++20
- CMake
- Win32 + GDI for the window, canvas, and drawing

What it does today:

- opens a native window
- creates a blank page surface
- lets you draw freehand ink strokes on that page
- includes a dark header with a file-title dropdown for file actions/settings and a live zoom percentage control
- includes `Settings` in the file-title dropdown for palette, brush-size, and smoothing controls
- keeps the native Windows title bar and window buttons
- includes tool, color, undo, and redo controls in the header
- includes an eraser tool for deleting strokes
- supports zooming, panning, brush sizing, and color switching
- renders opened PDFs in a stacked multi-page document view and lets you ink on top of each page
- dynamically upgrades raster quality and caches visible page surfaces to keep scrolling and drawing responsive
- saves editable note files as `.cnote`, embedding the source PDF and all overlays for later editing
- reopens `.cnote` files directly from the app or from a shell file association/command-line launch

What it does not do yet:

- persist annotations back into a PDF
- support selections, text, shapes, or export

## Build

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

Run:

```powershell
.\build\cnote.exe
```

## Controls

- `Left Mouse`: draw
- `Header`: click the file title for file actions/settings, click the zoom percentage to type a zoom value, then use the tool/color/history controls
- `Settings`: edit palette colors with HSL sliders, paste hex values, add/remove palette colors, change brush size, tune stabilization/smoothing, adjust autosave, and configure zoom presets/shortcut bindings
- `Ctrl+B`: toggle the PDF page preview sidebar and its page navigator
- `Ctrl+W`: open the active brush/eraser size slider near the cursor
- `Middle Mouse Hold`: pan
- `Mouse Wheel`: scroll
- `Trackpad Horizontal Scroll`: pan horizontally
- `Ctrl` + `Mouse Wheel`: zoom
- `Page Up` / `Page Down`: jump focus to the previous or next PDF page
- `Arrow Keys`: pan
- `Ctrl+N`: new blank document
- `Ctrl+O`: open a `.pdf` or `.cnote`
- `Ctrl+S`: save the current `.cnote` immediately, or prompt for a `.cnote` path if the document has not been saved yet
- `Ctrl+Z`: undo
- `Ctrl+Y` or `Ctrl+Shift+Z`: redo
- `[` or `-`: smaller brush
- `]` or `+`: larger brush
- `C`: cycle color
- `0`: reset view

## PDF backend

The app now uses PDFium behind `IPdfBackend` in [src/PdfBackend.h](C:\Users\Cayd\Documents\Projects\ctrainer\src\PdfBackend.h) to rasterize PDF pages into bitmaps for display.

## Cnote files

The editor-native note format is `.cnote`.

- If you open a PDF and save a note, the `.cnote` file embeds the PDF bytes plus strokes and pasted images so the note stays editable even if the original PDF moves.
- If you start from a blank document, the `.cnote` file stores the editable page content without requiring a PDF source.
- Use the file-title dropdown's `Export as PDF...` action to write a flattened PDF version of the current note.

The next natural steps are:

1. unload distant page rasters for very large PDFs
2. map ink strokes into PDF page coordinates for export
3. save new annotations or flattened output back to PDF
