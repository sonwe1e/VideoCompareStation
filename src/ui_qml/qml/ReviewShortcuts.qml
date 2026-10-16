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
    // D2: one-key toggle between fit-window and true 1:1 (same verb as the canvas
    // double-click). The host routes it to the viewport's anchor-preserving toggle.
    signal nativeFitToggleRequested
    signal zoomInRequested
    signal zoomOutRequested
    signal resetViewRequested
    // D4 hold peek: press shows the raw reference view, release restores the difference view.
    // The lock (Shift+`) is a separate action; the hold never changes the lock state.
    signal referencePeekHoldChanged(bool held)
    signal referencePeekLockToggleRequested
    // D3 pair look (T): while paused, alternate the whole-canvas direct look between the two
    // slots of the current comparison pair. The host computes the actual pair (GT/candidate
    // combinations included) and keeps the key inert while playing.
    signal pairLookToggleRequested

    objectName: "reviewShortcuts"
    visible: false

    // E04 held-key jog. The handler owns the bare frame-step sequences while enabled: a press
    // becomes one immediate step plus a 250 ms hold window, OS auto-repeat is consumed so it
    // can never re-trigger a jog, and the release (or window deactivation) stops the walk. The
    // per-repeat Shortcuts this replaces could not see key releases, which is why the walk was
    // previously keyed to the OS repeat clock instead of the media clock.
    // qmllint disable import unqualified unresolved-type
    JogKeyHandler {
        id: jogKeys

        window: control.Window.window
        enabled: reviewActions.shortcutsEnabled
        backwardEnabled: reviewActions.canPrevious
        forwardEnabled: reviewActions.canNext
        playerPresetKeys: control.shortcutPreset === 1
        onJogPressed: direction => reviewActions.jogPress(direction)
        // Only the active direction's release ends the walk: pressing the opposite direction
        // starts a new jog in the single consumer slot, and the old key's staggered release must
        // not stop the walk the user is still holding.
        onJogReleased: direction => {
            if (reviewActions.jogDirection === 0 || reviewActions.jogDirection === direction)
                reviewActions.jogRelease();
        }
    }
    // qmllint enable import unqualified unresolved-type

    // Non-release ends: Esc, focus loss (window deactivation is handled by the key handler),
    // dialog open (shortcutsEnabled drops and the handler releases), and window close via the
    // host's cancelJog() call.
    function cancelJog() {
        reviewActions.jogCancel();
    }

    onShortcutsEnabledChanged: {
        // A dialog or menu opened mid-hold disables the key handler, which releases the held
        // keys; jogCancel() here is the safety net for the press window between enable flips.
        if (!shortcutsEnabled)
            reviewActions.jogCancel();
    }

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
        // I02: toggle-type actions never auto-repeat - a held Space must not flip play/pause
        // on the OS repeat clock.
        autoRepeat: false
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
    // Bare A/D and Left/Right are owned by JogKeyHandler above (press = tap step, hold = walk,
    // release = stop), so they appear here only as help-table rows. The overlay collects these
    // entries exactly like Shortcut help rows; their per-repeat Shortcut predecessors could not
    // observe key releases. Multi-frame jumps stay on Shift/Ctrl variants and the ±5/±1s buttons.
    component JogKeyHelpRow: QtObject {
        id: jogKeyHelpRow

        required property string helpKeys
        required property string helpLabel
        required property int helpPresets
    }
    JogKeyHelpRow {
        helpKeys: qsTr("A / D")
        helpLabel: qsTr("上一帧 / 下一帧（长按可连续逐帧）")
        helpPresets: -1
    }
    JogKeyHelpRow {
        helpKeys: qsTr("← / →")
        helpLabel: qsTr("上一帧 / 下一帧（长按可连续逐帧）")
        helpPresets: -1
    }
    JogKeyHelpRow {
        helpKeys: qsTr(", / .")
        helpLabel: qsTr("上一帧 / 下一帧（长按可连续逐帧）")
        helpPresets: 1
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
    // ,/. are player-preset aliases of the bare frame-step keys; JogKeyHandler owns them while
    // the player preset is active (playerPresetKeys) and they are documented in the help rows.
    // E1: W toggles the wipe split axis (vertical left/right <-> horizontal top/bottom).
    // It must fire only while a real wipe composite is on screen: wipe view mode AND no
    // full-canvas direct look (canvasFocusSlot >= 0 hides the handle and renders the
    // single-source branch, so silently flipping the axis then would be invisible tampering).
    signal wipeOrientationToggleRequested
    signal wipeMoveYRequested(real delta)
    required property bool wipeSplitActive
    // E1: which axis the split is currently on. The Alt+Left/Right and Alt+Up/Down move keys
    // are gated to their real axes - moving the ratio with the wrong arrow pair would drag
    // the split along an axis the on-screen handle no longer shows.
    required property bool wipeHorizontal
    Shortcut {
        sequence: "W"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && control.presentationShortcutsEnabled
        onActivated: control.wipeOrientationToggleRequested()
        property string helpKeys: qsTr("W")
        property string helpLabel: qsTr("切换分屏线方向（水平 / 垂直）")
    }
    Shortcut {
        sequence: "Alt+Up"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && control.wipeHorizontal
        onActivated: control.wipeMoveYRequested(-0.01)
        property string helpKeys: qsTr("Alt+↑ / Alt+↓")
        property string helpLabel: qsTr("水平分屏线上移 / 下移（Shift 大步；仅上下切分）")
    }
    Shortcut {
        sequence: "Alt+Down"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && control.wipeHorizontal
        onActivated: control.wipeMoveYRequested(0.01)
    }
    Shortcut {
        sequence: "Shift+Alt+Up"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && control.wipeHorizontal
        onActivated: control.wipeMoveYRequested(-0.05)
    }
    Shortcut {
        sequence: "Shift+Alt+Down"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && control.wipeHorizontal
        onActivated: control.wipeMoveYRequested(0.05)
    }
    Shortcut {
        sequence: "Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && !control.wipeHorizontal
        onActivated: reviewActions.moveWipe(-0.01)
        property string helpKeys: qsTr("Alt+← / →")
        property string helpLabel: qsTr("垂直分屏线左移 / 右移（仅左右切分）")
    }
    Shortcut {
        sequence: "Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && !control.wipeHorizontal
        onActivated: reviewActions.moveWipe(0.01)
    }
    Shortcut {
        sequence: "Shift+Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && !control.wipeHorizontal
        onActivated: reviewActions.moveWipe(-0.05)
        property string helpKeys: qsTr("Shift+Alt+← / →")
        property string helpLabel: qsTr("垂直分屏线大步左移 / 右移（仅左右切分）")
    }
    Shortcut {
        sequence: "Shift+Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.wipeEnabled && control.wipeSplitActive && !control.wipeHorizontal
        onActivated: reviewActions.moveWipe(0.05)
    }
    // D1: pure mode moved from Tab to H. Tab/Shift+Tab return to their platform meaning -
    // keyboard focus navigation - which an ApplicationShortcut on Tab would swallow.
    Shortcut {
        sequence: "H"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.presentationShortcutsEnabled
        onActivated: control.chromeToggleRequested()
        property string helpKeys: qsTr("H")
        property string helpLabel: qsTr("隐藏界面")
    }
    Shortcut {
        sequence: "F11"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.presentationShortcutsEnabled
        onActivated: control.fullScreenToggleRequested()
        property string helpKeys: qsTr("F11")
        property string helpLabel: qsTr("全屏")
    }
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.fullScreen || !control.chromeVisible
        onActivated: control.presentationEscapeRequested()
    }
    // Esc ends an active jog (E04 end list). The presentation Esc above owns the key whenever it
    // is enabled, and Main.qml cancels the jog on that path too, so the two never compete.
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: reviewActions.jogActive && !control.fullScreen && control.chromeVisible
        onActivated: reviewActions.jogCancel()
    }
    Shortcut {
        sequence: "?"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.shortcutsEnabled
        onActivated: control.shortcutHelpRequested()
        property string helpKeys: qsTr("?")
        property string helpLabel: qsTr("本帮助")
    }
    Shortcut {
        sequence: "I"
        context: Qt.ApplicationShortcut
        // Marking shortcuts follow the I02 toggle rule: one press, one mark — holding the key
        // must not re-issue the mark (and re-light the HUD) on every auto-repeat.
        autoRepeat: false
        enabled: control.shortcutsEnabled && control.currentFrame >= 0
        onActivated: control.inPointRequested()
        property string helpKeys: qsTr("I / O")
        property string helpLabel: qsTr("设置入点 / 出点")
    }
    Shortcut {
        sequence: "O"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.shortcutsEnabled && control.currentFrame >= 0
        onActivated: control.outPointRequested()
    }
    // Step-2 "固定 GT 切候选": flip the candidate side of the active pair against the
    // fixed reference without moving the observation position (C for Candidate).
    Shortcut {
        sequence: "C"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.shortcutsEnabled && control.candidateSwitchEnabled
        onActivated: control.candidateSwitchRequested()
        property string helpKeys: qsTr("C")
        property string helpLabel: qsTr("三源时固定参考切换候选")
    }
    Shortcut {
        sequence: "\\"
        context: Qt.ApplicationShortcut
        autoRepeat: false
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
        autoRepeat: false
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
    // D2: Z mirrors the canvas double-click - one key flips between fit-window and the
    // content/DPR-aware 100%, preserving the observation anchor. Never auto-repeats: each
    // press is one deliberate flip.
    Shortcut {
        sequence: "Z"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.viewShortcutsEnabled
        onActivated: control.nativeFitToggleRequested()
        property string helpKeys: qsTr("Z")
        property string helpLabel: qsTr("100% ↔ 适应窗口")
    }
    // D3: T is the cross-workspace "alternate in-place look" key. Video side pauses only:
    // while playing the key stays inert so it never competes with playback semantics.
    Shortcut {
        sequence: "T"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.viewShortcutsEnabled && !control.busy
        onActivated: control.pairLookToggleRequested()
        property string helpKeys: qsTr("T")
        property string helpLabel: qsTr("暂停时在当前比较对间交替直看")
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
    // D4: the backtick is a real hold, not a toggle. The window-level watcher pairs with the
    // mode bar's hold button (same availability); auto-repeats never re-arm it, a modified
    // release still ends it, and window deactivation releases the hold. Shift+` below stays
    // the lock - a deliberate separate action. The literal backtick is used (not the
    // QuoteLeft key name) because QKeySequence matches punctuation characters reliably.
    // qmllint disable import unqualified unresolved-type
    HoldKeyHandler {
        window: control.Window.window
        enabled: control.viewShortcutsEnabled && control.differencePeekAvailable && !control.busy
        holdKey: Qt.Key_QuoteLeft
        onHoldPressed: control.referencePeekHoldChanged(true)
        onHoldReleased: control.referencePeekHoldChanged(false)
    }
    // qmllint enable import unqualified unresolved-type
    JogKeyHelpRow {
        helpKeys: qsTr("`（按住）")
        helpLabel: qsTr("按住直看参考（GT）原图，松开恢复差异视图")
        helpPresets: -1
    }
    Shortcut {
        sequence: "Shift+`"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: control.viewShortcutsEnabled && control.differencePeekAvailable && !control.busy
        onActivated: control.referencePeekLockToggleRequested()
        property string helpKeys: qsTr("Shift+`")
        property string helpLabel: qsTr("锁定 / 解锁参考原图直看")
    }
}
