pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Dark styled menu row laid out as three fixed columns: [Check/Icon] Label ... Shortcut / ›.
// Used inside VcsMenu popups (and VcsMenuBar top-level menus).
MenuItem {
    id: control

    property bool popupInputContext: true
    property string shortcutText: ""
    property color textColor: Theme.primaryText
    property color disabledTextColor: Theme.disabledText
    property color shortcutColor: Theme.mutedText
    property color hoverColor: Theme.controlPressed
    property color checkColor: Theme.accent
    property color checkBackgroundColor: Theme.disabledPanel
    property color checkBorderColor: Theme.menuBorder
    property bool radioIndicator: false
    readonly property var nestedMenu: control.subMenu

    visible: !control.nestedMenu || control.nestedMenu.menuItemVisible
    enabled: !control.nestedMenu || control.nestedMenu.menuItemEnabled

    implicitHeight: visible ? 35 : 0
    height: implicitHeight
    leftPadding: control.checkable ? 30 : 12
    rightPadding: 12
    topPadding: 0
    bottomPadding: 0

    // The native submenu arrow is rendered inside contentItem so the row keeps an exact
    // three-column layout; reserve nothing on the right for the internal arrow.
    arrow: Item {
        visible: false
        implicitWidth: 0
        implicitHeight: 0
    }

    contentItem: Item {
        Text {
            id: labelText

            anchors {
                left: parent.left
                right: trailingText.left
                rightMargin: 10
                verticalCenter: parent.verticalCenter
            }
            text: control.text
            color: control.enabled ? control.textColor : control.disabledTextColor
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        VcsMenuShortcut {
            id: trailingText

            anchors {
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            shortcutEnabled: control.enabled
            text: control.subMenu ? "›" : control.shortcutText
            shortcutColor: control.subMenu ? control.textColor : control.shortcutColor
            font.pixelSize: control.subMenu ? 14 : 12
            font.bold: control.subMenu
        }
    }

    indicator: Rectangle {
        objectName: control.objectName.length > 0 ? control.objectName + "Indicator" : "vcsMenuItemIndicator"
        visible: control.checkable
        x: 8
        y: control.height / 2 - height / 2
        implicitWidth: 14
        implicitHeight: 14
        radius: control.radioIndicator ? 7 : 3
        color: control.radioIndicator ? Theme.raisedPanel : (control.checked ? control.checkColor : control.checkBackgroundColor)
        border.color: control.checked ? control.checkColor : control.checkBorderColor
        border.width: 1

        Rectangle {
            objectName: control.objectName.length > 0 ? control.objectName + "RadioDot" : "vcsMenuItemRadioDot"
            visible: control.radioIndicator && control.checked
            width: 6
            height: 6
            radius: 3
            color: control.checkColor
            anchors.centerIn: parent
        }

        Text {
            anchors.centerIn: parent
            visible: !control.radioIndicator
            text: control.checked ? "✓" : ""
            color: Theme.inverseText
            font.pixelSize: 9
            font.bold: true
        }
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: control.hovered || control.highlighted ? control.hoverColor : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: 100
            }
        }
    }
}
