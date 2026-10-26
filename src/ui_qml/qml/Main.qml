pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import QtQuick.Window
import "VcsTheme.js" as Theme
import "LayoutMetrics.js" as LayoutMetrics
import "StageStates.js" as StageStates
import "MediaLabels.js" as MediaLabels
import "ShortcutCatalog.js" as ShortcutCatalog

// Dvs.Ui is registered by the C++ host before this document is loaded; the generated
// .qmltypes (see docs/adr/0006-qml-type-registration.md) lets qmllint resolve the module's
// types and enums instead of suppressing the import.

import Dvs.Ui 1.0

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
    // The validated comparison actually identifies a GT source. Without it
    // referenceSourceIndex falls back to the canonical (timeline master) slot, so every
    // GT-anchored action gates on this flag - a predictions-only set must never read its
    // master as a reference.
    readonly property bool hasExplicitReference: controller ? controller.hasExplicitReference === true : false
    readonly property int canonicalSourceIndex: controller ? Number(controller.canonicalSourceIndex) : -1
    readonly property bool inspectorOpen: shell ? Boolean(shell.inspectorVisible) : false
    readonly property bool chromeVisible: shell ? Boolean(shell.chromeVisible) : true
    // UX-13 S4 compact-chrome tier (docs/plans/ux13-narrow-window-redesign.md §2 D4): same
    // threshold and derivation as PlayerOsc (S1) and ImageWorkspace (S2) - the hosting
    // window's height decides, 680 gives the 960x640 window minimum + frame rounding a
    // stable band (plan §1). Below it the video source strip and the mode bar merge into one
    // 40 px row; every other layout stays exactly as the wide window renders it. A plain
    // property so the contract tests can pin either tier explicitly.
    // The S1/S2 consumers write `Window.window.height <= 680` because their roots are Items;
    // this root IS the window, and the Window attached property only resolves to a window
    // for Item attachees - on the window itself it reads null at runtime (qmllint's static
    // type cannot see that), so the shell reads its own height directly instead. Same
    // threshold, same meaning, no attached indirection.
    property bool compactChrome: height <= 680
    // The video chrome budget everything below the top rows consumes: one merged 40 px row
    // in the compact tier, the two stacked 40 px rows otherwise (both bars report height 0
    // for an empty session, so the value collapses with them).
    readonly property real videoChromeHeight: compactChrome ? Math.max(sourceBar.height, comparisonBar.height) : sourceBar.height + comparisonBar.height
    // The file sidebar has a fixed width; the inspector retains its session width preference.
    // The canvas guard can temporarily suppress the sidebar without losing its visible flag.
    readonly property real videoFolderSidebarWidth: Theme.fileSidebarWidthDefault
    property real inspectorPanelWidth: Theme.inspectorWidthDefault
    readonly property real clampedSidebarWidth: LayoutMetrics.clampFileSidebarWidth(videoFolderSidebarWidth)
    readonly property real clampedInspectorWidth: LayoutMetrics.clampInspectorWidth(inspectorPanelWidth)
    readonly property bool videoFolderListVisible: videoFolderModel !== null && videoFolderSidebarVisible && chromeVisible && LayoutMetrics.fileSidebarAllowed(LayoutMetrics.availableCanvasWidth(root.width, chromeVisible, false, 0, false, 0), Theme.canvasMinimumWidth)
    readonly property real videoFolderInset: videoFolderListVisible ? LayoutMetrics.fileSidebarStageWidth(clampedSidebarWidth) : 0
    // Canvas width before the inspector's own claim; the guard decision below consumes it.
    readonly property real availableCanvasWithoutInspector: LayoutMetrics.availableCanvasWidth(root.width, chromeVisible, videoFolderListVisible, clampedSidebarWidth, false, 0)
    readonly property bool inspectorDocked: alignmentBar.visible && LayoutMetrics.inspectorShouldDock(availableCanvasWithoutInspector, clampedInspectorWidth, Theme.canvasMinimumWidth)
    readonly property bool drawerMode: alignmentBar.visible && !inspectorDocked
    property int visibilityBeforeFullScreen: Window.Windowed

    // 0 = video compare workspace, 1 = still-image workspace. Kept writable so existing

    // automation can select the visible workspace; routing code should call activateWorkspace().

    property int workspaceMode: 0
    property bool issueLogPanelVisible: false
    property bool nearbyFramesCardVisible: false
    property int activeDiagnosticIssueIndex: -1
    // qmllint disable unqualified
    readonly property var issueLogModel: typeof issueLog !== "undefined" ? issueLog : null
    readonly property var comparisonExportService: typeof comparisonExport !== "undefined" ? comparisonExport : null
    // P03 interpolation-protocol controller from the composition root. The typeof guard keeps
    // lightweight QML-only harnesses valid: without the controller the transport renders no
    // protocol row and every protocol action below is skipped.
    readonly property var protocolModel: typeof interpolationProtocol !== "undefined" ? interpolationProtocol : null
    // Range clip-export controller from the composition root. Only the composition root knows
    // whether a lossless exporter exists, so its presence alone decides whether the transport
    // shows an export chip; the typeof guard keeps lightweight QML-only harnesses valid.
    readonly property var clipExportService: typeof clipExport !== "undefined" ? clipExport : null
    // E01 training-sample triplet export controller from the composition root.
    readonly property var sampleExportModel: typeof sampleExport !== "undefined" ? sampleExport : null
    // Performance automation and smoke runs keep the playhead deterministic: a resume restore
    // here would move the frame-zero baseline seconds after the open and fail the run.
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

    // The pilot's required services, declared by the root itself: the composition root
    // passes the real adapter instances as initial properties, a missing one fails
    // creation, and the production loader verifies identity before showing the window
    // (an explicit null or a wrong-typed object degrades the property to null with a
    // warning instead of failing creation). See docs/adr/0006-qml-type-registration.md.
    required property ImageReviewController stillImageController
    required property ColorClipboardController colorClipboardService
    // qmllint disable unqualified
    // Multi-task host from the composition root. Absent in isolated harnesses, where the
    // single stillImageController remains the whole image workspace.
    readonly property var imageTaskHost: typeof imageTasks !== "undefined" ? imageTasks : null
    // qmllint enable unqualified
    readonly property ImageReviewController activeImageController: root.imageTaskHost && root.imageTaskHost.activeController ? root.imageTaskHost.activeController : root.stillImageController
    readonly property var imageTabList: root.imageTaskHost ? root.imageTaskHost.tabs : []
    readonly property int imageTabCount: root.imageTaskHost ? Number(root.imageTaskHost.count) : 0
    readonly property int imageTabActiveIndex: root.imageTaskHost ? Number(root.imageTaskHost.activeIndex) : -1
    readonly property bool imageEditComparisonTask: root.imageTabActiveIndex >= 0 && root.imageTabActiveIndex < root.imageTabList.length && Boolean(root.imageTabList[root.imageTabActiveIndex].editComparison)
    // An in-place image or folder open still commits into the task that started it. Switching
    // or closing the active tab during that window would apply the terminal to the wrong task.
    readonly property bool imageTabOpenPending: root.pendingImageRequestId > 0 || root.pendingFolderPairRow >= 0 || Boolean(root.folderPairModel && root.folderPairModel.openPending)
    // qmllint disable unqualified
    // Video task-tab host from the composition root (video task tabs design §1). Absent in
    // isolated harnesses, where the legacy single-session open paths stay in force.
    readonly property var videoTaskHost: typeof videoTasks !== "undefined" ? videoTasks : null
    // qmllint enable unqualified
    readonly property var videoTabList: root.videoTaskHost ? root.videoTaskHost.tabs : []
    readonly property int videoTabCount: root.videoTaskHost ? Number(root.videoTaskHost.count) : 0
    readonly property int videoTabActiveIndex: root.videoTaskHost ? Number(root.videoTaskHost.activeIndex) : -1
    // One shell open may be in flight per window (design §6): the shell queue replaces
    // queued opens, so a second open while one record is still loading would strand it.
    readonly property bool videoTaskOpenPending: {
        for (let index = 0; index < root.videoTabList.length; ++index) {
            if (root.videoTabList[index].loading)
                return true;
        }
        return false;
    }
    // Which tab the single edit session belongs to. With no host the historical
    // single-controller behaviour is preserved by returning -1 for both sides.
    readonly property int imageEditTabIndex: {
        if (!root.imageEditService || !root.imageEditService.active)
            return -1;
        const tabs = root.imageTabList;
        if (!tabs || tabs.length === 0)
            return -1;
        const label = String(root.imageEditService.sourceLabel || "");
        if (label.length === 0)
            return root.imageTabActiveIndex;
        for (let index = 0; index < tabs.length; ++index) {
            const tab = tabs[index];
            if (root.sameFilePath(label, String(tab.primaryPath || "")) || root.sameFilePath(label, String(tab.secondaryPath || "")))
                return index;
        }
        return -1;
    }
    readonly property int imageTabDirtyIndex: root.imageEditService && root.imageEditService.active && root.imageEditService.dirty ? root.imageEditTabIndex : -1
    readonly property string imageEditStartBlockedReason: {
        if (root.imageEditComparisonTask)
            return qsTr("此标签是固定的编辑对比快照，请切回原编辑任务继续修改。");
        if (!root.imageEditService || !root.imageEditService.active || !root.imageEditService.dirty)
            return "";
        if (root.imageEditTabIndex === root.imageTabActiveIndex)
            return "";
        return qsTr("另一个标签页有未保存的编辑副本：请先切回该标签页，另存副本或放弃修改。");
    }
    // qmllint disable unqualified
    // Step-3 image editing service from the composition root; the typeof guard keeps
    // lightweight QML-only harnesses valid (they simply see no edit tools).
    readonly property var imageEditService: typeof imageEdit !== "undefined" ? imageEdit : null
    readonly property var folderPairModel: typeof imageFolderPairs !== "undefined" ? imageFolderPairs : null
    // FLOW3 single-directory browsing cursor. It only remembers the neighbour list of the last
    // single image opened; every step still goes through performImageReview, so failure and
    // request-identity rules stay exactly the ones the direct open already uses.
    readonly property var imageFolderCursorModel: typeof imageFolderCursor !== "undefined" ? imageFolderCursor : null
    readonly property bool imageFolderBrowseActive: root.imageFolderCursorModel !== null && root.imageFolderCursorModel.count > 1
    readonly property string imageFolderBrowseText: root.imageFolderBrowseActive ? qsTr("%1 / %2").arg(root.imageFolderCursorModel.currentIndex).arg(root.imageFolderCursorModel.count) : ""
    // Replacing or closing the active image task ends the edit session, and the working copy
    // lives only in memory. A session that belongs to another tab is allowed to sit there while
    // this tab is replaced; only the active session's dirty state can refuse the action.
    readonly property string imageLeaveBlockedReason: (root.imageEditService && root.imageEditService.active && root.imageEditService.dirty && root.imageEditTabIndex === root.imageTabActiveIndex) ? qsTr("编辑副本有未保存修改：请先「另存副本…」或「放弃修改」，再切换图片。") : ""
    readonly property var videoFolderModel: typeof videoFolder !== "undefined" ? videoFolder : null
    property bool videoFolderSidebarVisible: false
    // videoFolderListVisible / videoFolderInset live with the other U01 layout properties
    // above: the guard-aware visibility and the token-driven stage width are one decision.
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
    // The explicit-reference flag is the real fact: without it referenceSourceIndex is the
    // canonical fallback, which is a timeline master and not a reference.
    readonly property bool differencePeekTargetValid: root.hasExplicitReference && root.referenceSourceIndex >= 0 && root.referenceSourceIndex < root.sourceCount
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
    readonly property var workspaceHelpEntries: [[ShortcutCatalog.openVideos, qsTr("打开文件")], [ShortcutCatalog.openVideoFolder, qsTr("打开文件夹")], [ShortcutCatalog.addVideo, qsTr("向当前对比添加视频")], [ShortcutCatalog.openImagePair, qsTr("打开图片对")], [ShortcutCatalog.addImage, qsTr("向当前图片对添加图片")], [ShortcutCatalog.compareImageFolders, qsTr("对比图片文件夹")], [ShortcutCatalog.closeCurrentTask, qsTr("关闭当前任务")], [ShortcutCatalog.nextImageTab, qsTr("下一个图片标签")], [ShortcutCatalog.previousImageTab, qsTr("上一个图片标签")], [ShortcutCatalog.nextVideoTab, qsTr("下一个视频标签")], [ShortcutCatalog.previousVideoTab, qsTr("上一个视频标签")], [ShortcutCatalog.captureIssue, qsTr("记录当前问题，可填备注")]]
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
    // FLOW1 image-restore state machine. The folder/pair open is asynchronous, so the machine
    // waits for the committed pair to match the record, opens the recorded folder row, and
    // only then replays the observation settings. A workspace switch cancels it so a late
    // result cannot land on another material.
    property var pendingImageIssueRestore: null
    property int pendingImageIssueRestoreStage: 0
    property bool pendingImageIssueRestoreApplied: false
    property bool pendingImageIssueRestoreRowOpened: false
    property bool imageIssueRestoreContinuing: false
    // Video task tabs (design §2.2/§4, slice-2 minimal skeleton). pendingVideoTaskOpen is
    // the loading record whose shell open is in flight; it is keyed by the shell intent id
    // captured at submission. Startup-origin opens are adopted at their terminal: the shell
    // owns their queue entry before QML ever sees the URLs.
    property var pendingVideoTaskOpen: null
    property var videoStartupOpenIntentIds: []
    property url pendingVideoAppendUrl: ""
    // Sidebar routing (design §5 rows 5-8): while an in-place disc switch is in flight,
    // the url of the material the ACTIVE single-video tab is switching to. Its successful
    // OpenSourcesIntent terminal rewrites that tab's record group (position not carried).
    property url pendingSidebarInPlaceUrl: ""
    // Minimal tab-switch machine (design §4): pause -> snapshot -> replace-open without the
    // displayed-time mapping -> seek the snapshot frame -> replay the view mode -> stay
    // paused. Every stage is bound to the session epoch plus the target task's URL list;
    // a terminal from any other session cancels the restore instead of landing on it.
    property var pendingVideoTaskSwitch: null
    property var pendingVideoTaskSwitchOpenCommandId: 0
    property int pendingVideoTaskSwitchOpenOutcome: -1
    property var pendingVideoTaskSwitchOpenSessionEpoch: 0
    property var pendingVideoTaskSwitchSessionEpoch: 0
    property var pendingVideoTaskSwitchSeekCommandId: 0
    // Stage-2 pair replay (design §4.5): the recorded pair is a command, so its terminal
    // is awaited before the presentation family replays; a refused or unverified pair is
    // a partial-restore note, never a blocker.
    property bool pendingVideoTaskSwitchContextApplied: false
    property var pendingVideoTaskSwitchPairCommandId: 0
    property bool pendingVideoTaskSwitchPairApplied: true
    property var pendingVideoTaskSwitchPartial: []
    property bool videoTaskSwitchContinuing: false
    // Identity of the task the switch machinery last adopted (urls + reference slot), not
    // its index: closing the active tab can slide a neighbour into the same numeric index
    // without any activeChanged, while a background close shifts indexes without switching.
    property string videoLastActivatedKey: ""
    readonly property bool videoTaskSwitchPending: pendingVideoTaskSwitch !== null
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
    // I01: playback-purpose opens autoplay only while the user keeps this preference on.
    readonly property bool autoplayOnOpen: preferences ? Boolean(preferences.autoplayOnOpen) : true
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
    readonly property bool imageHasContent: Boolean(root.activeImageController && (root.activeImageController.hasPrimary || root.activeImageController.hasSecondary))
    readonly property bool videoHasSession: sourceCount > 0
    readonly property bool activeTaskHasMedia: workspaceSession.imageActive ? (imageHasContent || imageTabCount > 0) : videoHasSession
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
    readonly property bool dualDifferenceMode: effectiveViewMode === ComparisonSurface.DualDifference
    readonly property bool analysisGridMode: effectiveViewMode === ComparisonSurface.AnalysisGrid
    readonly property bool differenceMode: effectiveViewMode === ComparisonSurface.Difference || analysisGridMode || dualDifferenceMode
    // D4 peek capability: the 差异定位 task (pure pair difference or GT-anchored dual
    // difference) is exactly where the "hold to see the GT original" button and the ` key
    // apply. AnalysisGrid is a layout flavour, not the difference task, so neither entry is
    // offered there - one shared condition, no hidden-but-triggerable states, no button that
    // promises a key the keyboard will not honour.
    readonly property bool peekDifferenceTask: effectiveViewMode === ComparisonSurface.Difference || dualDifferenceMode
    readonly property bool wipeMode: effectiveViewMode === ComparisonSurface.Wipe
    readonly property int threeUpViewMode: ComparisonSurface.ThreeUp

    // "任意比较对一步切换": the four full-group views show every participating source at
    // once, so none of them displays a single compared pair. RoiTriplet counts as a
    // full-group view for the same reason - its shared-ROI windows cover all sources, not
    // just the active pair.
    readonly property bool fullGroupView: effectiveViewMode === ComparisonSurface.ThreeUp || effectiveViewMode === ComparisonSurface.ReferenceFocus || effectiveViewMode === ComparisonSurface.DualDifference || effectiveViewMode === ComparisonSurface.RoiTriplet
    // The last two-path comparison mode the user actually had on screen (并排 / 分割线 /
    // 差异 / 分析网格); SideBySide until one of them is used. Tracked from the effective
    // mode so every entry point records the same fact - mode bar, context menu,
    // shortcuts, presets and restores alike.
    property int lastPairMode: ComparisonSurface.SideBySide
    onEffectiveViewModeChanged: {
        const mode = root.effectiveViewMode;
        if (mode === ComparisonSurface.SideBySide || mode === ComparisonSurface.Wipe || mode === ComparisonSurface.Difference || mode === ComparisonSurface.AnalysisGrid)
            root.lastPairMode = mode;
    }

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

    // V04 dual-difference caliber: the two rendered panes are the non-reference slots (in
    // ascending order, exactly like computeSurfacePanelLayout) each against the reference
    // slot. The QML projection describes these panes - not the active pair selector - so the
    // legend, the missing-frame overlay and the "converted" note all speak about the two
    // windows the user actually sees, under their one shared threshold/gain/metric/colour scale.
    readonly property int dualDifferenceReferenceSlot: referenceSourceIndex >= 0 ? referenceSourceIndex : 0
    readonly property var dualDifferencePaneSlots: {
        const slots = [];
        if (sourceCount === 3) {
            for (let slot = 0; slot < 3; ++slot) {
                if (slot !== dualDifferenceReferenceSlot)
                    slots.push(slot);
            }
        }
        return slots;
    }

    function dualPaneEdge(firstSlot, secondSlot) {
        for (const edge of differenceEdges) {
            const primary = Number(edge.primarySlot);
            const secondary = Number(edge.secondarySlot);
            if ((primary === firstSlot && secondary === secondSlot) || (primary === secondSlot && secondary === firstSlot))
                return edge;
        }
        return null;
    }

    readonly property var dualDifferencePaneEdges: {
        const edges = [];
        for (const slot of dualDifferencePaneSlots)
            edges.push(dualPaneEdge(slot, dualDifferenceReferenceSlot));
        return edges;
    }

    // One legend line for both panes: pane identity plus the per-dimension exactness combined
    // across the two pairs (a dimension is only "exact" when both panes are), then the shared
    // caliber note. Missing evidence stays "不可用/未知"; nothing is reported as exact or zero
    // without it.
    readonly property string dualDifferenceLegendText: {
        if (!dualDifferenceMode)
            return "";
        if (dualDifferencePaneSlots.length !== 2)
            return qsTr("双差异（无参考）");
        const paneLetter = slot => String.fromCharCode(65 + slot);
        const head = qsTr("双差异 %1↔GT · %2↔GT").arg(paneLetter(dualDifferencePaneSlots[0])).arg(paneLetter(dualDifferencePaneSlots[1]));
        const first = dualDifferencePaneEdges[0];
        const second = dualDifferencePaneEdges[1];
        if (!first || !second || Number(first.dimensionsAvailable) !== 1 || Number(second.dimensionsAvailable) !== 1)
            return head + qsTr(" · 比较语义不可用");
        const bothExact = field => Number(first[field]) === 1 && Number(second[field]) === 1;
        const parts = [head];
        parts.push(bothExact("temporalExact") ? qsTr("时间 索引及时间戳一致") : qsTr("时间 映射或时间戳不同"));
        parts.push(bothExact("spatialExact") ? qsTr("空间 原尺寸") : qsTr("空间 尺寸或几何不同"));
        parts.push(bothExact("pixelExact") ? qsTr("像素 数值审查路径（原码值）") : qsTr("像素 观看路径（已做显示转换，非原码值）"));
        parts.push(qsTr("两窗共用阈值/增益/色阶"));
        return parts.join(" · ");
    }

    readonly property bool dualDifferenceLegendExact: {
        if (!dualDifferenceMode || dualDifferencePaneEdges.length !== 2)
            return false;
        const first = dualDifferencePaneEdges[0];
        const second = dualDifferencePaneEdges[1];
        return Boolean(first) && Boolean(second) && Number(first.dimensionsAvailable) === 1 && Number(second.dimensionsAvailable) === 1 && Number(first.exactness) === 0 && Number(second.exactness) === 0;
    }

    // Combined "cannot correspond exactly" note across the two panes; empty when both pane
    // pairs correspond exactly. Duplicated reasons are named once.
    readonly property string dualDifferenceInexactDetail: {
        if (!dualDifferenceMode || dualDifferencePaneEdges.length !== 2)
            return "";
        const notes = [];
        for (const edge of dualDifferencePaneEdges) {
            const reason = edge ? String(edge.inexactReason) : "";
            if (reason.length > 0 && notes.indexOf(reason) < 0)
                notes.push(reason);
        }
        return notes.join(qsTr("；"));
    }

    readonly property string differenceUnavailableDetail: {
        if ((!differenceMode && !wipeMode) || currentFrame < 0)
            return "";

        const missing = [];
        const missingLabel = slot => sourceLabel(slot);

        if (dualDifferenceMode) {
            // The overlay names the panes the dual layout actually draws: each non-reference
            // slot against the reference. Reporting the active pair selector here could name a
            // pair the canvas is not showing (or hide a pane that is black).
            if (dualDifferencePaneSlots.length === 2) {
                for (const slot of dualDifferencePaneSlots) {
                    if (sourceMissing(slot))
                        missing.push(missingLabel(slot));
                }
                if (sourceMissing(dualDifferenceReferenceSlot))
                    missing.push(qsTr("参考（%1）").arg(sourceLabel(dualDifferenceReferenceSlot)));
                if (missing.length > 0)
                    return qsTr("无法对比第 %1 帧，因为 %2 缺失。").arg(Number(currentFrame) + 1).arg(missing.join(qsTr(" 和 ")));
                if (exactPlaneMode && (Number(dualDifferencePaneEdges[0] ? dualDifferencePaneEdges[0].exactness : 4) !== 0 || Number(dualDifferencePaneEdges[1] ? dualDifferencePaneEdges[1].exactness : 4) !== 0))
                    return qsTr("逐像素精确差异要求分辨率、像素格式、位深、色彩元数据一致，且两侧使用 ExactIndex 映射并具有相同时间戳。");
                return "";
            }
            return "";
        }

        if (sourceMissing(differenceFirstSlot))
            missing.push(missingLabel(differenceFirstSlot));

        if (sourceMissing(differenceSecondSlot))
            missing.push(missingLabel(differenceSecondSlot));

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
        // The rail speaks the active behaviour. The Contextual preference resolves per session,
        // so the effective policy - not the requested one - is what the user is watching.
        const policy = Number(controller ? controller.playbackContinuityPolicyEffective : 1);
        return policy === 0 ? qsTr("逐帧完整") : qsTr("流畅优先");
    }
    readonly property string playbackModeDetail: {
        const policy = Number(controller ? controller.playbackContinuityPolicyEffective : 1);
        return policy === 0 ? qsTr("每一帧都显示；负载高时播放会放慢") : qsTr("保持原速；解码跟不上时整组跳帧");
    }
    // U03: the waiting indicator names the work in progress. A held reverse walk (E04) is
    // preparing earlier frames through the reverse GOP window, which reads differently from
    // an ordinary latest-frame fetch; the underlying fields stay untouched.
    readonly property string framePendingLabel: messageCatalog.framePendingText(controller ? controller.jogActive : false, controller ? controller.jogDirection : 0)
    // U03: the one mutually exclusive answer to what the video stage is showing. Pure over
    // the same booleans the overlay surfaces read (see StageStates.js); the five-state
    // distinction lives here so tests can pin it and the surfaces cannot drift from it.
    // The per-source missing flags default to true for slots the session does not fill, so
    // only sources that actually exist may speak.
    readonly property string stageStateId: StageStates.resolveStageState(videoHasSession, graphicsReady, busy, currentFrame >= 0, hasErrors, showFramePending, (sourceCount > 0 && sourceAMissing) || (sourceCount > 1 && sourceBMissing) || (sourceCount > 2 && sourceCMissing))
    readonly property string sourceRateText: {
        const rate = controller ? String(controller.rationalFrameRate || "") : "";
        return rate.length > 0 ? qsTr("源 %1 · 屏刷新无法逐帧呈现时以逐帧审查核对单帧").arg(rate) : "";
    }
    readonly property real frameProgress: currentFrame >= 0 && totalFrames > 1 ? Math.max(0, Math.min(1, Number(currentFrame) / (Number(totalFrames) - 1))) : 0
    readonly property real timelineProgress: timelineDragging && timelinePreviewFrame >= 0 && totalFrames > 1 ? Number(timelinePreviewFrame) / (Number(totalFrames) - 1) : frameProgress
    readonly property bool timelineEnabled: graphicsReady && !busy && Boolean(controller && controller.canFirst) && totalFrames > 0
    readonly property bool anyMenuOpen: Boolean(applicationMenuBar && applicationMenuBar.anyMenuOpen) || Boolean(sourceBar && sourceBar.anyMenuOpen) || Boolean(viewerContextMenu && viewerContextMenu.anyMenuOpen) || Boolean(videoFolderSidebar && videoFolderSidebar.anyMenuOpen)

    // QML modal popups own the keyboard while they are open. Each popup also carries the
    // popupInputContext marker the focus walk below reads, but a visibility-driven gate covers
    // the frames before focus has landed inside the popup and keeps the ownership honest as new
    // popups are added. Native dialogs need no entry: the OS takes focus for them.
    readonly property bool anyModalPopupOpen: Boolean(clipExportDialog && clipExportDialog.dialogVisible) || Boolean(sampleExportDialog && sampleExportDialog.dialogVisible) || fileAssociationDialog.dialogVisible || unsavedEditDialog.visible || Boolean(imageWorkspace && imageWorkspace.anyEditDialogOpen)

    // Modal/input routing is intentionally computed in one place. Native image dialogs are

    // included even though they are not Popups, so a selector opened from the video workspace

    // can never let media shortcuts reach the hidden video session.

    readonly property int inputContext: Boolean(reviewInputDialogs && reviewInputDialogs.modalVisible) || anchorDialog.visible || shortcutHelp.visible || issueNoteDialog.visible || imageSingleDialog.visible || imageAddDialog.visible || imagePairDialog.visible || imageReplacePrimaryDialog.visible || imageReplaceSecondaryDialog.visible || imageFolderLeftDialog.visible || imageFolderRightDialog.visible || anyModalPopupOpen ? 3 : (anyMenuOpen || focusIsPopup(root.activeFocusItem) ? 2 : (focusIsTextEditing(root.activeFocusItem) ? 1 : 0))
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

    // Whole-timeline duration in the same timecode vocabulary (workspace redesign §8 layer 1);
    // evaluated at the last frame, and empty while no session exists so the flank only states
    // the frame total it can honestly report.
    readonly property string totalTimecode: controller && root.totalFrames > 0 ? controller.timecodeForFrame(root.totalFrames - 1, dropFrameTimecode) : ""

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
    // Normalized shared-ROI rect (V04 nearby-frames strip). The surface owns the values; the
    // card only consumes them, so the strip and the canvas can never disagree about the region.
    readonly property real surfaceRoiLeft: viewportFrame ? viewportFrame.surfaceRoiLeft : 0
    readonly property real surfaceRoiTop: viewportFrame ? viewportFrame.surfaceRoiTop : 0
    readonly property real surfaceRoiRight: viewportFrame ? viewportFrame.surfaceRoiRight : 1
    readonly property real surfaceRoiBottom: viewportFrame ? viewportFrame.surfaceRoiBottom : 1

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

    // Only an unsaved image working copy needs a close guard. Review state and background
    // work retain their existing persistence and host shutdown paths.
    onClosing: close => {
        if (root.imageEditService && root.imageEditService.active) {
            root.imageEditService.endStroke();
            root.imageEditService.endAnnotationDrag();
        }
        if (root.imageEditService && root.imageEditService.active && root.imageEditService.dirty) {
            close.accepted = false;
            root.showUnsavedEditDialog(root.imageEditTabIndex, true);
            return;
        }
        // C1: the debounce can still be pending when the window goes away, and the last thing the
        // user did was move the playhead. Record synchronously so the position survives the close.
        resumeRecordTimer.stop();
        root.recordResumePosition();
        // E04: a held frame-step key dies with the window; end the walk so the coordinator's
        // bounded drain cannot outlive the interaction.
        reviewShortcuts.cancelJog();
    }

    // The directory can change while the window is hidden (a delete or rename in Explorer).
    // Re-scanning refreshes both the neighbour list and the "n / N" label without a file watcher.
    // An empty cursor means the current task is a pair or folder session, which must not gain a
    // directory list here.
    function refreshImageFolderCursor() {
        if (!root.activeImageController || !root.imageFolderCursorModel)
            return false;
        if (root.imageFolderCursorModel.count <= 0)
            return false;
        if (!root.activeImageController.hasPrimary || root.activeImageController.hasSecondary)
            return false;
        root.imageFolderCursorModel.setCurrentFile(root.activeImageController.primaryPath);
        return root.imageFolderCursorModel.count > 0;
    }
    // A tab switch owns its own cursor hand-off: a single-image tab re-scans its directory,
    // while a pair tab must drop the previous single-image neighbour list.
    function armImageFolderCursorForActiveTab() {
        if (!root.activeImageController || !root.imageFolderCursorModel)
            return false;
        if (!root.activeImageController.hasPrimary || root.activeImageController.hasSecondary) {
            root.imageFolderCursorModel.clear();
            return false;
        }
        root.imageFolderCursorModel.setCurrentFile(root.activeImageController.primaryPath);
        return root.imageFolderCursorModel.count > 0;
    }
    onActiveChanged: {
        if (active)
            root.refreshImageFolderCursor();
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

                // A chrome-time action: the immersive HUD only renders while the chrome is
                // hidden, so the confirmation must ride the channel that is actually visible.
                showActionFeedback(qsTr("区间起点 · 第 %1 帧").arg(frame + 1));
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

                showActionFeedback(qsTr("区间终点 · 第 %1 帧").arg(frame + 1));
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
            showActionFeedback(qsTr("当前视频或所选区间暂不可导出，请检查素材和区间起点、终点。"));

            return false;
        }

        clipExportDialogLoader.active = true;

        if (!clipExportDialog)
            return false;

        clipExportDialog.open();

        return true;
    }

    // E01: opens the training-sample triplet export dialog for the given frame (or the current
    // playhead). Validates that the frame is an exportable prediction with its inputs resolvable.
    function openSampleExportDialog(frame) {
        if (!sampleExportModel)
            return false;

        const target = frame !== undefined && frame >= 0 ? frame : Number(root.currentFrame);
        if (!sampleExportModel.canExportAtFrame(target)) {
            showActionFeedback(qsTr("当前帧不是插帧协议中的生成帧，或未载入插帧协议。"));
            return false;
        }

        sampleExportDialogLoader.active = true;

        if (!sampleExportDialog)
            return false;

        sampleExportDialog.openWithFrame(target);
        return true;
    }

    // P03: opens the protocol-manifest picker. The controller owns validation; a bad manifest is
    // reported through the transport's protocol badge, not a dead-end dialog here.
    function openProtocolLoadDialog() {
        if (!protocolModel)
            return false;

        protocolLoadDialog.open();

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

    // E3: flip the back-and-forth walk intent on the CURRENT loop interval. The interval and
    // loop stay untouched; only pingPong flips, so the coordinator keeps the same closed range.
    function toggleRangePingPong() {
        if (!controller) {
            showIntentMessage(qsTr("当前无活动会话。"));

            return false;
        }
        if (!controller.togglePlaybackRangePingPong()) {
            showIntentMessage(qsTr("往返需要完整的入出区间（设置后自动开启循环）。"));

            return false;
        }
        showIntentMessage(controller.playbackRangePingPong ? qsTr("往返循环：到出点后倒放返回") : qsTr("单向区间循环"));
        return true;
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
            openImageInNewTab(action.urls);

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
            if (root.focusIsTaskTab(root.activeFocusItem))
                return;
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

    // Keyboard flavour of hold-to-peek: the backtick is a real hold (D4) - press shows the
    // raw reference view, release restores the difference view. The hold never touches the
    // lock; a locked peek simply stays on when the hold ends. Shift+backtick remains the
    // lock toggle, and the mouse button keeps its transient press/release semantics; a
    // release never defeats an active lock (see onDifferencePeekChanged). The locked state
    // also stays on screen through the viewport's peek indicator, not only as a transient
    // HUD line.
    function setDifferencePeekHeld(held) {
        if (held) {
            if (!root.differencePeekTargetValid)
                return;
            differencePeekActive = true;
        } else {
            differencePeekActive = differencePeekLocked;
        }
    }

    function toggleDifferencePeek(lock) {
        if (!root.differencePeekTargetValid) {
            showActionFeedback(qsTr("当前会话没有可直看的参考（GT）源"));
            return;
        }
        if (lock) {
            differencePeekLocked = !differencePeekLocked;
            differencePeekActive = differencePeekLocked;
            showActionFeedback(differencePeekLocked ? qsTr("已锁定参考原图直看") : qsTr("已解锁参考原图"));
        } else if (differencePeekLocked) {
            differencePeekLocked = false;
            differencePeekActive = false;
            showActionFeedback(qsTr("已解锁，恢复差异视图"));
        } else {
            differencePeekActive = !differencePeekActive;
            showActionFeedback(differencePeekActive ? qsTr("查看参考原图") : qsTr("恢复差异视图"));
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
        if (root.pendingImageRequestId > 0 && root.activeImageController) {
            root.activeImageController.cancelOpenRequest(root.pendingImageRequestId);

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

        const target = root.activeImageController;

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

        // A committed single image becomes the directory-cursor position; a committed pair is a
        // different task, so the single-directory cursor must stop offering neighbours.
        if (root.imageFolderCursorModel) {
            if (kind === "single")
                root.imageFolderCursorModel.setCurrentFile(target.primaryPath);
            else
                root.imageFolderCursorModel.clear();
        }

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

        root.continueImageIssueRestore();
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

        root.continueImageIssueRestore();
    }

    // Pure view switch used after a commit or a close; it owns no media command by itself so

    // an asynchronous open/close cannot be disturbed by a redundant pause command.

    function showWorkspace(media) {
        const target = Number(media) === workspaceSession.imageMedia ? workspaceSession.imageMedia : workspaceSession.videoMedia;

        if (target !== workspaceSession.imageMedia)
            root.cancelPendingImageIssueRestore();

        workspaceSession.pendingMedia = -1;

        root.workspaceMode = target;

        focusActiveWorkspace();

        return true;
    }

    // Explicit workspace switch from the View menu. Leaving video or returning to it always

    // lands on an explicit paused state; returning never resumes playback implicitly.

    function activateWorkspace(media) {
        ensureVideoPaused();

        if (Number(media) !== workspaceSession.imageMedia && root.videoTaskHost) {
            // Leaving the video workspace keeps the runtime alive: refresh the active
            // task's snapshot so the recorded position matches what the user actually
            // left, and drop an in-flight tab restore instead of yanking the user back.
            root.snapshotVideoSessionTask();
            root.cancelPendingVideoTaskSwitch();
        }

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

        if (target === workspaceSession.imageMedia) {
            ensureVideoPaused();
            // Every image commit ends the previous directory sequence. A single open re-arms the
            // cursor immediately after this call; a pair or folder session leaves it empty so
            // the arrow keys cannot walk into an unrelated folder.
            if (root.imageFolderCursorModel)
                root.imageFolderCursorModel.clear();
            // A committed image also replaces the edit session's source. A clean session has
            // nothing to lose and ends here; a dirty one is refused before the open starts, so
            // edit mode can never sit on top of a different image.
            if (root.imageEditService && root.imageEditService.active && !root.imageEditService.dirty)
                root.imageEditService.endSession();
        }

        return showWorkspace(target);
    }

    // Ctrl+W / File-menu close. Only the active task is affected; a retained task in the other

    // workspace stays alive and becomes visible if it has committed content.

    function closeCurrentTask() {
        if (workspaceSession.imageActive) {
            if (root.imageTaskHost && root.imageTabCount > 0) {
                const tabIndex = root.imageTabActiveIndex;
                if (tabIndex < 0)
                    return false;
                if (root.imageTabOpenPending) {
                    root.showIntentMessage(qsTr("正在打开图片，请稍候再关闭标签。"));
                    return false;
                }
                if (root.imageTabDirtyIndex === tabIndex) {
                    root.showUnsavedEditDialog(tabIndex, false, true);
                    return false;
                }
                if (root.imageEditService && root.imageEditService.active && root.imageEditTabIndex === tabIndex)
                    root.imageEditService.endSession();
                return Boolean(root.imageTaskHost.closeActive());
            }
            const imageTarget = root.activeImageController;

            if (!imageTarget || (!root.imageHasContent && root.pendingImageRequestId <= 0))
                return false;

            if (root.imageTaskSwitchBlocked())
                return false;

            root.pendingImageRequestId = -1;

            root.pendingImageOpenKind = "";

            root.pendingFolderPairRow = -1;

            if (imageTarget)
                imageTarget.closeAll();

            // Closing the task drops the directory cursor with it; otherwise the arrow keys
            // would reopen an image the user just closed.
            if (root.imageFolderCursorModel)
                root.imageFolderCursorModel.clear();

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

        // Video task tabs (design §6/§8): with more than one tab, Ctrl+W closes the
        // current task's tab; the host selects the neighbour and its restore rebuilds the
        // session. Closing the last tab goes through allTasksClosed, which keeps the
        // legacy whole-session close and its feedback.
        if (root.videoTaskHost && root.videoTabCount > 0) {
            if (root.videoTaskOpenPending) {
                root.showIntentMessage(qsTr("正在打开视频，请稍候再关闭标签。"));
                return false;
            }
            const closing = root.videoTabActiveIndex;
            if (closing >= 0 && closing < root.videoTabList.length && root.videoTabList[closing].restoring) {
                root.showIntentMessage(qsTr("正在恢复视频标签，请稍候再关闭。"));
                return false;
            }
            if (!root.videoTaskHost.closeActive())
                return false;
            // Same rationale as closeVideoTab: the neighbour may keep the numeric index,
            // so the identity-keyed adoption runs once more, idempotently.
            root.handleVideoTaskActiveChanged();
            return true;
        }

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
        if (!root.routeSidebarVideoSubmit(url, () => videoFolderModel.openAt(Number(row)))) {
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
        if (!openImageInNewTab([url])) {
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
        const entry = videoFolderModel.recentFiles[row];
        const url = entry && entry.fileUrl ? entry.fileUrl : "";
        cancelWorkspaceOpen();
        workspaceSession.beginOpen(workspaceSession.videoMedia);
        if (!root.routeSidebarVideoSubmit(url, () => videoFolderModel.openRecent(Number(row)))) {
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
            if (!root.activeImageController)
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
        if (!root.routeSidebarVideoSubmit(url, () => videoFolderModel.openFile(url))) {
            workspaceSession.cancelOpen();
            return false;
        }
        pendingNewReviewWantsThreeUp = false;
        return true;
    }

    // Row 7: the sidebar context menu's 「打开为新单视频任务」 is an explicit new task,
    // so it skips the in-place disc-switch judgement entirely. Without the task host it
    // degrades to the legacy plain open.
    function openSidebarVideoInNewTab(url) {
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
        if (!root.armSidebarAdoptOpen(() => videoFolderModel.openFile(url))) {
            workspaceSession.cancelOpen();
            return false;
        }
        pendingNewReviewWantsThreeUp = false;
        return true;
    }

    // Sidebar video routing (design §5 rows 5-8). With a single-video task active whose
    // session still shows that task, the sidebar (and folder stepping) switch the disc
    // IN PLACE: the record's group is rewritten at the successful open terminal, the
    // state snapshot is reset (the new material has nothing to restore) and the folder
    // model's own autoplay chain is untouched. Every other shape (comparison active, no
    // task, foreign session) opens a NEW task tab by adopting the intent's terminal -
    // the same adoption the startup opens use.
    function routeSidebarVideoSubmit(url, submit) {
        if (!root.videoTaskHost || !root.shell || !root.controller)
            return submit();
        const active = root.videoTabActiveIndex;
        const tabs = root.videoTabList;
        const activeTab = active >= 0 && active < tabs.length ? tabs[active] : null;
        const singleActive = activeTab && !activeTab.loading && !activeTab.restoring && (activeTab.urls || []).length === 1 && root.videoSessionTaskIndex() === active;
        if (singleActive) {
            root.pendingSidebarInPlaceUrl = url;
            const accepted = submit();
            if (!accepted)
                root.pendingSidebarInPlaceUrl = "";
            return accepted;
        }
        return root.armSidebarAdoptOpen(submit);
    }

    // Registers the shell intent a sidebar submit just created for terminal adoption
    // (new-tab shape); without the host it is a plain legacy submit.
    function armSidebarAdoptOpen(submit) {
        if (!root.videoTaskHost || !root.shell) {
            return submit();
        }
        const before = root.shellIntentIds();
        const accepted = submit();
        if (accepted) {
            const id = root.firstNewShellIntentId(before);
            if (id > 0 && root.videoStartupOpenIntentIds.indexOf(id) < 0)
                root.videoStartupOpenIntentIds = root.videoStartupOpenIntentIds.concat([id]);
        }
        return accepted;
    }

    function cancelSidebarComparison() {
        if (!pendingSidebarComparison)
            return;
        pendingSidebarComparison = false;
        if (shell && shell.sidebarAppendStaged)
            shell.clearStagedSources();
        pendingComparisonPreservesPosition = false;
        pendingNewReviewWantsThreeUp = false;
        pendingVideoAppendUrl = "";
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

    // The merged 打开文件 entry: one dialog, both media kinds. The pending-workspace state is
    // only begun once the selection's kind is known, so a cancelled dialog leaves nothing open.
    function requestOpenMediaFiles() {
        ensureReviewInputDialogs().openMediaFiles();

        return true;
    }

    function openMediaFilesSelection(urls) {
        const files = [];

        for (let i = 0; urls && i < urls.length; ++i) {
            if (urls[i] && urls[i].toString().length > 0)
                files.push(urls[i]);
        }

        if (files.length === 0)
            return true;

        let images = 0;

        for (let i = 0; i < files.length; ++i) {
            if (videoFolderModel && videoFolderModel.isImageUrl(files[i]))
                ++images;
        }

        if (images > 0 && images < files.length) {
            showActionFeedback(qsTr("视频与图片不能混合打开，请分两次选择。"));

            return false;
        }

        if (images === 0) {
            workspaceSession.beginOpen(workspaceSession.videoMedia);

            return openNewReviewUrls(files);
        }

        workspaceSession.beginOpen(workspaceSession.imageMedia);

        if (files.length === 1)
            return openImageInNewTab([files[0]]);

        return performImagePairSelection(files);
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
        if (!root.canChangeImageSources())
            return false;
        workspaceSession.beginOpen(workspaceSession.imageMedia);

        imageAddDialog.open();

        return true;
    }

    function requestImageSwapSides() {
        if (!root.canChangeImageSources())
            return false;
        const target = root.activeImageController;
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
        if (!root.canChangeImageSources())
            return false;
        const target = root.activeImageController;
        if (!target || !target.hasPrimary)
            return false;
        // In-place slot replace: no workspace beginOpen — failure must not tear down
        // the already-committed image workspace.
        imageReplacePrimaryDialog.open();
        return true;
    }

    function requestImageReplaceSecondary() {
        if (!root.canChangeImageSources())
            return false;
        const target = root.activeImageController;
        if (!target || !target.hasPrimary)
            return false;
        imageReplaceSecondaryDialog.open();
        return true;
    }

    function performImageSideReplace(url, side) {
        if (!root.canChangeImageSources())
            return false;
        const target = root.activeImageController;
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
        if (root.imageTaskSwitchBlocked())
            return false;

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

            // Video task tabs (design §5 row 1): the host decides new tab versus focus;
            // openRequested submits through this same staged chain.
            if (root.videoTaskHost)
                return root.openVideoTask(normalizedUrls, 0, "Review");

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
        if (root.imageWorkspaceActive && root.imageEditComparisonTask) {
            root.showIntentMessage(qsTr("临时编辑快照不能跨重启恢复；请先另存编辑副本，再打开文件记录问题。"));
            return false;
        }
        // M freezes the observation the moment it is pressed: the record (frame, sources,
        // ROI, zoom, pair) is captured now, so playback advancing while the note is typed can
        // no longer shift what was recorded. The dialog only attaches text to that record.
        if (!root.issueLogModel)
            return false;

        root.issueLogModel.setWorkspaceMode(root.imageWorkspaceActive ? "image" : "video");

        // FLOW1: the background choice is QML view state; hand it to the capture so restore
        // can replay it. Outside the image workspace it stays "not captured" (-1).
        root.issueLogModel.setImageBackgroundMode(root.imageWorkspaceActive && root.imageWorkspace ? root.imageWorkspace.backgroundMode : -1);

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

    function openNearbyFramesCard(index) {
        if (!root.issueLogModel || index < 0 || index >= root.issueLogModel.count)
            return false;

        const issue = root.issueLogModel.issueAt(index);
        if (!issue || !issue.hasInterval)
            return false;

        root.activeDiagnosticIssueIndex = index;
        root.nearbyFramesCardVisible = true;
        root.issueLogModel.restoreIssue(index);
        return true;
    }

    function playIntervalRange(inF, outF) {
        if (!root.controller || inF < 0 || outF < inF)
            return false;

        if (root.shell) {
            root.shell.setRangeIn(inF, Number(root.controller.mediaTimeForFrame(inF)));
            root.shell.setRangeOut(outF, Number(root.controller.mediaTimeForFrame(outF)));
            root.shell.setRangePlaybackState(true, Number(root.currentFrame) !== inF);
            root.shell.setRangeStartPending(false);
        }
        return root.controller.playRange(inF, outF);
    }

    // Step-2 comparison export: the provenance names the compared sources (GT and
    // predictions), the observation context and a timestamp. It travels in the saved PNG's
    // text metadata, never in the pixels, so the exported image stays the bare comparison
    // picture. The capture itself is the presented display result, cropped to the comparison
    // picture — never original code values.
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
        // FLOW2 display contract: a saved/copied comparison is the screen result, not source
        // pixels and not a colour-managed export. Every shared capture carries that statement.
        lines.push(qsTr("显示结果 · 屏幕像素 · 未色彩管理；非源文件原始码值"));
        lines.push(qsTr("CompareStation · %1").arg(Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm")));
        return lines;
    }

    function copyComparisonImage() {
        if (!comparisonExportService) {
            showActionFeedback(qsTr("复制对比图不可用"));
            return false;
        }
        const ok = Boolean(comparisonExportService.copyComparison(viewportFrame.comparisonPicture));
        showActionFeedback(ok ? qsTr("已复制对比图") : qsTr("复制失败：%1").arg(comparisonExportService.lastStatus));
        return ok;
    }

    function saveComparisonImage() {
        if (!comparisonExportService) {
            showActionFeedback(qsTr("另存为对比图不可用"));
            return false;
        }
        comparisonSaveDialog.currentFile = root.suggestedComparisonFileName();
        comparisonSaveDialog.open();
        return true;
    }

    // Default save name: the compared video's base name plus the current frame number, so a
    // folder of captures stays sortable and traceable back to the source. The source extension
    // is dropped so the result reads "name_123.png", not "name.mp4_123.png".
    function suggestedComparisonFileName() {
        const raw = (sourceAName && sourceAName.length > 0) ? sourceAName : qsTr("对比图");
        const dot = raw.lastIndexOf(".");
        const slash = Math.max(raw.lastIndexOf("/"), raw.lastIndexOf("\\"));
        const base = (dot > slash + 1) ? raw.substring(0, dot) : raw;
        const frame = currentFrame >= 0 ? String(currentFrame + 1) : qsTr("无帧");
        // Characters Windows rejects in a file name would make the save fail outright.
        const safe = (base + "_" + frame).replace(/[<>:"\/\\|?*]/g, "_");
        return safe + ".png";
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

        // FLOW1 image-pair restore: reopen the recorded material first, then wait for that
        // pair to commit, open the recorded folder row, and only then replay the observation
        // settings. Nothing is replayed onto whatever happens to be open.
        root.cancelPendingImageIssueRestore();

        const hasFolders = Boolean(payload.leftFolder && payload.rightFolder);
        const hasPaths = Boolean(payload.leftPath && payload.rightPath);

        if (!hasFolders && !hasPaths) {
            root.showIntentMessage(qsTr("部分恢复：记录没有可重开的图片素材"));

            return false;
        }

        root.pendingImageIssueRestore = payload;
        root.pendingImageIssueRestoreStage = 1;
        root.pendingImageIssueRestoreApplied = false;
        root.pendingImageIssueRestoreRowOpened = false;

        if (hasFolders) {
            root.imageFolderLeftUrl = "file:///" + String(payload.leftFolder).replace(/\\/g, "/");
            root.imageFolderRightUrl = "file:///" + String(payload.rightFolder).replace(/\\/g, "/");
            root.loadFolderComparison();
        } else {
            const leftUrl = "file:///" + String(payload.leftPath).replace(/\\/g, "/");
            const rightUrl = "file:///" + String(payload.rightPath).replace(/\\/g, "/");
            root.performImageReview([leftUrl, rightUrl]);
        }

        imageIssueRestoreDeadline.restart();
        root.continueImageIssueRestore();

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

        // E1: wipe state replay has three distinguishable states, matching what the
        // repository round-trips - a valid pair (orientation 0/1, ratio in [0,1]) replays
        // exactly; the invalid sentinel (-2, the record's stored pair was rejected on load)
        // reports the rejection instead of quietly looking legacy; the legacy sentinel (-1,
        // no fields) keeps the current split. The ratio check is also strict here so a
        // tampered file cannot smuggle an out-of-range value through the restore.
        const restoreWipeOrientation = payload.wipeOrientation !== undefined ? Number(payload.wipeOrientation) : -1;
        const restoreWipePosition = payload.wipePosition !== undefined ? Number(payload.wipePosition) : -1;
        if (payload.viewMode === 5 && restoreWipeOrientation !== -1) {
            if (restoreWipeOrientation === -2) {
                root.showIntentMessage(qsTr("部分恢复：记录中的分割线状态无效，已保持当前切分"));
            } else if ((restoreWipeOrientation === 0 || restoreWipeOrientation === 1) && Number.isFinite(restoreWipePosition) && restoreWipePosition >= 0 && restoreWipePosition <= 1) {
                viewportFrame.setWipeOrientation(restoreWipeOrientation);
                root.wipePosition = restoreWipePosition;
            } else {
                root.showIntentMessage(qsTr("部分恢复：记录中的分割线状态无效，已保持当前切分"));
            }
        } else if (payload.viewMode === 5) {
            root.showIntentMessage(qsTr("部分恢复：记录未含分割线方向/位置，保持当前切分"));
        }

        // A recorded pair the session would not take is a partial restore: the files, mode, frame
        // and viewport are back, but the comparison pair is not. Saying so is the difference
        // between "restored" and "looks restored".
        if (payload.differenceEdge !== undefined && !root.pendingIssueRestorePairApplied)
            root.showIntentMessage(qsTr("部分恢复：记录中的比较对未能应用"));

        root.cancelPendingIssueRestore();
        // Slice-4 adoption: the restore machine has finished rebuilding the session, so
        // the task records must describe it again (matching record adopted in place,
        // otherwise the active record rewritten - never a new tab). Cancelled, failed
        // or timed-out restores never reach this point and leave records untouched.
        root.syncActiveVideoTaskWithSession();
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

    Timer {
        id: imageIssueRestoreDeadline

        objectName: "imageIssueRestoreDeadline"
        interval: 15000
        onTriggered: root.abandonPendingImageIssueRestore(qsTr("恢复超时：未能确认记录中的图片与观察状态"))
    }

    // Video tab-switch restore deadline (decision a): one 15 s bound for the whole
    // pause -> reopen -> seek -> mode chain, mirroring the issue-restore precedent.
    Timer {
        id: videoTaskSwitchDeadline

        objectName: "videoTaskSwitchDeadline"
        interval: 15000
        onTriggered: root.abandonPendingVideoTaskSwitch(qsTr("恢复超时：未能确认视频标签的观察位置"))
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

    // FLOW1: staged continuation of an image issue restore. It waits for the recorded pair to
    // commit, opens the recorded folder row, then replays the captured observation settings once.
    function continueImageIssueRestore() {
        if (root.imageIssueRestoreContinuing)
            return;

        root.imageIssueRestoreContinuing = true;

        try {
            root.advanceImageIssueRestore();
        } finally {
            root.imageIssueRestoreContinuing = false;
        }
    }

    function advanceImageIssueRestore() {
        const payload = root.pendingImageIssueRestore;

        if (!payload)
            return;

        if (!root.imageWorkspaceActive || !root.activeImageController) {
            root.abandonPendingImageIssueRestore(qsTr("恢复已取消：工作区已切换"));

            return;
        }

        const target = root.activeImageController;
        const pairModel = root.folderPairModel;
        const recordedRow = Number(payload.rowIndex);
        const leftPath = String(payload.leftPath || "");
        const rightPath = String(payload.rightPath || "");
        const committedMatches = Boolean(target.hasPair && (leftPath.length === 0 || root.sameFilePath(target.primaryPath, leftPath)) && (rightPath.length === 0 || root.sameFilePath(target.secondaryPath, rightPath)));

        // Stage 1: make the committed pair the recorded one. Folder sessions open their first
        // complete row first; move to the recorded row once the scan has produced it. A row
        // that never opens, or a pair that does not match, is reported instead of replayed.
        if (!committedMatches) {
            if (target.openPending || (pairModel && pairModel.openPending) || root.pendingFolderPairRow >= 0)
                return;

            if (pairModel && pairModel.pairCount > 0 && recordedRow >= 0 && !root.pendingImageIssueRestoreRowOpened) {
                root.pendingImageIssueRestoreRowOpened = true;
                root.pendingFolderPairRow = recordedRow;

                if (!pairModel.openPairAt(recordedRow)) {
                    root.abandonPendingImageIssueRestore(qsTr("恢复失败：无法打开记录中的文件夹行"));

                    return;
                }

                return;
            }

            if (!root.pendingImageIssueRestoreRowOpened) {
                root.abandonPendingImageIssueRestore(qsTr("恢复失败：打开的图片与记录不一致"));
            }

            return;
        }

        if (root.pendingImageIssueRestoreApplied)
            return;

        root.pendingImageIssueRestoreApplied = true;

        const missing = [];
        const compareMode = Number(payload.compareMode);

        if (Number.isInteger(compareMode) && compareMode >= 0 && compareMode <= 7)
            target.compareMode = compareMode;
        else
            missing.push(qsTr("比较模式"));

        const zoom = Number(payload.zoom);
        const panX = Number(payload.panX);
        const panY = Number(payload.panY);

        if (Number.isFinite(zoom) && zoom > 0 && Number.isFinite(panX) && Number.isFinite(panY))
            target.restoreObservationPosition(zoom, panX, panY);
        else
            missing.push(qsTr("缩放和平移"));

        const viewMode = Number(payload.imageViewMode);

        if (Number.isInteger(viewMode) && viewMode >= 0 && viewMode <= 2)
            target.viewMode = viewMode;
        else
            missing.push(qsTr("通道观察"));

        const background = Number(payload.imageBackgroundMode);

        if (root.imageWorkspace && Number.isInteger(background) && background >= 0 && background <= 3)
            root.imageWorkspace.backgroundMode = background;
        else
            missing.push(qsTr("背景"));

        const gain = Number(payload.imageDiffGain);

        if (Number.isInteger(gain) && gain >= 1 && gain <= 16)
            target.diffGain = gain;
        else
            missing.push(qsTr("差异增益"));

        if (typeof payload.resampleAllowed === "boolean")
            target.resampleAllowed = payload.resampleAllowed;
        else
            missing.push(qsTr("重采样"));

        const wipeOrientation = Number(payload.wipeOrientation);
        const wipePosition = Number(payload.wipePosition);

        if ((wipeOrientation === 0 || wipeOrientation === 1) && Number.isFinite(wipePosition) && wipePosition >= 0 && wipePosition <= 1) {
            target.wipeOrientation = wipeOrientation;
            target.wipePosition = wipePosition;
        } else if (compareMode === 5) {
            missing.push(qsTr("分割方向与比例"));
        }

        const alphaEnabled = Number(payload.imageAlphaFilterEnabled);
        const alphaThreshold = Number(payload.imageAlphaThreshold);

        if (alphaEnabled === 0 || alphaEnabled === 1) {
            target.alphaFilterEnabled = alphaEnabled === 1;

            if (Number.isInteger(alphaThreshold) && alphaThreshold >= 0 && alphaThreshold <= 255)
                target.alphaThreshold = alphaThreshold;
            else
                missing.push(qsTr("Alpha 阈值"));
        } else {
            missing.push(qsTr("Alpha 过滤"));
        }

        root.cancelPendingImageIssueRestore();

        if (missing.length > 0)
            root.showIntentMessage(qsTr("部分恢复：%1 未恢复").arg(missing.join(qsTr("、"))));
        else
            root.showIntentMessage(qsTr("已恢复记录中的图片与观察状态"));
    }

    function abandonPendingImageIssueRestore(message) {
        root.cancelPendingImageIssueRestore();

        root.showIntentMessage(message);
    }

    function cancelPendingImageIssueRestore() {
        root.pendingImageIssueRestore = null;
        root.pendingImageIssueRestoreStage = 0;
        root.pendingImageIssueRestoreApplied = false;
        root.pendingImageIssueRestoreRowOpened = false;
        imageIssueRestoreDeadline.stop();
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

    // One refusal for every path that would replace or close the image task. Returns true when
    // the action was blocked (and already explained), so callers only have to early-return.
    function imageTaskSwitchBlocked() {
        const reason = root.imageLeaveBlockedReason;

        if (reason.length === 0)
            return false;

        root.showIntentMessage(reason);

        return true;
    }
    function performImageReview(normalizedUrls) {
        const target = root.activeImageController;

        if (!target)
            return false;
        // A restore or an internal in-place open can start with no image task at all. It must
        // still own a tab, or the resident workspace would commit content the tab strip and
        // host do not know about.
        if (root.imageTaskHost && root.imageTabCount <= 0)
            return root.openImageInNewTab(normalizedUrls);
        if (root.imageTaskSwitchBlocked())
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
    // New-tab open used by the file dialogs, drops, the sidebar and external opens. A task in
    // the middle of an in-place open owns pendingImageRequestId; switching active tabs then
    // would apply that commit to the wrong task, so the brief wait is explicit.
    function openImageInNewTab(normalizedUrls) {
        if (!root.imageTaskHost)
            return root.performImageReview(normalizedUrls);
        if (!normalizedUrls || normalizedUrls.length < 1 || normalizedUrls.length > 2) {
            const detail = qsTr("一次只能打开一张或两张图片。");

            dropError = detail;
            root.showIntentMessage(detail);
            workspaceSession.cancelOpen();
            return false;
        }
        if (root.pendingImageRequestId > 0 || root.imageTabOpenPending) {
            root.showIntentMessage(qsTr("正在打开图片，请稍候再打开新的标签。"));
            return false;
        }
        const accepted = Number(root.imageTaskHost.openImages(normalizedUrls));
        if (!(accepted >= 0)) {
            workspaceSession.cancelOpen();
            return false;
        }
        workspaceSession.beginOpen(workspaceSession.imageMedia);
        root.dropError = "";
        if (root.activeImageController && (root.activeImageController.hasPrimary || root.activeImageController.hasSecondary))
            root.adoptActiveImageTab();
        return true;
    }
    // Commit a task switch/open to the visible workspace. This deliberately does not end a
    // clean edit session and does not detach a folder session: both belong to the tab, not to
    // the act of looking at it.
    function adoptActiveImageTab() {
        const target = root.activeImageController;
        if (!target || (!target.hasPrimary && !target.hasSecondary))
            return false;
        root.ensureVideoPaused();
        // The active task may change while the same workspace item stays resident. Assigning
        // the typed controller explicitly is what makes the workspace rebind; a required
        // property is not re-evaluated as a live binding after its initial creation.
        if (root.imageWorkspace && root.imageWorkspace.controller !== target)
            root.imageWorkspace.controller = target;
        const identity = target.hasSecondary ? target.primaryPath + "\n" + target.secondaryPath : target.primaryPath;
        workspaceSession.markCommitted(workspaceSession.imageMedia, identity);
        root.showWorkspace(workspaceSession.imageMedia);
        root.armImageFolderCursorForActiveTab();
        root.focusActiveWorkspace();
        return true;
    }
    function activateImageTab(index) {
        if (!root.imageTaskHost)
            return false;
        if (root.imageTabOpenPending) {
            root.showIntentMessage(qsTr("正在打开图片，请稍候再切换标签。"));
            return false;
        }
        return Boolean(root.imageTaskHost.activate(Number(index)));
    }
    function canChangeImageSources() {
        if (root.imageEditComparisonTask) {
            root.showIntentMessage(qsTr("编辑对比快照保持固定；请打开新任务，或切回原编辑任务修改。"));
            return false;
        }
        if (root.imageLeaveBlockedReason.length > 0) {
            root.showUnsavedEditDialog(root.imageEditTabIndex, false);
            return false;
        }
        return true;
    }
    function showUnsavedEditDialog(index, exiting, closeTask) {
        const tab = index >= 0 && index < root.imageTabList.length ? root.imageTabList[index] : null;
        unsavedEditDialog.taskTitle = tab ? String(tab.title) : qsTr("当前图片");
        unsavedEditDialog.sourceLabel = root.imageEditService ? String(root.imageEditService.sourceLabel) : "";
        unsavedEditDialog.exiting = exiting;
        unsavedEditDialog.closingTask = Boolean(closeTask);
        root.showIntentMessage(qsTr("%1 有未保存的编辑副本，请切回保存或明确放弃修改。").arg(unsavedEditDialog.taskTitle));
        unsavedEditDialog.open();
    }
    function resumeOriginalImageEdit() {
        const source = root.imageEditComparisonTask ? String(root.imageTabList[root.imageTabActiveIndex].editSourceLabel || "") : String(root.imageEditService ? root.imageEditService.sourceLabel : "");
        return root.resumeImageEditForSource(source);
    }
    function resumeImageEditForSource(source) {
        let index = root.imageEditTabIndex;
        let secondary = false;
        if (source.length > 0 && root.imageTaskHost) {
            index = -1;
            for (let i = 0; i < root.imageTabList.length; ++i) {
                const tab = root.imageTabList[i];
                if (!tab.editComparison && (root.sameFilePath(source, String(tab.primaryPath)) || root.sameFilePath(source, String(tab.secondaryPath)))) {
                    index = i;
                    secondary = root.sameFilePath(source, String(tab.secondaryPath));
                    break;
                }
            }
        }
        if (!root.imageTaskHost && root.imageEditService && root.imageEditService.active && root.imageWorkspace) {
            root.activateWorkspace(1);
            return root.imageWorkspace.beginImageEdit();
        }
        if (index >= 0 && root.activateImageTab(index) && root.imageWorkspace) {
            if (!root.imageEditService.active) {
                root.activeImageController.compareMode = 0;
                root.imageWorkspace.singleViewShowSecondary = secondary;
            }
            return root.imageWorkspace.beginImageEdit();
        }
        root.showIntentMessage(qsTr("原编辑任务已关闭，请重新打开原文件后编辑；当前快照仍可比较。"));
        return false;
    }
    function compareEditedImage() {
        if (!root.imageTaskHost || !root.imageWorkspace || !root.imageWorkspace.hasEditSession)
            return false;
        const wasEditing = root.imageEditService.editingVisible;
        root.imageWorkspace.finishImageEdit();
        const index = Number(root.imageTaskHost.openEditComparison(root.imageEditService));
        if (index < 0) {
            root.imageEditService.editingVisible = wasEditing;
            return false;
        }
        root.detachFolderSession();
        root.showIntentMessage(qsTr("编辑对比快照：A 原图，B 当前编辑结果（含标注）；继续编辑后可再次建立新版本。"));
        return true;
    }
    function closeImageTab(index) {
        if (!root.imageTaskHost)
            return false;
        const tabIndex = Number(index);
        if (root.imageTabOpenPending) {
            root.showIntentMessage(qsTr("正在打开图片，请稍候再关闭标签。"));
            return false;
        }
        if (root.imageTabDirtyIndex === tabIndex) {
            root.showUnsavedEditDialog(tabIndex, false, true);
            return false;
        }
        if (root.imageEditService && root.imageEditService.active && root.imageEditTabIndex === tabIndex)
            root.imageEditService.endSession();
        return Boolean(root.imageTaskHost.closeAt(tabIndex));
    }
    function cycleImageTab(delta) {
        if (!root.imageTaskHost || root.imageTabCount < 2)
            return false;
        if (root.imageTabOpenPending) {
            root.showIntentMessage(qsTr("正在打开图片，请稍候再切换标签。"));
            return false;
        }
        root.imageTaskHost.cycle(Number(delta));
        return true;
    }
    function handleImageTaskHostActiveChanged() {
        const host = root.imageTaskHost;
        if (!host || Number(host.count) <= 0) {
            // The closed task's controller may already be gone (owned tabs are destroyed with
            // the task). Point the resident workspace back at the composition root's live
            // controller before any later binding reads it.
            if (root.imageWorkspace)
                root.imageWorkspace.controller = root.stillImageController;
            return;
        }
        const target = root.activeImageController;
        if (!target || (!target.hasPrimary && !target.hasSecondary))
            return;
        root.adoptActiveImageTab();
    }
    function finishOpenedImageTask(index) {
        const target = root.activeImageController;
        if (!target)
            return;
        // A direct open is a new review context, not a folder-session row.
        root.pendingImageRequestId = -1;
        root.pendingImageOpenKind = "";
        root.dropError = "";
        root.detachFolderSession();
        root.adoptActiveImageTab();
        if (root.preferences) {
            root.preferences.rememberMediaPath(target.primaryPath);
            if (target.secondaryPath && target.secondaryPath.length > 0)
                root.preferences.rememberMediaPath(target.secondaryPath);
        }
        if (root.videoFolderModel)
            root.videoFolderModel.finishPendingImageOpen(true, "", target.primaryPath);
        root.continueImageIssueRestore();
    }
    function handleImageTaskOpenFailure(error) {
        const detail = String(error || "").length > 0 ? String(error) : qsTr("无法打开图片。");
        root.dropError = detail;
        root.showIntentMessage(detail);
        workspaceSession.cancelOpen();
        if (root.videoFolderModel)
            root.videoFolderModel.cancelPendingImageOpen();
        if (root.imageTabCount <= 0 && !root.videoHasSession)
            root.focusActiveWorkspace();
    }
    function handleAllImageTasksClosed() {
        root.pendingImageRequestId = -1;
        root.pendingImageOpenKind = "";
        root.pendingFolderPairRow = -1;
        if (root.imageWorkspace)
            root.imageWorkspace.controller = root.stillImageController;
        if (root.imageEditService && root.imageEditService.active)
            root.imageEditService.endSession();
        if (root.imageFolderCursorModel)
            root.imageFolderCursorModel.clear();
        root.detachFolderSession();
        workspaceSession.clearCommitted(workspaceSession.imageMedia);
        if (root.videoHasSession)
            root.showWorkspace(workspaceSession.videoMedia);
        else
            root.focusActiveWorkspace();
    }

    // --- Video task tabs: open routing, switch skeleton, close/cycle ----------------------
    // (docs/plans/video-task-tabs-design.md §2.2/§4/§5/§6; slice 2 keeps the sidebar and
    // issue-restore paths on their legacy single-session routes.)

    function shellIntentIds() {
        const ids = [];
        if (!root.shell)
            return ids;
        const running = root.shell.activeIntent;
        if (Number(running.id || 0) > 0)
            ids.push(Number(running.id));
        const queued = root.shell.queuedIntents || [];
        for (let index = 0; index < queued.length; ++index)
            ids.push(Number(queued[index].id || 0));
        return ids;
    }

    // The id of the intent a shell submission just added: the one id present now that was
    // absent before. A synchronous submission either starts running or joins the queue, so
    // exactly one candidate appears.
    function firstNewShellIntentId(before) {
        if (!root.shell)
            return 0;
        const known = id => before.indexOf(Number(id)) >= 0;
        const running = root.shell.activeIntent;
        if (Number(running.id || 0) > 0 && !known(running.id))
            return Number(running.id);
        const queued = root.shell.queuedIntents || [];
        for (let index = 0; index < queued.length; ++index) {
            const id = Number(queued[index].id || 0);
            if (id > 0 && !known(id))
                return id;
        }
        return 0;
    }

    // Session-fact check: the controller projection must show exactly this slot-ordered
    // URL list. The comparison runs in C++ (VideoTaskHost::urlsMatch): the JS String()
    // of a QVariant-wrapped QUrl is not stable across engine states, and an identity
    // gate must not depend on it. The shell's Active Sources mirror is deliberately NOT
    // consulted - it can lag the controller by one publish.
    function sessionShowsUrls(urls) {
        if (!root.controller || !urls || urls.length === 0)
            return false;
        if (!root.videoTaskHost)
            return false;
        return Boolean(root.videoTaskHost.urlsMatch(root.controller.sourceUrls || [], urls));
    }

    // The host tab whose group the runtime session currently shows (slot order compared as
    // exact URL strings - the same canonicalized round-trip matchesOpenUrls relies on), or
    // -1 when the session belongs to no tab (sidebar open, issue restore, no session).
    function videoSessionTaskIndex() {
        if (!root.videoTaskHost || root.videoTabCount <= 0 || !root.controller)
            return -1;
        const current = root.controller.sourceUrls || [];
        if (current.length === 0)
            return -1;
        for (let index = 0; index < root.videoTabList.length; ++index) {
            const tab = root.videoTabList[index];
            const urls = tab.urls || [];
            if (urls.length !== current.length)
                continue;
            let same = true;
            for (let slot = 0; slot < urls.length; ++slot) {
                if (String(urls[slot]) !== String(current[slot])) {
                    same = false;
                    break;
                }
            }
            if (!same)
                continue;
            // The reference identity is part of the task fact (design §2.1). A session
            // without an explicit reference cannot disagree with the recorded slot.
            if (current.length > 1 && root.hasExplicitReference && Number(tab.referenceIndex) !== root.referenceSourceIndex)
                continue;
            return index;
        }
        return -1;
    }

    function videoTaskFrozenIdentities(urls) {
        const frozen = [];
        if (!root.controller)
            return frozen;
        for (let index = 0; index < urls.length; ++index)
            frozen.push(String(root.controller.frozenSourceIdentity(urls[index])));
        return frozen;
    }

    // Per-task snapshot (design §3, the full slice-3 set). Everything below is view or
    // session state the tab must come back to; the pack stays opaque to the host and is
    // captured after the pause and before any rebuild command is submitted (§4.2).
    function captureVideoTaskSnapshot() {
        const surface = viewportFrame && viewportFrame.surface ? viewportFrame.surface : null;
        const state = {
            urls: [],
            referenceIndex: root.referenceSourceIndex,
            hasExplicitReference: root.hasExplicitReference,
            canonicalSourceIndex: root.canonicalSourceIndex,
            frame: Number(root.currentFrame),
            mediaTime: root.controller ? Number(root.controller.currentMediaTime) : -1,
            viewMode: root.preferences ? Number(root.preferences.viewMode) : -1,
            // #4 alignment mode and manual offsets (decision g: analysis results and
            // manual anchors are deliberately NOT saved).
            alignmentMode: root.controller ? Number(root.controller.alignmentMode) : -1,
            sourceOffsets: root.sourceOffsets(),
            // #6 rate.
            playbackRate: root.controller ? Number(root.controller.playbackRate) : -1,
            // #7 range family: the shell mirror first, the kernel command second.
            rangeIn: root.inFrame,
            rangeOut: root.outFrame,
            rangeInMediaTime: root.inMediaTime,
            rangeOutMediaTime: root.outMediaTime,
            rangeActive: root.rangePlaybackActive,
            rangeLoop: Boolean(root.controller && root.controller.playbackRangeLoop),
            rangePingPong: Boolean(root.controller && root.controller.playbackRangePingPong),
            // #8 the current pair; single-source tasks have none.
            differenceEdge: root.sourceCount > 1 ? Number(root.differenceEdge) : -1,
            // #10 viewport zoom/pan/ROI.
            viewport: surface ? {
                centerX: Number(surface.viewCenterX),
                centerY: Number(surface.viewCenterY),
                scale: Number(surface.viewScale),
                roiEnabled: Boolean(surface.roiEnabled),
                roiLeft: Number(surface.roiLeft),
                roiTop: Number(surface.roiTop),
                roiRight: Number(surface.roiRight),
                roiBottom: Number(surface.roiBottom)
            } : null,
            // #11 wipe split.
            wipeOrientation: viewportFrame ? Number(viewportFrame.wipeOrientation) : -1,
            wipePosition: Number(root.wipePosition),
            // #12 difference parameters (threshold family is QML observation state).
            differenceThresholdEnabled: root.differenceThresholdEnabled,
            differenceThresholdCode: root.differenceThresholdCode,
            differenceThresholdPolicy: root.differenceThresholdPolicy,
            differenceMetric: root.preferences ? Number(root.preferences.differenceMetric) : -1,
            differenceGain: root.preferences ? Number(root.preferences.differenceGain) : -1,
            differenceFilter: root.preferences ? Number(root.preferences.differenceFilter) : -1,
            pairMetricsThreshold: root.pairMetricsModel ? Number(root.pairMetricsModel.threshold) : -1,
            pairMetricsThresholdPolicy: root.pairMetricsModel ? Number(root.pairMetricsModel.thresholdPolicy) : -1,
            // #13 GT direct look identity (transient peek deliberately not saved).
            canvasFocusIdentity: root.shell ? String(root.shell.canvasFocusIdentity) : "",
            // #14 folder navigation (decision c: the directory stays global; the row
            // highlight is recorded - its replay needs a model setter slice 4 owns).
            folderUrl: root.videoFolderModel ? String(root.videoFolderModel.folderUrl) : "",
            folderRow: root.videoFolderModel ? Number(root.videoFolderModel.currentRow) : -1
        };
        const current = root.controller ? (root.controller.sourceUrls || []) : [];
        for (let index = 0; index < current.length; ++index)
            state.urls.push(String(current[index]));
        return state;
    }

    // Snapshot the task the session currently shows (design §4.2). Must run before any
    // rebuild command replaces the session: the leaving task's position is the restore
    // target when it comes back.
    function snapshotVideoSessionTask() {
        if (!root.videoTaskHost)
            return false;
        const index = root.videoSessionTaskIndex();
        if (index < 0)
            return false;
        root.videoTaskHost.updateTaskState(index, root.captureVideoTaskSnapshot());
        return true;
    }

    // Routes a new-review video open through the task host (design §5 rows 1/2/9): the host
    // decides "focus the existing tab" versus "new loading record"; openRequested drives the
    // existing shell chain. Callers have already begun the workspace open.
    function openVideoTask(urls, referenceIndex, purpose) {
        if (!root.videoTaskHost || !root.controller || !root.shell)
            return false;
        const requested = [];
        for (let index = 0; urls && index < urls.length; ++index) {
            if (urls[index] && urls[index].toString().length > 0)
                requested.push(urls[index]);
        }
        if (requested.length === 0 || requested.length > 3)
            return false;
        if (root.videoTaskOpenPending) {
            root.showIntentMessage(qsTr("正在打开视频，请稍候再打开新的标签。"));
            workspaceSession.cancelOpen();
            return false;
        }
        const reference = Number(referenceIndex) >= 0 && Number(referenceIndex) < requested.length ? Number(referenceIndex) : 0;
        const index = Number(root.videoTaskHost.openTask(requested, reference, String(purpose || "Review")));
        if (index < 0) {
            workspaceSession.cancelOpen();
            return false;
        }
        if (!root.pendingVideoTaskOpen || Number(root.pendingVideoTaskOpen.index) !== index) {
            // Focus: the record and its snapshot stay untouched (design §2.2 step 2); the
            // activation already started the delta/restore path synchronously.
            workspaceSession.cancelOpen();
            if (root.videoTabActiveIndex === index && !root.videoTaskSwitchPending)
                root.showWorkspace(workspaceSession.videoMedia);
            return true;
        }
        return true;
    }

    // openRequested: the host created a loading record; submit the actual open through the
    // shell queue exactly like the legacy paths did. Runs synchronously inside openTask().
    function handleVideoTaskOpenRequested(index, urls, referenceIndex, purpose) {
        if (!root.videoTaskHost || !root.controller || !root.shell)
            return;
        // Adoption: the session already shows exactly this request - a startup open whose
        // shell intent finished before any record existed. Commit the session facts into
        // the record instead of submitting a second open.
        if (root.sessionShowsUrls(urls)) {
            root.commitVideoTaskOpen(Number(index), urls);
            return;
        }
        // Snapshot the task being left before the new open replaces the session.
        root.snapshotVideoSessionTask();
        const before = root.shellIntentIds();
        const staged = root.shell.stageSources(Array.from(urls), Number(referenceIndex));
        const opened = Boolean(staged && root.shell.openStagedSources(false));
        if (!opened) {
            root.videoTaskHost.failOpen(Number(index), qsTr("无法打开暂存的视频源。"));
            workspaceSession.cancelOpen();
            root.showIntentMessage(qsTr("无法打开该视频。"));
            root.focusActiveWorkspace();
            return;
        }
        root.pendingVideoTaskOpen = {
            index: Number(index),
            urls: Array.from(urls),
            referenceIndex: Number(referenceIndex),
            purpose: String(purpose || "Review"),
            intentId: root.firstNewShellIntentId(before)
        };
        root.dropError = "";
        root.commitWorkspace(workspaceSession.videoMedia, urls[0].toString());
    }

    function commitVideoTaskOpen(index, urls) {
        if (!root.videoTaskHost)
            return;
        root.videoTaskHost.commitOpen(Number(index), root.videoTaskFrozenIdentities(urls));
        root.dropError = "";
    }

    // Rewrites the ACTIVE task record from the session facts (design §6/§11-f): an
    // in-place disc switch resets the state snapshot (nothing to restore on new
    // material); a group evolution - one source removed - keeps it (decision f).
    function evolveActiveTaskFromSession(resetState) {
        if (!root.videoTaskHost || !root.controller)
            return false;
        const index = root.videoTabActiveIndex;
        if (index < 0)
            return false;
        const urls = root.controller.sourceUrls || [];
        if (urls.length === 0)
            return false;
        const reference = root.referenceSourceIndex >= 0 && root.referenceSourceIndex < urls.length ? root.referenceSourceIndex : 0;
        if (!root.videoTaskHost.replaceActiveTaskGroup(urls, reference, resetState))
            return false;
        root.videoTaskHost.commitOpen(index, root.videoTaskFrozenIdentities(urls));
        return true;
    }

    // Issue-restore adoption (slice 4): once a video record's restore machine has
    // finished, the task records must describe the session again. A matching record is
    // adopted in place (no new tab); otherwise the ACTIVE record's group is rewritten.
    // Cancelled, failed or timed-out restores deliberately leave the records untouched.
    function syncActiveVideoTaskWithSession() {
        if (!root.videoTaskHost || !root.controller)
            return false;
        const urls = root.controller.sourceUrls || [];
        if (urls.length === 0 || urls.length > 3)
            return false;
        const reference = root.referenceSourceIndex >= 0 && root.referenceSourceIndex < urls.length ? root.referenceSourceIndex : 0;
        const matched = Number(root.videoTaskHost.indexOfTask(urls, reference));
        if (matched >= 0) {
            root.videoTaskHost.commitOpen(matched, root.videoTaskFrozenIdentities(urls));
            if (matched !== root.videoTabActiveIndex)
                root.videoTaskHost.activate(matched);
            return true;
        }
        return root.evolveActiveTaskFromSession(true);
    }

    // Terminal of a shell OpenSourcesIntent: commit the pending host record, or adopt a
    // startup open the shell queued before any record existed (design §5 row 9).
    function settleVideoTaskOpenIntent(intentId) {
        if (root.pendingVideoTaskOpen && (Number(root.pendingVideoTaskOpen.intentId) === Number(intentId) || Number(root.pendingVideoTaskOpen.intentId) === 0)) {
            const record = root.pendingVideoTaskOpen;
            root.pendingVideoTaskOpen = null;
            root.commitVideoTaskOpen(Number(record.index), record.urls);
            return;
        }
        if (!root.videoTaskHost)
            return;
        if (root.videoStartupOpenIntentIds.indexOf(Number(intentId)) < 0)
            return;
        root.videoStartupOpenIntentIds = root.videoStartupOpenIntentIds.filter(id => Number(id) !== Number(intentId));
        const sessionUrls = root.activeSourceUrls();
        if (sessionUrls.length === 0 || sessionUrls.length > 3)
            return;
        const reference = root.referenceSourceIndex >= 0 && root.referenceSourceIndex < sessionUrls.length ? root.referenceSourceIndex : 0;
        // May focus an existing task or create a record; the openRequested handler sees
        // the session already showing the request and commits directly (adoption).
        root.videoTaskHost.openTask(sessionUrls, reference, "Startup");
    }

    // Startup-origin OpenSourcesIntents are adopted at their terminal; their ids are
    // learned from the shell's intent maps because the queue belongs to C++. A pending
    // host open whose queued intent was replaced, rejected or canceled must not stay a
    // loading tab forever: no terminal will ever arrive for it.
    function observeVideoTaskIntentEvent(intentId, status, kind) {
        if (!root.shell)
            return;
        if (kind === 0 && (status === 0 || status === 1)) {
            // Careful with || fallbacks here: kind 0 (OpenSourcesIntent) is falsy, so an
            // absent field must be told apart from a present zero.
            const candidates = [];
            const running = root.shell.activeIntent;
            const runningKind = running.kind === undefined ? -1 : Number(running.kind);
            const runningOrigin = running.origin === undefined ? -1 : Number(running.origin);
            if (runningKind === 0 && runningOrigin === 1)
                candidates.push(running.id === undefined ? 0 : Number(running.id));
            const queued = root.shell.queuedIntents || [];
            for (let index = 0; index < queued.length; ++index) {
                const entry = queued[index];
                const entryKind = entry.kind === undefined ? -1 : Number(entry.kind);
                const entryOrigin = entry.origin === undefined ? -1 : Number(entry.origin);
                if (entryKind === 0 && entryOrigin === 1)
                    candidates.push(entry.id === undefined ? 0 : Number(entry.id));
            }
            for (const id of candidates) {
                if (id > 0 && root.videoStartupOpenIntentIds.indexOf(id) < 0)
                    root.videoStartupOpenIntentIds = root.videoStartupOpenIntentIds.concat([id]);
            }
        }
        if (root.pendingVideoTaskOpen && Number(root.pendingVideoTaskOpen.intentId) === intentId && (status === 4 || status === 5 || status === 6)) {
            const record = root.pendingVideoTaskOpen;
            root.pendingVideoTaskOpen = null;
            root.videoTaskHost.failOpen(Number(record.index), status === 6 ? qsTr("更新的请求已取代本次打开。") : qsTr("本次打开已取消。"));
            workspaceSession.cancelOpen();
        }
    }

    // Group-evolution writeback for the append chain (design §5 row 3, decision f): the
    // shell path stays untouched; the active task's record grows in place on success.
    function applyVideoAppendWriteback() {
        if (!root.videoTaskHost || root.pendingVideoAppendUrl.toString().length === 0)
            return;
        const appended = root.pendingVideoAppendUrl;
        root.pendingVideoAppendUrl = "";
        const index = root.videoTabActiveIndex;
        if (index < 0 || !root.videoTaskHost.appendSourceToActiveTask(appended))
            return;
        const current = root.controller ? (root.controller.sourceUrls || []) : [];
        const urls = [];
        for (let slot = 0; slot < current.length; ++slot)
            urls.push(current[slot]);
        if (urls.length > 0)
            root.videoTaskHost.commitOpen(index, root.videoTaskFrozenIdentities(urls));
    }

    // A pure tab switch must not churn the workspace commit bookkeeping: an open that is
    // still pending finishes through its own terminal path.
    function focusVideoWorkspaceForTab() {
        if (workspaceSession.openPending)
            return;
        if (root.imageWorkspaceActive)
            root.showWorkspace(workspaceSession.videoMedia);
        else
            root.focusActiveWorkspace();
    }

    // Tab activation (click, cycle, focus-after-reopen, neighbour after close): pause,
    // snapshot the task being left, then either confirm focus (the session still shows the
    // target - nothing was lost, the live paused position is the truth) or rebuild the
    // session for the target without the displayed-time mapping (design §4.3).
    function beginVideoTaskSwitch(targetIndex) {
        if (!root.videoTaskHost || !root.controller)
            return false;
        const tabs = root.videoTabList;
        if (targetIndex < 0 || targetIndex >= tabs.length)
            return false;
        const tab = tabs[targetIndex];
        if (tab.loading || tab.restoring)
            return false;
        // A switch still in flight is superseded, not queued (design §4.5 step 4).
        root.cancelPendingVideoTaskSwitch();
        root.ensureVideoPaused();
        const sessionIndex = root.videoSessionTaskIndex();
        if (sessionIndex >= 0 && sessionIndex !== targetIndex)
            root.videoTaskHost.updateTaskState(sessionIndex, root.captureVideoTaskSnapshot());
        if (sessionIndex === targetIndex) {
            // Same group as the runtime session: no reopen, the epoch stays put.
            root.focusVideoWorkspaceForTab();
            return true;
        }
        const urls = [];
        for (let slot = 0; slot < tab.urls.length; ++slot)
            urls.push(tab.urls[slot]);
        const reference = Number(tab.referenceIndex) >= 0 && Number(tab.referenceIndex) < urls.length ? Number(tab.referenceIndex) : 0;
        root.videoTaskHost.setTaskRestoring(targetIndex, true);
        root.showIntentMessage(qsTr("正在恢复视频标签…"));
        const reopened = Boolean(root.controller.reopenSourcesWithoutMapping(urls, reference));
        if (!reopened) {
            root.failVideoTaskSwitch(targetIndex, qsTr("恢复失败：无法重新打开该标签的视频来源。"), urls);
            return false;
        }
        root.pendingVideoTaskSwitch = {
            targetIndex: targetIndex,
            urls: urls,
            state: root.videoTaskHost.taskState(targetIndex) || {}
        };
        root.pendingVideoTaskSwitchOpenCommandId = Number(root.controller.lastSubmittedCommandId);
        root.pendingVideoTaskSwitchOpenSessionEpoch = root.controller.sessionEpoch;
        root.pendingVideoTaskSwitchOpenOutcome = -1;
        root.pendingVideoTaskSwitchSessionEpoch = 0;
        root.pendingVideoTaskSwitchSeekCommandId = 0;
        root.pendingVideoTaskSwitchContextApplied = false;
        root.pendingVideoTaskSwitchPairCommandId = 0;
        root.pendingVideoTaskSwitchPairApplied = true;
        root.pendingVideoTaskSwitchPartial = [];
        videoTaskSwitchDeadline.restart();
        root.continueVideoTaskSwitch();
        return true;
    }

    function continueVideoTaskSwitch() {
        if (root.videoTaskSwitchContinuing)
            return;
        root.videoTaskSwitchContinuing = true;
        try {
            root.advanceVideoTaskSwitch();
        } finally {
            root.videoTaskSwitchContinuing = false;
        }
    }

    function advanceVideoTaskSwitch() {
        const plan = root.pendingVideoTaskSwitch;
        if (!plan || !root.controller || !root.videoTaskHost)
            return;
        const urls = plan.urls;
        // Stage 1: the reopen terminal, bound to the submit-time epoch (design §4.6). A
        // terminal from any other session is not this switch's answer.
        if (root.pendingVideoTaskSwitchOpenCommandId !== 0)
            return;
        if (root.pendingVideoTaskSwitchOpenOutcome !== 0 || root.sourceCount === 0) {
            root.failVideoTaskSwitch(Number(plan.targetIndex), qsTr("恢复失败：无法重新打开该标签的视频来源。"), urls);
            return;
        }
        if (!root.sessionShowsUrls(urls)) {
            root.abandonPendingVideoTaskSwitch(qsTr("恢复已取消：打开的素材与标签不一致"));
            return;
        }
        if (root.pendingVideoTaskSwitchSessionEpoch === 0) {
            root.pendingVideoTaskSwitchSessionEpoch = root.controller.sessionEpoch;
        } else if (root.controller.sessionEpoch !== root.pendingVideoTaskSwitchSessionEpoch || !root.sessionShowsUrls(urls)) {
            root.abandonPendingVideoTaskSwitch(qsTr("恢复已取消：打开的素材与标签不一致"));
            return;
        }
        root.ensureVideoPaused();
        // Stage 2: the recorded pair exactly once - a pure submission, never the
        // full-group fallback - with the recorded mode landing right after it (the
        // issue-restore order: pair first, mode last so it survives; D07). A refused
        // pair is a partial-restore note, never a blocker.
        if (!root.pendingVideoTaskSwitchContextApplied) {
            root.pendingVideoTaskSwitchContextApplied = true;
            const edge = Number(plan.state.differenceEdge);
            if (Number.isFinite(edge) && edge >= 0 && root.sourceCount > 1) {
                root.pendingVideoTaskSwitchPairApplied = false;
                if (root.submitDifferenceEdge(edge)) {
                    root.pendingVideoTaskSwitchPairCommandId = Number(root.controller.lastSubmittedCommandId);
                } else {
                    root.noteVideoTaskPartialRestore(qsTr("比较对"));
                }
            }
            if (Number(plan.state.viewMode) >= 0 && root.preferences)
                root.preferences.viewMode = Number(plan.state.viewMode);
        }
        if (root.pendingVideoTaskSwitchPairCommandId !== 0)
            return;
        // Stage 3: land the snapshot frame and stay paused (design §4.4 - the protocol
        // never plays). A position that cannot land is a partial note; the family below
        // still replays (§4.6 tier 2).
        const frame = Number(plan.state.frame);
        if (Number.isFinite(frame) && frame > 0 && root.currentFrame !== frame) {
            if (root.totalFrames > 0 && frame >= root.totalFrames) {
                root.noteVideoTaskPartialRestore(qsTr("位置"));
            } else if (root.busy || root.framePending) {
                return;
            } else if (root.pendingVideoTaskSwitchSeekCommandId === 0) {
                if (!root.controller.seekFrame(frame)) {
                    root.noteVideoTaskPartialRestore(qsTr("位置"));
                } else {
                    root.pendingVideoTaskSwitchSeekCommandId = Number(root.controller.lastSubmittedCommandId);
                    return;
                }
            } else {
                return;
            }
        }
        if (root.pendingVideoTaskSwitchSeekCommandId !== 0 || root.framePending)
            return;
        root.finishVideoTaskSwitch();
    }

    // Design §4.5 stages 4-5: replay the presentation family, order mirroring the
    // issue-restore machine (viewport, wipe, difference parameters, range, alignment,
    // direct look, rate). Pure assignments need no terminal; nothing here blocks.
    function finishVideoTaskSwitch() {
        const plan = root.pendingVideoTaskSwitch;
        if (!plan)
            return;
        const state = plan.state || {};
        const surface = viewportFrame && viewportFrame.surface ? viewportFrame.surface : null;
        if (state.viewport && surface)
            surface.restoreViewport(Number(state.viewport.centerX), Number(state.viewport.centerY), Number(state.viewport.scale), Boolean(state.viewport.roiEnabled), Number(state.viewport.roiLeft), Number(state.viewport.roiTop), Number(state.viewport.roiRight), Number(state.viewport.roiBottom));
        if (Number.isFinite(Number(state.wipeOrientation)) && Number(state.wipeOrientation) >= 0 && viewportFrame)
            viewportFrame.setWipeOrientation(Number(state.wipeOrientation));
        root.wipePosition = Number(state.wipePosition);
        root.differenceThresholdEnabled = Boolean(state.differenceThresholdEnabled);
        root.differenceThresholdCode = Number(state.differenceThresholdCode);
        root.differenceThresholdPolicy = Number(state.differenceThresholdPolicy);
        if (root.preferences) {
            if (Number.isFinite(Number(state.differenceMetric)) && Number(state.differenceMetric) >= 0)
                root.preferences.differenceMetric = Number(state.differenceMetric);
            if (Number.isFinite(Number(state.differenceGain)) && Number(state.differenceGain) >= 0)
                root.preferences.differenceGain = Number(state.differenceGain);
            if (Number.isFinite(Number(state.differenceFilter)) && Number(state.differenceFilter) >= 0)
                root.preferences.differenceFilter = Number(state.differenceFilter);
        }
        if (root.pairMetricsModel) {
            if (Number.isFinite(Number(state.pairMetricsThreshold)) && Number(state.pairMetricsThreshold) >= 0)
                root.pairMetricsModel.threshold = Number(state.pairMetricsThreshold);
            if (Number.isFinite(Number(state.pairMetricsThresholdPolicy)) && Number(state.pairMetricsThresholdPolicy) >= 0)
                root.pairMetricsModel.thresholdPolicy = Number(state.pairMetricsThresholdPolicy);
        }
        if (root.shell) {
            // Range family: the shell mirror first, then the kernel command (the
            // playIntervalRange precedent order); ping-pong rides its own toggle.
            const rangeIn = Number(state.rangeIn);
            const rangeOut = Number(state.rangeOut);
            if (Number.isFinite(rangeIn) && rangeIn >= 0 && Number.isFinite(rangeOut) && rangeOut >= rangeIn) {
                root.shell.setRangeIn(rangeIn, Number(state.rangeInMediaTime));
                root.shell.setRangeOut(rangeOut, Number(state.rangeOutMediaTime));
                root.shell.setRangePlaybackState(Boolean(state.rangeActive), false);
                root.shell.setRangeStartPending(false);
                if (root.controller) {
                    root.controller.setPlaybackRange(rangeIn, rangeOut, Boolean(state.rangeLoop));
                    if (Boolean(state.rangePingPong) && !root.controller.playbackRangePingPong)
                        root.controller.togglePlaybackRangePingPong();
                }
            } else {
                root.shell.clearRange();
                if (root.controller)
                    root.controller.setPlaybackRange(-1, -1, false);
            }
        }
        if (root.controller) {
            if (Number.isFinite(Number(state.alignmentMode)) && Number(state.alignmentMode) >= 0)
                root.controller.setAlignmentMode(Number(state.alignmentMode));
            if (state.sourceOffsets && state.sourceOffsets.length > 0)
                root.controller.applySourceOffsets(state.sourceOffsets);
            if (Number(state.playbackRate) > 0)
                root.controller.setPlaybackRate(Number(state.playbackRate));
            // GT direct look replays by frozen identity, never remapped onto whatever
            // source sits in the slot now (the C2 rule the shell itself follows).
            if (root.shell) {
                const focusIdentity = String(state.canvasFocusIdentity || "");
                if (focusIdentity.length > 0) {
                    const slot = root.shell.activeSourceIdentities ? root.shell.activeSourceIdentities.indexOf(focusIdentity) : -1;
                    if (slot >= 0)
                        root.shell.requestCanvasFocus(comparisonBar.canvasFocusRequestSourceSlot, slot);
                } else if (root.shell.canvasFocusSlot >= 0) {
                    root.shell.requestCanvasFocus(comparisonBar.canvasFocusRequestOff, 0);
                }
            }
        }
        // #14 folder row highlight: the directory stays global (decision c); replaying
        // the row highlight needs a VideoFolderModel setter that slice 4 owns, so the
        // recorded values ride along in the snapshot for that slice.
        // #14 folder row highlight (decision c): the directory stays global and follows
        // the session source; the restore only replays the recorded row highlight, and
        // only when the model is still showing that task's folder.
        if (root.videoFolderModel && String(state.folderUrl).length > 0 && String(root.videoFolderModel.folderUrl) === String(state.folderUrl) && Number(state.folderRow) >= 0)
            root.videoFolderModel.setRowHighlight(Number(state.folderRow));
        const partial = root.pendingVideoTaskSwitchPartial;
        if (!root.pendingVideoTaskSwitchPairApplied && partial.indexOf(qsTr("比较对")) < 0)
            partial.push(qsTr("比较对"));
        root.cancelPendingVideoTaskSwitch();
        root.focusVideoWorkspaceForTab();
        if (partial.length > 0) {
            root.showIntentMessage(qsTr("部分恢复：%1 未恢复").arg(partial.join(qsTr("、"))));
        } else {
            root.showIntentMessage(qsTr("已恢复视频标签 · %1").arg(root.frameTextFor(Number(state.frame))));
        }
    }

    // Tier-2 partial-restore bookkeeping (§4.6): a named part that could not come back;
    // the restore itself continues.
    function noteVideoTaskPartialRestore(what) {
        if (root.pendingVideoTaskSwitchPartial.indexOf(what) < 0)
            root.pendingVideoTaskSwitchPartial = root.pendingVideoTaskSwitchPartial.concat([what]);
    }

    // Design §4.6 first failure tier: the reopen failed, so the previous session is
    // untouched (the ReplaceSources contract). Fall back to the tab the session still
    // shows and name the files that refused to reopen.
    function failVideoTaskSwitch(targetIndex, message, urls) {
        if (root.videoTaskHost && targetIndex >= 0 && targetIndex < root.videoTabCount) {
            // Covers the synchronous failure too, where no plan exists yet to cancel.
            root.videoTaskHost.setTaskRestoring(Number(targetIndex), false);
            root.videoTaskHost.failOpen(targetIndex, message);
        }
        root.cancelPendingVideoTaskSwitch();
        const names = [];
        for (let index = 0; urls && index < urls.length; ++index)
            names.push(root.fileName(urls[index]));
        root.showIntentMessage(names.length > 0 ? message + qsTr("（%1）").arg(names.join(qsTr("、"))) : message);
        const sessionIndex = root.videoSessionTaskIndex();
        if (sessionIndex >= 0 && sessionIndex !== targetIndex)
            root.videoTaskHost.activate(sessionIndex);
    }

    function abandonPendingVideoTaskSwitch(message) {
        root.cancelPendingVideoTaskSwitch();
        root.showIntentMessage(message);
    }

    function cancelPendingVideoTaskSwitch() {
        const plan = root.pendingVideoTaskSwitch;
        if (plan && root.videoTaskHost && Number(plan.targetIndex) >= 0 && Number(plan.targetIndex) < root.videoTabCount)
            root.videoTaskHost.setTaskRestoring(Number(plan.targetIndex), false);
        root.pendingVideoTaskSwitch = null;
        root.pendingVideoTaskSwitchOpenCommandId = 0;
        root.pendingVideoTaskSwitchOpenOutcome = -1;
        root.pendingVideoTaskSwitchOpenSessionEpoch = 0;
        root.pendingVideoTaskSwitchSessionEpoch = 0;
        root.pendingVideoTaskSwitchSeekCommandId = 0;
        root.pendingVideoTaskSwitchContextApplied = false;
        root.pendingVideoTaskSwitchPairCommandId = 0;
        root.pendingVideoTaskSwitchPairApplied = true;
        root.pendingVideoTaskSwitchPartial = [];
        videoTaskSwitchDeadline.stop();
    }

    function acceptVideoTaskSwitchCommand(commandId, sessionEpoch, outcome) {
        if (!root.pendingVideoTaskSwitch)
            return;
        if (commandId === root.pendingVideoTaskSwitchOpenCommandId) {
            if (sessionEpoch !== root.pendingVideoTaskSwitchOpenSessionEpoch)
                return;
            root.pendingVideoTaskSwitchOpenCommandId = 0;
            root.pendingVideoTaskSwitchOpenOutcome = Number(outcome);
        } else if (root.pendingVideoTaskSwitchSessionEpoch !== 0 && sessionEpoch !== root.pendingVideoTaskSwitchSessionEpoch) {
            return;
        } else if (commandId === root.pendingVideoTaskSwitchSeekCommandId) {
            root.pendingVideoTaskSwitchSeekCommandId = 0;
            if (Number(outcome) !== 0)
                root.noteVideoTaskPartialRestore(qsTr("位置"));
        } else if (commandId === root.pendingVideoTaskSwitchPairCommandId) {
            // The pair terminal only counts when the session now shows the recorded
            // edge; anything else is a partial note, not a blocker (§4.5 step 1).
            root.pendingVideoTaskSwitchPairCommandId = 0;
            const expected = Number(root.pendingVideoTaskSwitch.state ? root.pendingVideoTaskSwitch.state.differenceEdge : -1);
            root.pendingVideoTaskSwitchPairApplied = Number(outcome) === 0 && root.differenceEdge === expected;
            if (!root.pendingVideoTaskSwitchPairApplied)
                root.noteVideoTaskPartialRestore(qsTr("比较对"));
        } else {
            return;
        }
        root.continueVideoTaskSwitch();
    }

    function handleVideoTaskActiveChanged() {
        if (!root.videoTaskHost || root.videoTabCount <= 0)
            return;
        const index = root.videoTabActiveIndex;
        if (index < 0 || index >= root.videoTabList.length)
            return;
        const tab = root.videoTabList[index];
        const key = (tab.urls || []).join("\n") + "#" + String(tab.referenceIndex);
        if (key === root.videoLastActivatedKey)
            return;
        root.videoLastActivatedKey = key;
        root.beginVideoTaskSwitch(index);
    }

    function activateVideoTab(index) {
        if (!root.videoTaskHost)
            return false;
        const tabIndex = Number(index);
        if (tabIndex < 0 || tabIndex >= root.videoTabList.length)
            return false;
        if (root.videoTaskOpenPending) {
            root.showIntentMessage(qsTr("正在打开视频，请稍候再切换标签。"));
            return false;
        }
        if (root.videoTabList[tabIndex].restoring) {
            root.showIntentMessage(qsTr("正在恢复视频标签，请稍候。"));
            return false;
        }
        if (tabIndex === root.videoTabActiveIndex)
            return true;
        return Boolean(root.videoTaskHost.activate(tabIndex));
    }

    function closeVideoTab(index) {
        if (!root.videoTaskHost)
            return false;
        const tabIndex = Number(index);
        if (tabIndex < 0 || tabIndex >= root.videoTabList.length)
            return false;
        if (root.videoTaskOpenPending) {
            root.showIntentMessage(qsTr("正在打开视频，请稍候再关闭标签。"));
            return false;
        }
        if (root.videoTabList[tabIndex].restoring) {
            root.showIntentMessage(qsTr("正在恢复视频标签，请稍候再关闭。"));
            return false;
        }
        if (!root.videoTaskHost.closeAt(tabIndex))
            return false;
        // Closing the active tab can slide the right neighbour into the same numeric
        // index without any activeChanged; the identity-keyed handler is idempotent, so
        // calling it after every close covers both orders.
        root.handleVideoTaskActiveChanged();
        return true;
    }

    function cycleVideoTab(delta) {
        if (!root.videoTaskHost || root.videoTabCount < 2)
            return false;
        if (root.videoTaskOpenPending) {
            root.showIntentMessage(qsTr("正在打开视频，请稍候再切换标签。"));
            return false;
        }
        root.videoTaskHost.cycle(Number(delta));
        return true;
    }

    // The last video tab closed through the host: the shared session must close with it
    // (the legacy Ctrl+W tail), and the stage goes to a surviving image task if any.
    function handleAllVideoTasksClosed() {
        root.videoLastActivatedKey = "";
        root.cancelPendingVideoTaskSwitch();
        if (!root.shell || !root.controller || !root.videoHasSession)
            return;
        const shellHadSources = Boolean(shell.activeSources && shell.activeSources.length > 0);
        if (!shell.closeSources())
            return;
        if (!shellHadSources && root.videoHasSession && controller && Number(controller.sourceCount) > 0)
            controller.closeSources();
        workspaceSession.clearCommitted(workspaceSession.videoMedia);
        showIntentMessage(qsTr("正在关闭视频…"));
        if (root.imageHasContent || root.imageTabCount > 0)
            showWorkspace(workspaceSession.imageMedia);
        else
            focusActiveWorkspace();
    }
    // FLOW3 single-directory stepping. The cursor supplies a URL only; the ordinary image open
    // decides whether it commits, so a neighbour that fails to decode leaves the current image
    // on screen and the request-identity rules are the same ones a direct open already uses.
    function stepImageFolder(delta) {
        const cursor = root.imageFolderCursorModel;

        if (!cursor || delta === 0)
            return false;

        const url = delta > 0 ? cursor.nextUrl() : cursor.previousUrl();

        if (!url || String(url).length === 0) {
            root.showIntentMessage(delta > 0 ? qsTr("已经是本目录的最后一张图片。") : qsTr("已经是本目录的第一张图片。"));
            return false;
        }

        return root.performImageReview([url]);
    }
    function jumpImageFolder(toLast) {
        const cursor = root.imageFolderCursorModel;

        if (!cursor)
            return false;

        const url = toLast ? cursor.lastUrl() : cursor.firstUrl();

        if (!url || String(url).length === 0)
            return false;

        return root.performImageReview([url]);
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

        return openImageInNewTab(selected);
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
            const imageTarget = root.activeImageController;

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

                // Video task tabs (design §5 row 3): remember the appended source so the
                // successful terminal can grow the active task's record in place.
                pendingVideoAppendUrl = normalizedUrls[0];

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
        return reviewUrls(urls, false);
    }

    function openDroppedComparison(referenceIndex) {
        if (shell)
            shell.stagedReferenceIndex = referenceIndex;

        // Video task tabs (design §5 row 2): for a new comparison review the reference
        // dialog only decides the task key's reference slot; the host routes new tab
        // versus focus and openRequested submits the staged sources. The append flow
        // (row 3) keeps the legacy preserve-position chain untouched.
        const appendFlow = pendingSidebarComparison || (pendingComparisonPreservesPosition && sourceCount > 0);
        if (!appendFlow && root.videoTaskHost && shell) {
            const staged = Array.from(shell.stagedSources);
            if (staged.length >= 2 && staged.length <= 3) {
                pendingNewReviewWantsThreeUp = staged.length === 3;
                if (root.openVideoTask(staged, Number(referenceIndex), "Review")) {
                    if (!root.pendingVideoTaskOpen && root.shell)
                        root.shell.clearStagedSources();
                    pendingSidebarComparison = false;
                    pendingComparisonPreservesPosition = false;
                    return true;
                }
                return false;
            }
        }

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

    // V02 direct look from a source chip: toggle the shell's canvas focus for that session
    // slot through the P05 request chain. Presentation state only - the same frame, ROI,
    // zoom/pan, GT role and comparison pair come back when the focus returns Off - so this
    // function must do nothing else. The request codes come from the mode bar's enum mirrors
    // (themselves cross-checked against the shell enum by contract tests).
    // Selecting a comparison layout always leaves a direct look or a locked GT peek,
    // even when the selected mode is already the stored preference.
    function selectComparisonMode(mode) {
        if (!preferences || !shell)
            return;
        shell.requestCanvasFocus(comparisonBar.canvasFocusRequestOff, 0);
        differencePeekLocked = false;
        differencePeekActive = false;
        preferences.viewMode = mode;
    }

    function toggleDirectLook(sourceSlot) {
        if (!shell || !comparisonBar)
            return false;
        const focused = Number(shell.canvasFocusSlot);
        const request = focused === sourceSlot ? comparisonBar.canvasFocusRequestOff : comparisonBar.canvasFocusRequestSourceSlot;
        const ok = Boolean(shell.requestCanvasFocus(request, sourceSlot));
        return ok;
    }

    // D3 pair look (T): alternate the whole-canvas direct look between the two slots of the
    // actual current comparison pair - the effective edge, so GT/candidate combinations are
    // included and nothing is hardcoded to A/B. Paused sessions only: while playing the key
    // must stay inert so it never competes with playback semantics. Presentation-only chain,
    // identical to the mode bar's 看 A/B/C entries.
    function togglePairLook() {
        if (playing || sourceCount < 2 || !shell || !comparisonBar)
            return false;
        const first = differenceFirstSlot;
        const second = differenceSecondSlot;
        if (first === second || first < 0 || second < 0 || first >= sourceCount || second >= sourceCount)
            return false;
        const focused = Number(shell.canvasFocusSlot);
        return toggleDirectLook(focused === first ? second : first);
    }

    // V02 "替换文件": the chip menu records which frozen identity is being replaced, then a
    // single-file dialog picks the successor. The swap keeps the slot count, the reference
    // role and the displayed position (openStagedSources(true) -> ReplaceSourcesIntent);
    // a bad file fails the intent without tearing down the current session.
    property string pendingReplaceSourceIdentity: ""

    function requestReplaceSource(identity) {
        if (!shell || String(identity || "").length === 0)
            return false;
        pendingReplaceSourceIdentity = String(identity);
        ensureReviewInputDialogs().openReplaceVideo();
        return true;
    }

    // I01 "拖到明确源卡片": a video dropped on one chip replaces exactly that source through
    // the same staged swap as the chip menu. Only a single readable video is a replacement;
    // anything else refuses without touching the compare set, and a failed swap keeps the old
    // session (the ReplaceSourcesIntent contract).
    function dropReplaceSource(identity, url) {
        if (!shell || String(identity || "").length === 0 || !url || url.toString().length === 0)
            return false;
        const reviewed = controller ? controller.handleDroppedUrls([url]) : null;
        if (!reviewed || !reviewed.accepted || reviewed.kind !== "videos") {
            showIntentMessage(qsTr("请拖入单个视频文件替换该源。"));
            return false;
        }
        pendingReplaceSourceIdentity = String(identity);
        return performSourceReplacement(url);
    }

    // V02 acceptance: an already-open file is refused with the canonical local path, not a raw
    // URL string. handleDroppedUrls is the one validator that canonicalizes a local file; both
    // the replacement candidate and the already-open sources go through it, so the comparison
    // holds even when the open set was staged under a different spelling (redundant path
    // segment, case differences, UNC vs mapped drive). Unresolvable spellings fall back to the
    // raw form, which can only add refusals, never silently accept a duplicate.
    function canonicalVideoUrl(url) {
        if (!controller || !url)
            return url;
        const reviewed = controller.handleDroppedUrls([url]);
        if (reviewed && reviewed.accepted && reviewed.urls && reviewed.urls.length === 1)
            return reviewed.urls[0];
        return url;
    }

    function performSourceReplacement(url) {
        const identity = pendingReplaceSourceIdentity;
        pendingReplaceSourceIdentity = "";
        if (!shell || identity.length === 0 || !url || url.toString().length === 0)
            return false;
        const identities = shell.activeSourceIdentities;
        const slot = identities ? identities.indexOf(identity) : -1;
        if (slot < 0) {
            showIntentMessage(qsTr("所选视频已不可用。"));
            return false;
        }
        let candidateUrl = url;
        if (controller) {
            const reviewed = controller.handleDroppedUrls([url]);
            if (!reviewed || !reviewed.accepted || reviewed.kind !== "videos" || !reviewed.urls || reviewed.urls.length !== 1) {
                showIntentMessage(qsTr("请选择单个可用的视频文件替换该源。"));
                return false;
            }
            candidateUrl = canonicalVideoUrl(url);
        }
        const urls = activeSourceUrls();
        const candidate = candidateUrl.toString();
        for (let index = 0; index < urls.length; ++index) {
            if (index !== slot && canonicalVideoUrl(urls[index]).toString() === candidate) {
                showIntentMessage(qsTr("该视频已在当前对比中。"));
                return false;
            }
        }
        urls[slot] = candidateUrl;
        const reference = shell.referenceSourceIndex >= 0 ? shell.referenceSourceIndex : 0;
        if (!shell.stageSources(urls, reference))
            return false;
        return Boolean(shell.openStagedSources(true));
    }

    function showImmersiveHud(message) {
        immersiveHudText = message;

        immersiveHudVisible = true;

        immersiveHudTimer.restart();
    }

    // Feedback for actions the user starts from the chrome (menu, transport chips, dialogs).
    // The immersive HUD only renders while the chrome is hidden, so chrome-time messages would
    // be invisible; the intent toast renders in both presentation states.
    function showActionFeedback(message) {
        if (chromeVisible)
            showIntentMessage(message);
        else
            showImmersiveHud(message);
    }

    // The 流畅优先/逐帧完整 toggle switches policy silently otherwise; the note names the
    // trade-off the choice just made, which is the part the labels alone cannot carry.
    function showContinuityPolicyHud(policy) {
        if (policy === 0)
            showActionFeedback(qsTr("逐帧完整：每一帧都显示；负载高时播放会放慢"));
        else if (policy === 1)
            showActionFeedback(qsTr("流畅优先：保持原速；解码跟不上时整组跳帧"));
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

    // D07: the pair command itself, with no view change of any kind. Shared by the
    // explicit pair entries (applyDifferenceEdge, which adds the full-group fallback)
    // and the fixed-GT candidate switch - C keeps its original meaning
    // ("按 C → 参考不动，换另一个版本"): a pure pair flip that never leaves the current
    // view, including the GT-anchored DualDifference, whose canvas does not follow the
    // active edge (the layout takes the reference slot and the two non-reference slots).
    function submitDifferenceEdge(value) {
        if (!controller || !controller.applyComparisonPairFromEdge)
            return false;
        const pairPolicy = preferences ? Number(preferences.defaultPairPolicy) : 2;
        return Boolean(controller.applyComparisonPairFromEdge(Number(value), Number.isFinite(pairPolicy) ? pairPolicy : 2));
    }

    // D07: user selection sends SetActiveComparisonPairCommand. Preferences only store
    // DefaultPairPolicy; canvas/menus read the committed effective pair.
    function applyDifferenceEdge(edge) {
        const applied = submitDifferenceEdge(edge);
        // From a full-group view (every source on screen) choosing a pair means going
        // back to a two-path comparison on that pair: the last used two-path mode,
        // SideBySide before any was used. selectComparisonMode also leaves a canvas
        // direct look and a locked peek behind - the same edge behaviour a mode click
        // has. Inside a two-path mode the switch stays presentation-only (V02: no
        // seek, no mode change, observation kept); the fallback only fires from the
        // full-group set above. Because the fallback lives here, every EXPLICIT pair
        // entry gets it - the button group, the context menu and the inspector rows.
        // The candidate switch deliberately calls submitDifferenceEdge instead: it is
        // not a "pick a pair" entry, it is the C-key flip against the fixed GT.
        if (applied && fullGroupView)
            selectComparisonMode(lastPairMode);
        return applied;
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
        // D3: without an explicitly identified reference there is no GT-anchored pair to
        // switch to. referenceSourceIndex alone is not the fact - the controller falls back
        // to the canonical (timeline master) slot when the validated comparison has no
        // reference - so gate on the explicit role and name the reason through the same
        // visible feedback channel as the peek. The canonical fallback stays untouched for
        // its existing readers.
        if (!hasExplicitReference || referenceSourceIndex < 0 || referenceSourceIndex >= sourceCount) {
            showActionFeedback(qsTr("未设置参考（GT）源：切候选需要 GT 锚定的比较对，请先在来源菜单设置参考"));
            return false;
        }
        const reference = referenceSourceIndex;
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
            // Pure pair submission (submitDifferenceEdge), NOT applyDifferenceEdge: the
            // C key never triggers the full-group fallback, so a DualDifference canvas
            // (edge-independent layout) survives the candidate flip untouched.
            return submitDifferenceEdge(Number(edge.preferenceValue));
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
            root.continueVideoTaskSwitch();
            const key = root.playbackPrefsSessionKey();
            if (key.length === 0 || key === root.playbackPrefsAppliedKey)
                return;
            root.playbackPrefsAppliedKey = key;
            root.applyPlaybackPreferencesFromSettings();
        }

        function onFrameStateChanged() {
            root.continueIssueRestore();
            root.continueVideoTaskSwitch();
        }

        function onCommandFinished(commandId, sessionEpoch, outcome, errorKey) {
            root.acceptIssueRestoreCommand(commandId, sessionEpoch, outcome);
            root.acceptVideoTaskSwitchCommand(commandId, sessionEpoch, outcome);
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

    function focusIsTaskTab(item) {
        let candidate = item;
        while (candidate) {
            if (candidate.keepsTaskTabFocus === true)
                return true;
            candidate = candidate.parent;
        }
        return false;
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
        autoplayOnOpen: root.autoplayOnOpen
        sourceIdentities: root.shell ? root.shell.activeSourceIdentities : []
        playbackContinuityPolicy: {
            if (!controller)
                return 2;
            const fromPref = root.preferences ? Number(root.preferences.playbackContinuityPolicy) : 2;
            const fromSession = Number(controller.playbackContinuityPolicy);
            return Number.isFinite(fromSession) && controller.displayState === 2 ? fromSession : fromPref;
        }
        workspaceMode: root.workspaceMode
        imageHasPrimary: Boolean(root.activeImageController && root.activeImageController.hasPrimary)
        imageHasSecondary: Boolean(root.activeImageController && root.activeImageController.hasSecondary)
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
        onOpenFilesRequested: root.requestOpenMediaFiles()
        onAddVideoRequested: root.requestAddVideo()
        onAddImageRequested: root.requestImageAdd()
        onOpenImagePairRequested: root.requestImagePairOpen()
        onCompareImageFoldersRequested: root.requestCompareFolders()
        // Open one event-loop turn later, not inline: the menu closes itself while the item's
        // handler runs, and a popup that entered the overlay while the menu was open gets
        // popped with it.
        onFileAssociationDialogRequested: Qt.callLater(fileAssociationDialog.open)
        onWorkspaceRequested: mode => root.activateWorkspace(mode)
        onCloseCurrentRequested: root.closeCurrentTask()
        onDestructiveActionRequested: kind => root.requestDestructiveAction({
                "kind": kind,
                "external": false
            })
        onChromeToggleRequested: root.toggleChrome()
        onFullScreenToggleRequested: root.toggleFullScreen()
        onViewerFocusRequested: root.returnFocusToViewer()
        // Same one-turn delay as the association dialog: the menu is still closing while the
        // item handler runs, and a popup raised inside a closing menu is popped with it.
        onShortcutHelpRequested: Qt.callLater(shortcutHelp.open)
    }

    ReviewShortcuts {
        id: reviewShortcuts

        controller: root.controller
        preferences: root.preferences
        shortcutsEnabled: root.globalMediaShortcutsEnabled
        presentationShortcutsEnabled: root.presentationShortcutsEnabled
        helpShortcutEnabled: root.presentationShortcutsEnabled
        oneSecondStepFrames: root.oneSecondStepFrames
        wipeEnabled: root.wipeMode
        // E1: W/Alt axis keys act only while a real wipe composite is on screen - the shell's
        // canvas focus (direct look) renders the single-source branch instead, where flipping
        // the axis would be invisible tampering. root has no canvasFocusSlot of its own; the
        // shell owns it.
        wipeSplitActive: root.wipeMode && root.shell !== null && root.shell.canvasFocusSlot < 0
        wipeHorizontal: viewportFrame.wipeOrientation === 1
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
        differencePeekAvailable: root.peekDifferenceTask && root.differencePeekTargetValid
        onWipePositionRequested: position => {
            root.wipePosition = position;

            if (!root.chromeVisible)
                root.showImmersiveHud(qsTr("分割线位置 %1%").arg(Math.round(position * 100)));
        }
        onManualNavigationRequested: {
            // The user took the wheel: a restore still waiting for its frame stops here
            // instead of fighting the navigation. The same rule ends an in-flight video
            // tab restore - the manual position is the truth now.
            root.cancelPendingIssueRestore();
            root.cancelPendingVideoTaskSwitch();
            root.revealOsc();

            if (!root.chromeVisible)
                root.manualHudPending = true;
        }
        onChromeToggleRequested: root.toggleChrome()
        onFullScreenToggleRequested: root.toggleFullScreen()
        onPresentationEscapeRequested: {
            // Esc during a held jog ends the walk in every presentation state; the chrome/full
            // screen exit keeps its existing behavior.
            reviewShortcuts.cancelJog();
            root.escapePresentationMode();
        }
        onShortcutHelpRequested: shortcutHelp.open()
        onInPointRequested: root.setInPoint()
        onOutPointRequested: root.setOutPoint()
        onSelectedRangePlaybackRequested: root.playSelectedRange()
        onCandidateSwitchRequested: root.switchCandidateEdge()
        onModeRequested: mode => root.selectComparisonMode(mode)
        onFitRequested: viewportFrame.fitToWindow()
        onNativeSizeRequested: viewportFrame.zoomToNativeSize()
        onNativeFitToggleRequested: viewportFrame.toggleNativeFit()
        onPairLookToggleRequested: root.togglePairLook()
        onZoomInRequested: viewportFrame.zoomViewStep(1.25)
        onZoomOutRequested: viewportFrame.zoomViewStep(0.8)
        onResetViewRequested: viewportFrame.resetView()
        onReferencePeekHoldChanged: held => root.setDifferencePeekHeld(held)
        onReferencePeekLockToggleRequested: root.toggleDifferencePeek(true)
        // E1: wipe split axis. The surface is the single state owner; toggling keeps the
        // split ratio, sources, frame, zoom and pan untouched by construction.
        onWipeOrientationToggleRequested: {
            viewportFrame.setWipeOrientation(viewportFrame.wipeOrientation === 1 ? 0 : 1);
            // One noun and one qualifier vocabulary across the mode bar, the canvas HUD and this
            // toast (UX-10): 分割线 + which halves it divides. The previous text mixed 水平/垂直
            // axis jargon into the same sentence as 上下切分.
            root.showActionFeedback(viewportFrame.wipeOrientation === 1 ? qsTr("分割线：上下切分") : qsTr("分割线：左右切分"));
        }
        onWipeMoveYRequested: delta => {
            // The delta is already a normalized split-ratio step: apply it to the single
            // ratio state directly. Routing it through the outer viewport height and the
            // native surface height mixed two different extents (the frame adds chrome
            // margins the surface does not have), which skewed every step by that ratio.
            const next = Math.max(0, Math.min(1, root.wipePosition + delta));
            root.wipePosition = next;
            if (!root.chromeVisible)
                root.showImmersiveHud(qsTr("分割线位置 %1%").arg(Math.round(next * 100)));
        }
    }

    // The transport's 流畅优先/逐帧完整 toggle routes through reviewShortcuts.actions; the
    // HUD note lives here so the action map stays free of UI plumbing.
    Connections {
        target: reviewShortcuts.actions

        function onPlaybackContinuityPolicyChanged(policy) {
            root.showContinuityPolicyHud(policy);
        }
    }

    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        autoRepeat: false

        // In full screen the presentation Escape (ReviewShortcuts) owns the key and exits

        // full screen first; the drawer can still be dismissed by clicking the scrim.

        // A held jog keeps the docked-chrome Esc (ReviewShortcuts) as the sole owner, so the
        // drawer waits for the next press instead of racing it on the same sequence.
        enabled: root.drawerMode && root.inputContext === 0 && !root.fullScreen && !reviewShortcuts.actions.jogActive
        onActivated: {
            if (root.shell)
                root.shell.inspectorVisible = false;
        }
    }

    // Workspace-level shortcuts stay active across both workspaces; media transport shortcuts
    // remain owned by ReviewShortcuts and are gated by globalMediaShortcutsEnabled. Sequences
    // come from ShortcutCatalog.js - the same constants the menu labels and the help rows use.
    //
    // F10 is the Windows menu key. Qt Quick's MenuBar template exposes no Alt/F10 activation
    // and the custom delegate never entered the tab order, so the menu bar was pointer-only
    // (UX-05). This opens the first menu at its item with the popup focused; arrows, Enter and
    // Esc then behave exactly as in the pointer path. Alt alone is not bound: it would need a
    // key-release state machine and per-language mnemonics the Chinese titles do not carry.
    Shortcut {
        sequence: "F10"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: root.inputContext === 0 && root.chromeVisible && !root.fullScreen
        onActivated: applicationMenuBar.focusFirstMenu()
    }

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
        onActivated: root.requestOpenMediaFiles()
    }

    Shortcut {
        sequence: ShortcutCatalog.addVideo
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0 && root.videoHasSession && root.sourceCount < 3
        onActivated: root.requestAddVideo()
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
        enabled: root.inputContext === 0 && root.imageHasContent && !Boolean(root.activeImageController && root.activeImageController.hasSecondary)
        onActivated: root.requestImageAdd()
    }

    Shortcut {
        sequence: ShortcutCatalog.closeCurrentTask
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: root.inputContext === 0 && root.activeTaskHasMedia
        onActivated: root.closeCurrentTask()
    }

    Shortcut {
        sequence: ShortcutCatalog.nextImageTab
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: root.inputContext === 0 && root.imageWorkspaceActive && root.imageTabCount > 1
        onActivated: root.cycleImageTab(1)
    }

    Shortcut {
        sequence: ShortcutCatalog.previousImageTab
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: root.inputContext === 0 && root.imageWorkspaceActive && root.imageTabCount > 1
        onActivated: root.cycleImageTab(-1)
    }

    // Same sequences as the image-tab pair (video task tabs design §8): the enabled
    // conditions are mutually exclusive on the active workspace, so one key pair cycles
    // whichever task strip is on screen and never reaches the hidden one.
    Shortcut {
        sequence: ShortcutCatalog.nextVideoTab
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: root.inputContext === 0 && !root.imageWorkspaceActive && root.videoTabCount > 1
        onActivated: root.cycleVideoTab(1)
    }

    Shortcut {
        sequence: ShortcutCatalog.previousVideoTab
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: root.inputContext === 0 && !root.imageWorkspaceActive && root.videoTabCount > 1
        onActivated: root.cycleVideoTab(-1)
    }

    Shortcut {
        sequence: ShortcutCatalog.compareImageFolders
        context: Qt.ApplicationShortcut
        enabled: root.inputContext === 0
        onActivated: root.requestCompareFolders()
    }

    // M for "mark": freezes the observation and opens the note dialog; Enter attaches the
    // typed note, Escape attaches an empty one. Active in both workspaces whenever there is
    // something to capture. Never auto-repeats: every activation creates an issue record.
    Shortcut {
        sequence: ShortcutCatalog.captureIssue
        context: Qt.ApplicationShortcut
        autoRepeat: false
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
            if (root.videoTaskHost)
                root.observeVideoTaskIntentEvent(Number(intentId), Number(status), Number(kind));
            if (Number(status) === 5)
                root.showIntentMessage(root.messageCatalog.intentErrorText(error));
            else if (Number(status) === 6)
                root.showIntentMessage(qsTr("更新的请求已取代 %1。").arg(root.messageCatalog.intentKindText(kind, sourceCountValue)));
        }

        function onIntentFinished(intentId, kind, outcome, errorKey) {
            if (Number(outcome) !== 0) {
                if (Number(kind) === 0) {
                    root.pendingNewReviewWantsThreeUp = false;
                    // The loading record whose open failed goes away again (design §2.2
                    // step 4); a loaded record never gets destroyed by a failed reopen.
                    if (root.videoTaskHost && root.pendingVideoTaskOpen && (Number(root.pendingVideoTaskOpen.intentId) === Number(intentId) || Number(root.pendingVideoTaskOpen.intentId) === 0)) {
                        const record = root.pendingVideoTaskOpen;
                        root.pendingVideoTaskOpen = null;
                        root.videoTaskHost.failOpen(Number(record.index), errorKey.length > 0 ? root.messageCatalog.errorMessage(errorKey) : qsTr("无法打开视频。"));
                    }
                    // A failed or superseded sidebar switch adopts nothing; its intent
                    // id must not linger in the adoption list either.
                    root.pendingSidebarInPlaceUrl = "";
                    if (root.videoTaskHost)
                        root.videoStartupOpenIntentIds = root.videoStartupOpenIntentIds.filter(id => Number(id) !== Number(intentId));
                    if (!root.videoFolderModel || !root.videoFolderModel.openPending)
                        workspaceSession.cancelOpen();
                } else if (Number(kind) === 1 || Number(kind) === 2) {
                    root.pendingVideoAppendUrl = "";
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
                // Sidebar in-place disc switch (design §5 rows 5/8): the terminal rebuilt
                // the session on the new material, so the active single-video task's
                // record follows it - same tab, fresh group, state reset; the autoplay
                // itself stays the folder model's own chain.
                if (root.pendingSidebarInPlaceUrl.toString().length > 0) {
                    const switched = root.pendingSidebarInPlaceUrl;
                    root.pendingSidebarInPlaceUrl = "";
                    if (root.videoTaskHost && root.sessionShowsUrls([switched]))
                        root.evolveActiveTaskFromSession(true);
                }
                // Video task tabs: commit the pending host record, or adopt a startup or
                // sidebar open whose intent never had one (design §5 rows 6/9).
                if (root.videoTaskHost)
                    root.settleVideoTaskOpenIntent(Number(intentId));
            } else if (Number(kind) === 1 || Number(kind) === 2) {
                // Group evolution (design §5 row 3): the append chain grew the session;
                // grow the active task's record on the same tab.
                root.applyVideoAppendWriteback();
            } else if (Number(kind) === 3) {
                root.showIntentMessage(qsTr("已移除视频。"));
                // §6/§11-f: removing one source evolves the ACTIVE record in place
                // (3->2, 2->1 on the same tab), keeping the state snapshot.
                if (root.videoTaskHost)
                    root.evolveActiveTaskFromSession(false);
            } else if (Number(kind) === 4) {
                root.showIntentMessage(qsTr("已更换参考源。"));
                if (root.videoTaskHost && root.videoTabActiveIndex >= 0)
                    root.videoTaskHost.setTaskReference(root.videoTabActiveIndex, root.referenceSourceIndex);
            } else if (Number(kind) === 5) {
                root.clearReviewUi();
            }
        }

        // I01: a playback-purpose open (system double-click) asked the transport to start
        // after the first confirmed frame. The refusal is a fact the user must see, not one
        // to swallow now that the open itself succeeded.
        function onOpenPlaybackRequested(intentId, started) {
            if (!started)
                root.showIntentMessage(qsTr("视频已打开，但未能开始播放。"));
        }
    }

    Connections {
        target: root.videoFolderModel

        function onCurrentFileOpened() {
            root.videoFolderSidebarVisible = true;
        }
    }

    Connections {
        target: root.activeImageController

        function onOpenFinished(requestId, pairId, success, error) {
            root.completeImageOpen(requestId, pairId, success, error);
        }

        function onStateChanged() {
            root.continueImageIssueRestore();
        }
    }

    Connections {
        target: root.imageTaskHost

        // Isolated harnesses may provide a minimal host stand-in; only the real host's
        // signals are required for the tab routes exercised below.
        ignoreUnknownSignals: true

        function onActiveChanged() {
            root.handleImageTaskHostActiveChanged();
        }

        function onTaskOpened(index) {
            root.finishOpenedImageTask(index);
        }

        function onOpenFailed(error) {
            root.handleImageTaskOpenFailure(error);
        }

        function onAllTasksClosed() {
            root.handleAllImageTasksClosed();
        }
    }

    Connections {
        target: root.videoTaskHost

        // Same stand-in allowance as the image host: without the composition root's host
        // the legacy single-session paths never emit these signals.
        ignoreUnknownSignals: true

        function onOpenRequested(index, urls, referenceIndex, purpose) {
            root.handleVideoTaskOpenRequested(index, urls, referenceIndex, purpose);
        }

        function onActiveChanged() {
            root.handleVideoTaskActiveChanged();
        }

        function onOpenFailed(error) {
            const detail = String(error || "").length > 0 ? String(error) : qsTr("无法打开视频。");
            root.dropError = detail;
            root.showIntentMessage(detail);
            if (!root.videoHasSession && root.videoTabCount <= 0 && !root.imageHasContent)
                root.focusActiveWorkspace();
        }

        function onAllTasksClosed() {
            root.handleAllVideoTasksClosed();
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
            onMediaFilesAccepted: urls => root.openMediaFilesSelection(urls)
            onMediaFilesRejected: root.focusActiveWorkspace()
            onAddVideoAccepted: url => root.reviewDroppedUrls([url])
            onAddVideoRejected: root.cancelWorkspaceOpen()
            // V02 "替换文件": the identity captured by the chip menu picks the slot; a
            // rejection just returns focus - nothing about the session changed yet.
            onReplaceVideoAccepted: url => root.performSourceReplacement(url)
            onReplaceVideoRejected: root.returnFocusToViewer()
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

    Dialog {
        id: unsavedEditDialog
        objectName: "unsavedImageEditDialog"
        property string taskTitle: ""
        property string sourceLabel: ""
        property bool exiting: false
        property bool closingTask: false
        title: exiting ? qsTr("退出前处理未保存的编辑") : qsTr("图片有未保存的编辑")
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(root.width - 40, 520)
        contentItem: Label {
            text: qsTr("%1 的编辑副本尚未保存。返回查看不会清除这些修改。请选择切回保存、放弃修改或取消。").arg(unsavedEditDialog.taskTitle)
            wrapMode: Text.WordWrap
        }
        footer: Row {
            spacing: 8
            padding: 12
            ReviewActionButton {
                objectName: "unsavedImageEditSaveButton"
                text: qsTr("切回保存…")
                onClicked: {
                    unsavedEditDialog.close();
                    if (root.resumeImageEditForSource(unsavedEditDialog.sourceLabel) && root.imageWorkspace)
                        root.imageWorkspace.saveEditCopy();
                }
            }
            ReviewActionButton {
                objectName: "unsavedImageEditDiscardButton"
                text: unsavedEditDialog.exiting ? qsTr("放弃修改并退出") : (unsavedEditDialog.closingTask ? qsTr("放弃修改并关闭") : qsTr("放弃修改"))
                onClicked: {
                    if (!root.imageEditService || String(root.imageEditService.sourceLabel) !== unsavedEditDialog.sourceLabel) {
                        unsavedEditDialog.close();
                        return;
                    }
                    const ownerIndex = root.imageEditTabIndex;
                    root.imageEditService.endSession();
                    const exiting = unsavedEditDialog.exiting;
                    unsavedEditDialog.close();
                    if (exiting)
                        root.close();
                    else if (unsavedEditDialog.closingTask && ownerIndex >= 0)
                        root.closeImageTab(ownerIndex);
                }
            }
            ReviewActionButton {
                id: cancelUnsavedEditButton
                text: qsTr("取消")
                onClicked: unsavedEditDialog.close()
            }
        }
        onOpened: cancelUnsavedEditButton.forceActiveFocus(Qt.TabFocusReason)
        onClosed: root.focusActiveWorkspace()
    }

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
            radius: Theme.radiusSmall
            color: root.panelColor
            border.color: root.borderColor
        }

        contentItem: Column {
            spacing: 14

            Text {
                text: qsTr("手动对齐锚点")
                color: root.primaryTextColor
                font.pixelSize: Theme.fontGroupTitle
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
                font.pixelSize: Theme.fontSecondary
                font.weight: Font.DemiBold
            }

            Text {
                objectName: "manualOffsetStatusLabel"
                text: root.manualOffsetActive ? qsTr("帧对齐偏移 · 已启用") : qsTr("帧对齐偏移")
                color: root.manualOffsetActive ? Theme.warning : root.mutedTextColor
                font.pixelSize: Theme.fontSecondary
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
                    spacing: Theme.spaceMain

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
                font.pixelSize: Theme.fontCaption
                width: parent.width
                wrapMode: Text.WordWrap
            }

            Text {
                visible: root.compatibilityDetails().length > 0
                text: root.compatibilityDetails()
                color: Theme.warning
                font.pixelSize: Theme.fontCaption
                width: parent.width
                wrapMode: Text.WordWrap
            }
        }
    }

    ActiveSourceStrip {
        id: sourceBar

        // EX-04: the strip is the silent record of what is on screen, so it survives down to a
        // single video instead of disappearing with the second one.
        visible: !root.imageWorkspaceActive && root.sourceCount > 0
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
        // V02: the chip highlights the source the whole canvas is focused on; pure mirror.
        canvasFocusIdentity: root.shell ? root.shell.canvasFocusIdentity : ""
        compactChrome: root.compactChrome
        z: 30
        borderColor: root.borderColor
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        mutedTextColor: root.mutedTextColor
        // UX-13 S4: explicit geometry instead of the old top/left/right anchors, so the
        // compact tier can switch the strip from full-width to its chip-content width
        // without tearing an anchor line mid-flip. The wide values are the exact anchor
        // arithmetic they replace (x = singleMode leftMargin, width = window width minus
        // that margin); the compact value narrows to the letter+dot chips so the mode bar
        // can share the row's remainder. Workspace redesign §2: the source strip is the band
        // closest to the canvas - the command bar sits above it, the tabs above that.
        x: root.singleMode ? 8 : 0
        y: (root.videoTabsStripVisible ? videoTabs.height : 0) + (root.compactChrome ? 0 : comparisonBar.height)
        width: root.compactChrome ? compactRowWidth : root.width - (root.singleMode ? 8 : 0)
        onAddRequested: root.requestAddVideo()
        onCloseSessionRequested: root.closeCurrentTask()
        onRemoveRequested: sourceIdentity => root.removeSelectedSource(sourceIdentity)
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onReplaceRequested: sourceIdentity => root.requestReplaceSource(sourceIdentity)
        // I01: a drop on one chip replaces that chip's source; the strip already resolved
        // which frozen identity the drop names.
        onReplaceDropped: (sourceIdentity, url) => root.dropReplaceSource(sourceIdentity, url)
        // V02 direct look: one plain chip click enters/exits that source's full-canvas view.
        // The shell owns the focus; this handler only forwards the toggle request.
        onDirectLookRequested: sourceSlot => root.toggleDirectLook(sourceSlot)
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

        // EX-03: the comparison tasks stay on screen for a single video (disabled, with the
        // reason printed in the bar) so the way into a comparison is discoverable before the
        // user has a second file.
        visible: !root.imageWorkspaceActive && root.sourceCount > 0
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
        // V02 (P05): the bar mirrors the shell's canvas focus (session slot, -1 = off) and the
        // reference slot so the 看A/看B/看GT checked states name what the canvas shows.
        canvasFocusSlot: root.shell ? root.shell.canvasFocusSlot : -1
        referenceSourceSlot: root.referenceSourceIndex
        compactChrome: root.compactChrome
        borderColor: root.borderColor
        accentColor: root.accentColor
        textColor: root.primaryTextColor
        // UX-13 S4: explicit geometry instead of the old stacked anchors. Wide values are
        // the anchor arithmetic exactly (x = 0, width = window width); compact shares the
        // source strip's top edge and starts one spaceGroup gap right of the strip's chip
        // content, so the two 40 px bars read as one merged row. Workspace redesign §2: the
        // command bar is the band under the task tabs, above the source strip.
        x: root.compactChrome ? sourceBar.x + sourceBar.width + Theme.spaceGroup : 0
        y: root.videoTabsStripVisible ? videoTabs.height : 0
        width: root.compactChrome ? root.width - x : root.width
        onModeRequested: mode => root.selectComparisonMode(mode)
        onEdgeRequested: edge => root.applyDifferenceEdge(edge)
        // qmllint disable unqualified
        onDifferenceViewRequested: metric => {
            root.selectComparisonMode(ComparisonSurface.Difference);
            root.preferences.differenceMetric = metric;
        }
        // qmllint enable unqualified
        onDifferencePeekChanged: held => {
            // A keyboard lock must survive a stray press/release of the mouse button.
            if (held || !root.differencePeekLocked)
                root.differencePeekActive = held && root.differencePeekTargetValid;
        }
        onSwitchCandidateRequested: root.switchCandidateEdge()
        // V02 (P05) full-canvas direct look: one shell action switches which participating
        // source the whole canvas shows. Presentation state only - no seek, no re-pairing, no
        // reference change - so the handler forwards the request and nothing else.
        onFullCanvasFocusRequested: (requestCode, sourceSlot) => {
            if (root.shell)
                root.shell.requestCanvasFocus(requestCode, sourceSlot);
        }
        onCopyComparisonRequested: root.copyComparisonImage()
        onSaveComparisonRequested: root.saveComparisonImage()
        onInspectorRequested: root.shell.inspectorVisible = !root.inspectorOpen
        // EX-03: one add path - the bar's button runs the same action as the menu and Ctrl+Shift+O.
        onAddVideoRequested: root.requestAddVideo()
    }

    // Video task-tab strip (video task tabs design §6 + workspace redesign §2): inside the
    // video workspace the tabs sit directly under the menu bar, above the command chrome -
    // the same stacking the image workspace uses, so a task tab always reads as the window's
    // top-level switch and never as chrome under the source strip.
    readonly property bool videoTabsStripVisible: !root.imageWorkspaceActive && root.chromeVisible && root.videoTabCount > 0
    VideoTaskTabs {
        id: videoTabs

        objectName: "videoTaskTabs"
        visible: root.videoTabsStripVisible
        z: 15
        height: implicitHeight
        tabsModel: root.videoTabList
        activeIndex: root.videoTabActiveIndex
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        onTabActivated: index => root.activateVideoTab(index)
        onTabCloseRequested: index => root.closeVideoTab(index)
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
            // EX-03/EX-04: both chrome rows now stay on screen for a single video, so the
            // drawer starts below them in every video session (both report height 0 when the
            // session is empty). UX-13 S4: the compact tier spends one merged row here.
            // Workspace redesign §2: the task tabs sit above the chrome when present.
            topMargin: (root.videoTabsStripVisible ? videoTabs.height : 0) + root.videoChromeHeight
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
        messageCatalog: root.messageCatalog
        timingDiagnostics: typeof timingDiagnostics !== "undefined" ? timingDiagnostics : null
        session: root.shell
        // U01: the clamped, user-draggable panel width. The component's own default clamps
        // the same tokens, so a standalone instantiation cannot drift out of the band either.
        width: root.clampedInspectorWidth
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
            // EX-03/EX-04: both chrome rows now stay on screen for a single video, so the
            // drawer starts below them in every video session (both report height 0 when the
            // session is empty). UX-13 S4: the compact tier spends one merged row here.
            // Workspace redesign §2: below the task tabs when present.
            topMargin: (root.videoTabsStripVisible ? videoTabs.height : 0) + root.videoChromeHeight
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

    // U01: the docked inspector's resize splitter. Only shown while the inspector actually
    // docks - the drawer overlay does not consume canvas width, so there is nothing to resize.
    MouseArea {
        id: inspectorSplitter

        objectName: "inspectorSplitter"
        visible: root.inspectorDocked
        enabled: visible
        z: 21
        width: 8
        cursorShape: Qt.SizeHorCursor
        anchors {
            right: alignmentBar.left
            rightMargin: -inspectorSplitter.width / 2
            top: alignmentBar.top
            bottom: alignmentBar.bottom
        }

        property real dragStartWidth: 0

        onPressed: mouse => {
            inspectorSplitter.dragStartWidth = root.inspectorPanelWidth;
            mouse.accepted = true;
        }
        onPositionChanged: mouse => {
            if (!inspectorSplitter.pressed)
                return;
            root.inspectorPanelWidth = LayoutMetrics.clampInspectorWidth(inspectorSplitter.dragStartWidth - mouse.x + inspectorSplitter.width / 2);
        }
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
        chromeVisible: root.chromeVisible && root.sourceCount > 0
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
        // V02 (P05): the whole canvas shows the shell-focused source slot (-1 = off). The
        // viewport forwards it to the surface; image workspace is not involved.
        canvasFocusSlot: root.shell ? root.shell.canvasFocusSlot : -1
        referenceSourceIndex: root.referenceSourceIndex
        sourceCount: root.sourceCount
        wipeMode: root.wipeMode
        differenceMode: root.differenceMode
        dualDifferenceMode: root.dualDifferenceMode
        dualDifferenceLegendText: root.dualDifferenceLegendText
        dualDifferenceLegendExact: root.dualDifferenceLegendExact
        dualDifferenceInexactDetail: root.dualDifferenceInexactDetail
        analysisGridMode: root.analysisGridMode
        immersiveHudVisible: root.immersiveHudVisible
        immersiveHudText: root.immersiveHudText
        showFramePending: root.showFramePending
        framePendingText: root.framePendingLabel
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
            topMargin: root.chromeVisible ? root.videoChromeHeight + 6 + (root.videoTabsStripVisible ? videoTabs.height + 4 : 0) : 0
            bottom: parent.bottom
            bottomMargin: root.transportDocked ? root.transportDockHeight : 0
            left: parent.left
            leftMargin: (root.chromeVisible ? LayoutMetrics.canvasEdgeMargin : 0) + root.videoFolderInset
            right: alignmentBar.visible && root.inspectorDocked ? alignmentBar.left : parent.right
            rightMargin: root.chromeVisible ? LayoutMetrics.canvasEdgeMargin : 0
        }
        onWipePositionRequested: position => root.wipePosition = position
        // Immersive mode owns one deliberate affordance for waking the transport: the bottom
        // wake strip. A pointer that merely ends up over the canvas (it was already there when
        // H was pressed, and the collapsing chrome moves the layout under it) must not raise
        // the controls again, so only real gestures carry the request through.
        onOscRevealRequested: interactive => {
            if (root.chromeVisible || interactive)
                root.revealOsc();
        }
        onContextMenuRequested: root.openViewerContextMenu()
        // A dropped ROI marquee (released in the letterbox or too small) reaches the user on the
        // same action-feedback channel as every other chrome action, so it renders with the
        // chrome up as well as on the immersive HUD.
        onRoiRejected: reason => root.showActionFeedback(reason)
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
        objectName: "videoFolderSidebarLoader"
        active: root.videoFolderListVisible
        visible: active
        width: root.clampedSidebarWidth
        z: 36
        anchors.left: parent.left
        anchors.leftMargin: LayoutMetrics.fileSidebarLoaderMargin
        anchors.top: parent.top
        anchors.topMargin: root.imageWorkspaceActive ? 10 : (root.chromeVisible ? (root.videoTabsStripVisible ? videoTabs.height : 0) + root.videoChromeHeight + 6 : 0)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.imageWorkspaceActive ? 0 : (root.transportDocked ? root.transportDockHeight : 0)
        sourceComponent: VideoFolderSidebar {
            folderModel: root.videoFolderModel
            fileActionState: url => root.sidebarVideoActionState(url)
            actionRevision: JSON.stringify([root.shell ? root.shell.activeSources : [], root.referenceSourceIndex, root.busy, root.graphicsReady, root.imageWorkspaceActive, root.videoFolderModel ? root.videoFolderModel.scanning : false, root.shell ? root.shell.activeIntent : {}, root.shell ? root.shell.queuedIntentCount : 0])
            onContextOpenRequested: url => root.openSidebarVideoInNewTab(url)
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

        // Drag delivery is topmost-first across the scene: this content must stack
        // above the window-level workspaceDropArea (z:0) so the A/B source chips'
        // DropAreas receive a drag entered over them; drags over the canvas or empty
        // chrome find no accepting item in this subtree and fall through to the
        // window-level open/append target below.
        z: 1

        anchors {
            top: parent.top
            topMargin: root.chromeVisible ? 10 : 0
            bottom: parent.bottom
            left: parent.left
            leftMargin: (root.chromeVisible ? LayoutMetrics.canvasEdgeMargin : 0) + root.videoFolderInset
            right: parent.right
            rightMargin: root.chromeVisible ? LayoutMetrics.canvasEdgeMargin : 0
        }

        sourceComponent: imageWorkspaceComponent
    }

    Component {
        id: imageWorkspaceComponent

        ImageWorkspace {
            objectName: "imageWorkspaceRoot"
            anchors.fill: parent
            visible: root.imageWorkspaceActive
            controller: root.activeImageController
            imageEdit: root.imageEditService
            imageTabsModel: root.imageTabList
            imageTabsActiveIndex: root.imageTabActiveIndex
            imageTabsDirtyIndex: root.imageTabDirtyIndex
            editStartBlockedReason: root.imageEditStartBlockedReason
            editComparisonTask: root.imageEditComparisonTask
            onCompareEditRequested: root.compareEditedImage()
            onResumeEditRequested: root.resumeOriginalImageEdit()
            pairModel: root.folderPairModel
            folderBrowseActive: root.imageFolderBrowseActive
            folderBrowseText: root.imageFolderBrowseText
            onFolderStepRequested: delta => root.stepImageFolder(delta)
            onFolderJumpRequested: toLast => root.jumpImageFolder(toLast)
            sidebarVisible: root.imageFolderSidebarVisible
            onOpenImageRequested: root.requestImageOpen()
            onAddImageRequested: root.requestImageAdd()
            onOpenPairRequested: root.requestImagePairOpen()
            onCompareFoldersRequested: root.requestCompareFolders()
            onToggleSidebarRequested: root.imageFolderSidebarVisible = !root.imageFolderSidebarVisible
            onSwapSidesRequested: root.requestImageSwapSides()
            onReplacePrimaryRequested: root.requestImageReplacePrimary()
            onReplaceSecondaryRequested: root.requestImageReplaceSecondary()
            onImageTabActivated: index => root.activateImageTab(index)
            onImageTabCloseRequested: index => root.closeImageTab(index)
            onEditStartRefused: reason => root.showIntentMessage(reason)
            // A single image dropped on the A/B identity chip replaces exactly that side
            // through the same in-place swap the replace dialogs use; the other side, the
            // committed pair and the observation position stay put.
            onReplacePrimaryDropped: url => root.performImageSideReplace(url, "a")
            onReplaceSecondaryDropped: url => root.performImageSideReplace(url, "b")
            // I0x: closing one side leaves the other side's task alive. The workspace keeps
            // whatever is left, so the host only has to notice when nothing is left at all
            // and hand the stage back to the video task.
            onCloseSideRequested: sourceSlot => {
                if (root.imageHasContent)
                    return;
                if (root.imageTaskHost && root.imageTabCount > 0) {
                    root.imageTaskHost.closeAt(root.imageTabActiveIndex);
                    return;
                }
                if (root.videoHasSession)
                    root.showWorkspace(workspaceSession.videoMedia);
                else
                    root.focusActiveWorkspace();
            }
            colorClipboard: root.colorClipboardService
            // Workspace redesign §6: the image inspector answers to the shell's shared
            // 检查器 toggle, so one switch spans both workspaces.
            reviewShell: root.shell
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
                root.openImageInNewTab([picked]);
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
    // Comparison-picture save. The controller writes the PNG transactionally and carries the
    // provenance in the file's text metadata; the dialog only picks the destination, so the
    // saved image stays the bare comparison picture.
    NativeDialogs.FileDialog {
        id: comparisonSaveDialog

        objectName: "comparisonSaveDialog"
        title: qsTr("另存为对比图")
        fileMode: NativeDialogs.FileDialog.SaveFile
        nameFilters: [qsTr("PNG 图片 (*.png)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (!root.comparisonExportService || !picked || picked.toString().length === 0)
                return;
            const ok = Boolean(root.comparisonExportService.saveComparison(viewportFrame.comparisonPicture, root.comparisonCaptionLines(), picked));
            root.showActionFeedback(ok ? qsTr("已保存对比图") : qsTr("保存失败：%1").arg(root.comparisonExportService.lastStatus));
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
    // P03 interpolation-protocol manifest import. Loading is asynchronous; outcome and errors
    // surface in the transport's protocol badge.
    NativeDialogs.FileDialog {
        id: protocolLoadDialog

        objectName: "protocolLoadDialog"
        title: qsTr("导入插帧协议")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("插帧协议 (*.json)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (root.protocolModel && picked && picked.toString().length > 0)
                root.protocolModel.loadProtocol(picked);
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
            // The dialog may have been closed on purpose while the export ran, so the outcome
            // must land on a channel that renders with the chrome up - not only the immersive
            // HUD, which stays hidden whenever the chrome is visible.
            root.showActionFeedback(message);

            // A cancel is deliberate and stays silent; a real failure reopens the dialog so the
            // technical detail is readable instead of disappearing with the popup.
            if (!succeeded && root.clipExportDialog && root.clipExportDialog.failureDetail.length > 0)
                root.clipExportDialog.open();
        }
    }
    // qmllint disable incompatible-type
    readonly property SampleExportDialog sampleExportDialog: sampleExportDialogLoader.item
    // qmllint enable incompatible-type

    Loader {
        id: sampleExportDialogLoader

        active: false

        sourceComponent: sampleExportDialogComponent
    }

    Component {
        id: sampleExportDialogComponent

        SampleExportDialog {
            service: root.sampleExportModel
        }
    }
    Connections {
        target: root.sampleExportModel
        enabled: root.sampleExportModel !== null
        function onExportFinished(succeeded, message) {
            // Same channel rule as the clip export above: the dialog may be closed on purpose,
            // so the outcome rides the feedback channel that renders with the chrome up.
            root.showActionFeedback(message);

            if (!succeeded && root.sampleExportDialog && root.sampleExportDialog.lastFailureDetail.length > 0)
                root.sampleExportDialog.open();
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
        radius: Theme.radiusSubtle
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
            spacing: Theme.spaceMain

            Row {
                width: parent.width
                spacing: Theme.spaceMain

                Text {
                    text: qsTr("问题记录")
                    color: root.primaryTextColor
                    font.pixelSize: Theme.fontBody
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
                font.pixelSize: Theme.fontSecondary
                objectName: "issueLogStatusText"
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                visible: root.issueLogModel && root.issueLogModel.lastError.length > 0
                text: root.issueLogModel ? root.issueLogModel.lastError : ""
                color: Theme.errorText
                font.pixelSize: Theme.fontSecondary
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

                    readonly property var issueInfo: root.issueLogModel ? root.issueLogModel.issueAt(issueLogRow.index) : null
                    readonly property bool rowHasInterval: Boolean(issueInfo && issueInfo.hasInterval)

                    width: issueLogList.width
                    height: 48
                    color: Theme.raisedPanel
                    border.color: Theme.border
                    radius: Theme.radiusMicro

                    Column {
                        anchors {
                            left: parent.left
                            leftMargin: 6
                            right: diagButton.visible ? diagButton.left : parent.right
                            rightMargin: 6
                            verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: issueLogRow.issueInfo ? String(issueLogRow.issueInfo.summary || "") : ""
                            color: root.primaryTextColor
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                            width: parent.width
                        }

                        Text {
                            text: issueLogRow.issueInfo ? String(issueLogRow.issueInfo.note || "") : ""
                            color: root.mutedTextColor
                            font.pixelSize: Theme.fontSecondary
                            elide: Text.ElideRight
                            width: parent.width
                        }
                    }

                    ReviewActionButton {
                        id: diagButton

                        objectName: "issueLogDiagnosticButton"
                        visible: issueLogRow.rowHasInterval
                        text: qsTr("诊断卡")
                        implicitHeight: 24
                        implicitWidth: 54
                        anchors {
                            right: parent.right
                            rightMargin: 6
                            verticalCenter: parent.verticalCenter
                        }
                        onClicked: root.openNearbyFramesCard(issueLogRow.index)
                    }

                    MouseArea {
                        anchors {
                            left: parent.left
                            top: parent.top
                            bottom: parent.bottom
                            right: diagButton.visible ? diagButton.left : parent.right
                        }
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
                spacing: Theme.spaceMain

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
        onOpenFilesRequested: root.requestOpenMediaFiles()
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
        protocol: root.protocolModel
        playing: root.playing
        playbackRate: root.controller ? Number(root.controller.playbackRate) : 1
        jogRate: root.controller ? Number(root.controller.jogRate) : 1
        // I8: the transport tooltips tell the truth about the ±1 s buttons - the player
        // preset makes them ±30 s, and the preset comes from the same preference the
        // bindings read.
        shortcutPreset: root.shortcutPreset
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
        // Plan §8 layer 1 right flank: the whole-timeline duration, evaluated at the last
        // frame so it shares the current timecode's vocabulary and drop-frame setting.
        durationText: root.totalTimecode
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
        loopRangePingPongActive: Boolean(root.controller && root.controller.playbackRangePingPong)
        rangeExportVisible: root.rangeExportVisible
        rangeExportEnabled: root.rangeExportEnabled
        rangeExportBusy: root.clipExportService !== null && Boolean(root.clipExportService.busy)
        rangeExportProgress: root.clipExportService !== null ? Number(root.clipExportService.progress) : 0
        sampleExport: root.sampleExportModel
        sampleExportBusy: root.sampleExportModel !== null && Boolean(root.sampleExportModel.busy)
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
        playbackContinuityPolicy: root.controller ? root.controller.playbackContinuityPolicyEffective : 1
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
        onRangePingPongRequested: root.toggleRangePingPong()
        onClearRangeRequested: root.clearSelectedRange()
        onExportRangeRequested: root.openClipExportDialog()
        onExportSampleRequested: root.openSampleExportDialog(root.currentFrame)
        // P03 protocol navigation intents. Loading goes through the file dialog so the import is
        // an explicit user action; jumps ride the controller's seekFrame() (the existing
        // precise-frame command chain), never a direct playback mutation.
        onProtocolLoadRequested: {
            if (root.protocolModel)
                root.openProtocolLoadDialog();
        }
        onProtocolClearRequested: {
            if (root.protocolModel)
                root.protocolModel.clearProtocol();
        }
        onPreviousGeneratedFrameRequested: {
            if (root.protocolModel)
                root.protocolModel.previousGeneratedFrame();
        }
        onNextGeneratedFrameRequested: {
            if (root.protocolModel)
                root.protocolModel.nextGeneratedFrame();
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
        imagePreset: root.imageWorkspaceActive
        mediaEntries: root.imageWorkspaceActive ? [] : reviewShortcuts.helpEntries
        workspaceEntries: root.workspaceHelpEntries
    }

    // I03: the file-association dialog must live in the window's item tree. It used to be
    // declared inside ApplicationMenuBar, but a MenuBar only parents Menu children into the
    // scene, so the dialog item never reached a window and its popup silently never opened.
    // qmllint disable unqualified
    readonly property var fileAssociationService: typeof fileAssociation !== "undefined" ? fileAssociation : null
    // qmllint enable unqualified

    FileAssociationDialog {
        id: fileAssociationDialog

        service: root.fileAssociationService
    }

    NearbyFramesCard {
        id: nearbyFramesCard

        objectName: "nearbyFramesCard"
        visible: root.nearbyFramesCardVisible && !root.imageWorkspaceActive && root.activeDiagnosticIssueIndex >= 0
        z: 55
        width: Math.min(root.width - 32, implicitWidth)
        height: implicitHeight
        anchors {
            horizontalCenter: parent.horizontalCenter
            bottom: transport.top
            bottomMargin: 16
        }
        issueIndex: root.activeDiagnosticIssueIndex
        issueData: root.issueLogModel && root.activeDiagnosticIssueIndex >= 0 ? root.issueLogModel.issueAt(root.activeDiagnosticIssueIndex) : null
        intervalData: root.issueLogModel && root.activeDiagnosticIssueIndex >= 0 ? root.issueLogModel.intervalAt(root.activeDiagnosticIssueIndex) : null
        protocolController: root.protocolModel
        reviewController: root.controller
        previewThumbnails: root.previewThumbnailsModel
        sampleExportController: root.sampleExportModel
        currentFrame: Number(root.currentFrame)
        totalFrames: Number(root.totalFrames)
        // Same-ROI consecutive strip (V04): the shared ROI of the main canvas, in normalized
        // source coordinates straight off the comparison surface.
        roiEnabled: root.roiEnabled
        roiLeft: root.surfaceRoiLeft
        roiTop: root.surfaceRoiTop
        roiRight: root.surfaceRoiRight
        roiBottom: root.surfaceRoiBottom

        onCloseRequested: root.nearbyFramesCardVisible = false
        onSeekRequested: frame => {
            root.revealOsc();
            if (root.controller)
                root.controller.seekFrame(frame);
        }
        onLoopIntervalRequested: (inF, outF) => root.playIntervalRange(inF, outF)
        onStepFrameRequested: delta => {
            root.revealOsc();
            if (root.controller)
                root.controller.stepFrames(delta);
        }
        onExportSampleRequested: frame => root.openSampleExportDialog(frame)
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

        onSideRequested: root.selectComparisonMode(ComparisonSurface.SideBySide)
        onWipeRequested: root.selectComparisonMode(ComparisonSurface.Wipe)
        onDiffRequested: root.selectComparisonMode(ComparisonSurface.Difference)

        // qmllint enable unqualified

        onEdgeRequested: edge => root.applyDifferenceEdge(edge)
        onReferenceRequested: sourceIdentity => root.changeReference(sourceIdentity)
        onOpenRequested: root.requestOpenVideos()
        onInspectorRequested: root.shell.inspectorVisible = true
        onFullScreenRequested: root.toggleFullScreen()
        onViewerFocusRequested: root.returnFocusToViewer()
    }

    // Transient notices share one stack above the stage's own bottom controls and a floating
    // transport, so they never cover the header toolbars. With the chrome hidden the stack sits
    // under the restore pill, clear of the immersive HUD.
    //
    // Aligned to the stage's right edge, not centred: the comparison divider is the one column
    // a reviewer reads continuously, and a centred notice sat exactly across it for as long as
    // the message lasted (UX-14). On the right, a transient covers one source's corner instead
    // of the seam the two sources are being judged against.
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
        spacing: Theme.spaceMain
        // Right edge of the stage, inset 16; the children anchor to `parent.right` so a narrow
        // toast does not drift back towards the divider.
        x: Math.round((root.chromeVisible ? notificationStack.stageRight : root.width) - notificationStack.width - 16)
        y: Math.round(root.chromeVisible ? notificationStack.stageFloor - 12 - notificationStack.height : chromeRestorePill.y + chromeRestorePill.height + 10)

        Rectangle {
            visible: root.intentMessage.length > 0
            width: Math.min(notificationStack.noticeWidth, intentToastText.implicitWidth + 32)
            height: intentToastText.paintedHeight + 20
            radius: Theme.radiusLarge
            color: Theme.oscGlass
            border.color: Theme.oscBorder
            anchors.right: parent.right

            Text {
                id: intentToastText

                anchors.centerIn: parent
                width: Math.max(0, parent.width - 32)
                text: root.intentMessage
                color: root.primaryTextColor
                font.pixelSize: Theme.fontBody
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
            anchors.right: parent.right

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
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                    width: parent.width
                }

                Row {
                    visible: intentQueuePanel.queued.length > 0
                    width: parent.width

                    Text {
                        text: qsTr("%1 个请求排队中").arg(intentQueuePanel.queued.length)
                        color: root.mutedTextColor
                        font.pixelSize: Theme.fontSecondary
                        width: parent.width - cancelAllButton.width
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    VcsToolButton {
                        id: cancelAllButton

                        text: qsTr("全部取消")
                        implicitWidth: 74
                        implicitHeight: Theme.hitAreaMinimum
                        labelPixelSize: Theme.fontSecondary
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
                            font.pixelSize: Theme.fontSecondary
                            elide: Text.ElideRight
                            width: parent.width - cancelQueuedButton.width
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        VcsToolButton {
                            id: cancelQueuedButton

                            text: qsTr("取消")
                            implicitWidth: 58
                            implicitHeight: Theme.hitAreaMinimum
                            labelPixelSize: Theme.fontSecondary
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
            anchors.right: parent.right

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
                    font.pixelSize: Theme.fontBody
                    anchors.verticalCenter: parent.verticalCenter
                }
                ReviewActionButton {
                    text: qsTr("取消")
                    implicitWidth: 58
                    implicitHeight: Theme.hitAreaMinimum
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
            anchors.right: parent.right

            Text {
                id: dropErrorText

                width: parent.width - 34
                text: root.dropError
                color: Theme.errorText
                font.pixelSize: Theme.fontBody
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

        // With the chrome hidden (H / presentation mode) every menu and bar disappears and the

        // immersive HUD fades after 800 ms. This persistent pill is the always-visible way back:

        // it names the H shortcut and restores the interface on click.

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
                duration: Theme.animationFast
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
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: qsTr("界面已隐藏 · 按 H 或点击恢复")
                color: root.primaryTextColor
                font.pixelSize: Theme.fontSecondary
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
            // While the chrome is hidden this band is the one *passive* affordance that wakes
            // the transport: canvas hover stays quiet there (see onOscRevealRequested), and the
            // gesture paths (press / drag / wheel) wake it without the pointer ever reaching the
            // band. The objectName lets the contract suite read the hover state instead of
            // guessing from where the physical cursor happens to sit.
            objectName: "immersiveWakeHover"

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
                font.pixelSize: Theme.fontDropOverlayTitle
                font.bold: true
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                // I01: the overlay must tell the two drop intents apart - a drop on the empty
                // canvas opens/appends, a drop on a source chip replaces that source.
                text: qsTr("1–3 个视频或图片 · 拖到源卡片上替换该源")
                color: root.mutedTextColor
                font.pixelSize: Theme.fontBody
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }
}
