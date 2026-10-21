pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

Item {
    id: control

    property bool canFirst: false
    property bool canPrevious: false
    property bool canPlay: false
    property bool canPause: false
    property bool canNext: false
    property bool canLast: false
    property bool playing: false
    property real playbackRate: 1
    // E04 long-press walk rate. Defaults to real time; the plan's optional 0.5x/2x steps ride on
    // one ladder so the combo's index->value mapping and the emitted rate cannot drift apart.
    property real jogRate: 1
    readonly property var jogRateLadder: [0.5, 1.0, 2.0]
    property Item focusTarget: null
    property bool compact: false
    // UX-13 S1 compact-chrome tier, mirrored from PlayerOsc. The range row stays visible with
    // the always-shown in/out chips plus a summary toggle chip; the working actions (play
    // range, loop, ping-pong, clear, export) stay behind that toggle until asked for.
    property bool compactChrome: false
    property bool rangeExpandedCompact: false
    // Vertical centre of the first transport row inside the control, for hosts that overlay
    // their info line on the row's free right side (PlayerOsc compact tier).
    readonly property real mainRowCenterY: buttons.height / 2
    // I8: the second-step buttons follow the active shortcut preset so their tooltips never
    // promise a different jump than ReviewShortcuts performs. 0 = frame-review preset
    // (Ctrl+←/→ steps one second), 1 = player preset (Ctrl+←/→ jumps 30 seconds). The host
    // routes the button actions off the same value; the bar only reports it.
    property int shortcutPreset: 0
    property int playbackContinuityPolicy: 1
    property int currentFrame: -1
    property int totalFrames: 0
    // In/out range state. The endpoints live on the transport (not only in the inspector) because
    // marking a range and replaying it is part of frame-by-frame review, not a session setting.
    // `rangeControlsVisible` stays opt-in so hosts that do not own range plumbing (the docked
    // TimelineBar) keep rendering the bar unchanged.
    property bool rangeControlsVisible: false
    property bool canMarkRange: false
    property int rangeInFrame: -1
    property int rangeOutFrame: -1
    property bool rangeLoopActive: false
    // E3: back-and-forth range walk toggle; meaningful only while the loop is active.
    property bool rangePingPongActive: false
    readonly property bool hasRangeEndpoints: control.rangeInFrame >= 0 || control.rangeOutFrame >= 0
    readonly property bool hasCompleteRange: control.rangeInFrame >= 0 && control.rangeOutFrame >= control.rangeInFrame
    // The quick-stepping strip exists only on an explicit ask AND a paused session with a
    // current frame; the toggle chip mirrors this to stay honest while playing.
    readonly property bool adjacentStripVisible: control.adjacentExpanded && !control.playing && control.currentFrame >= 0
    // Range clip export. Opt-in like the range row itself so hosts without an export adapter keep
    // the chip hidden. `rangeExportBusy` deliberately does not disable the chip: a click during a
    // running export reopens the dialog so progress and the cancel action stay reachable.
    property bool rangeExportVisible: false
    property bool rangeExportEnabled: false
    property bool rangeExportBusy: false
    property real rangeExportProgress: 0
    // P03 interpolation-protocol navigation. The InterpolationProtocolController context
    // object, injected by the host (optional like the OSC's `metrics`): null keeps the whole
    // protocol row out of the layout, so hosts without the controller render the bar unchanged.
    // Navigation stays an explicit user action — plain playback never consults the protocol.
    property var protocol: null
    // E01 training-sample triplet export context and status.
    property var sampleExport: null
    property bool sampleExportBusy: false
    // Phase-3: the adjacent-frame quick-stepping strip is a secondary paused-review tool, so
    // it rides behind one chip in the button row instead of costing every paused transport a
    // permanent line. Collapsed by default; the strip itself re-checks pause/frame validity.
    property bool adjacentExpanded: false

    signal firstRequested
    signal previousSecondRequested
    signal previousFiveRequested
    signal previousRequested
    // E04 hold-to-jog: pressed while the pointer (or keyboard activation) goes down on the
    // single-frame buttons, released when it comes up. The host maps these to the jog state
    // machine, so holding a button walks exactly like holding its shortcut key.
    signal previousPressed
    signal nextPressed
    signal navigationReleased
    signal playbackRequested
    signal nextRequested
    signal nextFiveRequested
    signal nextSecondRequested
    signal lastRequested
    signal playbackRateRequested(real rate)
    signal jogRateRequested(real rate)
    signal continuityPolicyRequested(int policyCode)
    signal stepFramesRequested(int delta)
    signal markInRequested
    signal markOutRequested
    signal playRangeRequested
    // E3: flip the back-and-forth intent of the active loop range.
    signal rangePingPongRequested
    signal rangeLoopRequested
    signal clearRangeRequested
    signal exportRangeRequested
    signal exportSampleRequested
    signal protocolLoadRequested
    signal protocolClearRequested
    signal previousGeneratedFrameRequested
    signal nextGeneratedFrameRequested

    implicitWidth: transportColumn.implicitWidth
    implicitHeight: transportColumn.implicitHeight

    function restoreFocus() {
        if (focusTarget)
            focusTarget.forceActiveFocus();
    }

    component TransportButton: VcsToolButton {
        id: button

        implicitWidth: control.compact ? 36 : 44
        implicitHeight: control.compact ? Theme.hitAreaMinimum : 38
        iconExtent: control.compact ? 18 : 22
        controlRadius: control.compact ? 6 : 8
        // One tooltip delay for the whole transport: the chips below use the same 500 ms as
        // this default instead of a slower second feel.
        toolTipDelay: 500
        // Centred on the taller play button so every icon in the row shares one centre line.
        anchors.verticalCenter: parent.verticalCenter
    }

    // In/out range chip. Sized from the label text instead of a fixed width so the four short
    // Chinese labels stay narrow. Declares its own `clicked` signal (instead of relying on the
    // MouseArea) so both keyboard handlers and tests can drive a chip without synthesising
    // mouse events.
    component RangeChip: Rectangle {
        id: chip

        property string chipText: ""
        property string chipHelp: ""
        property bool chipEnabled: true
        property bool chipActive: false
        property bool prominent: false
        // A chip whose gate can be explained stays hoverable and pressable while disabled:
        // `chipEnabled` keeps the disabled styling (and stays the contract-tested state), while
        // the pointer and keyboard path still reach the owner's reason instead of going silent.
        property bool explainWhenDisabled: false
        readonly property bool chipInteractive: chip.chipEnabled || chip.explainWhenDisabled
        // Hover feedback for a disabled-but-explainable chip: it lifts one step without
        // pretending the action itself is available.
        readonly property bool explainHover: chip.explainWhenDisabled && !chip.chipEnabled && chipMouse.containsMouse

        signal clicked

        // Keyboard reachability. The transport chips are drawn rectangles rather than Controls,
        // so they declare their own tab stop, activation keys and focus ring. The MouseArea
        // never takes focus, which is what keeps this ring honest: `activeFocus` can only
        // arrive through Tab, so a mouse click cannot light it (the focus contract in
        // VcsTheme.js). Screen readers already got role/name/description; this is the missing
        // way to reach and press them without a pointer (UX-06).
        activeFocusOnTab: chip.chipInteractive
        Accessible.focusable: true
        Accessible.onPressAction: chip.clicked()

        Keys.onPressed: event => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                event.accepted = true;
                if (chip.chipInteractive)
                    chip.clicked();
            }
        }

        implicitWidth: chipLabel.implicitWidth + (control.compact ? 16 : 20)
        implicitHeight: control.compact ? Theme.hitAreaMinimum : 36
        radius: Theme.radiusMedium
        // Neutral chips read as ordinary controls; export keeps a green edge because it is the only
        // chip that writes a file.
        color: !chip.chipEnabled ? (chip.explainHover ? Theme.controlHover : Theme.disabledPanel) : (chip.chipActive ? Theme.accentFill : (chipMouse.containsMouse ? Theme.controlHover : Theme.control))
        // Flat by default: the fill and the label carry the state. Only the active range keeps
        // an accent hairline, because it marks a selected interval rather than a hover.
        border.color: !chip.chipEnabled ? "transparent" : (chip.chipActive ? Theme.accent : (chip.prominent ? (chipMouse.containsMouse ? Theme.successBorder : Theme.successFill) : "transparent"))
        border.width: 1

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }
        Behavior on border.color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }

        Accessible.role: Accessible.Button
        Accessible.name: chip.chipText
        Accessible.description: chip.chipHelp

        // 2 px focus ring, keyboard arrival only (same contract as ReviewActionButton).
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: chip.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.focus
            visible: chip.activeFocus
        }

        Text {
            id: chipLabel

            anchors.centerIn: parent
            text: chip.chipText
            font.pixelSize: control.compact ? Theme.fontSecondary : Theme.fontBody
            font.weight: chip.chipActive || chip.prominent ? Font.DemiBold : Font.Normal
            color: !chip.chipEnabled ? (chip.explainHover ? Theme.mutedText : Theme.disabledText) : (chip.chipActive ? Theme.inverseText : (chip.prominent ? Theme.success : (chipMouse.containsMouse ? Theme.primaryText : Theme.secondaryText)))
        }

        MouseArea {
            id: chipMouse

            anchors.fill: parent
            // `chipEnabled` gates the action, not the explanation: an explainable chip keeps
            // receiving hover and clicks so the host can say why the action is unavailable.
            enabled: chip.chipInteractive
            hoverEnabled: true
            cursorShape: chip.chipInteractive ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: {
                chip.clicked();
                control.restoreFocus();
            }
        }

        ToolTip.visible: chip.chipHelp.length > 0 && chipMouse.containsMouse
        ToolTip.text: chip.chipHelp
        ToolTip.delay: 500
    }

    // One segment of the playback continuity switch. Same toggle language as the compare mode
    // bar: transparent at rest, a quiet wash on hover, and the checked tint with an accent edge.
    component ContinuityOption: Rectangle {
        id: option

        property int policyCode: 0
        property string optionText: ""
        property string optionHelp: ""
        readonly property bool isSelected: control.playbackContinuityPolicy === option.policyCode

        implicitWidth: control.compact ? 62 : 70
        implicitHeight: control.compact ? Theme.hitAreaMinimum : 36
        radius: Theme.radiusSmall
        color: option.isSelected ? Theme.controlChecked : (optionMouse.pressed ? Theme.fluentPressed : (optionMouse.containsMouse ? Theme.fluentHover : "transparent"))
        border.width: 1
        border.color: "transparent"

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }

        Accessible.role: Accessible.RadioButton
        Accessible.name: option.optionText
        Accessible.description: option.optionHelp
        Accessible.checkable: true
        Accessible.checked: option.isSelected
        // Same tab-stop and activation rule as RangeChip: the drawn pill becomes reachable
        // and pressable without a pointer. Each segment is its own stop - two segments make
        // the group cheap to traverse, and a roving tab stop would need group-level state the
        // bar does not otherwise keep.
        activeFocusOnTab: true
        Accessible.focusable: true
        Accessible.onPressAction: control.continuityPolicyRequested(option.policyCode)

        Keys.onPressed: event => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                event.accepted = true;
                control.continuityPolicyRequested(option.policyCode);
            }
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: option.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.focus
            visible: option.activeFocus
        }

        Text {
            anchors.centerIn: parent
            text: option.optionText
            font.pixelSize: control.compact ? Theme.fontSecondary : Theme.fontBody
            font.weight: option.isSelected ? Font.DemiBold : Font.Normal
            color: option.isSelected ? Theme.primaryText : (optionMouse.containsMouse ? Theme.primaryText : Theme.mutedText)
        }

        MouseArea {
            id: optionMouse

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                control.continuityPolicyRequested(option.policyCode);
                control.restoreFocus();
            }
        }

        ToolTip.visible: option.optionHelp.length > 0 && optionMouse.containsMouse
        ToolTip.text: option.optionHelp
        ToolTip.delay: 500
    }

    Column {
        id: transportColumn

        // One group (the button row) per line; the group step keeps four stacked rows from
        // reading as a single dense block. The compact tier spends the micro step between the
        // rows instead - giving those pixels back to the canvas is the tier's whole purpose.
        spacing: control.compactChrome ? Theme.spaceMicro : Theme.spaceMain
        width: control.width
        anchors.horizontalCenter: parent.horizontalCenter

        Row {
            id: buttons

            anchors.horizontalCenter: parent.horizontalCenter
            // Compact tier left-aligns the row so the host's info line can overlay the free
            // right side without crowding the buttons.
            anchors.horizontalCenterOffset: control.compactChrome ? Theme.spaceMain * 2 + implicitWidth / 2 - parent.width / 2 : 0

            spacing: control.compact ? 4 : 6

            TransportButton {
                id: firstButton

                objectName: "firstButton"
                iconSource: "qrc:/icons/first.svg"
                helpText: qsTr("第一帧\n快捷键：Home")
                enabled: control.canFirst
                onClicked: {
                    control.firstRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: previousSecondButton

                objectName: "previousSecondButton"
                iconSource: "qrc:/icons/previous-second.svg"
                // I8: preset-bound wording. The player preset turns this control (and its
                // Ctrl+← shortcut) into the ±30 s jump; the frame-review preset keeps the
                // exact second. Ctrl+A always stays the one-second chord, so the player
                // listing must not present it as a 30-second alias.
                helpText: control.shortcutPreset === 1 ? qsTr("快退 30 秒\n快捷键：Ctrl+←（Ctrl+A 仍后退 1 秒）") : qsTr("后退 1 秒\n快捷键：Ctrl+← / Ctrl+A")
                enabled: control.canPrevious
                onClicked: {
                    control.previousSecondRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: previousFiveButton

                objectName: "previousFiveButton"
                iconSource: "qrc:/icons/previous-five.svg"
                helpText: qsTr("后退 5 帧\n快捷键：↓ / Shift+← / Shift+A")
                enabled: control.canPrevious
                onClicked: {
                    control.previousFiveRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: previousButton

                // Guards the click/press split: mouse presses step via onPressed, keyboard
                // activation (Space/Enter) has no press phase and steps via onClicked.
                // jogArmed spans the hold; jogPressed survives the release handlers because
                // released() reaches the button before clicked() and the click must still be
                // able to tell "mouse already stepped" from "keyboard activation".
                property bool jogArmed: false
                property bool jogPressed: false

                objectName: "previousButton"
                iconSource: "qrc:/icons/previous.svg"
                // The player preset adds the , alias (JogKeyHandler owns it while active).
                helpText: control.shortcutPreset === 1 ? qsTr("上一帧（长按可连续逐帧）\n快捷键：← / A / ,") : qsTr("上一帧（长按可连续逐帧）\n快捷键：← / A")
                enabled: control.canPrevious
                onPressed: {
                    previousButton.jogArmed = true;
                    previousButton.jogPressed = true;
                    control.previousPressed();
                    // No restoreFocus here: a focus change while the button is held makes
                    // AbstractButton cancel the press, which ends the hold after one frame.
                    // The click path below restores focus after the release.
                }
                onReleased: {
                    // Idempotent: some synthesized releases deliver both released and canceled,
                    // and either path alone must end the hold exactly once. jogPressed stays set
                    // here on purpose: released() arrives before clicked(), and the click path
                    // below still needs to tell "the mouse already stepped" from a keyboard
                    // activation.
                    if (previousButton.jogArmed) {
                        previousButton.jogArmed = false;
                        control.navigationReleased();
                    }
                }
                onCanceled: {
                    // Released off-button or stolen by a grab: still end the hold. No clicked
                    // follows a canceled press, so this is the only path that can clear the flag -
                    // leaving it set would make the next keyboard activation believe the mouse
                    // had already stepped and silently drop one frame.
                    if (previousButton.jogArmed) {
                        previousButton.jogArmed = false;
                        control.navigationReleased();
                    }
                    previousButton.jogPressed = false;
                }
                onClicked: {
                    // Keyboard activation (Space/Enter) fires clicked without a press/release;
                    // mouse clicks already stepped via onPressed, so only the keyboard path
                    // steps here.
                    if (!previousButton.jogPressed)
                        control.previousRequested();
                    previousButton.jogPressed = false;
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: playbackButton

                objectName: "playbackButton"
                prominent: true
                implicitWidth: control.compact ? 48 : 56
                implicitHeight: control.compact ? Theme.playButtonHeight : Theme.playButtonHeightLarge
                iconExtent: control.compact ? 20 : 24
                controlRadius: control.compact ? 20 : 22
                iconSource: control.playing ? "qrc:/icons/pause.svg" : "qrc:/icons/play.svg"
                helpText: control.playing ? qsTr("暂停\n快捷键：空格") : qsTr("播放\n快捷键：空格")
                enabled: control.playing ? control.canPause : control.canPlay
                onClicked: {
                    control.playbackRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: nextButton

                // Same jog/jogPressed split as previousButton.
                property bool jogArmed: false
                property bool jogPressed: false

                objectName: "nextButton"
                iconSource: "qrc:/icons/next.svg"
                // The player preset adds the . alias (JogKeyHandler owns it while active).
                helpText: control.shortcutPreset === 1 ? qsTr("下一帧（长按可连续逐帧）\n快捷键：→ / D / .") : qsTr("下一帧（长按可连续逐帧）\n快捷键：→ / D")
                enabled: control.canNext
                onPressed: {
                    nextButton.jogArmed = true;
                    nextButton.jogPressed = true;
                    control.nextPressed();
                    // Same guard as previousButton: no focus change during the hold.
                }
                onReleased: {
                    // jogPressed stays set on purpose: released() arrives before clicked(), and
                    // the click path below still needs the "mouse already stepped" marker.
                    if (nextButton.jogArmed) {
                        nextButton.jogArmed = false;
                        control.navigationReleased();
                    }
                }
                onCanceled: {
                    // No clicked follows a canceled press, so this is the only path that can
                    // clear the flag; leaving it set would silently drop the next keyboard step.
                    if (nextButton.jogArmed) {
                        nextButton.jogArmed = false;
                        control.navigationReleased();
                    }
                    nextButton.jogPressed = false;
                }
                onClicked: {
                    if (!nextButton.jogPressed)
                        control.nextRequested();
                    nextButton.jogPressed = false;
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: nextFiveButton

                objectName: "nextFiveButton"
                iconSource: "qrc:/icons/next-five.svg"
                helpText: qsTr("前进 5 帧\n快捷键：↑ / Shift+→ / Shift+D")
                enabled: control.canNext
                onClicked: {
                    control.nextFiveRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: nextSecondButton

                objectName: "nextSecondButton"
                iconSource: "qrc:/icons/next-second.svg"
                // I8: same preset binding as previousSecondButton, mirrored for the forward
                // jump. Ctrl+D always stays the one-second chord, like Ctrl+A.
                helpText: control.shortcutPreset === 1 ? qsTr("快进 30 秒\n快捷键：Ctrl+→（Ctrl+D 仍快进 1 秒）") : qsTr("前进 1 秒\n快捷键：Ctrl+→ / Ctrl+D")
                enabled: control.canNext
                onClicked: {
                    control.nextSecondRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: lastButton

                objectName: "lastButton"
                iconSource: "qrc:/icons/last.svg"
                helpText: qsTr("最后一帧\n快捷键：End")
                enabled: control.canLast
                onClicked: {
                    control.lastRequested();
                    control.restoreFocus();
                }
            }
            ToolbarCombo {
                id: playbackRateCombo

                objectName: "playbackRateCombo"
                // Sized from the widest rate label: a fixed 58/68 px left the current rate elided to "…".
                implicitWidth: Math.ceil(rateLabelMetrics.advanceWidth) + playbackRateCombo.leftPadding + playbackRateCombo.rightPadding + 4
                // Same height as the continuity switch beside it, so the row's boxed controls line up.
                implicitHeight: control.compact ? Theme.hitAreaMinimum : 36
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("0.25×"), qsTr("0.5×"), qsTr("1×"), qsTr("1.5×"), qsTr("2×"), qsTr("4×")]
                currentIndex: {
                    const rate = control.playbackRate;
                    if (rate < 0.375)
                        return 0;
                    if (rate < 0.75)
                        return 1;
                    if (rate < 1.25)
                        return 2;
                    if (rate < 1.75)
                        return 3;
                    if (rate < 3)
                        return 4;
                    return 5;
                }
                Accessible.name: qsTr("播放速度")
                onActivated: index => {
                    const ladder = [0.25, 0.5, 1, 1.5, 2, 4];
                    control.playbackRateRequested(ladder[index]);
                    control.restoreFocus();
                }

                TextMetrics {
                    id: rateLabelMetrics

                    font: playbackRateCombo.font
                    text: qsTr("0.25×")
                }
            }

            // E04 walk-rate selector for the long-press step walk. The labels carry the 长按逐帧
            // prefix because a bare "1×" beside the playback-rate combo would be ambiguous, and
            // the prefix names the gesture the rate applies to.
            ToolbarCombo {
                id: jogRateCombo

                objectName: "jogRateCombo"
                implicitWidth: Math.ceil(jogRateLabelMetrics.advanceWidth) + jogRateCombo.leftPadding + jogRateCombo.rightPadding + 4
                implicitHeight: control.compact ? Theme.hitAreaMinimum : 36
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("长按逐帧 0.5×"), qsTr("长按逐帧 1×"), qsTr("长按逐帧 2×")]
                currentIndex: {
                    // Nearest ladder value: the controller clamps to the same [0.5, 2.0] range, so
                    // the projection never has to invent an index outside the three labels.
                    const ladder = control.jogRateLadder;
                    let best = 0;
                    let bestDistance = Math.abs(ladder[0] - control.jogRate);
                    for (let index = 1; index < ladder.length; ++index) {
                        const distance = Math.abs(ladder[index] - control.jogRate);
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            best = index;
                        }
                    }
                    return best;
                }
                Accessible.name: qsTr("长按连续逐帧的速率")
                onActivated: index => {
                    control.jogRateRequested(control.jogRateLadder[index]);
                    control.restoreFocus();
                }

                TextMetrics {
                    id: jogRateLabelMetrics

                    font: jogRateCombo.font
                    text: qsTr("长按逐帧 0.5×")
                }
            }

            // Secondary paused-review tool, collapsed behind this chip (Phase-3): the strip
            // costs a layout line, so it only exists while the user asked for it.
            RangeChip {
                id: adjacentToggle

                objectName: "transportAdjacentToggle"
                visible: !control.playing && control.currentFrame >= 0
                width: visible ? implicitWidth : 0
                chipText: control.adjacentStripVisible ? qsTr("相邻帧 ▴") : qsTr("相邻帧 ▾")
                chipHelp: qsTr("展开或收起相邻帧快捷条（±3 帧一步直达；仅暂停时可用）")
                chipEnabled: true
                chipActive: control.adjacentStripVisible
                onClicked: control.adjacentExpanded = !control.adjacentExpanded
            }

            // Segmented switch: both options share one well so they read as a single choice.
            Rectangle {
                id: continuityToggleGroup

                objectName: "continuityPolicyToggleGroup"
                implicitWidth: continuityGroup.implicitWidth + 6
                implicitHeight: continuityGroup.implicitHeight + 6
                radius: Theme.radiusMedium
                color: Theme.window
                border.width: 1
                border.color: Theme.controlBorder
                anchors.verticalCenter: parent.verticalCenter

                Row {
                    id: continuityGroup

                    spacing: 2
                    anchors.centerIn: parent

                    ContinuityOption {
                        objectName: "realTimeContinuityButton"
                        policyCode: 1
                        optionText: qsTr("流畅优先")
                        optionHelp: qsTr("保持实时节奏；解码跟不上时整组跳帧")
                    }

                    ContinuityOption {
                        objectName: "reviewEveryFrameContinuityButton"
                        policyCode: 0
                        optionText: qsTr("逐帧完整")
                        optionHelp: qsTr("每一帧都显示，不跳帧；负载高时播放会变慢")
                    }
                }
            }

            // UX-13 S1 compact tier: the range state rides this chip so collapsing the working
            // actions never hides "is a range set". The in/out chips stay outside the toggle -
            // always visible per the product's 2026-10-16 user revision.
            RangeChip {
                objectName: "transportRangeSummaryCompact"
                visible: control.compactChrome
                anchors.verticalCenter: parent.verticalCenter
                chipActive: control.rangeExpandedCompact
                chipText: {
                    if (!control.hasRangeEndpoints)
                        return qsTr("区间 ▾");
                    const inText = control.rangeInFrame >= 0 ? String(control.rangeInFrame + 1) : "—";
                    const outText = control.rangeOutFrame >= 0 ? String(control.rangeOutFrame + 1) : "—";
                    const stateSuffix = control.rangeLoopActive ? qsTr(" · 循环") : (control.rangePingPongActive ? qsTr(" · 往返") : "");
                    // QML's String.arg() is single-argument: a multi-arg call fails at runtime
                    // ("Invalid arguments"), so the substitutions chain.
                    return qsTr("%1–%2%3 %4").arg(inText).arg(outText).arg(stateSuffix).arg(control.rangeExpandedCompact ? "▴" : "▾");
                }
                chipHelp: qsTr("展开或收起区间操作（播放区间、循环、往返、清除与导出）；设点 I / O 始终可见")
                onClicked: control.rangeExpandedCompact = !control.rangeExpandedCompact
            }
        }

        // A stable second row keeps mark, playback and export controls reachable in
        // every range state. Its position never changes when an endpoint is added.
        Row {
            id: rangeCluster

            objectName: "transportRangeRow"
            visible: control.rangeControlsVisible
            // Collapse this row for hosts without range controls.
            height: visible ? implicitHeight : 0
            anchors.horizontalCenter: parent.horizontalCenter
            // The three faces (mark / working / export) are separate groups: the group step
            // separates them while each face keeps its own tight intra-group spacing.
            spacing: Theme.spaceGroup

            // Keep both marking actions after either endpoint has been set.
            Row {
                id: rangeMarkFace

                visible: true
                width: visible ? implicitWidth : 0
                spacing: control.compact ? 4 : 6

                RangeChip {
                    objectName: "transportMarkInButton"
                    // Task language on the chip, the editing term in the tooltip: "in point" is
                    // what the I key is called, "range start" is what the user is doing.
                    chipText: qsTr("设区间起点 I")
                    chipHelp: qsTr("把当前帧设为区间起点（入点）\n快捷键：I")
                    chipEnabled: control.canMarkRange
                    onClicked: control.markInRequested()
                }

                RangeChip {
                    objectName: "transportMarkOutButton"
                    chipText: qsTr("设区间终点 O")
                    chipHelp: qsTr("把当前帧设为区间终点（出点）\n快捷键：O")
                    chipEnabled: control.canMarkRange
                    onClicked: control.markOutRequested()
                }
            }

            // Working face: the range readout as one capsule plus the chips that act on it.
            Row {
                id: rangeWorkingFace

                // Compact tier folds the working face behind the summary chip; the row keeps
                // the always-visible in/out chips.
                visible: control.hasRangeEndpoints && (!control.compactChrome || control.rangeExpandedCompact)
                width: visible ? implicitWidth : 0
                spacing: control.compact ? 4 : 6

                // The single-source range readout, restyled as the capsule that groups the
                // working chips. MainQmlContractTests reads `transportRangeLabel.text` in every
                // range state, including the unset state while this capsule is hidden.
                Rectangle {
                    id: rangeCapsule

                    objectName: "transportRangeCapsule"
                    radius: Theme.radiusMedium
                    implicitHeight: control.compact ? Theme.hitAreaMinimum : 36
                    implicitWidth: Math.min(rangeLabel.implicitWidth, control.compact ? 168 : 210) + 20
                    // A complete range is an interval worth acting on; an invalid pair stays a
                    // quiet panel so it reads as "not a range yet" next to the active chips.
                    color: control.hasCompleteRange ? Theme.control : Theme.disabledPanel
                    border.width: 1
                    border.color: control.hasCompleteRange ? Theme.accent : "transparent"
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        id: rangeLabel

                        objectName: "transportRangeLabel"
                        text: {
                            if (!control.hasRangeEndpoints)
                                return qsTr("未设区间（I / O 设点）");
                            const inText = control.rangeInFrame >= 0 ? String(control.rangeInFrame + 1) : "—";
                            const outText = control.rangeOutFrame >= 0 ? String(control.rangeOutFrame + 1) : "—";
                            if (!control.hasCompleteRange)
                                return qsTr("起 %1 · 终 %2（区间无效）").arg(inText).arg(outText);
                            const frameCount = control.rangeOutFrame - control.rangeInFrame + 1;
                            return qsTr("起 %1 · 终 %2 · %3 帧%4").arg(inText).arg(outText).arg(frameCount).arg(control.rangeLoopActive ? qsTr(" · 循环") : "");
                        }
                        color: control.hasCompleteRange ? Theme.accentText : Theme.mutedText
                        font.family: Theme.fontFamilyMono
                        font.pixelSize: Theme.fontSecondary
                        // Cap instead of overflowing: the cluster has no clip, and a three-digit
                        // frame count still fits, so elision only ever triggers on an
                        // unexpectedly narrow window.
                        width: Math.min(implicitWidth, control.compact ? 168 : 210)
                        elide: Text.ElideRight
                        anchors.centerIn: parent
                    }
                }

                RangeChip {
                    objectName: "transportPlayRangeButton"
                    visible: control.hasCompleteRange
                    chipText: qsTr("播放区间")
                    chipHelp: qsTr("播放区间起点到终点\n快捷键：\\")
                    chipEnabled: control.hasCompleteRange
                    onClicked: control.playRangeRequested()
                }

                RangeChip {
                    objectName: "transportLoopRangeButton"
                    chipText: qsTr("循环 \\")
                    chipHelp: control.rangeLoopActive ? qsTr("停止循环，改为单次播放区间") : qsTr("循环播放区间起点到终点；\\ 键单次播放区间")
                    chipEnabled: control.hasCompleteRange
                    chipActive: control.rangeLoopActive
                    onClicked: control.rangeLoopRequested()
                }

                RangeChip {
                    objectName: "transportPingPongRangeButton"
                    // E3: the click flips the back-and-forth intent on the SAME interval;
                    // available whenever a complete range exists (armed even while paused,
                    // so a loop can be configured before playback starts).
                    visible: control.hasCompleteRange
                    chipText: qsTr("往返")
                    chipHelp: control.rangePingPongActive ? qsTr("往返开启中：到区间终点后倒放返回起点；点击恢复单向循环") : qsTr("循环到区间终点后倒放返回起点，端点不重复")
                    chipEnabled: control.hasCompleteRange
                    chipActive: control.rangePingPongActive
                    onClicked: control.rangePingPongRequested()
                }

                RangeChip {
                    objectName: "transportClearRangeButton"
                    chipText: qsTr("清除")
                    chipHelp: qsTr("清除区间起点与终点")
                    chipEnabled: control.hasRangeEndpoints
                    onClicked: control.clearRangeRequested()
                }
            }

            // Export chip, hoisted out of the two faces (the MainQmlContractTests contract):
            // it stays visible in BOTH faces whenever the host publishes an export service.
            // With no endpoints it is visible but not enabled, and `explainWhenDisabled` keeps
            // hover and clicks alive: the click still reports the intent, the host's guard
            // answers with the reason in the existing intent toast, and no dialog opens. Once a
            // range is marked the chip enables and opens the clip export dialog. Hoisting also
            // keeps the chip off the mark/working face flip, so its geometry never rides that
            // relayout and a real mouse click cannot land on stale coordinates. Trailing
            // position matches the pre-cluster flat row (…, 清除, 导出).
            RangeChip {
                objectName: "transportExportRangeButton"
                // Compact tier folds the export chip behind the summary chip with the rest of
                // the working face; its explain-on-click contract keeps working once expanded.
                visible: control.rangeExportVisible && (!control.compactChrome || control.rangeExpandedCompact)
                prominent: true
                chipText: control.rangeExportBusy ? qsTr("导出中 %1%").arg(Math.round(control.rangeExportProgress * 100)) : qsTr("导出")
                // The hover text carries the gate before the click is spent on it.
                chipHelp: control.rangeExportEnabled || control.rangeExportBusy ? qsTr("把区间起点到终点导出为无损片段（起点对齐到前一个关键帧）") : qsTr("暂不可导出：请先设置有效的区间起点和终点")
                chipEnabled: control.rangeExportEnabled || control.rangeExportBusy
                explainWhenDisabled: true
                chipActive: control.rangeExportBusy
                onClicked: control.exportRangeRequested()
            }
        }

        // P03 protocol navigation row: jump to the next/previous generated output position and
        // read the current frame's provenance. The row exists only when a protocol controller is
        // wired; the navigation chips additionally require a loaded protocol, so with no protocol
        // imported nothing here can seek. The badge is text-first so provenance never relies on
        // colour alone.
        Row {
            id: protocolRow

            objectName: "transportProtocolRow"
            visible: control.protocol !== null
            // Column positioners reserve a slot for invisible children; collapse so hosts
            // without a protocol controller lose the row's height, not just its paint.
            height: visible ? implicitHeight : 0
            spacing: control.compact ? 4 : 6
            anchors.horizontalCenter: parent.horizontalCenter

            RangeChip {
                objectName: "transportLoadProtocolButton"
                chipText: qsTr("导入协议")
                chipHelp: qsTr("从 JSON 清单导入插帧协议（decimation 或 explicit）")
                onClicked: control.protocolLoadRequested()
            }

            RangeChip {
                objectName: "transportPreviousGeneratedButton"
                visible: Boolean(control.protocol) && Boolean(control.protocol.protocolLoaded)
                chipText: qsTr("上一生成帧")
                chipHelp: qsTr("跳到上一个由模型生成的帧（跳过保留帧、缺帧与未知来源）")
                chipEnabled: Boolean(control.protocol) && Boolean(control.protocol.canGoPreviousGenerated)
                onClicked: control.previousGeneratedFrameRequested()
            }

            RangeChip {
                objectName: "transportNextGeneratedButton"
                visible: Boolean(control.protocol) && Boolean(control.protocol.protocolLoaded)
                chipText: qsTr("下一生成帧")
                chipHelp: qsTr("跳到下一个由模型生成的帧（跳过保留帧、缺帧与未知来源）")
                chipEnabled: Boolean(control.protocol) && Boolean(control.protocol.canGoNextGenerated)
                onClicked: control.nextGeneratedFrameRequested()
            }

            RangeChip {
                objectName: "transportExportSampleButton"
                visible: Boolean(control.sampleExport) && Boolean(control.protocol) && Boolean(control.protocol.protocolLoaded)
                chipText: qsTr("导出样本…")
                chipHelp: qsTr("导出当前帧对应的训练三元组（input0、GT、input1）与推理帧")
                chipEnabled: Boolean(control.sampleExport) && control.currentFrame >= 0 && Boolean(control.sampleExport.canExportAtFrame(control.currentFrame)) && !control.sampleExportBusy
                onClicked: control.exportSampleRequested()
            }

            RangeChip {
                objectName: "transportClearProtocolButton"
                visible: Boolean(control.protocol) && Boolean(control.protocol.protocolLoaded)
                chipText: qsTr("关闭协议")
                chipHelp: qsTr("关闭协议导航，回到普通审查")
                chipEnabled: true
                onClicked: control.protocolClearRequested()
            }

            Text {
                id: protocolBadge

                objectName: "transportProtocolBadge"
                visible: Boolean(control.protocol) && text.length > 0
                text: {
                    if (!control.protocol)
                        return "";
                    if (control.protocol.lastError.length > 0)
                        return qsTr("协议错误：%1").arg(control.protocol.lastError);
                    if (!control.protocol.protocolLoaded)
                        return control.protocol.statusText;
                    return control.protocol.currentOriginBadge.length > 0 ? control.protocol.currentOriginBadge + " · " + control.protocol.statusText : control.protocol.statusText;
                }
                color: control.protocol && control.protocol.lastError.length > 0 ? Theme.warning : Theme.mutedText
                font.pixelSize: Theme.fontSecondary
                maximumLineCount: 1
                elide: Text.ElideRight
                // Cap instead of overflowing: a generated-frame badge carries two GT indices and
                // a ratio, which must elide rather than push the chips off the panel.
                width: Math.min(implicitWidth, control.compact ? 260 : 320)
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            id: adjacentStrip

            objectName: "adjacentFramesStrip"
            visible: control.adjacentStripVisible
            // Collapses while hidden or while playing: positioners keep layout space for
            // invisible children otherwise.
            height: visible ? implicitHeight : 0
            // 6 px between the label and the cells (and between the cells themselves) so a
            // 30 px cell still reads as a separate target, not a wall of buttons.
            spacing: 6
            anchors.horizontalCenter: parent.horizontalCenter

            Text {
                text: qsTr("相邻帧：")
                font.pixelSize: Theme.fontSecondary
                color: Theme.mutedText
                anchors.verticalCenter: parent.verticalCenter
            }

            Repeater {
                model: [-3, -2, -1, 0, 1, 2, 3]

                Rectangle {
                    id: adjacentItem
                    required property int modelData
                    readonly property int delta: adjacentItem.modelData
                    readonly property int targetFrame: control.currentFrame + adjacentItem.delta
                    readonly property bool isCurrent: adjacentItem.delta === 0
                    readonly property bool isValid: adjacentItem.targetFrame >= 0 && (control.totalFrames <= 0 || adjacentItem.targetFrame < control.totalFrames)

                    implicitWidth: adjacentItem.isCurrent ? 58 : 30
                    // 28 px keeps the pointer target at the hit-area minimum while the row
                    // still stays one compact line under the transport.
                    implicitHeight: 28
                    radius: Theme.radiusSmall
                    color: adjacentItem.isCurrent ? Theme.accentFill : (!adjacentItem.isValid ? Theme.disabledPanel : (adjacentMouse.containsMouse ? Theme.controlHover : Theme.control))
                    border.color: adjacentItem.isCurrent ? Theme.accent : (!adjacentItem.isValid ? Theme.disabledBorder : (adjacentMouse.containsMouse ? Theme.borderHover : Theme.controlBorder))
                    opacity: adjacentItem.isValid ? 1.0 : 0.4

                    Text {
                        anchors.centerIn: parent
                        text: adjacentItem.isCurrent ? qsTr("当前 %1").arg(adjacentItem.targetFrame + 1) : (adjacentItem.delta > 0 ? "+" + adjacentItem.delta : String(adjacentItem.delta))
                        font.pixelSize: Theme.fontSecondary
                        font.weight: adjacentItem.isCurrent ? Font.DemiBold : Font.Normal
                        color: adjacentItem.isCurrent ? Theme.inverseText : (adjacentMouse.containsMouse ? Theme.primaryText : Theme.mutedText)
                    }

                    MouseArea {
                        id: adjacentMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !adjacentItem.isCurrent && adjacentItem.isValid
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            control.stepFramesRequested(adjacentItem.delta);
                            control.restoreFocus();
                        }
                    }
                }
            }
        }
    }
}
