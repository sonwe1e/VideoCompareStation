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
    property color panelColor: Theme.menu
    property color borderColor: Theme.border
    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText

    signal modeRequested(int mode)
    signal edgeRequested(int preferenceValue)
    // Step-2 difference entry: one control offers the two flavors the review named —
    // overlay-highlight to locate defects on the original, pure diff for distribution.
    signal differenceViewRequested(int metric)
    // Hold-to-peek: while the button is held the surface replaces the difference pass
    // with the raw first source of the active pair; releasing restores the difference.
    signal differencePeekChanged(bool held)
    // Step-2 "固定 GT 切候选": one action flips the candidate side against the fixed
    // reference; Main keeps the frame, zoom/pan and wipe split untouched.
    signal switchCandidateRequested
    // Step-2 comparison export: one action copies the labeled comparison capture.
    signal copyComparisonRequested
    signal inspectorRequested

    // qmllint disable import unqualified unresolved-type
    readonly property bool pairRelevant: currentMode === ComparisonSurface.Wipe || currentMode === ComparisonSurface.Difference || currentMode === ComparisonSurface.AnalysisGrid
    readonly property bool advancedMode: currentMode === ComparisonSurface.ThreeUp || currentMode === ComparisonSurface.ReferenceFocus || currentMode === ComparisonSurface.AnalysisGrid
    readonly property string advancedModeLabel: currentMode === ComparisonSurface.ThreeUp ? qsTr("三联") : (currentMode === ComparisonSurface.ReferenceFocus ? qsTr("参考聚焦") : (currentMode === ComparisonSurface.AnalysisGrid ? qsTr("分析网格") : ""))
    // The button names the active quick flavor; other metrics (signed/heatmap/...) keep
    // the plain "差异" label and stay selectable from the inspector.
    readonly property string differenceFlavorLabel: currentMode === ComparisonSurface.Difference ? (differenceMetric === ComparisonSurface.Highlight ? qsTr("叠加高亮") : (differenceMetric === ComparisonSurface.RgbAbsolute ? qsTr("纯差异图") : "")) : ""
    // qmllint enable import unqualified unresolved-type

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
        // Hold-to-peek: while held the surface shows the raw first source of the active
        // pair instead of the difference pass; releasing restores the difference view.
        VcsToolButton {
            id: differencePeekButton

            objectName: "differencePeekButton"
            text: qsTr("按住看原图")
            visible: control.currentMode === ComparisonSurface.Difference
            enabled: !control.busy
            prominent: pressed
            implicitWidth: 104
            implicitHeight: 30
            labelPixelSize: 12
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
            model: control.differenceEdges
            textRole: "label"
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
