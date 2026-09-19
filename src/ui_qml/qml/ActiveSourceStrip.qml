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

    objectName: "activeSourceStrip"
    height: sourceCount > 1 ? 42 : 0
    visible: sourceCount > 1
    color: singleMode ? "transparent" : panelColor
    border.color: singleMode ? "transparent" : borderColor
    opacity: singleMode && !sourceHover.hovered ? 0.68 : 1.0

    Behavior on opacity {
        NumberAnimation {
            duration: 160
        }
    }

    HoverHandler {
        id: sourceHover
    }

    Row {
        id: chips

        spacing: 7
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
                readonly property real chipLabelPlainWidth: letterLabel.implicitWidth + separatorOne.implicitWidth + filenameLabel.implicitWidth + 10
                readonly property real chipLabelCanonicalWidth: chipLabelPlainWidth + referenceTag.implicitWidth + separatorTwo.implicitWidth + 10
                width: Math.min(280, Math.max(128, (chip.isCanonical ? chipLabelCanonicalWidth : chipLabelPlainWidth) + (control.singleMode ? 24 : 84)))
                radius: 8
                readonly property string resolvedSourceIdentity: chip.sourceIdentity.length > 0 ? chip.sourceIdentity : (chip.sourceId >= 0 && chip.sourceId < control.sourceIdentities.length ? String(control.sourceIdentities[chip.sourceId]) : "")
                readonly property bool isCanonical: chip.resolvedSourceIdentity.length > 0 ? chip.resolvedSourceIdentity === control.canonicalSourceIdentity : chip.sourceId === control.canonicalSourceIndex
                readonly property bool pending: control.pendingSourceIdentities.indexOf(chip.resolvedSourceIdentity) >= 0 || requestQueued
                property bool requestQueued: false
                color: chip.isCanonical ? Theme.controlChecked : Theme.raisedPanel
                border.color: chip.isCanonical ? control.accentColor : control.borderColor

                function requestReference() {
                    if (chip.pending || chip.isCanonical || chip.resolvedSourceIdentity.length === 0)
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

                    Text {
                        id: letterLabel

                        text: String.fromCharCode(65 + chip.sourceId)
                        color: control.textColor
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        id: separatorOne

                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: 12
                    }
                    Text {
                        id: referenceTag

                        objectName: "sourceReferenceTag-" + chip.sourceId
                        visible: chip.isCanonical
                        text: qsTr("参考")
                        color: control.accentColor
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        Accessible.name: qsTr("参考源")
                        Accessible.description: qsTr("以它为帧号基准的参考源")

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

                        visible: chip.isCanonical
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
                        color: "#3d2e10"
                        border.width: 1
                        border.color: "#b08630"
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
                            color: "#e6a817"
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
                            visible: !chip.isCanonical
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
            width: 34
            height: chips.height
            text: "+"
            enabled: true
            helpText: qsTr("添加视频")
            onClicked: control.addRequested()
            controlRadius: 8
        }
    }
}
