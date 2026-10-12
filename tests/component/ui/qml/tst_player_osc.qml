import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

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

        function firstFrame() {
        }
        function stepBackwardSecond() {
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

        function test_button_hold_event_order_has_one_press_and_one_release() {
            orderedOsc.visible = true;
            const previousButton = findChild(orderedOsc, "previousButton");
            verify(previousButton !== null);
            orderedActions.events = [];

            mousePress(previousButton);
            mouseRelease(previousButton);
            compare(orderedActions.events, ["jogPress:-1", "jogRelease"]);
            orderedOsc.visible = false;
        }

        // E04 walk-rate completion: the 走带 combo mirrors the controller projection and its
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
    }
}
