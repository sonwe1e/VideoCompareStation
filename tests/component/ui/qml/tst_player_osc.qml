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
                // The cursor gate has to open: with no tree under the cursor the OSC must still
                // decide the cursor is outside the controls, otherwise the fix for the progress bar
                // would disable auto-hide outright.
                verify(!osc.pointerInsidePanel, "no tree under the cursor means no cursor on the controls");
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

        // The auto-hide gate is a cursor gate, not a timer gate. The host calls transport.reveal()
        // on every transport seek (scrubbing the bar, the wheel over the bar), so a countdown that
        // ignores the cursor fades the progress bar out from under the pointer. This is the defect
        // the user reported: hovering the bar used to make it disappear.
        function test_hovering_the_progress_bar_keeps_it_visible_under_the_cursor() {
            const panel = findChild(osc, "oscPanel");
            const timeline = findChild(osc, "timelineSlider");
            verify(panel !== null);
            verify(timeline !== null);
            osc.revealActive = true;

            // Rest the cursor on the progress bar.
            mouseMove(timeline, timeline.width / 2, timeline.height / 2);
            wait(50);
            // Precondition: the OSC itself has to know the cursor is on the controls.
            verify(osc.pointerInsidePanel, "a cursor on the progress bar must report as inside the panel");
            verify(osc.controlsEnabled);

            // Re-reveal exactly like a transport seek does, then wait past the 1200 ms interval.
            osc.reveal();
            wait(1600);
            verify(osc.revealActive, "the bar stayed revealed with the cursor on it");
            verify(osc.controlsEnabled);
            compare(panel.opacity, 1.0);
        }

        // The countdown must not only be blocked under the cursor: it also has to resume the moment
        // the controls are no longer under it. A countdown that only ever stops (or that only one
        // hover source can arm) left the bar stuck visible after a scrub was dragged out of the
        // panel and released, which reads as "the overlay can't be dismissed".
        function test_releasing_a_scrub_outside_the_panel_hides_the_bar_again() {
            const timeline = findChild(osc, "timelineSlider");
            verify(timeline !== null);
            osc.revealActive = true;

            const scrubX = timeline.width / 2;
            const scrubY = timeline.height / 2;
            mouseMove(timeline, scrubX, scrubY);
            wait(50);
            mousePress(timeline, scrubX, scrubY, Qt.LeftButton);
            wait(50);
            // Drag the playhead out of the panel, then release: the grab is gone and the cursor is
            // off the controls, so the countdown owns the next hide.
            mouseMove(root, mouseXOfPanelOutside(), mouseYAbovePanel());
            wait(50);
            // While the drag is live the scrub itself counts as "on the controls"; only the release
            // hands the countdown back.
            verify(osc.pointerInsidePanel, "a live scrub counts as the cursor being on the controls");
            mouseRelease(timeline, scrubX, scrubY, Qt.LeftButton);
            wait(50);
            verify(!osc.pointerInsidePanel, "released off the controls, the cursor is outside");
            tryCompare(osc, "controlsEnabled", false, 3000);
        }

        function mouseXOfPanelOutside() {
            const panel = findChild(osc, "oscPanel");
            return panel ? panel.mapToItem(root, panel.width / 2, panel.height / 2).x : 0;
        }

        function mouseYAbovePanel() {
            const panel = findChild(osc, "oscPanel");
            return panel ? Math.max(0, panel.mapToItem(root, 0, 0).y - 20) : 0;
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

        function test_playback_status_separates_three_phenomena() {
            osc.revealActive = true;
            osc.playbackModeLabel = "正常速度观看";
            osc.playbackModeDetail = "落后超过约 2 秒时跳过整组追上时间";
            osc.playbackTargetRate = 1.0;
            osc.playbackPresentationRate = 0.72;
            osc.playbackRunSkippedFrameSets = 3;
            osc.playbackLagMilliseconds = 800;
            osc.playbackCatchingUp = false;
            osc.sourceDuplicateCount = 2;
            osc.displayGapCount = 5;
            osc.playing = true;
            const status = findChild(osc, "playbackStatusText");
            verify(status !== null);
            verify(status.visible);
            // Mode, target vs actual rate, player-side skips, lag, source duplicates and
            // display gaps must appear as distinct labels so they cannot be conflated.
            verify(status.text.indexOf("正常速度观看") >= 0);
            verify(status.text.indexOf("目标 1.00×") >= 0);
            verify(status.text.indexOf("实际 0.72×") >= 0);
            verify(status.text.indexOf("播放器跳过 3 组") >= 0);
            verify(status.text.indexOf("明显落后") >= 0);
            verify(status.text.indexOf("重复标记 2") >= 0);
            verify(status.text.indexOf("呈现间隙 5") >= 0);
            verify(osc.statusLegend.indexOf("可能与播放器跳过重合") >= 0);
            verify(osc.statusLegend.indexOf("不能相加") >= 0);
            const hint = findChild(osc, "playbackStatusHint");
            verify(hint !== null);
            verify(hint.visible);
            verify(hint.text.indexOf("不跳过") < 0);
            verify(hint.text.indexOf("跳过整组") >= 0);
        }

        function test_review_every_frame_status_reports_no_skip() {
            osc.revealActive = true;
            osc.playbackModeLabel = "逐帧完整审查";
            osc.playbackModeDetail = "不跳过整组；落后时放慢以保留每组画面";
            osc.playbackTargetRate = 1.0;
            osc.playbackPresentationRate = 0.4;
            osc.playbackRunSkippedFrameSets = 0;
            osc.playbackLagMilliseconds = 1500;
            osc.playbackCatchingUp = false;
            osc.sourceDuplicateCount = 0;
            osc.displayGapCount = 0;
            osc.playing = true;
            const status = findChild(osc, "playbackStatusText");
            verify(status !== null);
            verify(status.text.indexOf("逐帧完整审查") >= 0);
            verify(status.text.indexOf("播放器跳过") < 0);
            verify(status.text.indexOf("明显落后") >= 0);
        }

        function test_status_says_unmeasured_before_the_run_presents_a_frame() {
            osc.revealActive = true;
            osc.playbackModeLabel = "连续";
            osc.playbackModeDetail = "";
            osc.playbackTargetRate = 1.0;
            osc.playbackPresentationRate = 0;
            osc.playbackRunSkippedFrameSets = 0;
            osc.playbackRunPresentedFrames = 0;
            osc.playbackLagMilliseconds = 0;
            osc.playbackCatchingUp = false;
            osc.sourceDuplicateCount = 0;
            osc.displayGapCount = 0;
            osc.playing = false;
            const status = findChild(osc, "playbackStatusText");
            verify(status !== null);
            // Nothing has been observed yet, so a zero skip count is not evidence of a clean run.
            // The rail has to say so rather than print nothing and let silence read as "no drops".
            verify(status.text.indexOf("尚未测量") >= 0);
        }

        function test_status_drops_unmeasured_marker_once_the_run_has_samples() {
            osc.revealActive = true;
            osc.playbackModeLabel = "连续";
            osc.playbackModeDetail = "";
            osc.playbackTargetRate = 1.0;
            osc.playbackPresentationRate = 1.0;
            osc.playbackRunSkippedFrameSets = 0;
            osc.playbackRunPresentedFrames = 120;
            osc.playbackLagMilliseconds = 0;
            osc.playbackCatchingUp = false;
            osc.sourceDuplicateCount = 0;
            osc.displayGapCount = 0;
            osc.playing = false;
            const status = findChild(osc, "playbackStatusText");
            verify(status !== null);
            // 120 frames presented and still no skip: now silence IS the measurement.
            verify(status.text.indexOf("尚未测量") < 0);
            verify(status.text.indexOf("播放器跳过") < 0);
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
