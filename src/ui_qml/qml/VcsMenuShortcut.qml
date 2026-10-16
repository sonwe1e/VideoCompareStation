pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// Renders the trailing shortcut column of a VcsMenuItem row, or the submenu "›" arrow.
Text {
    id: control

    property color shortcutColor: Theme.mutedText
    property color disabledColor: Theme.disabledText
    property bool shortcutEnabled: true

    color: control.shortcutEnabled ? control.shortcutColor : control.disabledColor
    font.pixelSize: Theme.fontSecondary
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignRight
}
