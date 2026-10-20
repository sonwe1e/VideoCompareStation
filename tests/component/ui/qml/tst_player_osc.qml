import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

Item {
    id: root

    width: 900
    height: 400

    QtObject {
        id: actions

        property var jogPressCalls: []
        property int jogReleaseCalls: 0
        property int previousFrameCalls: 0
        property int nextFrameCalls: 0
        property var jogRateCalls: []
        // I8 preset routing: the player preset sends the second-step buttons to stepSeconds
        // (like ReviewShortcuts' Ctrl+←/→ branch); the frame-review preset keeps the exact
        // second seam.
        property var stepSecondsCalls: []
        property int backwardSecondCalls: 0
        property int forwardSecondCalls: 0
        // The continuity switch is the one transport intent PlayerOsc does not re-emit as a
        // signal: it calls straight through to the host action. Recording the call here is the
        // only way to observe it, and it is the same seam the pointer path uses.
        property int continuityPolicyCalls: 0
        property int lastContinuityPolicy: -1

        function setPlaybackContinuityPolicy(policy) {
            continuityPolicyCalls += 1;
            lastContinuityPolicy = policy;
        }

        function firstFrame() {
        }
        function stepBackwardSecond() {
            backwardSecondCalls += 1;
        }
        function stepBackwardFive() {
        }
        function previousFrame() {
            previousFrameCalls += 1;
        }
        function togglePlayback() {
        }
        function nextFrame() {
            nextFrameCalls += 1;
        }
        function stepForwardFive() {
        }
        function stepForwardSecond() {
            forwardSecondCalls += 1;
        }
        function stepSeconds(seconds) {
            const nextCalls = stepSecondsCalls.slice(0);
            nextCalls.push(seconds);
            stepSecondsCalls = nextCalls;
        }
        function lastFrame() {
        }
        function jogPress(direction) {
            const nextPresses = jogPressCalls.slice(0);
            nextPresses.push(direction);
            jogPressCalls = nextPresses;
            return true;
        }
        function jogRelease() {
            jogReleaseCalls += 1;
        }
        function setJogRate(rate) {
            const nextRates = jogRateCalls.slice(0);
            nextRates.push(rate);
            jogRateCalls = nextRates;
        }
    }

    // Order-sensitive stand-in used by the hold-to-jog test: records the exact event sequence
    // so a double release or a stray keyboard-fallback step is attributable.
    QtObject {
        id: orderedActions

        property var events: []

        function record(name) {
            const next = events.slice(0);
            next.push(name);
            events = next;
        }
        function previousFrame() {
            record("previousFrame");
        }
        function nextFrame() {
            record("nextFrame");
        }
        function jogPress(direction) {
            record("jogPress:" + direction);
            return true;
        }
        function jogRelease() {
            record("jogRelease");
        }
        function firstFrame() {
        }
        function stepBackwardSecond() {
        }
        function stepBackwardFive() {
        }
        function togglePlayback() {
        }
        function stepForwardFive() {
        }
        function stepForwardSecond() {
        }
        function lastFrame() {
        }
    }

    Dvs.PlayerOsc {
        id: orderedOsc

        // Raised above dockedOsc (same top anchor, declared later): without this the
        // docked instance's identical transport layout occludes this one and synthetic
        // presses land on the wrong buttons.
        z: 5
        visible: false
        width: root.width
        controllerState: 0
        docked: true
        playing: false
        timelineEnabled: true
        currentFrame: 10
        totalFrames: 100
        progress: 0.1
        timecodeText: "00:00:00:10"
        markers: []
        actions: orderedActions
        focusTarget: focusTarget
        canFirst: true
        canPrevious: true
        canPlay: true
        canPause: false
        canNext: true
        canLast: true
        anchors.top: root.top
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

    SignalSpy {
        id: markInSpy
        target: osc
        signalName: "markInRequested"
    }
    SignalSpy {
        id: pingPongSpy
        target: osc
        signalName: "rangePingPongRequested"
    }
    SignalSpy {
        id: exportRangeSpy
        target: osc
        signalName: "exportRangeRequested"
    }

    // P03 protocol-controller stand-in: a plain QtObject carrying the properties the transport
    // row reads, so visibility/enabled/badge behaviour is testable without a C++ repository.
    QtObject {
        id: protocolFake

        property bool protocolLoaded: false
        property bool canGoNextGenerated: false
        property bool canGoPreviousGenerated: false
        property string currentOriginBadge: ""
        property string statusText: "无协议（协议导航关闭）"
        property string lastError: ""
    }

    // V-07 pair-metrics stand-in: the members MetricTimelineLane reads off PairMetricsController,
    // so the lane's hover, focus and hit-area behaviour is testable without a C++ controller.
    // `samplesChanged` is the publication signal the lane listens for.
    QtObject {
        id: metricsFake

        signal samplesChanged()

        property bool available: true
        property bool laneEnabled: true
        property bool sampling: false
        property bool hasCurrentSample: false
        property bool currentComparable: false
        property real currentMae: 0
        property real currentPsnrDb: 0
        property int sampleCount: 0
        property real sampleMaxMae: 1.0
        property var peaks: []

        function peakFrames(count, neighborhood) {
            return metricsFake.peaks;
        }
        function samplePoints(maximumPoints) {
            return [];
        }
    }

    // Standalone instance wired to the fake controller. The main `osc` above stays protocol-free
    // and covers the no-controller layout. Hidden outside the protocol tests: this instance is
    // full-width and would otherwise compete with the hover-sensitive `osc` for synthetic
    // cursor events (the scrub/auto-hide tests read the cursor state of `osc`).
    Dvs.PlayerOsc {
        id: protocolOsc

        // Only this fixture should receive protocol clicks when its centered panel
        // overlaps the top/bottom transport fixtures after a status-row reflow.
        z: 6
        visible: false
        width: root.width
        controllerState: 0
        docked: true
        playing: false
        timelineEnabled: true
        currentFrame: 4
        totalFrames: 10
        progress: 0.4
        timecodeText: "00:00:00:04"
        markers: []
        actions: actions
        focusTarget: focusTarget
        canFirst: true
        canPrevious: true
        canPlay: true
        canPause: false
        canNext: true
        canLast: true
        protocol: protocolFake
        anchors.verticalCenter: root.verticalCenter
    }

    SignalSpy {
        id: protocolLoadSpy
        target: protocolOsc
        signalName: "protocolLoadRequested"
    }

    SignalSpy {
        id: protocolClearSpy
        target: protocolOsc
        signalName: "protocolClearRequested"
    }

    SignalSpy {
        id: protocolNextSpy
        target: protocolOsc
        signalName: "nextGeneratedFrameRequested"
    }

    SignalSpy {
        id: protocolPreviousSpy
        target: protocolOsc
        signalName: "previousGeneratedFrameRequested"
    }

    // Retargeted onto the transport hold buttons inside the hold test: released and canceled
    // both feed the same navigationReleased handler, so the ordered event log alone cannot tell
    // a healthy hold from one that Qt silently canceled (for example by a focus change pressed
    // into the hold).
    SignalSpy {
        id: holdReleaseSpy
    }

    SignalSpy {
        id: holdCancelSpy
    }

    // V-07 metric-lane instance. The lane only exists when metrics are available, so it gets its
    // own transport: the shared `osc` keeps the pre-feature layout for the cursor tests, and
    // dockedOsc (declared after `osc`, same z) covers the top of the panel the lane sits in.
    // Raised above every other fixture and hidden outside these tests, exactly like the
    // protocol instance, so synthetic hover and clicks land on this lane.
    Dvs.PlayerOsc {
        id: metricOsc

        z: 7
        visible: false
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
        anchors.top: root.top
    }

    SignalSpy {
        id: seekSpy
        target: metricOsc
        signalName: "seekRequested"
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
            mouseRelease(root, mouseXOfPanelOutside(), mouseYAbovePanel(), Qt.LeftButton);
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

        // E04: holding the single-frame buttons drives the jog state machine exactly like the
        // keyboard — pressed feeds jogPress, release stops it — while a plain click still steps
        // exactly one frame through the keyboard-style clicked fallback.
        function test_hold_previous_button_jogs_and_release_stops() {
            osc.revealActive = true;
            const previousButton = findChild(osc, "previousButton");
            verify(previousButton !== null);
            actions.jogPressCalls = [];
            actions.jogReleaseCalls = 0;
            actions.previousFrameCalls = 0;

            mousePress(previousButton);
            compare(actions.jogPressCalls, [-1]);
            compare(actions.previousFrameCalls, 0);

            mouseRelease(previousButton);
            compare(actions.jogReleaseCalls, 1);
            compare(actions.previousFrameCalls, 0);
        }

        function test_hold_next_button_jogs_and_keyboard_click_steps_once() {
            osc.revealActive = true;
            const nextButton = findChild(osc, "nextButton");
            verify(nextButton !== null);
            actions.jogPressCalls = [];
            actions.jogReleaseCalls = 0;
            actions.nextFrameCalls = 0;

            mousePress(nextButton);
            compare(actions.jogPressCalls, [1]);
            mouseRelease(nextButton);
            compare(actions.jogReleaseCalls, 1);
            compare(actions.nextFrameCalls, 0);

            // Keyboard activation (Space/Enter on a focused button) emits clicked without a
            // press/release pair; the fallback must step exactly once, not double with a jog.
            nextButton.clicked();
            compare(actions.nextFrameCalls, 1);
            compare(actions.jogPressCalls.length, 1);
        }

        // A press that ends in canceled (released off-button or stolen by a grab) emits no
        // clicked, so nothing clears the mouse-pressed marker. The next keyboard activation must
        // still step once instead of silently swallowing the frame.
        function test_canceled_press_does_not_swallow_the_next_keyboard_step() {
            osc.revealActive = true;
            const previousButton = findChild(osc, "previousButton");
            verify(previousButton !== null);
            actions.jogPressCalls = [];
            actions.jogReleaseCalls = 0;
            actions.previousFrameCalls = 0;

            mousePress(previousButton);
            compare(actions.jogPressCalls, [-1]);
            previousButton.canceled();
            compare(actions.jogReleaseCalls, 1);

            previousButton.clicked();
            compare(actions.previousFrameCalls, 1, "the keyboard step after a canceled press must land");
            compare(actions.jogPressCalls.length, 1);
        }

        function test_button_hold_event_order_has_one_press_and_one_release() {
            orderedOsc.visible = true;
            const previousButton = findChild(orderedOsc, "previousButton");
            verify(previousButton !== null);
            holdReleaseSpy.target = previousButton;
            holdReleaseSpy.signalName = "released";
            holdCancelSpy.target = previousButton;
            holdCancelSpy.signalName = "canceled";
            holdReleaseSpy.clear();
            holdCancelSpy.clear();
            orderedActions.events = [];

            mousePress(previousButton);
            mouseRelease(previousButton);
            compare(orderedActions.events, ["jogPress:-1", "jogRelease"]);
            // A canceled hold produces the same two action events, so the log cannot catch it:
            // the press must actually survive to released, with canceled never firing. This is
            // the regression gate for "restoreFocus inside onPressed killed the mouse hold".
            compare(holdReleaseSpy.count, 1, "the hold must end through released");
            compare(holdCancelSpy.count, 0, "the hold must not be canceled mid-press");
            orderedOsc.visible = false;
        }

        // E04 walk-rate completion: the 长按逐帧 combo mirrors the controller projection and its
        // selection reaches the same action seam the jog keys use. Driven by the real activation
        // path (open, arrow, confirm) so a handler wired to the wrong signal fails here.
        function test_jog_rate_selector_mirrors_and_forwards_the_walk_rate() {
            osc.revealActive = true;
            const transport = findChild(osc, "transportBar");
            verify(transport !== null);
            const combo = findChild(transport, "jogRateCombo");
            verify(combo !== null);

            osc.jogRate = 0.5;
            compare(combo.currentIndex, 0);
            osc.jogRate = 1;
            compare(combo.currentIndex, 1);
            osc.jogRate = 2;
            compare(combo.currentIndex, 2);

            osc.jogRate = 1;
            actions.jogRateCalls = [];
            combo.forceActiveFocus();
            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryVerify(() => combo.popup.visible);
            keyClick(Qt.Key_Down);
            keyClick(Qt.Key_Return);
            compare(actions.jogRateCalls, [2]);
        }

        function test_playback_status_separates_three_phenomena() {
            osc.revealActive = true;
            osc.playbackModeLabel = "流畅优先";
            osc.playbackModeDetail = "保持原速；解码跟不上时整组跳帧";
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
            verify(status.text.indexOf("流畅优先") >= 0);
            verify(status.text.indexOf("目标 1.00×") >= 0);
            verify(status.text.indexOf("实际 0.72×") >= 0);
            verify(status.text.indexOf("播放器跳过 3 组") >= 0);
            verify(status.text.indexOf("明显落后") >= 0);
            verify(status.text.indexOf("重复标记 2") >= 0);
            verify(status.text.indexOf("呈现间隙 5") >= 0);
            verify(osc.statusLegend.indexOf("可能与播放器跳过重合") >= 0);
            verify(osc.statusLegend.indexOf("不能相加") >= 0);
            // The strategy explanation left its own layout line for the tooltip: the wording
            // stays reachable on the hidden anchor and inside the shared tooltip text.
            const hint = findChild(osc, "playbackStatusHint");
            verify(hint !== null);
            verify(!hint.visible, "the detail line no longer spends a row in any state");
            verify(hint.text.indexOf("整组跳帧") >= 0);
            verify(osc.playbackStatusToolTipText().indexOf("整组跳帧") >= 0);
            // The alert rail carries the warning colour; the quiet capsule is off-stage.
            verify(Qt.colorEqual(status.color, Theme.warning));
            const quiet = findChild(osc, "playbackStatusQuiet");
            verify(quiet !== null);
            verify(!quiet.visible, "an alert run must not also show the quiet capsule");
        }

        function test_review_every_frame_status_reports_no_skip() {
            osc.revealActive = true;
            osc.playbackModeLabel = "逐帧完整";
            osc.playbackModeDetail = "每一帧都显示；负载高时播放会放慢";
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
            verify(status.text.indexOf("逐帧完整") >= 0);
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
            const quiet = findChild(osc, "playbackStatusQuiet");
            verify(status !== null);
            verify(quiet !== null);
            // Nothing has been observed yet, so a zero skip count is not evidence of a clean run.
            // Silence would read as "no drops", so the capsule states what it does not know -
            // but in mutedText, not warning yellow: an unmeasured run is a caveat, not a
            // failure, and yellow has to keep meaning "look at this" (UX-02).
            verify(!osc.playbackStatusAlert, "an unmeasured run is not an alert");
            verify(quiet.visible);
            verify(quiet.text.indexOf("尚未测量") >= 0);
            verify(Qt.colorEqual(quiet.color, Theme.mutedText));
            verify(!status.visible, "the warning rail stays off-stage for an unmeasured run");
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
            const quiet = findChild(osc, "playbackStatusQuiet");
            verify(status !== null);
            verify(quiet !== null);
            // 120 frames presented and still no skip: now silence IS the measurement, so both
            // surfaces drop the admission and the capsule is back to mode + target rate.
            verify(!osc.playbackStatusUnmeasured);
            verify(quiet.text.indexOf("尚未测量") < 0);
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

        // P03: without a protocol controller the whole protocol row stays out of the layout, so
        // the transport renders exactly as before the feature existed.
        function test_protocol_row_hidden_without_controller() {
            const row = findChild(osc, "transportProtocolRow");
            verify(row !== null);
            verify(!row.visible);
            const badge = findChild(osc, "transportProtocolBadge");
            verify(badge !== null);
            verify(!badge.visible);
        }

        function test_protocol_row_follows_controller_state() {
            // Show the protocol instance only while this test drives it; `finally` restores the
            // shared fake and the visibility so a failure cannot poison the later tests.
            protocolOsc.visible = true;
            try {
                const row = findChild(protocolOsc, "transportProtocolRow");
                const loadButton = findChild(protocolOsc, "transportLoadProtocolButton");
                const previousButton = findChild(protocolOsc, "transportPreviousGeneratedButton");
                const nextButton = findChild(protocolOsc, "transportNextGeneratedButton");
                const clearButton = findChild(protocolOsc, "transportClearProtocolButton");
                const badge = findChild(protocolOsc, "transportProtocolBadge");
                verify(row !== null);
                verify(loadButton !== null);
                verify(previousButton !== null);
                verify(nextButton !== null);
                verify(clearButton !== null);
                verify(badge !== null);

                // Controller wired but no protocol loaded: only the import entry and the status
                // hint exist; nothing here can seek yet. The chips gate on `chipEnabled` — the
                // chip's own Item.enabled is not wired to it (only the inner MouseArea is).
                verify(row.visible);
                verify(loadButton.visible && loadButton.chipEnabled);
                verify(!previousButton.visible);
                verify(!nextButton.visible);
                verify(!clearButton.visible);
                verify(badge.visible);
                verify(badge.text.indexOf("无协议") >= 0);

                // Protocol loaded, both directions available.
                protocolFake.protocolLoaded = true;
                protocolFake.canGoNextGenerated = true;
                protocolFake.canGoPreviousGenerated = true;
                protocolFake.statusText = "协议已载入 · 10 个输出位 · GT 10 帧";
                protocolFake.currentOriginBadge = "生成帧 · 输入 GT 1/3 · 比例 1/3";
                tryCompare(previousButton, "visible", true);
                verify(nextButton.visible && nextButton.chipEnabled);
                verify(previousButton.chipEnabled);
                verify(clearButton.visible && clearButton.chipEnabled);
                // The badge is text-first: provenance must read as words, never colour alone.
                verify(badge.text.indexOf("生成帧") >= 0);
                verify(badge.text.indexOf("输入 GT 1/3") >= 0);
                verify(badge.text.indexOf("比例 1/3") >= 0);

                // A direction with no target disables its chip instead of hiding the pair.
                protocolFake.canGoNextGenerated = false;
                tryCompare(nextButton, "chipEnabled", false);
                verify(nextButton.visible);
                verify(previousButton.chipEnabled);

                // Import failures surface in the badge text.
                protocolFake.lastError = "interpolation-protocol document is not valid UTF-8 JSON.";
                tryCompare(badge, "text", "协议错误：interpolation-protocol document is not valid UTF-8 JSON.");
                protocolFake.lastError = "";

                // Clearing hides the navigation chips again.
                protocolFake.protocolLoaded = false;
                tryCompare(previousButton, "visible", false);
                verify(!nextButton.visible);
                verify(!clearButton.visible);
            } finally {
                protocolFake.protocolLoaded = false;
                protocolFake.canGoNextGenerated = false;
                protocolFake.canGoPreviousGenerated = false;
                protocolFake.currentOriginBadge = "";
                protocolFake.statusText = "无协议（协议导航关闭）";
                protocolFake.lastError = "";
                protocolOsc.visible = false;
            }
        }

        function test_protocol_chips_forward_intents_as_signals() {
            protocolLoadSpy.clear();
            protocolClearSpy.clear();
            protocolNextSpy.clear();
            protocolPreviousSpy.clear();
            protocolOsc.visible = true;
            try {
                const loadButton = findChild(protocolOsc, "transportLoadProtocolButton");
                const nextButton = findChild(protocolOsc, "transportNextGeneratedButton");
                const previousButton = findChild(protocolOsc, "transportPreviousGeneratedButton");
                const clearButton = findChild(protocolOsc, "transportClearProtocolButton");
                verify(loadButton !== null);
                verify(nextButton !== null);
                verify(previousButton !== null);
                verify(clearButton !== null);

                waitForRendering(loadButton);
                mouseClick(loadButton);
                compare(protocolLoadSpy.count, 1);

                protocolFake.protocolLoaded = true;
                protocolFake.canGoNextGenerated = true;
                protocolFake.canGoPreviousGenerated = true;
                // Wait for the layout polish after the chips appear: a click mapped from the
                // pre-reflow chip position lands where the chip no longer is (the row re-centres
                // when the navigation chips join it).
                tryCompare(nextButton, "visible", true);
                waitForRendering(nextButton);
                mouseClick(nextButton);
                compare(protocolNextSpy.count, 1);
                mouseClick(previousButton);
                compare(protocolPreviousSpy.count, 1);
                mouseClick(clearButton);
                compare(protocolClearSpy.count, 1);
            } finally {
                // Restore the shared fake and hide the instance again so later cursor-state
                // tests (the scrub release) see the pre-feature layout.
                protocolFake.protocolLoaded = false;
                protocolFake.canGoNextGenerated = false;
                protocolFake.canGoPreviousGenerated = false;
                protocolOsc.visible = false;
            }
        }

        // V-07 U10: the lane's expand/collapse toggle is a control, not a caption: it answers
        // the pointer with the readable accent and the keyboard with the same focus ring
        // ReviewActionButton draws, and Return/Space perform the click's toggle.
        function test_metric_lane_toggle_answers_hover_and_keyboard_focus() {
            metricOsc.visible = true;
            metricOsc.revealActive = true;
            metricOsc.metrics = metricsFake;
            try {
                const lane = findChild(metricOsc, "metricTimelineLane");
                verify(lane !== null);
                verify(lane.visible, "available metrics must reveal the lane");
                const toggle = findChild(lane, "metricLaneToggle");
                verify(toggle !== null);
                verify(toggle.visible);

                // Hover answers with the readable accent; leaving returns to the resting one.
                verify(Qt.colorEqual(toggle.color, Theme.accent));
                mouseMove(toggle, toggle.width / 2, toggle.height / 2);
                tryVerify(function() { return Qt.colorEqual(toggle.color, Theme.accentText); });
                mouseMove(root, 1, 1);
                tryVerify(function() { return Qt.colorEqual(toggle.color, Theme.accent); });

                // Keyboard reachable, with the repo's canonical ring (2px Theme.focus). A Text
                // has no visualFocus, so the ring follows activeFocus.
                verify(toggle.activeFocusOnTab, "the toggle must be reachable with Tab");
                toggle.forceActiveFocus(Qt.TabFocusReason);
                verify(toggle.activeFocus);
                let ring = null;
                for (const child of toggle.children) {
                    if (child.border !== undefined)
                        ring = child;
                }
                verify(ring !== null, "a focused toggle must draw a focus ring");
                compare(ring.border.width, 2);
                verify(Qt.colorEqual(ring.border.color, Theme.focus));
                // The ring belongs to focus alone: it disappears again when focus moves on.
                root.forceActiveFocus();
                verify(!toggle.activeFocus);
                compare(ring.border.width, 0);
                toggle.forceActiveFocus(Qt.TabFocusReason);

                // Return and Space both flip the lane, exactly like the click does.
                const enabledBefore = metricsFake.laneEnabled;
                keyClick(Qt.Key_Return);
                compare(metricsFake.laneEnabled, !enabledBefore);
                keyClick(Qt.Key_Space);
                compare(metricsFake.laneEnabled, enabledBefore);
            } finally {
                metricOsc.metrics = null;
                metricOsc.visible = false;
            }
        }

        // UX-06: the range chips and the continuity segments are drawn rectangles, not Controls.
        // They could always be *named* by a screen reader and clicked by a pointer, but there was
        // no way to reach or press them from the keyboard. Each now owns its tab stop, its
        // activation keys and the same 2 px ring the rest of the chrome uses.
        function test_range_chip_and_continuity_option_are_keyboard_operable() {
            // The fixture already carries the range preconditions (timelineEnabled, currentFrame
            // 10 → canMarkRange) and PlayerOsc pins rangeControlsVisible on its own transport, so
            // the only thing to arrange is the panel being on screen.
            osc.revealActive = true;
            osc.playing = false;
            osc.inFrame = -1;
            osc.outFrame = -1;
            wait(0);
            try {
                const chip = findChild(osc, "transportMarkInButton");
                verify(chip !== null);
                verify(chip.visible, "the expanded range row must show the chip");
                verify(chip.chipEnabled);
                verify(chip.activeFocusOnTab, "the chip must be reachable with Tab");

                // Same shape as the metric-lane toggle above: the ring is the only borderless-fill
                // child that carries a border, so finding it by `border` is unambiguous.
                let ring = null;
                for (const child of chip.children) {
                    if (child.border !== undefined)
                        ring = child;
                }
                verify(ring !== null, "a chip must own a focus ring");
                verify(!ring.visible, "an unfocused chip draws no ring");

                chip.forceActiveFocus(Qt.TabFocusReason);
                verify(chip.activeFocus);
                verify(ring.visible);
                compare(ring.border.width, 2);
                verify(Qt.colorEqual(ring.border.color, Theme.focus));

                const marksBefore = markInSpy.count;
                keyClick(Qt.Key_Return);
                compare(markInSpy.count, marksBefore + 1, "Return presses the focused chip");
                keyClick(Qt.Key_Space);
                compare(markInSpy.count, marksBefore + 2, "Space presses it too");

                // The ring belongs to focus alone.
                root.forceActiveFocus();
                verify(!chip.activeFocus);
                verify(!ring.visible);

                // PlayerOsc does not re-emit the transport's continuity signal; it routes it to
                // actions.setPlaybackContinuityPolicy, which is what the pointer path does too.
                // Asserting on that call keeps the keyboard and pointer paths on one seam.
                const option = findChild(osc, "realTimeContinuityButton");
                verify(option !== null);
                verify(option.visible);
                verify(option.activeFocusOnTab, "a continuity segment must be reachable with Tab");
                option.forceActiveFocus(Qt.TabFocusReason);
                verify(option.activeFocus);
                const policiesBefore = actions.continuityPolicyCalls;
                keyClick(Qt.Key_Return);
                compare(actions.continuityPolicyCalls, policiesBefore + 1,
                        "Return presses the focused segment");
                compare(actions.lastContinuityPolicy, 1,
                        "the focused segment reports its own policy");
                keyClick(Qt.Key_Space);
                compare(actions.continuityPolicyCalls, policiesBefore + 2,
                        "Space presses it too");
            } finally {
                osc.inFrame = -1;
                osc.outFrame = -1;
                osc.revealActive = false;
                root.forceActiveFocus();
            }
        }

        // V-07 U10: the peak marker's visible dot stays 6x6 (a data marker on a 22px curve), but
        // its pointer target must reach the theme's 32px hit-area minimum, stay centred on the
        // dot and never reach outside the curve area - including at the lane's left edge.
        function test_metric_lane_peak_marker_reaches_the_hit_area_minimum() {
            metricOsc.visible = true;
            metricOsc.revealActive = true;
            metricsFake.peaks = [
                { "frame": 1, "mae": 5.0 },
                { "frame": 10, "mae": 4.0 },
                { "frame": 90, "mae": 2.0 }
            ];
            metricOsc.metrics = metricsFake;
            try {
                const lane = findChild(metricOsc, "metricTimelineLane");
                verify(lane !== null);
                const canvas = findChild(lane, "metricLaneCanvas");
                verify(canvas !== null);
                const curveArea = canvas.parent;
                verify(curveArea !== null);
                // Publishing samples is what builds the peak list.
                metricsFake.samplesChanged();

                // The markers are Repeater delegates, so their hit areas sit below curveArea.
                const hitAreas = collectPeakHitAreas(curveArea, []);
                compare(hitAreas.length, 3, "every reported peak must render a marker");
                for (const hitArea of hitAreas) {
                    const dot = hitArea.parent;
                    verify(dot !== null);
                    compare(dot.width, 6, "the visible dot must stay 6x6");
                    compare(dot.height, 6, "the visible dot must stay 6x6");
                    verify(hitArea.width >= Theme.hitAreaMinimum, "the pointer target must reach the hit-area minimum");
                    verify(hitArea.height >= Theme.hitAreaMinimum, "the pointer target must reach the hit-area minimum");
                    verify(hitArea.x >= 0, "the enlarged target must stay inside the curve area");
                    verify(hitArea.x + hitArea.width <= curveArea.width, "the enlarged target must stay inside the curve area");
                    // The target covers the dot's centre, centred wherever the edges allow.
                    const dotCentre = dot.x + dot.width / 2;
                    verify(hitArea.x <= dotCentre && hitArea.x + hitArea.width >= dotCentre);
                }

                // The click still seeks to the peak's own frame.
                seekSpy.clear();
                const firstDot = hitAreas[0].parent;
                mouseClick(curveArea, firstDot.x + firstDot.width / 2, firstDot.y + firstDot.height / 2);
                compare(seekSpy.count, 1);
                compare(seekSpy.signalArguments[0][0], 1);
            } finally {
                metricsFake.peaks = [];
                metricOsc.metrics = null;
                metricOsc.visible = false;
            }
        }

        // I8: the second-step tooltips must promise exactly what ReviewShortcuts performs in
        // the active preset - ±1 s in the frame-review preset, ±30 s in the player preset.
        function test_second_step_tooltips_follow_the_shortcut_preset() {
            osc.revealActive = true;
            const transport = findChild(osc, "transportBar");
            verify(transport !== null);
            const previousSecond = findChild(transport, "previousSecondButton");
            const nextSecond = findChild(transport, "nextSecondButton");
            verify(previousSecond !== null);
            verify(nextSecond !== null);

            osc.shortcutPreset = 0;
            verify(previousSecond.helpText.indexOf(qsTr("后退 1 秒")) >= 0);
            verify(nextSecond.helpText.indexOf(qsTr("前进 1 秒")) >= 0);
            verify(previousSecond.helpText.indexOf("Ctrl+←") >= 0);
            verify(nextSecond.helpText.indexOf("Ctrl+→") >= 0);

            osc.shortcutPreset = 1;
            verify(previousSecond.helpText.indexOf(qsTr("快退 30 秒")) >= 0);
            verify(nextSecond.helpText.indexOf(qsTr("快进 30 秒")) >= 0);
            // The key chords themselves stay truthful in both presets: the player listing must
            // say the Ctrl+A/D chords keep their one-second step instead of claiming them as
            // 30-second aliases (that mismatch was the I8 regression).
            verify(previousSecond.helpText.indexOf("Ctrl+A") >= 0);
            verify(previousSecond.helpText.indexOf(qsTr("仍后退 1 秒")) >= 0);
            verify(nextSecond.helpText.indexOf("Ctrl+D") >= 0);
            verify(nextSecond.helpText.indexOf(qsTr("仍快进 1 秒")) >= 0);
            osc.shortcutPreset = 0;
        }

        // I8 action side: in the player preset the buttons drive the same ±30 s jump the
        // Ctrl+←/→ shortcuts perform; the frame-review preset keeps the exact-second seam.
        function test_player_preset_second_buttons_jump_30_seconds() {
            osc.revealActive = true;
            const previousSecond = findChild(osc, "previousSecondButton");
            const nextSecond = findChild(osc, "nextSecondButton");
            verify(previousSecond !== null);
            verify(nextSecond !== null);

            osc.shortcutPreset = 1;
            actions.stepSecondsCalls = [];
            actions.backwardSecondCalls = 0;
            actions.forwardSecondCalls = 0;
            nextSecond.clicked();
            previousSecond.clicked();
            compare(actions.stepSecondsCalls, [30, -30]);
            compare(actions.backwardSecondCalls, 0);
            compare(actions.forwardSecondCalls, 0);

            osc.shortcutPreset = 0;
            actions.stepSecondsCalls = [];
            actions.backwardSecondCalls = 0;
            actions.forwardSecondCalls = 0;
            nextSecond.clicked();
            previousSecond.clicked();
            compare(actions.stepSecondsCalls, []);
            compare(actions.forwardSecondCalls, 1);
            compare(actions.backwardSecondCalls, 1);
        }

        // E3 ping-pong: the walk chip arms with a complete range, a real click emits the
        // flip intent, and the active face follows the loopRangePingPongActive projection.
        function test_ping_pong_chip_flips_walk_intent_on_same_interval() {
            osc.revealActive = true;
            osc.inFrame = 41;
            osc.outFrame = 47;
            osc.loopRangePingPongActive = false;
            wait(0);
            const chip = findChild(osc, "transportPingPongRangeButton");
            verify(chip !== null);
            tryCompare(chip, "visible", true);
            verify(!chip.chipActive);

            pingPongSpy.clear();
            chip.clicked();
            compare(pingPongSpy.count, 1);

            osc.loopRangePingPongActive = true;
            tryCompare(chip, "chipActive", true);

            // Clearing the endpoints retires the chip with the rest of the working face.
            osc.inFrame = -1;
            osc.outFrame = -1;
            tryCompare(chip, "visible", false);
            osc.loopRangePingPongActive = false;
        }
        // Phase-3 inline range cluster: no endpoints means the row is exactly the two compact
        // mark chips; endpoints expand the working face (capsule + loop/export/clear) while
        // every contract objectName and the range label wording stay intact.
        function test_range_controls_remain_reachable_after_marking_endpoints() {
            osc.revealActive = true;
            osc.playing = false;
            osc.inFrame = -1;
            osc.outFrame = -1;
            wait(0);
            const markIn = findChild(osc, "transportMarkInButton");
            const markOut = findChild(osc, "transportMarkOutButton");
            const playRange = findChild(osc, "transportPlayRangeButton");
            const loopRange = findChild(osc, "transportLoopRangeButton");
            const clearRange = findChild(osc, "transportClearRangeButton");
            const exportChip = findChild(osc, "transportExportRangeButton");
            const label = findChild(osc, "transportRangeLabel");
            verify(markIn !== null);
            verify(markOut !== null);
            verify(playRange !== null);
            verify(loopRange !== null);
            verify(clearRange !== null);
            verify(exportChip !== null);
            verify(label !== null);

            // Unset: the mark chips are the row; the working chips exist but stay hidden.
            verify(markIn.visible);
            verify(markOut.visible);
            verify(!playRange.visible);
            verify(!loopRange.visible);
            verify(!clearRange.visible);
            compare(label.text, qsTr("未设区间（I / O 设点）"));

            // Marking intent still flows from the collapsed chips.
            markInSpy.clear();
            markIn.clicked();
            compare(markInSpy.count, 1);

            // One endpoint expands the working face; the capsule speaks the task language
            // (起点/终点) while the mark chips keep the I / O shortcut in their tooltip.
            osc.inFrame = 41;
            tryCompare(label, "text", qsTr("起 42 · 终 —（区间无效）"));
            tryCompare(loopRange, "visible", true);
            tryCompare(clearRange, "visible", true);
            verify(!playRange.visible, "playback needs both endpoints");

            // A complete range names itself with the frame count, exactly as before.
            osc.outFrame = 47;
            tryCompare(label, "text", qsTr("起 42 · 终 48 · 7 帧"));

            osc.inFrame = -1;
            osc.outFrame = -1;
            tryCompare(markIn, "visible", true);

            const transport = findChild(osc, "transportBar");
            const cluster = findChild(transport, "transportRangeRow");
            const buttonRow = findChild(transport, "firstButton").parent;
            const strip = findChild(transport, "adjacentFramesStrip");
            transport.adjacentExpanded = true;
            for (const width of [960, 1280]) {
                osc.width = width;
                osc.inFrame = 41;
                osc.outFrame = 47;
                waitForRendering(transport);
                verify(markIn.visible && markOut.visible && playRange.visible);
                verify(cluster.y >= buttonRow.y + buttonRow.height - 0.5);
                verify(cluster.width <= transport.width + 0.5);
                verify(buttonRow.width <= transport.width + 0.5);
                const centre = transport.width / 2;
                const clusterRect = cluster.mapToItem(transport, Qt.rect(0, 0, cluster.width, cluster.height));
                const stripRect = strip.mapToItem(transport, Qt.rect(0, 0, strip.width, strip.height));
                verify(Math.abs(clusterRect.x + clusterRect.width / 2 - centre) < 1);
                verify(Math.abs(stripRect.x + stripRect.width / 2 - centre) < 1);
                verify(stripRect.y >= clusterRect.y + clusterRect.height);
                verify(clusterRect.y + clusterRect.height <= transport.height);
            }
            transport.adjacentExpanded = false;
            osc.inFrame = -1;
            osc.outFrame = -1;
            // Restore the shared fixture width for the alphabetically later cases.
            osc.width = root.width;
        }

        // EX-08: the export chip keeps answering while the range is not exportable. The gate
        // stays on `chipEnabled` (so the disabled styling and the C++ contract are unchanged),
        // but a hover names the reason and a click still reaches the host's existing
        // explanation path - which in Main.qml is the intent toast, not a dialog.
        function test_export_chip_explains_an_unavailable_range() {
            osc.revealActive = true;
            osc.playing = false;
            osc.rangeExportVisible = true;
            osc.rangeExportEnabled = false;
            osc.inFrame = -1;
            osc.outFrame = -1;
            wait(0);
            try {
                const chip = findChild(osc, "transportExportRangeButton");
                verify(chip !== null);
                tryVerify(() => chip.visible && chip.width > 0);
                // The row re-centres when the export chip joins it; a click mapped from the
                // pre-reflow position would land where the chip no longer is.
                waitForRendering(chip);
                wait(50);
                verify(!chip.chipEnabled, "an unmarked range keeps the disabled styling");
                verify(chip.chipHelp.indexOf(qsTr("请先设置有效的区间起点和终点")) >= 0,
                    "the hover text must state the reason before the click is spent");

                exportRangeSpy.clear();
                mouseClick(chip, chip.width / 2, chip.height / 2);
                compare(exportRangeSpy.count, 1,
                    "the click must still reach the host's explanation path");

                // Keyboard reaches the same seam: the gate is on the action, not the reason.
                chip.forceActiveFocus(Qt.TabFocusReason);
                verify(chip.activeFocus, "an explainable chip stays tabbable");
                keyClick(Qt.Key_Return);
                compare(exportRangeSpy.count, 2);
            } finally {
                // Retire the reveal before parking the cursor off the controls, in that order:
                // the shared fixture arms its auto-hide countdown on the inside→outside
                // transition, and a countdown left running here would expire inside the next
                // case and hide the bar from under that case's cursor.
                osc.revealActive = false;
                // Hovering the chip raises its help tooltip after its 500 ms delay, and that
                // popup's overlay swallows mouse events until it has finished closing - the next
                // case's clicks would silently land on nothing. Parking the cursor and giving
                // the popup time to retire keeps the shared pointer usable.
                mouseMove(root, 2, 2);
                wait(600);
                osc.inFrame = -1;
                osc.outFrame = -1;
                osc.rangeExportVisible = false;
                osc.rangeExportEnabled = false;
                root.forceActiveFocus();
            }
        }

        // C-07 quiet capsule: a clean run shows the one-line summary inline in the info row
        // and keeps the warning rail off-stage; any real anomaly expands the full rail again,
        // and the full text stays one hover away in both states.
        function test_status_quiet_capsule_when_run_is_clean() {
            osc.revealActive = true;
            osc.playbackModeLabel = "流畅优先";
            osc.playbackModeDetail = "保持原速；解码跟不上时整组跳帧";
            osc.playbackTargetRate = 1.0;
            osc.playbackPresentationRate = 1.0;
            osc.playbackRunSkippedFrameSets = 0;
            osc.playbackRunPresentedFrames = 120;
            osc.playbackLagMilliseconds = 0;
            osc.playbackCatchingUp = false;
            osc.sourceDuplicateCount = 0;
            osc.displayGapCount = 0;
            osc.playing = true;
            const status = findChild(osc, "playbackStatusText");
            const quiet = findChild(osc, "playbackStatusQuiet");
            verify(status !== null);
            verify(quiet !== null);

            verify(!osc.playbackStatusAlert, "a measured run with no skips and no lag is quiet");
            verify(quiet.visible);
            compare(quiet.text, qsTr("流畅优先 · 目标 1.00×"));
            verify(Qt.colorEqual(quiet.color, Theme.mutedText));
            verify(!status.visible, "the warning rail stays off-stage while the run is clean");
            // The quiet capsule hides nothing: the full rail keeps the delivered rate.
            verify(osc.playbackStatusFullText().indexOf("实际 1.00×") >= 0);

            // Any real anomaly swaps the capsule for the warning rail, same vocabulary as
            // before.
            osc.playbackRunSkippedFrameSets = 1;
            tryVerify(() => osc.playbackStatusAlert);
            tryVerify(() => status.visible);
            tryVerify(() => !quiet.visible);
            verify(status.text.indexOf(qsTr("播放器跳过 1 组")) >= 0);
            verify(Qt.colorEqual(status.color, Theme.warning));
        }

        // Phase-3: the metric curve rides behind an explicit readout-row toggle; collapsed
        // keeps the single-frame summary as the always-on readout. The toggle is a Control, so
        // its focus ring follows the visualFocus-only contract: a mouse click stays quiet,
        // keyboard arrival lights the canonical ring.
        function test_metrics_summary_row_toggle_expands_the_lane() {
            metricOsc.visible = true;
            metricOsc.revealActive = true;
            metricsFake.available = true;
            metricsFake.laneEnabled = false;
            metricOsc.metrics = metricsFake;
            try {
                const toggle = findChild(metricOsc, "oscMetricsLaneToggle");
                verify(toggle !== null);
                verify(toggle.visible);
                verify(toggle.text.indexOf(qsTr("展开指标曲线")) >= 0);

                waitForRendering(toggle);
                mouseClick(toggle);
                verify(metricsFake.laneEnabled);
                tryCompare(toggle, "text", qsTr("收起指标曲线 ▴"));
                verify(!toggle.visualFocus, "a mouse click must not light the ring");

                // Keyboard arrival is a focus transition. The click above left the toggle
                // holding active focus with the mouse reason, and forceActiveFocus on an item
                // that already has focus does not rewrite that reason - the same no-op a real
                // user gets, who cannot Tab onto the control they are already on. Drop focus
                // to the root first (the tst_review_action_button pattern) so the Tab reason
                // actually registers.
                root.forceActiveFocus();
                toggle.forceActiveFocus(Qt.TabFocusReason);
                verify(toggle.visualFocus, "keyboard arrival must light the ring");
                root.forceActiveFocus();
            } finally {
                metricOsc.metrics = null;
                metricOsc.visible = false;
            }
        }

        // Phase-3: the adjacent-frame strip is on-demand - collapsed by default, one chip in
        // the button row expands it, and playing closes both the strip and the entry.
        function test_adjacent_frames_strip_is_on_demand() {
            osc.revealActive = true;
            const transport = findChild(osc, "transportBar");
            osc.playing = false;
            osc.currentFrame = 10;
            transport.adjacentExpanded = false;
            wait(0);
            const toggle = findChild(osc, "transportAdjacentToggle");
            const strip = findChild(osc, "adjacentFramesStrip");
            verify(toggle !== null);
            verify(strip !== null);
            verify(toggle.visible);
            verify(!strip.visible, "collapsed by default - the strip costs a line only on ask");
            toggle.clicked();
            tryVerify(() => strip.visible);
            verify(toggle.chipActive);
            // Playing retires the strip and the entry until the next pause.
            osc.playing = true;
            tryVerify(() => !strip.visible);
            tryVerify(() => !toggle.visible);
            osc.playing = false;
            transport.adjacentExpanded = false;
        }

        function collectPeakHitAreas(item, found) {
            for (const child of item.children) {
                if (child.objectName === "peakMarkerHitArea")
                    found.push(child);
                collectPeakHitAreas(child, found);
            }
            return found;
        }
    }
}
