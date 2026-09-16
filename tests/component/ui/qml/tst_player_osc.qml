import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 900
    height: 400

    QtObject {
        id: actions

        function firstFrame() {
        }
        function stepBackwardSecond() {
        }
        function stepBackwardFive() {
        }
        function previousFrame() {
        }
        function togglePlayback() {
        }
        function nextFrame() {
        }
        function stepForwardFive() {
        }
        function stepForwardSecond() {
        }
        function lastFrame() {
        }
    }

    Item {
        id: focusTarget
    }

    Dvs.PlayerOsc {
        id: osc

        width: root.width
        controllerState: 1
        docked: false
        playing: false
        timelineEnabled: true
        currentFrame: 10
        totalFrames: 100
        progress: 0.1
        timecodeText: "00:00:00:10"
        markers: []
        actions: actions
        focusTarget: focusTarget
        canFirst: true
        canPrevious: true
        canPlay: true
        canPause: false
        canNext: true
        canLast: true
        anchors.bottom: root.bottom
    }

    // Docked instance: controllerState 0 (pinned) with docked == true. Exercises the
    // opaque, always-enabled, no-wake-strip docked transport path. Anchored to the top so
    // it does not overlap the auto-hide osc (which owns the bottom) — the two must not
    // compete for synthetic hover events in the MouseArea-free QML test surface.
    Dvs.PlayerOsc {
        id: dockedOsc

        width: root.width
        controllerState: 0
        docked: true
        sourceLabel: "gameplay_capture.mp4"
        playing: false
        timelineEnabled: true
        currentFrame: 2188
        totalFrames: 9012
        progress: 0.24
        timecodeText: "00:01:12:18"
        markers: []
        actions: actions
        focusTarget: focusTarget
        canFirst: true
        canPrevious: true
        canPlay: true
        canPause: false
        canNext: true
        canLast: true
        anchors.top: root.top
    }

    SignalSpy {
        id: previewSpy
        target: osc
        signalName: "previewRequested"
    }

    TestCase {
        id: testCase

        name: "PlayerOsc"
        when: windowShown

        function test_auto_hide_disables_invisible_controls() {
            const panel = findChild(osc, "oscPanel");
            verify(panel !== null);
            // Exercise the production reveal entry point directly. Synthetic hover transitions
            // are platform-window dependent and can leave HoverHandler hovered even after a
            // mouseMove to another test item, preventing the hide timer from restarting.
            // Hide the content tree while the timer runs so the runner's global cursor cannot
            // immediately wake the panel again when the wake strip becomes visible.
            root.visible = false;
            wait(50);
            try {
                osc.revealActive = false;
                compare(osc.controlsEnabled, false);
                osc.reveal();
                verify(osc.controlsEnabled);
                verify(panel.enabled);
                tryCompare(osc, "controlsEnabled", false, 3000);
                tryCompare(panel, "enabled", false, 400);
                tryCompare(panel, "opacity", 0, 1000);
            } finally {
                root.visible = true;
            }
        }

        function test_compact_layout_does_not_overlap() {
            osc.revealActive = true;
            const timeline = findChild(osc, "timelineSlider");
            const transport = findChild(osc, "transportBar");
            const thumb = findChild(timeline, "playheadThumb");
            verify(timeline !== null);
            verify(transport !== null);
            verify(thumb !== null);
            // The transport bar must sit below the timeline box...
            verify(timeline.y + timeline.height <= transport.y);
            // ...and the playhead thumb (which extends ~6 px below the rail) must clear the
            // transport bar so it is neither painted over by nor ungrabbable behind the buttons.
            // Translate the thumb's bounding box into the osc coordinate space before comparing, since
            // thumb.y is relative to the timeline while transport.y is relative to the osc. NOTE: in
            // QML a QRectF exposes `bottom` as a read-only property, not a method — use `.bottom`.
            // Map the thumb's OWN bounds (origin 0,0 in its local space) to the osc coordinate space.
            // NOTE: do NOT pass thumb.x/thumb.y as the rect origin — mapToItem already accounts for the
            // item's position in its parent, so doing so would double-count the offset.
            const thumbRect = thumb.mapToItem(osc, Qt.rect(0, 0, thumb.width, thumb.height));
            verify(thumbRect.bottom <= transport.y, "thumb bottom " + thumbRect.bottom + " must clear transport top " + transport.y);
        }

        function test_preview_is_forwarded_as_a_signal() {
            previewSpy.clear();
            const timeline = findChild(osc, "timelineSlider");
            timeline.previewRequested(42);
            compare(previewSpy.count, 1);
            compare(previewSpy.signalArguments[0][0], 42);
        }

        function test_docked_is_always_enabled_without_wake_strip() {
            verify(dockedOsc.docked);
            verify(dockedOsc.controlsEnabled);
            const panel = findChild(dockedOsc, "oscPanel");
            const wakeArea = findChild(dockedOsc, "oscWakeArea");
            verify(panel !== null);
            verify(wakeArea !== null);
            verify(panel.enabled);
            // Docked never fades: fully opaque regardless of revealActive.
            compare(panel.opacity, 1.0);
            // Docked has no wake strip.
            verify(!wakeArea.visible);
            // sourceLabel readout is present when a label is bound.
            const label = findChild(dockedOsc, "sourceLabelReadout");
            verify(label !== null);
            verify(label.visible);
            compare(label.text, "gameplay_capture.mp4");
        }
    }
}
