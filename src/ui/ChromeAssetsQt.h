#pragma once

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace cnote::ui {

// Reference SVG artwork used by the compact chrome (migration plan step 6.1).
//
// Legacy App::LoadSvgIcon (src/App.cpp:1227) reads files from
// `<base>/assets/icons/<name>` and paints the parsed paths as a single
// monochrome tint (App::PaintSvgIcon, src/App.cpp:1300).  The Qt port keeps the
// same artwork and the same monochrome tint, but renders it through
// QSvgRenderer: assets are compiled into cnote_chrome_qt as a Qt resource
// (prefix /cnote) with a filesystem fallback so an out-of-tree harness can run
// the widgets without the repository build.

// Adds the compiled resource bundle.  Safe to call repeatedly; the static
// library resource needs an explicit Q_INIT_RESOURCE because the linker may not
// pull the generated object in on its own.
void EnsureChromeResources();

// Filesystem asset root used when the Qt resource is unavailable or the file is
// missing from it.  Checks, in order: $CNOTE_ASSETS_ROOT, <appdir>/assets,
// <appdir>/../assets, <cwd>/assets.
[[nodiscard]] QString ChromeAssetsRoot();

// Resource path (":/cnote/icons/<name>") when the compiled bundle has the file,
// otherwise the filesystem path.  `<name>` is e.g. "draw.svg" or "text/bold.svg".
[[nodiscard]] QString ChromeIconPath(const QString& fileName);

// Renders one reference icon tinted `color` to fit `size` minus the reference
// 8-logical-pixel inset, centered, like App::PaintSvgIcon.  Returns a null
// pixmap when the artwork cannot be loaded; callers then paint their own
// fallback glyph, exactly like the legacy PaintSvgOrFallbackLabel path.
[[nodiscard]] QPixmap ChromeIconPixmap(const QString& fileName, const QSize& size, const QColor& color, qreal devicePixelRatio = 1.0);

}  // namespace cnote::ui
