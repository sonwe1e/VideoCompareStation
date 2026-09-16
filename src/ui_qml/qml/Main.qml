pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import QtQuick.Window
import "VcsTheme.js" as Theme
// Dvs.Ui is registered by the C++ host before this document is loaded.
// qmllint disable import
import Dvs.Ui 1.0

// qmllint enable import

ApplicationWindow {
    id: root

    width: 1440
    height: 900
    minimumWidth: 960
    minimumHeight: 640
    visible: false
    title: qsTr("VCStation — 视频对比工作站")
    color: Theme.window

    readonly property color panelColor: Theme.menu
    readonly property color raisedPanelColor: Theme.raisedPanel
    readonly property color borderColor: Theme.border
    readonly property color accentColor: Theme.accent
    readonly property color mutedTextColor: Theme.mutedText
    readonly property color primaryTextColor: Theme.primaryText
    readonly property color errorColor: Theme.error

    palette {
        window: root.panelColor
        windowText: root.primaryTextColor
        base: root.raisedPanelColor
        alternateBase: root.panelColor
        text: root.primaryTextColor
        button: root.raisedPanelColor
        buttonText: root.primaryTextColor
        highlight: root.accentColor
        highlightedText: Theme.inverseText
        toolTipBase: root.raisedPanelColor
        toolTipText: root.primaryTextColor
    }

    // reviewFacade is the stable composition boundary provided by the C++ host.
    // qmllint disable unqualified
    readonly property var facade: reviewFacade
    readonly property var controller: facade ? facade.playback : null
    readonly property var preferences: facade ? facade.comparison : null
    readonly property var shell: facade ? facade.shell : null

    ReviewMessageCatalog {
        id: reviewMessageCatalog
    }
    readonly property var messageCatalog: reviewMessageCatalog
    // qmllint enable unqualified

    readonly property int referenceSourceIndex: controller ? Number(controller.referenceSourceIndex) : -1
    readonly property int canonicalSourceIndex: controller ? Number(controller.canonicalSourceIndex) : -1
    readonly property bool inspectorOpen: shell ? Boolean(shell.inspectorVisible) : false
    readonly property bool chromeVisible: shell ? Boolean(shell.chromeVisible) : true
    readonly property bool drawerMode: alignmentBar.visible && root.width < 1120
    property int visibilityBeforeFullScreen: Window.Windowed
    // 0 = video compare workspace, 1 = still-image workspace.
    property int workspaceMode: 0
    readonly property bool imageWorkspaceActive: workspaceMode === 1
    // Captured from the C++ context property so nested scopes never resolve a same-named
    // Item property (ImageWorkspace.imageReview) into a circular binding.
    // qmllint disable unqualified
    readonly property var stillImageController: imageReview
    property string immersiveHudText: ""
    property bool immersiveHudVisible: false
    property bool manualHudPending: false
    property bool showFramePending: false
    property real wipePosition: 0.5
    property bool pendingComparisonPreservesPosition: false
    property bool pendingNewReviewWantsThreeUp: false
    property string dropError: ""
    property string intentMessage: ""
    readonly property var pendingDestructiveAction: shell && shell.hasPendingAction ? shell.pendingAction : null
    readonly property int inFrame: shell ? Number(shell.inFrame) : -1
    readonly property int outFrame: shell ? Number(shell.outFrame) : -1
    readonly property real inMediaTime: shell ? Number(shell.inMediaTime) : -1
    readonly property real outMediaTime: shell ? Number(shell.outMediaTime) : -1
    readonly property bool rangePlaybackActive: Boolean(shell && shell.rangePlaybackActive)
    readonly property bool rangeStartPending: Boolean(shell && shell.rangeStartPending)
    property bool shortcutHelpVisible: false
    readonly property int shortcutPreset: preferences ? Number(preferences.shortcutPreset) : 0
    readonly property bool dropFrameTimecode: Boolean(preferences && preferences.dropFrameTimecode)
    property int changedOnDiskAnnouncedGeneration: -1

    readonly property bool busy: Boolean(controller && controller.busy)
    readonly property bool framePending: Boolean(controller && controller.framePending)
    readonly property bool playing: Boolean(controller && controller.playing)
    readonly property bool fullScreen: visibility === Window.FullScreen
    readonly property bool graphicsReady: Boolean(controller && controller.graphicsReady)
    readonly property var currentFrame: controller ? controller.currentFrame : -1
    readonly property var totalFrames: controller ? controller.totalFrames : 0
    readonly property int oneSecondStepFrames: controller ? controller.oneSecondStepFrames : 30
    readonly property string sourceAErrorKey: controller ? controller.sourceAErrorKey : ""
    readonly property string sourceBErrorKey: controller ? controller.sourceBErrorKey : ""
    readonly property string sourceCErrorKey: controller ? controller.sourceCErrorKey : ""
    readonly property bool sourceAMissing: Boolean(!controller || controller.sourceAMissing)
    readonly property bool sourceBMissing: Boolean(!controller || controller.sourceBMissing)
    readonly property bool sourceCMissing: Boolean(!controller || controller.sourceCMissing)
    readonly property int sourceCount: controller ? Number(controller.sourceCount) : 0
    readonly property bool singleMode: sourceCount === 1
    readonly property int preferredOscState: preferences ? Number(preferences.oscMode) : -1
    readonly property int oscState: sourceCount === 0 || !chromeVisible ? 2 : (preferredOscState >= 0 ? preferredOscState : (singleMode ? 1 : 0))
    readonly property bool transportHidden: oscState === 2 || !chromeVisible
    readonly property bool transportDocked: !transportHidden && oscState === 0
    readonly property bool transportOverlay: !transportHidden && oscState === 1
    readonly property int transportDockHeight: transportDocked ? transport.height + 8 : 0
    readonly property string pairErrorKey: controller ? controller.pairErrorKey : ""
    readonly property string frameMappingStatus: controller ? controller.frameMappingStatus : ""
    readonly property string alignmentEstimateStatus: controller ? controller.alignmentEstimateStatus : ""
    readonly property string sequenceAlignmentStatus: controller ? controller.sequenceAlignmentStatus : ""
    readonly property bool alignmentAnalysisRunning: Boolean(controller && controller.alignmentAnalysisRunning)
    readonly property string alignmentAnalysisStatus: controller ? controller.alignmentAnalysisStatus : ""
    readonly property string manualAnchorStatus: controller ? controller.manualAnchorStatus : ""
    readonly property var alignmentTimelineMarkers: controller ? controller.alignmentTimelineMarkers : []
    readonly property bool manualAnchorActive: Boolean(controller && controller.manualAnchorActive)
    readonly property bool alignmentRequired: Boolean(controller && controller.alignmentRequired)
    readonly property bool automaticAlignmentPending: Boolean(controller && controller.automaticAlignmentPending)
    readonly property bool canConfirmAutomaticAlignment: Boolean(controller && controller.canConfirmAutomaticAlignment)
    readonly property bool canUndoAutomaticAlignment: Boolean(controller && controller.canUndoAutomaticAlignment)
    readonly property var compatibilityFindings: controller ? controller.compatibilityFindings : []
    readonly property var differenceEdges: controller ? controller.differenceEdges : []
    readonly property string combinedAlignmentStatus: {
        const parts = [];
        if (frameMappingStatus.length > 0)
            parts.push(frameMappingStatus);
        if (alignmentEstimateStatus.length > 0)
            parts.push(alignmentEstimateStatus);
        if (sequenceAlignmentStatus.length > 0)
            parts.push(sequenceAlignmentStatus);
        if (alignmentAnalysisStatus.length > 0)
            parts.push(alignmentAnalysisStatus);
        if (manualAnchorStatus.length > 0)
            parts.push(manualAnchorStatus);
        return parts.join("  |  ");
    }
    readonly property bool autoAlignmentActive: Boolean(controller && controller.autoAlignmentActive)
    readonly property bool hasErrors: sourceAErrorKey.length > 0 || sourceBErrorKey.length > 0 || sourceCErrorKey.length > 0 || pairErrorKey.length > 0
    readonly property string sourceAName: sourceCount > 0 ? controller.sourceAFilename : qsTr("未选择文件")
    readonly property string sourceBName: sourceCount > 1 ? controller.sourceBFilename : qsTr("未选择文件")
    readonly property string sourceCName: sourceCount > 2 ? controller.sourceCFilename : qsTr("可选的第三个源")
    readonly property bool canFirstAction: graphicsReady && !busy && Boolean(controller && controller.canFirst)
    readonly property bool canPreviousAction: graphicsReady && !busy && Boolean(controller && controller.canPrevious)
    readonly property bool canNextAction: graphicsReady && !busy && Boolean(controller && controller.canNext)
    readonly property bool canLastAction: graphicsReady && !busy && Boolean(controller && controller.canLast)
    readonly property bool canPlayAction: graphicsReady && !busy && Boolean(controller && controller.canPlay)
    readonly property bool canPauseAction: !busy && Boolean(controller && controller.canPause)
    // ComparisonSurface is a C++ type registered by the host.
    // qmllint disable unqualified
    readonly property int effectiveViewMode: shell ? Number(shell.effectiveViewMode) : (singleMode ? ComparisonSurface.Single : ComparisonSurface.SideBySide)
    readonly property var availableViewModes: sourceCount <= 1 ? [
        {
            "label": qsTr("单画面"),
            "value": ComparisonSurface.Single
        }
    ] : sourceCount === 2 ? [
        {
            "label": qsTr("并排"),
            "value": ComparisonSurface.SideBySide
        },
        {
            "label": qsTr("分割线对比"),
            "value": ComparisonSurface.Wipe
        },
        {
            "label": qsTr("差异"),
            "value": ComparisonSurface.Difference
        }
    ] : [
        {
            "label": qsTr("并排"),
            "value": ComparisonSurface.SideBySide
        },
        {
            "label": qsTr("三联"),
            "value": ComparisonSurface.ThreeUp
        },
        {
            "label": qsTr("参考聚焦"),
            "value": ComparisonSurface.ReferenceFocus
        },
        {
            "label": qsTr("差异"),
            "value": ComparisonSurface.Difference
        },
        {
            "label": qsTr("分析网格"),
            "value": ComparisonSurface.AnalysisGrid
        },
        {
            "label": qsTr("分割线对比"),
            "value": ComparisonSurface.Wipe
        }
    ]
    readonly property bool analysisGridMode: effectiveViewMode === ComparisonSurface.AnalysisGrid
    readonly property bool differenceMode: effectiveViewMode === ComparisonSurface.Difference || analysisGridMode
    readonly property bool wipeMode: effectiveViewMode === ComparisonSurface.Wipe
    readonly property bool sideBySideMode: effectiveViewMode === ComparisonSurface.SideBySide
    readonly property bool threeUpMode: effectiveViewMode === ComparisonSurface.ThreeUp
    readonly property int threeUpViewMode: ComparisonSurface.ThreeUp
    // qmllint enable unqualified
    readonly property int differenceEdge: shell ? Number(shell.effectiveDifferenceEdge) : 0
    property bool differenceThresholdEnabled: false
    property int differenceThresholdCode: 0
    property int differenceThresholdPolicy: 1
    readonly property var selectedDifferenceEdge: {
        for (const edge of differenceEdges) {
            if (Number(edge.preferenceValue) === differenceEdge)
                return edge;
        }
        return null;
    }
    readonly property int differenceFirstSlot: selectedDifferenceEdge ? Number(selectedDifferenceEdge.firstSourceId) : 0
    readonly property int differenceSecondSlot: selectedDifferenceEdge ? Number(selectedDifferenceEdge.secondSourceId) : 1
    readonly property int selectedDifferenceExactness: selectedDifferenceEdge ? Number(selectedDifferenceEdge.exactness) : 4
    readonly property bool exactPlaneMode: preferences ? Number(preferences.differenceMetric) === 4 : false
    readonly property string differenceUnavailableDetail: {
        if ((!differenceMode && !wipeMode) || currentFrame < 0)
            return "";
        const missing = [];
        if (sourceMissing(differenceFirstSlot))
            missing.push(sourceLabel(differenceFirstSlot));
        if (sourceMissing(differenceSecondSlot))
            missing.push(sourceLabel(differenceSecondSlot));
        if (missing.length > 0)
            return qsTr("无法对比第 %1 帧，因为 %2 缺失。").arg(Number(currentFrame) + 1).arg(missing.join(qsTr(" 和 ")));
        if (differenceMode && exactPlaneMode && selectedDifferenceExactness !== 0)
            return qsTr("逐像素精确差异要求分辨率、像素格式、位深、色彩元数据一致，并使用 ExactIndex 映射。");
        return "";
    }
    property var sourceOffsetValues: ({})
    readonly property bool manualOffsetActive: {
        for (const sourceId of Object.keys(sourceOffsetValues)) {
            if (Number(sourceOffsetValues[sourceId]) !== 0)
                return true;
        }
        return false;
    }
    readonly property bool anyManualAlignmentActive: manualAnchorActive || manualOffsetActive
    property bool timelineDragging: false
    property int timelinePreviewFrame: -1
    readonly property string frameText: timelineDragging && timelinePreviewFrame >= 0 ? qsTr("第 %1 / %2 帧（松开跳转）").arg(timelinePreviewFrame + 1).arg(totalFrames) : (currentFrame >= 0 && totalFrames > 0 ? qsTr("第 %1 / %2 帧").arg(currentFrame + 1).arg(totalFrames) : qsTr("当前无帧"))
    // Live drop telemetry from the render-ack relay, surfaced beside the frame counter. The
    // count covers the whole process lifetime of the attached surface; only show it when the
    // relay has actually observed a gap.
    readonly property int droppedFrames: viewportFrame ? Number(viewportFrame.droppedFrames) : 0
    readonly property string droppedFramesText: droppedFrames > 0 ? qsTr("丢帧 %1").arg(droppedFrames) : ""
    readonly property real frameProgress: currentFrame >= 0 && totalFrames > 1 ? Math.max(0, Math.min(1, Number(currentFrame) / (Number(totalFrames) - 1))) : 0
    readonly property real timelineProgress: timelineDragging && timelinePreviewFrame >= 0 && totalFrames > 1 ? Number(timelinePreviewFrame) / (Number(totalFrames) - 1) : frameProgress
    readonly property bool timelineEnabled: graphicsReady && !busy && Boolean(controller && controller.canFirst) && totalFrames > 0
    readonly property bool anyMenuOpen: Boolean(applicationMenuBar && applicationMenuBar.anyMenuOpen) || Boolean(sourceBar && sourceBar.anyMenuOpen) || Boolean(viewerContextMenu && viewerContextMenu.anyMenuOpen)
    readonly property int inputContext: reviewInputDialogs.modalVisible || anchorDialog.visible || shortcutHelp.visible ? 3 : (anyMenuOpen || focusIsPopup(root.activeFocusItem) ? 2 : (focusIsTextEditing(root.activeFocusItem) ? 1 : 0))
    readonly property bool globalMediaShortcutsEnabled: inputContext === 0 && (!chromeVisible || !focusBlocksGlobalMediaShortcuts(root.activeFocusItem))
    readonly property bool presentationShortcutsEnabled: inputContext === 0
    readonly property bool frameErrorBannerVisible: hasErrors && currentFrame >= 0 && !busy && graphicsReady && Boolean(controller && controller.canFirst)
    readonly property string overlayTitle: busy ? qsTr("正在加载…") : (!graphicsReady ? qsTr("图形设备不可用") : (hasErrors ? qsTr("无法打开文件") : qsTr("把视频或图片拖到这里")))
    readonly property string overlayDetail: busy ? qsTr("请稍候，正在准备媒体。") : (!graphicsReady ? qsTr("图形设备就绪前，导航和打开操作不可用。") : (hasErrors ? errorDetails() : qsTr("打开一个视频可播放和逐帧检查，两三个视频可对比；图片也可直接拖入查看。")))
    readonly property bool overlayVisible: (busy && currentFrame < 0) || !graphicsReady || (hasErrors && !frameErrorBannerVisible)
    readonly property string currentTimecode: controller ? controller.timecodeForFrame(currentFrame, dropFrameTimecode) : "00:00:00:00"
    // The hover popup must only ever describe the frame its thumbnail actually shows. Thumbnails
    // are captured at sample-interval frames, so display the nearest sample instead of the raw
    // hover frame; when nothing is cached the popup stays hidden (see PlayerOsc).
    readonly property int timelinePreviewSampleFrame: timelinePreviewFrame >= 0 && totalFrames > 0 ? thumbnailCache.nearestSample(timelinePreviewFrame) : -1
    readonly property string previewTimecode: controller && timelinePreviewSampleFrame >= 0 ? controller.timecodeForFrame(timelinePreviewSampleFrame, dropFrameTimecode) : "00:00:00:00"
    readonly property bool roiEnabled: Boolean(viewportFrame && viewportFrame.roiEnabled)

    onFramePendingChanged: {
        if (framePending)
            framePendingDelay.restart();
        else {
            framePendingDelay.stop();
            showFramePending = false;
        }
    }

    onCurrentFrameChanged: {
        if (!chromeVisible && manualHudPending && currentFrame >= 0) {
            manualHudPending = false;
            showImmersiveHud(frameText);
        }
        if (rangeStartPending && Number(currentFrame) === inFrame && !busy) {
            shell.setRangeStartPending(false);
            Qt.callLater(() => {
                if (rangePlaybackActive && controller && !controller.playing && !controller.play())
                    stopRangeLoop(qsTr("无法启动播放，已停止循环播放。"));
            });
            return;
        }
        if (rangePlaybackActive && playing && outFrame >= inFrame && Number(currentFrame) >= outFrame) {
            shell.setRangeStartPending(true);
            if (!controller.seekFrame(inFrame))
                stopRangeLoop(qsTr("无法到达入点，已停止循环播放。"));
        }
    }

    onSourceCountChanged: {
        Qt.callLater(remapReviewRange);
    }

    onCanonicalSourceIndexChanged: {
        Qt.callLater(remapReviewRange);
        revealOsc();
    }

    onHasErrorsChanged: {
        if (hasErrors)
            revealOsc();
    }

    onPlayingChanged: {
        revealOsc();
        if (!chromeVisible && currentFrame >= 0)
            showImmersiveHud(playing ? qsTr("播放中 · %1").arg(frameText) : qsTr("已暂停 · %1").arg(frameText));
    }

    // Closing the window never prompts to save. Alignment, probe, and decode work are cancelled
    // by the host shutdown path; the review session needs no persistence guard.
    onClosing: {}

    // Fade was removed from the video compare modes; fall back to Wipe for persisted preferences
    // that still point at it, so the UI never lands on an unselectable mode.
    Component.onCompleted: {
        // qmllint disable unqualified
        if (preferences && Number(preferences.viewMode) === ComparisonSurface.Fade)
            preferences.viewMode = ComparisonSurface.Wipe;
        // qmllint enable unqualified
    }

    function fileName(fileUrl) {
        const decodedUrl = decodeURIComponent(fileUrl.toString());
        const separator = Math.max(decodedUrl.lastIndexOf("/"), decodedUrl.lastIndexOf("\\"));
        return decodedUrl.substring(separator + 1);
    }

    // Parent-directory label used when same-named files from different folders must stay distinct.
    function sourcePathLabel(fileUrl) {
        const decodedUrl = decodeURIComponent(String(fileUrl));
        let path = decodedUrl;
        if (path.startsWith("file:///"))
            path = path.substring(8);
        else if (path.startsWith("file://"))
            path = path.substring(7);
        const separator = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        if (separator < 0)
            return "";
        const rest = path.substring(0, separator);
        const parentSeparator = Math.max(rest.lastIndexOf("/"), rest.lastIndexOf("\\"));
        return parentSeparator >= 0 ? rest.substring(parentSeparator + 1) : rest;
    }

    function setInPoint() {
        if (currentFrame >= 0 && shell) {
            revealOsc();
            const frame = Number(currentFrame);
            const mediaTime = controller ? Number(controller.mediaTimeForFrame(frame)) : -1;
            if (shell.setRangeIn(frame, mediaTime))
                showImmersiveHud(qsTr("入点 · 第 %1 帧").arg(frame + 1));
        }
    }

    function setOutPoint() {
        if (currentFrame >= 0 && shell) {
            revealOsc();
            const frame = Number(currentFrame);
            const mediaTime = controller ? Number(controller.mediaTimeForFrame(frame)) : -1;
            if (shell.setRangeOut(frame, mediaTime))
                showImmersiveHud(qsTr("出点 · 第 %1 帧").arg(frame + 1));
        }
    }

    function playSelectedRange() {
        if (inFrame < 0 || outFrame < inFrame || !controller || !shell)
            return false;
        const seekRequired = Number(currentFrame) !== inFrame;
        if (!shell.setRangePlaybackState(true, seekRequired))
            return false;
        if (seekRequired) {
            if (!controller.seekFrame(inFrame)) {
                stopRangeLoop(qsTr("无法到达入点，已停止循环播放。"));
                return false;
            }
        } else if (!controller.play()) {
            stopRangeLoop(qsTr("无法启动播放，已停止循环播放。"));
            return false;
        }
        return true;
    }

    function clearSelectedRange() {
        if (shell)
            shell.clearRange();
    }

    function remapReviewRange() {
        if (!controller || !shell || sourceCount === 0)
            return;
        const mappedIn = inMediaTime >= 0 ? Number(controller.frameForMediaTime(inMediaTime)) : -1;
        const mappedOut = outMediaTime >= 0 ? Number(controller.frameForMediaTime(outMediaTime)) : -1;
        shell.remapRange(mappedIn, mappedOut);
    }

    function revealOsc() {
        transport.reveal();
    }

    function toggleRangeLoop() {
        if (inFrame < 0 || outFrame < inFrame || !shell)
            return false;
        const nextActive = !rangePlaybackActive;
        if (!shell.setRangePlaybackState(nextActive, false))
            return false;
        if (!nextActive && controller && controller.playing && !controller.pause()) {
            showIntentMessage(qsTr("循环播放已关闭，但无法暂停播放。"));
            return false;
        }
        return true;
    }

    function stopRangeLoop(message) {
        if (shell) {
            shell.setRangePlaybackState(false, false);
            shell.setRangeStartPending(false);
        }
        const paused = !controller || !controller.playing || controller.pause();
        if (message.length > 0)
            showIntentMessage(message);
        else if (!paused)
            showIntentMessage(qsTr("循环播放已关闭，但无法暂停播放。"));
        return paused;
    }

    function resetViewport() {
        if (viewportFrame && viewportFrame.surface)
            viewportFrame.surface.resetViewport();
    }

    function clearRoi() {
        if (viewportFrame)
            viewportFrame.clearRoi();
    }

    function setDropFrameTimecode(enabled) {
        if (preferences)
            preferences.dropFrameTimecode = Boolean(enabled);
    }

    function openViewerContextMenu() {
        viewerContextMenu.popup();
    }

    function setDroppedVideoOrder(urls) {
        return shell && shell.stageSources(urls.slice(0), 0);
    }

    function swapDroppedVideos(first, second) {
        if (shell)
            shell.moveStagedSource(first, second);
    }

    function resetReviewVisualState() {
        sourceOffsetValues = {};
        if (shell)
            shell.clearRange();
        wipePosition = 0.5;
        differenceThresholdEnabled = false;
        differenceThresholdCode = 0;
        if (viewportFrame && viewportFrame.surface)
            viewportFrame.surface.resetViewport();
    }

    function clearReviewUi() {
        if (shell)
            shell.clearStagedSources();
        resetReviewVisualState();
    }

    function requestDestructiveAction(action) {
        if (!shell || shell.hasPendingAction)
            return false;
        if (!shell.beginPendingAction(action))
            return false;
        executePendingDestructiveAction();
        return true;
    }

    function executePendingDestructiveAction() {
        if (!shell)
            return;
        const action = shell.takePendingAction();
        if (!action || !action.kind)
            return;
        if (action.kind === "openVideos") {
            performVideoReview(action.urls);
            return;
        }
        if (action.kind === "openImages") {
            performImageReview(action.urls);
            return;
        }
        if (action.kind === "closeReview") {
            shell.closeSources();
            return;
        }
        if (action.kind === "exit")
            close();
    }

    function cancelPendingDestructiveAction() {
        if (shell)
            shell.cancelPendingAction();
    }

    // After a menu action runs, hand keyboard control back to the viewer so transport shortcuts
    // (Space, arrows, I/O) work immediately.
    function returnFocusToViewer() {
        Qt.callLater(() => {
            if (viewportFrame)
                viewportFrame.forceActiveFocus();
        });
    }

    function showIntentMessage(message) {
        intentMessage = String(message);
        intentMessageTimer.restart();
    }

    function enqueueStartupRequest(kind, urls) {
        return shell && shell.enqueueStartupRequest(Number(kind), Array.from(urls));
    }

    function performVideoReview(normalizedUrls) {
        root.workspaceMode = 0;
        if (normalizedUrls.length === 1) {
            dropError = "";
            if (shell && shell.stageSources(normalizedUrls, 0))
                shell.openStagedSources(false);
            return;
        }
        dropError = "";
        if (!setDroppedVideoOrder(normalizedUrls)) {
            showIntentMessage(qsTr("无法暂存这些视频。"));
            return;
        }
        pendingComparisonPreservesPosition = false;
        reviewInputDialogs.openComparison();
    }

    function performImageReview(normalizedUrls) {
        root.workspaceMode = 1;
        // Opening images replaces the current video review, mirroring how opening videos
        // replaces an existing session. CloseSources runs as a background intent; the image
        // workspace is what the user sees immediately.
        if (root.sourceCount > 0 && shell)
            shell.closeSources();
        const target = root.stillImageController;
        if (!target || normalizedUrls.length === 0)
            return;
        target.openPrimary(normalizedUrls[0]);
        if (normalizedUrls.length >= 2 && normalizedUrls[1])
            target.openSecondary(normalizedUrls[1]);
    }

    function reviewUrls(urls, allowSingleSourceAppend) {
        const reviewed = controller.handleDroppedUrls(urls);
        if (!reviewed.accepted) {
            dropError = root.messageCatalog.droppedUrlError(reviewed.errorKey, reviewed.detail);
            return;
        }
        const normalizedUrls = reviewed.urls;
        if (reviewed.kind === "images") {
            requestDestructiveAction({
                "kind": "openImages",
                "urls": normalizedUrls,
                "external": false
            });
            return;
        }
        if (normalizedUrls.length === 1) {
            const existing = activeSourceUrls();
            if (allowSingleSourceAppend && sourceCount > 0 && existing.length === sourceCount && existing.length < 3) {
                const candidate = normalizedUrls[0].toString();
                for (const current of existing) {
                    if (current.toString() === candidate) {
                        dropError = qsTr("该视频已打开。");
                        return;
                    }
                }
                existing.push(normalizedUrls[0]);
                dropError = "";
                root.workspaceMode = 0;
                setDroppedVideoOrder(existing);
                pendingComparisonPreservesPosition = true;
                reviewInputDialogs.openComparison();
                return;
            }
            requestDestructiveAction({
                "kind": "openVideos",
                "urls": normalizedUrls,
                "external": false
            });
            return;
        }
        requestDestructiveAction({
            "kind": "openVideos",
            "urls": normalizedUrls,
            "external": false
        });
    }

    function reviewDroppedUrls(urls) {
        reviewUrls(urls, true);
    }

    function openNewReviewUrls(urls) {
        reviewUrls(urls, false);
    }

    function openDroppedComparison(referenceIndex) {
        if (shell)
            shell.stagedReferenceIndex = referenceIndex;
        pendingNewReviewWantsThreeUp = Boolean(shell && shell.stagedSources.length === 3);
        if (shell && !shell.openStagedSources(pendingComparisonPreservesPosition && sourceCount > 0))
            pendingNewReviewWantsThreeUp = false;
    }

    function activeSourceUrls() {
        return shell ? Array.from(shell.activeSources) : [];
    }

    function removeSelectedSource(identity) {
        const ok = shell ? shell.removeActiveSourceByIdentity(identity) : false;
        if (!ok)
            showIntentMessage(qsTr("所选视频已不可用。"));
        return ok;
    }

    function changeReference(identity) {
        const ok = shell ? shell.changeReferenceByIdentity(identity) : false;
        if (!ok)
            showIntentMessage(qsTr("所选视频已不可用。"));
        return ok;
    }

    function changeReferenceAtIndex(index) {
        if (!shell || index < 0 || index >= shell.activeSourceIdentities.length)
            return false;
        return changeReference(String(shell.activeSourceIdentities[index]));
    }

    function showImmersiveHud(message) {
        immersiveHudText = message;
        immersiveHudVisible = true;
        immersiveHudTimer.restart();
    }

    function toggleChrome() {
        if (!shell)
            return;
        shell.chromeVisible = !chromeVisible;
        if (!shell.chromeVisible) {
            viewportFrame.forceActiveFocus();
            Qt.callLater(() => viewportFrame.forceActiveFocus());
            showImmersiveHud(frameText);
        }
    }

    function toggleFullScreen() {
        if (fullScreen) {
            if (visibilityBeforeFullScreen === Window.Maximized)
                showMaximized();
            else
                showNormal();
            return;
        }
        visibilityBeforeFullScreen = visibility;
        showFullScreen();
    }

    function escapePresentationMode() {
        if (fullScreen)
            toggleFullScreen();
        else if (!chromeVisible)
            shell.chromeVisible = true;
    }

    function openManualAnchorsDialog() {
        anchorDialog.open();
    }

    function sourceLabel(slot) {
        return qsTr("源 %1").arg(String.fromCharCode(65 + slot));
    }

    function sourceMissing(slot) {
        if (slot === 0)
            return sourceAMissing;
        if (slot === 1)
            return sourceBMissing;
        return sourceCMissing;
    }

    function differenceEdgeIndex(preferenceValue) {
        for (let index = 0; index < differenceEdges.length; ++index) {
            if (Number(differenceEdges[index].preferenceValue) === preferenceValue)
                return index;
        }
        return differenceEdges.length > 0 ? 0 : -1;
    }

    function sourceOffsets() {
        const offsets = [];
        const sourceIds = Object.keys(sourceOffsetValues).sort((first, second) => Number(first) - Number(second));
        for (const sourceId of sourceIds)
            offsets.push({
                "sourceId": Number(sourceId),
                "frames": Number(sourceOffsetValues[sourceId])
            });
        return offsets;
    }

    function updateSourceOffset(sourceId, frames) {
        const next = {};
        for (const currentSourceId of Object.keys(sourceOffsetValues))
            next[currentSourceId] = sourceOffsetValues[currentSourceId];
        next[String(sourceId)] = Number(frames);
        sourceOffsetValues = next;
    }

    function sourceOffset(sourceId, fallback) {
        const key = String(sourceId);
        return Object.prototype.hasOwnProperty.call(sourceOffsetValues, key) ? Number(sourceOffsetValues[key]) : Number(fallback);
    }

    function resetSourceOffsets() {
        const next = {};
        for (const sourceId of Object.keys(sourceOffsetValues))
            next[sourceId] = 0;
        sourceOffsetValues = next;
    }

    function resetCanonicalSourceOffset() {
        const canonicalSourceId = canonicalSourceIndex >= 0 ? canonicalSourceIndex : 0;
        updateSourceOffset(canonicalSourceId, 0);
    }

    function frameAtTimelinePosition(position) {
        if (totalFrames <= 1)
            return 0;
        const normalized = Math.max(0, Math.min(1, position));
        return Math.round(normalized * (Number(totalFrames) - 1));
    }

    function alignmentMarkerColor(kind) {
        if (kind === "missing")
            return "#f87171";
        if (kind === "duplicate")
            return "#fb923c";
        if (kind === "extra")
            return "#c084fc";
        if (kind === "anchor")
            return "#22d3ee";
        if (kind === "rejected-segment")
            return "#dc2626";
        if (kind === "review-segment")
            return "#facc15";
        return "#facc15";
    }

    function focusBlocksGlobalMediaShortcuts(item) {
        let candidate = item;
        while (candidate) {
            if (candidate.blocksGlobalMediaShortcuts === true)
                return true;
            candidate = candidate.parent;
        }
        return false;
    }

    function focusIsTextEditing(item) {
        let candidate = item;
        while (candidate) {
            if (candidate.textEditingInputContext === true)
                return true;
            candidate = candidate.parent;
        }
        return false;
    }

    function focusIsPopup(item) {
        let candidate = item;
        while (candidate) {
            if (candidate.popupInputContext === true)
                return true;
            candidate = candidate.parent;
        }
        return false;
    }

    function errorDetails() {
        const errors = [];
        if (sourceAErrorKey.length > 0)
            errors.push(qsTr("源 A：%1").arg(root.messageCatalog.errorMessage(sourceAErrorKey)));
        if (sourceBErrorKey.length > 0)
            errors.push(qsTr("源 B：%1").arg(root.messageCatalog.errorMessage(sourceBErrorKey)));
        if (sourceCErrorKey.length > 0)
            errors.push(qsTr("源 C：%1").arg(root.messageCatalog.errorMessage(sourceCErrorKey)));
        if (pairErrorKey.length > 0)
            errors.push(qsTr("对比：%1").arg(root.messageCatalog.errorMessage(pairErrorKey)));
        return errors.join(" | ");
    }

    function compatibilityDetails() {
        const messages = [];
        for (const finding of compatibilityFindings) {
            const labels = [];
            for (const sourceId of finding.sources)
                labels.push(String.fromCharCode(65 + Number(sourceId)));
            let severity = qsTr("警告");
            if (Number(finding.severity) === 0)
                severity = qsTr("不兼容");
            else if (Number(finding.severity) === 2)
                severity = qsTr("需要对齐");
            messages.push(qsTr("%1：%2 — %3").arg(labels.join(" ↔ ")).arg(root.messageCatalog.errorMessage(finding.code)).arg(severity));
        }
        return messages.join(" | ");
    }

    menuBar: ApplicationMenuBar {
        id: applicationMenuBar

        visible: root.chromeVisible
        controller: root.controller
        preferences: root.preferences
        session: root.shell
        sourceCount: root.sourceCount
        busy: root.busy
        canonicalSourceIndex: root.canonicalSourceIndex
        currentViewMode: root.effectiveViewMode
        inspectorOpen: root.inspectorOpen
        graphicsReady: root.graphicsReady
        currentFrame: root.currentFrame
        alignmentAnalysisRunning: root.alignmentAnalysisRunning
        chromeVisible: root.chromeVisible
        fullScreen: root.fullScreen
        shortcutPreset: root.shortcutPreset
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        workspaceMode: root.workspaceMode
        imageHasPrimary: Boolean(root.stillImageController && root.stillImageController.hasPrimary)
        imageHasSecondary: Boolean(root.stillImageController && root.stillImageController.hasSecondary)
        automaticAlignmentPending: root.automaticAlignmentPending
        canConfirmAutomaticAlignment: root.canConfirmAutomaticAlignment
        canUndoAutomaticAlignment: root.canUndoAutomaticAlignment
        manualAnchorActive: root.manualAnchorActive
        anyManualAlignmentActive: root.anyManualAlignmentActive
        autoAlignmentActive: root.autoAlignmentActive
        openManualAnchorsDialog: root.openManualAnchorsDialog
        sourceOffsets: root.sourceOffsets
        resetSourceOffsets: root.resetSourceOffsets
        onOpenVideosRequested: reviewInputDialogs.openVideos()
        onAddVideoRequested: reviewInputDialogs.openAddVideo()
        onOpenImageRequested: {
            root.workspaceMode = 1;
            imageSingleDialog.open();
        }
        onAddImageRequested: {
            root.workspaceMode = 1;
            imageAddDialog.open();
        }
        onOpenImagePairRequested: {
            root.workspaceMode = 1;
            imagePairDialog.open();
        }
        onWorkspaceRequested: mode => root.workspaceMode = mode
        onDestructiveActionRequested: kind => root.requestDestructiveAction({
                "kind": kind,
                "external": false
            })
        onChromeToggleRequested: root.toggleChrome()
        onFullScreenToggleRequested: root.toggleFullScreen()
        onViewerFocusRequested: root.returnFocusToViewer()
    }

    ReviewShortcuts {
        id: reviewShortcuts

        controller: root.controller
        shortcutsEnabled: root.globalMediaShortcutsEnabled
        presentationShortcutsEnabled: root.presentationShortcutsEnabled
        oneSecondStepFrames: root.oneSecondStepFrames
        wipeEnabled: root.wipeMode
        wipePosition: root.wipePosition
        shortcutPreset: root.shortcutPreset
        fullScreen: root.fullScreen
        chromeVisible: root.chromeVisible
        currentFrame: root.currentFrame
        inFrame: root.inFrame
        outFrame: root.outFrame
        sourceCount: root.sourceCount
        onWipePositionRequested: position => {
            root.wipePosition = position;
            if (!root.chromeVisible)
                root.showImmersiveHud(qsTr("分割线位置 %1%").arg(Math.round(position * 100)));
        }
        onManualNavigationRequested: {
            root.revealOsc();
            if (!root.chromeVisible)
                root.manualHudPending = true;
        }
        onChromeToggleRequested: root.toggleChrome()
        onFullScreenToggleRequested: root.toggleFullScreen()
        onPresentationEscapeRequested: root.escapePresentationMode()
        onShortcutHelpRequested: shortcutHelp.open()
        onInPointRequested: root.setInPoint()
        onOutPointRequested: root.setOutPoint()
        onSelectedRangePlaybackRequested: root.playSelectedRange()
        onOpenVideosRequested: reviewInputDialogs.openVideos()
        onAddVideoRequested: reviewInputDialogs.openAddVideo()
        onCloseVideosRequested: root.requestDestructiveAction({
            "kind": "closeReview",
            "external": false
        })
    }

    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        // In full screen the presentation Escape (ReviewShortcuts) owns the key and exits
        // full screen first; the drawer can still be dismissed by clicking the scrim.
        enabled: root.drawerMode && root.inputContext === 0 && !root.fullScreen
        onActivated: {
            if (root.shell)
                root.shell.inspectorVisible = false;
        }
    }

    Timer {
        id: framePendingDelay

        interval: 140
        repeat: false
        onTriggered: root.showFramePending = root.framePending
    }

    Timer {
        id: intentMessageTimer

        interval: 5000
        repeat: false
        onTriggered: root.intentMessage = ""
    }

    Timer {
        id: immersiveHudTimer

        interval: 800
        repeat: false
        onTriggered: root.immersiveHudVisible = false
    }

    Connections {
        target: root.shell

        function onIntentEvent(intentId, status, kind, error, sourceCountValue) {
            if (Number(status) === 5)
                root.showIntentMessage(root.messageCatalog.intentErrorText(error));
            else if (Number(status) === 6)
                root.showIntentMessage(qsTr("更新的请求已取代 %1。").arg(root.messageCatalog.intentKindText(kind, sourceCountValue)));
        }

        function onIntentFinished(intentId, kind, outcome, errorKey) {
            if (Number(outcome) !== 0) {
                if (Number(kind) === 0)
                    root.pendingNewReviewWantsThreeUp = false;
                root.showIntentMessage(errorKey.length > 0 ? root.messageCatalog.errorMessage(errorKey) : qsTr("检查请求失败。"));
                return;
            }
            if (Number(kind) === 0) {
                if (root.pendingNewReviewWantsThreeUp && root.sourceCount === 3 && root.preferences)
                    root.preferences.viewMode = root.threeUpViewMode;
                root.pendingNewReviewWantsThreeUp = false;
                root.resetReviewVisualState();
            } else if (Number(kind) === 3) {
                root.showIntentMessage(qsTr("已移除视频。"));
            } else if (Number(kind) === 4) {
                root.showIntentMessage(qsTr("已更换参考源。"));
            } else if (Number(kind) === 5) {
                root.clearReviewUi();
            }
        }
    }

    Connections {
        target: root.controller

        function onStateChanged() {
            if (!root.controller || !root.shell)
                return;
            const generation = root.shell.activeGeneration;
            if (generation === root.changedOnDiskAnnouncedGeneration)
                return;
            const model = root.controller.sources;
            if (!model)
                return;
            for (let i = 0; i < model.rowCount(); ++i) {
                const idx = model.index(i, 0);
                if (model.data(idx, 0x010C)) {
                    root.changedOnDiskAnnouncedGeneration = generation;
                    root.showIntentMessage(qsTr("磁盘上的视频文件已变化，本次会话仍保留其原始标识。"));
                    return;
                }
            }
        }
    }

    ReviewInputDialogs {
        id: reviewInputDialogs

        stagedVideos: root.shell ? root.shell.stagedSources : []
        fileNameFunction: root.fileName
        pathNameFunction: root.sourcePathLabel
        initialReferenceIndex: root.pendingComparisonPreservesPosition ? root.canonicalSourceIndex : 0
        onOpenVideosAccepted: urls => root.openNewReviewUrls(urls)
        onAddVideoAccepted: url => root.reviewDroppedUrls([url])
        onMoveRequested: (fromIndex, toIndex) => root.swapDroppedVideos(fromIndex, toIndex)
        onComparisonAccepted: referenceIndex => {
            root.openDroppedComparison(referenceIndex);
        }
    }

    // Popup shell, not Dialog: a Dialog with a title adds style chrome (auto header + empty
    // footer strip) around custom content, which looks like stray bars in the dark theme.
    Popup {
        id: anchorDialog

        objectName: "manualAnchorDialog"
        width: 460
        modal: true
        closePolicy: Popup.CloseOnEscape
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        popupType: Popup.Item
        padding: 14

        property var sourceChoices: {
            const choices = [];
            const count = root.sourceCount;
            for (let index = 0; index < count; ++index) {
                if (index !== root.canonicalSourceIndex)
                    choices.push({
                        "label": String.fromCharCode(65 + index),
                        "sourceIndex": index
                    });
            }
            return choices;
        }

        onOpened: {
            canonicalAnchorFrame.value = Math.max(1, root.currentFrame + 1);
            sourceAnchorFrame.value = canonicalAnchorFrame.value;
        }

        background: Rectangle {
            radius: 8
            color: root.panelColor
            border.color: root.borderColor
        }

        contentItem: Column {
            spacing: 14

            Text {
                text: qsTr("手动对齐锚点")
                color: root.primaryTextColor
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                text: qsTr("将一个基准帧映射到非参考源中的某一帧。多个锚点保持单调。")
                color: root.mutedTextColor
                wrapMode: Text.WordWrap
                width: parent.width
            }

            Row {
                spacing: 12

                Text {
                    text: qsTr("源")
                    color: root.primaryTextColor
                    anchors.verticalCenter: parent.verticalCenter
                }

                ToolbarCombo {
                    id: anchorSource

                    objectName: "anchorSourceCombo"
                    width: 90
                    model: anchorDialog.sourceChoices
                    textRole: "label"
                    valueRole: "sourceIndex"
                }

                Text {
                    text: qsTr("基准帧")
                    color: root.primaryTextColor
                    anchors.verticalCenter: parent.verticalCenter
                }

                ReviewOffsetSpinBox {
                    id: canonicalAnchorFrame

                    objectName: "canonicalAnchorFrame"
                    from: 1
                    to: Math.max(1, Math.min(2147483647, root.totalFrames))
                    editable: true
                }
            }

            Row {
                spacing: 12

                Text {
                    text: qsTr("源帧")
                    color: root.primaryTextColor
                    anchors.verticalCenter: parent.verticalCenter
                }

                ReviewOffsetSpinBox {
                    id: sourceAnchorFrame

                    objectName: "sourceAnchorFrame"
                    from: 1
                    to: Math.max(1, Math.min(2147483647, root.totalFrames + 16))
                    editable: true
                }

                ReviewActionButton {
                    objectName: "addManualAnchorButton"
                    text: qsTr("添加 / 替换")
                    enabled: !root.busy && anchorSource.currentIndex >= 0
                    onClicked: {
                        if (root.controller.setManualAlignmentAnchor(anchorSource.currentValue, canonicalAnchorFrame.value - 1, sourceAnchorFrame.value - 1))
                            anchorDialog.close();
                    }
                }

                ReviewActionButton {
                    objectName: "clearManualAnchorsButton"
                    text: qsTr("全部清除")
                    enabled: !root.busy && root.manualAnchorActive
                    onClicked: {
                        if (root.controller.clearManualAlignmentAnchors())
                            anchorDialog.close();
                    }
                }
            }

            Text {
                text: root.manualAnchorStatus.length > 0 ? root.manualAnchorStatus : qsTr("暂无手动锚点")
                color: root.manualAnchorActive ? Theme.warning : root.mutedTextColor
                wrapMode: Text.WordWrap
                width: parent.width
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.borderColor
            }

            Text {
                objectName: "alignmentModeStatus"
                text: root.anyManualAlignmentActive ? qsTr("手动对齐") : (root.autoAlignmentActive ? qsTr("自动对齐") : qsTr("严格索引"))
                color: root.anyManualAlignmentActive || root.autoAlignmentActive ? Theme.warning : Theme.success
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }

            Text {
                objectName: "manualOffsetStatusLabel"
                text: root.manualOffsetActive ? qsTr("帧对齐偏移 · 已启用") : qsTr("帧对齐偏移")
                color: root.manualOffsetActive ? Theme.warning : root.mutedTextColor
                font.pixelSize: 11
            }

            Repeater {
                id: sourceOffsetRepeater

                objectName: "sourceOffsetRepeater"
                model: root.controller ? root.controller.sources : null

                delegate: Row {
                    id: sourceOffsetDelegate

                    required property int sourceId
                    required property int role
                    required property int manualOffset
                    property int sourceIdValue: sourceId
                    width: parent.width
                    spacing: 8

                    Text {
                        width: parent.width - sourceOffsetInput.width - parent.spacing
                        text: qsTr("源 %1 偏移（帧）").arg(String.fromCharCode(65 + sourceOffsetDelegate.sourceIdValue))
                        color: root.mutedTextColor
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    ReviewOffsetSpinBox {
                        id: sourceOffsetInput

                        objectName: "sourceOffset-" + sourceOffsetDelegate.sourceIdValue
                        textColor: root.primaryTextColor
                        mutedTextColor: root.mutedTextColor
                        accentColor: root.accentColor
                        panelColor: root.raisedPanelColor
                        borderColor: root.borderColor
                        value: root.sourceOffset(sourceOffsetDelegate.sourceIdValue, sourceOffsetDelegate.manualOffset)
                        enabled: sourceOffsetDelegate.sourceIdValue !== (root.referenceSourceIndex >= 0 ? root.referenceSourceIndex : 0) && !root.busy
                        Accessible.name: qsTr("源 %1 全局帧偏移").arg(String.fromCharCode(65 + sourceOffsetDelegate.sourceIdValue))
                        onValueChanged: root.updateSourceOffset(sourceOffsetDelegate.sourceIdValue, value)
                    }
                }
            }

            Text {
                visible: root.anyManualAlignmentActive || root.autoAlignmentActive
                text: qsTr("缺失的映射帧保持黑色；偏移不会被截断。")
                color: root.mutedTextColor
                font.pixelSize: 10
                width: parent.width
                wrapMode: Text.WordWrap
            }

            Text {
                visible: root.compatibilityDetails().length > 0
                text: root.compatibilityDetails()
                color: Theme.warning
                font.pixelSize: 10
                width: parent.width
                wrapMode: Text.WordWrap
            }
        }
    }

    ActiveSourceStrip {
        id: sourceBar

        visible: !root.imageWorkspaceActive && root.sourceCount > 1
        sourcesModel: root.controller ? root.controller.sources : null
        sourceCount: root.sourceCount
        singleMode: root.singleMode
        canonicalSourceIndex: root.canonicalSourceIndex
        canonicalSourceIdentity: root.shell ? root.shell.canonicalSourceIdentity : ""
        pendingSourceIdentities: root.shell ? root.shell.pendingSourceIdentities : []
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        z: 30
        borderColor: root.borderColor
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            leftMargin: root.singleMode ? 8 : 0
        }
        onAddRequested: reviewInputDialogs.openAddVideo()
        onRemoveRequested: sourceIdentity => root.removeSelectedSource(sourceIdentity)
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onViewerFocusRequested: root.returnFocusToViewer()
    }
    CompareModeBar {
        id: comparisonBar

        visible: !root.imageWorkspaceActive && root.sourceCount > 1
        sourceCount: root.sourceCount
        currentMode: root.effectiveViewMode
        differenceEdges: root.differenceEdges
        currentEdgeIndex: root.differenceEdgeIndex(root.differenceEdge)
        inspectorOpen: root.inspectorOpen
        busy: root.busy
        borderColor: root.borderColor
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        anchors {
            top: sourceBar.bottom
            left: parent.left
            right: parent.right
        }
        onModeRequested: mode => root.preferences.viewMode = mode
        onEdgeRequested: edge => root.preferences.differenceEdge = edge
        onInspectorRequested: root.shell.inspectorVisible = !root.inspectorOpen
    }
    Rectangle {
        id: inspectorScrim

        objectName: "inspectorScrim"
        z: 19
        visible: root.drawerMode
        color: Theme.modalScrim
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            top: parent.top
            topMargin: root.singleMode ? 0 : sourceBar.height + comparisonBar.height
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.shell)
                    root.shell.inspectorVisible = false;
            }
        }
    }

    TabbedInspector {
        id: alignmentBar

        controller: root.controller
        preferences: root.preferences
        session: root.shell
        borderColor: root.borderColor
        primaryTextColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        singleMode: root.singleMode
        sourceCount: root.sourceCount
        wipeMode: root.wipeMode
        differenceMode: root.differenceMode
        analysisGridMode: root.analysisGridMode
        differenceEdges: root.differenceEdges
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        differenceEdge: root.differenceEdge
        referenceSourceIndex: root.referenceSourceIndex
        differenceThresholdEnabled: root.differenceThresholdEnabled
        differenceThresholdCode: root.differenceThresholdCode
        differenceThresholdPolicy: root.differenceThresholdPolicy
        wipePosition: root.wipePosition
        roiEnabled: root.roiEnabled
        graphicsReady: root.graphicsReady
        dropFrameTimecode: root.dropFrameTimecode
        currentFrame: root.currentFrame
        inFrame: root.inFrame
        outFrame: root.outFrame
        rangePlaybackActive: root.rangePlaybackActive
        visible: !root.imageWorkspaceActive && root.chromeVisible && root.inspectorOpen && root.sourceCount > 0
        z: 20
        anchors {
            top: parent.top
            topMargin: root.singleMode ? 0 : sourceBar.height + comparisonBar.height
            right: parent.right
            bottom: parent.bottom
            bottomMargin: 0
        }
        onDifferenceEdgeRequested: edge => root.preferences.differenceEdge = edge
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onDifferenceThresholdEnabledRequested: enabled => root.differenceThresholdEnabled = enabled
        onDifferenceThresholdCodeRequested: code => root.differenceThresholdCode = code
        onDifferenceThresholdPolicyRequested: policy => root.differenceThresholdPolicy = policy
        onWipePositionRequested: position => root.wipePosition = position
        onResetViewportRequested: root.resetViewport()
        onClearRoiRequested: root.clearRoi()
        onDropFrameTimecodeRequested: enabled => root.setDropFrameTimecode(enabled)
        onInPointRequested: root.setInPoint()
        onOutPointRequested: root.setOutPoint()
        onClearRangeRequested: root.clearSelectedRange()
        onRangeLoopToggleRequested: root.toggleRangeLoop()
    }
    ComparisonViewport {
        id: viewportFrame

        visible: !root.imageWorkspaceActive
        preferences: root.preferences
        borderColor: root.borderColor
        accentColor: root.accentColor
        primaryTextColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        errorColor: root.errorColor
        chromeVisible: root.chromeVisible
        effectiveViewMode: root.effectiveViewMode
        wipePosition: root.wipePosition
        selectedDifferenceExactness: root.selectedDifferenceExactness
        differenceThresholdEnabled: root.differenceThresholdEnabled
        differenceThresholdCode: root.differenceThresholdCode
        differenceThresholdPolicy: root.differenceThresholdPolicy
        referenceSourceIndex: root.referenceSourceIndex
        sourceCount: root.sourceCount
        wipeMode: root.wipeMode
        differenceMode: root.differenceMode
        analysisGridMode: root.analysisGridMode
        immersiveHudVisible: root.immersiveHudVisible
        immersiveHudText: root.immersiveHudText
        showFramePending: root.showFramePending
        currentFrame: root.currentFrame
        differenceUnavailableDetail: root.differenceUnavailableDetail
        combinedAlignmentStatus: root.combinedAlignmentStatus
        singleMode: root.singleMode
        differenceFirstSlot: root.differenceFirstSlot
        effectiveDifferenceEdge: root.differenceEdge
        sourceNames: [root.sourceAName, root.sourceBName, root.sourceCName]
        sourceMediaInfo: root.controller ? root.controller.sourceMediaInfo : []
        frameErrorBannerVisible: root.frameErrorBannerVisible
        errorDetail: root.errorDetails()
        overlayVisible: root.overlayVisible
        hasErrors: root.hasErrors
        busy: root.busy
        overlayTitle: root.overlayTitle
        overlayDetail: root.overlayDetail
        anchors {
            top: parent.top
            topMargin: root.chromeVisible && !root.singleMode ? sourceBar.height + comparisonBar.height + 6 : 0
            bottom: parent.bottom
            bottomMargin: root.transportDocked ? root.transportDockHeight : 0
            left: parent.left
            leftMargin: root.chromeVisible ? 14 : 0
            right: alignmentBar.visible && root.width >= 1120 ? alignmentBar.left : parent.right
            rightMargin: root.chromeVisible ? 14 : 0
        }
        onWipePositionRequested: position => root.wipePosition = position
        onOscRevealRequested: root.revealOsc()
        onContextMenuRequested: root.openViewerContextMenu()
        onFullScreenToggleRequested: root.toggleFullScreen()
    }
    ImageWorkspace {
        id: imageWorkspace

        objectName: "imageWorkspaceRoot"
        visible: root.imageWorkspaceActive
        controller: root.stillImageController
        anchors {
            top: parent.top
            topMargin: root.chromeVisible ? 10 : 0
            bottom: parent.bottom
            left: parent.left
            leftMargin: root.chromeVisible ? 14 : 0
            right: parent.right
            rightMargin: root.chromeVisible ? 14 : 0
        }
        onOpenImageRequested: {
            root.workspaceMode = 1;
            imageSingleDialog.open();
        }
        onAddImageRequested: {
            root.workspaceMode = 1;
            imageAddDialog.open();
        }
        onOpenPairRequested: {
            root.workspaceMode = 1;
            imagePairDialog.open();
        }
    }
    NativeDialogs.FileDialog {
        id: imageSingleDialog

        objectName: "imageSingleDialog"
        title: qsTr("打开图片")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;
            if (picked && picked.toString().length > 0)
                root.performImageReview([picked]);
        }
    }
    NativeDialogs.FileDialog {
        id: imageAddDialog

        objectName: "imageAddDialog"
        title: qsTr("添加图片")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;
            if (picked && picked.toString().length > 0)
                root.stillImageController.openSecondary(picked);
        }
    }
    NativeDialogs.FileDialog {
        id: imagePairDialog

        objectName: "imagePairDialog"
        title: qsTr("打开图片对")
        fileMode: NativeDialogs.FileDialog.OpenFiles
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const files = selectedFiles && selectedFiles.length > 0 ? selectedFiles : [selectedFile];
            root.performImageReview(files.filter(url => url && url.toString().length > 0));
        }
    }
    EmptyReviewView {
        visible: !root.imageWorkspaceActive && root.sourceCount === 0 && !root.busy && root.graphicsReady && !root.hasErrors
        z: 35
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        anchors.fill: viewportFrame
        onOpenVideosRequested: reviewInputDialogs.openVideos()
    }
    TimelineThumbnailCache {
        id: thumbnailCache

        sourceItem: viewportFrame.videoOutput
        currentFrame: Number(root.currentFrame)
        totalFrames: Number(root.totalFrames)
        generation: root.shell ? root.shell.activeGeneration : 0
    }
    PlayerOsc {
        id: transport

        z: 50
        controllerState: root.imageWorkspaceActive ? 2 : root.oscState
        docked: root.transportDocked
        sourceLabel: root.sourceAName
        playing: root.playing
        playbackRate: root.controller ? Number(root.controller.playbackRate) : 1
        timelineEnabled: root.timelineEnabled
        currentFrame: Number(root.currentFrame)
        totalFrames: Number(root.totalFrames)
        progress: root.timelineProgress
        timecodeText: root.currentTimecode
        markers: root.alignmentTimelineMarkers
        actions: reviewShortcuts.actions
        focusTarget: viewportFrame
        canFirst: root.canFirstAction
        canPrevious: root.canPreviousAction
        canPlay: root.canPlayAction
        canPause: root.canPauseAction
        canNext: root.canNextAction
        canLast: root.canLastAction
        inFrame: root.inFrame
        outFrame: root.outFrame
        loopRangeActive: root.rangePlaybackActive
        previewFrame: root.timelinePreviewSampleFrame
        previewTimecode: root.previewTimecode
        previewThumbnailSource: thumbnailCache.urlForFrame(root.timelinePreviewFrame)
        anchors {
            left: viewportFrame.left
            right: root.drawerMode ? alignmentBar.left : viewportFrame.right
            bottom: parent.bottom
        }
        onSeekRequested: frame => {
            root.revealOsc();
            root.timelinePreviewFrame = frame;
            root.controller.seekFrame(frame);
        }
        onPreviewRequested: frame => root.timelinePreviewFrame = frame
    }

    ShortcutHelpOverlay {
        id: shortcutHelp

        playerPreset: root.shortcutPreset === 1
    }

    ReviewContextMenu {
        id: viewerContextMenu

        sourceCount: root.sourceCount
        canonicalSourceIndex: root.canonicalSourceIndex
        currentViewMode: root.effectiveViewMode
        fullScreen: root.fullScreen
        differenceEdges: root.differenceEdges
        currentEdgeIndex: root.differenceEdgeIndex(root.differenceEdge)
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        // qmllint disable unqualified
        onSideRequested: root.preferences.viewMode = ComparisonSurface.SideBySide
        onWipeRequested: root.preferences.viewMode = ComparisonSurface.Wipe
        onDiffRequested: root.preferences.viewMode = ComparisonSurface.Difference
        // qmllint enable unqualified
        onEdgeRequested: edge => root.preferences.differenceEdge = edge
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onOpenRequested: reviewInputDialogs.openVideos()
        onInspectorRequested: root.shell.inspectorVisible = true
        onFullScreenRequested: root.toggleFullScreen()
        onViewerFocusRequested: root.returnFocusToViewer()
    }

    Rectangle {
        id: intentQueuePanel

        readonly property var runningIntent: root.shell ? root.shell.activeIntent : ({})
        readonly property var queued: root.shell ? root.shell.queuedIntents : []
        visible: Number(runningIntent.id || 0) > 0 || queued.length > 0
        z: 890
        width: 286
        height: queueColumn.implicitHeight + 20
        radius: 7
        color: "#f21d2635"
        border.color: root.borderColor
        anchors {
            top: parent.top
            right: parent.right
            topMargin: root.chromeVisible ? 58 : 18
            rightMargin: 18
        }

        Column {
            id: queueColumn

            spacing: 7
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: 10
            }

            Text {
                visible: Number(intentQueuePanel.runningIntent.id || 0) > 0
                text: qsTr("处理中 · %1").arg(root.messageCatalog.intentKindText(intentQueuePanel.runningIntent.kind, intentQueuePanel.runningIntent.sourceCount))
                color: root.primaryTextColor
                font.pixelSize: 12
                elide: Text.ElideRight
                width: parent.width
            }

            Row {
                visible: intentQueuePanel.queued.length > 0
                width: parent.width

                Text {
                    text: qsTr("%1 个请求排队中").arg(intentQueuePanel.queued.length)
                    color: root.mutedTextColor
                    font.pixelSize: 12
                    width: parent.width - cancelAllButton.width
                    anchors.verticalCenter: parent.verticalCenter
                }

                VcsToolButton {
                    id: cancelAllButton

                    text: qsTr("全部取消")
                    implicitWidth: 74
                    implicitHeight: 24
                    labelPixelSize: 11
                    onClicked: root.shell.cancelAllQueuedIntents()
                }
            }

            Repeater {
                model: intentQueuePanel.queued

                delegate: Row {
                    id: queuedIntentRow

                    required property var modelData
                    width: queueColumn.width

                    Text {
                        text: root.messageCatalog.intentKindText(queuedIntentRow.modelData.kind, queuedIntentRow.modelData.sourceCount)
                        color: root.primaryTextColor
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        width: parent.width - cancelQueuedButton.width
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    VcsToolButton {
                        id: cancelQueuedButton

                        text: qsTr("取消")
                        implicitWidth: 58
                        implicitHeight: 22
                        labelPixelSize: 11
                        onClicked: root.shell.cancelQueuedIntent(queuedIntentRow.modelData.id)
                    }
                }
            }
        }
    }

    Rectangle {
        visible: root.intentMessage.length > 0
        z: 900
        radius: 6
        color: "#ed1d2635"
        border.color: root.borderColor
        width: Math.max(0, Math.min(root.width - 48, intentToastText.implicitWidth + 32))
        height: intentToastText.paintedHeight + 20
        anchors {
            horizontalCenter: parent.horizontalCenter
            top: parent.top
            topMargin: root.chromeVisible ? 58 : 18
        }

        Text {
            id: intentToastText

            anchors.centerIn: parent
            width: Math.max(0, parent.width - 32)
            text: root.intentMessage
            color: root.primaryTextColor
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
    Rectangle {
        id: chromeRestorePill

        objectName: "chromeRestorePill"
        // With the chrome hidden (Tab / presentation mode) every menu and bar disappears and the
        // immersive HUD fades after 800 ms. This persistent pill is the always-visible way back:
        // it names the Tab shortcut and restores the interface on click.
        visible: !root.chromeVisible
        z: 980
        height: 32
        radius: 16
        color: restorePillHover.hovered ? "#e61d2635" : "#c21d2635"
        border.color: "#46597a"
        border.width: 1
        opacity: restorePillHover.hovered ? 1.0 : 0.62
        anchors {
            top: parent.top
            topMargin: 14
            horizontalCenter: parent.horizontalCenter
        }

        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }

        HoverHandler {
            id: restorePillHover
        }

        TapHandler {
            onTapped: root.toggleChrome()
        }

        Row {
            spacing: 7
            anchors.centerIn: parent

            Text {
                text: "↑"
                color: root.primaryTextColor
                font.pixelSize: 13
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: qsTr("界面已隐藏 · 按 Tab 或点击恢复")
                color: root.primaryTextColor
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
    DropArea {
        id: workspaceDropArea

        objectName: "workspaceDropArea"
        anchors.fill: parent
        z: 0
        onEntered: drag => {
            if (drag.hasUrls)
                drag.acceptProposedAction();
        }
        onDropped: drop => {
            drop.acceptProposedAction();
            root.reviewDroppedUrls(drop.urls);
        }
    }

    Rectangle {
        id: workspaceDropOverlay

        anchors.fill: parent
        z: 1000
        enabled: false
        visible: workspaceDropArea.containsDrag
        color: "#df0b1421"
        border.width: 3
        border.color: root.accentColor

        Column {
            spacing: 10
            anchors.centerIn: parent

            Text {
                text: qsTr("松开即可在 VCStation 中打开")
                color: root.primaryTextColor
                font.pixelSize: 24
                font.bold: true
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                text: qsTr("1–3 个视频或图片")
                color: root.mutedTextColor
                font.pixelSize: 14
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }

    Rectangle {
        visible: root.dropError.length > 0
        z: 1100
        width: Math.min(parent.width - 48, 620)
        height: dropErrorText.implicitHeight + 26
        radius: 7
        color: "#f0351f2a"
        border.color: "#a9503f4a"
        anchors {
            top: parent.top
            topMargin: 18
            horizontalCenter: parent.horizontalCenter
        }

        Text {
            id: dropErrorText

            width: parent.width - 34
            text: root.dropError
            color: "#ffb4b4"
            wrapMode: Text.WordWrap
            anchors.centerIn: parent
        }

        TapHandler {
            onTapped: root.dropError = ""
        }
    }
}
