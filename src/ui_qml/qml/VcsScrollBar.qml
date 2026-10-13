pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Overlay scrollbar for the dark lists: transparent track, a thin rounded thumb that stays a
// quiet gray veil until the pointer or a drag engages it. One shared component so the file
// sidebar and the folder-pair list cannot drift into two different scroll affordances.
ScrollBar {
    id: control

    implicitWidth: 10
    padding: 2
    policy: ScrollBar.AsNeeded

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 36
        radius: width / 2
        color: control.pressed ? "#59ffffff" : (control.hovered ? "#47ffffff" : "#33ffffff")

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationBase
                easing.type: Easing.OutCubic
            }
        }
    }

    background: Item {}
}
