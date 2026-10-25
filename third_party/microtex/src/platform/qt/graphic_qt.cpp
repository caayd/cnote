// Vendored MicroTeX Qt platform adapter.
//
// Taken from NanoMichael/MicroTeX @ 0e3707f6 (MIT) at
//   src/platform/qt/graphic_qt.cpp
// and adapted locally for cnote; every local change is marked "cnote:". See
// graphic_qt.h for the list of deviations and the pinned revision.

#include "graphic_qt.h"

#include "config.h"

#if defined(BUILD_QT) && !defined(MEM_CHECK)

#include "platform/qt/graphic_qt.h"

#include <QDebug>

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QtMath>

#include <algorithm>
#include <cmath>

using namespace tex;
using namespace std;

QMap<QString, QString> Font_qt::_loaded_families;

namespace {

// cnote: MicroTeX sizes its fonts in pixels on a fixed 96-DPI reference device
// (FontInfo::getFont() passes Formula::PIXELS_PER_POINT, which is 1 px unless a
// DPI target is set, to Font::create).  QFont stores points, so convert
// explicitly and keep the fractional value; page zoom and the device pixel
// ratio are applied by the caller's painter transform, never here.
constexpr float kReferenceDpi = 96.f;
constexpr float kPointsPerInch = 72.f;

inline float CnotePointSizeF(float pixelSize) {
  return pixelSize * kPointsPerInch / kReferenceDpi;
}

// cnote: scale factors of a painter transform, used for MicroTeX's sx()/sy().
inline float CnoteScaleX(const QTransform& transform) {
  return static_cast<float>(std::hypot(transform.m11(), transform.m12()));
}

inline float CnoteScaleY(const QTransform& transform) {
  return static_cast<float>(std::hypot(transform.m21(), transform.m22()));
}

}  // namespace

namespace tex {
// cnote: MicroTeX's std::wstring is code-unit oriented on every platform, so
// map one 16-bit unit to one QChar explicitly.  QString::fromStdWString would
// reinterpret surrogate units as Unicode scalar values where wchar_t is 32-bit.
// Some wstrings arrive with a \0 at end, so we remove it when converting.
QString wstring_to_QString(const std::wstring& ws)
{
  QString out;
  out.reserve(static_cast<int>(ws.size()));
  for (const wchar_t unit : ws) {
    out.append(static_cast<QChar>(static_cast<char16_t>(static_cast<std::uint16_t>(unit))));
  }
  auto index = out.indexOf(QChar(u'\0'));
  if (index != -1)
    out.truncate(index);
  return out;
}
}

Font_qt::Font_qt(const string& family, int style, float size) {
  _size = size;
  _font.setFamily(QString::fromStdString(family));
  // cnote: point size on the 96-DPI reference device, fractional value kept.
  _font.setPointSizeF(CnotePointSizeF(size));

  _font.setBold(style & BOLD);
  _font.setItalic(style & ITALIC);
}

Font_qt::Font_qt(const string& file, float size)
{
  // set size for newly loaded and previously loaded font
  _size = size;
  // cnote: point size on the 96-DPI reference device, fractional value kept.
  _font.setPointSizeF(CnotePointSizeF(size));

  QString filename(QString::fromStdString(file));
  if(_loaded_families.contains(filename)) {
    // file already loaded
    _font.setFamily(_loaded_families.value(filename));
    return;
  }

  // cnote: the MicroTeX resources (fonts, XML) ship as real files next to the
  // executable, so there is no Qt-resource fallback, and a font that cannot be
  // loaded is a hard error rather than silently wrong glyphs.
  int id = QFontDatabase::addApplicationFont(filename);
  if( id == -1 ) {
    throw ex_invalid_state("cannot load font file " + file);
  }
  QStringList families = QFontDatabase::applicationFontFamilies(id);
  if( families.size() == 0 ) {
    throw ex_invalid_state("no font families found in file " + file);
  }
  _loaded_families[filename] = families.first();
  _font.setFamily(families.first());
}

string Font_qt::getFamily() const {
  return _font.family().toStdString();
}

int Font_qt::getStyle() const {
  int out = PLAIN;
  if(_font.bold())   out |= BOLD;
  if(_font.italic()) out |= ITALIC;
  return out;
}

QFont Font_qt::getQFont() const {
  return _font;
}

float Font_qt::getSize() const {
  // cnote: MicroTeX's font bookkeeping is in reference-device pixels.
  return _size;
}

sptr<Font> Font_qt::deriveFont(int style) const {
  return sptrOf<Font_qt>(getFamily(), style, getSize());
}

bool Font_qt::operator==(const Font& ft) const {
  const Font_qt& o = static_cast<const Font_qt&>(ft);

  return getFamily()==o.getFamily() && getSize()==o.getSize() &&
    getStyle()==o.getStyle();
}

bool Font_qt::operator!=(const Font& ft) const {
  return !(*this == ft);
}

Font* Font::create(const string& file, float size) {
  return new Font_qt(file, size);
}

sptr<Font> Font::_create(const string& name, int style, float size) {
  return sptrOf<Font_qt>(name, style, size);
}

/**************************************************************************************************/

TextLayout_qt::TextLayout_qt(const std::wstring& src, const sptr<Font_qt>& f) :
  _font(f->getQFont()),
  _text(wstring_to_QString(src))
{
}

void TextLayout_qt::getBounds(Rect& r) {
  QFontMetricsF fm(_font);
  QRectF br(fm.boundingRect(_text));

  r.x = br.left();
  r.y = br.top();
  r.w = br.width();
  r.h = br.height();
}

void TextLayout_qt::draw(Graphics2D& g2, float x, float y) {
  Graphics2D_qt& g = static_cast<Graphics2D_qt&>(g2);
  g.drawFontText(_font, _text, x, y);
}

sptr<TextLayout> TextLayout::create(const std::wstring& src, const sptr<Font>& font) {
  sptr<Font_qt> f = static_pointer_cast<Font_qt>(font);
  return sptrOf<TextLayout_qt>(src, f);
}

/**************************************************************************************************/

//Font_qt Graphics2D_qt::_default_font("SansSerif", PLAIN, 20.f);

Graphics2D_qt::Graphics2D_qt(QPainter* painter)
    : _painter(painter),
      _initial_transform(painter->transform()) {
  // cnote: the caller may hand us a painter that already carries its page/DPR
  // transform (see cnote::latex::Draw).  Remember that transform, so reset()
  // (which MicroTeX calls at the end of TeXRender::draw()) restores it.  The
  // scale comes from the device transform: a widget painter carries its
  // device pixel ratio there rather than in the world transform, and glyphs
  // must rasterize at the same device size whichever way the DPR arrives
  // (the text editor vs. the committed page surface).  It also keeps the
  // 1/sx(), 1/sy() hairline helpers device-relative.
  _initial_sx = CnoteScaleX(painter->deviceTransform());
  _initial_sy = CnoteScaleY(painter->deviceTransform());
  _sx = _initial_sx;
  _sy = _initial_sy;
  setColor(BLACK);
  setStroke(Stroke());
  setFont(&_default_font);
}

QPainter* Graphics2D_qt::getQPainter() const {
  return _painter;
}

QBrush Graphics2D_qt::getQBrush() const {
  return QBrush(QColor(color_r(_color), color_g(_color),
                       color_b(_color), color_a(_color)));
}

void Graphics2D_qt::setPen() {

  QBrush brush(getQBrush());

  Qt::PenCapStyle cap;
  switch (_stroke.cap) {
  case CAP_ROUND:
    cap = Qt::RoundCap;
    break;
  case CAP_SQUARE:
    cap = Qt::SquareCap;
    break;
  case CAP_BUTT:
  default:
    cap = Qt::FlatCap;
    break;
  }

  Qt::PenJoinStyle join;
  switch (_stroke.join) {
  case JOIN_BEVEL:
    join = Qt::BevelJoin;
    break;
  case JOIN_ROUND:
    join = Qt::RoundJoin;
    break;
  case JOIN_MITER:
  default:
    join = Qt::MiterJoin;
    break;
  }

  QPen pen(brush, _stroke.lineWidth, Qt::SolidLine, cap, join);
  pen.setMiterLimit(_stroke.miterLimit);
  _painter->setPen(pen);
}

void Graphics2D_qt::setColor(color c) {
  _color = c;
  setPen();
}

color Graphics2D_qt::getColor() const {
  return _color;
}

void Graphics2D_qt::setStroke(const Stroke& s) {
  _stroke = s;
  setPen();
}

const Stroke& Graphics2D_qt::getStroke() const {
  return _stroke;
}

void Graphics2D_qt::setStrokeWidth(float w) {
  _stroke.lineWidth = w;
  setPen();
}

const Font* Graphics2D_qt::getFont() const {
  return _font;
}

void Graphics2D_qt::setFont(const Font* font) {
  _font = static_cast<const Font_qt*>(font);
}

void Graphics2D_qt::translate(float dx, float dy) {
  _painter->translate(dx, dy);
}

void Graphics2D_qt::scale(float sx, float sy) {
  _sx *= sx;
  _sy *= sy;
  _painter->scale(sx, sy);
}

void Graphics2D_qt::rotate(float angle) {
  _painter->rotate(qRadiansToDegrees(angle));
}

void Graphics2D_qt::rotate(float angle, float px, float py) {
  _painter->translate(px, py);
  _painter->rotate(qRadiansToDegrees(angle));
  _painter->translate(-px, -py);
}

void Graphics2D_qt::reset() {
  // cnote: restore the transform captured at construction; MicroTeX ends
  // TeXRender::draw() with g2.reset() and must not erase the caller's
  // page/DPR transform.
  _painter->setTransform(_initial_transform);
  _sx = _initial_sx;
  _sy = _initial_sy;
}

float Graphics2D_qt::sx() const {
  return _sx;
}

float Graphics2D_qt::sy() const {
  return _sy;
}

void Graphics2D_qt::drawChar(wchar_t c, float x, float y) {
  std::wstring str = {c};
  drawText(str, x, y);
}

void Graphics2D_qt::drawFontText(const QFont& baseFont, const QString& text, float x, float y) {
  if (text.isEmpty()) {
    return;
  }
  // cnote: MicroTeX creates every font at PIXELS_PER_POINT - 1 px on the 96-DPI
  // reference device - and scales glyphs through the painter transform.  Qt's
  // glyph rasteriser becomes unreliable for such sub-pixel nominal sizes: at
  // several ordinary zoom/scale combinations it draws no glyph at all, while
  // the fraction rule next to it still paints.  Rasterise the glyph at the
  // device size instead and undo the extra scale in the painter, so the device
  // geometry is unchanged (one reference-device pixel times the painter scale)
  // but the font Qt sees is legible.
  const float scale = std::max(std::abs(_sx), std::abs(_sy));
  if (!(scale > 0.0f)) {
    return;
  }
  QFont deviceFont = baseFont;
  deviceFont.setPointSizeF(baseFont.pointSizeF() * scale);
  const QTransform savedTransform = _painter->transform();
  const QFont savedFont = _painter->font();
  _painter->setFont(deviceFont);
  _painter->translate(x, y);
  _painter->scale(1.0 / scale, 1.0 / scale);
  _painter->drawText(QPointF(0.0, 0.0), text);
  _painter->setTransform(savedTransform);
  _painter->setFont(savedFont);
}

void Graphics2D_qt::drawText(const std::wstring& t, float x, float y) {
  drawFontText(_font->getQFont(), wstring_to_QString(t), x, y);
}

void Graphics2D_qt::drawLine(float x1, float y1, float x2, float y2) {
  _painter->drawLine(QPointF(x1, y1), QPointF(x2, y2));
}

void Graphics2D_qt::drawRect(float x, float y, float w, float h) {
  _painter->drawRect(QRectF(x, y, w, h));
}

void Graphics2D_qt::fillRect(float x, float y, float w, float h) {
  _painter->fillRect(QRectF(x, y, w, h), getQBrush());
}

void Graphics2D_qt::drawRoundRect(float x, float y, float w, float h, float rx, float ry) {
  _painter->drawRoundedRect(QRectF(x, y, w, h), rx, ry);
}

void Graphics2D_qt::fillRoundRect(float x, float y, float w, float h, float rx, float ry) {
  _painter->setPen(QPen(Qt::NoPen));
  _painter->setBrush(getQBrush());

  _painter->drawRoundedRect(QRectF(x, y, w, h), rx, ry);

  setPen();
  _painter->setBrush(QBrush());
}


/**************************************************************************************************/


#endif
