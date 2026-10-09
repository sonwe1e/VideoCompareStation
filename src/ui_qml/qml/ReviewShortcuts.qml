pragma ComponentBehavior: Bound

import QtQuick
// qmllint disable import
import Dvs.Ui 1.0

// qmllint enable import

Item {
    id: control

    required property var controller
    required property bool shortcutsEnabled
    required property bool presentationShortcutsEnabled
    required property int oneSecondStepFrames
    required property bool wipeEnabled
    required property real wipePosition
    required property int shortcutPreset
    required property bool fullScreen
    required property bool chromeVisible
    property var preferences: null
    required property int currentFrame
    required property int inFrame
    required property int outFrame
    // Step-2 "固定 GT 切候选": available with three sources in a pair mode; the actual
    // availability (workspace + input focus) still flows through shortcutsEnabled.
    required property bool candidateSwitchEnabled
    // View shortcuts (modes, zoom, peek) only make sense with a drawable comparison stage.
    required property bool viewShortcutsEnabled
    required property int sourceCount
    required property bool busy
    required property bool differencePeekAvailable
    property alias actions: reviewActions

    signal wipePositionRequested(real position)
    signal manualNavigationRequested
    signal chromeToggleRequested
    signal fullScreenToggleRequested
    signal presentationEscapeRequested
    signal shortcutHelpRequested
    signal inPointRequested
    signal outPointRequested
    signal selectedRangePlaybackRequested
    signal candidateSwitchRequested
    signal modeRequested(int mode)
    signal fitRequested
    signal nativeSizeRequested
    signal zoomInRequested
    signal zoomOutRequested
    signal resetViewRequested
    signal referencePeekToggleRequested
    signal referencePeekLockToggleRequested

    objectName: "reviewShortcuts"
    visible: false

    // Single source of truth for the shortcut help: every Shortcut below declares its own
    // helpKeys/helpLabel next to its sequence, and the overlay renders this table instead of a
    // hand-maintained copy. Entries are [keycaps, label, playerLabel, presetMask]: playerLabel
    // retells the row for the player preset when it differs, presetMask -1 lists the row in
    // both video presets and 1 in the player preset only. Sequences kept out of helpKeys
    // (Shift+A/D, End, …) are documented aliases of the row that does carry them. Built once
    // after instantiation: `data` is not a bindable property and never changes at runtime.
    property var helpEntries: []

    Component.onCompleted: {
        const entries = [];
        const declared = control.data;
        for (let i = 0; i < declared.length; ++i) {
            const entry = declared[i];
            if (entry === null || entry.helpKeys === undefined)
                continue;
            const label = String(entry.helpLabel);
            const playerLabel = entry.helpPlayerLabel === undefined ? label : String(entry.helpPlayerLabel);
            entries.push([entry.helpKeys, label, playerLabel, entry.helpPresets === undefined ? -1 : entry.helpPresets]);
        }
        control.helpEntries = entries;
    }

    ReviewActions {
        id: reviewActions

        controller: control.controller
        preferences: control.preferences
        shortcutsEnabled: control.shortcutsEnabled
        oneSecondStepFrames: control.oneSecondStepFrames
        wipeEnabled: control.wipeEnabled
        wipePosition: control.wipePosition
        onWipePositionRequested: position => control.wipePositionRequested(position)
        onManualNavigationRequested: control.manualNavigationRequested()
    }

    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canTogglePlayback
        onActivated: reviewActions.togglePlayback()
        property string helpKeys: qsTr("空格")
        property string helpLabel: qsTr("播放 / 暂停")
    }
    Shortcut {
        sequence: "Home"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canFirst
        onActivated: reviewActions.firstFrame()
        property string helpKeys: qsTr("Home / End")
        property string helpLabel: qsTr("第一帧 / 最后一帧")
    }
    Shortcut {
        sequence: "End"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canLast
        onActivated: reviewActions.lastFrame()
    }
    Shortcut {
        sequence: "A"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: reviewActions.previousFrame()
        property string helpKeys: qsTr("A / D")
        property string helpLabel: qsTr("上一帧 / 下一帧")
    }
    Shortcut {
        sequence: "D"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: reviewActions.nextFrame()
    }
    // Left/Right always step a single frame, in every shortcut preset. The OS auto-repeat drives
    // rapid stepping (plan 1.5.md §11.1: "continue letting nextFrame() be driven by OS auto-repeat").
    // Preset 1 (Player) previously mapped these to stepSeconds(5) = 150 frames at 30 fps — a jump that
    // violated the "±1 frame" expectation. Multi-frame jumps in preset 1 are still available via
    // Ctrl+Left/Ctrl+Right (stepSeconds(30)) and the dedicated five-frame/second buttons.
    Shortcut {
        sequence: "Left"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: reviewActions.previousFrame()
        property string helpKeys: qsTr("← / →")
        property string helpLabel: qsTr("上一帧 / 下一帧")
    }
    Shortcut {
        sequence: "Right"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: reviewActions.nextFrame()
    }
    Shortcut {
        sequences: ["Down", "Shift+Left", "Shift+A"]
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: reviewActions.stepBackwardFive()
        property string helpKeys: qsTr("↓ / ↑")
        property string helpLabel: qsTr("后退 / 前进 5 帧")
    }
    Shortcut {
        sequences: ["Up", "Shift+Right", "Shift+D"]
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: reviewActions.stepForwardFive()
        property string helpKeys: qsTr("Shift+← / →")
        property string helpLabel: qsTr("后退 / 前进 5 帧")
    }
    Shortcut {
        sequence: "Ctrl+A"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: reviewActions.stepBackwardSecond()
        property string helpKeys: qsTr("Ctrl+A / D")
        property string helpLabel: qsTr("后退 / 前进 1 秒")
    }
    Shortcut {
        sequence: "Ctrl+D"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: reviewActions.stepForwardSecond()
    }
    Shortcut {
        sequence: "Ctrl+Left"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: control.shortcutPreset === 1 ? reviewActions.stepSeconds(-30) : reviewActions.stepBackwardSecond()
        property string helpKeys: qsTr("Ctrl+← / →")
        property string helpLabel: qsTr("后退 / 前进 1 秒")
        property string helpPlayerLabel: qsTr("快退 / 快进 30 秒")
    }
    Shortcut {
        sequence: "Ctrl+Right"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: control.shortcutPreset === 1 ? reviewActions.stepSeconds(30) : reviewActions.stepForwardSecond()
    }
    Shortcut {
        sequence: ","
        context: Qt.ApplicationShortcut
        enabled: control.shortcutPreset === 1 && reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: reviewActions.previousFrame()
        property string helpKeys: qsTr(", / .")
        property string helpLabel: qsTr("上一帧 / 下一帧")
        property int helpPresets: 1
    }
    Shortcut {
        sequence: "."
        context: Qt.ApplicationShortcut
        enabled: control.shortcutPreset === 1 && reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: reviewActions.nextFrame()
    }
    Shortcut {
        sequence: "Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled
        onActivated: reviewActions.moveWipe(-0.01)
        property string helpKeys: qsTr("Alt+← / →")
        property string helpLabel: qsTr("Wipe 分屏线左移 / 右移")
    }
    Shortcut {
        sequence: "Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled
        onActivated: reviewActions.moveWipe(0.01)
    }
    Shortcut {
        sequence: "Shift+Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled
        onActivated: reviewActions.moveWipe(-0.05)
        property string helpKeys: qsTr("Shift+Alt+← / →")
        property string helpLabel: qsTr("Wipe 分屏线大步左移 / 右移")
    }
    Shortcut {
        sequence: "Shift+Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled
        onActivated: reviewActions.moveWipe(0.05)
    }
    Shortcut {
        sequence: "Tab"
        context: Qt.ApplicationShortcut
        enabled: control.presentationShortcutsEnabled
        onActivated: control.chromeToggleRequested()
        property string helpKeys: qsTr("Tab")
        property string helpLabel: qsTr("隐藏界面")
    }
    Shortcut {
        sequence: "F11"
        context: Qt.ApplicationShortcut
        enabled: control.presentationShortcutsEnabled
        onActivated: control.fullScreenToggleRequested()
        property string helpKeys: qsTr("F11")
        property string helpLabel: qsTr("全屏")
    }
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: control.fullScreen || !control.chromeVisible
        onActivated: control.presentationEscapeRequested()
    }
    Shortcut {
        sequence: "?"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled
        onActivated: control.shortcutHelpRequested()
        property string helpKeys: qsTr("?")
        property string helpLabel: qsTr("本帮助")
    }
    Shortcut {
        sequence: "I"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.currentFrame >= 0
        onActivated: control.inPointRequested()
        property string helpKeys: qsTr("I / O")
        property string helpLabel: qsTr("设置入点 / 出点")
    }
    Shortcut {
        sequence: "O"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.currentFrame >= 0
        onActivated: control.outPointRequested()
    }
    // Step-2 "固定 GT 切候选": flip the candidate side of the active pair against the
    // fixed reference without moving the observation position (C for Candidate).
    Shortcut {
        sequence: "C"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.candidateSwitchEnabled
        onActivated: control.candidateSwitchRequested()
        property string helpKeys: qsTr("C")
        property string helpLabel: qsTr("三源时固定参考切换候选")
    }
    Shortcut {
        sequence: "\\"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.inFrame >= 0 && control.outFrame >= control.inFrame
        onActivated: control.selectedRangePlaybackRequested()
        property string helpKeys: qsTr("\\")
        property string helpLabel: qsTr("播放选中区间")
    }

    // Keyboard view commands. Digit keys mirror CompareModeBar: 1-3 are pair modes,
    // 4-6 need a third source (the same availability the mode bar's "…" menu applies).
    component ModeKeyShortcut: Shortcut {
        id: modeShortcut

        required property int modeValue
        required property int requiredSourceCount
        required property string keySequence
        required property string modeLabel
        property string helpKeys: keySequence
        property string helpLabel: qsTr("切换视图：%1").arg(modeLabel)

        sequence: keySequence
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled && !control.busy && control.sourceCount >= modeShortcut.requiredSourceCount
        onActivated: control.modeRequested(modeValue)
    }

    // qmllint disable import unqualified unresolved-type
    ModeKeyShortcut {
        keySequence: "1"
        modeValue: ComparisonSurface.SideBySide
        requiredSourceCount: 2
        modeLabel: qsTr("并排")
    }
    ModeKeyShortcut {
        keySequence: "2"
        modeValue: ComparisonSurface.Wipe
        requiredSourceCount: 2
        modeLabel: qsTr("分割线")
    }
    ModeKeyShortcut {
        keySequence: "3"
        modeValue: ComparisonSurface.Difference
        requiredSourceCount: 2
        modeLabel: qsTr("差异")
    }
    ModeKeyShortcut {
        keySequence: "4"
        modeValue: ComparisonSurface.ThreeUp
        requiredSourceCount: 3
        modeLabel: qsTr("三联")
    }
    ModeKeyShortcut {
        keySequence: "5"
        modeValue: ComparisonSurface.ReferenceFocus
        requiredSourceCount: 3
        modeLabel: qsTr("参考聚焦")
    }
    ModeKeyShortcut {
        keySequence: "6"
        modeValue: ComparisonSurface.AnalysisGrid
        requiredSourceCount: 3
        modeLabel: qsTr("分析网格")
    }
    // qmllint enable import unqualified unresolved-type

    Shortcut {
        sequence: "F"
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled
        onActivated: control.fitRequested()
        property string helpKeys: qsTr("F")
        property string helpLabel: qsTr("适应窗口")
    }
    Shortcut {
        sequence: "Ctrl+0"
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled
        onActivated: control.nativeSizeRequested()
        property string helpKeys: qsTr("Ctrl+0")
        property string helpLabel: qsTr("100% 真实尺寸")
    }
    Shortcut {
        sequences: ["Plus", "Num+Plus"]
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled
        onActivated: control.zoomInRequested()
        property string helpKeys: qsTr("+ / −")
        property string helpLabel: qsTr("放大 / 缩小视图")
    }
    Shortcut {
        sequences: ["Minus", "Num+Minus"]
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled
        onActivated: control.zoomOutRequested()
    }
    Shortcut {
        sequence: "R"
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled
        onActivated: control.resetViewRequested()
        property string helpKeys: qsTr("R")
        property string helpLabel: qsTr("重置视图（清除 ROI 并适应窗口）")
    }
    // Keyboard flavour of the mode bar's hold-to-peek button: tap toggles the raw reference
    // view, Shift+` locks it so it survives view switches. Difference mode only, same
    // availability as the button. The literal backtick is used (not the QuoteLeft key name)
    // because QKeySequence parses and matches punctuation characters reliably.
    Shortcut {
        sequence: "`"
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled && control.differencePeekAvailable && !control.busy
        onActivated: control.referencePeekToggleRequested()
        property string helpKeys: qsTr("`")
        property string helpLabel: qsTr("切换查看参考原图（差异模式）")
    }
    Shortcut {
        sequence: "Shift+`"
        context: Qt.ApplicationShortcut
        enabled: control.viewShortcutsEnabled && control.differencePeekAvailable && !control.busy
        onActivated: control.referencePeekLockToggleRequested()
        property string helpKeys: qsTr("Shift+`")
        property string helpLabel: qsTr("锁定 / 解锁参考原图直看")
    }
}
