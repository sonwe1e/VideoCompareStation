pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// One tab per video review task (docs/plans/video-task-tabs-design.md §6). Deliberately the
// same presentation-only shape as ImageTaskTabs.qml: the host owns the task records and the
// window owns the open/switch guards, so a stale strip can never destroy a task. It is a
// separate file rather than a parameterized shared component because the video titles carry
// their own state rules (restoring vs loading, host-generated pair titles, source-count
// badge, no dirty dot) and the objectNames must stay video-prefixed for the contract suite;
// keeping ImageTaskTabs.qml untouched guarantees zero image-side behaviour drift.
Item {
    id: root

    objectName: "videoTaskTabs"

    property var tabsModel: []
    property int activeIndex: -1

    signal tabActivated(int index)
    signal tabCloseRequested(int index)

    implicitHeight: 36

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

        objectName: "videoTaskTabList"
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            bottom: parent.bottom
        }
        orientation: ListView.Horizontal
        spacing: Theme.spaceNone
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        model: root.tabsModel

        delegate: Rectangle {
            id: tabItem

            required property var modelData
            required property int index

            objectName: "videoTaskTab-" + index
            width: Math.min(240, Math.max(160, tabLabel.implicitWidth + (tabBadge.visible ? tabBadge.width + 10 : 0) + 56))
            height: tabList.height
            // Task-tab shape (workspace redesign §3.3): light radius on the top corners, a
            // square bottom edge sitting on the strip's hairline, and the active underline.
            // The bottom patch re-covers the rounded corners with the same colour, so the
            // translucent hover fill stays one veil (no alpha stacking) while the foot reads
            // flat - one glance separates a task tab from any source chip.
            radius: Theme.radiusSubtle
            color: index === root.activeIndex ? Theme.panelElevated : (tabHoverArea.containsMouse ? Theme.controlHover : "transparent")

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: tabItem.radius
                color: tabItem.color
            }

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
                    right: (tabBadge.visible ? tabBadge.left : tabCloseArea.left)
                    rightMargin: 4
                    verticalCenter: parent.verticalCenter
                }
                // The host generates the title (§6): file name, "A ↔ B", "A + 2"; the strip
                // only replaces it with the two progress states, loading and restoring.
                text: tabItem.modelData.restoring ? qsTr("正在恢复…") : (tabItem.modelData.loading ? qsTr("正在打开…") : String(tabItem.modelData.title || ""))
                color: tabItem.index === root.activeIndex ? Theme.primaryText : Theme.mutedText
                font.pixelSize: Theme.fontSecondary
                elide: Text.ElideMiddle
                textFormat: Text.PlainText
                maximumLineCount: 1
                opacity: tabItem.modelData.loading || tabItem.modelData.restoring ? 0.7 : 1

                HoverHandler {
                    id: tabLabelHover
                }

                // The elided title stays inspectable: the host-generated title reads in full
                // on hover while the strip keeps its clipped width.
                VcsToolTip {
                    visible: tabLabelHover.hovered && tabLabel.truncated && !tabItem.modelData.loading && !tabItem.modelData.restoring
                    text: String(tabItem.modelData.title || "")
                }
            }

            // Source-count badge marks a comparison task (§6); single videos show none.
            Rectangle {
                id: tabBadge

                objectName: "videoTaskTabBadge-" + tabItem.index
                visible: Boolean(tabItem.modelData.comparison)
                width: badgeText.implicitWidth + 10
                height: 16
                radius: 8
                color: Theme.controlHover
                anchors {
                    right: tabCloseArea.left
                    rightMargin: 6
                    verticalCenter: parent.verticalCenter
                }

                Text {
                    id: badgeText

                    anchors.centerIn: parent
                    text: String(tabItem.modelData.sourceCount || 0)
                    color: Theme.mutedText
                    font.pixelSize: Theme.fontCaption
                }
            }

            Item {
                id: tabCloseArea

                objectName: "videoTaskTabClose-" + tabItem.index
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
