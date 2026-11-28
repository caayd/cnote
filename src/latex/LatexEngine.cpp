#include "latex/LatexEngine.h"

#include <algorithm>
#include <cwctype>
#include <deque>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>

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
constexpr std::size_t kMaxValidatedSources = 256;
constexpr const char* kResourceMarker = ".clatexmath-res_root";

// Explicit lifecycle: Ready is the only state in which Parse may run. A
// failed Initialize is sticky until the next Initialize (which may succeed),
// so callers can never mistake "not ready yet" for "parsed to nothing".
enum class EngineState { Uninitialized, Ready, Failed };

struct CacheEntry {
    std::u16string source;
    std::uint64_t sourceHash = 0;
    float fontSize = 0.0f;
    bool display = false;
    std::uint64_t generation = 0;
    FormulaHandle formula;
};

// Structural validation is a pure function of the source, so its result is
// cached per source: repeated looks at an invalid formula (a half-typed
// `$...$`, a typo) never widen and scan the source again.
struct ValidationEntry {
    std::u16string source;
    std::uint64_t sourceHash = 0;
    bool valid = false;
};

std::deque<CacheEntry> g_cache;
std::deque<ValidationEntry> g_validationCache;
EngineState g_state = EngineState::Uninitialized;
// Advanced on every Initialize attempt and every Shutdown. Caches include it
// so output produced by another engine lifetime is never reused.
std::uint64_t g_generation = 0;

// MicroTeX's parser is code-unit oriented: each model UTF-16 unit maps to one
// wchar. Never reinterpret surrogate units as scalar values. QString::fromUtf16
// preserves every code unit; toStdWString only widens them.
std::wstring WideFromUtf16(std::u16string_view value) {
    return QString::fromUtf16(value.data(), static_cast<qsizetype>(value.size())).toStdWString();
}

std::uint64_t HashSource(std::u16string_view source) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char16_t unit : source) {
        hash ^= static_cast<std::uint16_t>(unit);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool Matches(const CacheEntry& entry, std::u16string_view source, std::uint64_t sourceHash, float fontSize,
    bool display) {
    return entry.generation == g_generation
        && entry.display == display
        && entry.fontSize == fontSize
        && entry.sourceHash == sourceHash
        && std::u16string_view(entry.source) == source;
}

// The resource root is a directory carrying MicroTeX's marker file. Checking
// it before tex::LaTeX::init prevents a silent "initialized" state on a tree
// MicroTeX could not use.
bool HasResourceRoot(const std::filesystem::path& resRoot) {
    if (resRoot.empty()) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(resRoot, error) || error) {
        return false;
    }
    return std::filesystem::exists(resRoot / kResourceMarker, error) && !error;
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

// Structural validity of `source`, consulting the bounded per-source cache so
// a repeated look at an invalid formula never widens or scans it again.
bool SourceIsValid(std::u16string_view source, std::uint64_t sourceHash) {
    const auto existing = std::find_if(g_validationCache.begin(), g_validationCache.end(),
        [&](const ValidationEntry& entry) {
            return entry.sourceHash == sourceHash && std::u16string_view(entry.source) == source;
        });
    if (existing != g_validationCache.end()) {
        ValidationEntry entry = std::move(*existing);
        g_validationCache.erase(existing);
        g_validationCache.push_back(std::move(entry));
        return g_validationCache.back().valid;
    }

    const std::wstring wide = WideFromUtf16(source);
    const bool valid = !HasUnmatchedClosingGroup(wide) && !HasEmptyFractionArgument(wide);
    g_validationCache.push_back({ std::u16string(source), sourceHash, valid });
    if (g_validationCache.size() > kMaxValidatedSources) {
        g_validationCache.pop_front();
    }
    return valid;
}


}  // namespace

bool Initialize(const std::filesystem::path& resRoot) {
    // Every attempt starts a new engine lifetime: cached formulae and layouts
    // keyed to the previous generation must not survive it.
    g_cache.clear();
    g_validationCache.clear();
    ++g_generation;

    if (!HasResourceRoot(resRoot)) {
        g_state = EngineState::Failed;
        return false;
    }
    // tex::LaTeX::init takes a UTF-8 path and locates the resource tree through
    // the `.clatexmath-res_root` marker inside that directory. u8string() is the
    // portable wide/native-to-UTF-8 conversion on every supported platform.
    const std::u8string utf8 = resRoot.u8string();
    try {
        tex::LaTeX::init(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    } catch (const std::exception&) {
        g_state = EngineState::Failed;
        return false;
    }
    g_state = EngineState::Ready;
    return true;
}

void Shutdown() {
    g_cache.clear();
    g_validationCache.clear();
    ++g_generation;
    // MicroTeX's LaTeX::release() frees its formatter but leaves the static
    // pointers non-null, so the next LaTeX::init() skips initialization and
    // parse() dereferences freed memory. Keep the backend for the process
    // instead; Shutdown only makes it unavailable and invalidates every cache
    // keyed to its generation.
    g_state = EngineState::Uninitialized;
}

bool IsInitialized() {
    return g_state == EngineState::Ready;
}

std::uint64_t EngineGeneration() {
    return g_generation;
}

FormulaHandle Parse(std::u16string_view source, float fontSize, bool display) {
    if (g_state != EngineState::Ready
        || source.empty()
        || fontSize <= 0.0f) {
        return nullptr;
    }
    const std::uint64_t sourceHash = HashSource(source);
    // Keyed on the original UTF-16 source, so a hit never widens it or runs
    // the structural scans again.
    const auto existing = std::find_if(g_cache.begin(), g_cache.end(), [&](const CacheEntry& entry) {
        return Matches(entry, source, sourceHash, fontSize, display);
    });
    if (existing != g_cache.end()) {
        CacheEntry entry = std::move(*existing);
        g_cache.erase(existing);
        g_cache.push_back(std::move(entry));
        return g_cache.back().formula;
    }

    if (!SourceIsValid(source, sourceHash)) {
        return nullptr;
    }

    const std::wstring wideSource = WideFromUtf16(source);
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

    g_cache.push_back({ std::u16string(source), sourceHash, fontSize, display, g_generation, formula });
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
