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

        // E4: the whole-timeline position strip mirrors the visible window inside the rail
        // while zoomed, and disappears at full-timeline zoom (there is no "somewhere else").
        // Geometry must stay finite and in-bounds even for degenerate timelines (0/1 frames),
        // because hidden QML items still evaluate their bindings.
        function test_window_position_strip_tracks_the_visible_window() {
            reset_window();
            const strip = findChild(tracks, "timelineWindowPositionStrip");
            verify(strip !== null, "the position strip must exist");
            verify(!strip.visible, "the strip is hidden at 1x - the window is everything");
            // Degenerate timelines: bindings still evaluate, values must stay finite and
            // in-bounds (no Infinity/NaN from the totalFrames - 1 divisor).
            const savedTotal = tracks.totalFrames;
            tracks.totalFrames = 0;
            verify(isFinite(strip.x) && strip.x >= 0, "0-frame x must be finite and >= 0");
            verify(isFinite(strip.width) && strip.width >= 2, "0-frame width must be finite");
            tracks.totalFrames = 1;
            verify(isFinite(strip.x) && strip.x >= 0, "1-frame x must be finite and >= 0");
            verify(isFinite(strip.width) && strip.width >= 2, "1-frame width must be finite");
            // Single-frame endpoints stay 0/0 - last must not overrun the last frame.
            verify(strip.firstVisibleFrame === 0 && strip.lastVisibleFrame === 0,
                   "1-frame endpoints must be 0/0, got " + strip.firstVisibleFrame + "/" + strip.lastVisibleFrame);
            verify(strip.lastVisibleFrame <= tracks.totalFrames - 1,
                   "last endpoint must stay within the timeline on degenerate timelines");
            tracks.totalFrames = savedTotal;
            verify(isFinite(strip.x) && isFinite(strip.width));

            const unit = tracks.width / (tracks.totalFrames - 1);
            // Start of the timeline at 32x: the strip hugs the left edge (a 32x window of
            // 100 frames covers ceil(100/32) = 4 frames).
            tracks.setZoom(32, 0.0);
            verify(strip.visible, "the strip shows while zoomed in");
            tracks.windowStartFrame = 0;
            tryCompare(strip, "x", 0);
            verify(strip.width < tracks.width / 4, "a 32x window covers about 4/99 of the rail");
            verify(strip.x + strip.width <= tracks.width + 0.5, "the strip stays in bounds");

            // Middle: centre the window deterministically (setZoom anchors around the frame
            // under the anchor point, which the forced start above pinned to frame 0 - assign
            // the centred window start directly instead).
            tracks.setZoom(4, 0.5);
            tracks.windowStartFrame = Math.floor((tracks.totalFrames - tracks.visibleFrameCount) / 2);
            const midStart = tracks.windowStartFrame;
            verify(midStart > 0 && midStart + tracks.visibleFrameCount < tracks.totalFrames,
                   "the window must sit strictly inside the timeline for the centre check");
            const expectedMidX = midStart * unit;
            verify(Math.abs(strip.x - expectedMidX) < 1.5,
                   "strip x tracks the window start: " + strip.x + " vs " + expectedMidX);
            verify(strip.x >= 0 && strip.x + strip.width <= tracks.width + 0.5,
                   "the strip never leaves the rail");
            verify(strip.width < tracks.width / 2, "a 4x window covers about a quarter");

            // End of the timeline: the strip reaches the right edge.
            tracks.windowStartFrame = tracks.maximumWindowStart;
            verify(strip.x + strip.width >= tracks.width - 1.5,
                   "at the end the strip reaches the right edge");

            // Accessible name states the 1-based visible range.
            const from = strip.firstVisibleFrame + 1;
            const to = strip.lastVisibleFrame + 1;
            verify(from >= 1 && to <= tracks.totalFrames && from <= to,
                   "the accessible range must be ordered and inside the timeline");
            verify(strip.Accessible.name.indexOf(from.toString()) >= 0);
            verify(strip.Accessible.name.indexOf(to.toString()) >= 0);
            reset_window();
            verify(!strip.visible);
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
            tracks.progress = 0.5;
            tracks.dragging = false;
        }

        // The later cases in this file (marker hover, brackets) assume the timeline starts
        // unzoomed at the declaration default; every follow case therefore restores the state
        // it changed before handing the shared timeline on.

        // I5: zoomed-in playback or stepping must scroll the window once the playhead leaves
        // the 10-90% safety band, keeping the playhead centred. A frame inside the band never
        // moves the window (no fight with reading the neighbourhood).
        function test_follow_scrolls_window_when_playhead_exits_band() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(4, 0.5);
            verify(tracks.zoomFactor > 1);
            const windowBefore = tracks.windowStartFrame;
            // Frame 80 of 100 with a 25-frame window: 80 > start + 0.9 * 24 = window + 21.6.
            tracks.progress = 80 / 99;
            const expected = Math.max(0, Math.min(tracks.maximumWindowStart, Math.round(80 - (tracks.visibleFrameCount - 1) / 2)));
            tryCompare(tracks, "windowStartFrame", expected);
            verify(tracks.windowStartFrame > windowBefore, "the window followed the playhead");
            const position = tracks.positionForFrame(80);
            verify(position > 0.1 && position < 0.9, "playhead re-entered the band centred: " + position);
            reset_window();
        }

        function test_follow_holds_steady_inside_the_band() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(4, 0.5);
            const windowBefore = tracks.windowStartFrame;
            // One frame forward stays inside the band: the window must not budge.
            tracks.progress = 51 / 99;
            compare(tracks.windowStartFrame, windowBefore);
            reset_window();
        }

        // A live scrub owns the window: the follow may not fight the hand that drags the
        // playhead, even after the playhead has left the band.
        function test_manual_drag_suspends_follow() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(4, 0.5);
            const windowBefore = tracks.windowStartFrame;
            tracks.dragging = true;
            tracks.progress = 95 / 99;
            compare(tracks.windowStartFrame, windowBefore, "the scrub keeps the window where it was");
            tracks.dragging = false;
            // The next frame change after the release resumes the follow.
            tracks.progress = 96 / 99;
            verify(tracks.windowStartFrame > windowBefore, "follow resumes once the scrub ends");
            reset_window();
        }

        // Walking backwards past the left band edge scrolls the window back and clamps at the
        // timeline start; the playhead never parks outside the visible window.
        function test_follow_backward_clamps_at_zero() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(4, 0.5);
            tracks.windowStartFrame = 30;
            tracks.progress = 2 / 99;
            compare(tracks.windowStartFrame, 0, "window clamped to the timeline start");
            verify(tracks.playheadFrame >= tracks.windowStartFrame);
            reset_window();
        }

        function test_follow_forward_clamps_at_maximum_window() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(4, 0.5);
            tracks.progress = 1.0;
            tryCompare(tracks, "windowStartFrame", tracks.maximumWindowStart);
            reset_window();
        }

        // A new task invalidates the frame base; the window restarts on the whole timeline
        // instead of keeping a clamped stale window into the new clip. The playhead parks at
        // frame 0 first, as the controller does on an open, so the follow cannot legitimately
        // scroll the fresh window during the swap.
        function test_task_change_resets_window() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(4, 0.5);
            tracks.windowStartFrame = 30;
            tracks.progress = 0;
            tracks.totalFrames = 200;
            compare(tracks.windowStartFrame, 0, "window restarted with the new task");
            tracks.totalFrames = 100;
            reset_window();
        }

        // The zoom pill is the one-click way back to the whole timeline.
        function test_zoom_badge_resets_zoom() {
            reset_window();
            tracks.progress = 0.5;
            tracks.setZoom(8, 0.5);
            const badge = findChild(tracks, "timelineZoomBadge");
            verify(badge !== null);
            verify(badge.visible);
            mouseClick(badge);
            tryCompare(tracks, "zoomFactor", 1.0);
            compare(tracks.windowStartFrame, 0);
            reset_window();
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
