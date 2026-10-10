pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import "VcsTheme.js" as Theme

// V01 first-version "panorama + three same-ROI locals" strip. The panorama stays the live
// ComparisonSurface; this strip adds three local windows that re-sample one flattened
// snapshot of that same surface, so the windows are extra draws of the already-decoded
// frame set - no second surface, no extra decode path, no extra presentation ACK.
//
// Coordinate contract: the ROI and the panorama transform are inputs owned by the surface.
// Every window maps the shared ROI through the same math the renderer uses
// (effectiveSurfaceSampleRect + oriented rotation + centered aspect fit), so zoom, pan,
// source switches and mode round-trips keep the windows pinned to the same source pixels.
// Sample rects clamp half-open to the frame ([0,w) x [0,h), right/bottom at the last valid
// pixel centre) and to the panel content rect, so a window never shows the panorama's
// letterbox or stage background inside its footage area.
Rectangle {
    id: control

    // The live panorama surface whose flattened texture the windows sample.
    required property Item sourceItem
    // surface.sourcePanelRects: [{slot,x,y,width,height,contentWidth,contentHeight}, ...]
    required property var panels
    // Per-slot media info: [{width,height,rotationDegrees}, ...]
    required property var sourceInfos
    required property int sourceCount
    property var sourceLabels: []
    // Shared ROI state - single source of truth, owned by the surface.
    required property bool roiEnabled
    property real roiLeft: 0
    property real roiTop: 0
    property real roiRight: 1
    property real roiBottom: 1
    // Shared panorama transform.
    property real viewScale: 1
    property real viewCenterX: 0.5
    property real viewCenterY: 0.5
    // One zoom for all three windows ("三路使用一个 ROI 变换"); the viewport owns the state.
    property int zoomPercent: 100
    property bool smoothSampling: true
    // Owned by the viewport so the value survives strip teardown; the strip only asks.
    signal zoomStepRequested(int percent)

    readonly property var zoomSteps: [100, 200, 400]
    readonly property real snapshotDpr: Window.window ? Window.window.devicePixelRatio : 1
    // Effective sample rect in source-normalized coordinates - mirrors the C++
    // effectiveSurfaceSampleRect so window content matches what the panorama presents.
    readonly property var sampleState: {
        const roiL = control.roiEnabled ? control.roiLeft : 0;
        const roiT = control.roiEnabled ? control.roiTop : 0;
        const roiW = control.roiEnabled ? control.roiRight - control.roiLeft : 1;
        const roiH = control.roiEnabled ? control.roiBottom - control.roiTop : 1;
        const visible = 1 / Math.max(control.viewScale, 1);
        // Mirrors the C++ std::clamp(center - visible/2, 0, 1 - visible). visible is capped
        // at 1 (the max below), so the upper bound stays >= 0 and below fit the sample
        // retains the whole ROI while the destination shrinks (panelContentRect).
        const viewLeft = Math.min(Math.max(control.viewCenterX - visible * 0.5, 0), 1 - visible);
        const viewTop = Math.min(Math.max(control.viewCenterY - visible * 0.5, 0), 1 - visible);
        return {
            "left": roiL + viewLeft * roiW,
            "top": roiT + viewTop * roiH,
            "width": visible * roiW,
            "height": visible * roiH
        };
    }
    // True while all three windows resolved a valid sample rect; drives the header hint.
    readonly property bool windowsReady: {
        for (let slot = 0; slot < 3; ++slot) {
            if (!control.windowTextureRect(slot))
                return false;
        }
        return true;
    }

    objectName: "roiTripletStrip"
    color: Theme.panel
    border.color: Theme.border
    border.width: 1
    radius: Theme.radiusMedium

    function normalizedRotation(raw) {
        const value = ((Number(raw) % 360) + 360) % 360;
        return value === 90 || value === 180 || value === 270 ? value : 0;
    }

    // Rotates a source-normalized rect into the oriented (display) frame, mirroring the C++
    // orientedSurfaceSamplePixels mapping.
    function orientedRect(rotation, left, top, right, bottom) {
        switch (rotation) {
        case 90:
            return {
                "left": top,
                "top": 1 - right,
                "width": bottom - top,
                "height": right - left
            };
        case 180:
            return {
                "left": 1 - right,
                "top": 1 - bottom,
                "width": right - left,
                "height": bottom - top
            };
        case 270:
            return {
                "left": 1 - bottom,
                "top": left,
                "width": bottom - top,
                "height": right - left
            };
        default:
            return {
                "left": left,
                "top": top,
                "width": right - left,
                "height": bottom - top
            };
        }
    }

    // The shared ROI rotated into slot space and clamped half-open to the frame: the
    // right/bottom edges may not pass the last valid pixel centre, so sampling can never
    // average in the texel at w or h. A missing or degenerate frame rejects the window.
    function clampedOrientedRoi(slot) {
        const info = slot >= 0 && slot < control.sourceInfos.length ? control.sourceInfos[slot] : null;
        if (!info)
            return null;
        const frameWidth = Number(info.width);
        const frameHeight = Number(info.height);
        if (!(frameWidth > 0) || !(frameHeight > 0))
            return null;
        const rotation = control.normalizedRotation(info.rotationDegrees);
        const rect = control.orientedRect(rotation, control.roiLeft, control.roiTop, control.roiRight, control.roiBottom);
        const orientedWidth = (rotation === 90 || rotation === 270) ? frameHeight : frameWidth;
        const orientedHeight = (rotation === 90 || rotation === 270) ? frameWidth : frameHeight;
        const maxRight = (orientedWidth - 0.5) / orientedWidth;
        const maxBottom = (orientedHeight - 0.5) / orientedHeight;
        const left = Math.max(0, rect.left);
        const top = Math.max(0, rect.top);
        const right = Math.max(Math.min(left + rect.width, maxRight), left + 0.5 / orientedWidth);
        const bottom = Math.max(Math.min(top + rect.height, maxBottom), top + 0.5 / orientedHeight);
        return {
            "left": left,
            "top": top,
            "width": right - left,
            "height": bottom - top,
            "rotation": rotation,
            "orientedWidth": orientedWidth,
            "orientedHeight": orientedHeight
        };
    }

    function panelForSlot(slot) {
        const panels = control.panels;
        for (let index = 0; index < panels.length; ++index) {
            if (Number(panels[index].slot) === slot)
                return panels[index];
        }
        return null;
    }

    // Centered aspect fit of the panel's fitted extent, then the shared zoom transform: at
    // scale >= 1 the fitted rect is the content destination, below 1 the content shrinks
    // about its centre (sub-pixel phase alignment stays with the renderer; sampling needs
    // only the logical rect).
    function panelContentRect(panel) {
        if (!panel)
            return null;
        const panelWidth = Number(panel.width);
        const panelHeight = Number(panel.height);
        const contentWidth = Number(panel.contentWidth);
        const contentHeight = Number(panel.contentHeight);
        if (!(panelWidth > 0) || !(panelHeight > 0) || !(contentWidth > 0) || !(contentHeight > 0))
            return null;
        let fittedWidth = panelWidth;
        let fittedHeight = panelHeight;
        if (contentWidth / contentHeight > panelWidth / panelHeight)
            fittedHeight = panelWidth / (contentWidth / contentHeight);
        else
            fittedWidth = panelHeight * (contentWidth / contentHeight);
        const scale = control.viewScale < 1 ? control.viewScale : 1;
        const scaledWidth = fittedWidth * scale;
        const scaledHeight = fittedHeight * scale;
        return {
            "x": Number(panel.x) + (panelWidth - scaledWidth) * 0.5,
            "y": Number(panel.y) + (panelHeight - scaledHeight) * 0.5,
            "width": scaledWidth,
            "height": scaledHeight
        };
    }

    // Normalized sub-rect of the flattened panorama texture that this window should show:
    // the oriented ROI cropped by the window zoom about its centre, mapped through the
    // oriented visible sample, then clamped into the panel's content rect so the window can
    // never sample the panorama's letterbox or another panel. Returns null when the region
    // is not resolvable (no frame, no panel, or the panorama no longer shows the ROI).
    function windowTextureRect(slot, zoomOverride) {
        if (slot < 0 || slot >= control.sourceCount)
            return null;
        const roi = control.clampedOrientedRoi(slot);
        if (!roi)
            return null;
        const zoom = zoomOverride !== undefined ? Math.max(1, zoomOverride) : Math.max(1, control.zoomPercent / 100);
        const cropWidth = roi.width / zoom;
        const cropHeight = roi.height / zoom;
        const cropLeft = roi.left + (roi.width - cropWidth) * 0.5;
        const cropTop = roi.top + (roi.height - cropHeight) * 0.5;
        const sample = control.sampleState;
        const orientedSample = control.orientedRect(roi.rotation, sample.left, sample.top, sample.left + sample.width, sample.top + sample.height);
        if (!(orientedSample.width > 0) || !(orientedSample.height > 0))
            return null;
        const content = control.panelContentRect(control.panelForSlot(slot));
        const textureWidth = control.sourceItem ? control.sourceItem.width : 0;
        const textureHeight = control.sourceItem ? control.sourceItem.height : 0;
        if (!content || !(textureWidth > 0) || !(textureHeight > 0))
            return null;
        // Fractions over the visible sample map linearly onto the content rect; the panorama
        // renders the sample region into that rect, so the texture position of an oriented
        // point is content.x + fraction * content.width before texture normalization.
        const rawLeft = (cropLeft - orientedSample.left) / orientedSample.width;
        const rawTop = (cropTop - orientedSample.top) / orientedSample.height;
        const rawRight = rawLeft + cropWidth / orientedSample.width;
        const rawBottom = rawTop + cropHeight / orientedSample.height;
        const boundsLeft = content.x / textureWidth;
        const boundsTop = content.y / textureHeight;
        const boundsRight = (content.x + content.width) / textureWidth;
        const boundsBottom = (content.y + content.height) / textureHeight;
        const left = Math.max((content.x + rawLeft * content.width) / textureWidth, boundsLeft);
        const top = Math.max((content.y + rawTop * content.height) / textureHeight, boundsTop);
        const right = Math.min((content.x + rawRight * content.width) / textureWidth, boundsRight);
        const bottom = Math.min((content.y + rawBottom * content.height) / textureHeight, boundsBottom);
        if (right <= left || bottom <= top)
            return null;
        return {
            "x": left,
            "y": top,
            "width": right - left,
            "height": bottom - top
        };
    }

    // Physical aspect of what a texture rect occupies on the panorama surface.
    function textureRectAspect(rect) {
        if (!rect || !control.sourceItem || !(control.sourceItem.width > 0) || !(control.sourceItem.height > 0))
            return 0;
        return rect.width * control.sourceItem.width / (rect.height * control.sourceItem.height);
    }

    function sourceLabel(slot) {
        return slot >= 0 && slot < control.sourceLabels.length ? String(control.sourceLabels[slot]) : "";
    }

    function cycleZoom() {
        const index = control.zoomSteps.indexOf(control.zoomPercent);
        const next = control.zoomSteps[(index + 1 + control.zoomSteps.length) % control.zoomSteps.length];
        control.zoomStepRequested(next);
    }

    // One live snapshot of the panorama; the three windows are additional draws of this
    // already-decoded content. `smooth` carries the shared nearest/bilinear preference.
    ShaderEffectSource {
        id: panoramaSnapshot

        objectName: "roiTripletSnapshot"
        sourceItem: control.sourceItem
        live: control.visible
        smooth: control.smoothSampling
        hideSource: false
        textureSize: control.sourceItem && control.sourceItem.width > 0 && control.sourceItem.height > 0 ? Qt.size(Math.ceil(control.sourceItem.width * control.snapshotDpr), Math.ceil(control.sourceItem.height * control.snapshotDpr)) : Qt.size(1, 1)
        visible: false
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

            spacing: 8
            width: parent.width

            ReviewActionButton {
                id: zoomButton

                objectName: "roiTripletZoomButton"
                enabled: control.visible
                text: qsTr("局部 %1%").arg(control.zoomPercent)
                helpText: qsTr("三个局部窗共用一个放大倍率：100% = ROI 适配窗口，200%/400% 以 ROI 中心再放大。\n采样方式跟随画面下方的「采样」按钮（自动/平滑/像素）。")
                implicitWidth: 96
                implicitHeight: 24
                leftPadding: 8
                rightPadding: 8
                topPadding: 0
                bottomPadding: 0
                onClicked: control.cycleZoom()
            }

            Text {
                id: roiHint

                objectName: "roiTripletHint"
                width: Math.max(0, parent.width - zoomButton.width - 8)
                elide: Text.ElideRight
                visible: !control.roiEnabled
                text: qsTr("在上方全景上 Alt+拖动 框选 ROI，三个局部窗将同框显示")
                color: Theme.mutedText
                font.pixelSize: 11
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
                model: Math.min(3, control.sourceCount)

                delegate: Rectangle {
                    id: windowCell

                    required property int index

                    objectName: "roiTripletWindow"
                    readonly property int slot: windowCell.index
                    readonly property var textureRect: control.windowTextureRect(windowCell.slot)
                    readonly property real contentAspect: control.textureRectAspect(textureRect)
                    readonly property bool showsFootage: Boolean(textureRect)

                    // Explicit id: the delegate's runtime parent is windowRow (Repeater
                    // reparents delegates to its parent), but qmllint resolves `parent`
                    // as the Repeater, which has no spacing.
                    width: (windowRow.width - 2 * windowRow.spacing) / 3
                    height: windowRow.height
                    radius: Theme.radiusSmall
                    color: Theme.canvas
                    border.width: 1
                    border.color: Theme.sourceBorder(windowCell.slot)

                    // Footage area keeps the sampled region's aspect and letterboxes inside
                    // the cell, so the panorama's stage/letterbox can never bleed in.
                    Item {
                        id: windowFootage

                        visible: windowCell.showsFootage && windowCell.contentAspect > 0
                        width: visible ? Math.min(parent.width - 16, (parent.height - 16) * windowCell.contentAspect) : 0
                        height: visible ? Math.min(parent.height - 16, width / Math.max(windowCell.contentAspect, 0.0001)) : 0
                        anchors.centerIn: parent
                        clip: true

                        ShaderEffect {
                            id: windowEffect

                            anchors.fill: parent
                            visible: parent.visible
                            readonly property rect sampleRect: windowCell.textureRect ? Qt.rect(windowCell.textureRect.x, windowCell.textureRect.y, windowCell.textureRect.width, windowCell.textureRect.height) : Qt.rect(0, 0, 1, 1)
                            property var src: panoramaSnapshot
                            // Qt 6 ShaderEffect loads preprocessed .qsb packages by relative
                            // URL: the URL resolves against this document, so the qrc copy
                            // (dvs_ui_qml_resources.qrc) serves the app and the contract
                            // harness, while the source-tree copy serves filesystem-loaded
                            // QML tests. Inline GLSL strings cannot work - Qt 6 resolves
                            // them as URLs and the windows stay blank. `uv` carries the cell
                            // quad's 0..1 coordinates; the fragment stage remaps them into
                            // the per-window sub-rect of the snapshot (see shaders/).
                            vertexShader: "shaders/roitripletwindow.vert.qsb"
                            fragmentShader: "shaders/roitripletwindow.frag.qsb"
                        }
                    }

                    Rectangle {
                        width: 30
                        height: 20
                        radius: 4
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
                            font.pixelSize: 11
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
                        visible: !windowCell.showsFootage && control.roiEnabled
                        text: qsTr("全景未覆盖该 ROI")
                        color: Theme.warning
                        font.pixelSize: 11
                        anchors.centerIn: parent
                    }
                }
            }
        }
    }
}
