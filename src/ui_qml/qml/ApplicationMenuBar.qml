pragma ComponentBehavior: Bound

import QtQuick
import "ShortcutCatalog.js" as ShortcutCatalog
import "VcsTheme.js" as Theme

VcsMenuBar {
    id: control

    // A row that states a fact instead of offering an action (version, supported formats). A
    // disabled MenuItem would be the wrong vehicle: grey rows read as "temporarily broken" and
    // can be focused by nobody. This keeps the menu's 35 px rhythm with no hover and no trigger.
    component MenuNote: Item {
        id: note

        property string noteText: ""

        implicitWidth: 0
        implicitHeight: noteLabel.implicitHeight + 14
        // A hidden note must cost nothing, exactly like VcsMenuItem's hidden rows: the menu
        // layout reads `height`, so leaving it at implicitHeight would reserve a blank band.
        height: visible ? implicitHeight : 0

        Text {
            id: noteLabel

            anchors {
                left: parent.left
                leftMargin: 12
                right: parent.right
                rightMargin: 12
                verticalCenter: parent.verticalCenter
            }
            text: note.noteText
            color: Theme.mutedText
            font.pixelSize: Theme.fontCaption
            lineHeight: Theme.lineHeightCompact
            wrapMode: Text.Wrap
        }
    }

    required property var controller
    required property var preferences
    required property var session
    required property int sourceCount
    required property bool busy
    required property int canonicalSourceIndex
    property int referenceSourceIndex: 0
    property int effectiveDifferenceEdge: 0
    required property int currentViewMode
    required property bool inspectorOpen
    required property bool graphicsReady
    required property int currentFrame
    required property bool alignmentAnalysisRunning
    required property bool chromeVisible
    required property bool fullScreen
    required property int shortcutPreset
    // I01: mirrors the preference like shortcutPreset does - a plain value binding, so the
    // check mark tracks preferencesChanged; the trigger writes through control.preferences.
    required property bool autoplayOnOpen
    required property var sourceIdentities
    // C-07 effective/requested continuity code (0 ReviewEveryFrame, 1 RealTime, 2 Contextual).
    property int playbackContinuityPolicy: 2
    property int workspaceMode: 0
    property bool imageHasPrimary: false
    property bool imageHasSecondary: false
    property bool videoFolderAvailable: false
    property bool videoFolderVisible: false
    property bool videoHasSession: false
    property bool imageHasSession: false
    // Alignment state and helpers live on the root; the Analyze menu hosts the actions that used
    // to live in the inspector's Alignment tab.
    property bool automaticAlignmentPending: false
    property bool canConfirmAutomaticAlignment: false
    property bool canUndoAutomaticAlignment: false
    property bool manualAnchorActive: false
    property bool anyManualAlignmentActive: false
    property bool autoAlignmentActive: false
    property var openManualAnchorsDialog: null
    property var sourceOffsets: null
    property var resetSourceOffsets: null

    signal openVideoFolderRequested
    signal videoFolderToggleRequested
    signal openFilesRequested
    signal addVideoRequested
    signal addImageRequested
    signal openImagePairRequested
    signal compareImageFoldersRequested
    signal fileAssociationDialogRequested
    signal issueLogCaptureRequested
    signal issueLogToggleRequested
    signal issueLogSaveRequested
    signal issueLogLoadRequested
    signal workspaceRequested(int mode)
    signal closeCurrentRequested
    signal destructiveActionRequested(string kind)
    signal chromeToggleRequested
    signal fullScreenToggleRequested
    signal viewerFocusRequested
    signal shortcutHelpRequested

    property bool returnViewerFocusAfterClose: false
    readonly property bool anyMenuOpen: fileMenu.opened || compareMenu.opened || analyzeMenu.opened || viewMenu.opened || settingsMenu.opened || helpMenu.opened

    // F10 entry point. Qt Quick's MenuBar has no Alt/F10 activation of its own (the template
    // only exposes delegate/menus/contentData/menuAt), so the window owns the key and this
    // function opens the first menu the same way a click would: at the item, with the popup
    // focused. Up/Down/Enter/Esc then behave exactly as in the pointer path.
    function focusFirstMenu() {
        if (control.count <= 0)
            return false;
        const item = control.itemAt(0);
        if (!item || !item.menu || !item.enabled)
            return false;
        item.menu.popup(item, 0, item.height);
        item.menu.forceActiveFocus();
        return true;
    }

    function changeReferenceByIndex(sourceIndex) {
        if (!control.session || sourceIndex < 0 || sourceIndex >= control.sourceIdentities.length)
            return false;
        return control.session.changeReferenceByIdentity(String(control.sourceIdentities[sourceIndex]));
    }

    VcsMenu {
        id: fileMenu

        objectName: "fileMenu"
        title: qsTr("文件")
        onClosed: {
            if (control.returnViewerFocusAfterClose) {
                control.returnViewerFocusAfterClose = false;
                control.viewerFocusRequested();
            }
        }

        VcsMenuItem {
            objectName: "openFilesMenuItem"
            text: qsTr("打开文件…")
            shortcutText: ShortcutCatalog.openVideos
            onTriggered: control.openFilesRequested()
        }
        VcsMenuItem {
            objectName: "openVideoFolderMenuItem"
            text: qsTr("打开文件夹…")
            shortcutText: ShortcutCatalog.openVideoFolder
            enabled: control.videoFolderAvailable
            onTriggered: control.openVideoFolderRequested()
        }
        VcsMenuItem {
            text: qsTr("添加视频…")
            shortcutText: ShortcutCatalog.addVideo
            enabled: control.sourceCount > 0 && control.sourceCount < 3
            onTriggered: control.addVideoRequested()
        }
        VcsMenuSeparator {
            objectName: "fileImageSeparator"
        }
        VcsMenuItem {
            objectName: "openImagePairMenuItem"
            text: qsTr("打开图片对…")
            shortcutText: ShortcutCatalog.openImagePair
            onTriggered: control.openImagePairRequested()
        }
        VcsMenuItem {
            objectName: "addImageMenuItem"
            text: qsTr("添加图片…")
            shortcutText: ShortcutCatalog.addImage
            enabled: control.workspaceMode === 1 && control.imageHasPrimary && !control.imageHasSecondary
            onTriggered: control.addImageRequested()
        }
        VcsMenuItem {
            objectName: "compareImageFoldersMenuItem"
            text: qsTr("对比文件夹…")
            shortcutText: ShortcutCatalog.compareImageFolders
            onTriggered: control.compareImageFoldersRequested()
        }
        VcsMenuSeparator {
            objectName: "fileIssueSeparator"
        }
        VcsMenuItem {
            objectName: "captureIssueMenuItem"
            text: qsTr("记录当前问题")
            shortcutText: ShortcutCatalog.captureIssue
            onTriggered: control.issueLogCaptureRequested()
        }
        VcsMenuItem {
            objectName: "toggleIssueLogMenuItem"
            text: qsTr("问题记录面板")
            onTriggered: control.issueLogToggleRequested()
        }
        VcsMenuItem {
            objectName: "saveIssueLogMenuItem"
            text: qsTr("保存问题记录…")
            onTriggered: control.issueLogSaveRequested()
        }
        VcsMenuItem {
            objectName: "loadIssueLogMenuItem"
            text: qsTr("加载问题记录…")
            onTriggered: control.issueLogLoadRequested()
        }
        VcsMenuItem {
            objectName: "closeCurrentMenuItem"
            text: control.workspaceMode === 1 ? qsTr("关闭图片工具") : qsTr("关闭视频")
            shortcutText: ShortcutCatalog.closeCurrentTask
            enabled: control.workspaceMode === 1 ? control.imageHasSession : control.videoHasSession
            onTriggered: {
                control.closeCurrentRequested();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {}
        VcsMenuItem {
            text: qsTr("退出")
            shortcutText: ShortcutCatalog.quit
            onTriggered: {
                control.destructiveActionRequested("exit");
                control.returnViewerFocusAfterClose = true;
            }
        }
    }

    VcsMenu {
        id: compareMenu
        objectName: "compareMenu"
        title: qsTr("对比")
        // No menu-level `enabled`. This menu also owns playback continuity and the inspector
        // toggle, which are useful with a single video; a disabled parent used to make those
        // unreachable while greying them out for a reason that had nothing to do with them
        // (UX-09). Comparison rows carry their own source-count gate instead.
        onClosed: {
            if (control.returnViewerFocusAfterClose) {
                control.returnViewerFocusAfterClose = false;
                control.viewerFocusRequested();
            }
        }

        // The comparison rows are gated per row, so the menu has to say *why* they are grey -
        // otherwise the user gets three dead rows and no reason, which is the failure mode this
        // whole change was meant to remove. The reason differs by state, so the note does too:
        // an image session, an empty window and a single video are not the same situation.
        MenuNote {
            objectName: "compareGateNote"

            visible: control.workspaceMode === 1 || control.sourceCount < 2
            noteText: control.workspaceMode === 1 ? qsTr("图片工作区的左右对比在图片工具里，不使用这三个视频对比方式。") : (control.sourceCount === 0 ? qsTr("还没有打开视频。") : qsTr("对比方式需要至少两个视频。当前只打开了一个。"))
        }

        VcsRadioMenuItem {
            objectName: "sideBySideMenuItem"
            text: qsTr("并排")
            enabled: control.videoHasSession && control.sourceCount > 1
            checked: control.currentViewMode === 0
            onTriggered: {
                control.preferences.viewMode = 0;
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsRadioMenuItem {
            objectName: "wipeMenuItem"
            text: qsTr("分割线")
            enabled: control.videoHasSession && control.sourceCount > 1
            checked: control.currentViewMode === 5
            onTriggered: {
                control.preferences.viewMode = 5;
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsRadioMenuItem {
            objectName: "differenceMenuItem"
            text: qsTr("差异")
            enabled: control.videoHasSession && control.sourceCount > 1
            checked: control.currentViewMode === 3
            onTriggered: {
                control.preferences.viewMode = 3;
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {
            objectName: "compareModeSeparator"
        }
        VcsMenu {
            id: layoutMenu

            objectName: "layoutMenu"
            title: qsTr("布局")
            menuItemVisible: control.sourceCount === 3
            menuItemEnabled: control.sourceCount === 3

            VcsRadioMenuItem {
                objectName: "threeUpMenuItem"
                text: qsTr("三联")
                checked: control.currentViewMode === 1
                onTriggered: {
                    control.preferences.viewMode = 1;
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("参考聚焦")
                checked: control.currentViewMode === 2
                onTriggered: {
                    control.preferences.viewMode = 2;
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                objectName: "analysisGridMenuItem"
                text: qsTr("分析网格")
                checked: control.currentViewMode === 4
                enabled: control.sourceCount === 3
                onTriggered: {
                    control.preferences.viewMode = 4;
                    control.returnViewerFocusAfterClose = true;
                }
            }
        }
        VcsMenu {
            id: pairMenu

            objectName: "pairMenu"
            title: qsTr("对比对")
            menuItemVisible: control.sourceCount === 3
            menuItemEnabled: control.sourceCount === 3

            VcsRadioMenuItem {
                text: qsTr("A / B")
                // D07: checked state is the committed effective pair, not the preference slot.
                checked: Number(control.effectiveDifferenceEdge) === 0
                onTriggered: {
                    if (control.controller && control.controller.applyComparisonPairFromEdge) {
                        control.controller.applyComparisonPairFromEdge(0, Number(control.preferences.defaultPairPolicy));
                    }
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("A / C")
                checked: Number(control.effectiveDifferenceEdge) === 1
                onTriggered: {
                    if (control.controller && control.controller.applyComparisonPairFromEdge) {
                        control.controller.applyComparisonPairFromEdge(1, Number(control.preferences.defaultPairPolicy));
                    }
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("B / C")
                checked: Number(control.effectiveDifferenceEdge) === 2
                onTriggered: {
                    if (control.controller && control.controller.applyComparisonPairFromEdge) {
                        control.controller.applyComparisonPairFromEdge(2, Number(control.preferences.defaultPairPolicy));
                    }
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsMenu {
                id: pairPolicyMenu

                objectName: "defaultPairPolicyMenu"
                title: qsTr("源对策略")
                // Persist even without a live session so the next open uses this policy.
                menuItemEnabled: true

                VcsRadioMenuItem {
                    objectName: "pairPolicyReferenceAndFirst"
                    text: qsTr("基准 + 首个候选")
                    checked: Number(control.preferences.defaultPairPolicy) === 0
                    onTriggered: {
                        control.preferences.defaultPairPolicy = 0;
                        // D07: apply policy only; never replay a stored A/B/C edge ordinal.
                        if (control.controller && control.controller.applyDefaultPairPolicy) {
                            control.controller.applyDefaultPairPolicy(0);
                        }
                        control.returnViewerFocusAfterClose = true;
                    }
                }
                VcsRadioMenuItem {
                    objectName: "pairPolicyLastTwo"
                    text: qsTr("最后两个源")
                    checked: Number(control.preferences.defaultPairPolicy) === 1
                    onTriggered: {
                        control.preferences.defaultPairPolicy = 1;
                        if (control.controller && control.controller.applyDefaultPairPolicy) {
                            control.controller.applyDefaultPairPolicy(1);
                        }
                        control.returnViewerFocusAfterClose = true;
                    }
                }
                VcsRadioMenuItem {
                    objectName: "pairPolicyPreserve"
                    text: qsTr("尽量保留当前源对")
                    checked: Number(control.preferences.defaultPairPolicy) === 2
                    onTriggered: {
                        control.preferences.defaultPairPolicy = 2;
                        if (control.controller && control.controller.applyDefaultPairPolicy) {
                            control.controller.applyDefaultPairPolicy(2);
                        }
                        control.returnViewerFocusAfterClose = true;
                    }
                }
            }
        }
        VcsMenu {
            id: continuityMenu

            objectName: "playbackContinuityMenu"
            title: qsTr("播放连续性")
            menuItemEnabled: control.videoHasSession

            VcsRadioMenuItem {
                objectName: "continuityReviewEveryFrame"
                // One name per policy across the switch, this menu, the status rail and the HUD
                // note; the trade-off rides in the label tail instead of renaming the mode.
                text: qsTr("逐帧完整 · 每一帧都显示")
                checked: Number(control.playbackContinuityPolicy) === 0
                onTriggered: {
                    control.preferences.playbackContinuityPolicy = 0;
                    if (control.controller && control.controller.setPlaybackContinuityPolicy) {
                        control.controller.setPlaybackContinuityPolicy(0);
                    }
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                objectName: "continuityRealTime"
                text: qsTr("流畅优先 · 落后时整组跳帧")
                checked: Number(control.playbackContinuityPolicy) === 1
                onTriggered: {
                    control.preferences.playbackContinuityPolicy = 1;
                    if (control.controller && control.controller.setPlaybackContinuityPolicy) {
                        control.controller.setPlaybackContinuityPolicy(1);
                    }
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                objectName: "continuityContextual"
                text: qsTr("按会话自适应")
                checked: Number(control.playbackContinuityPolicy) === 2
                onTriggered: {
                    control.preferences.playbackContinuityPolicy = 2;
                    if (control.controller && control.controller.setPlaybackContinuityPolicy) {
                        control.controller.setPlaybackContinuityPolicy(2);
                    }
                    control.returnViewerFocusAfterClose = true;
                }
            }
        }
        VcsMenu {
            id: referenceMenu

            objectName: "referenceMenu"
            title: qsTr("参考源")
            enabled: control.sourceCount > 1

            VcsRadioMenuItem {
                text: qsTr("视频 A")
                // D08: checked by comparison reference, not timeline master.
                checked: control.referenceSourceIndex === 0
                onTriggered: {
                    control.changeReferenceByIndex(0);
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("视频 B")
                checked: control.referenceSourceIndex === 1
                onTriggered: {
                    control.changeReferenceByIndex(1);
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("视频 C")
                visible: control.sourceCount > 2
                checked: control.referenceSourceIndex === 2
                onTriggered: {
                    control.changeReferenceByIndex(2);
                    control.returnViewerFocusAfterClose = true;
                }
            }
        }
        VcsMenuSeparator {
            objectName: "compareInspectorSeparator"
        }
        VcsMenuItem {
            objectName: "compareInspectorMenuItem"
            text: control.inspectorOpen ? qsTr("隐藏检查器") : qsTr("显示检查器")
            onTriggered: {
                control.session.inspectorVisible = !control.inspectorOpen;
                control.returnViewerFocusAfterClose = true;
            }
        }
    }

    VcsMenu {
        id: analyzeMenu
        objectName: "analyzeMenu"
        title: qsTr("分析")
        enabled: control.sourceCount > 1 && control.graphicsReady && control.currentFrame >= 0 && !control.busy
        onClosed: {
            if (control.returnViewerFocusAfterClose) {
                control.returnViewerFocusAfterClose = false;
                control.viewerFocusRequested();
            }
        }

        VcsMenuItem {
            text: qsTr("估算全局帧偏移")
            enabled: control.graphicsReady && !control.busy && !control.alignmentAnalysisRunning && Boolean(control.controller && control.controller.canFirst)
            onTriggered: {
                control.controller.estimateAlignment();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuItem {
            text: control.alignmentAnalysisRunning ? qsTr("取消分析") : qsTr("分析缺失 / 重复帧")
            enabled: control.graphicsReady && !control.busy && Boolean(control.controller && (control.alignmentAnalysisRunning || control.controller.canFirst))
            onTriggered: {
                control.alignmentAnalysisRunning ? control.controller.cancelAlignmentAnalysis() : control.controller.analyzeSequenceAlignment();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {
            objectName: "analyzeAutomaticSeparator"
        }
        VcsMenuItem {
            objectName: "confirmAutomaticAlignmentMenuItem"
            text: control.canConfirmAutomaticAlignment ? qsTr("确认建议映射") : qsTr("确认前先分析序列")
            visible: control.automaticAlignmentPending
            enabled: control.graphicsReady && !control.busy && !control.alignmentAnalysisRunning && control.canConfirmAutomaticAlignment
            onTriggered: {
                control.controller.confirmAutomaticAlignment();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuItem {
            objectName: "undoAutomaticAlignmentMenuItem"
            text: qsTr("撤销自动映射")
            visible: control.canUndoAutomaticAlignment
            enabled: control.graphicsReady && !control.busy && !control.alignmentAnalysisRunning
            onTriggered: {
                control.controller.undoAutomaticAlignment();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuItem {
            objectName: "manualAnchorsButton"
            text: control.manualAnchorActive ? qsTr("已设手动锚点…") : qsTr("编辑手动锚点…")
            enabled: control.graphicsReady && !control.busy && Boolean(control.controller && control.controller.canFirst)
            onTriggered: {
                control.openManualAnchorsDialog();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {
            objectName: "analyzeOffsetSeparator"
        }
        VcsMenuItem {
            objectName: "applyAlignmentMenuItem"
            text: qsTr("应用帧偏移")
            enabled: control.graphicsReady && !control.busy && Boolean(control.controller && control.controller.canFirst)
            onTriggered: {
                control.controller.applySourceOffsets(control.sourceOffsets());
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuItem {
            objectName: "resetAlignmentMenuItem"
            text: qsTr("回到严格索引")
            enabled: control.graphicsReady && !control.busy && (control.anyManualAlignmentActive || control.autoAlignmentActive) && Boolean(control.controller && control.controller.canFirst)
            onTriggered: {
                control.resetSourceOffsets();
                control.controller.applySourceOffsets(control.sourceOffsets());
                control.returnViewerFocusAfterClose = true;
            }
        }
    }

    VcsMenu {
        id: viewMenu

        objectName: "viewMenu"
        title: qsTr("视图")
        onClosed: {
            if (control.returnViewerFocusAfterClose) {
                control.returnViewerFocusAfterClose = false;
                control.viewerFocusRequested();
            }
        }

        VcsMenuItem {
            text: control.chromeVisible ? qsTr("隐藏界面 · H") : qsTr("显示界面 · H")
            onTriggered: {
                control.chromeToggleRequested();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuItem {
            objectName: "toggleVideoFolderMenuItem"
            text: control.videoFolderVisible ? qsTr("隐藏文件与历史侧栏") : qsTr("显示文件与历史侧栏")
            enabled: control.videoFolderAvailable
            onTriggered: control.videoFolderToggleRequested()
        }
        VcsMenuItem {
            text: control.fullScreen ? qsTr("退出全屏 · F11") : qsTr("进入全屏 · F11")
            onTriggered: {
                control.fullScreenToggleRequested();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {}
        VcsRadioMenuItem {
            objectName: "workspaceVideoMenuItem"
            text: qsTr("视频对比")
            checked: control.workspaceMode === 0
            onTriggered: {
                control.workspaceRequested(0);
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsRadioMenuItem {
            objectName: "workspaceImageMenuItem"
            text: qsTr("图片工具")
            checked: control.workspaceMode === 1
            onTriggered: {
                control.workspaceRequested(1);
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {}
    }

    // I03: the Windows default-open entry point. The controller context property only exists
    // when the composition root injected the association service, and without it the entry
    // stays hidden like the other optional services. The dialog itself must NOT be declared
    // here: a MenuBar only parents Menu children into the scene, so a dialog item declared in
    // it never reaches a window and its popup silently never opens. Main.qml owns the dialog
    // and opens it on fileAssociationDialogRequested.
    // qmllint disable unqualified
    readonly property var fileAssociationService: typeof fileAssociation !== "undefined" ? fileAssociation : null
    // qmllint enable unqualified

    VcsMenu {
        id: settingsMenu

        objectName: "settingsMenu"
        title: qsTr("设置")
        // A top-level menu never appears and disappears with the runtime environment (UX-08).
        // Whether the association service exists now decides one row's enabled state, not the
        // existence of the whole menu - a menu bar whose shape depends on which services the
        // build injected is unpredictable.
        //
        // The preference rows that used to live under 视图 and 对比 moved here so "where do I
        // change X" has one answer: shortcuts, autoplay and file association are all settings,
        // not views and not comparisons.

        VcsMenuItem {
            objectName: "fileAssociationMenuItem"
            text: qsTr("文件关联…")
            enabled: control.fileAssociationService !== null
            onTriggered: control.fileAssociationDialogRequested()
        }
        VcsMenuSeparator {}
        VcsMenu {
            id: shortcutPresetMenu

            objectName: "shortcutPresetMenu"
            title: qsTr("快捷键方案")

            VcsRadioMenuItem {
                text: qsTr("逐帧检查")
                checked: control.shortcutPreset === 0
                onTriggered: {
                    control.preferences.shortcutPreset = 0;
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("播放器")
                checked: control.shortcutPreset === 1
                onTriggered: {
                    control.preferences.shortcutPreset = 1;
                    control.returnViewerFocusAfterClose = true;
                }
            }
        }
        // I01: the user's off switch for intent-driven autoplay. A double-clicked video plays
        // after its first confirmed frame while this stays checked; unchecked, every open
        // lands paused like a review open.
        VcsMenuItem {
            objectName: "autoplayOnOpenMenuItem"
            checkable: true
            checked: control.autoplayOnOpen
            text: qsTr("双击打开后自动播放")
            onTriggered: {
                control.preferences.autoplayOnOpen = !control.preferences.autoplayOnOpen;
                control.returnViewerFocusAfterClose = true;
            }
        }
    }

    // Help has its own top-level menu: the shortcut table was already written, tested and
    // unreachable outside a video session (UX-07), and version/formats had no home at all.
    // The one action opens the same overlay the ? key does; the notes state facts instead of
    // pretending to be commands.
    VcsMenu {
        id: helpMenu

        objectName: "helpMenu"
        title: qsTr("帮助")
        onClosed: {
            if (control.returnViewerFocusAfterClose) {
                control.returnViewerFocusAfterClose = false;
                control.viewerFocusRequested();
            }
        }

        VcsMenuItem {
            objectName: "shortcutHelpMenuItem"
            text: qsTr("快捷键总表…")
            onTriggered: {
                control.shortcutHelpRequested();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {}
        MenuNote {
            noteText: qsTr("视频 MP4 · MKV · MOV · AVI · M4V\n图片 PNG · JPG · TIFF · BMP · GIF · WebP · PNM")
        }
        MenuNote {
            noteText: qsTr("CompareStation %1 · 视觉审查").arg(Qt.application.version)
        }
    }
}
