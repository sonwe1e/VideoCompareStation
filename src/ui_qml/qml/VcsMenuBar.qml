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
    // Fill behind the top-level item whose menu is open (or held down). A neutral veil, not a
    // saturated accent: the open title is a position marker, not a selected action.
    property color accentColor: "#2bffffff"

    delegate: MenuBarItem {
        id: topLevelItem

        objectName: menu && menu.objectName.length > 0 ? menu.objectName + "Button" : ""
        enabled: Boolean(menu && menu.enabled)

        leftPadding: 12
        rightPadding: 12
        topPadding: 6
        bottomPadding: 6

        contentItem: Text {
            text: topLevelItem.text
            color: topLevelItem.enabled ? control.textColor : control.mutedTextColor
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: Theme.radiusSmall
            color: {
                if (!topLevelItem.enabled)
                    return "transparent";
                if (topLevelItem.highlighted || topLevelItem.down)
                    return control.accentColor;
                if (topLevelItem.activeFocus)
                    return "#2bffffff";
                if (topLevelItem.hovered)
                    return Theme.control;
                return "transparent";
            }

            Behavior on color {
                ColorAnimation {
                    duration: Theme.animationBase
                    easing.type: Easing.OutCubic
                }
            }
        }
    }

    background: Rectangle {
        color: control.menuBarColor
    }
}
