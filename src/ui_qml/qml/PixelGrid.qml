pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window

// Geometry only: no image readback or per-source-pixel objects. At 800% the number of
// lines is bounded by the visible viewport in physical pixels, independent of image size.
Item {
    id: grid

    property bool requested: false
    property bool nearest: false
    property real originX: 0
    property real originY: 0
    property real stepX: 0
    property real stepY: 0
    readonly property real deviceScale: Window.window ? Window.window.devicePixelRatio : 1
    readonly property bool geometryValid: isFinite(stepX) && isFinite(stepY) && stepX > 0 && stepY > 0 && width > 0 && height > 0
    readonly property bool active: requested && nearest && geometryValid && Math.min(stepX, stepY) * deviceScale >= 8
    readonly property real firstX: geometryValid ? ((originX % stepX) + stepX) % stepX : 0
    readonly property real firstY: geometryValid ? ((originY % stepY) + stepY) % stepY : 0
    readonly property int columns: active ? Math.min(4096, Math.max(0, Math.ceil((width - firstX) / stepX))) : 0
    readonly property int rows: active ? Math.min(4096, Math.max(0, Math.ceil((height - firstY) / stepY))) : 0

    visible: active
    clip: true

    Repeater {
        model: grid.columns

        Rectangle {
            required property int index
            x: grid.firstX + index * grid.stepX
            width: 1 / grid.deviceScale
            height: grid.height
            color: "#88000000"
        }
    }
    Repeater {
        model: grid.rows

        Rectangle {
            required property int index
            y: grid.firstY + index * grid.stepY
            width: grid.width
            height: 1 / grid.deviceScale
            color: "#88000000"
        }
    }
}
