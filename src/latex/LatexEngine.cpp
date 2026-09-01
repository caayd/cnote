#include "latex/LatexEngine.h"

#include <algorithm>
#include <bit>
#include <cwchar>
#include <deque>

#include <objidl.h>
#include <gdiplus.h>

#include "latex.h"
#include "platform/gdi_win/graphic_win32.h"
#include "render.h"

namespace cnote::latex {

struct Formula {
    std::unique_ptr<tex::TeXRender> render;
    FormulaMetrics metrics;
};

namespace {

constexpr std::size_t kMaxCachedFormulae = 128;

struct CacheEntry {
    std::uint64_t key = 0;
    FormulaHandle formula;
};

std::deque<CacheEntry> g_cache;
bool g_initialized = false;

void HashValue(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
}

std::uint64_t FormulaKey(const std::wstring& source, float fontSize, bool display) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const wchar_t ch : source) {
        HashValue(hash, static_cast<std::uint16_t>(ch));
    }
    HashValue(hash, std::bit_cast<std::uint32_t>(fontSize));
    HashValue(hash, display ? 1ULL : 0ULL);
    return hash;
}

FormulaMetrics ReadMetrics(const tex::TeXRender& render) {
    FormulaMetrics metrics;
    metrics.width = static_cast<float>(render.getWidth());
    // MicroTeX's getHeight() is the total height (ascent + descent), verified
    // empirically: getBaseline() * getHeight() + getDepth() == getHeight().
    metrics.height = static_cast<float>(render.getHeight());
    metrics.depth = static_cast<float>(render.getDepth());
    metrics.baselineFraction = render.getBaseline();
    return metrics;
}

}  // namespace

bool Initialize(const std::wstring& resRoot) {
    if (g_initialized) {
        return true;
    }
    // LaTeX::init takes a UTF-8 path.
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, resRoot.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 1) {
        return false;
    }
    std::string utf8(static_cast<std::size_t>(utf8Length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, resRoot.c_str(), -1, utf8.data(), utf8Length, nullptr, nullptr);
    tex::LaTeX::init(utf8);
    g_initialized = true;
    return true;
}

void Shutdown() {
    if (!g_initialized) {
        return;
    }
    g_cache.clear();
    tex::LaTeX::release();
    g_initialized = false;
}

bool IsInitialized() {
    return g_initialized;
}

FormulaHandle Parse(const std::wstring& source, float fontSize, bool display) {
    if (!g_initialized || source.empty() || fontSize <= 0.0f) {
        return nullptr;
    }

    const std::uint64_t key = FormulaKey(source, fontSize, display);
    const auto existing = std::find_if(g_cache.begin(), g_cache.end(), [&](const CacheEntry& entry) {
        return entry.key == key;
    });
    if (existing != g_cache.end()) {
        FormulaHandle formula = existing->formula;
        g_cache.erase(existing);
        g_cache.push_back({ key, formula });
        return formula;
    }

    std::wstring effective = source;
    if (display
        && effective.compare(0, 2, L"$$") != 0
        && effective.compare(0, 2, L"\\[") != 0) {
        effective = L"\\[" + effective + L"\\]";
    }

    std::unique_ptr<tex::TeXRender> render;
    try {
        // width=0: unconstrained natural width; lineSpace tracks fontSize.
        render.reset(tex::LaTeX::parse(effective, 0, fontSize, fontSize, tex::black));
    } catch (const std::exception&) {
        return nullptr;
    }
    if (render == nullptr) {
        return nullptr;
    }

    auto formula = std::make_shared<Formula>();
    formula->render = std::move(render);
    formula->metrics = ReadMetrics(*formula->render);

    g_cache.push_back({ key, formula });
    if (g_cache.size() > kMaxCachedFormulae) {
        g_cache.pop_front();
    }
    return formula;
}

FormulaMetrics Measure(const FormulaHandle& formula) {
    return formula != nullptr ? formula->metrics : FormulaMetrics {};
}

FormulaHandle Parse(const PageLatex& latex) {
    return Parse(latex.content, latex.fontSize, true);
}

FormulaMetrics Measure(const PageLatex& latex) {
    return Measure(Parse(latex));
}

void Draw(const FormulaHandle& formula, Gdiplus::Graphics& graphics, float x, float y, float scale, std::uint32_t argb) {
    if (formula == nullptr || formula->render == nullptr || scale <= 0.0f) {
        return;
    }
    const Gdiplus::GraphicsState state = graphics.Save();
    // The Graphics2D_win32 constructor resets the transform, so it must wrap
    // the graphics before we apply ours.
    tex::Graphics2D_win32 g2(&graphics);
    graphics.TranslateTransform(x, y);
    graphics.ScaleTransform(scale, scale);
    formula->render->setForeground(argb);
    formula->render->draw(g2, 0, 0);
    // TeXRender::draw() ends with g2.reset(); restore our caller's state.
    graphics.Restore(state);
}

void Draw(const FormulaHandle& formula, HDC dc, float x, float y, float scale, std::uint32_t argb) {
    if (dc == nullptr) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    Draw(formula, graphics, x, y, scale, argb);
}

void Draw(const PageLatex& latex, HDC dc, float x, float y, float scale) {
    Draw(Parse(latex), dc, x, y, scale, latex.color);
}

}  // namespace cnote::latex
