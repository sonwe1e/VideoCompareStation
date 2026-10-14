import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 600
    height: 120

    Dvs.TimelineTracks {
        id: tracks
        width: 560
        markers: [{"frame": 30, "kind": "missing", "source": "B", "confidence": 12}]
        totalFrames: 100
        progress: 0.5
        inFrame: 0
        outFrame: 99
        anchors.centerIn: root
    }

    TestCase {
        name: "TimelineTracks"
        when: windowShown

        function test_range_is_clipped_to_zoom_window() {
            tracks.setZoom(4, 0.5);
            const range = findChild(tracks, "rangeHighlight");
            verify(range !== null);
            verify(range.visible);
            verify(range.x >= 0);
            verify(range.x + range.width <= tracks.width + 0.5);
        }

        function test_preview_signal_uses_visible_window_frame() {
            const expected = tracks.frameAt(0.75);
            verify(expected >= tracks.windowStartFrame);
            verify(expected < tracks.windowStartFrame + tracks.visibleFrameCount);
        }

        // The OSC's auto-hide countdown asks this. "The pointer is on the timeline" has to survive
        // a scrub that carries the cursor past the bar's edge: Qt drops containsMouse from the
        // timeline's own mouse area while its grab drags outside it, so hover alone would fade the
        // bar out from under a live scrub.
        function test_pointer_inside_survives_a_scrub_outside_the_bar() {
            verify(!tracks.pointerInside, "no pointer and no scrub reads as outside");
            tracks.dragging = true;
            verify(tracks.pointerInside, "a live scrub counts as the pointer being on the timeline");
            tracks.dragging = false;
            verify(!tracks.pointerInside, "ending the scrub restores the outside reading");
        }

        function reset_window() {
            tracks.zoomFactor = 1.0;
            tracks.windowStartFrame = 0;
        }

        // A range whose out-point is not marked yet must still show where it begins: the band
        // runs open to the window's right edge and the in-bracket stands on its own.
        function test_lone_in_point_keeps_band_and_bracket_visible() {
            reset_window();
            tracks.inFrame = 42;
            tracks.outFrame = -1;
            const range = findChild(tracks, "rangeHighlight");
            verify(range.visible, "band stays visible with only the in-point set");
            fuzzyCompare(range.x + range.width, tracks.width, 1.0);
            verify(findChild(tracks, "inPointBracket").visible);
            verify(!findChild(tracks, "outPointBracket").visible);
            tracks.inFrame = 0;
            tracks.outFrame = 99;
        }

        function test_lone_out_point_keeps_band_and_bracket_visible() {
            reset_window();
            tracks.inFrame = -1;
            tracks.outFrame = 48;
            const range = findChild(tracks, "rangeHighlight");
            verify(range.visible, "band stays visible with only the out-point set");
            fuzzyCompare(range.x, 0, 0.5);
            verify(findChild(tracks, "outPointBracket").visible);
            verify(!findChild(tracks, "inPointBracket").visible);
            tracks.inFrame = 0;
            tracks.outFrame = 99;
        }

        // The pair being reversed is invalid as a range, but the user still marked two frames:
        // both brackets stay readable while the band disappears.
        function test_reversed_pair_hides_band_but_keeps_both_brackets() {
            reset_window();
            tracks.inFrame = 80;
            tracks.outFrame = 20;
            verify(!findChild(tracks, "rangeHighlight").visible);
            verify(findChild(tracks, "inPointBracket").visible);
            verify(findChild(tracks, "outPointBracket").visible);
            tracks.inFrame = 0;
            tracks.outFrame = 99;
        }

        // U5: hovering an alignment marker must name its kind in the user's language. The tooltip
        // used to interpolate `marker.kind` directly, so the raw enum the review controller mints
        // ("missing") reached the user. One shared mapping (Theme.timelineMarkerLabel) now feeds
        // this tooltip, the OSC hover readout and the inspector legend.
        function test_marker_tooltip_names_the_kind_in_chinese() {
            // The delegate comes from a Repeater, so it is reached through the visual tree.
            const marker = findChild(tracks, "timelineMarker");
            verify(marker !== null, "the marker delegate is in the timeline's tree");
            const tip = findChild(marker, "vcsToolTip");
            verify(tip !== null, "the marker delegate owns a tooltip");
            verify(tip.text.indexOf("missing") < 0,
                "the raw enum must never reach the user: " + tip.text);

            // Rest the cursor on the marker for frame 30, the model's only marker. The rail is
            // bottom-anchored with a 5 px margin and is 4 px tall, and the 11 px marker straddles
            // it, so the rail's centre is the marker's centre.
            mouseMove(tracks, marker.x + marker.width / 2, marker.y + marker.height / 2);
            wait(50);
            compare(tracks.hoverFrame, 30, "the cursor rests on the marker's frame");

            verify(tip.text.indexOf(qsTr("缺失")) >= 0,
                "the kind is named in Chinese: " + tip.text);
            verify(tip.text.indexOf("missing") < 0,
                "the raw enum must never reach the user: " + tip.text);

            // Leave no hover behind. A later case reads the same timeline's pointer-inside state,
            // and a synthetic hover outlives the case that created it.
            mouseMove(root, 1, 1);
            wait(50);
            verify(!tracks.pointerInside, "the cursor is off the timeline again");
        }
    }
}
