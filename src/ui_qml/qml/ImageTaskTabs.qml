pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
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
    property bool blocksGlobalMediaShortcuts: true

    signal tabActivated(int index)
    signal tabCloseRequested(int index)

    implicitHeight: 36

    function revealActiveTab() {
        if (root.activeIndex >= 0)
            tabList.positionViewAtIndex(root.activeIndex, ListView.Contain);
    }
    function navigateTo(index) {
        if (index < 0 || index >= root.tabsModel.length)
            return;
        root.tabActivated(index);
        Qt.callLater(() => {
            root.revealActiveTab();
            const item = tabList.itemAtIndex(root.activeIndex);
            if (item)
                item.forceActiveFocus(Qt.TabFocusReason);
        });
    }
    onActiveIndexChanged: Qt.callLater(revealActiveTab)
    onWidthChanged: Qt.callLater(revealActiveTab)
    onTabsModelChanged: Qt.callLater(revealActiveTab)

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
        spacing: Theme.spaceNone
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        model: root.tabsModel
        currentIndex: root.activeIndex
        ScrollBar.horizontal: ScrollBar {
            policy: ScrollBar.AsNeeded
            Accessible.name: qsTr("任务标签滚动")
        }

        delegate: Rectangle {
            id: tabItem

            required property var modelData
            required property int index
            property bool keepsTaskTabFocus: true
            activeFocusOnTab: index === root.activeIndex
            Accessible.role: Accessible.PageTab
            Accessible.name: String(modelData.title || "")
            Accessible.description: index === root.dirtyIndex ? qsTr("有未保存的编辑修改") : String(modelData.detail || "")
            Accessible.selected: index === root.activeIndex
            Accessible.focusable: true
            Accessible.onPressAction: root.navigateTo(index)
            border.width: activeFocus ? 2 : 0
            border.color: Theme.focus
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Left)
                    root.navigateTo(Math.max(0, tabItem.index - 1));
                else if (event.key === Qt.Key_Right)
                    root.navigateTo(Math.min(root.tabsModel.length - 1, tabItem.index + 1));
                else if (event.key === Qt.Key_Home)
                    root.navigateTo(0);
                else if (event.key === Qt.Key_End)
                    root.navigateTo(root.tabsModel.length - 1);
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Space)
                    root.navigateTo(tabItem.index);
                else if (event.key === Qt.Key_Delete)
                    root.tabCloseRequested(tabItem.index);
                else
                    return;
                event.accepted = true;
            }

            objectName: "imageTaskTab-" + index
            width: Math.min(240, Math.max(160, tabLabel.implicitWidth + 56))
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

                HoverHandler {
                    id: tabLabelHover
                }

                // The elided title stays inspectable: the full title (and the task's paths,
                // when the host publishes them) reads on hover.
                VcsToolTip {
                    visible: (tabLabelHover.hovered || tabItem.activeFocus || tabCloseArea.activeFocus) && tabLabel.truncated
                    text: tabItem.modelData.loading ? "" : (String(tabItem.modelData.detail || tabItem.modelData.title || ""))
                }
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

            Button {
                id: tabCloseArea
                property bool keepsTaskTabFocus: true
                activeFocusOnTab: tabItem.index === root.activeIndex
                Accessible.name: qsTr("关闭任务：%1").arg(String(tabItem.modelData.title || ""))
                onClicked: root.tabCloseRequested(tabItem.index)
                background: Rectangle {
                    color: tabCloseArea.hovered ? Theme.controlHover : "transparent"
                    radius: Theme.radiusMicro
                    border.width: tabCloseArea.visualFocus ? 2 : 0
                    border.color: Theme.focus
                }

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
                    color: tabCloseArea.hovered ? Theme.primaryText : Theme.mutedText
                    font.pixelSize: Theme.fontCaption
                }
            }
        }
    }
}
