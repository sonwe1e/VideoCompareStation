pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// One tab per still-image review task. The strip is deliberately presentation-only: the host
// owns the task list and the window owns the edit/close guards, so a stale strip can never
// destroy a task or steal the active controller.
Item {
    id: root

    objectName: "imageTaskTabs"

    property var tabsModel: []
    property int activeIndex: -1
    property int dirtyIndex: -1

    signal tabActivated(int index)
    signal tabCloseRequested(int index)

    implicitHeight: 34

    Rectangle {
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: 1
        color: Theme.border
    }

    ListView {
        id: tabList

        objectName: "imageTaskTabList"
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            bottom: parent.bottom
        }
        orientation: ListView.Horizontal
        spacing: 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        model: root.tabsModel

        delegate: Rectangle {
            id: tabItem

            required property var modelData
            required property int index

            objectName: "imageTaskTab-" + index
            width: Math.min(240, Math.max(136, tabLabel.implicitWidth + 56))
            height: tabList.height
            color: index === root.activeIndex ? Theme.panelElevated : (tabHoverArea.containsMouse ? Theme.controlHover : "transparent")

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: 2
                visible: tabItem.index === root.activeIndex
                color: Theme.accent
            }

            Rectangle {
                anchors {
                    right: parent.right
                    top: parent.top
                    bottom: parent.bottom
                }
                width: 1
                visible: tabItem.index < root.tabsModel.length - 1
                color: Theme.border
            }

            MouseArea {
                id: tabHoverArea

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onClicked: root.tabActivated(tabItem.index)
            }

            Text {
                id: tabLabel

                anchors {
                    left: parent.left
                    leftMargin: 12
                    right: tabCloseArea.left
                    rightMargin: 4
                    verticalCenter: parent.verticalCenter
                }
                text: tabItem.modelData.loading ? qsTr("正在打开…") : String(tabItem.modelData.title || "")
                color: tabItem.index === root.activeIndex ? Theme.primaryText : Theme.mutedText
                font.pixelSize: Theme.fontSecondary
                elide: Text.ElideMiddle
                textFormat: Text.PlainText
                maximumLineCount: 1
                opacity: tabItem.modelData.loading ? 0.7 : 1
            }

            Rectangle {
                id: dirtyDot

                objectName: "imageTaskTabDirty-" + tabItem.index
                width: 6
                height: 6
                radius: width / 2
                color: Theme.warning
                visible: tabItem.index === root.dirtyIndex
                anchors {
                    right: tabCloseArea.left
                    rightMargin: 6
                    verticalCenter: parent.verticalCenter
                }
            }

            Item {
                id: tabCloseArea

                objectName: "imageTaskTabClose-" + tabItem.index
                width: 24
                height: 24
                anchors {
                    right: parent.right
                    rightMargin: 4
                    verticalCenter: parent.verticalCenter
                }
                opacity: tabHoverArea.containsMouse || tabItem.index === root.activeIndex ? 1 : 0.45

                Text {
                    anchors.centerIn: parent
                    text: "\u2715"
                    color: closeHoverArea.containsMouse ? Theme.primaryText : Theme.mutedText
                    font.pixelSize: Theme.fontCaption
                }

                MouseArea {
                    id: closeHoverArea

                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton
                    onClicked: root.tabCloseRequested(tabItem.index)
                }
            }
        }
    }
}
