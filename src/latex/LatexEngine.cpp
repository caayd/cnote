#include "latex/LatexEngine.h"

#include <algorithm>
#include <bit>
#include <cwctype>
#include <deque>
#include <string>
#include <string_view>

#include <QString>

#include "latex.h"
#include "render.h"

// Backend-independent MicroTeX core: lifetime, LRU formula cache, source
// validation and natural metrics.  Drawing lives in the platform translation
// unit (LatexEngineQt.cpp); see LatexEngine.h.

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

// MicroTeX's parser is code-unit oriented: each model UTF-16 unit maps to one
// wchar. Never reinterpret surrogate units as scalar values. QString::fromUtf16
// preserves every code unit; toStdWString only widens them.
std::wstring WideFromUtf16(std::u16string_view value) {
    return QString::fromUtf16(value.data(), static_cast<qsizetype>(value.size())).toStdWString();
}

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

bool HasUnmatchedClosingGroup(const std::wstring& source) {
    int groupDepth = 0;
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source[index] == L'\\' && index + 1U < source.size()) {
            ++index;
            continue;
        }
        if (source[index] == L'{') {
            ++groupDepth;
        } else if (source[index] == L'}') {
            if (groupDepth == 0) {
                return true;
            }
            --groupDepth;
        }
    }
    return false;
}
bool HasEmptyFractionArgument(const std::wstring& source) {
    constexpr std::wstring_view commands[] = { L"\\frac", L"\\dfrac" };
    for (const std::wstring_view command : commands) {
        std::size_t searchPosition = 0;
        while ((searchPosition = source.find(command, searchPosition)) != std::wstring::npos) {
            std::size_t argumentPosition = searchPosition + command.size();
            while (argumentPosition < source.size() && std::iswspace(source[argumentPosition])) {
                ++argumentPosition;
            }
            for (int argument = 0; argument < 2 && argumentPosition < source.size(); ++argument) {
                if (source[argumentPosition] != L'{') {
                    break;
                }
                if (argumentPosition + 1U < source.size() && source[argumentPosition + 1U] == L'}') {
                    return true;
                }
                int depth = 0;
                std::size_t endPosition = argumentPosition;
                for (; endPosition < source.size(); ++endPosition) {
                    if (source[endPosition] == L'\\' && endPosition + 1U < source.size()) {
                        ++endPosition;
                        continue;
                    }
                    if (source[endPosition] == L'{') {
                        ++depth;
                    } else if (source[endPosition] == L'}' && --depth == 0) {
                        break;
                    }
                }
                if (endPosition >= source.size()) {
                    break;
                }
                argumentPosition = endPosition + 1U;
                while (argumentPosition < source.size() && std::iswspace(source[argumentPosition])) {
                    ++argumentPosition;
                }
            }
            searchPosition += command.size();
        }
    }
    return false;
}


}  // namespace

bool Initialize(const std::filesystem::path& resRoot) {
    if (g_initialized) {
        return true;
    }
    // tex::LaTeX::init takes a UTF-8 path and locates the resource tree through
    // the `.clatexmath-res_root` marker inside that directory. u8string() is the
    // portable wide/native-to-UTF-8 conversion on every supported platform.
    const std::u8string utf8 = resRoot.u8string();
    if (utf8.empty()) {
        return false;
    }
    tex::LaTeX::init(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
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

FormulaHandle Parse(std::u16string_view source, float fontSize, bool display) {
    if (!g_initialized
        || source.empty()
        || fontSize <= 0.0f) {
        return nullptr;
    }
    const std::wstring wideSource = WideFromUtf16(source);
    if (HasUnmatchedClosingGroup(wideSource)
        || HasEmptyFractionArgument(wideSource)) {
        return nullptr;
    }
    const std::uint64_t key = FormulaKey(wideSource, fontSize, display);
    const auto existing = std::find_if(g_cache.begin(), g_cache.end(), [&](const CacheEntry& entry) {
        return entry.key == key;
    });
    if (existing != g_cache.end()) {
        FormulaHandle formula = existing->formula;
        g_cache.erase(existing);
        g_cache.push_back({ key, formula });
        return formula;
    }

    std::wstring effective = wideSource;
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

namespace detail {

void DrawRender(const FormulaHandle& formula, tex::Graphics2D& graphics, std::uint32_t argb) {
    if (formula == nullptr || formula->render == nullptr) {
        return;
    }
    formula->render->setForeground(argb);
    formula->render->draw(graphics, 0, 0);
}

}  // namespace detail

}  // namespace cnote::latex
