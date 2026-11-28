// Font-family fallback on systems without the reference Windows fonts
// (migration plan 3.7 and the real-desktop gates).
//
// The plan allows font-driven wrapping and rasterization differences between
// platforms, but it forbids dropping spans, clipping text or changing page
// placement.  On Linux neither "Segoe UI", "Times New Roman" nor "Consolas" is
// installed, so every one of those behaviors goes through Qt's style-hint
// substitution; on Windows the requested families exist and are used directly.
// Both cases must lay out every paragraph and every character, keep styling
// effective, wrap inside the requested width and produce a non-empty measured
// box.  The measured metrics are printed so the Windows/Linux difference can be
// reviewed against the plan's allowance.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QStringList>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "text/TextEditingQt.h"
#include "text/TextLayoutQt.h"

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& message) {
    if (condition) {
        std::printf("ok   %s\n", message.c_str());
    } else {
        std::printf("FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

struct FamilyCase {
    cnote::TextFontFamily family;
    const char* requested;
    bool monospace;
};

constexpr FamilyCase kFamilies[] = {
    { cnote::TextFontFamily::Sans, "Segoe UI", false },
    { cnote::TextFontFamily::Serif, "Times New Roman", false },
    { cnote::TextFontFamily::Monospace, "Consolas", true },
    // Bundled (registered from the Qt resource on first use), so it resolves
    // to the real family on every platform.
    { cnote::TextFontFamily::ShantellSans, "Shantell Sans", false },
};

cnote::PageTextStyle StyleFor(cnote::TextFontFamily family, float fontSize) {
    cnote::PageTextStyle style {};
    style.fontFamily = family;
    style.fontSize = fontSize;
    return style;
}

// Three paragraphs, one per family, plus a non-Latin/emoji run that no single
// font contains; the style span makes the first paragraph measurably taller.
cnote::PageText BuildStyledText() {
    cnote::PageText text {};
    text.id = 1;
    text.x = 30.0f;
    text.y = 40.0f;
    text.width = 300.0f;  // stored object width: the explicit wrap width
    text.fontSize = 15.0f;
    text.alignment = cnote::TextAlignment::Center;
    text.content =
        u"Sans family paragraph with a styled span \x65E5\x672C\x8A9E \x03B1\x03B2\x03B3"
        u"\nSerif family paragraph"
        u"\nMonospace family paragraph";
    cnote::PageTextStyleSpan span {};
    span.start = 0;
    span.length = 41;  // "Sans family paragraph with a styled span"
    span.style = StyleFor(cnote::TextFontFamily::Sans, 30.0f);
    text.styleSpans.push_back(span);
    return text;
}

std::size_t LineCount(const cnote::text::QtTextLayout& layout) {
    return layout.lines.size();
}

// Every non-whitespace code unit must fall inside exactly one laid-out line.
bool CoversAllContent(const cnote::PageText& text, const cnote::text::QtTextLayout& layout) {
    std::size_t line = 0;
    for (std::size_t index = 0; index < text.content.size(); ++index) {
        const char16_t unit = text.content[index];
        if (unit == u'\n' || unit == u' ') {
            continue;
        }
        while (line < layout.lines.size()
               && index >= static_cast<std::size_t>(layout.lines[line].start) + layout.lines[line].text.size()) {
            ++line;
        }
        if (line >= layout.lines.size() || index < layout.lines[line].start) {
            return false;
        }
    }
    return true;
}

void CheckFamilyResolution() {
    for (const FamilyCase& entry : kFamilies) {
        const QFont font = cnote::text::FontForStyle(StyleFor(entry.family, 15.0f));
        const QFontInfo info(font);
        const QFontMetricsF metrics(font);
        const bool installed = QFontDatabase::families().contains(QString::fromUtf8(entry.requested));

        std::printf("     %-14s requested=%-16s resolved=%-24s lineHeight=%.1f advance(\"Hamburgefonstiv\")=%.1f installed=%d\n",
            entry.family == cnote::TextFontFamily::Sans ? "Sans"
                : entry.family == cnote::TextFontFamily::Serif ? "Serif"
                : entry.family == cnote::TextFontFamily::Monospace ? "Monospace" : "Shantell Sans",
            entry.requested, info.family().toUtf8().constData(), metrics.height(),
            metrics.horizontalAdvance(QStringLiteral("Hamburgefonstiv")), installed ? 1 : 0);

        Check(!info.family().isEmpty(), std::string(entry.requested) + ": Qt resolved a family");
        Check(QFontDatabase::families().contains(info.family()),
            std::string(entry.requested) + ": the resolved family is installed");
        Check(metrics.height() > 0.0 && metrics.horizontalAdvance(QStringLiteral("Hamburgefonstiv")) > 0.0,
            std::string(entry.requested) + ": the resolved font has real metrics");
        if (entry.monospace) {
            Check(info.fixedPitch(), "the Monospace style resolves to a fixed-pitch font");
        }
        if (installed) {
            Check(info.family() == QString::fromUtf8(entry.requested),
                std::string(entry.requested) + ": an installed requested family is used directly");
        } else {
            Check(info.family() != QString::fromUtf8(entry.requested),
                std::string(entry.requested) + ": a missing family is substituted, not claimed");
        }
    }
}

void CheckLayoutKeepsEverything() {
    const cnote::PageText text = BuildStyledText();
    const cnote::text::QtTextLayout layout = cnote::text::LayoutPageTextQt(text, 300.0f);

    Check(LineCount(layout) >= 3U, "every paragraph is laid out on a line");
    Check(layout.lineHeight > 0 && layout.height > 0, "the layout reports a positive line height");
    Check(layout.height >= layout.lineHeight, "the layout is at least one line tall");
    Check(CoversAllContent(text, layout), "no character of the content is dropped");
    Check(layout.lines.front().start == 0U, "the first line starts at the first character");
    Check(layout.lines.back().start + layout.lines.back().text.size() <= text.content.size(),
        "the last line stays inside the content");

    const cnote::text::QtTextBounds bounds = cnote::text::MeasurePageTextBoundsQt(text, 300.0f);
    std::printf("     measured bounds: left=%.1f top=%.1f right=%.1f bottom=%.1f (lineHeight=%.2f lines=%zu)\n",
        bounds.left, bounds.top, bounds.right, bounds.bottom, layout.lineHeight, LineCount(layout));
    Check(bounds.right > bounds.left && bounds.bottom > bounds.top, "the measured box is non-empty");
    Check(bounds.right - bounds.left <= text.width + 1.0f,
        "the measured box stays inside the stored wrap width");
    Check(bounds.left >= text.x - 0.5f && bounds.right <= text.x + text.width + 1.0f,
        "the laid-out text stays inside the object box");
    Check(bounds.bottom - bounds.top >= static_cast<float>(layout.lineHeight), "the box covers the laid-out text height");

    // Text-only layout with no style spans must remain sane as well.
    cnote::PageText plain = text;
    plain.styleSpans.clear();
    const cnote::text::QtTextLayout plainLayout = cnote::text::LayoutPageTextQt(plain, 300.0f);
    Check(plainLayout.lineHeight > 0 && LineCount(plainLayout) >= 3U, "an unstyled paragraph set lays out too");
    Check(layout.lineHeight > plainLayout.lineHeight,
        "the 30px style span still reaches the layout (span styling is applied)");
    Check(cnote::text::TextLayoutSignature(text) != cnote::text::TextLayoutSignature(plain),
        "styling is part of the layout identity");
}

void CheckWrappingAndDeterminism() {
    cnote::PageText wideText = BuildStyledText();
    wideText.width = 400.0f;
    cnote::PageText narrowText = BuildStyledText();
    narrowText.width = 140.0f;
    const cnote::text::QtTextLayout wide = cnote::text::LayoutPageTextQt(wideText, 400.0f);
    const cnote::text::QtTextLayout narrow = cnote::text::LayoutPageTextQt(narrowText, 400.0f);
    Check(LineCount(narrow) >= LineCount(wide), "a narrower wrap width never uses fewer lines");
    Check(LineCount(narrow) > LineCount(wide), "the long paragraph wraps at the narrower stored width");

    const cnote::text::QtTextBounds narrowBounds = cnote::text::MeasurePageTextBoundsQt(narrowText, 400.0f);
    Check(narrowBounds.right - narrowBounds.left <= narrowText.width + 1.0f,
        "the narrow layout stays inside its stored width");
    Check(narrowBounds.right <= 400.0f, "the laid-out text stays on the page");

    const cnote::text::QtTextLayout repeat = cnote::text::LayoutPageTextQt(narrowText, 400.0f);
    Check(LineCount(repeat) == LineCount(narrow) && repeat.lineHeight == narrow.lineHeight
              && repeat.lines.front().text == narrow.lines.front().text,
        "identical input and width produce an identical layout");
}

// Text layouts embed inline-formula geometry, so the process-global cache must
// not serve a layout built in another engine lifetime.  This test needs no
// resource root: a failed Initialize and Shutdown both advance the generation.
void CheckLayoutCacheTracksEngineGeneration() {
    cnote::PageText text = BuildStyledText();
    text.width = 260.0f;
    constexpr float pageWidth = 260.0f;

    const std::uint64_t missesBefore = cnote::text::TextLayoutCacheMisses();
    const cnote::text::QtTextLayout first = cnote::text::LayoutPageTextQt(text, pageWidth);
    Check(cnote::text::TextLayoutCacheMisses() == missesBefore + 1U, "a fresh paragraph layout is a cache miss");
    const cnote::text::QtTextLayout cached = cnote::text::LayoutPageTextQt(text, pageWidth);
    Check(cnote::text::TextLayoutCacheMisses() == missesBefore + 1U
            && cached.lines.size() == first.lines.size(),
        "an unchanged layout is served from the cache");

    const std::uint64_t generation = cnote::latex::EngineGeneration();
    Check(!cnote::latex::Initialize(std::filesystem::path("cnote-missing-latex-res-root")),
        "a missing resource root fails initialization");
    Check(cnote::latex::EngineGeneration() > generation, "a failed initialization advances the engine generation");
    Check(cnote::text::LayoutPageTextQt(text, pageWidth).lines.size() == first.lines.size()
            && cnote::text::TextLayoutCacheMisses() == missesBefore + 2U,
        "a failed initialization invalidates layouts built before it");

    const std::uint64_t generationBeforeShutdown = cnote::latex::EngineGeneration();
    cnote::latex::Shutdown();
    Check(!cnote::latex::IsInitialized(), "Shutdown leaves the engine uninitialized");
    Check(cnote::latex::EngineGeneration() > generationBeforeShutdown, "Shutdown advances the engine generation");
    Check(cnote::text::LayoutPageTextQt(text, pageWidth).lines.size() == first.lines.size()
            && cnote::text::TextLayoutCacheMisses() == missesBefore + 3U,
        "Shutdown invalidates layouts built before it");
}

// --- measurement cache: a bounds hit builds no documents ------------------
void CheckBoundsHitBuildsNoDocuments() {
    cnote::PageText autoText = BuildStyledText();
    autoText.width = 0.0f;  // auto width: the first query must measure the content
    const std::uint64_t documentsBefore = cnote::text::TextDocumentBuilds();
    const cnote::text::QtTextBounds first = cnote::text::MeasurePageTextBoundsQt(autoText, 300.0f);
    Check(cnote::text::TextDocumentBuilds() > documentsBefore,
        "a first auto-width bounds query builds its measuring documents");
    const std::uint64_t documentsAfterFirst = cnote::text::TextDocumentBuilds();

    const cnote::text::QtTextBounds second = cnote::text::MeasurePageTextBoundsQt(autoText, 300.0f);
    Check(cnote::text::TextDocumentBuilds() == documentsAfterFirst,
        "a cached auto-width bounds query builds no documents");
    Check(first.left == second.left && first.top == second.top && first.right == second.right
            && first.bottom == second.bottom,
        "the cached bounds are identical");
    Check(cnote::text::LayoutPageTextQt(autoText, 300.0f).width == first.right - first.left,
        "the cached layout carries the resolved auto width");

    // A different page width is not a different layout: the key must not
    // include a dimension the layout ignores.
    const cnote::text::QtTextBounds otherPageWidth = cnote::text::MeasurePageTextBoundsQt(autoText, 123.0f);
    Check(cnote::text::TextDocumentBuilds() == documentsAfterFirst
            && otherPageWidth.right == second.right && otherPageWidth.bottom == second.bottom,
        "the page width is not part of the measurement key");

    // Fixed-width text has no measuring document at all, and a hit adds none.
    cnote::PageText fixed = BuildStyledText();
    fixed.width = 280.0f;  // a width no earlier check laid out
    const std::uint64_t documentsBeforeFixed = cnote::text::TextDocumentBuilds();
    static_cast<void>(cnote::text::MeasurePageTextBoundsQt(fixed, 300.0f));
    const std::uint64_t documentsAfterFirstFixed = cnote::text::TextDocumentBuilds();
    static_cast<void>(cnote::text::MeasurePageTextBoundsQt(fixed, 300.0f));
    Check(cnote::text::TextDocumentBuilds() == documentsAfterFirstFixed
            && documentsAfterFirstFixed - documentsBeforeFixed == 1U,
        "fixed-width bounds build one layout document and a hit adds none");
}

// --- a dense page must fit the measurement caches -------------------------
// Text is measured and drawn object by object, so a page with more text boxes
// than the cache holds turns a second pass into a cyclic miss.  200 boxes is
// a deliberately dense page; both passes must leave every later query cached.
void CheckDensePageBoundsHitNoDocuments() {
    std::vector<cnote::PageText> texts;
    texts.reserve(200);
    for (int index = 0; index < 200; ++index) {
        cnote::PageText text = BuildStyledText();
        text.id = 1000U + static_cast<std::uint64_t>(index);
        text.x = static_cast<float>(index % 20) * 12.0f;
        text.y = static_cast<float>(index / 20) * 40.0f;
        // Mix auto-width (resolved) and fixed-width (never measured) boxes.
        text.width = index % 2 == 0 ? 0.0f : 260.0f;
        text.content.push_back(static_cast<char16_t>(0x4E00 + index));
        texts.push_back(std::move(text));
    }

    const std::uint64_t documentsBefore = cnote::text::TextDocumentBuilds();
    for (const cnote::PageText& text : texts) {
        static_cast<void>(cnote::text::MeasurePageTextBoundsQt(text, 400.0f));
    }
    const std::uint64_t documentsAfterCold = cnote::text::TextDocumentBuilds();
    Check(documentsAfterCold > documentsBefore, "a 200-box page builds layouts on its first pass");

    for (const cnote::PageText& text : texts) {
        static_cast<void>(cnote::text::MeasurePageTextBoundsQt(text, 400.0f));
    }
    Check(cnote::text::TextDocumentBuilds() == documentsAfterCold,
        "a 200-box page's second bounds pass builds no documents");
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    CheckFamilyResolution();
    CheckLayoutKeepsEverything();
    CheckWrappingAndDeterminism();
    CheckBoundsHitBuildsNoDocuments();
    CheckDensePageBoundsHitNoDocuments();
    CheckLayoutCacheTracksEngineGeneration();

    if (gFailures != 0) {
        std::printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
