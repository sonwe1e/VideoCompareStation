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
    required property int currentViewMode
    required property bool inspectorOpen
    required property bool graphicsReady
    required property int currentFrame
    required property bool alignmentAnalysisRunning
    required property bool chromeVisible
    required property bool fullScreen
    required property int shortcutPreset
    required property var sourceIdentities
    property int workspaceMode: 0

    signal openVideosRequested
    signal addVideoRequested
    signal openImageRequested
    signal openImagePairRequested
    signal workspaceRequested(int mode)
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
            text: qsTr("关闭视频")
            shortcutText: "Ctrl+W"
            enabled: control.sourceCount > 0
            onTriggered: {
                control.destructiveActionRequested("closeReview");
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
                checked: Number(control.preferences.differenceEdge) === 0
                onTriggered: {
                    control.preferences.differenceEdge = 0;
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("A / C")
                checked: Number(control.preferences.differenceEdge) === 1
                onTriggered: {
                    control.preferences.differenceEdge = 1;
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("B / C")
                checked: Number(control.preferences.differenceEdge) === 2
                onTriggered: {
                    control.preferences.differenceEdge = 2;
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
                checked: control.canonicalSourceIndex === 0
                onTriggered: {
                    control.changeReferenceByIndex(0);
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("视频 B")
                checked: control.canonicalSourceIndex === 1
                onTriggered: {
                    control.changeReferenceByIndex(1);
                    control.returnViewerFocusAfterClose = true;
                }
            }
            VcsRadioMenuItem {
                text: qsTr("视频 C")
                visible: control.sourceCount > 2
                checked: control.canonicalSourceIndex === 2
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
