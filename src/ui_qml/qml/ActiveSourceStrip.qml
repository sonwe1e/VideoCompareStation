pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property var sourcesModel
    required property int sourceCount
    required property bool singleMode
    required property int canonicalSourceIndex
    required property string canonicalSourceIdentity
    required property int referenceSourceIndex
    required property string referenceSourceIdentity
    required property var pendingSourceIdentities
    required property var sourceIdentities
    property color panelColor: Theme.panel
    property color borderColor: Theme.border
    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText

    signal addRequested
    signal removeRequested(string sourceIdentity)
    signal referenceRequested(string sourceIdentity)
    signal viewerFocusRequested

    property int openMenuCount: 0
    property bool returnViewerFocusAfterClose: false
    readonly property bool anyMenuOpen: openMenuCount > 0
    // C2. The order the user sees, and the sources they have selected. Both are keyed by frozen
    // source identity so that arranging the list cannot reach media truth - the chip keeps its
    // sourceId, and with it its A/B/C role, its accent colour and its pairing.
    property var displayOrder: []
    property var selectedIdentities: []
    readonly property bool anySelected: control.selectedIdentities.length > 0
    signal moveRequested(int fromIndex, int toIndex)
    signal selectionToggled(string sourceIdentity)
    signal selectionCleared
    signal removeSelectedRequested
    readonly property real chipSpacing: 7
    // A press has to travel this far before it counts as a drag, so an ordinary click on a chip
    // (which opens its menu) is never mistaken for the start of a reorder.
    readonly property real dragThreshold: 12

    // Resolves a released drag to a target index in the display order and asks for the move. The
    // controller rejects an index it does not have, so a drop past either end is simply not a move
    // rather than a move to a clamped position the user did not choose.
    function commitDrag(fromIndex, dropCentreX) {
        if (fromIndex < 0 || control.displayOrder.length === 0)
            return;
        let cursor = 12;
        let target = 0;
        for (let position = 0; position < control.displayOrder.length; ++position) {
            const identity = String(control.displayOrder[position]);
            const existing = chips.children.find(child => child.resolvedSourceIdentity === identity);
            const width = existing ? existing.width : 128;
            const centre = cursor + width / 2;
            if (dropCentreX > centre) {
                target = position + 1;
            } else {
                target = position;
                break;
            }
            cursor += width + control.chipSpacing;
        }
        if (target > control.displayOrder.length - 1)
            target = control.displayOrder.length - 1;
        if (target !== fromIndex)
            control.moveRequested(fromIndex, target);
    }

    // Positions are bindings, not an imperative pass. A Row would place its children in model order,
    // and the display order is exactly what may differ from it, so each chip computes its own x from
    // the order and the widths ahead of it. That matters because a chip's width is derived from its
    // label and settles a frame or more after the delegate appears: an imperative reposition
    // scheduled from a signal runs once, too early, and leaves every chip at a position computed
    // from stale widths. A binding re-evaluates on its own whenever the order or any width changes.
    function chipStartX(position) {
        let cursor = 12;
        for (let index = 0; index < position; ++index) {
            const preceding = chipAt(control.displayOrder[index]);
            if (preceding)
                cursor += preceding.width + control.chipSpacing;
        }
        return cursor;
    }

    // The chip showing one identity, or null when that source has no chip yet. A chip whose identity
    // is missing from the display order falls back to model order so it is still on screen rather
    // than stacked on top of another chip.
    function chipAt(identity) {
        const wanted = String(identity);
        let fallback = null;
        for (let index = 0; index < chips.children.length; ++index) {
            const child = chips.children[index];
            if (child.objectName === "addSourceChipButton" || child.objectName === "removeSelectedChipButton")
                continue;
            if (child.resolvedSourceIdentity === wanted)
                return child;
            if (!fallback)
                fallback = child;
        }
        return fallback;
    }

    // The trailing buttons follow the chips; the second one starts where the first ends.
    readonly property real trailingButtonX: {
        let cursor = 12;
        for (let index = 0; index < chips.children.length; ++index) {
            const child = chips.children[index];
            if (child.objectName === "addSourceChipButton" || child.objectName === "removeSelectedChipButton")
                continue;
            if (child.width > 0.0)
                cursor += child.width + control.chipSpacing;
        }
        return cursor;
    }

    // No repositioning hooks: every position is a binding, so there is nothing to schedule.

    // Clicking the chip body toggles its selection. The chip body had no click target before this, so
    // nothing that used to work moves; the chip menu stays on the overflow button, and a plain
    // selection needs no modifier key, which also keeps it reachable from a keyboard.
    function chipClicked(chip) {
        if (chip.resolvedSourceIdentity.length === 0)
            return;
        control.selectionToggled(chip.resolvedSourceIdentity);
    }

    objectName: "activeSourceStrip"
    height: sourceCount > 1 ? 40 : 0
    visible: sourceCount > 1
    color: "transparent"
    border.color: "transparent"
    opacity: singleMode && !sourceHover.hovered ? 0.68 : 1.0

    Behavior on opacity {
        NumberAnimation {
            duration: 160
        }
    }

    HoverHandler {
        id: sourceHover
    }

    Item {
        id: chips

        anchors {
            fill: parent
            leftMargin: 12
            rightMargin: 12
            topMargin: 6
            bottomMargin: 6
        }

        Repeater {
            objectName: "activeSourceRepeater"
            model: control.sourcesModel

            delegate: Rectangle {
                id: chip

                required property int sourceId
                required property string sourceIdentity
                required property string filename
                required property string parentLabel
                required property string fullPath
                required property bool changedOnDisk

                height: chips.height
                // Explicit label width: RowLayout.fillWidth children do not feed back into an
                // implicit width, so sum the pieces here to size the chip from the real text.
                readonly property real chipLabelPlainWidth: letterBadge.width + separatorOne.implicitWidth + filenameLabel.implicitWidth + 10
                readonly property real chipLabelTaggedWidth: chipLabelPlainWidth + (chip.isReference ? referenceTag.implicitWidth + separatorTwo.implicitWidth + 10 : 0) + (chip.isTimelineMaster ? masterTag.implicitWidth + separatorThree.implicitWidth + 10 : 0)
                width: Math.min(280, Math.max(128, ((chip.isReference || chip.isTimelineMaster) ? chipLabelTaggedWidth : chipLabelPlainWidth) + (control.singleMode ? 24 : 84)))
                radius: 8
                readonly property string resolvedSourceIdentity: chip.sourceIdentity.length > 0 ? chip.sourceIdentity : (chip.sourceId >= 0 && chip.sourceId < control.sourceIdentities.length ? String(control.sourceIdentities[chip.sourceId]) : "")
                // D08: timeline master and comparison reference are independent projections.
                readonly property bool isTimelineMaster: chip.resolvedSourceIdentity.length > 0 ? chip.resolvedSourceIdentity === control.canonicalSourceIdentity : chip.sourceId === control.canonicalSourceIndex
                readonly property bool isReference: chip.resolvedSourceIdentity.length > 0 ? (control.referenceSourceIdentity.length > 0 ? chip.resolvedSourceIdentity === control.referenceSourceIdentity : chip.sourceId === control.referenceSourceIndex) : chip.sourceId === control.referenceSourceIndex
                readonly property bool pending: control.pendingSourceIdentities.indexOf(chip.resolvedSourceIdentity) >= 0 || requestQueued
                property bool requestQueued: false
                readonly property bool selected: control.selectedIdentities.indexOf(chip.resolvedSourceIdentity) >= 0
                // Drag state. A drag starts only from a plain press that is not a modifier-click,
                // so ctrl-clicking to select never also starts a reorder.
                property bool dragArmed: false
                property int dragFromIndex: -1
                property real dragOffset: 0
                readonly property int displayIndex: control.displayOrder.indexOf(chip.resolvedSourceIdentity)
                // The chip's place in the display order, computed as a binding so it follows the
                // order and the widths ahead of it. A chip the display order does not mention yet
                // falls to the end rather than sitting on top of another chip.
                readonly property real layoutX: chip.displayIndex >= 0 ? control.chipStartX(chip.displayIndex) : control.chipStartX(control.sourceCount)
                x: chip.layoutX

                readonly property color sourceAccent: Theme.sourceColor(chip.sourceId)
                readonly property color sourceBg: Theme.sourceBackground(chip.sourceId)
                readonly property color sourceBorder: Theme.sourceBorder(chip.sourceId)

                color: chip.selected ? chip.sourceBg : (chip.isReference ? chip.sourceBg : Theme.raisedPanel)
                border.color: chip.selected ? chip.sourceAccent : (chip.isReference ? chip.sourceAccent : (chipHover.hovered ? chip.sourceBorder : Theme.border))
                border.width: (chip.selected || chip.isReference) ? 1.5 : 1

                // The drag is resolved on release rather than continuously: the chip follows the
                // pointer as a single offset, and only a release past the threshold commits a move.
                // Living reordering during the drag would need the strip to animate around a moving
                // pointer, and this build has no way to confirm that looks right.
                DragHandler {
                    id: chipDrag

                    target: null
                    property real pressX: 0
                    readonly property real travelled: chip.x - chipDrag.pressX

                    onActiveChanged: {
                        if (active) {
                            chip.dragArmed = true;
                            chip.dragFromIndex = chip.displayIndex;
                            chipDrag.pressX = chip.x;
                        } else if (chip.dragArmed) {
                            chip.dragArmed = false;
                            if (Math.abs(chipDrag.travelled) >= control.dragThreshold)
                                control.commitDrag(chip.dragFromIndex, chipDrag.pressX + chipDrag.travelled + chip.width / 2);
                        }
                    }
                }

                HoverHandler {
                    id: chipHover
                }

                TapHandler {
                    // Left button only. The chip's overflow button and the add button are children and
                    // consume their own clicks first, so this handler sees presses on the chip body
                    // alone and does not steal the menu from them.
                    acceptedButtons: Qt.LeftButton
                    gesturePolicy: TapHandler.DragThreshold

                    onTapped: function (eventPoint, button) {
                        control.chipClicked(chip);
                    }
                }

                function requestReference() {
                    if (chip.pending || chip.isReference || chip.resolvedSourceIdentity.length === 0)
                        return;
                    chip.requestQueued = true;
                    control.returnViewerFocusAfterClose = true;
                    sourceMenu.close();
                    Qt.callLater(() => {
                        control.referenceRequested(chip.resolvedSourceIdentity);
                        chip.requestQueued = false;
                    });
                }

                function requestRemoval() {
                    if (chip.pending || chip.resolvedSourceIdentity.length === 0)
                        return;
                    chip.requestQueued = true;
                    control.returnViewerFocusAfterClose = true;
                    sourceMenu.close();
                    Qt.callLater(() => {
                        control.removeRequested(chip.resolvedSourceIdentity);
                        chip.requestQueued = false;
                    });
                }

                RowLayout {
                    id: chipLabel

                    spacing: 5
                    anchors {
                        left: parent.left
                        leftMargin: 12
                        right: control.singleMode ? parent.right : chipControls.left
                        rightMargin: control.singleMode ? 12 : 6
                        verticalCenter: parent.verticalCenter
                    }

                    Rectangle {
                        id: letterBadge
                        implicitWidth: 18
                        implicitHeight: 18
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: chip.isReference ? chip.sourceAccent : Theme.fluentHover

                        Text {
                            id: letterLabel
                            anchors.centerIn: parent
                            text: String.fromCharCode(65 + chip.sourceId)
                            color: chip.isReference ? Theme.sourceInk : chip.sourceAccent
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                    }

                    Text {
                        id: separatorOne

                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: 12
                    }
                    Text {
                        id: masterTag

                        objectName: "sourceTimelineMasterTag-" + chip.sourceId
                        visible: chip.isTimelineMaster
                        text: qsTr("主时间线")
                        color: control.mutedTextColor
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        Accessible.name: qsTr("时间线主源")
                        Accessible.description: qsTr("会话时间轴与帧号由它决定")

                        HoverHandler {
                            id: masterTagHover
                        }

                        VcsToolTip {
                            visible: masterTagHover.hovered
                            text: qsTr("时间线主源")
                        }
                    }
                    Text {
                        id: separatorThree

                        visible: chip.isTimelineMaster && chip.isReference
                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: 12
                    }
                    Text {
                        id: referenceTag

                        objectName: "sourceReferenceTag-" + chip.sourceId
                        visible: chip.isReference
                        text: qsTr("参考")
                        // The reference chip is drawn entirely in its source colour.
                        color: chip.sourceAccent
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        Accessible.name: qsTr("参考源")
                        Accessible.description: qsTr("对比基准参考源")

                        HoverHandler {
                            id: referenceTagHover
                        }

                        VcsToolTip {
                            visible: referenceTagHover.hovered
                            text: qsTr("参考源")
                        }
                    }
                    Text {
                        id: separatorTwo

                        visible: chip.isReference
                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: 12
                    }
                    Text {
                        id: filenameLabel

                        text: chip.parentLabel.length > 0 ? "%1 (%2)".arg(chip.filename).arg(chip.parentLabel) : chip.filename
                        color: control.textColor
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true

                        HoverHandler {
                            id: filenameHover
                        }

                        VcsToolTip {
                            visible: filenameHover.hovered && chip.fullPath.length > 0
                            text: chip.fullPath
                        }
                    }
                }

                Row {
                    id: chipControls

                    visible: !control.singleMode
                    spacing: 3
                    anchors {
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                    }

                    BusyIndicator {
                        id: pendingIndicator

                        visible: chip.pending
                        running: visible
                        width: 22
                        height: 22
                    }

                    Rectangle {
                        id: changedOnDiskBadge

                        objectName: "changedOnDiskBadge-" + chip.sourceId
                        visible: chip.changedOnDisk
                        width: 24
                        height: 24
                        radius: 6
                        color: Theme.warningPanel
                        border.width: 1
                        border.color: Theme.warningBorder
                        Accessible.name: qsTr("视频文件已在磁盘上被修改")
                        VcsToolTip {
                            visible: changedOnDiskHover.hovered
                            text: qsTr("视频文件已在磁盘上被修改")
                        }

                        HoverHandler {
                            id: changedOnDiskHover
                        }

                        Text {
                            anchors.centerIn: parent
                            text: "!"
                            color: Theme.warning
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                    }

                    VcsToolButton {
                        id: overflowButton

                        objectName: "sourceOverflowButton-" + chip.sourceId
                        readonly property var sourceMenuControl: sourceMenu
                        readonly property var makeReferenceAction: useAsReferenceItem
                        readonly property var removeSourceAction: removeSourceItem
                        width: 30
                        height: 30
                        text: "⋯"
                        enabled: !chip.pending && chip.resolvedSourceIdentity.length > 0
                        helpText: chip.pending ? qsTr("正在更新视频…") : qsTr("源操作")
                        Accessible.name: qsTr("%1 的源操作").arg(chip.filename)
                        onClicked: sourceMenu.popup(overflowButton, Qt.point(0, overflowButton.height))
                    }

                    VcsMenu {
                        id: sourceMenu

                        objectName: "sourceMenu-" + chip.sourceId
                        property bool wasOpened: false
                        onOpened: {
                            wasOpened = true;
                            control.openMenuCount += 1;
                        }
                        onClosed: {
                            if (wasOpened) {
                                wasOpened = false;
                                control.openMenuCount = Math.max(0, control.openMenuCount - 1);
                            }
                            if (control.returnViewerFocusAfterClose) {
                                control.returnViewerFocusAfterClose = false;
                                control.viewerFocusRequested();
                            }
                        }
                        Component.onDestruction: {
                            if (wasOpened) {
                                wasOpened = false;
                                control.openMenuCount = Math.max(0, control.openMenuCount - 1);
                            }
                            if (control.returnViewerFocusAfterClose) {
                                control.returnViewerFocusAfterClose = false;
                                control.viewerFocusRequested();
                            }
                        }

                        VcsMenuItem {
                            id: useAsReferenceItem

                            objectName: "makeReferenceAction-" + chip.sourceId
                            // D08: any non-reference source (including the timeline master) can be reference.
                            visible: !chip.isReference
                            text: qsTr("设为参考源")
                            enabled: !chip.pending
                            onTriggered: chip.requestReference()
                        }

                        VcsMenuItem {
                            id: removeSourceItem

                            objectName: "removeSourceAction-" + chip.sourceId
                            text: qsTr("移除视频")
                            enabled: control.sourceCount > 1
                            onTriggered: chip.requestRemoval()
                        }
                    }
                }
            }
        }

        VcsToolButton {
            id: addSourceChipButton

            objectName: "addSourceChipButton"
            visible: control.sourceCount < 3
            x: control.trailingButtonX
            width: 34
            height: chips.height
            text: "+"
            enabled: true
            helpText: qsTr("添加视频")
            onClicked: control.addRequested()
            controlRadius: 8
        }

        VcsToolButton {
            id: removeSelectedChipButton

            objectName: "removeSelectedChipButton"
            visible: control.anySelected
            x: control.trailingButtonX + (addSourceChipButton.visible ? addSourceChipButton.width + control.chipSpacing : 0)
            width: 34
            height: chips.height
            text: "−"
            enabled: true
            helpText: qsTr("移除所选（%1）").arg(control.selectedIdentities.length)
            Accessible.name: helpText
            onClicked: control.removeSelectedRequested()
            controlRadius: 8
        }
    }
}
