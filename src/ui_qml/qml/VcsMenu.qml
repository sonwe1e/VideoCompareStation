pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Dark popup menu with a fixed width, used both for the top-level File/Compare/Analyze/View
// menus and for their one-level nested submenus. Rows are rendered by VcsMenuItem; the nested
// menus are VcsMenu again so submenu popups inherit the same background.
Menu {
    id: control

    property bool popupInputContext: true
    // Menu inherits Popup.visible, so submenu availability must not bind that state. The
    // delegate reads these properties when it renders this Menu as a parent-menu row.
    property bool menuItemVisible: true
    property bool menuItemEnabled: true
    property color menuBackgroundColor: Theme.menu
    property color menuBorderColor: Theme.menuBorder
    property real menuRadius: Theme.radiusLarge
    property real menuWidth: 280

    topMargin: 6
    bottomMargin: 6
    padding: 6
    width: control.menuWidth
    popupType: Popup.Window
    cascade: true
    focus: true
    delegate: VcsMenuItem {}

    // Qt permits open() to be invoked on a disabled Popup. Close at the pre-show boundary so
    // neither automation nor a style-specific MenuBar delegate can create an empty window.
    onAboutToShow: {
        if (!control.enabled)
            control.close();
    }

    background: Rectangle {
        radius: control.menuRadius
        color: control.menuBackgroundColor
        border.width: 1
        border.color: control.menuBorderColor
    }
}
