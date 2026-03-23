# ctrainer

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
- supports zooming, panning, brush sizing, and color switching
- opens a `.pdf` path into a placeholder document session so the app flow is already wired for PDF work

What it does not do yet:

- render actual PDF page content
- persist annotations back into a PDF
- support multiple pages, selections, text, shapes, or export

## Build

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

Run:

```powershell
.\build\ctrainer.exe
```

## Controls

- `Left Mouse`: draw
- `Middle Mouse Hold`: pan
- `Mouse Wheel`: zoom
- `Arrow Keys`: pan
- `Ctrl+N`: new blank document
- `Ctrl+O`: open a PDF path into the placeholder PDF session
- `[` or `-`: smaller brush
- `]` or `+`: larger brush
- `1`-`5`: choose color
- `C`: cycle color
- `0`: reset view

## Next step for real PDFs

The app already isolates PDF concerns behind `IPdfBackend` in [src/PdfBackend.h](C:\Users\Cayd\Documents\Projects\ctrainer\src\PdfBackend.h). The natural next move is to swap `StubPdfBackend` for a real renderer/editor backend such as PDFium or MuPDF and use it to:

1. rasterize each page into a backing bitmap
2. map ink strokes into PDF page coordinates
3. save new annotations or flattened output
