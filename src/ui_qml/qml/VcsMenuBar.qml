pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

MenuBar {
    id: control

    property color menuBarColor: Theme.headerBackground
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
            // Body size, not the mono readout size: the menu is an entry point, and it used to
            // render one pixel smaller than the copy it opens.
            font.pixelSize: Theme.fontBody
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
                // Keyboard focus only (the contract in VcsTheme.js): a mouse click must not
                // leave the open-title veil behind after the menu closes.
                if (topLevelItem.visualFocus)
                    return control.accentColor;
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
