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
    }
}