pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// P06 (D02) nearby-frames diagnostic card: filmstrip showing 5-9 frames around a marked issue,
// real timeline thumbnails, provenance badges (retained/generated/missing/unknown), anchor highlight,
// loop seam indicator, interval playback controls, and training sample triplet export.
// V04: the window stays 5-9 cells centered on the anchor, one low-priority thumbnail batch
// replaces per-cell requests, thumbnails name their source, and the strip can switch to a
// same-ROI consecutive view for locating a defect before close inspection on the main canvas.
Rectangle {
    id: control

    required property int issueIndex
    property var issueData: null
    property var intervalData: null
    property var protocolController: null
    property var reviewController: null
    property var previewThumbnails: null
    property var sampleExportController: null
    property int currentFrame: -1
    property int totalFrames: 0
    // Shared-ROI state from the main canvas (normalized source rect). The consecutive strip
    // crops each thumbnail to this rect so every cell shows the same region across frames.
    property bool roiEnabled: false
    property real roiLeft: 0
    property real roiTop: 0
    property real roiRight: 1
    property real roiBottom: 1

    signal closeRequested
    signal seekRequested(int frame)
    signal loopIntervalRequested(int inFrame, int outFrame)
    signal stepFrameRequested(int delta)
    signal exportSampleRequested(int frame)

    objectName: "nearbyFramesCard"
    color: Theme.panelElevated
    border.color: Theme.menuBorder
    border.width: 1
    radius: Theme.radiusCard
    implicitWidth: 720
    // U01: expanded, the card sits at the top of its height band (144-184); folded it keeps
    // only the header row. The filmstrip shrinks its thumbnail wells to fit rather than
    // dropping the provenance badges V04 pinned.
    implicitHeight: collapsed ? cardHeaderRow.height + 20 : Theme.nearbyFramesHeightMax
    width: implicitWidth
    height: implicitHeight
    property bool collapsed: false

    readonly property int anchorFrame: intervalData && intervalData.anchorFrame !== undefined ? Number(intervalData.anchorFrame) : -1
    readonly property int inFrame: intervalData && intervalData.inFrame !== undefined ? Number(intervalData.inFrame) : -1
    readonly property int outFrame: intervalData && intervalData.outFrame !== undefined ? Number(intervalData.outFrame) : -1
    readonly property bool loopSeamAtOut: intervalData && intervalData.loopSeamAtOut !== undefined ? Boolean(intervalData.loopSeamAtOut) : false
    readonly property var sessionEpoch: intervalData && intervalData.sessionEpoch !== undefined ? intervalData.sessionEpoch : 0
    readonly property bool epochMatches: !reviewController || sessionEpoch === 0 || Number(reviewController.sessionEpoch) === Number(sessionEpoch)

    // V04 same-ROI consecutive strip: false shows the issue interval with provenance badges,
    // true shows a contiguous ±4 window around the anchor cropped to the shared ROI.
    property bool consecutiveStripMode: false
    readonly property int stripCenterFrame: anchorFrame >= 0 ? anchorFrame : currentFrame

    readonly property int selectedExportFrame: anchorFrame >= 0 ? anchorFrame : currentFrame
    readonly property bool canExportSample: sampleExportController && selectedExportFrame >= 0 ? sampleExportController.canExportAtFrame(selectedExportFrame) : false
    readonly property bool sampleExportBusy: sampleExportController ? Boolean(sampleExportController.busy) : false

    readonly property var summariesList: {
        if (inFrame < 0 || outFrame < inFrame)
            return [];

        if (protocolController && Boolean(protocolController.protocolLoaded)) {
            const summaries = protocolController.originSummariesInRange(inFrame, outFrame);
            if (summaries && summaries.length > 0)
                return clampToLocatingWindow(summaries);
        }

        // No loaded protocol: the strip has no provenance evidence for any frame, so every
        // cell reports "unknown" - claiming a definite "outside the interval" would invent
        // knowledge nobody has (V04: unknown protocol is not "out of interval").
        const fallback = [];
        for (let f = windowInFrame; f <= windowOutFrame; ++f) {
            fallback.push({
                "frame": f,
                "displayFrame": f + 1,
                "kind": "unknown",
                "badgeMarker": ""
            });
        }
        return fallback;
    }

    // V04 locating window: at most 9 cells (5-9 frames stay usable for locating), centered on
    // the anchor when the recorded interval is wider. The protocol summary is trimmed to the
    // same window so badges describe exactly the frames shown.
    readonly property int windowInFrame: {
        if (inFrame < 0 || outFrame < inFrame)
            return -1;
        if (outFrame - inFrame + 1 <= 9)
            return inFrame;
        const center = anchorFrame >= 0 ? Math.min(Math.max(anchorFrame, inFrame), outFrame) : currentFrame;
        return Math.max(inFrame, Math.min(center - 4, outFrame - 8));
    }
    readonly property int windowOutFrame: {
        if (inFrame < 0 || outFrame < inFrame || windowInFrame < 0)
            return -1;
        if (outFrame - inFrame + 1 <= 9)
            return outFrame;
        return Math.min(outFrame, windowInFrame + 8);
    }

    function clampToLocatingWindow(summaries) {
        if (windowInFrame < 0 || windowOutFrame < windowInFrame)
            return [];
        return summaries.filter(entry => {
            const f = Number(entry && entry.frame !== undefined ? entry.frame : -1);
            return f >= windowInFrame && f <= windowOutFrame;
        });
    }

    // V04 consecutive strip: contiguous frames around the anchor (current frame without one),
    // clamped to the timeline. No provenance claim is made for these cells - the point is
    // comparing the same region across neighbouring frames, and the interval view keeps the
    // provenance badges.
    readonly property var consecutiveList: {
        const total = Number(control.totalFrames);
        if (stripCenterFrame < 0 || total <= 0)
            return [];
        const first = Math.max(0, stripCenterFrame - 4);
        const last = Math.min(total - 1, stripCenterFrame + 4);
        const cells = [];
        for (let f = first; f <= last; ++f) {
            cells.push({
                "frame": f,
                "displayFrame": f + 1,
                "kind": ""
            });
        }
        return cells;
    }

    readonly property var displayList: consecutiveStripMode ? consecutiveList : summariesList

    // All thumbnail decodes for the card go through one low-priority batch (V04): a single
    // replaceable queue in the preview controller, never per-cell latest-wins requests that
    // would fight the timeline hover preview and drop all but the last frame.
    readonly property var thumbnailFrames: {
        const frames = [];
        for (let i = 0; i < displayList.length; ++i) {
            const f = displayList[i] && displayList[i].frame !== undefined ? Number(displayList[i].frame) : -1;
            if (f >= 0)
                frames.push(f);
        }
        return frames;
    }

    function requestVisibleThumbnails() {
        if (control.previewThumbnails && control.previewThumbnails.requestBatch)
            control.previewThumbnails.requestBatch(control.thumbnailFrames);
    }

    // Thumbnails always come from the canonical (timeline master) source; the card says so
    // instead of letting a frame read like "the" frame of every source.
    readonly property string thumbnailSourceName: {
        if (!reviewController)
            return "";
        const names = [reviewController.sourceAFilename, reviewController.sourceBFilename, reviewController.sourceCFilename];
        const index = Number(reviewController.canonicalSourceIndex);
        return index >= 0 && index < names.length ? String(names[index]) : "";
    }

    onThumbnailFramesChanged: requestVisibleThumbnails()
    onVisibleChanged: {
        // A closed or hidden card cancels its outstanding batch so stale frame decodes stop;
        // becoming visible again re-requests exactly the shown window.
        if (control.visible) {
            requestVisibleThumbnails();
        } else if (control.previewThumbnails && control.previewThumbnails.cancelBatch) {
            control.previewThumbnails.cancelBatch();
        }
    }
    onPreviewThumbnailsChanged: {
        if (control.visible)
            requestVisibleThumbnails();
    }

    Column {
        id: cardColumn

        anchors.fill: parent
        anchors.margins: 10
        spacing: Theme.spaceMain

        Row {
            id: cardHeaderRow

            width: parent.width
            spacing: Theme.spaceMain

            // U11: the spacer used to eat a flat 450px, which overflowed the card by ~178px
            // (and ~246px with the cross-session badge) because the header's fixed content is
            // wider than that guess. Derive the budget from the real siblings instead, so the
            // flexible middle expands into whatever the fixed content leaves and the action
            // buttons can never be pushed past the card's right edge. The gap count follows the
            // visible children: a Row skips an invisible child and its spacing with it.
            readonly property real fixedContentWidth: {
                const fixed = headerTitleText.width + anchorReadoutText.width + (thumbnailSourceLabel.visible ? thumbnailSourceLabel.width : 0) + (epochBadge.visible ? epochBadge.width : 0) + stripToggleButton.width + foldButton.width + closeButton.width;
                const visibleOthers = (thumbnailSourceLabel.visible ? 1 : 0) + (epochBadge.visible ? 1 : 0) + 5;
                return fixed + spacing * visibleOthers;
            }

            Text {
                id: headerTitleText

                text: qsTr("附近帧诊断卡")
                color: Theme.primaryText
                font.pixelSize: 13
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: anchorReadoutText

                text: control.anchorFrame >= 0 ? qsTr("锚点：第 %1 帧（区间 %2..%3）").arg(control.anchorFrame + 1).arg(control.inFrame + 1).arg(control.outFrame + 1) : ""
                color: Theme.mutedText
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }

            // V04: thumbnails decode the canonical (timeline master) source through the
            // independent preview path; say which file that is instead of an anonymous frame.
            Text {
                id: thumbnailSourceLabel

                objectName: "nearbyFramesThumbnailSourceLabel"
                visible: control.previewThumbnails !== null
                text: control.thumbnailSourceName.length > 0 ? qsTr("缩略图源：%1").arg(control.thumbnailSourceName) : qsTr("缩略图源：未知")
                color: control.thumbnailSourceName.length > 0 ? Theme.secondaryText : Theme.warningText
                font.pixelSize: 10
                elide: Text.ElideMiddle
                width: 150
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                id: epochBadge

                visible: !control.epochMatches
                height: 20
                width: epochText.implicitWidth + 10
                color: Theme.warningPanel
                border.color: Theme.warningBorder
                border.width: 1
                radius: 3
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    id: epochText

                    anchors.centerIn: parent
                    text: qsTr("跨会话记录")
                    color: Theme.warningText
                    font.pixelSize: 10
                }
            }

            Item {
                height: 1
                width: Math.max(Theme.spaceMain, cardHeaderRow.width - cardHeaderRow.fixedContentWidth)
            }

            ReviewActionButton {
                id: stripToggleButton

                objectName: "nearbyFramesStripToggle"
                // Interval view keeps the provenance badges; the consecutive view crops every
                // cell to the same ROI so the eye compares one region across neighbouring
                // frames. Without a ROI the strip still shows whole frames, and says so.
                text: control.consecutiveStripMode ? (control.roiEnabled ? qsTr("同 ROI 连续帧") : qsTr("连续帧（未启用 ROI）")) : qsTr("区间帧")
                checked: control.consecutiveStripMode
                implicitHeight: 26
                implicitWidth: control.consecutiveStripMode ? 120 : 76
                anchors.verticalCenter: parent.verticalCenter
                onClicked: control.consecutiveStripMode = !control.consecutiveStripMode
            }

            // U01: folding keeps the card on stage as its header alone, so re-expanding the
            // locating filmstrip costs one click; 收起 still removes the card entirely.
            ReviewActionButton {
                id: foldButton

                objectName: "nearbyFramesFoldButton"
                text: control.collapsed ? qsTr("展开") : qsTr("折叠")
                implicitHeight: 26
                implicitWidth: 60
                anchors.verticalCenter: parent.verticalCenter
                onClicked: control.collapsed = !control.collapsed
            }

            ReviewActionButton {
                id: closeButton

                objectName: "nearbyFramesCloseButton"
                text: qsTr("收起")
                implicitHeight: 26
                implicitWidth: 60
                anchors.verticalCenter: parent.verticalCenter
                onClicked: control.closeRequested()
            }
        }

        ListView {
            id: filmstripList

            objectName: "nearbyFramesFilmstrip"
            width: parent.width
            // U01: the filmstrip fills whatever the expanded height band leaves between the
            // header and the footer rows; folded, it collapses to zero with the card.
            height: control.collapsed ? 0 : Math.max(0, control.height - cardHeaderRow.height - cardFooterRow.height - 36)
            visible: !control.collapsed
            orientation: ListView.Horizontal
            spacing: 6
            clip: true
            cacheBuffer: 300
            model: control.displayList

            delegate: Rectangle {
                id: cellRect

                required property var modelData

                readonly property int cellFrame: modelData && modelData.frame !== undefined ? Number(modelData.frame) : -1
                readonly property int cellDisplayFrame: modelData && modelData.displayFrame !== undefined ? Number(modelData.displayFrame) : (cellFrame + 1)
                readonly property string cellKind: modelData && modelData.kind ? String(modelData.kind) : ""
                readonly property bool isAnchor: cellFrame === control.anchorFrame
                readonly property bool isCurrent: cellFrame === control.currentFrame
                readonly property bool isSeam: cellFrame === control.outFrame && control.loopSeamAtOut

                readonly property url thumbnailUrl: {
                    if (control.previewThumbnails && control.previewThumbnails.generation > 0 && cellFrame >= 0) {
                        const u = control.previewThumbnails.urlForFrame(cellFrame);
                        if (u && u.toString().length > 0)
                            return u;
                    }
                    return "";
                }

                // The same-ROI strip crops in the thumbnail's own pixels. The size comes from
                // the controller's decoded cache (reading the QML Image's implicit size would
                // loop: the implicit size itself follows sourceClipRect). An empty rect keeps
                // the whole image - also the fallback while the frame is not decoded yet.
                readonly property size thumbnailSourceSize: {
                    if (!control.previewThumbnails || !control.previewThumbnails.thumbnailSourceSize || cellFrame < 0)
                        return Qt.size(0, 0);
                    return control.previewThumbnails.thumbnailSourceSize(cellFrame);
                }
                readonly property rect roiCropRect: {
                    if (!control.consecutiveStripMode || !control.roiEnabled || cellRect.thumbnailUrl.toString().length === 0 || cellRect.thumbnailSourceSize.width <= 0 || cellRect.thumbnailSourceSize.height <= 0)
                        return Qt.rect(0, 0, 0, 0);
                    return Qt.rect(control.roiLeft * cellRect.thumbnailSourceSize.width, control.roiTop * cellRect.thumbnailSourceSize.height, (control.roiRight - control.roiLeft) * cellRect.thumbnailSourceSize.width, (control.roiBottom - control.roiTop) * cellRect.thumbnailSourceSize.height);
                }

                objectName: cellFrame >= 0 ? ("nearbyFrameCell_" + cellFrame) : ""
                width: 76
                height: filmstripList.height
                radius: Theme.radiusSmall
                color: isCurrent ? Theme.raisedPanel : Theme.panel
                border.width: isCurrent || isAnchor ? 2 : 1
                border.color: isCurrent ? Theme.accent : (isAnchor ? Theme.focus : Theme.border)

                Column {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 2

                    // Frame number header
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 2

                        Text {
                            text: "#" + cellRect.cellDisplayFrame
                            font.pixelSize: 10
                            font.weight: cellRect.isAnchor ? Font.Bold : Font.Normal
                            color: cellRect.isAnchor ? Theme.accentText : Theme.primaryText
                        }
                    }

                    // Thumbnail viewport
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - 4
                        height: Math.max(24, cellRect.height - 61)
                        radius: 3
                        color: Theme.thumbnailWell
                        clip: true

                        Image {
                            id: cellThumbnail

                            objectName: "nearbyFrameThumbnailImage"
                            anchors.fill: parent
                            // Cropped cells stretch the ROI region into the well (locating
                            // view); whole-frame cells keep their aspect like before.
                            fillMode: cellRect.roiCropRect.width > 0 && cellRect.roiCropRect.height > 0 ? Image.Stretch : Image.PreserveAspectFit
                            source: cellRect.thumbnailUrl
                            cache: false
                            visible: cellRect.thumbnailUrl.toString().length > 0
                            // V04 same-ROI strip: crop the decoded full frame to the shared
                            // ROI (normalized source rect maps 1:1 onto the whole-frame
                            // thumbnail). An empty rect keeps the whole image, which is also
                            // the fallback while the image dimensions are not loaded yet.
                            sourceClipRect: cellRect.roiCropRect
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: cellRect.thumbnailUrl.toString().length === 0
                            text: "#" + cellRect.cellDisplayFrame
                            color: Theme.mutedText
                            font.pixelSize: 10
                        }
                    }

                    // Provenance badge
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - 4
                        height: 18
                        radius: 3
                        color: {
                            if (cellRect.cellKind === "generated")
                                return Theme.controlChecked;
                            if (cellRect.cellKind === "missing")
                                return Theme.errorPanel;
                            if (cellRect.cellKind === "retained")
                                return Theme.control;
                            return "transparent";
                        }
                        border.width: cellRect.cellKind === "missing" ? 1 : 0
                        border.color: cellRect.cellKind === "missing" ? Theme.errorBorder : "transparent"

                        Text {
                            id: kindBadgeText

                            objectName: "nearbyFrameKindBadge"
                            anchors.centerIn: parent
                            // A status badge, not a conclusion: the decorative fontMicro floor.
                            font.pixelSize: Theme.fontMicro
                            text: {
                                if (cellRect.cellKind === "generated")
                                    return qsTr("生成");
                                if (cellRect.cellKind === "retained")
                                    return qsTr("保留");
                                if (cellRect.cellKind === "missing")
                                    return qsTr("缺帧");
                                if (cellRect.cellKind === "unknown")
                                    return qsTr("未知");
                                if (cellRect.cellKind === "outside")
                                    return qsTr("超界");
                                return "—";
                            }
                            color: {
                                if (cellRect.cellKind === "missing")
                                    return Theme.errorText;
                                if (cellRect.cellKind === "generated")
                                    return Theme.accentText;
                                if (cellRect.cellKind === "retained")
                                    return Theme.secondaryText;
                                return Theme.mutedText;
                            }
                        }
                    }

                    // Anchor & Seam tags
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 2

                        Rectangle {
                            visible: cellRect.isAnchor
                            width: anchorTag.implicitWidth + 6
                            height: 14
                            radius: 2
                            color: Theme.accentFill

                            Text {
                                id: anchorTag

                                anchors.centerIn: parent
                                text: qsTr("锚点")
                                color: Theme.inverseText
                                font.pixelSize: Theme.fontMicro
                            }
                        }

                        Rectangle {
                            id: loopSeamTagRect

                            objectName: "loopSeamTag"
                            visible: cellRect.isSeam
                            width: seamTag.implicitWidth + 6
                            height: 14
                            radius: 2
                            color: Theme.warningPanel
                            border.color: Theme.warningBorder
                            border.width: 1

                            Text {
                                id: seamTag

                                anchors.centerIn: parent
                                text: qsTr("接缝")
                                color: Theme.warningText
                                font.pixelSize: Theme.fontMicro
                            }
                        }
                    }
                }

                MouseArea {
                    objectName: "nearbyFrameCellClick"
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: control.seekRequested(cellRect.cellFrame)
                }
            }
        }

        Row {
            id: cardFooterRow

            width: parent.width
            spacing: Theme.spaceMain
            visible: !control.collapsed

            ReviewActionButton {
                objectName: "nearbyFramesLoopButton"
                text: qsTr("原速循环")
                implicitHeight: 28
                implicitWidth: 84
                enabled: control.inFrame >= 0 && control.outFrame >= control.inFrame
                onClicked: control.loopIntervalRequested(control.inFrame, control.outFrame)
            }

            ReviewActionButton {
                objectName: "nearbyFramesStepPrevButton"
                text: qsTr("上一帧")
                implicitHeight: 28
                implicitWidth: 70
                onClicked: control.stepFrameRequested(-1)
            }

            ReviewActionButton {
                objectName: "nearbyFramesStepNextButton"
                text: qsTr("下一帧")
                implicitHeight: 28
                implicitWidth: 70
                onClicked: control.stepFrameRequested(1)
            }

            ReviewActionButton {
                objectName: "nearbyFramesJumpAnchorButton"
                text: qsTr("跳转锚点")
                implicitHeight: 28
                implicitWidth: 84
                enabled: control.anchorFrame >= 0
                onClicked: control.seekRequested(control.anchorFrame)
            }

            ReviewActionButton {
                id: nearbyFramesExportButton

                objectName: "nearbyFramesExportButton"
                text: qsTr("导出训练样本…")
                implicitHeight: 28
                implicitWidth: 104
                visible: control.sampleExportController !== null
                enabled: control.canExportSample && !control.sampleExportBusy
                onClicked: control.exportSampleRequested(control.selectedExportFrame)
            }
        }
    }
}
