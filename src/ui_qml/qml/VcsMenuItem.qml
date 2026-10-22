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
    // EX-04 (UX-13 S4): optional hover explanation, carried by the folded mode entries of
    // compact-tier "more" menus. Purely additive: the default empty string keeps the tooltip
    // permanently invisible and its popup content unbuilt, so every existing menu renders
    // and behaves exactly as before this property existed.
    property string helpText: ""
    property color textColor: Theme.primaryText
    property color disabledTextColor: Theme.disabledText
    property color shortcutColor: Theme.mutedText
    property color hoverColor: Theme.fluentHover
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

            objectName: control.objectName.length > 0 ? control.objectName + "Label" : ""
            anchors {
                left: parent.left
                right: trailingText.left
                rightMargin: 10
                verticalCenter: parent.verticalCenter
            }
            text: control.text
            color: control.enabled ? control.textColor : control.disabledTextColor
            // Menu rows use the body step. `fontMono` is the timecode/frame-counter size and
            // must not be borrowed for prose: it made every menu one step smaller than the
            // copy it leads to.
            font.pixelSize: Theme.fontBody
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        VcsMenuShortcut {
            id: trailingText

            objectName: control.objectName.length > 0 ? control.objectName + "Trailing" : ""
            anchors {
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            shortcutEnabled: control.enabled
            text: control.subMenu ? "›" : control.shortcutText
            shortcutColor: control.subMenu ? control.textColor : control.shortcutColor
            // The arrow marks the row; it must never outweigh the label it follows, so it
            // stays at the shortcut/arrow step (12) below the 14 px label.
            font.pixelSize: Theme.fontSecondary
            font.bold: control.subMenu
        }

        // Same tooltip component and delay step every chrome control uses (UX-20's 500 ms
        // lives in VcsToolTip). Declared inside the content item so the menu's own content
        // model never sees it; invisible - and its content unbuilt - while helpText is empty.
        VcsToolTip {
            objectName: control.objectName.length > 0 ? control.objectName + "Tip" : ""
            visible: control.helpText.length > 0 && control.hovered
            text: control.helpText
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
            // A pure glyph, not a label: the checkmark carries no wording of its own, so it stays
            // at the decorative fontMicro floor rather than the secondary size.
            font.pixelSize: Theme.fontMicro
            font.bold: true
        }
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: control.hovered || control.highlighted ? control.hoverColor : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }
    }
}
