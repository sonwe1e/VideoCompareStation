pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// V01 "panorama + same-ROI locals" strip. The panorama stays the live ComparisonSurface; each
// local window below it is an additional draw the D3D11 renderer performs of the same presented
// frame set - the window cells stay transparent over the footage area, and the renderer samples
// the decoded source textures directly, so a fixed magnification never depends on the panorama
// transform and details the panorama downscaled stay inspectable. No second surface, no extra
// decode path, no extra presentation ACK.
//
// Geometry contract: the renderer resolves every window through the shared pure geometry
// (surfaceLocalWindowGeometry, half-open clamp, SAR on the horizontal axis). This strip only
// stages each cell's content rect into surface coordinates and mirrors the published footage
// extents as letterbox chrome, so QML keeps no sampling formula of its own. Zoom levels are
// named for what they mean: 适配ROI fits the whole ROI, then 源像素100%/200%/400% are physical
// device pixels per source pixel (DPR handled by the renderer's pixel grid).
Rectangle {
    id: control

    // The live panorama surface. Window content rects are staged into its item coordinates.
    required property Item sourceItem
    required property int sourceCount
    property var sourceLabels: []
    // Shared ROI state - owned by the surface; the strip only mirrors whether one is active.
    required property bool roiEnabled
    // One zoom for all windows; 0 = 适配ROI, otherwise physical percent. The viewport owns
    // the state so the value survives strip teardown.
    property int zoomPercent: 0
    property bool smoothSampling: true
    // Published footage extents keyed by slot: [{slot, valid, width, height}, ...]. Bound to
    // the surface's roiTripletWindowRects so the chrome cannot drift from the drawn picture.
    property var windowRects: []
    // Owned by the viewport so the value survives strip teardown; the strip only asks.
    signal zoomStepRequested(int percent)
    // Phase-3 direct manipulation: drag deltas from one local window, in that cell's logical
    // pixels. The shared ROI means every window follows; the viewport forwards the deltas to
    // the surface, which owns the window->source geometry (rotation included) in one place -
    // the strip keeps no sampling formula of its own.
    signal roiPanRequested(int slot, real deltaLogicalX, real deltaLogicalY)

    readonly property var zoomSteps: [0, 100, 200, 400]

    objectName: "roiTripletStrip"
    // The strip is declared above the live surface in the scene graph, so anything opaque painted
    // here would cover the renderer's local-window overdraw that lands inside the window cells.
    // The root therefore stays transparent; only the header band keeps an opaque chrome plate and
    // the cells paint their own letterbox bars around the footage holes.
    color: "transparent"
    border.color: Theme.border
    border.width: 1
    radius: Theme.radiusMedium

    // Opaque chrome behind the header band only. The window row below must stay transparent so
    // the footage overdraw shows through the cells' footage holes. The band height is derived
    // from the column's own layout values instead of anchoring to `windowRow`: that row is
    // positioner-managed, and an anchor line into a positioner child does not track its layout.
    Rectangle {
        id: headerPlate

        objectName: "roiTripletHeaderPlate"
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            leftMargin: 1
            rightMargin: 1
            topMargin: 1
        }
        height: stripColumn.anchors.margins + stripHeader.height + stripColumn.spacing - 1
        color: Theme.panel
        radius: Math.max(0, control.radius - 1)
    }

    // Staged window content rects in surface item coordinates, one entry per active source.
    // The viewport binds the surface's roiTripletWindows to this list. Every input the mapping
    // depends on is read below, including both items' own positions, so the binding re-maps
    // whenever the relative layout moves.
    readonly property var stagedWindows: {
        if (!control.visible || !control.sourceItem || control.sourceCount < 1) {
            return [];
        }
        // Explicit reads so layout moves re-evaluate the mapping even though mapToItem itself
        // is not trackable.
        const stripX = control.x;
        const stripY = control.y;
        const surfaceX = control.sourceItem.x;
        const surfaceY = control.sourceItem.y;
        const surfaceWidth = control.sourceItem.width;
        const surfaceHeight = control.sourceItem.height;
        if (!(surfaceWidth > 0) || !(surfaceHeight > 0)) {
            return [];
        }
        const entries = [];
        for (let slot = 0; slot < control.windowCount(); ++slot) {
            const cell = control.cellItem(slot);
            if (!cell || !(cell.width > 16) || !(cell.height > 16)) {
                continue;
            }
            // Read the cell's own position so the binding re-maps when the row layout moves
            // its delegates; mapToItem's result is not itself trackable.
            const cellX = cell.x;
            const cellY = cell.y;
            const mapped = cell.mapToItem(control.sourceItem, 8, 8, cell.width - 16, cell.height - 16);
            entries.push({
                "slot": slot,
                "x": mapped.x,
                "y": mapped.y,
                "width": mapped.width,
                "height": mapped.height,
                "zoomPercent": control.zoomPercent,
                "smoothSampling": control.smoothSampling
            });
        }
        return entries;
    }

    function windowCount() {
        return Math.min(3, Math.max(0, control.sourceCount));
    }

    function cellItem(slot) {
        return slot >= 0 && slot < windowRepeater.count ? windowRepeater.itemAt(slot) : null;
    }

    function footageEntryFor(slot) {
        const rects = control.windowRects;
        for (let index = 0; index < rects.length; ++index) {
            if (Number(rects[index].slot) === slot) {
                return rects[index];
            }
        }
        return null;
    }

    function sourceLabel(slot) {
        return slot >= 0 && slot < control.sourceLabels.length ? String(control.sourceLabels[slot]) : "";
    }

    function zoomTitle() {
        // B09/plan V01 naming: ROI-fit must never read as a source-pixel magnification, so the
        // fixed steps say 源像素 explicitly (100% = physical-pixel 1:1 against the source).
        return control.zoomPercent === 0 ? qsTr("局部·适配ROI") : qsTr("局部·源像素 %1%").arg(control.zoomPercent);
    }

    // direction >= 0 walks the ladder toward higher magnification (wheel up / button click),
    // direction < 0 toward 适配ROI; both wrap. A missing argument keeps the legacy forward
    // direction so existing callers stay valid.
    function cycleZoom(direction) {
        const index = control.zoomSteps.indexOf(control.zoomPercent);
        const count = control.zoomSteps.length;
        const step = direction === undefined || direction >= 0 ? 1 : -1;
        const next = control.zoomSteps[(index + step + count) % count];
        control.zoomStepRequested(next);
    }

    Column {
        id: stripColumn

        spacing: 6
        anchors {
            fill: parent
            margins: 8
        }

        Row {
            id: stripHeader

            spacing: Theme.spaceMain
            width: parent.width

            ReviewActionButton {
                id: zoomButton

                objectName: "roiTripletZoomButton"
                enabled: control.visible
                text: control.zoomTitle()
                helpText: qsTr("各局部窗直接采样解码后的源纹理，共用一个倍率：适配ROI 撑满窗口；源像素100% 为纵向源像素 1:1（横向按 SAR），200%/400% 再放大。\n采样方式跟随画面下方的「采样」按钮（自动/平滑/像素）。局部显示的是已按契约转换的色彩，不是原始码值。\n在局部窗内拖动可平移共享 ROI（内容跟随光标），滚轮切换局部倍率。")
                implicitWidth: 96
                implicitHeight: 24
                leftPadding: 8
                rightPadding: 8
                topPadding: 0
                bottomPadding: 0
                onClicked: control.cycleZoom(1)
            }

            Text {
                id: roiHint

                objectName: "roiTripletHint"
                width: Math.max(0, parent.width - zoomButton.width - 8)
                elide: Text.ElideRight
                visible: !control.roiEnabled
                text: qsTr("在上方全景上 Alt+拖动 框选 ROI，局部窗将同框显示该区域的源像素")
                color: Theme.mutedText
                font.pixelSize: Theme.fontCaption
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            id: windowRow

            spacing: 6
            width: parent.width
            height: parent.height - stripHeader.height - stripColumn.spacing

            Repeater {
                id: windowRepeater

                objectName: "roiTripletWindowRepeater"
                model: control.windowCount()

                delegate: Rectangle {
                    id: windowCell

                    required property int index

                    objectName: "roiTripletWindow"
                    readonly property int slot: windowCell.index
                    readonly property var footageEntry: control.footageEntryFor(windowCell.slot)
                    readonly property bool footageValid: Boolean(windowCell.footageEntry) && windowCell.footageEntry.valid === true && Number(windowCell.footageEntry.width) > 0 && Number(windowCell.footageEntry.height) > 0
                    readonly property real footageWidth: windowCell.footageValid ? Number(windowCell.footageEntry.width) : 0
                    readonly property real footageHeight: windowCell.footageValid ? Number(windowCell.footageEntry.height) : 0
                    // The renderer draws the footage centred inside the content rect (cell minus
                    // the 8 px chrome inset); with no resolvable footage the bars cover the cell.
                    readonly property real footageX: (windowCell.width - windowCell.footageWidth) / 2
                    readonly property real footageY: (windowCell.height - windowCell.footageHeight) / 2

                    // Explicit column placement instead of relying on the Row positioner: the
                    // staged-window binding reads cell.x, and a deterministic binding keeps the
                    // mapped content rects in step with the layout.
                    x: windowCell.slot * (windowCell.width + windowRow.spacing)
                    width: (windowRow.width - 2 * windowRow.spacing) / 3
                    height: windowRow.height
                    radius: Theme.radiusSmall
                    // The footage area must stay transparent: the renderer's local draw sits
                    // underneath this cell in the scene graph.
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.sourceBorder(windowCell.slot)

                    Rectangle {
                        x: 0
                        y: 0
                        width: parent.width
                        height: Math.max(0, windowCell.footageY)
                        color: Theme.canvas
                    }
                    Rectangle {
                        x: 0
                        y: windowCell.footageY + windowCell.footageHeight
                        width: parent.width
                        height: Math.max(0, windowCell.height - windowCell.footageY - windowCell.footageHeight)
                        color: Theme.canvas
                    }
                    Rectangle {
                        x: 0
                        y: Math.max(0, windowCell.footageY)
                        width: Math.max(0, windowCell.footageX)
                        height: Math.max(0, windowCell.footageHeight)
                        color: Theme.canvas
                    }
                    Rectangle {
                        x: windowCell.footageX + windowCell.footageWidth
                        y: Math.max(0, windowCell.footageY)
                        width: Math.max(0, windowCell.width - windowCell.footageX - windowCell.footageWidth)
                        height: Math.max(0, windowCell.footageHeight)
                        color: Theme.canvas
                    }

                    Rectangle {
                        width: 30
                        height: 20
                        radius: Theme.radiusMicro
                        color: Theme.stageLabel
                        border.width: 1
                        border.color: Theme.sourceBorder(windowCell.slot)
                        anchors {
                            left: parent.left
                            leftMargin: 6
                            top: parent.top
                            topMargin: 6
                        }

                        Text {
                            anchors.centerIn: parent
                            text: String.fromCharCode(65 + windowCell.slot)
                            color: Theme.sourceColor(windowCell.slot)
                            font.bold: true
                            font.pixelSize: Theme.fontCaption
                        }
                    }

                    Text {
                        objectName: "roiTripletWindowLabel"
                        visible: control.sourceLabel(windowCell.slot).length > 0
                        text: control.sourceLabel(windowCell.slot)
                        color: Theme.secondaryText
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                        anchors {
                            left: parent.left
                            leftMargin: 42
                            right: parent.right
                            rightMargin: 6
                            top: parent.top
                            topMargin: 9
                        }
                    }

                    Text {
                        objectName: "roiTripletWindowUnavailable"
                        visible: !windowCell.footageValid
                        text: qsTr("暂无可采样画面")
                        color: Theme.warning
                        font.pixelSize: Theme.fontCaption
                        anchors.centerIn: parent
                    }

                    // Phase-3 direct manipulation: a drag nudges the shared ROI centre (the
                    // content follows the cursor, like the panorama pan), the wheel cycles the
                    // window magnification. Both need a resolvable ROI; without one the hint in
                    // the header still teaches the marquee and the cell stays inert.
                    MouseArea {
                        id: windowDrag

                        objectName: "roiTripletWindowDragArea"
                        anchors.fill: parent
                        enabled: control.roiEnabled && windowCell.footageValid
                        hoverEnabled: true
                        // The cursor names the verb before the first press: size-all on the
                        // draggable window, closed hand while the grab holds.
                        cursorShape: windowDrag.pressed ? Qt.ClosedHandCursor : (windowDrag.enabled && windowDrag.containsMouse ? Qt.SizeAllCursor : Qt.ArrowCursor)

                        property real lastLogicalX: 0
                        property real lastLogicalY: 0

                        onPressed: mouse => {
                            windowDrag.lastLogicalX = mouse.x;
                            windowDrag.lastLogicalY = mouse.y;
                            mouse.accepted = true;
                        }
                        onPositionChanged: mouse => {
                            if (!windowDrag.pressed)
                                return;
                            control.roiPanRequested(windowCell.slot, mouse.x - windowDrag.lastLogicalX, mouse.y - windowDrag.lastLogicalY);
                            windowDrag.lastLogicalX = mouse.x;
                            windowDrag.lastLogicalY = mouse.y;
                        }
                        onWheel: wheel => {
                            control.cycleZoom(wheel.angleDelta.y > 0 ? 1 : -1);
                            wheel.accepted = true;
                        }
                    }
                }
            }
        }
    }
}
