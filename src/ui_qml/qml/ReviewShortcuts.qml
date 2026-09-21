pragma ComponentBehavior: Bound

import QtQuick

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
    required property int currentFrame
    required property int inFrame
    required property int outFrame
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

    visible: false

    ReviewActions {
        id: reviewActions

        controller: control.controller
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
    }
    Shortcut {
        sequence: "Home"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canFirst
        onActivated: reviewActions.firstFrame()
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
    }
    Shortcut {
        sequences: ["Up", "Shift+Right", "Shift+D"]
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canNext
        onActivated: reviewActions.stepForwardFive()
    }
    Shortcut {
        sequence: "Ctrl+A"
        context: Qt.ApplicationShortcut
        enabled: reviewActions.shortcutsEnabled && reviewActions.canPrevious
        onActivated: reviewActions.stepBackwardSecond()
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
    }
    Shortcut {
        sequence: "F11"
        context: Qt.ApplicationShortcut
        enabled: control.presentationShortcutsEnabled
        onActivated: control.fullScreenToggleRequested()
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
    }
    Shortcut {
        sequence: "I"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.currentFrame >= 0
        onActivated: control.inPointRequested()
    }
    Shortcut {
        sequence: "O"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.currentFrame >= 0
        onActivated: control.outPointRequested()
    }
    Shortcut {
        sequence: "\\"
        context: Qt.ApplicationShortcut
        enabled: control.shortcutsEnabled && control.inFrame >= 0 && control.outFrame >= control.inFrame
        onActivated: control.selectedRangePlaybackRequested()
    }
}
