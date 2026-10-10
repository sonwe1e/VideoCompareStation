pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import QtQuick.Window
import "VcsTheme.js" as Theme
import "MediaLabels.js" as MediaLabels
import "ShortcutCatalog.js" as ShortcutCatalog

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
    title: Qt.application.version.length > 0 ? qsTr("CompareStation %1 — 视觉审查").arg(Qt.application.version) : qsTr("CompareStation — 视觉审查")
    color: Theme.window

    readonly property color panelColor: Theme.menu
    readonly property color raisedPanelColor: Theme.raisedPanel
    readonly property color borderColor: Theme.border
    readonly property color accentColor: Theme.accent
    readonly property color mutedTextColor: Theme.mutedText
    readonly property color primaryTextColor: Theme.primaryText

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

    // 0 = video compare workspace, 1 = still-image workspace. Kept writable so existing

    // automation can select the visible workspace; routing code should call activateWorkspace().

    property int workspaceMode: 0
    property bool issueLogPanelVisible: false
    // qmllint disable unqualified
    readonly property var issueLogModel: typeof issueLog !== "undefined" ? issueLog : null
    readonly property var comparisonExportService: typeof comparisonExport !== "undefined" ? comparisonExport : null
    // Range clip-export controller from the composition root. Only the composition root knows
    // whether a lossless exporter exists, so its presence alone decides whether the transport
    // shows an export chip; the typeof guard keeps lightweight QML-only harnesses valid.
    readonly property var clipExportService: typeof clipExport !== "undefined" ? clipExport : null
    // Performance automation keeps the playhead deterministic: a resume restore here would move
    // the gate's frame-zero baseline seconds after the open and fail the run.
    readonly property bool performanceAutomation: typeof dvsPerformanceAutomation !== "undefined" && dvsPerformanceAutomation
    // qmllint enable unqualified
    readonly property bool imageWorkspaceActive: workspaceSession.imageActive

    // Folder-comparison state: two staged folder URLs, selection progress, sidebar toggle.

    property url imageFolderLeftUrl: ""
    property url imageFolderRightUrl: ""
    property int imageFolderStage: 0
    property bool imageFolderSidebarVisible: true

    // T4 asynchronous image-open state. A request is pending until openFinished arrives;

    // until then the previous committed pair stays visible and no workspace commit happens.

    property int pendingImageRequestId: -1
    property string pendingImageOpenKind: ""
    property int pendingFolderPairRow: -1

    // Captured from the C++ context property so nested scopes never resolve a same-named

    // Item property (ImageWorkspace.imageReview) into a circular binding. The typeof guard

    // keeps lightweight QML-only tests without the image context valid and warning-free.

    // qmllint disable unqualified

    readonly property var stillImageController: typeof imageReview !== "undefined" ? imageReview : null
    // Step-3 image editing service from the composition root; the typeof guard keeps
    // lightweight QML-only harnesses valid (they simply see no edit tools).
    readonly property var imageEditService: typeof imageEdit !== "undefined" ? imageEdit : null
    readonly property var folderPairModel: typeof imageFolderPairs !== "undefined" ? imageFolderPairs : null
    readonly property var videoFolderModel: typeof videoFolder !== "undefined" ? videoFolder : null
    property bool videoFolderSidebarVisible: false
    readonly property bool videoFolderListVisible: videoFolderModel !== null && videoFolderSidebarVisible && chromeVisible
    readonly property real videoFolderInset: videoFolderListVisible ? 268 : 0
    // V-07 pair-metrics controller from the composition root; the typeof guard keeps
    // lightweight QML-only tests (no metrics service) valid and warning-free.
    readonly property var pairMetricsModel: typeof pairMetrics !== "undefined" ? pairMetrics : null

    // qmllint enable unqualified

    property string immersiveHudText: ""
    property bool immersiveHudVisible: false
    property bool manualHudPending: false
    property bool showFramePending: false
    property real wipePosition: 0.5
    // Hold-to-peek ("按住看原图"): while the toolbar button is held, the difference pass
    // is replaced by the reference (GT) source of the session - the same slot ReferenceFocus
    // puts in the main panel - so the shortcut, the button and the hint all name one target.
    // Transient; not persisted.
    property bool differencePeekActive: false
    // Keyboard peek (backtick) can lock the raw-reference view; unlocking clears both.
    property bool differencePeekLocked: false
    // The peek is only offered while the session actually identifies a GT slot. Showing a
    // candidate under a "reference original" label would be worse than offering nothing.
    readonly property bool differencePeekTargetValid: {
        const index = root.referenceSourceIndex;
        return index >= 0 && index < root.sourceCount;
    }
    // What the peek is looking at, named for the indicator plate: "B · clip.mp4".
    readonly property string differencePeekTargetLabel: {
        const index = root.referenceSourceIndex;
        if (index < 0 || index >= root.sourceCount)
            return "";
        const names = [root.sourceAName, root.sourceBName, root.sourceCName];
        const name = index < names.length ? String(names[index] || "") : "";
        return name.length > 0 ? name : qsTr("源 %1").arg(String.fromCharCode(65 + index));
    }
    // The record frozen by the most recent M press, waiting for its note.
    property int pendingIssueNoteIndex: -1
    // A peek that names a reference source cannot outlive the session that provided it: a new
    // open (or a session that lost its GT slot) releases the peek instead of showing a candidate
    // under the "reference original" label.
    onDifferencePeekTargetValidChanged: {
        if (!root.differencePeekTargetValid) {
            root.differencePeekActive = false;
            root.differencePeekLocked = false;
        }
    }
    // Workspace-level help rows. The sequences come from ShortcutCatalog.js - the same
    // constants the bindings and the menu labels use - and the contract test cross-checks
    // rows, labels and bindings against each other.
    readonly property var workspaceHelpEntries: [[ShortcutCatalog.openVideos, qsTr("打开视频")], [ShortcutCatalog.openVideoFolder, qsTr("从文件夹选视频")], [ShortcutCatalog.addVideo, qsTr("向当前对比添加视频")], [ShortcutCatalog.openImage, qsTr("打开图片")], [ShortcutCatalog.openImagePair, qsTr("打开图片对")], [ShortcutCatalog.addImage, qsTr("向当前图片对添加图片")], [ShortcutCatalog.compareImageFolders, qsTr("对比图片文件夹")], [ShortcutCatalog.closeCurrentTask, qsTr("关闭当前任务")], [ShortcutCatalog.captureIssue, qsTr("记录当前问题，可填备注")]]
    property bool pendingComparisonPreservesPosition: false
    property bool pendingSidebarComparison: false
    property bool pendingNewReviewWantsThreeUp: false
    // Issue-restore state machine. An openSources() acceptance is only a request; busy stays
    // set until the session's first frame is presented, and a viewport restore attempted
    // before that is silently dropped by the surface. Stage 1 waits out the open, stage 2
    // waits for the recorded frame to be presented, then replays the viewport.
    property var pendingIssueRestore: null
    property int pendingIssueRestoreStage: 0
    property bool pendingIssueRestoreContextApplied: false
    // Whether the recorded comparison pair was accepted by the controller. False with a recorded
    // edge means the restore is partial; the message at the end of the machine says so.
    property bool pendingIssueRestorePairApplied: true
    property var pendingIssueRestoreOpenCommandId: 0
    property var pendingIssueRestoreOpenSessionEpoch: 0
    property int pendingIssueRestoreOpenOutcome: -1
    property var pendingIssueRestoreSessionEpoch: 0
    property var pendingIssueRestorePairCommandId: 0
    property var pendingIssueRestoreSeekCommandId: 0
    property bool issueRestoreContinuing: false
    property string dropError: ""
    property string intentMessage: ""
    // C1 resume: the identity currently open, and the one identity a restore has already been
    // attempted for. The guard is per-identity rather than per-session so that reopening the same
    // file inside one window still resumes, but a reload of the same file does not fight the
    // position the user is already at.
    readonly property string resumeSourceIdentity: controller ? String(controller.currentSourceIdentity || "") : ""
    property string resumeAttemptedIdentity: ""
    readonly property int inFrame: shell ? Number(shell.inFrame) : -1
    readonly property int outFrame: shell ? Number(shell.outFrame) : -1
    readonly property real inMediaTime: shell ? Number(shell.inMediaTime) : -1
    readonly property real outMediaTime: shell ? Number(shell.outMediaTime) : -1
    readonly property bool rangePlaybackActive: Boolean(controller && controller.playbackRangeLoopActive) || Boolean(shell && shell.rangePlaybackActive && controller && controller.playbackRangeLoop)
    // The shell endpoints update immediately; canExport follows the authoritative snapshot via
    // availabilityChanged. Both are required so an asynchronous range command cannot leave the
    // export chip disabled after the coordinator accepts it.
    readonly property bool rangeExportVisible: root.clipExportService !== null
    readonly property bool rangeExportEnabled: {
        // `canExport` carries the parts only the session knows (a video session, a canonical source
        // and a usable file path); the endpoints are checked here so the gate also follows the
        // range the user is currently marking.
        if (root.clipExportService === null || root.inFrame < 0 || root.outFrame < root.inFrame)
            return false;
        return Boolean(root.clipExportService.canExport);
    }
    readonly property int shortcutPreset: preferences ? Number(preferences.shortcutPreset) : 0
    readonly property bool dropFrameTimecode: Boolean(preferences && preferences.dropFrameTimecode)
    property int changedOnDiskAnnouncedGeneration: -1

    // Single owner of workspace intent vs. committed content. activeMedia is the workspace

    // the user is looking at; pendingMedia is a selector/staging intent and must never change

    // the visible workspace until an open operation actually commits. committedMedia/Identity

    // record the last successfully committed task so closing or switching never falls back to

    // an older hidden canvas by accident.

    QtObject {
        id: workspaceSession

        objectName: "workspaceSession"
        readonly property int videoMedia: 0
        readonly property int imageMedia: 1
        readonly property int activeMedia: root.workspaceMode
        property int pendingMedia: -1
        property int committedMedia: 0
        property string committedIdentity: ""
        property int commitRevision: 0
        readonly property bool videoActive: activeMedia === videoMedia
        readonly property bool imageActive: activeMedia === imageMedia
        readonly property bool openPending: pendingMedia >= 0

        function beginOpen(media) {
            pendingMedia = Number(media) === imageMedia ? imageMedia : videoMedia;
        }

        function cancelOpen() {
            pendingMedia = -1;
        }

        function markCommitted(media, identity) {
            committedMedia = Number(media) === imageMedia ? imageMedia : videoMedia;

            committedIdentity = String(identity || "");

            commitRevision += 1;

            pendingMedia = -1;
        }

        function clearCommitted(media) {
            if (committedMedia !== Number(media))
                return;

            committedIdentity = "";

            commitRevision += 1;
        }
    }

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
    readonly property var sourceMediaInfo: controller ? controller.sourceMediaInfo : []
    readonly property var videoSessionEpoch: controller ? controller.sessionEpoch : 0
    onVideoSessionEpochChanged: {
        root.differencePeekActive = false;
        root.differencePeekLocked = false;
    }
    readonly property bool singleMode: sourceCount === 1
    readonly property bool imageHasContent: Boolean(stillImageController && (stillImageController.hasPrimary || stillImageController.hasSecondary))
    readonly property bool videoHasSession: sourceCount > 0
    readonly property bool activeTaskHasMedia: workspaceSession.imageActive ? imageHasContent : videoHasSession
    readonly property int preferredOscState: preferences ? Number(preferences.oscMode) : -1
    property bool immersiveOscRevealed: false
    readonly property int oscState: sourceCount === 0 ? 2 : (!chromeVisible ? (immersiveOscRevealed ? 1 : 2) : (preferredOscState >= 0 ? preferredOscState : (singleMode ? 1 : 0)))
    readonly property bool transportHidden: oscState === 2
    readonly property bool transportDocked: !transportHidden && oscState === 0
    readonly property int transportDockHeight: transportDocked ? transport.height + 8 : 0
    // How far the floating transport rises into the stage from the viewport's bottom edge. When the
    // transport is docked it occupies its own band below the viewport and the viewport's own corner
    // controls need no extra room; when it floats over the stage, the viewport lifts its fit/reset row
    // and pixel-scale badge above it. Without this the row sat at y=568 under a transport spanning
    // 437..640 at the 960 px minimum width - permanently unreachable.
    // See ui.MainQmlContractTests.NarrowWindowKeepsViewportCornerControlsReachable.
    readonly property real transportOverlayInset: {
        if (transportHidden || transportDocked || transport.y <= 0)
            return 0;
        const viewportFloor = viewportFrame.y + viewportFrame.height;
        return Math.max(0, viewportFloor - transport.y) + 8;
    }
    readonly property string pairErrorKey: controller ? controller.pairErrorKey : ""
    readonly property string frameMappingStatus: controller ? controller.frameMappingStatus : ""
    readonly property string alignmentEstimateStatus: controller ? controller.alignmentEstimateStatus : ""
    readonly property string sequenceAlignmentStatus: controller ? controller.sequenceAlignmentStatus : ""
    readonly property bool alignmentAnalysisRunning: Boolean(controller && controller.alignmentAnalysisRunning)
    readonly property string alignmentAnalysisStatus: controller ? controller.alignmentAnalysisStatus : ""
    readonly property string manualAnchorStatus: controller ? controller.manualAnchorStatus : ""
    readonly property var alignmentTimelineMarkers: controller ? controller.alignmentTimelineMarkers : []
    readonly property bool manualAnchorActive: Boolean(controller && controller.manualAnchorActive)
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
    readonly property bool analysisGridMode: effectiveViewMode === ComparisonSurface.AnalysisGrid
    readonly property bool differenceMode: effectiveViewMode === ComparisonSurface.Difference || analysisGridMode
    readonly property bool wipeMode: effectiveViewMode === ComparisonSurface.Wipe
    readonly property int threeUpViewMode: ComparisonSurface.ThreeUp

    // qmllint enable unqualified

    readonly property int differenceEdge: shell ? Number(shell.effectiveDifferenceEdge) : 0
    property bool differenceThresholdEnabled: false
    property int differenceThresholdCode: 0
    property int differenceThresholdPolicy: 1
    // Step-2 "固定 GT 切候选": pair modes (wipe / difference / analysis grid) with three
    // sources can flip the candidate side against the fixed reference in one action.
    readonly property bool candidateSwitchAvailable: sourceCount === 3 && (wipeMode || differenceMode)
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
            return qsTr("逐像素精确差异要求分辨率、像素格式、位深、色彩元数据一致，且两侧使用 ExactIndex 映射并具有相同时间戳。");

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
    readonly property int playbackRunSkippedFrameSets: controller ? Number(controller.playbackRunSkippedFrameSets || 0) : 0
    // Frames the current run actually finished presenting. 0 means nothing has been observed yet,
    // so the skip count next to it is "not measured", not "clean" (product section 2).
    readonly property int playbackRunPresentedFrames: controller ? Number(controller.playbackRunPresentedFrames || 0) : 0
    readonly property real playbackTargetRate: controller ? Number(controller.playbackTargetRate || 1) : 1
    readonly property real playbackPresentationRate: controller ? Number(controller.playbackPresentationRate || 0) : 0
    readonly property int playbackLagMilliseconds: controller ? Math.round(Number(controller.playbackLagMicroseconds || 0) / 1000) : 0
    readonly property bool playbackCatchingUp: controller ? Boolean(controller.playbackCatchingUp) : false
    readonly property int sourceDuplicateCount: {
        if (!alignmentTimelineMarkers)
            return 0;
        let n = 0;
        for (let i = 0; i < alignmentTimelineMarkers.length; ++i) {
            const marker = alignmentTimelineMarkers[i];
            if (marker && marker.kind === "duplicate")
                ++n;
        }
        return n;
    }
    readonly property string playbackModeLabel: {
        const policy = Number(controller ? controller.playbackContinuityPolicy : 2);
        return policy === 0 ? qsTr("逐帧完整审查") : qsTr("正常速度观看");
    }
    readonly property string playbackModeDetail: {
        const policy = Number(controller ? controller.playbackContinuityPolicy : 2);
        return policy === 0 ? qsTr("不跳过整组；落后时放慢以保留每组画面") : qsTr("落后超过约 2 秒时跳过整组追上时间");
    }
    readonly property string sourceRateText: {
        const rate = controller ? String(controller.rationalFrameRate || "") : "";
        return rate.length > 0 ? qsTr("源 %1 · 屏刷新无法逐帧呈现时以逐帧审查核对单帧").arg(rate) : "";
    }
    readonly property real frameProgress: currentFrame >= 0 && totalFrames > 1 ? Math.max(0, Math.min(1, Number(currentFrame) / (Number(totalFrames) - 1))) : 0
    readonly property real timelineProgress: timelineDragging && timelinePreviewFrame >= 0 && totalFrames > 1 ? Number(timelinePreviewFrame) / (Number(totalFrames) - 1) : frameProgress
    readonly property bool timelineEnabled: graphicsReady && !busy && Boolean(controller && controller.canFirst) && totalFrames > 0
    readonly property bool anyMenuOpen: Boolean(applicationMenuBar && applicationMenuBar.anyMenuOpen) || Boolean(sourceBar && sourceBar.anyMenuOpen) || Boolean(viewerContextMenu && viewerContextMenu.anyMenuOpen) || Boolean(videoFolderSidebar && videoFolderSidebar.anyMenuOpen)

    // Modal/input routing is intentionally computed in one place. Native image dialogs are

    // included even though they are not Popups, so a selector opened from the video workspace

    // can never let media shortcuts reach the hidden video session.

    readonly property int inputContext: Boolean(reviewInputDialogs && reviewInputDialogs.modalVisible) || anchorDialog.visible || shortcutHelp.visible || issueNoteDialog.visible || imageSingleDialog.visible || imageAddDialog.visible || imagePairDialog.visible || imageReplacePrimaryDialog.visible || imageReplaceSecondaryDialog.visible || imageFolderLeftDialog.visible || imageFolderRightDialog.visible ? 3 : (anyMenuOpen || focusIsPopup(root.activeFocusItem) ? 2 : (focusIsTextEditing(root.activeFocusItem) ? 1 : 0))
    readonly property bool globalMediaShortcutsEnabled: workspaceSession.videoActive && inputContext === 0 && (!chromeVisible || !focusBlocksGlobalMediaShortcuts(root.activeFocusItem))
    // View shortcuts (digit modes, fit/native/zoom, peek) need a drawable video stage and no
    // modal on top; they stay available while the OSC is hidden, like the transport keys.
    readonly property bool videoViewShortcutsEnabled: workspaceSession.videoActive && inputContext === 0 && sourceCount > 0 && !overlayVisible
    readonly property bool presentationShortcutsEnabled: inputContext === 0
    readonly property bool frameErrorBannerVisible: hasErrors && currentFrame >= 0 && !busy && graphicsReady && Boolean(controller && controller.canFirst)
    readonly property string overlayTitle: busy ? qsTr("正在加载…") : (!graphicsReady ? qsTr("图形设备不可用") : (hasErrors ? qsTr("无法打开文件") : qsTr("把视频或图片拖到这里")))
    readonly property string overlayDetail: busy ? qsTr("请稍候，正在准备媒体。") : (!graphicsReady ? qsTr("图形设备就绪前，导航和打开操作不可用。") : (hasErrors ? errorDetails() : qsTr("打开一个视频可播放和逐帧检查，两三个视频可对比；图片也可直接拖入查看。")))
    readonly property bool overlayVisible: (busy && currentFrame < 0) || !graphicsReady || (hasErrors && !frameErrorBannerVisible)
    readonly property string currentTimecode: controller ? controller.timecodeForFrame(currentFrame, dropFrameTimecode) : "00:00:00:00"

    // The hover popup must only ever describe the frame its thumbnail actually shows. Thumbnails

    // are captured at sample-interval frames, so display the nearest sample instead of the raw

    // hover frame; when nothing is cached the popup stays hidden (see PlayerOsc).

    readonly property var previewInfo: thumbnailCache.previewInfoForFrame(timelinePreviewFrame)
    // qmllint disable unqualified
    readonly property var previewThumbnailsModel: typeof previewThumbnails !== "undefined" ? previewThumbnails : null
    // qmllint enable unqualified
    readonly property int effectivePreviewFrame: timelinePreviewFrame >= 0 ? timelinePreviewFrame : timelinePreviewSampleFrame
    onTimelinePreviewFrameChanged: {
        if (root.previewThumbnailsModel && root.timelinePreviewFrame >= 0)
            root.previewThumbnailsModel.request(root.timelinePreviewFrame);
    }
    readonly property int timelinePreviewSampleFrame: timelinePreviewFrame >= 0 && totalFrames > 0 ? thumbnailCache.nearestSample(timelinePreviewFrame) : -1
    readonly property string previewTimecode: controller && effectivePreviewFrame >= 0 ? controller.timecodeForFrame(effectivePreviewFrame, dropFrameTimecode) : "00:00:00:00"
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

        // C1 resume: record the position the user actually reached, keyed by the identity of the
        // file that produced it. Debounced through a timer rather than written per frame, because
        // the settings save is already debounced and a per-frame write would turn scrubbing into
        // disk IO.
        resumeRecordTimer.restart();
    }

    onSourceCountChanged: {
        Qt.callLater(remapReviewRange);
        Qt.callLater(attemptResume);
    }

    onCanonicalSourceIndexChanged: {
        Qt.callLater(remapReviewRange);

        revealOsc();
    }

    onResumeSourceIdentityChanged: Qt.callLater(attemptResume)

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

    onClosing: {
        // C1: the debounce can still be pending when the window goes away, and the last thing the
        // user did was move the playhead. Record synchronously so the position survives the close.
        resumeRecordTimer.stop();
        root.recordResumePosition();
    }

    // Fade was removed from the video compare modes; fall back to Wipe for persisted preferences

    // that still point at it, so the UI never lands on an unselectable mode.

    Component.onCompleted: {

        // qmllint disable unqualified

        if (preferences && Number(preferences.viewMode) === ComparisonSurface.Fade)
            preferences.viewMode = ComparisonSurface.Wipe;

        // qmllint enable unqualified

    }

    function fileName(fileUrl) {
        return MediaLabels.fileName(MediaLabels.localPathFromUrl(fileUrl));
    }

    // Parent-directory label used when same-named files from different folders must stay distinct.

    function sourcePathLabel(fileUrl) {
        return MediaLabels.parentFolderLabel(MediaLabels.localPathFromUrl(fileUrl));
    }

    function setInPoint() {
        if (currentFrame >= 0 && shell) {
            revealOsc();

            const frame = Number(currentFrame);

            const mediaTime = controller ? Number(controller.mediaTimeForFrame(frame)) : -1;

            if (shell.setRangeIn(frame, mediaTime)) {
                syncApplicationRange();

                showImmersiveHud(qsTr("入点 · 第 %1 帧").arg(frame + 1));
            }
        }
    }

    function setOutPoint() {
        if (currentFrame >= 0 && shell) {
            revealOsc();

            const frame = Number(currentFrame);

            const mediaTime = controller ? Number(controller.mediaTimeForFrame(frame)) : -1;

            if (shell.setRangeOut(frame, mediaTime)) {
                syncApplicationRange();

                showImmersiveHud(qsTr("出点 · 第 %1 帧").arg(frame + 1));
            }
        }
    }

    function syncApplicationRange() {
        if (!controller)
            return;

        const nextIn = shell ? Number(shell.inFrame) : -1;
        const nextOut = shell ? Number(shell.outFrame) : -1;
        const loop = Boolean(controller.playbackRangeLoop);

        controller.setPlaybackRange(nextIn, nextOut, loop && nextIn >= 0 && nextOut >= nextIn);
    }

    function playSelectedRange() {
        if (inFrame < 0 || outFrame < inFrame || !controller)
            return false;

        if (shell) {
            shell.setRangePlaybackState(true, Number(currentFrame) !== inFrame);
            shell.setRangeStartPending(false);
        }

        if (!controller.playRange(inFrame, outFrame)) {
            stopRangeLoop(qsTr("无法启动范围循环播放。"));

            return false;
        }

        return true;
    }

    function clearSelectedRange() {
        if (shell)
            shell.clearRange();

        if (controller)
            controller.setPlaybackRange(-1, -1, false);
    }

    // Opens the export dialog. Returns false (with a status line) when there is nothing to export,
    // so the chip can never lead to a dead-end dialog.
    function openClipExportDialog() {
        if (!clipExportService)
            return false;

        if (!rangeExportEnabled) {
            showImmersiveHud(qsTr("当前视频或所选区间暂不可导出，请检查素材和入点、出点。"));

            return false;
        }

        clipExportDialogLoader.active = true;

        if (!clipExportDialog)
            return false;

        clipExportDialog.open();

        return true;
    }

    function remapReviewRange() {
        if (!controller || !shell || sourceCount === 0)
            return;

        const mappedIn = inMediaTime >= 0 ? Number(controller.frameForMediaTime(inMediaTime)) : -1;

        const mappedOut = outMediaTime >= 0 ? Number(controller.frameForMediaTime(outMediaTime)) : -1;

        shell.remapRange(mappedIn, mappedOut);
        syncApplicationRange();
    }

    function revealOsc() {
        if (!chromeVisible) {
            revealImmersiveOsc();
        } else {
            transport.reveal();
        }
    }

    function revealImmersiveOsc() {
        if (!imageWorkspaceActive && !chromeVisible && sourceCount > 0) {
            immersiveOscRevealed = true;
            immersiveOscTimer.restart();
            transport.reveal();
        }
    }

    Timer {
        id: immersiveOscTimer

        interval: 2500
        repeat: false
        onTriggered: {
            // Immersive chrome hides on a timer too, and that timer has to respect the cursor the
            // same way the OSC's own countdown does: the transport is fully usable under the
            // pointer (dragging the timeline, changing the rate), so hiding it there reads as a
            // break. Re-check instead; leaving the controls re-arms this immediately through the
            // OSC's own hover-leave path.
            if (transport.pointerInsidePanel) {
                immersiveOscTimer.restart();
                return;
            }
            root.immersiveOscRevealed = false;
        }
    }

    function toggleRangeLoop() {
        if (inFrame < 0 || outFrame < inFrame || !controller)
            return false;

        const nextActive = !rangePlaybackActive;

        if (shell)
            shell.setRangePlaybackState(nextActive, false);

        if (nextActive) {
            if (!controller.playRange(inFrame, outFrame)) {
                showIntentMessage(qsTr("无法启动范围循环播放。"));

                return false;
            }
        } else if (!controller.stopRangeLoop()) {
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

        const paused = !controller || controller.stopRangeLoop();

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

    // Focus follows the active workspace so the visible focus ring and the key receiving

    // surface can never point at a hidden task.

    function focusActiveWorkspace() {
        Qt.callLater(() => {
            if (workspaceSession.imageActive) {
                if (imageWorkspace)
                    imageWorkspace.forceActiveFocus();
            } else if (viewportFrame) {
                viewportFrame.forceActiveFocus();
            }
        });
    }

    function returnFocusToViewer() {
        focusActiveWorkspace();
    }

    // Keyboard flavour of hold-to-peek: tap toggles the raw reference view, Shift+backtick
    // locks it. The mouse button keeps its transient press/release semantics; its release
    // never defeats an active lock (see onDifferencePeekChanged). The locked state also stays
    // on screen through the viewport's peek indicator, not only as a transient HUD line.
    function toggleDifferencePeek(lock) {
        if (!root.differencePeekTargetValid) {
            root.showImmersiveHud(qsTr("当前会话没有可直看的参考（GT）源"));
            return;
        }
        if (lock) {
            differencePeekLocked = !differencePeekLocked;
            differencePeekActive = differencePeekLocked;
            showImmersiveHud(differencePeekLocked ? qsTr("已锁定参考原图直看") : qsTr("已解锁参考原图"));
        } else if (differencePeekLocked) {
            differencePeekLocked = false;
            differencePeekActive = false;
            showImmersiveHud(qsTr("已解锁，恢复差异视图"));
        } else {
            differencePeekActive = !differencePeekActive;
            showImmersiveHud(differencePeekActive ? qsTr("查看参考原图") : qsTr("恢复差异视图"));
        }
    }

    function showIntentMessage(message) {
        intentMessage = String(message);

        intentMessageTimer.restart();
    }

    function recordResumePosition() {
        if (!preferences || resumeSourceIdentity.length === 0 || currentFrame < 0)
            return;
        // Frame 0 is the position a fresh open already lands on; storing it would grow the settings
        // document with entries that restore nothing.
        preferences.rememberResumeFrame(resumeSourceIdentity, currentFrame);
    }

    function frameTextFor(frame) {
        const value = Number(frame);
        return value >= 0 ? qsTr("第 %1 帧").arg(value + 1) : qsTr("未知位置");
    }

    function attemptResume() {
        if (!controller || !preferences)
            return;
        if (performanceAutomation)
            return;
        const identity = resumeSourceIdentity;
        if (identity.length === 0)
            return;
        resumeAttemptedIdentity = identity;
        if (sourceCount <= 0 || currentFrame !== 0)
            return;
        const stored = Number(preferences.resumeFrameFor(identity));
        if (!(stored > 0))
            return;
        // The stored frame is only meaningful inside this timeline; a stale entry from a shorter
        // version of the same file must not push the playhead past the end.
        if (totalFrames > 0 && stored >= totalFrames)
            return;
        if (!controller.seekFrame(stored))
            return;
        showIntentMessage(qsTr("已恢复到上次位置 %1").arg(frameTextFor(stored)));
    }

    function enqueueStartupRequest(kind, urls) {
        return shell && shell.enqueueStartupRequest(Number(kind), Array.from(urls));
    }

    // Called before leaving video and again when video becomes active, so switching away and

    // returning always restores an explicit paused state instead of a hidden playback clock.

    function ensureVideoPaused() {
        if (!controller || !controller.playing)
            return true;

        return Boolean(controller.pause());
    }

    function beginWorkspaceOpen(media) {
        workspaceSession.beginOpen(media);

        return true;
    }

    // T4: cancelling a pending selector/open must invalidate the candidate request, not

    // merely hide the dialog. The already-committed canvas is untouched.

    function cancelWorkspaceOpen() {
        if (root.pendingImageRequestId > 0 && root.stillImageController) {
            root.stillImageController.cancelOpenRequest(root.pendingImageRequestId);

            root.pendingImageRequestId = -1;

            root.pendingImageOpenKind = "";
        }

        if (root.pendingFolderPairRow >= 0 && root.folderPairModel) {
            root.folderPairModel.cancelPendingOpen();

            root.pendingFolderPairRow = -1;
        }

        if (root.videoFolderModel)
            root.videoFolderModel.cancelPendingImageOpen();

        workspaceSession.cancelOpen();

        focusActiveWorkspace();

        return true;
    }

    // T4 completion handler for direct image opens. The loader reports exactly one terminal

    // for each accepted request; stale ids are ignored so N cannot override N+2.

    function completeImageOpen(requestId, pairId, success, error) {
        if (Number(requestId) !== Number(root.pendingImageRequestId))
            return;

        const kind = root.pendingImageOpenKind;

        const target = root.stillImageController;

        root.pendingImageRequestId = -1;

        root.pendingImageOpenKind = "";

        if (kind === "replaceA" || kind === "replaceB") {
            if (!success) {
                const replaceDetail = String(error || "").length > 0 ? String(error) : qsTr("无法打开图片。");
                root.dropError = replaceDetail;
                root.showIntentMessage(replaceDetail);
                return;
            }
            // Slot replace keeps the current workspace and folder session; only the
            // canvas side content changes (T1 single-side semantics).
            root.dropError = "";
            return;
        }

        if (!success) {
            const detail = String(error || "").length > 0 ? String(error) : qsTr("无法打开图片。");

            root.dropError = detail;

            root.showIntentMessage(detail);

            if (root.videoFolderModel)
                root.videoFolderModel.cancelPendingImageOpen();

            workspaceSession.cancelOpen();

            return;
        }

        if (!target)
            return;

        root.detachFolderSession();

        const identity = kind === "single" ? target.primaryPath : target.primaryPath + "\n" + target.secondaryPath;

        root.commitWorkspace(workspaceSession.imageMedia, identity);

        root.dropError = "";

        // A committed image task joins the shared recent-media list; a sidebar-staged open
        // finishes its pending row here (its file must match, so dialog opens stay no-ops).
        if (root.preferences) {
            root.preferences.rememberMediaPath(target.primaryPath);
            if (kind !== "single" && target.secondaryPath && target.secondaryPath.length > 0)
                root.preferences.rememberMediaPath(target.secondaryPath);
        }
        if (root.videoFolderModel)
            root.videoFolderModel.finishPendingImageOpen(true, "", target.primaryPath);
    }

    // Folder rows finish through ImageFolderPairModel. Commit the folder workspace only

    // after the first candidate actually commits, preserving the previous task on failure.

    function completeFolderPairOpen(row, success, error) {
        if (Number(row) !== Number(root.pendingFolderPairRow))
            return;

        root.pendingFolderPairRow = -1;

        if (!success) {
            const detail = String(error || "").length > 0 ? String(error) : qsTr("无法打开图片对。");

            root.dropError = detail;

            root.showIntentMessage(detail);

            workspaceSession.cancelOpen();

            return;
        }

        const pairModel = root.folderPairModel;

        if (!pairModel)
            return;

        root.dropError = "";

        root.commitWorkspace(workspaceSession.imageMedia, pairModel.leftFolderPath + "\n" + pairModel.rightFolderPath);
    }

    // Pure view switch used after a commit or a close; it owns no media command by itself so

    // an asynchronous open/close cannot be disturbed by a redundant pause command.

    function showWorkspace(media) {
        const target = Number(media) === workspaceSession.imageMedia ? workspaceSession.imageMedia : workspaceSession.videoMedia;

        workspaceSession.pendingMedia = -1;

        root.workspaceMode = target;

        focusActiveWorkspace();

        return true;
    }

    // Explicit workspace switch from the View menu. Leaving video or returning to it always

    // lands on an explicit paused state; returning never resumes playback implicitly.

    function activateWorkspace(media) {
        ensureVideoPaused();

        if (root.issueLogModel) {
            const modeName = Number(media) === workspaceSession.imageMedia ? "image" : "video";

            root.issueLogModel.setWorkspaceMode(modeName);
        }

        return showWorkspace(media);
    }

    // Selector/drop intents call this only after the candidate actually committed. Merely

    // opening (or cancelling) a selector must not replace the committed workspace.

    function commitWorkspace(media, identity) {
        const target = Number(media) === workspaceSession.imageMedia ? workspaceSession.imageMedia : workspaceSession.videoMedia;

        workspaceSession.markCommitted(target, identity);

        if (target === workspaceSession.imageMedia)
            ensureVideoPaused();

        return showWorkspace(target);
    }

    // Ctrl+W / File-menu close. Only the active task is affected; a retained task in the other

    // workspace stays alive and becomes visible if it has committed content.

    function closeCurrentTask() {
        if (workspaceSession.imageActive) {
            const imageTarget = root.stillImageController;

            if (!imageTarget || (!root.imageHasContent && root.pendingImageRequestId <= 0))
                return false;

            root.pendingImageRequestId = -1;

            root.pendingImageOpenKind = "";

            root.pendingFolderPairRow = -1;

            if (imageTarget)
                imageTarget.closeAll();

            detachFolderSession();

            workspaceSession.clearCommitted(workspaceSession.imageMedia);

            showIntentMessage(qsTr("已关闭图片任务。"));

            if (root.videoHasSession)
                showWorkspace(workspaceSession.videoMedia);
            else
                focusActiveWorkspace();

            return true;
        }

        if (!root.videoHasSession || !shell)
            return false;

        // ReviewShellController owns the source-topology intent, but a projector/sync race can

        // leave its cached Active Sources empty while the media truth already has sources.

        // Fall back to the controller in that specific case so Ctrl+W cannot leave the visible

        // video task alive; normal non-empty shell state continues through the intent queue.

        const shellHadSources = Boolean(shell.activeSources && shell.activeSources.length > 0);

        if (!shell.closeSources())
            return false;

        if (!shellHadSources && root.videoHasSession && controller && Number(controller.sourceCount) > 0)
            controller.closeSources();

        workspaceSession.clearCommitted(workspaceSession.videoMedia);

        showIntentMessage(qsTr("正在关闭视频…"));

        if (root.imageHasContent)
            showWorkspace(workspaceSession.imageMedia);
        else
            focusActiveWorkspace();

        return true;
    }

    // Loose images are not part of the folder-pair task; clear its model so arrow keys (or a

    // stale list selection) cannot navigate the previous folder session after a direct import.

    // A detached session can have zero rows but still hold folder paths/error state, so clear

    // those too instead of only checking pairCount.

    function detachFolderSession() {
        if (!root.folderPairModel)
            return;

        const hasFolderSession = root.folderPairModel.pairCount > 0 || root.folderPairModel.leftFolderPath.length > 0 || root.folderPairModel.rightFolderPath.length > 0 || root.folderPairModel.errorText.length > 0;

        if (hasFolderSession)
            root.folderPairModel.clear();
    }

    // Instantiates the input-dialog cluster on first use (synchronous Loader activation) and
    // returns it; every open path funnels through here so the lazy dialog stays an
    // implementation detail of the host.
    function ensureReviewInputDialogs() {
        reviewInputDialogsLoader.active = true;

        return reviewInputDialogs;
    }

    function requestOpenVideoFolder() {
        if (!videoFolderModel)
            return false;
        ensureReviewInputDialogs().openVideoFolder();
        return true;
    }

    function loadVideoFolder(folder) {
        if (!videoFolderModel)
            return false;
        videoFolderSidebarVisible = true;
        return videoFolderModel.loadFolder(folder);
    }

    function openFolderVideo(row) {
        if (!videoFolderModel || videoFolderModel.scanning || row < 0 || row >= videoFolderModel.fileCount)
            return false;
        const url = videoFolderModel.urlForRow(Number(row));
        const isImage = videoFolderModel.isImageUrl(url);
        cancelWorkspaceOpen();
        workspaceSession.beginOpen(isImage ? workspaceSession.imageMedia : workspaceSession.videoMedia);
        if (isImage)
            return openSidebarImage(url);
        if (!videoFolderModel.openAt(Number(row))) {
            workspaceSession.cancelOpen();
            return false;
        }
        pendingNewReviewWantsThreeUp = false;
        return true;
    }

    // Shared sidebar/recent image path: the model stages the pending row (no shell intent),
    // the image workspace drives the decode, and completeImageOpen is the terminal that
    // records the recent entry and finishes the model's pending state.
    function openSidebarImage(url) {
        if (!videoFolderModel.openFile(url)) {
            workspaceSession.cancelOpen();
            return false;
        }
        if (!performImageReview([url])) {
            videoFolderModel.cancelPendingImageOpen();
            workspaceSession.cancelOpen();
            return false;
        }
        return true;
    }

    function openRecentVideo(row) {
        if (!videoFolderModel || row < 0 || row >= videoFolderModel.recentFiles.length)
            return false;
        if (videoFolderModel.isRecentImage(Number(row))) {
            const entry = videoFolderModel.recentFiles[row];
            cancelWorkspaceOpen();
            workspaceSession.beginOpen(workspaceSession.imageMedia);
            return openSidebarImage(entry.fileUrl);
        }
        cancelWorkspaceOpen();
        workspaceSession.beginOpen(workspaceSession.videoMedia);
        if (!videoFolderModel.openRecent(Number(row))) {
            workspaceSession.cancelOpen();
            return false;
        }
        pendingNewReviewWantsThreeUp = false;
        return true;
    }

    function sidebarVideoActionState(url) {
        let openReason = "";
        // Still images open in the image workspace; they never join a video comparison.
        if (videoFolderModel && videoFolderModel.isImageUrl(url)) {
            if (!stillImageController)
                openReason = qsTr("图片打开服务暂不可用");
            else if (busy || videoFolderModel.openPending || (reviewInputDialogs && reviewInputDialogs.modalVisible))
                openReason = qsTr("请等待当前打开操作完成");
            return {
                openReason: openReason,
                compareReason: openReason.length > 0 ? openReason : qsTr("图片请在图片工作区对比")
            };
        }
        if (!shell || !controller || !videoFolderModel || !graphicsReady)
            openReason = qsTr("视频打开服务暂不可用");
        else if (busy || videoFolderModel.openPending || shell.queuedIntentCount > 0 || Object.keys(shell.activeIntent).length > 0 || (reviewInputDialogs && reviewInputDialogs.modalVisible))
            openReason = qsTr("请等待当前打开操作完成");
        else {
            const checked = controller.handleDroppedUrls([url]);
            if (!checked.accepted || checked.kind !== "videos")
                openReason = qsTr("视频已移动、删除或不可读取");
        }
        let compareReason = openReason;
        if (compareReason.length === 0) {
            const existing = activeSourceUrls();
            if (imageWorkspaceActive)
                compareReason = qsTr("请先切回视频工作区");
            else if (existing.length !== sourceCount)
                compareReason = qsTr("当前视频会话尚未就绪");
            else
                compareReason = shell.sidebarAppendError(url);
        }
        return {
            openReason: openReason,
            compareReason: compareReason
        };
    }

    function openSidebarVideo(url) {
        const reason = sidebarVideoActionState(url).openReason;
        if (reason.length > 0) {
            showIntentMessage(reason);
            return false;
        }
        if (videoFolderModel && videoFolderModel.isImageUrl(url)) {
            cancelWorkspaceOpen();
            workspaceSession.beginOpen(workspaceSession.imageMedia);
            return openSidebarImage(url);
        }
        cancelWorkspaceOpen();
        workspaceSession.beginOpen(workspaceSession.videoMedia);
        if (!videoFolderModel.openFile(url)) {
            workspaceSession.cancelOpen();
            return false;
        }
        pendingNewReviewWantsThreeUp = false;
        return true;
    }

    function cancelSidebarComparison() {
        if (!pendingSidebarComparison)
            return;
        pendingSidebarComparison = false;
        if (shell && shell.sidebarAppendStaged)
            shell.clearStagedSources();
        pendingComparisonPreservesPosition = false;
        pendingNewReviewWantsThreeUp = false;
        cancelWorkspaceOpen();
    }

    function compareSidebarVideo(url) {
        const reason = sidebarVideoActionState(url).compareReason;
        if (reason.length > 0) {
            showIntentMessage(reason);
            return false;
        }
        return reviewUrls([url], true, true);
    }

    function stepFolderVideo(delta) {
        if (!videoFolderModel || delta === 0)
            return false;
        const anchor = videoFolderModel.openPending ? videoFolderModel.pendingRow : videoFolderModel.currentRow;
        const row = anchor < 0 ? (delta > 0 ? 0 : videoFolderModel.fileCount - 1) : anchor + (delta > 0 ? 1 : -1);
        return openFolderVideo(row);
    }

    function requestOpenVideos() {
        workspaceSession.beginOpen(workspaceSession.videoMedia);

        ensureReviewInputDialogs().openVideos();

        return true;
    }

    function requestAddVideo() {
        workspaceSession.beginOpen(workspaceSession.videoMedia);

        ensureReviewInputDialogs().openAddVideo();

        return true;
    }

    function requestImageOpen() {
        workspaceSession.beginOpen(workspaceSession.imageMedia);

        imageSingleDialog.open();

        return true;
    }

    function requestImagePairOpen() {
        workspaceSession.beginOpen(workspaceSession.imageMedia);

        imagePairDialog.open();

        return true;
    }

    function requestImageAdd() {
        workspaceSession.beginOpen(workspaceSession.imageMedia);

        imageAddDialog.open();

        return true;
    }

    function requestImageSwapSides() {
        const target = root.stillImageController;
        if (!target || !target.hasPair)
            return false;
        if (!target.swapSides()) {
            const detail = target.errorText.length > 0 ? String(target.errorText) : qsTr("无法对调 A/B。");
            dropError = detail;
            showIntentMessage(detail);
            return false;
        }
        dropError = "";
        return true;
    }

    function requestImageReplacePrimary() {
        const target = root.stillImageController;
        if (!target || !target.hasPrimary)
            return false;
        // In-place slot replace: no workspace beginOpen — failure must not tear down
        // the already-committed image workspace.
        imageReplacePrimaryDialog.open();
        return true;
    }

    function requestImageReplaceSecondary() {
        const target = root.stillImageController;
        if (!target || !target.hasPrimary)
            return false;
        imageReplaceSecondaryDialog.open();
        return true;
    }

    function performImageSideReplace(url, side) {
        const target = root.stillImageController;
        if (!target || !target.hasPrimary || !url || url.toString().length === 0)
            return false;
        if (root.pendingImageRequestId > 0)
            target.cancelOpenRequest(root.pendingImageRequestId);
        let requestId = -1;
        let kind = "";
        if (side === "a") {
            requestId = Number(target.requestReplacePrimary(url));
            kind = "replaceA";
        } else {
            requestId = Number(target.requestReplaceSecondary(url));
            kind = "replaceB";
        }
        if (!(requestId > 0)) {
            const detail = target.errorText.length > 0 ? String(target.errorText) : qsTr("无法打开图片。");
            dropError = detail;
            showIntentMessage(detail);
            return false;
        }
        root.pendingImageRequestId = requestId;
        root.pendingImageOpenKind = kind;
        dropError = "";
        return true;
    }

    function requestCompareFolders() {
        workspaceSession.beginOpen(workspaceSession.imageMedia);

        root.imageFolderStage = 0;

        imageFolderLeftDialog.open();

        return true;
    }

    function performVideoReview(normalizedUrls) {
        if (!normalizedUrls || normalizedUrls.length === 0)
            return false;

        if (normalizedUrls.length === 1) {
            dropError = "";

            if (!setDroppedVideoOrder(normalizedUrls) || !shell || !shell.openStagedSources(false)) {
                workspaceSession.cancelOpen();

                showIntentMessage(qsTr("无法打开该视频。"));

                return false;
            }

            commitWorkspace(workspaceSession.videoMedia, normalizedUrls[0].toString());

            return true;
        }

        dropError = "";

        if (!setDroppedVideoOrder(normalizedUrls)) {
            workspaceSession.cancelOpen();

            showIntentMessage(qsTr("无法暂存这些视频。"));

            return false;
        }

        pendingComparisonPreservesPosition = false;

        workspaceSession.beginOpen(workspaceSession.videoMedia);

        ensureReviewInputDialogs().openComparison();

        return true;
    }

    // Loads the two staged folder URLs into the pair model and opens the first comparable

    // pair. The workspace commits only after the folder scan succeeds; an explicit valid

    // folder choice still switches to the image workspace when no complete pair exists.

    function captureIssueLog() {
        // M freezes the observation the moment it is pressed: the record (frame, sources,
        // ROI, zoom, pair) is captured now, so playback advancing while the note is typed can
        // no longer shift what was recorded. The dialog only attaches text to that record.
        if (!root.issueLogModel)
            return false;

        root.issueLogModel.setWorkspaceMode(root.imageWorkspaceActive ? "image" : "video");

        const ok = root.issueLogModel.captureCurrentIssue("");

        if (!ok)
            return false;

        root.pendingIssueNoteIndex = root.issueLogModel.count - 1;
        root.issueLogPanelVisible = true;
        issueNoteDialog.open();
        return true;
    }

    function captureIssueLogWithNote(note) {
        if (!root.issueLogModel || root.pendingIssueNoteIndex < 0)
            return false;

        const ok = root.issueLogModel.attachNote(root.pendingIssueNoteIndex, String(note));

        if (ok)
            root.pendingIssueNoteIndex = -1;

        return ok;
    }

    // Step-2 comparison export: the caption names the compared sources (GT and
    // predictions), the observation context and a timestamp, so the pasted image stands
    // alone in a report. The capture itself is the presented display result, cropped to
    // the comparison viewport — never original code values.
    function comparisonCaptionLines() {
        const lines = [];
        const names = [sourceAName, sourceBName, sourceCName];
        if (sourceCount === 3 && referenceSourceIndex >= 0 && referenceSourceIndex < 3 && names[referenceSourceIndex] && names[referenceSourceIndex].length > 0) {
            const others = [];
            for (let index = 0; index < 3; ++index) {
                if (index !== referenceSourceIndex && names[index] && names[index].length > 0)
                    others.push(names[index]);
            }
            lines.push(qsTr("GT：%1 · 预测：%2").arg(names[referenceSourceIndex]).arg(others.join(" / ")));
        } else {
            const present = [];
            for (let index = 0; index < sourceCount; ++index) {
                if (names[index] && names[index].length > 0)
                    present.push(names[index]);
            }
            if (present.length > 0)
                lines.push(present.join(" · "));
        }
        const modeNames = {
            0: qsTr("并排"),
            1: qsTr("三联"),
            2: qsTr("参考聚焦"),
            3: qsTr("差异"),
            4: qsTr("分析网格"),
            5: qsTr("分割线"),
            6: qsTr("单画面"),
            7: qsTr("淡化")
        };
        let context = modeNames[effectiveViewMode] || "";
        if (currentFrame >= 0)
            context += qsTr(" · 第 %1 帧").arg(currentFrame + 1);
        if (sourceCount === 3 && selectedDifferenceEdge)
            context += " · " + String(selectedDifferenceEdge.label || "");
        if (differenceThresholdEnabled)
            context += qsTr(" · 阈值 %1").arg(differenceThresholdCode);
        if (context.length > 0)
            lines.push(context);
        lines.push(qsTr("CompareStation · %1").arg(Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm")));
        return lines;
    }

    function copyComparisonImage() {
        if (!comparisonExportService) {
            root.showImmersiveHud(qsTr("复制对比图不可用"));
            return false;
        }
        const ok = Boolean(comparisonExportService.copyComparison(viewportFrame, comparisonCaptionLines()));
        root.showImmersiveHud(ok ? qsTr("已复制对比图（含标注）") : qsTr("复制失败：%1").arg(comparisonExportService.lastStatus));
        return ok;
    }

    function applyIssueRestore(payload) {
        if (!payload)
            return false;

        if (String(payload.decision) !== "ready")
            return false;

        root.cancelPendingIssueRestore();
        if (String(payload.kind) === "video") {
            const urls = payload.urls || [];
            let openCommandId = 0;

            if (urls.length > 0 && root.controller) {
                // The open command takes the reference (GT) slot, which is not the timeline's
                // canonical source. Records written before the distinction was captured carry
                // only the canonical index; fall back to it for them.
                const recorded = Number(payload.referenceSourceIndex);
                const referenceIndex = Number.isFinite(recorded) && recorded >= 0 ? recorded : (Number(payload.canonicalSourceIndex) || 0);
                const opened = root.controller.openSources(urls, referenceIndex);

                if (!opened) {
                    root.showIntentMessage(qsTr("无法按问题记录打开视频来源。"));

                    return false;
                }

                openCommandId = root.controller.lastSubmittedCommandId;
                workspaceSession.beginOpen(workspaceSession.videoMedia);

                root.commitWorkspace(workspaceSession.videoMedia, urls[0].toString());
            }

            // Everything else is staged: view context once the session settles, the frame
            // once it is presented, and the viewport only then. Re-entering from
            // controller state changes drives the machine.
            root.pendingIssueRestore = payload;
            root.pendingIssueRestoreStage = urls.length > 0 ? 1 : 2;
            root.pendingIssueRestoreContextApplied = false;
            root.pendingIssueRestorePairApplied = true;
            root.pendingIssueRestoreOpenCommandId = openCommandId;
            root.pendingIssueRestoreOpenSessionEpoch = root.controller ? root.controller.sessionEpoch : 0;
            root.pendingIssueRestoreSessionEpoch = urls.length === 0 && root.controller ? root.controller.sessionEpoch : 0;
            issueRestoreDeadline.restart();
            root.continueIssueRestore();

            return true;
        }

        // Image-pair restore: folders + row identity only after evaluation said Ready. The
        // recorded image view state (compare mode, zoom, pan, row) is not replayed yet, so the
        // message says what was actually restored instead of implying the same observation.
        const imageViewRecorded = payload.zoom !== undefined || payload.panX !== undefined || payload.compareMode !== undefined;

        if (payload.leftFolder && payload.rightFolder) {
            root.imageFolderLeftUrl = "file:///" + String(payload.leftFolder).replace(/\\/g, "/");

            root.imageFolderRightUrl = "file:///" + String(payload.rightFolder).replace(/\\/g, "/");

            root.loadFolderComparison();
        } else if (payload.leftPath && payload.rightPath && root.stillImageController) {
            const leftUrl = "file:///" + String(payload.leftPath).replace(/\\/g, "/");

            const rightUrl = "file:///" + String(payload.rightPath).replace(/\\/g, "/");

            root.performImageReview([leftUrl, rightUrl]);
        }

        if (imageViewRecorded)
            root.showIntentMessage(qsTr("部分恢复：正在打开记录中的图片，观察模式、缩放和平移未恢复"));

        return true;
    }

    // Staged continuation of an issue restore. Re-enters from controller state/frame changes;
    // returns without doing anything while the wait conditions still hold, so a still-opening
    // session simply picks the restore up on the next state change. Every stage is bound to the
    // recorded sources: a session that is no longer the recorded one cancels the restore instead
    // of replaying the recorded view onto whatever is open now.
    function continueIssueRestore() {
        if (root.issueRestoreContinuing)
            return;
        root.issueRestoreContinuing = true;
        try {
            root.advanceIssueRestore();
        } finally {
            root.issueRestoreContinuing = false;
        }
    }

    function advanceIssueRestore() {
        const payload = root.pendingIssueRestore;

        if (!payload)
            return;

        if (!root.controller || root.imageWorkspaceActive) {
            root.abandonPendingIssueRestore(qsTr("恢复已取消：工作区已切换"));
            return;
        }

        const urls = payload.urls || [];

        if (root.pendingIssueRestoreStage === 1) {
            if (root.pendingIssueRestoreOpenOutcome < 0)
                return;
            if (root.pendingIssueRestoreOpenOutcome !== 0 || root.sourceCount === 0) {
                root.abandonPendingIssueRestore(qsTr("恢复失败：无法打开记录中的视频来源"));
                return;
            }
            if (!root.matchesOpenUrls(urls)) {
                root.abandonPendingIssueRestore(qsTr("恢复已取消：打开的素材与记录不一致"));
                return;
            }
            root.pendingIssueRestoreSessionEpoch = root.controller.sessionEpoch;
            root.pendingIssueRestoreStage = 2;
        } else if (root.controller.sessionEpoch !== root.pendingIssueRestoreSessionEpoch || (urls.length > 0 && !root.matchesOpenUrls(urls))) {
            // Stage 2 onwards: the recorded session was replaced while the restore waited for
            // its frame, so the recorded view must not be replayed onto the new one.
            root.abandonPendingIssueRestore(qsTr("恢复已取消：打开的素材与记录不一致"));
            return;
        }

        // View context exactly once - this machine re-enters on every state change while the
        // frame wait below holds, and applyDifferenceEdge is a command, not an idempotent
        // assignment. The recorded pair goes first because applying it can carry the mode
        // along; the recorded mode lands last so it is the one that survives (D07: an
        // explicit command after open, not a preference replay on every later session).
        // A refused pair command no longer counts as applied: it is reported at the end as a
        // partial restore instead of being silently swallowed, and it never blocks the mode,
        // frame and viewport replay, which are independent of it.
        if (!root.pendingIssueRestoreContextApplied) {
            root.pendingIssueRestoreContextApplied = true;
            if (payload.differenceEdge !== undefined) {
                root.pendingIssueRestorePairApplied = false;
                if (root.applyDifferenceEdge(Number(payload.differenceEdge)))
                    root.pendingIssueRestorePairCommandId = root.controller.lastSubmittedCommandId;
            }
            // The QML property is `viewMode`; assigning `viewModeCode` (the C++ accessor)
            // silently does nothing and drops the recorded mode.
            if (payload.viewMode !== undefined && root.preferences)
                root.preferences.viewMode = Number(payload.viewMode);
        }

        if (root.pendingIssueRestorePairCommandId !== 0)
            return;

        // Stage 2: seek unless the recorded frame is already the presented one, then wait for
        // that frame before touching the viewport.
        const target = payload.frame !== undefined ? Number(payload.frame) : -1;
        if (target >= 0 && (!Number.isInteger(target) || target >= root.totalFrames)) {
            root.abandonPendingIssueRestore(qsTr("恢复失败：记录中的帧超出素材范围"));
            return;
        }
        if (root.pendingIssueRestoreSeekCommandId !== 0)
            return;
        if (target >= 0 && root.currentFrame !== target) {
            if (root.busy || root.framePending)
                return;
            if (!root.controller.seekFrame(target)) {
                root.abandonPendingIssueRestore(qsTr("恢复失败：无法提交帧定位"));
                return;
            }
            root.pendingIssueRestoreSeekCommandId = root.controller.lastSubmittedCommandId;
            return;
        }
        if (root.framePending)
            return;

        if (payload.roiEnabled !== undefined && viewportFrame.surface)
            viewportFrame.surface.restoreViewport(Number(payload.centerX), Number(payload.centerY), Number(payload.zoom), Boolean(payload.roiEnabled), Number(payload.roiLeft), Number(payload.roiTop), Number(payload.roiRight), Number(payload.roiBottom));

        // A recorded pair the session would not take is a partial restore: the files, mode, frame
        // and viewport are back, but the comparison pair is not. Saying so is the difference
        // between "restored" and "looks restored".
        if (payload.differenceEdge !== undefined && !root.pendingIssueRestorePairApplied)
            root.showIntentMessage(qsTr("部分恢复：记录中的比较对未能应用"));

        root.cancelPendingIssueRestore();
    }

    function acceptIssueRestoreCommand(commandId, sessionEpoch, outcome) {
        if (!root.pendingIssueRestore)
            return;
        if (commandId === root.pendingIssueRestoreOpenCommandId) {
            if (sessionEpoch !== root.pendingIssueRestoreOpenSessionEpoch)
                return;
            root.pendingIssueRestoreOpenCommandId = 0;
            root.pendingIssueRestoreOpenOutcome = outcome;
        } else if (sessionEpoch !== root.pendingIssueRestoreSessionEpoch) {
            return;
        } else if (commandId === root.pendingIssueRestorePairCommandId) {
            root.pendingIssueRestorePairCommandId = 0;
            root.pendingIssueRestorePairApplied = outcome === 0 && root.differenceEdge === Number(root.pendingIssueRestore.differenceEdge);
        } else if (commandId === root.pendingIssueRestoreSeekCommandId) {
            root.pendingIssueRestoreSeekCommandId = 0;
            if (outcome !== 0 || root.currentFrame !== Number(root.pendingIssueRestore.frame)) {
                root.abandonPendingIssueRestore(qsTr("恢复失败：无法定位到记录中的帧"));
                return;
            }
        } else {
            return;
        }
        root.continueIssueRestore();
    }

    Timer {
        id: issueRestoreDeadline

        objectName: "issueRestoreDeadline"
        interval: 15000
        onTriggered: root.abandonPendingIssueRestore(qsTr("恢复超时：未能确认记录中的观察位置"))
    }

    // Ends a pending restore that cannot complete, and says so. A silent drop would leave the
    // user believing the recorded observation was restored.
    function abandonPendingIssueRestore(message) {
        root.cancelPendingIssueRestore();
        root.showIntentMessage(message);
    }

    function cancelPendingIssueRestore() {
        root.pendingIssueRestore = null;
        root.pendingIssueRestoreStage = 0;
        root.pendingIssueRestoreContextApplied = false;
        root.pendingIssueRestorePairApplied = true;
        root.pendingIssueRestoreOpenCommandId = 0;
        root.pendingIssueRestoreOpenSessionEpoch = 0;
        root.pendingIssueRestoreOpenOutcome = -1;
        root.pendingIssueRestoreSessionEpoch = 0;
        root.pendingIssueRestorePairCommandId = 0;
        root.pendingIssueRestoreSeekCommandId = 0;
        issueRestoreDeadline.stop();
    }

    // A restore only proceeds while the session still shows the recorded sources.
    function matchesOpenUrls(urls) {
        if (!root.controller || urls.length === 0)
            return false;
        const current = root.controller.sourceUrls || [];
        if (current.length !== urls.length)
            return false;
        for (let index = 0; index < urls.length; ++index) {
            if (String(current[index]) !== String(urls[index]))
                return false;
        }
        return true;
    }

    function loadFolderComparison() {
        const pairModel = root.folderPairModel;

        if (!pairModel)
            return false;

        const ok = pairModel.loadFolders(root.imageFolderLeftUrl, root.imageFolderRightUrl);

        if (!ok) {
            workspaceSession.cancelOpen();

            dropError = pairModel.errorText;

            return false;
        }

        const firstRow = pairModel.firstCompleteRow();

        root.imageFolderSidebarVisible = true;

        if (firstRow < 0) {

            // A valid folder choice with no same-named pair still switches to the image

            // workspace, with the sidebar showing the missing rows and the reason.

            commitWorkspace(workspaceSession.imageMedia, pairModel.leftFolderPath + "\n" + pairModel.rightFolderPath);

            dropError = qsTr("两个文件夹没有同名图片。");

            return false;
        }

        dropError = "";

        root.pendingFolderPairRow = firstRow;

        if (!pairModel.openPairAt(firstRow)) {

            // The controller rejected the candidate synchronously; surface the failure

            // source instead of advancing the list selection.

            root.pendingFolderPairRow = -1;

            dropError = pairModel.errorText;

            workspaceSession.cancelOpen();

            return false;
        }

        if (!pairModel.openPending) {

            // Synchronous opener (tests/embedding) already committed the first pair.

            root.pendingFolderPairRow = -1;

            commitWorkspace(workspaceSession.imageMedia, pairModel.leftFolderPath + "\n" + pairModel.rightFolderPath);
        }

        return true;
    }

    function performImageReview(normalizedUrls) {
        const target = root.stillImageController;

        if (!target)
            return false;
        if (!normalizedUrls || normalizedUrls.length < 1 || normalizedUrls.length > 2) {
            const detail = qsTr("一次只能打开一张或两张图片。");

            dropError = detail;
            showIntentMessage(detail);
            cancelWorkspaceOpen();
            return false;
        }

        // New candidate invalidates an older pending one; the visible canvas remains the

        // previous committed pair until the new candidate actually decodes (T4).

        if (root.pendingImageRequestId > 0)
            target.cancelOpenRequest(root.pendingImageRequestId);

        let requestId = -1;

        let kind = "";

        if (normalizedUrls.length === 1) {
            requestId = Number(target.requestOpenPrimary(normalizedUrls[0]));

            kind = "single";
        } else {
            requestId = Number(target.requestOpenPair(normalizedUrls[0], normalizedUrls[1], -1));

            kind = "pair";
        }

        if (!(requestId > 0)) {
            const detail = target.errorText.length > 0 ? String(target.errorText) : qsTr("无法打开图片。");

            dropError = detail;

            showIntentMessage(detail);

            workspaceSession.cancelOpen();

            return false;
        }

        root.pendingImageRequestId = requestId;

        root.pendingImageOpenKind = kind;

        dropError = "";

        return true;
    }
    function performImagePairSelection(urls) {
        const selected = urls ? Array.from(urls).filter(url => url && url.toString().length > 0) : [];

        if (selected.length !== 2) {
            const detail = qsTr("图片对需要恰好两张图片，请重新选择。");

            dropError = detail;
            showIntentMessage(detail);
            cancelWorkspaceOpen();
            return false;
        }

        return performImageReview(selected);
    }
    function reviewUrls(urls, allowSingleSourceAppend, sidebarAppend = false) {
        const reviewed = controller.handleDroppedUrls(urls);

        if (!reviewed.accepted) {
            workspaceSession.cancelOpen();

            dropError = root.messageCatalog.droppedUrlError(reviewed.errorKey, reviewed.detail);

            return false;
        }

        const normalizedUrls = reviewed.urls;

        if (reviewed.kind === "images") {
            const imageTarget = root.stillImageController;

            if (allowSingleSourceAppend && imageTarget && imageTarget.hasPrimary && !imageTarget.hasSecondary && normalizedUrls.length === 1) {
                const primary = imageTarget.primaryPath;

                const candidate = decodeURIComponent(normalizedUrls[0].toString()).replace(/^file:[\/]{3}/, "");

                if (primary.length > 0 && sameFilePath(primary, candidate)) {
                    dropError = qsTr("该图片已打开。");

                    return false;
                }

                dropError = "";

                // T4: append candidate also goes through the async controller. A failed B

                // keeps the current committed workspace and never commits a half-open task.

                const appendRequestId = Number(imageTarget.requestOpenSecondary(normalizedUrls[0]));

                if (!(appendRequestId > 0)) {
                    const detail = imageTarget.errorText.length > 0 ? String(imageTarget.errorText) : qsTr("无法打开图片。");

                    dropError = detail;

                    showIntentMessage(detail);

                    return false;
                }

                root.pendingImageRequestId = appendRequestId;

                root.pendingImageOpenKind = "append";

                return true;
            }

            requestDestructiveAction({
                "kind": "openImages",
                "urls": normalizedUrls,
                "external": false
            });

            return true;
        }

        if (normalizedUrls.length === 1) {
            const existing = activeSourceUrls();

            if (allowSingleSourceAppend && sourceCount > 0 && existing.length === sourceCount && existing.length < 3) {
                const candidate = normalizedUrls[0].toString();

                for (const current of existing) {
                    if (current.toString() === candidate) {
                        dropError = qsTr("该视频已打开。");

                        return false;
                    }
                }

                existing.push(normalizedUrls[0]);

                dropError = "";

                // The confirmation dialog is still a pending open: keep the current workspace

                // visible until the user accepts. Cancelling must preserve it untouched.

                const staged = sidebarAppend ? shell.stageSidebarAppend(normalizedUrls[0]) : setDroppedVideoOrder(existing);
                if (!staged) {
                    if (sidebarAppend)
                        showIntentMessage(qsTr("当前视频对比已改变，请重新选择要加入的视频。"));
                    return false;
                }

                pendingSidebarComparison = sidebarAppend;
                pendingComparisonPreservesPosition = true;

                workspaceSession.beginOpen(workspaceSession.videoMedia);

                ensureReviewInputDialogs().openComparison();

                return true;
            }

            if (sidebarAppend)
                return false;

            requestDestructiveAction({
                "kind": "openVideos",
                "urls": normalizedUrls,
                "external": false
            });

            return true;
        }

        requestDestructiveAction({
            "kind": "openVideos",
            "urls": normalizedUrls,
            "external": false
        });

        return true;
    }

    // Path comparison helper for drop-append guards: controller paths are plain local

    // paths while staged URLs are file:/// form; compare case-insensitively on Windows.

    function sameFilePath(left, right) {
        const normalize = path => String(path || "").replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();

        return normalize(left) === normalize(right);
    }

    // Directory detection for folder drops. The C++ drop validator requires regular

    // files, so folders are routed here before it runs. Returns the local paths of every

    // dropped entry that is a directory, or an empty array when none are.

    function droppedFolderPaths(urls) {
        const folders = [];

        for (const value of urls) {
            const url = typeof value === "string" ? Qt.resolvedUrl(value) : value;

            if (!root.controller || !root.controller.isFolderPath(url))
                continue;

            folders.push(url);
        }

        return folders;
    }

    function reviewDroppedUrls(urls) {
        const folderPaths = droppedFolderPaths(urls);

        if (folderPaths.length === 2) {
            imageFolderLeftUrl = folderPaths[0];

            imageFolderRightUrl = folderPaths[1];

            loadFolderComparison();

            return;
        }

        if (folderPaths.length === 1) {
            dropError = qsTr("拖入两个文件夹可进行图片对比。");

            return;
        }

        if (folderPaths.length > 2) {
            dropError = qsTr("最多拖入两个文件夹。");

            return;
        }

        reviewUrls(urls, true);
    }

    function openNewReviewUrls(urls) {
        reviewUrls(urls, false);
    }

    function openDroppedComparison(referenceIndex) {
        if (shell)
            shell.stagedReferenceIndex = referenceIndex;

        pendingNewReviewWantsThreeUp = Boolean(shell && shell.stagedSources.length === 3);

        const preservePosition = pendingComparisonPreservesPosition && sourceCount > 0;

        const opened = Boolean(shell && (pendingSidebarComparison ? shell.openSidebarAppend(referenceIndex) : shell.openStagedSources(preservePosition)));

        if (!opened) {
            pendingNewReviewWantsThreeUp = false;

            workspaceSession.cancelOpen();

            showIntentMessage(qsTr("无法打开暂存的视频源。"));

            focusActiveWorkspace();

            return false;
        }

        pendingSidebarComparison = false;

        pendingComparisonPreservesPosition = false;

        commitWorkspace(workspaceSession.videoMedia, "");

        return true;
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

    // D07: user selection sends SetActiveComparisonPairCommand. Preferences only store
    // DefaultPairPolicy; canvas/menus read the committed effective pair.
    function applyDifferenceEdge(edge) {
        if (!controller || !controller.applyComparisonPairFromEdge)
            return false;
        const value = Number(edge);
        const pairPolicy = preferences ? Number(preferences.defaultPairPolicy) : 2;
        return Boolean(controller.applyComparisonPairFromEdge(value, Number.isFinite(pairPolicy) ? pairPolicy : 2));
    }

    // Step-2 core interaction "固定 GT 切候选": flip the active pair to the other edge
    // anchored on the reference (GT) source. Only the pair selection changes — the current
    // frame, zoom/pan and the wipe split stay exactly where they are, so a defect located
    // once stays on screen while the candidate side swaps. Entering from a
    // prediction-vs-prediction pair keeps the currently displayed primary slot as the
    // candidate so nothing jumps.
    function switchCandidateEdge() {
        if (sourceCount !== 3)
            return false;
        const reference = referenceSourceIndex >= 0 ? referenceSourceIndex : 0;
        let current = null;
        for (let index = 0; index < differenceEdges.length; ++index) {
            if (Number(differenceEdges[index].preferenceValue) === differenceEdge)
                current = differenceEdges[index];
        }
        if (!current)
            return false;
        const primary = Number(current.primarySlot);
        const currentHasReference = primary === reference || Number(current.secondarySlot) === reference;
        for (let index = 0; index < differenceEdges.length; ++index) {
            const edge = differenceEdges[index];
            const first = Number(edge.primarySlot);
            const second = Number(edge.secondarySlot);
            if (first !== reference && second !== reference)
                continue;
            if (Number(edge.preferenceValue) === differenceEdge)
                continue;
            if (!currentHasReference && first !== primary && second !== primary)
                continue;
            return applyDifferenceEdge(Number(edge.preferenceValue));
        }
        return false;
    }

    function applyDefaultPairPolicy(policyCode) {
        if (!preferences)
            return false;
        const value = Number(policyCode);
        preferences.defaultPairPolicy = value;
        if (controller && controller.applyDefaultPairPolicy)
            return Boolean(controller.applyDefaultPairPolicy(value));
        return false;
    }

    function applyPlaybackPreferencesFromSettings() {
        if (!controller || !preferences)
            return;
        if (controller.setPlaybackContinuityPolicy) {
            const continuity = Number(preferences.playbackContinuityPolicy);
            if (Number.isFinite(continuity))
                controller.setPlaybackContinuityPolicy(continuity);
        }
        // D07: do not replay a legacy A/B/C edge ordinal after open. Only the default pair policy.
        if (controller.applyDefaultPairPolicy) {
            const pairPolicy = Number(preferences.defaultPairPolicy);
            if (Number.isFinite(pairPolicy))
                controller.applyDefaultPairPolicy(pairPolicy);
        }
    }

    property string playbackPrefsAppliedKey: ""

    function playbackPrefsSessionKey() {
        if (!controller || !controller.graphicsReady || Number(controller.sourceCount) <= 0)
            return "";
        if (Number(controller.displayState) !== 2) // ReviewDisplayState::Ready
            return "";
        const identities = shell && shell.activeSourceIdentities ? String(shell.activeSourceIdentities) : "";
        return identities.length > 0 ? identities : String(controller.sourceCount) + ":" + String(controller.canonicalSourceIndex);
    }

    Connections {
        target: root.controller

        function onStateChanged() {
            root.continueIssueRestore();
            const key = root.playbackPrefsSessionKey();
            if (key.length === 0 || key === root.playbackPrefsAppliedKey)
                return;
            root.playbackPrefsAppliedKey = key;
            root.applyPlaybackPreferencesFromSettings();
        }

        function onFrameStateChanged() {
            root.continueIssueRestore();
        }

        function onCommandFinished(commandId, sessionEpoch, outcome, errorKey) {
            root.acceptIssueRestoreCommand(commandId, sessionEpoch, outcome);
        }

        function onSnapshotRefreshed() {
            root.continueIssueRestore();
        }
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
        referenceSourceIndex: root.referenceSourceIndex
        effectiveDifferenceEdge: root.differenceEdge
        currentViewMode: root.effectiveViewMode
        inspectorOpen: root.inspectorOpen
        graphicsReady: root.graphicsReady
        currentFrame: root.currentFrame
        alignmentAnalysisRunning: root.alignmentAnalysisRunning
        chromeVisible: root.chromeVisible
        fullScreen: root.fullScreen
        shortcutPreset: root.shortcutPreset
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        playbackContinuityPolicy: {
            if (!controller)
                return 2;
            const fromPref = root.preferences ? Number(root.preferences.playbackContinuityPolicy) : 2;
            const fromSession = Number(controller.playbackContinuityPolicy);
            return Number.isFinite(fromSession) && controller.displayState === 2 ? fromSession : fromPref;
        }
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
        videoFolderAvailable: root.videoFolderModel !== null
        videoFolderVisible: root.videoFolderSidebarVisible
        videoHasSession: root.videoHasSession
        imageHasSession: root.imageHasContent
        onIssueLogCaptureRequested: root.captureIssueLog()
        onIssueLogToggleRequested: root.issueLogPanelVisible = !root.issueLogPanelVisible
        onIssueLogSaveRequested: issueSaveDialog.open()
        onIssueLogLoadRequested: issueLoadDialog.open()
        onOpenVideoFolderRequested: root.requestOpenVideoFolder()
        onVideoFolderToggleRequested: root.videoFolderSidebarVisible = !root.videoFolderSidebarVisible
        onOpenVideosRequested: root.requestOpenVideos()
        onAddVideoRequested: root.requestAddVideo()
        onOpenImageRequested: root.requestImageOpen()
        onAddImageRequested: root.requestImageAdd()
        onOpenImagePairRequested: root.requestImagePairOpen()
        onCompareImageFoldersRequested: root.requestCompareFolders()
        onWorkspaceRequested: mode => root.activateWorkspace(mode)
        onCloseCurrentRequested: root.closeCurrentTask()
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
        preferences: root.preferences
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
        candidateSwitchEnabled: root.candidateSwitchAvailable
        viewShortcutsEnabled: root.videoViewShortcutsEnabled
        sourceCount: root.sourceCount
        busy: root.busy
        differencePeekAvailable: root.differenceMode && root.differencePeekTargetValid
        onWipePositionRequested: position => {
            root.wipePosition = position;

            if (!root.chromeVisible)
                root.showImmersiveHud(qsTr("分割线位置 %1%").arg(Math.round(position * 100)));
        }
        onManualNavigationRequested: {
            // The user took the wheel: a restore still waiting for its frame stops here
            // instead of fighting the navigation.
            root.cancelPendingIssueRestore();
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
        onCandidateSwitchRequested: root.switchCandidateEdge()
        onModeRequested: mode => root.preferences.viewMode = mode
        onFitRequested: viewportFrame.fitToWindow()
        onNativeSizeRequested: viewportFrame.zoomToNativeSize()
        onZoomInRequested: viewportFrame.zoomViewStep(1.25)
        onZoomOutRequested: viewportFrame.zoomViewStep(0.8)
        onResetViewRequested: viewportFrame.resetView()
        onReferencePeekToggleRequested: root.toggleDifferencePeek(false)
        onReferencePeekLockToggleRequested: root.toggleDifferencePeek(true)
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

    // Workspace-level shortcuts stay active across both workspaces; media transport shortcuts
    // remain owned by ReviewShortcuts and are gated by globalMediaShortcutsEnabled. Sequences
    // come from ShortcutCatalog.js - the same constants the menu labels and the help rows use.
    Shortcut {
        sequence: ShortcutCatalog.openVideoFolder
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0 && root.videoFolderModel !== null
        onActivated: root.requestOpenVideoFolder()
    }

    Shortcut {
        sequence: ShortcutCatalog.openVideos
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0
        onActivated: root.requestOpenVideos()
    }

    Shortcut {
        sequence: ShortcutCatalog.addVideo
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0 && root.videoHasSession && root.sourceCount < 3
        onActivated: root.requestAddVideo()
    }

    Shortcut {
        sequence: ShortcutCatalog.openImage
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0
        onActivated: root.requestImageOpen()
    }

    Shortcut {
        sequence: ShortcutCatalog.openImagePair
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0
        onActivated: root.requestImagePairOpen()
    }

    Shortcut {
        sequence: ShortcutCatalog.addImage
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0 && root.imageHasContent && !Boolean(root.stillImageController && root.stillImageController.hasSecondary)
        onActivated: root.requestImageAdd()
    }

    Shortcut {
        sequence: ShortcutCatalog.closeCurrentTask
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0 && root.activeTaskHasMedia
        onActivated: root.closeCurrentTask()
    }

    Shortcut {
        sequence: ShortcutCatalog.compareImageFolders
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0
        onActivated: root.requestCompareFolders()
    }

    // M for "mark": freezes the observation and opens the note dialog; Enter attaches the
    // typed note, Escape attaches an empty one. Active in both workspaces whenever there is
    // something to capture.
    Shortcut {
        sequence: ShortcutCatalog.captureIssue
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0 && (root.videoHasSession || root.imageHasContent)
        onActivated: root.captureIssueLog()
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

    // C1 resume writes are debounced well below the settings save debounce so scrubbing never turns
    // into disk IO, but still short enough that closing the window moments after moving the
    // playhead does not lose the position.
    Timer {
        id: resumeRecordTimer

        interval: 1500
        repeat: false
        onTriggered: root.recordResumePosition()
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
                if (Number(kind) === 0) {
                    root.pendingNewReviewWantsThreeUp = false;
                    if (!root.videoFolderModel || !root.videoFolderModel.openPending)
                        workspaceSession.cancelOpen();
                }

                root.showIntentMessage(errorKey.length > 0 ? root.messageCatalog.errorMessage(errorKey) : qsTr("检查请求失败。"));

                return;
            }

            if (Number(kind) === 0) {
                if (root.pendingNewReviewWantsThreeUp && root.sourceCount === 3 && root.preferences)
                    root.preferences.viewMode = root.threeUpViewMode;

                root.pendingNewReviewWantsThreeUp = false;

                root.resetReviewVisualState();
                // Normal CLI/shell opens bypass the selectors. Commit their workspace only
                // after the shell has adopted the successful source set, not on submission.
                const identity = root.activeSourceUrls().map(url => url.toString()).join("\n");
                root.commitWorkspace(workspaceSession.videoMedia, identity);
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
        target: root.videoFolderModel

        function onCurrentFileOpened() {
            root.videoFolderSidebarVisible = true;
        }
    }

    Connections {
        target: root.stillImageController

        function onOpenFinished(requestId, pairId, success, error) {
            root.completeImageOpen(requestId, pairId, success, error);
        }
    }

    Connections {
        target: root.folderPairModel

        function onPairOpenFinished(row, success, error) {
            root.completeFolderPairOpen(row, success, error);
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

    // The video input-dialog cluster (two native pickers plus the drop-confirmation popup)
    // exists only once an open/add/drop flow starts: until then its tree is dead chrome on
    // the startup path. Activation is synchronous, so the first open pays one instantiation
    // and the dialog then stays resident for the session, exactly as before.
    // Typed view of the lazily instantiated dialog cluster: Loader.item is only a QObject to
    // the linter, so member access goes through this property and stays type-checked. The
    // linter cannot see which component the Loader instantiates, hence the local disable.
    // qmllint disable incompatible-type
    readonly property ReviewInputDialogs reviewInputDialogs: reviewInputDialogsLoader.item
    // qmllint enable incompatible-type

    Loader {
        id: reviewInputDialogsLoader

        active: false

        sourceComponent: reviewInputDialogsComponent
    }

    Component {
        id: reviewInputDialogsComponent

        ReviewInputDialogs {
            stagedVideos: root.shell ? root.shell.stagedSources : []
            fileNameFunction: root.fileName
            pathNameFunction: root.sourcePathLabel
            initialReferenceIndex: root.pendingSidebarComparison && root.shell ? root.shell.stagedReferenceIndex : (root.pendingComparisonPreservesPosition ? root.canonicalSourceIndex : 0)
            onVideoFolderAccepted: folder => root.loadVideoFolder(folder)
            onVideoFolderRejected: root.focusActiveWorkspace()
            onOpenVideosAccepted: urls => root.openNewReviewUrls(urls)
            onOpenVideosRejected: root.cancelWorkspaceOpen()
            onAddVideoAccepted: url => root.reviewDroppedUrls([url])
            onAddVideoRejected: root.cancelWorkspaceOpen()
            onMoveRequested: (fromIndex, toIndex) => root.swapDroppedVideos(fromIndex, toIndex)
            onComparisonAccepted: referenceIndex => {
                root.openDroppedComparison(referenceIndex);
            }
            onComparisonClosed: root.cancelSidebarComparison()
            onComparisonRejected: {
                if (root.shell)
                    root.shell.clearStagedSources();

                root.pendingComparisonPreservesPosition = false;

                root.pendingNewReviewWantsThreeUp = false;

                root.cancelWorkspaceOpen();
            }
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
        referenceSourceIndex: root.referenceSourceIndex
        referenceSourceIdentity: root.shell ? root.shell.referenceSourceIdentity : ""
        pendingSourceIdentities: root.shell ? root.shell.pendingSourceIdentities : []
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        displayOrder: root.shell ? root.shell.displaySourceIdentities : []
        selectedIdentities: root.shell ? root.shell.selectedSourceIdentities : []
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
        onAddRequested: root.requestAddVideo()
        onRemoveRequested: sourceIdentity => root.removeSelectedSource(sourceIdentity)
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onMoveRequested: (fromIndex, toIndex) => {
            if (root.shell)
                root.shell.moveSourceInDisplayOrder(fromIndex, toIndex);
        }
        onSelectionToggled: sourceIdentity => {
            if (root.shell)
                root.shell.toggleSourceSelection(sourceIdentity);
        }
        onSelectionCleared: {
            if (root.shell)
                root.shell.clearSourceSelection();
        }
        onRemoveSelectedRequested: {
            if (root.shell)
                root.shell.removeSelectedSources();
        }
        onViewerFocusRequested: root.returnFocusToViewer()
    }
    CompareModeBar {
        id: comparisonBar

        visible: !root.imageWorkspaceActive && root.sourceCount > 1
        sourceCount: root.sourceCount
        currentMode: root.effectiveViewMode
        // qmllint disable unqualified
        differenceMetric: root.preferences ? Number(root.preferences.differenceMetric) : ComparisonSurface.RgbAbsolute
        // qmllint enable unqualified
        differenceEdges: root.differenceEdges
        currentEdgeIndex: root.differenceEdgeIndex(root.differenceEdge)
        inspectorOpen: root.inspectorOpen
        busy: root.busy
        peekLocked: root.differencePeekLocked
        peekTargetValid: root.differencePeekTargetValid
        borderColor: root.borderColor
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        anchors {
            top: sourceBar.bottom
            left: parent.left
            right: parent.right
        }
        onModeRequested: mode => root.preferences.viewMode = mode
        onEdgeRequested: edge => root.applyDifferenceEdge(edge)
        // qmllint disable unqualified
        onDifferenceViewRequested: metric => {
            root.preferences.viewMode = ComparisonSurface.Difference;
            root.preferences.differenceMetric = metric;
        }
        // qmllint enable unqualified
        onDifferencePeekChanged: held => {
            // A keyboard lock must survive a stray press/release of the mouse button.
            if (held || !root.differencePeekLocked)
                root.differencePeekActive = held && root.differencePeekTargetValid;
        }
        onSwitchCandidateRequested: root.switchCandidateEdge()
        onCopyComparisonRequested: root.copyComparisonImage()
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
        metrics: root.pairMetricsModel
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
        onDifferenceEdgeRequested: edge => root.applyDifferenceEdge(edge)
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onCloseRequested: {
            if (root.shell)
                root.shell.inspectorVisible = false;
            root.returnFocusToViewer();
        }
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

        // An open completes only after its first presentation ACK. Keep the surface drawable
        // behind the current image task while opening, without switching its UI or key receiver.
        visible: !root.imageWorkspaceActive || Boolean(root.shell && Number(root.shell.activeIntent.kind) === 0)
        enabled: !root.imageWorkspaceActive
        preferences: root.preferences
        borderColor: root.borderColor
        accentColor: root.accentColor
        primaryTextColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        chromeVisible: root.chromeVisible
        // Lift the viewport's own corner controls clear of the floating transport. 0 when the
        // transport is docked or hidden, so the docked layout is byte-for-byte unchanged.
        bottomOverlayInset: root.transportOverlayInset
        alignmentModeName: root.controller ? root.controller.alignmentModeName : ""
        inexactReason: root.controller ? root.controller.currentInexactReason : ""
        effectiveViewMode: root.effectiveViewMode
        wipePosition: root.wipePosition
        selectedDifferenceExactness: root.selectedDifferenceExactness
        selectedDifferenceEdge: root.selectedDifferenceEdge
        differenceThresholdEnabled: root.differenceThresholdEnabled
        differenceThresholdCode: root.differenceThresholdCode
        differenceThresholdPolicy: root.differenceThresholdPolicy
        // Keyboard peek can outlive a difference view switch (locked); it only ever
        // suppresses the difference pass itself.
        differenceSuppressed: root.differencePeekActive && root.differenceMode
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
        sourceParentLabels: root.controller ? root.controller.sourceParentLabels : []
        sourceFullPaths: root.controller ? root.controller.sourceFullPaths : []
        sourceMediaInfo: root.sourceMediaInfo
        differencePeekActive: root.differencePeekActive
        differencePeekLocked: root.differencePeekLocked
        differencePeekTargetLabel: root.differencePeekTargetLabel
        frameErrorBannerVisible: root.frameErrorBannerVisible
        errorDetail: root.errorDetails()
        overlayVisible: root.overlayVisible
        hasErrors: root.hasErrors
        busy: root.busy
        overlayTitle: root.overlayTitle
        overlayDetail: root.overlayDetail
        playbackActive: root.controller ? root.controller.playing : false
        playbackHudRevealed: transport.revealActive
        anchors {
            top: parent.top
            topMargin: root.chromeVisible && !root.singleMode ? sourceBar.height + comparisonBar.height + 6 : 0
            bottom: parent.bottom
            bottomMargin: root.transportDocked ? root.transportDockHeight : 0
            left: parent.left
            leftMargin: (root.chromeVisible ? 14 : 0) + root.videoFolderInset
            right: alignmentBar.visible && root.width >= 1120 ? alignmentBar.left : parent.right
            rightMargin: root.chromeVisible ? 14 : 0
        }
        onWipePositionRequested: position => root.wipePosition = position
        onOscRevealRequested: root.revealOsc()
        onContextMenuRequested: root.openViewerContextMenu()
    }
    // The still-image workspace is built on first activation and stays resident: video review
    // sessions never pay for it, and an image launch activates it during the initial binding
    // pass exactly as before. Typed view like reviewInputDialogs above.
    // qmllint disable incompatible-type
    readonly property ImageWorkspace imageWorkspace: imageWorkspaceLoader.item
    // qmllint enable incompatible-type

    // qmllint disable incompatible-type
    readonly property VideoFolderSidebar videoFolderSidebar: videoFolderSidebarLoader.item
    // qmllint enable incompatible-type

    Loader {
        id: videoFolderSidebarLoader
        active: root.videoFolderListVisible
        visible: active
        width: 260
        z: 36
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.top: parent.top
        anchors.topMargin: root.imageWorkspaceActive ? 10 : (root.chromeVisible && !root.singleMode ? sourceBar.height + comparisonBar.height + 6 : 0)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.imageWorkspaceActive ? 0 : (root.transportDocked ? root.transportDockHeight : 0)
        sourceComponent: VideoFolderSidebar {
            folderModel: root.videoFolderModel
            fileActionState: url => root.sidebarVideoActionState(url)
            actionRevision: JSON.stringify([root.shell ? root.shell.activeSources : [], root.referenceSourceIndex, root.busy, root.graphicsReady, root.imageWorkspaceActive, root.videoFolderModel ? root.videoFolderModel.scanning : false, root.shell ? root.shell.activeIntent : {}, root.shell ? root.shell.queuedIntentCount : 0])
            onContextOpenRequested: url => root.openSidebarVideo(url)
            onCompareRequested: url => root.compareSidebarVideo(url)
            errorText: {
                const text = root.videoFolderModel ? root.videoFolderModel.errorText : "";
                return /^[a-z][a-z0-9-]+$/.test(text) ? root.messageCatalog.errorMessage(text) : text;
            }
            onChooseFolderRequested: root.requestOpenVideoFolder()
            onFileRequested: row => root.openFolderVideo(row)
            onRecentFileRequested: row => root.openRecentVideo(row)
            onStepRequested: delta => root.stepFolderVideo(delta)
            onCloseRequested: root.videoFolderSidebarVisible = false
        }
    }

    Loader {
        id: imageWorkspaceLoader

        property bool keepActive: false

        active: root.imageWorkspaceActive || keepActive

        onLoaded: keepActive = true

        anchors {
            top: parent.top
            topMargin: root.chromeVisible ? 10 : 0
            bottom: parent.bottom
            left: parent.left
            leftMargin: (root.chromeVisible ? 14 : 0) + root.videoFolderInset
            right: parent.right
            rightMargin: root.chromeVisible ? 14 : 0
        }

        sourceComponent: imageWorkspaceComponent
    }

    Component {
        id: imageWorkspaceComponent

        ImageWorkspace {
            objectName: "imageWorkspaceRoot"
            anchors.fill: parent
            visible: root.imageWorkspaceActive
            controller: root.stillImageController
            imageEdit: root.imageEditService
            pairModel: root.folderPairModel
            sidebarVisible: root.imageFolderSidebarVisible
            onOpenImageRequested: root.requestImageOpen()
            onAddImageRequested: root.requestImageAdd()
            onOpenPairRequested: root.requestImagePairOpen()
            onCompareFoldersRequested: root.requestCompareFolders()
            onToggleSidebarRequested: root.imageFolderSidebarVisible = !root.imageFolderSidebarVisible
            onSwapSidesRequested: root.requestImageSwapSides()
            onReplacePrimaryRequested: root.requestImageReplacePrimary()
            onReplaceSecondaryRequested: root.requestImageReplaceSecondary()
        }
    }
    NativeDialogs.FolderDialog {
        id: imageFolderLeftDialog

        objectName: "imageFolderLeftDialog"
        title: qsTr("选择文件夹 A")
        onAccepted: {
            root.imageFolderLeftUrl = selectedFolder;

            root.imageFolderStage = 1;

            imageFolderRightDialog.open();
        }
        onRejected: root.cancelWorkspaceOpen()
    }
    NativeDialogs.FolderDialog {
        id: imageFolderRightDialog

        objectName: "imageFolderRightDialog"
        title: qsTr("选择文件夹 B")
        onAccepted: {
            root.imageFolderRightUrl = selectedFolder;

            root.loadFolderComparison();
        }
        onRejected: root.cancelWorkspaceOpen()
    }
    NativeDialogs.FileDialog {
        id: imageSingleDialog

        objectName: "imageSingleDialog"
        title: qsTr("打开图片")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.pnm *.ppm *.pgm *.pbm *.pam)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (picked && picked.toString().length > 0)
                root.performImageReview([picked]);
        }
        onRejected: root.cancelWorkspaceOpen()
    }
    NativeDialogs.FileDialog {
        id: imageAddDialog

        objectName: "imageAddDialog"
        title: qsTr("添加图片")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.pnm *.ppm *.pgm *.pbm *.pam)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (picked && picked.toString().length > 0)
                root.reviewUrls([picked], true);
        }
        onRejected: root.cancelWorkspaceOpen()
    }
    NativeDialogs.FileDialog {
        id: imageReplacePrimaryDialog

        objectName: "imageReplacePrimaryDialog"
        title: qsTr("替换 A")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.pnm *.ppm *.pgm *.pbm *.pam)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (picked && picked.toString().length > 0)
                root.performImageSideReplace(picked, "a");
        }
    }
    NativeDialogs.FileDialog {
        id: imageReplaceSecondaryDialog

        objectName: "imageReplaceSecondaryDialog"
        title: qsTr("替换 B")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.pnm *.ppm *.pgm *.pbm *.pam)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (picked && picked.toString().length > 0)
                root.performImageSideReplace(picked, "b");
        }
    }
    NativeDialogs.FileDialog {
        id: imagePairDialog

        objectName: "imagePairDialog"
        title: qsTr("打开图片对")
        fileMode: NativeDialogs.FileDialog.OpenFiles
        nameFilters: [qsTr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.pnm *.ppm *.pgm *.pbm *.pam)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const files = selectedFiles && selectedFiles.length > 0 ? selectedFiles : [selectedFile];

            root.performImagePairSelection(files);
        }
        onRejected: root.cancelWorkspaceOpen()
    }
    NativeDialogs.FileDialog {
        id: issueSaveDialog

        objectName: "issueSaveDialog"
        title: qsTr("保存问题记录")
        fileMode: NativeDialogs.FileDialog.SaveFile
        nameFilters: [qsTr("问题记录 (*.json)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (root.issueLogModel && picked && picked.toString().length > 0)
                root.issueLogModel.saveIssues(picked);
        }
    }
    NativeDialogs.FileDialog {
        id: issueLoadDialog

        objectName: "issueLoadDialog"
        title: qsTr("加载问题记录")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("问题记录 (*.json)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (root.issueLogModel && picked && picked.toString().length > 0) {
                root.issueLogModel.loadIssues(picked);

                root.issueLogPanelVisible = true;
            }
        }
    }
    // Lossless range-clip export. The dialog owns the destination picker and the progress readout;
    // the host owns the two things that must happen outside it: an outcome line in the status HUD,
    // and surfacing the adapter's own message when an export fails while the dialog is closed.
    // The dialog tree is instantiated on first use: the export chip gates it behind a validated
    // range, so until an export is requested it is pure startup cost.
    // Typed view of the lazily instantiated export dialog; see reviewInputDialogs above.
    // qmllint disable incompatible-type
    readonly property ClipExportDialog clipExportDialog: clipExportDialogLoader.item
    // qmllint enable incompatible-type

    Loader {
        id: clipExportDialogLoader

        active: false

        sourceComponent: clipExportDialogComponent
    }

    Component {
        id: clipExportDialogComponent

        ClipExportDialog {
            service: root.clipExportService
        }
    }
    Connections {
        target: root.clipExportService
        enabled: root.clipExportService !== null
        function onExportFinished(succeeded, message) {
            root.showImmersiveHud(message);

            // A cancel is deliberate and stays silent; a real failure reopens the dialog so the
            // technical detail is readable instead of disappearing with the popup.
            if (!succeeded && root.clipExportDialog && root.clipExportDialog.failureDetail.length > 0)
                root.clipExportDialog.open();
        }
    }
    Rectangle {
        id: issueLogPanel

        objectName: "issueLogPanel"
        visible: root.issueLogPanelVisible && root.issueLogModel
        z: 60
        width: 320
        color: Theme.panel
        border.color: Theme.menuBorder
        border.width: 1
        radius: 6
        anchors {
            top: parent.top
            topMargin: 48
            right: parent.right
            rightMargin: 16
            bottom: parent.bottom
            bottomMargin: 80
        }

        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            Row {
                width: parent.width
                spacing: 8

                Text {
                    text: qsTr("问题记录")
                    color: root.primaryTextColor
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    anchors.verticalCenter: parent.verticalCenter
                }
                Item {
                    width: parent.width - 160
                    height: 1
                }
                ReviewActionButton {
                    objectName: "issueLogCloseButton"
                    text: qsTr("关闭")
                    onClicked: root.issueLogPanelVisible = false
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: root.issueLogModel ? root.issueLogModel.statusText : ""
                color: root.mutedTextColor
                font.pixelSize: 11
                objectName: "issueLogStatusText"
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                visible: root.issueLogModel && root.issueLogModel.lastError.length > 0
                text: root.issueLogModel ? root.issueLogModel.lastError : ""
                color: Theme.errorText
                font.pixelSize: 11
                objectName: "issueLogErrorText"
            }

            ListView {
                id: issueLogList

                objectName: "issueLogList"
                width: parent.width
                height: parent.height - 120
                clip: true
                model: root.issueLogModel ? root.issueLogModel.count : 0
                delegate: Rectangle {
                    id: issueLogRow

                    required property int index

                    width: issueLogList.width
                    height: 44
                    color: Theme.raisedPanel
                    border.color: Theme.border
                    radius: 4
                    Column {
                        anchors.fill: parent
                        anchors.margins: 6
                        Text {
                            text: root.issueLogModel ? String(root.issueLogModel.issueAt(issueLogRow.index).summary || "") : ""
                            color: root.primaryTextColor
                            font.pixelSize: 12
                        }
                        Text {
                            text: root.issueLogModel ? String(root.issueLogModel.issueAt(issueLogRow.index).note || "") : ""
                            color: root.mutedTextColor
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            width: parent.width
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (!root.issueLogModel)
                                return;

                            // IssueLogController::restoreIssue already emits restoreRequested,
                            // and the Connections block below is the single entry that runs the
                            // restore. Applying the returned payload here as well would open
                            // the recorded sources twice from one click.
                            root.issueLogModel.restoreIssue(issueLogRow.index);
                        }
                    }
                }
            }

            Row {
                width: parent.width
                spacing: 8

                ReviewActionButton {
                    objectName: "issueLogCaptureButton"
                    text: qsTr("记录当前")
                    onClicked: root.captureIssueLog()
                }
                ReviewActionButton {
                    objectName: "issueLogClearButton"
                    text: qsTr("清空")
                    onClicked: root.issueLogModel && root.issueLogModel.clearIssues()
                }
            }
        }

        Connections {
            target: root.issueLogModel
            function onRestoreRequested(payload) {
                root.applyIssueRestore(payload);
            }
        }
    }
    EmptyReviewView {
        visible: !root.imageWorkspaceActive && root.sourceCount === 0 && !root.busy && root.graphicsReady && !root.hasErrors
        z: 35
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        anchors.fill: viewportFrame
        onOpenVideoFolderRequested: root.requestOpenVideoFolder()
        onOpenVideosRequested: root.requestOpenVideos()
        onOpenImageRequested: root.requestImageOpen()
        onOpenImagePairRequested: root.requestImagePairOpen()
        onCompareFoldersRequested: root.requestCompareFolders()
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
        metrics: root.pairMetricsModel
        playing: root.playing
        playbackRate: root.controller ? Number(root.controller.playbackRate) : 1
        playbackModeLabel: root.playbackModeLabel
        playbackModeDetail: root.playbackModeDetail
        playbackTargetRate: root.playbackTargetRate
        playbackPresentationRate: root.playbackPresentationRate
        playbackRunSkippedFrameSets: root.playbackRunSkippedFrameSets
        playbackRunPresentedFrames: root.playbackRunPresentedFrames
        playbackLagMilliseconds: root.playbackLagMilliseconds
        playbackCatchingUp: root.playbackCatchingUp
        displayGapCount: Number(root.droppedFrames)
        sourceDuplicateCount: root.sourceDuplicateCount
        sourceRateText: root.sourceRateText
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
        rangeExportVisible: root.rangeExportVisible
        rangeExportEnabled: root.rangeExportEnabled
        rangeExportBusy: root.clipExportService !== null && Boolean(root.clipExportService.busy)
        rangeExportProgress: root.clipExportService !== null ? Number(root.clipExportService.progress) : 0
        previewFrame: root.effectivePreviewFrame
        previewTimecode: root.previewTimecode
        previewThumbnailSource: {
            // `generation` participates so a session switch (decoded-cache reset) and every
            // finished decode re-evaluate this binding even when the hover frame is unchanged.
            if (root.previewThumbnailsModel && root.previewThumbnailsModel.generation > 0 && root.timelinePreviewFrame >= 0) {
                const decoded = root.previewThumbnailsModel.urlForFrame(root.timelinePreviewFrame);
                if (decoded && decoded.toString().length > 0)
                    return decoded;
            }
            return root.previewInfo ? root.previewInfo.url : "";
        }
        previewIsApproximate: {
            if (root.previewThumbnailsModel && root.previewThumbnailsModel.generation > 0 && root.timelinePreviewFrame >= 0 && root.previewThumbnailsModel.hasThumbnail(root.timelinePreviewFrame))
                return false;
            return root.previewInfo ? !root.previewInfo.isExact : false;
        }
        previewSampleFrame: root.previewInfo ? root.previewInfo.sampleFrame : -1
        playbackContinuityPolicy: root.controller ? root.controller.playbackContinuityPolicy : 1
        anchors {
            left: viewportFrame.left
            right: root.drawerMode ? alignmentBar.left : viewportFrame.right
            bottom: parent.bottom
        }
        onOverlayHidden: {
            if (!root.chromeVisible)
                root.immersiveOscRevealed = false;
        }
        // Transport range row intents. These reuse the very same range functions the inspector
        // buttons and the I / O / \ shortcuts call, so every entry point shares one range state.
        onMarkInRequested: root.setInPoint()
        onMarkOutRequested: root.setOutPoint()
        onPlayRangeRequested: root.playSelectedRange()
        onRangeLoopRequested: root.toggleRangeLoop()
        onClearRangeRequested: root.clearSelectedRange()
        onExportRangeRequested: root.openClipExportDialog()
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
        imagePreset: root.imageWorkspaceActive
        mediaEntries: root.imageWorkspaceActive ? [] : reviewShortcuts.helpEntries
        workspaceEntries: root.workspaceHelpEntries
    }

    IssueNoteDialog {
        id: issueNoteDialog

        onAccepted: note => root.captureIssueLogWithNote(note)
    }

    ReviewContextMenu {
        id: viewerContextMenu

        sourceCount: root.sourceCount
        canonicalSourceIndex: root.canonicalSourceIndex
        referenceSourceIndex: root.referenceSourceIndex
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

        onEdgeRequested: edge => root.applyDifferenceEdge(edge)
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onOpenRequested: root.requestOpenVideos()
        onInspectorRequested: root.shell.inspectorVisible = true
        onFullScreenRequested: root.toggleFullScreen()
        onViewerFocusRequested: root.returnFocusToViewer()
    }

    // Transient notices share one stack at the bottom of the stage, above the stage's own
    // bottom controls and a floating transport, so they never cover the header toolbars. With
    // the chrome hidden the stack sits under the restore pill, clear of the immersive HUD.
    Column {
        id: notificationStack

        objectName: "notificationStack"
        readonly property real stageLeft: root.imageWorkspaceActive ? imageWorkspaceLoader.x : viewportFrame.x
        readonly property real stageRight: {
            if (root.imageWorkspaceActive)
                return imageWorkspaceLoader.x + imageWorkspaceLoader.width;
            return root.drawerMode ? alignmentBar.x : viewportFrame.x + viewportFrame.width;
        }
        readonly property real stageFloor: {
            if (root.imageWorkspaceActive) {
                const inset = root.imageWorkspace ? root.imageWorkspace.bottomControlsInset : 0;
                return imageWorkspaceLoader.y + imageWorkspaceLoader.height - inset;
            }
            const viewportFloor = viewportFrame.y + viewportFrame.height - viewportFrame.bottomControlsInset;
            return transport.visible && !root.transportDocked ? Math.min(viewportFloor, transport.y) : viewportFloor;
        }
        readonly property real noticeWidth: Math.max(0, Math.min(560, (root.chromeVisible ? notificationStack.stageRight - notificationStack.stageLeft : root.width) - 48))

        z: 990
        spacing: 8
        x: Math.round((root.chromeVisible ? (notificationStack.stageLeft + notificationStack.stageRight) / 2 : root.width / 2) - notificationStack.width / 2)
        y: Math.round(root.chromeVisible ? notificationStack.stageFloor - 12 - notificationStack.height : chromeRestorePill.y + chromeRestorePill.height + 10)

        Rectangle {
            visible: root.intentMessage.length > 0
            width: Math.min(notificationStack.noticeWidth, intentToastText.implicitWidth + 32)
            height: intentToastText.paintedHeight + 20
            radius: Theme.radiusLarge
            color: Theme.oscGlass
            border.color: Theme.oscBorder
            anchors.horizontalCenter: parent.horizontalCenter

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
            id: intentQueuePanel

            readonly property var runningIntent: root.shell ? root.shell.activeIntent : ({})
            readonly property var queued: root.shell ? root.shell.queuedIntents : []
            visible: Number(runningIntent.id || 0) > 0 || queued.length > 0
            width: 286
            height: queueColumn.implicitHeight + 20
            radius: Theme.radiusLarge
            color: Theme.oscGlass
            border.color: Theme.oscBorder
            anchors.horizontalCenter: parent.horizontalCenter

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
            id: imageOpenProgress

            objectName: "imageOpenProgress"
            // Opening a file is progress, not a failure, so this notice stays neutral.
            visible: root.pendingImageRequestId > 0 && root.dropError.length === 0
            width: progressRow.implicitWidth + 28
            height: 42
            radius: Theme.radiusLarge
            color: Theme.oscGlass
            border.color: Theme.oscBorder
            anchors.horizontalCenter: parent.horizontalCenter

            Row {
                id: progressRow

                spacing: 10
                anchors.centerIn: parent

                BusyIndicator {
                    running: imageOpenProgress.visible
                    width: 22
                    height: 22
                }
                Text {
                    text: {
                        if (root.pendingImageOpenKind === "append")
                            return qsTr("正在添加图片…");
                        if (root.pendingImageOpenKind === "replaceA")
                            return qsTr("正在替换 A…");
                        if (root.pendingImageOpenKind === "replaceB")
                            return qsTr("正在替换 B…");
                        return qsTr("正在打开图片…");
                    }
                    color: root.primaryTextColor
                    font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }
                ReviewActionButton {
                    text: qsTr("取消")
                    implicitWidth: 58
                    implicitHeight: 28
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: root.cancelWorkspaceOpen()
                }
            }
        }

        // A failed open is a real failure, so this is the one notice drawn in the error family.
        Rectangle {
            visible: root.dropError.length > 0
            width: Math.min(notificationStack.noticeWidth, dropErrorText.implicitWidth + 34)
            height: dropErrorText.implicitHeight + 26
            radius: Theme.radiusLarge
            color: Theme.errorPanel
            border.color: Theme.errorBorder
            anchors.horizontalCenter: parent.horizontalCenter

            Text {
                id: dropErrorText

                width: parent.width - 34
                text: root.dropError
                color: Theme.errorText
                font.pixelSize: 13
                wrapMode: Text.Wrap
                anchors.centerIn: parent
            }

            TapHandler {
                onTapped: root.dropError = ""
            }
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
        color: restorePillHover.hovered ? Theme.oscGlassHover : Theme.oscGlass
        border.color: Theme.oscBorder
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
    Item {
        id: immersiveWakeStrip

        objectName: "immersiveWakeStrip"
        visible: !root.imageWorkspaceActive && !root.chromeVisible && root.sourceCount > 0
        z: 970
        height: 14
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        HoverHandler {
            onHoveredChanged: {
                if (hovered)
                    root.revealImmersiveOsc();
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
        color: Theme.dropScrim
        border.width: 3
        border.color: root.accentColor

        Column {
            spacing: 10
            anchors.centerIn: parent

            Text {
                text: qsTr("松开即可在 CompareStation 中打开")
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
}
