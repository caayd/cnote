// Vendored MicroTeX Qt platform adapter.
//
// Taken verbatim from NanoMichael/MicroTeX @ 0e3707f6 (MIT) at
//   src/platform/qt/graphic_qt.h
// and adapted locally for cnote; every local change is marked "cnote:". The
// upstream revision is pinned by the top-level CMakeLists.txt (target
// `microtex_qt`), the upstream license is third_party/microtex/LICENSE.
//
// cnote deviations from the pinned revision:
//   * Font_qt keeps the pixel-like MicroTeX size and converts it to points for
//     a fixed 96-DPI reference device (never integer pixel rounding).
//   * Font_qt loads font files through Qt 6's static QFontDatabase API and
//     treats a font file that cannot be loaded as an error, because cnote
//     ships the MicroTeX resources as real files next to the executable.
//   * Graphics2D_qt remembers the painter transform it was constructed with and
//     restores that (not the identity) in reset(), so MicroTeX's trailing
//     g2.reset() never erases the caller's page/DPR transform.
//   * wstring_to_QString converts one 16-bit code unit per wchar_t instead of
//     using QString::fromStdWString, which would reinterpret UTF-16 surrogate
//     units as Unicode scalar values on platforms with 32-bit wchar_t.
//   * Glyphs are rasterised at the device size (Graphics2D_qt::drawFontText)
//     instead of at MicroTeX's sub-pixel nominal font size, because Qt drops
//     glyphs drawn from a ~1 px font even when the painter transform scales it
//     up; the painter transforms are compensated so the device geometry is
//     unchanged.

#include "config.h"

#if defined(BUILD_QT) && !defined(MEM_CHECK)

#ifndef GRAPHIC_QT_H_INCLUDED
#define GRAPHIC_QT_H_INCLUDED

#include <string>

#include "common.h"
#include "graphic/graphic.h"

#include <QBrush>
#include <QFont>
#include <QMap>
#include <QPainter>
#include <QString>
#include <QTransform>

namespace tex {

// remove a null byte off end of QString
QString wstring_to_QString(const std::wstring& ws);

class Font_qt : public Font {

private:
  QFont _font;
  // cnote: pixel-like size on the fixed 96-DPI reference device; the QFont
  // itself stores the equivalent point size.
  float _size = 0.f;

  static QMap<QString, QString> _loaded_families;

public:

  Font_qt(const std::string& family = "", int style = PLAIN, float size = 1.f);

  Font_qt(const std::string& file, float size);

  std::string getFamily() const;

  int getStyle() const;

  QFont getQFont() const;

  virtual float getSize() const override;

  virtual sptr<Font> deriveFont(int style) const override;

  virtual bool operator==(const Font& f) const override;

  virtual bool operator!=(const Font& f) const override;

  virtual ~Font_qt() {};

};


/**************************************************************************************************/

class TextLayout_qt : public TextLayout {
private:
  QFont _font;
  QString _text;

public:
  TextLayout_qt(const std::wstring& src, const sptr<Font_qt>& font);

  virtual void getBounds(Rect& r) override;

  virtual void draw(Graphics2D& g2, float x, float y) override;
};

/**************************************************************************************************/

class Graphics2D_qt : public Graphics2D {
private:
  /*static*/ Font_qt _default_font = Font_qt("SansSerif", PLAIN, 20.f);

  QPainter* _painter;

  color _color;
  Stroke _stroke;
  const Font_qt* _font;
  float _sx, _sy;

  // cnote: painter transform captured at construction; reset() returns to it.
  QTransform _initial_transform;
  float _initial_sx = 1.f;
  float _initial_sy = 1.f;

  void setPen();
  QBrush getQBrush() const;

public:
  Graphics2D_qt(QPainter* painter);

  QPainter* getQPainter() const;

  // cnote: draws text at the device raster size; see the definition for why
  // MicroTeX's sub-pixel nominal font sizes cannot go through drawText directly.
  void drawFontText(const QFont& baseFont, const QString& text, float x, float y);

  virtual void setColor(color c) override;

  virtual color getColor() const override;

  virtual void setStroke(const Stroke& s) override;

  virtual const Stroke& getStroke() const override;

  virtual void setStrokeWidth(float w) override;

  virtual const Font* getFont() const override;

  virtual void setFont(const Font* font) override;

  virtual void translate(float dx, float dy) override;

  virtual void scale(float sx, float sy) override;

  virtual void rotate(float angle) override;

  virtual void rotate(float angle, float px, float py) override;

  virtual void reset() override;

  virtual float sx() const override;

  virtual float sy() const override;

  virtual void drawChar(wchar_t c, float x, float y) override;

  virtual void drawText(const std::wstring& t, float x, float y) override;

  virtual void drawLine(float x1, float y1, float x2, float y2) override;

  virtual void drawRect(float x, float y, float w, float h) override;

  virtual void fillRect(float x, float y, float w, float h) override;

  virtual void drawRoundRect(float x, float y, float w, float h, float rx, float ry) override;

  virtual void fillRoundRect(float x, float y, float w, float h, float rx, float ry) override;
};

}

#endif   // GRAPHIC_QT_H_INCLUDED
#endif   // defined(BUILD_QT) && !defined(MEM_CHECK)
