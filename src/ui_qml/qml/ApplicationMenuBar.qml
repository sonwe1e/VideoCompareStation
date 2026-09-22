pragma ComponentBehavior: Bound

import QtQuick

VcsMenuBar {
    id: control

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
    required property var sourceIdentities
    // C-07 effective/requested continuity code (0 ReviewEveryFrame, 1 RealTime, 2 Contextual).
    property int playbackContinuityPolicy: 2
    property int workspaceMode: 0
    property bool imageHasPrimary: false
    property bool imageHasSecondary: false
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

    signal openVideosRequested
    signal addVideoRequested
    signal openImageRequested
    signal addImageRequested
    signal openImagePairRequested
    signal compareImageFoldersRequested
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

    property bool returnViewerFocusAfterClose: false
    readonly property bool anyMenuOpen: fileMenu.opened || compareMenu.opened || analyzeMenu.opened || viewMenu.opened

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
            text: qsTr("打开视频…")
            shortcutText: "Ctrl+O"
            onTriggered: control.openVideosRequested()
        }
        VcsMenuItem {
            text: qsTr("添加视频…")
            shortcutText: "Ctrl+Shift+O"
            enabled: control.sourceCount > 0 && control.sourceCount < 3
            onTriggered: control.addVideoRequested()
        }
        VcsMenuSeparator {
            objectName: "fileImageSeparator"
        }
        VcsMenuItem {
            objectName: "openImageMenuItem"
            text: qsTr("打开图片…")
            shortcutText: "Ctrl+I"
            onTriggered: control.openImageRequested()
        }
        VcsMenuItem {
            objectName: "openImagePairMenuItem"
            text: qsTr("打开图片对…")
            shortcutText: "Ctrl+Shift+I"
            onTriggered: control.openImagePairRequested()
        }
        VcsMenuItem {
            objectName: "addImageMenuItem"
            text: qsTr("添加图片…")
            shortcutText: "Ctrl+Alt+I"
            enabled: control.workspaceMode === 1 && control.imageHasPrimary && !control.imageHasSecondary
            onTriggered: control.addImageRequested()
        }
        VcsMenuItem {
            objectName: "compareImageFoldersMenuItem"
            text: qsTr("对比文件夹…")
            shortcutText: "Ctrl+Shift+F"
            onTriggered: control.compareImageFoldersRequested()
        }
        VcsMenuSeparator {
            objectName: "fileIssueSeparator"
        }
        VcsMenuItem {
            objectName: "captureIssueMenuItem"
            text: qsTr("记录当前问题")
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
            shortcutText: "Ctrl+W"
            enabled: control.workspaceMode === 1 ? control.imageHasSession : control.videoHasSession
            onTriggered: {
                control.closeCurrentRequested();
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsMenuSeparator {}
        VcsMenuItem {
            text: qsTr("退出")
            shortcutText: "Alt+F4"
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
        enabled: control.sourceCount > 1 && !control.busy
        onClosed: {
            if (control.returnViewerFocusAfterClose) {
                control.returnViewerFocusAfterClose = false;
                control.viewerFocusRequested();
            }
        }

        VcsRadioMenuItem {
            objectName: "sideBySideMenuItem"
            text: qsTr("并排")
            checked: control.currentViewMode === 0
            onTriggered: {
                control.preferences.viewMode = 0;
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsRadioMenuItem {
            objectName: "wipeMenuItem"
            text: qsTr("分割线")
            checked: control.currentViewMode === 5
            onTriggered: {
                control.preferences.viewMode = 5;
                control.returnViewerFocusAfterClose = true;
            }
        }
        VcsRadioMenuItem {
            objectName: "differenceMenuItem"
            text: qsTr("差异")
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
                text: qsTr("逐帧审阅")
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
                text: qsTr("实时跟播")
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
            text: control.chromeVisible ? qsTr("隐藏界面 · Tab") : qsTr("显示界面 · Tab")
            onTriggered: {
                control.chromeToggleRequested();
                control.returnViewerFocusAfterClose = true;
            }
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
    }
}
