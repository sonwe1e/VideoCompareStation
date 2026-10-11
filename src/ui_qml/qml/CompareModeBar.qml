pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme
// qmllint disable import
import Dvs.Ui 1.0

// qmllint enable import

Rectangle {
    id: control

    required property int sourceCount
    required property int currentMode
    required property int differenceMetric
    required property var differenceEdges
    required property int currentEdgeIndex
    required property bool inspectorOpen
    required property bool busy
    // The keyboard lock outlives the button press, so the button has to say so.
    required property bool peekLocked
    // Set while the session has no identifiable reference (GT) source; the peek is offered by
    // the keyboard only when this is false, and the button explains itself instead of lying.
    required property bool peekTargetValid
    // V02 (P05): session slot the full canvas is focused on (-1 = regular comparison view),
    // and the slot carrying the reference role. Both default to "unknown" so an unwired bar
    // stays inert instead of checking an entry at random; the contract tests cross-check the
    // bindings once Main.qml wires them.
    property int canvasFocusSlot: -1
    property int referenceSourceSlot: -1
    property color panelColor: Theme.menu
    property color borderColor: Theme.border
    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText

    signal modeRequested(int mode)
    signal edgeRequested(int preferenceValue)
    // Step-2 difference entry: one control offers the two flavors the review named —
    // overlay-highlight to locate defects on the original, pure diff for distribution.
    signal differenceViewRequested(int metric)
    // Hold-to-peek: while held the surface replaces the difference pass with the reference (GT)
    // source of the session; releasing restores the difference. Same target the keyboard peek
    // and the viewport indicator name.
    signal differencePeekChanged(bool held)
    // Step-2 "固定 GT 切候选": one action flips the candidate side against the fixed
    // reference; Main keeps the frame, zoom/pan and wipe split untouched.
    signal switchCandidateRequested
    // V02 (P05) full-canvas direct look: one request switches which participating source the
    // whole canvas shows. Same frame, same ROI/zoom/pan, no seek, no GT role change, no
    // re-pairing - presentation state only. requestCode follows ReviewShellController's
    // CanvasFocusRequest (0 off, 1 reference, 2 session slot); sourceSlot addresses the
    // session order (A=0, B=1). Re-clicking the active entry requests Off.
    signal fullCanvasFocusRequested(int requestCode, int sourceSlot)
    // Step-2 comparison export: one action copies the labeled comparison capture.
    signal copyComparisonRequested
    signal inspectorRequested

    // qmllint disable import unqualified unresolved-type
    readonly property bool pairRelevant: currentMode === ComparisonSurface.Wipe || currentMode === ComparisonSurface.Difference || currentMode === ComparisonSurface.AnalysisGrid
    readonly property bool advancedMode: currentMode === ComparisonSurface.ThreeUp || currentMode === ComparisonSurface.ReferenceFocus || currentMode === ComparisonSurface.AnalysisGrid || currentMode === ComparisonSurface.RoiTriplet || currentMode === ComparisonSurface.DualDifference
    readonly property string advancedModeLabel: {
        if (currentMode === ComparisonSurface.ThreeUp)
            return qsTr("三联");
        if (currentMode === ComparisonSurface.ReferenceFocus)
            return qsTr("参考聚焦");
        if (currentMode === ComparisonSurface.AnalysisGrid)
            return qsTr("分析网格");
        if (currentMode === ComparisonSurface.RoiTriplet)
            return qsTr("全景+局部");
        if (currentMode === ComparisonSurface.DualDifference)
            return qsTr("双参考差异");
        return "";
    }
    // The button names the active quick flavor; other metrics (signed/heatmap/...) keep
    // the plain "差异" label and stay selectable from the inspector.
    readonly property string differenceFlavorLabel: currentMode === ComparisonSurface.Difference ? (differenceMetric === ComparisonSurface.Highlight ? qsTr("叠加高亮") : (differenceMetric === ComparisonSurface.RgbAbsolute ? qsTr("纯差异图") : "")) : ""
    // qmllint enable import unqualified unresolved-type

    // V02 (P05): request codes mirroring ReviewShellController::CanvasFocusRequest. Exposed as
    // properties so the emitted values are readable in one place; the shell contract tests
    // cross-check these against the C++ enum so a drift fails a test instead of silently
    // re-targeting the canvas.
    readonly property int canvasFocusRequestOff: 0
    readonly property int canvasFocusRequestReference: 1
    readonly property int canvasFocusRequestSourceSlot: 2

    objectName: "compareModeBar"
    height: sourceCount > 1 ? 40 : 0
    visible: sourceCount > 1
    color: "transparent"
    border.color: "transparent"

    // The mode bar is the last chrome row above the stage; one hairline separates the two.
    Rectangle {
        height: 1
        color: Theme.border
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
    }

    Row {
        spacing: 6
        anchors {
            left: parent.left
            leftMargin: 12
            verticalCenter: parent.verticalCenter
        }

        // qmllint disable import unqualified unresolved-type
        ModeButton {
            objectName: "sideModeButton"
            text: qsTr("并排")
            modeValue: ComparisonSurface.SideBySide
        }
        ModeButton {
            objectName: "wipeModeButton"
            text: qsTr("分割线")
            modeValue: ComparisonSurface.Wipe
        }
        // Step-2 difference entry: the button itself offers the flavors the review named
        // (overlay highlight / pure diff), mirroring the image workspace's diff dropdown.
        DiffModeButton {
            objectName: "diffModeButton"
            text: control.differenceFlavorLabel.length > 0 ? qsTr("差异·%1 ▾").arg(control.differenceFlavorLabel) : qsTr("差异 ▾")
        }
        // Hold-to-peek: while held the surface shows the reference (GT) source instead of the
        // difference pass; releasing restores the difference view. The label names that target
        // and stays visible while the keyboard lock holds it.
        VcsToolButton {
            id: differencePeekButton

            objectName: "differencePeekButton"
            text: control.peekLocked ? qsTr("GT 原图·已锁定") : qsTr("按住看 GT 原图")
            visible: control.currentMode === ComparisonSurface.Difference
            enabled: !control.busy && control.peekTargetValid
            prominent: pressed || control.peekLocked
            implicitWidth: 132
            implicitHeight: 30
            labelPixelSize: 12
            Accessible.description: control.peekTargetValid ? qsTr("按住显示会话的参考（GT）源，即使 GT 不在当前比较对中；松开恢复差异视图。键盘 ` 切换，Shift+` 锁定。") : qsTr("当前会话没有可直看的参考（GT）源。")
            onPressed: control.differencePeekChanged(true)
            onReleased: control.differencePeekChanged(false)
            onCanceled: control.differencePeekChanged(false)
        }
        // qmllint enable import unqualified unresolved-type

        Rectangle {
            visible: control.sourceCount === 3 && control.pairRelevant
            width: 1
            height: 20
            color: Theme.border
            anchors.verticalCenter: parent.verticalCenter
        }

        ToolbarCombo {
            id: pairComboBox

            objectName: "pairCombo"
            visible: control.sourceCount === 3 && control.pairRelevant
            implicitWidth: 92
            implicitHeight: 30
            // V02: the selector names the two real files ("gt.mp4 ↔ model.mp4") instead of the
            // A/B/C slot letters, so a same-letter relabel can never masquerade as a pair
            // switch. The role keeping the letters ("label") stays for the inspector rows.
            model: control.differenceEdges
            textRole: "pairLabel"
            currentIndex: control.currentEdgeIndex
            Accessible.name: qsTr("对比对")
            hoverEnabled: true
            leftPadding: 10
            rightPadding: 26
            font.pixelSize: 12
            onActivated: index => {
                if (index >= 0 && index < control.differenceEdges.length)
                    control.edgeRequested(Number(control.differenceEdges[index].preferenceValue));
            }
        }

        VcsToolButton {
            id: switchCandidateButton

            objectName: "switchCandidateButton"
            text: qsTr("切候选")
            visible: control.sourceCount === 3 && control.pairRelevant
            enabled: !control.busy
            implicitWidth: 78
            implicitHeight: 30
            labelPixelSize: 12
            Accessible.name: qsTr("固定参考切换候选")
            onClicked: control.switchCandidateRequested()
        }

        // V02: one direct-look entry per participating source, generated from the session
        // (A/B/C). The source that currently carries the reference (GT) role shows a GT badge
        // on its own button - GT is a role, never a fourth source - so "GT = A" never
        // duplicates the A entry and C gets the same entry A and B have. A click addresses
        // the session slot; re-clicking the checked entry requests Off and returns to the
        // comparison view. The switch keeps the frame, ROI/zoom/pan, the GT role and every
        // other entry on this bar untouched (P05 canvasFocus chain, no new state).
        Rectangle {
            visible: control.sourceCount > 1
            width: 1
            height: 20
            color: Theme.border
            anchors.verticalCenter: parent.verticalCenter
        }

        Repeater {
            objectName: "canvasFocusButtonRepeater"
            model: Math.max(0, control.sourceCount)

            FocusButton {
                id: canvasFocusButton

                required property int index
                objectName: "canvasFocus" + String.fromCharCode(65 + index) + "Button"
                text: qsTr("看 %1").arg(String.fromCharCode(65 + index))
                focusSlot: index
                referenceBadge: index === control.referenceSourceSlot
            }
        }

        VcsToolButton {
            id: moreCompareModesButton

            objectName: "moreCompareModesButton"
            text: control.advancedMode ? qsTr("… · %1").arg(control.advancedModeLabel) : "…"
            implicitWidth: control.advancedMode ? 128 : 36
            implicitHeight: 30
            onClicked: moreMenu.open()
            labelPixelSize: 12

            VcsMenu {
                id: moreMenu
                menuWidth: 220

                // qmllint disable import unqualified unresolved-type
                VcsRadioMenuItem {
                    id: threeUpMenuItem

                    objectName: "threeUpMenuItem"
                    text: qsTr("三联")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.ThreeUp
                    onTriggered: control.modeRequested(ComparisonSurface.ThreeUp)
                }
                VcsRadioMenuItem {
                    id: referenceFocusMenuItem

                    objectName: "referenceFocusMenuItem"
                    text: qsTr("参考聚焦")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.ReferenceFocus
                    onTriggered: control.modeRequested(ComparisonSurface.ReferenceFocus)
                }
                VcsRadioMenuItem {
                    id: analysisGridMenuItem

                    objectName: "analysisGridMenuItem"
                    text: qsTr("分析网格")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.AnalysisGrid
                    onTriggered: control.modeRequested(ComparisonSurface.AnalysisGrid)
                }
                // V01 task preset: the panorama stays one column per source and the renderer
                // draws the local windows as extra draws of the same presented frame set under
                // one shared ROI. Two sources are the minimum, so the item explains itself as
                // unavailable below that.
                VcsRadioMenuItem {
                    id: roiTripletMenuItem

                    objectName: "roiTripletMenuItem"
                    text: qsTr("全景+局部")
                    enabled: control.sourceCount >= 2
                    checked: control.currentMode === ComparisonSurface.RoiTriplet
                    onTriggered: control.modeRequested(ComparisonSurface.RoiTriplet)
                }
                VcsRadioMenuItem {
                    id: dualDifferenceMenuItem

                    objectName: "dualDifferenceMenuItem"
                    text: qsTr("双参考差异")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.DualDifference
                    onTriggered: control.modeRequested(ComparisonSurface.DualDifference)
                }
                // qmllint enable import unqualified unresolved-type
            }
        }
    }

    component ModeButton: ReviewActionButton {
        id: modeButton

        required property int modeValue
        checkable: true
        checked: control.currentMode === modeValue
        mirrorsState: true
        implicitHeight: 30
        implicitWidth: 64
        enabled: !control.busy
        leftPadding: 10
        rightPadding: 10
        onClicked: control.modeRequested(modeValue)

        contentItem: Text {
            text: modeButton.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !modeButton.enabled ? Theme.disabledText : (modeButton.checked || modeButton.hovered ? Theme.primaryText : Theme.mutedText)
            font.pixelSize: 12
            font.weight: modeButton.checked ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (!modeButton.enabled)
                    return "transparent";
                if (modeButton.checked)
                    return Theme.controlChecked;
                if (modeButton.down)
                    return Theme.fluentPressed;
                if (modeButton.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: modeButton.activeFocus ? 2 : 1
            border.color: {
                if (modeButton.activeFocus)
                    return Theme.focus;
                if (!modeButton.enabled)
                    return "transparent";
                if (modeButton.checked)
                    return Theme.accent;
                if (modeButton.hovered)
                    return Theme.subtleBorder;
                return "transparent";
            }

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: 120
                }
            }
        }
    }

    // V02: one full-canvas direct-look entry per source slot. Checked mirrors the shell's
    // focus slot; clicking the checked entry requests Off so the group always has a way back
    // to the regular comparison view. referenceBadge marks the slot that carries the session's
    // GT role - a badge on the source's own button, never a duplicated GT button.
    component FocusButton: ReviewActionButton {
        id: focusButton

        required property int focusSlot
        property bool referenceBadge: false
        checkable: true
        checked: control.canvasFocusSlot >= 0 && control.canvasFocusSlot === focusSlot
        mirrorsState: true
        implicitHeight: 30
        implicitWidth: focusButton.referenceBadge ? 96 : 64
        enabled: !control.busy
        leftPadding: 10
        rightPadding: 10
        onClicked: {
            if (focusButton.checked)
                control.fullCanvasFocusRequested(control.canvasFocusRequestOff, 0);
            else
                control.fullCanvasFocusRequested(control.canvasFocusRequestSourceSlot, focusSlot);
        }

        contentItem: Row {
            spacing: 4

            Text {
                text: focusButton.text
                color: !focusButton.enabled ? Theme.disabledText : (focusButton.checked || focusButton.hovered ? Theme.primaryText : Theme.mutedText)
                font.pixelSize: 12
                font.weight: focusButton.checked ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                visible: focusButton.referenceBadge
                width: gtBadgeLabel.implicitWidth + 8
                height: 16
                radius: 3
                color: Theme.controlChecked
                border.width: 1
                border.color: Theme.accent
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: qsTr("参考源（GT）")
                Accessible.description: qsTr("该源当前承担会话的对比基准角色")

                Text {
                    id: gtBadgeLabel

                    anchors.centerIn: parent
                    text: qsTr("GT")
                    color: Theme.primaryText
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }
            }
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (!focusButton.enabled)
                    return "transparent";
                if (focusButton.checked)
                    return Theme.controlChecked;
                if (focusButton.down)
                    return Theme.fluentPressed;
                if (focusButton.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: focusButton.activeFocus ? 2 : 1
            border.color: {
                if (focusButton.activeFocus)
                    return Theme.focus;
                if (!focusButton.enabled)
                    return "transparent";
                if (focusButton.checked)
                    return Theme.accent;
                if (focusButton.hovered)
                    return Theme.subtleBorder;
                return "transparent";
            }

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: 120
                }
            }
        }
    }

    // qmllint disable import unqualified unresolved-type
    component DiffModeButton: ReviewActionButton {
        id: diffModeButton

        checkable: true
        checked: control.currentMode === ComparisonSurface.Difference
        // Opening the flavour menu must not light the button up before a flavour is chosen.
        mirrorsState: true
        implicitHeight: 30
        implicitWidth: control.differenceFlavorLabel.length > 0 ? 132 : 84
        enabled: !control.busy
        leftPadding: 10
        rightPadding: 10
        onClicked: diffFlavorMenu.open()

        contentItem: Text {
            text: diffModeButton.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !diffModeButton.enabled ? Theme.disabledText : (diffModeButton.checked || diffModeButton.hovered ? Theme.primaryText : Theme.mutedText)
            font.pixelSize: 12
            font.weight: diffModeButton.checked ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (!diffModeButton.enabled)
                    return "transparent";
                if (diffModeButton.checked)
                    return Theme.controlChecked;
                if (diffModeButton.down)
                    return Theme.fluentPressed;
                if (diffModeButton.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: diffModeButton.activeFocus ? 2 : 1
            border.color: {
                if (diffModeButton.activeFocus)
                    return Theme.focus;
                if (!diffModeButton.enabled)
                    return "transparent";
                if (diffModeButton.checked)
                    return Theme.accent;
                if (diffModeButton.hovered)
                    return Theme.subtleBorder;
                return "transparent";
            }

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: 120
                }
            }
        }

        VcsMenu {
            id: diffFlavorMenu
            menuWidth: 220

            VcsRadioMenuItem {
                objectName: "diffPureMenuItem"

                text: qsTr("纯差异图")
                checked: control.currentMode === ComparisonSurface.Difference && control.differenceMetric === ComparisonSurface.RgbAbsolute
                onTriggered: control.differenceViewRequested(ComparisonSurface.RgbAbsolute)
            }
            VcsRadioMenuItem {
                objectName: "diffHighlightMenuItem"

                text: qsTr("原图叠加高亮")
                checked: control.currentMode === ComparisonSurface.Difference && control.differenceMetric === ComparisonSurface.Highlight
                onTriggered: control.differenceViewRequested(ComparisonSurface.Highlight)
            }
        }
    }
    // qmllint enable import unqualified unresolved-type

    ReviewActionButton {
        id: inspectorToggleButton

        objectName: "inspectorToggleButton"
        text: control.inspectorOpen ? qsTr("隐藏检查器") : qsTr("检查器")
        implicitHeight: 30
        leftPadding: 12
        rightPadding: 12
        prominent: control.inspectorOpen
        onClicked: control.inspectorRequested()
        anchors {
            right: parent.right
            rightMargin: 12
            verticalCenter: parent.verticalCenter
        }
    }

    // Step-2 comparison export: copies the comparison viewport plus an annotation bar
    // (source names, observation context) to the clipboard in one action.
    VcsToolButton {
        id: copyComparisonButton

        objectName: "copyComparisonButton"
        text: qsTr("复制对比图")
        enabled: !control.busy
        implicitWidth: 104
        implicitHeight: 30
        labelPixelSize: 12
        onClicked: control.copyComparisonRequested()
        anchors {
            right: inspectorToggleButton.left
            rightMargin: 8
            verticalCenter: parent.verticalCenter
        }
    }
}
