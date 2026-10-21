import QtQuick
import QtTest
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

/*
 * Guards one defect class only: a semantic colour that is byte-identical or near-identical to a
 * source-identity hue. The timeline rail is 4-11 px wide and the Alpha badge is a 7 px dot, so
 * "looks about the same as source B" is indistinguishable from "this is source B". Reverting any
 * single line in VcsTheme.js must fail exactly one case here - that is the discriminating evidence
 * for the A1 reallocation.
 */
Item {
    id: root

    width: 100
    height: 100

    // Two colours count as separated when their hues differ by at least this much, or when one of
    // them is near-neutral. Hue alone is the wrong test: #e2e8f0 computes to a hue close to the
    // accent blue but has almost no chroma, and it is plainly not a blue.
    readonly property real minimumHueSeparation: 30.0
    readonly property real neutralChroma: 0.15
    readonly property real minimumChromaDifference: 0.30

    function parseColor(text) {
        const hex = String(text).replace("#", "");
        const r = parseInt(hex.substring(0, 2), 16) / 255.0;
        const g = parseInt(hex.substring(2, 4), 16) / 255.0;
        const b = parseInt(hex.substring(4, 6), 16) / 255.0;
        const largest = Math.max(r, g, b);
        const smallest = Math.min(r, g, b);
        const delta = largest - smallest;
        let hue = 0.0;
        if (delta > 0.0) {
            if (largest === r) {
                hue = 60.0 * (((g - b) / delta + 6.0) % 6.0);
            } else if (largest === g) {
                hue = 60.0 * (((b - r) / delta + 2.0) % 6.0);
            } else {
                hue = 60.0 * (((r - g) / delta + 4.0) % 6.0);
            }
        }
        return { "hue": hue, "chroma": largest === 0.0 ? 0.0 : delta / largest };
    }

    function hueDistance(first, second) {
        const raw = Math.abs(first.hue - second.hue) % 360.0;
        return raw > 180.0 ? 360.0 - raw : raw;
    }

    // True when a user could tell the two apart: far apart in hue, or one of them near-neutral.
    function separated(firstText, secondText) {
        const first = parseColor(firstText);
        const second = parseColor(secondText);
        if (first.chroma < neutralChroma || second.chroma < neutralChroma)
            return true;
        if (Math.abs(first.chroma - second.chroma) >= minimumChromaDifference)
            return true;
        return hueDistance(first, second) >= minimumHueSeparation;
    }

    function report(label, firstText, secondText) {
        const first = parseColor(firstText);
        const second = parseColor(secondText);
        return label + ": " + firstText + " vs " + secondText
            + " hue=" + hueDistance(first, second).toFixed(1)
            + " chroma=" + first.chroma.toFixed(2) + "/" + second.chroma.toFixed(2);
    }

    TestCase {
        id: testCase

        name: "VcsTheme"
        when: windowShown

        function test_control_colours_are_valid_and_text_stays_readable() {
            const colours = [Theme.control, Theme.controlHover, Theme.controlPressed,
                Theme.controlChecked, Theme.controlBorder, Theme.accentFill,
                Theme.accentHover, Theme.accentPressed, Theme.mutedText];
            for (let i = 0; i < colours.length; ++i)
                verify(/^#([0-9a-f]{6}|[0-9a-f]{8})$/i.test(colours[i]), colours[i]);
            function luminance(hex) {
                const channels = [1, 3, 5].map(function(offset) {
                    const value = parseInt(hex.substring(offset, offset + 2), 16) / 255;
                    return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4);
                });
                return channels[0] * 0.2126 + channels[1] * 0.7152 + channels[2] * 0.0722;
            }
            const pairs = [[Theme.inverseText, Theme.accentFill],
                [Theme.inverseText, Theme.accentHover], [Theme.inverseText, Theme.accentPressed],
                [Theme.mutedText, Theme.panel], [Theme.mutedText, Theme.menu],
                [Theme.mutedText, Theme.raisedPanel],
                [Theme.accentText, Theme.menu], [Theme.mutedText, Theme.window]];
            for (let i = 0; i < pairs.length; ++i) {
                const a = luminance(pairs[i][0]);
                const b = luminance(pairs[i][1]);
                verify((Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) >= 4.5, pairs[i].join(" on "));
            }
        }

        // On-footage overlay marks (crop overlay, crosshair) pin the literals ImageWorkspace.qml
        // used to inline, so extracting them into tokens stayed a zero-visual-change refactor.
        // Reverting any single line in VcsTheme.js fails exactly one compare here.
        function test_footage_overlay_marks_keep_their_pinned_values() {
            compare(Theme.probeHalo, "#99000000");
            compare(Theme.cropScrim, "#73000000");
            compare(Theme.cropBoxBorder, "#ffffff");
            compare(Theme.cropBoxHalo, "#66000000");
            compare(Theme.cropHandle, "#ffffff");
            compare(Theme.cropHandleHalo, "#80000000");
            compare(Theme.cropGridLine, "#40ffffff");
        }

        // UX-16: inactive controls are exempt from WCAG 1.4.3, and that exemption is only
        // honest while nothing a user must read is published in this colour. The floor here is
        // the legibility one the token's comment promises (>= 3.5:1 on the darkest menu
        // surface, >= 4.5:1 on the window), plus the separation that keeps a disabled row from
        // looking like the explanation text next to it - menu explanations moved to
        // `mutedText` precisely so this colour never has to carry them.
        function test_disabled_text_is_legible_but_stays_visibly_inactive() {
            function luminance(hex) {
                const channels = [1, 3, 5].map(function(offset) {
                    const value = parseInt(hex.substring(offset, offset + 2), 16) / 255;
                    return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4);
                });
                return channels[0] * 0.2126 + channels[1] * 0.7152 + channels[2] * 0.0722;
            }
            function contrast(first, second) {
                const a = luminance(first);
                const b = luminance(second);
                return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
            }
            verify(contrast(Theme.disabledText, Theme.menu) >= 3.5,
                "disabledText on menu: " + contrast(Theme.disabledText, Theme.menu).toFixed(2));
            verify(contrast(Theme.disabledText, Theme.window) >= 4.5,
                "disabledText on window: " + contrast(Theme.disabledText, Theme.window).toFixed(2));
            verify(contrast(Theme.mutedText, Theme.menu)
                    > contrast(Theme.disabledText, Theme.menu) + 1.0,
                "mutedText must stay clearly brighter than disabledText on the same surface");
        }

        function test_timeline_markers_avoid_every_source_identity_hue() {
            const kinds = ["duplicate", "extra", "anchor", "other"];
            for (let i = 0; i < kinds.length; ++i) {
                const marker = Theme.timelineMarkerColor(kinds[i]);
                for (let slot = 0; slot < 3; ++slot) {
                    const source = Theme.sourceColor(slot);
                    verify(root.separated(marker, source),
                        root.report("marker " + kinds[i] + " vs source slot " + slot, marker, source));
                }
            }
        }

        function test_information_is_not_a_source_identity_colour() {
            for (let slot = 0; slot < 3; ++slot) {
                const source = Theme.sourceColor(slot);
                verify(root.separated(Theme.information, source),
                    root.report("information vs source slot " + slot, Theme.information, source));
            }
        }

        function test_timeline_markers_are_mutually_distinguishable() {
            const kinds = ["duplicate", "extra", "anchor", "other"];
            for (let i = 0; i < kinds.length; ++i) {
                for (let j = i + 1; j < kinds.length; ++j) {
                    const first = Theme.timelineMarkerColor(kinds[i]);
                    const second = Theme.timelineMarkerColor(kinds[j]);
                    verify(root.separated(first, second),
                        root.report(kinds[i] + " vs " + kinds[j], first, second));
                }
            }
        }

        function test_missing_marker_keeps_the_reserved_failure_red() {
            compare(Theme.timelineMarkerColor("missing"), Theme.error);
        }

        // U02 design-system tokens. Each case pins a value the key controls actually consume
        // (see MainQmlContractTests.DesignTokensGovernKeyControlMetrics); changing a token
        // without re-checking those controls must fail here first.

        function test_typography_tokens_form_the_planned_ladder() {
            compare(Theme.fontMicro, 10);
            compare(Theme.fontSecondary, 12);
            compare(Theme.fontBody, 14);
            compare(Theme.fontGroupTitle, 16);
            compare(Theme.fontMono, 13);
            verify(Theme.fontMicro < Theme.fontSecondary);
            verify(Theme.fontSecondary < Theme.fontBody);
            verify(Theme.fontBody < Theme.fontGroupTitle);
        }

        function test_line_height_tokens_stay_compact() {
            compare(Theme.lineHeightCompact, 1.25);
            compare(Theme.lineHeightBody, 1.35);
            verify(Theme.lineHeightCompact < Theme.lineHeightBody);
        }

        // EX-06: the spacing ladder runs from the inside of a control outwards. One case pins
        // all four steps, so a component that reaches for "12" or "16" without the token (or a
        // token edited to a value off the 4 px grid) fails here before it reaches a screenshot.
        function test_spacing_tokens_form_the_planned_ladder() {
            compare(Theme.spaceMicro, 4);
            compare(Theme.spaceMain, 8);
            compare(Theme.spaceGroup, 12);
            compare(Theme.spaceSection, 16);
            verify(Theme.spaceMicro < Theme.spaceMain,
                "micro must stay inside main");
            verify(Theme.spaceMain < Theme.spaceGroup,
                "main (inside a control) must stay below group (between groups)");
            verify(Theme.spaceGroup < Theme.spaceSection,
                "group (between groups) must stay below section (card padding)");
        }

        function test_hit_area_and_play_button_tokens() {
            compare(Theme.hitAreaMinimum, 32);
            compare(Theme.playButtonHeight, 40);
            compare(Theme.playButtonHeightLarge, 44);
            verify(Theme.hitAreaMinimum >= 32);
            verify(Theme.playButtonHeight >= 40 && Theme.playButtonHeight <= 44);
            verify(Theme.playButtonHeightLarge >= 40 && Theme.playButtonHeightLarge <= 44);
        }

        function test_chrome_row_height_tokens() {
            compare(Theme.barRowHeight, 40);
            compare(Theme.barRowHeightMin, 36);
            compare(Theme.barRowHeightMax, 40);
            verify(Theme.barRowHeightMin <= Theme.barRowHeight);
            verify(Theme.barRowHeight <= Theme.barRowHeightMax);
            compare(Theme.controlStripMinHeight, 64);
            compare(Theme.controlStripMaxHeight, 80);
            verify(Theme.controlStripMinHeight < Theme.controlStripMaxHeight);
        }

        function test_panel_width_tokens_match_the_planned_bands() {
            compare(Theme.fileSidebarWidthDefault, 232);
            compare(Theme.fileSidebarWidthMin, 200);
            compare(Theme.fileSidebarWidthMax, 288);
            verify(Theme.fileSidebarWidthMin <= Theme.fileSidebarWidthDefault);
            verify(Theme.fileSidebarWidthDefault <= Theme.fileSidebarWidthMax);
            compare(Theme.inspectorWidthDefault, 288);
            compare(Theme.inspectorWidthMin, 260);
            compare(Theme.inspectorWidthMax, 320);
            verify(Theme.inspectorWidthMin <= Theme.inspectorWidthDefault);
            verify(Theme.inspectorWidthDefault <= Theme.inspectorWidthMax);
        }

        function test_resizable_region_tokens_stay_in_their_bands() {
            compare(Theme.localStripShareMin, 0.28);
            compare(Theme.localStripShareMax, 0.40);
            verify(Theme.localStripShareMin < Theme.localStripShareMax);
            compare(Theme.nearbyFramesHeightMin, 144);
            compare(Theme.nearbyFramesHeightMax, 184);
            verify(Theme.nearbyFramesHeightMin < Theme.nearbyFramesHeightMax);
        }

        function test_canvas_guard_token() {
            compare(Theme.canvasMinimumWidth, 600);
        }

        // Phase-4 tokens. fontCaption collects the 11 px dense-metadata size 78 call sites
        // already used; radiusMicro/radiusSubtle collect the 4/6 px corners; the two family
        // tokens are single names because QML's font value type exposes one `family` string
        // only - see the tradeoff case below.
        function test_caption_radius_and_family_tokens() {
            compare(Theme.fontCaption, 11);
            verify(Theme.fontMicro < Theme.fontCaption);
            verify(Theme.fontCaption < Theme.fontSecondary);
            compare(Theme.radiusMicro, 4);
            compare(Theme.radiusSubtle, 6);
            verify(Theme.radiusMicro < Theme.radiusSubtle);
            verify(Theme.radiusSubtle < Theme.radiusSmall);
            compare(Theme.fontFamilyUi, "Segoe UI Variable Text");
            compare(Theme.fontFamilyMono, "Cascadia Mono");
            // E: tabular-numeral feature token for numeric mixed text in the UI font. The
            // object shape (and only tnum) is the contract - the rendered effect depends on
            // the font carrying the feature, which this theme never promises.
            verify(Theme.fontFeaturesNumeric !== undefined);
            compare(Theme.fontFeaturesNumeric.tnum, 1);
            const featureProbe = Qt.createQmlObject(
                "import QtQuick\nText { font.features: { \"tnum\": 1 } }", root,
                "themeFontFeaturesProbe");
            verify(featureProbe.font.features !== undefined)
                << "this Qt build must accept font.features assignments";
            featureProbe.destroy();
        }

        // Font-stack tradeoff, recorded as data and comments rather than a version probe:
        // Qt 6.11's QML `font` value type has `family` (string) and no `families` list (the
        // shipped plugins.qmltypes list QQuickFontValueType's properties without it, and the
        // Qt documentation agrees), so a CSS-style fallback stack cannot be expressed in
        // QML and the tokens above are single family names. Missing glyphs are covered by
        // Qt's own font-merging fallback (contextFontMerging exists since 6.8); which
        // substitute font that picks is Qt's decision, not a promise of this theme. The
        // runtime Qt version is not asserted here - the qmltestrunner log header carries
        // it. What IS asserted is that each token binds to a real font group assignment.
        function test_font_family_tokens_bind_to_a_real_font() {
            const probe = Qt.createQmlObject(
                "import QtQuick\nText { font.family: \"Cascadia Mono\" }", root,
                "themeFontFamilyProbe");
            compare(probe.font.family, Theme.fontFamilyMono);
            const uiProbe = Qt.createQmlObject(
                "import QtQuick\nText { font.family: \"Segoe UI Variable Text\" }", root,
                "themeFontFamilyUiProbe");
            compare(uiProbe.font.family, Theme.fontFamilyUi);
            probe.destroy();
            uiProbe.destroy();
        }
    }
}
