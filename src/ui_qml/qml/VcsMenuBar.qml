pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

MenuBar {
    id: control

    property color menuBarColor: Theme.headerBackground
    property color menuBarBorderColor: "transparent"
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText
    property color accentColor: Theme.controlPressed

    delegate: MenuBarItem {
        id: topLevelItem

        objectName: menu && menu.objectName.length > 0 ? menu.objectName + "Button" : ""
        enabled: Boolean(menu && menu.enabled)

        leftPadding: 10
        rightPadding: 10
        topPadding: 5
        bottomPadding: 5

        contentItem: Text {
            text: topLevelItem.text
            color: topLevelItem.enabled ? control.textColor : control.mutedTextColor
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (!topLevelItem.enabled)
                    return "transparent";
                if (topLevelItem.highlighted || topLevelItem.down)
                    return control.accentColor;
                if (topLevelItem.activeFocus)
                    return Theme.controlHover;
                if (topLevelItem.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: topLevelItem.activeFocus ? 1 : 0
            border.color: Theme.focus

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
        }
    }

    background: Rectangle {
        color: control.menuBarColor

        Rectangle {
            height: 1
            visible: control.menuBarBorderColor.a > 0
            color: control.menuBarBorderColor
            anchors {
                bottom: parent.bottom
                left: parent.left
                right: parent.right
            }
        }
    }
}
