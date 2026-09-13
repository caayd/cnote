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

#include <cstdio>
#include <string>
#include <vector>

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QStringList>

#include "Document.h"
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

        std::printf("     %-10s requested=%-16s resolved=%-24s lineHeight=%.1f advance(\"Hamburgefonstiv\")=%.1f installed=%d\n",
            entry.family == cnote::TextFontFamily::Sans ? "Sans"
                : entry.family == cnote::TextFontFamily::Serif ? "Serif" : "Monospace",
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
    std::printf("     measured bounds: left=%.1f top=%.1f right=%.1f bottom=%.1f (lineHeight=%d lines=%zu)\n",
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
    Check(cnote::text::TextLayoutSignature(text, 300.0f) != cnote::text::TextLayoutSignature(plain, 300.0f),
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

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    CheckFamilyResolution();
    CheckLayoutKeepsEverything();
    CheckWrappingAndDeterminism();

    if (gFailures != 0) {
        std::printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
