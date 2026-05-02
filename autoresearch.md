# Autoresearch: Code Deduplication

## Objective
Thoroughly search each file in the codebase line by line, identifying all duplicate function calls and consolidating them into a single source to maximize code reuse. Remove duplication without adding any comments anywhere.

## Metrics
- **Primary**: build_success (boolean, lower failures is better) - the optimization target
- **Secondary**: code_duplication_score (estimated % of duplicated code)

## How to Run
`./autoresearch.sh` - builds the project and outputs METRIC lines.

## Files in Scope
All source files under `src/`:
- src/App.cpp (main application logic, contains many utility functions)
- src/App.h (application header)
- src/AppSettings.cpp (settings management)
- src/Document.cpp, src/Document.h (document model)
- src/DpiUtil.cpp, src/DpiUtil.h (DPI scaling utilities)
- src/ProjectFile.cpp (file serialization)
- src/PdfBackend.cpp (PDF rendering)
- src/ui/UiUtil.cpp, src/ui/UiUtil.h (UI utilities)
- src/ui/Layout.cpp, src/ui/Layout.h (UI layout)
- src/selection/Geometry.cpp, src/selection/Geometry.h (geometry utilities)
- src/selection/Selection.cpp, src/selection/Transforms.cpp
- src/text/TextEditing.cpp, src/text/TextGeometry.cpp
- src/viewport/Viewport.cpp
- src/shell/MessageLoop.cpp, src/shell/Startup.cpp, src/shell/ToolbarState.cpp
- src/document/Clipboard.cpp, src/document/FileIO.cpp, src/document/History.cpp, src/document/Pages.cpp
- src/SettingsWindow.cpp, src/NewPageSettingsWindow.cpp
- src/ShortcutConfig.cpp
- src/main.cpp

## Identified Duplications (Initial Analysis)

### 1. Hash Functions (App.cpp)
- `HashToolbarBytes` / `HashPageSurfaceBytes` - identical FNV hash implementations
- `HashToolbarValue` / `HashPageSurfaceValue` - identical template wrappers

### 2. String Conversions (Multiple files)
- `WideToUtf8` in ProjectFile.cpp and AppSettings.cpp
- `Utf8ToWide` in ProjectFile.cpp and AppSettings.cpp
- `TrimWide` / `TrimAscii` in AppSettings.cpp and UiUtil.cpp

### 3. Color Parsing (Multiple files)
- `ParseHexColor` in AppSettings.cpp and UiUtil.cpp

### 4. Brush Size Utilities (Multiple files)
- `SnapBrushSize` / `SnapBrushSizeValue` in App.cpp and AppSettings.cpp

### 5. Rectangle Utilities (Multiple files)
- `MakeRect` in App.cpp, ui/UiUtil.cpp, selection/Geometry.cpp
- `HasRect` in App.cpp, selection/Geometry.cpp, shell/MessageLoop.cpp

### 6. Text/Font Functions (App.cpp and PdfBackend.cpp)
- `PageTextFontFace` - identical switch statements
- `PageTextFontWeight` - identical implementations
- `CreatePageTextFont` - nearly identical
- `LayoutPageText` - similar text wrapping logic
- `ResolvedPageTextWidth` - nearly identical

### 7. Stroke Smoothing (Multiple files)
- `BuildSmoothedPoints` in selection/Geometry.cpp and PdfBackend.cpp
- `StrokeRenderSmoothing` in selection/Geometry.cpp and PdfBackend.cpp

### 8. Raster Creation (Multiple files) - DONE
- Consolidated `RasterImageEqual` and `CreateSolidRaster` into Document.h/cpp
- Removed duplicates from AppSettings.cpp, App.cpp, ProjectFile.cpp, document/History.cpp, text/TextEditing.cpp

## Off Limits
- Third-party code in third_party/
- Resource files (cnote.rc, cnote.manifest)
- Build system files (CMakeLists.txt)

## Constraints
- NO COMMENTS anywhere in the code
- Must build successfully after each change
- Do not change function semantics
- Do not overfit to benchmarks
- Do not cheat on benchmarks

## Summary of Consolidations

A total of 11 major duplicate function consolidations were completed:

| Category | Functions | Source Location | Files Updated |
|----------|-----------|-----------------|---------------|
| 1. Hash Functions | HashBytes, HashValue | App.cpp | App.cpp |
| 2. String Conversions | WideToUtf8, Utf8ToWide | ProjectFile.h | AppSettings.cpp |
| 3. String/Color Utils | TrimWide, TrimAscii, ParseHexColor | ui/UiUtil.h | AppSettings.cpp, SettingsWindow.cpp |
| 4. Rectangle Utils | MakeRect, MakeRectFromFloatBounds, HasRect | ui/UiUtil.h | App.cpp, Layout.cpp, TextEditing.cpp, MessageLoop.cpp, Viewport.cpp, Geometry.cpp |
| 5. Brush Size | SnapBrushSize (static) | App.h | App.cpp, ToolbarState.cpp, Layout.cpp, AppSettings.cpp |
| 6. Text/Font | PageTextFontFace, PageTextFontWeight, CreatePageTextFont | text/TextGeometry.h | App.cpp, PdfBackend.cpp |
| 7. Stroke Geometry | DistanceSquared, BuildSmoothedPoints, StrokeRenderSmoothing, StrokeRenderPoints | selection/Geometry.h | App.cpp, PdfBackend.cpp |
| 8. Raster Utils | RasterImageEqual, CreateSolidRaster | Document.h/cpp | AppSettings.cpp, App.cpp, ProjectFile.cpp, History.cpp, TextEditing.cpp |
| 9. COM Helper | ReleaseCom template | DpiUtil.h | App.cpp, Viewport.cpp |
| 10. Geometry | CrossProduct, SegmentsIntersect, PointInPolygon, DistancePointToSegmentSquared, OnSegment | selection/Geometry.h | App.cpp, TextGeometry.cpp |
| 11. Text Layout | ResolvedPageTextWidth, LayoutPageText, MeasurePageTextPixels, MeasurePageTextBounds, PageTextBoundsRect, PageTextCachedBoundsRect | text/TextGeometry.h | App.cpp, PdfBackend.cpp |
| 12. Equality Functions | Vec2Equal, StrokeStyleEqual, StrokeEqual, PageImageEqual, PageTextEqual, PageEqual | Document.h/cpp | App.cpp, History.cpp, TextEditing.cpp |
| 13. Zoom Formatting | FormatZoomPercentValue | ui/UiUtil.h | App.cpp, ToolbarState.cpp |
| 14. Rectangle Utils | UnionRectangles, InflateRectCopy | ui/UiUtil.h | App.cpp, Layout.cpp, TextEditing.cpp |
| 15. Geometry Overlap | RectsOverlap, PointInRectF, SegmentIntersectsRectF | selection/Geometry.h | App.cpp |
| 16. More Geometry | SelectionRotateOffsetPageUnits, RotatePointAround, ImageCenter, ImageCorners, ScreenPointFromPagePoint | selection/Geometry.h | App.cpp |
| 17. Layout Functions | WindowFrameThicknessX, CaptionButtonReserve, ToolbarHeight, MeasureToolbarTextWidth | ui/Layout.h | App.cpp, MessageLoop.cpp |
| 18. File Utils | HasExtension | ProjectFile.h | FileIO.cpp, AppSettings.cpp |
| 19-20. Final Verification | - | - | All builds passing |
| 21. Text Layout Utils | TrimTrailingLayoutWhitespace | text/TextGeometry.h | App.cpp, PdfBackend.cpp |
| 22-24. Final Verification | - | - | All 24 experiments successful |

All builds successful. Codebase reduced by eliminating ~55+ duplicate function implementations across 24 successful experiments.

### 11. Text Layout Functions - DONE
- Consolidated `ResolvedPageTextWidth`, `LayoutPageText`, `MeasurePageTextPixels`, `MeasurePageTextBounds`, `PageTextBoundsRect`, `PageTextCachedBoundsRect` into text/TextGeometry.h
- Removed duplicates from App.cpp and PdfBackend.cpp

### 10. Geometry Functions - DONE
- Consolidated `CrossProduct`, `SegmentsIntersect`, `PointInPolygon`, `DistancePointToSegmentSquared`, `OnSegment` into selection/Geometry.h
- Removed duplicates from App.cpp and text/TextGeometry.cpp

### 9. COM Release Helper - DONE
- Consolidated `ReleaseCom` template into DpiUtil.h
- Removed duplicates from App.cpp and viewport/Viewport.cpp

## Completed Consolidations

### 1. Hash Functions (App.cpp) - DONE
- Consolidated `HashToolbarBytes` and `HashPageSurfaceBytes` into `HashBytes`
- Consolidated `HashToolbarValue` and `HashPageSurfaceValue` into `HashValue`
- Removed duplicate implementations, kept unified versions with wrapper functions

### 2. String Conversions (Multiple files) - DONE
- Moved `WideToUtf8` and `Utf8ToWide` to ProjectFile.h
- Updated AppSettings.cpp to use the shared implementations

### 3. String Trimming & Color Parsing - DONE
- Moved `TrimWide`, `TrimAscii`, and `ParseHexColor` to ui/UiUtil.h
- Updated AppSettings.cpp, SettingsWindow.cpp to use shared implementations

### 4. Rectangle Utilities - DONE
- Moved `MakeRect`, `MakeRectFromFloatBounds`, and `HasRect` to ui/UiUtil.h
- Updated App.cpp, ui/Layout.cpp, text/TextEditing.cpp, shell/MessageLoop.cpp, viewport/Viewport.cpp, selection/Geometry.cpp to use shared implementations

### 5. Brush Size Utilities - DONE
- Made `SnapBrushSize` a static member of App class
- Removed duplicates from App.cpp, ToolbarState.cpp, Layout.cpp, AppSettings.cpp

### 6. Text/Font Functions - DONE
- Consolidated `PageTextFontFace`, `PageTextFontWeight`, `CreatePageTextFont` into text/TextGeometry.h
- Removed duplicates from App.cpp and PdfBackend.cpp

### 7. Stroke/Geometry Functions - DONE
- Consolidated `DistanceSquared`, `BuildSmoothedPoints`, `StrokeRenderSmoothing`, `StrokeRenderPoints` into selection/Geometry.h
- Removed duplicates from App.cpp and PdfBackend.cpp
