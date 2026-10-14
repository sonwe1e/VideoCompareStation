pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: actions

    required property var controller
    property bool shortcutsEnabled: true
    property int oneSecondStepFrames: 30
    property bool wipeEnabled: false
    property real wipePosition: 0.5

    readonly property bool canFirst: Boolean(controller && controller.canFirst)
    readonly property bool canPrevious: Boolean(controller && controller.canPrevious)
    readonly property bool canNext: Boolean(controller && controller.canNext)
    readonly property bool canLast: Boolean(controller && controller.canLast)
    readonly property bool canTogglePlayback: Boolean(controller && (controller.playing ? controller.canPause : controller.canPlay))
    // E04 jog projection. `jogActive` covers the 250 ms press window too, so an Esc pressed
    // between the tap and the hold threshold still ends the interaction.
    readonly property bool jogging: Boolean(controller && controller.jogging)
    readonly property bool jogActive: Boolean(controller && controller.jogActive)
    readonly property int jogDirection: controller && controller.jogDirection !== undefined ? Number(controller.jogDirection) : 0
    readonly property real jogRate: controller && controller.jogRate !== undefined ? Number(controller.jogRate) : 1

    signal wipePositionRequested(real position)
    signal manualNavigationRequested

    function firstFrame() {
        if (canFirst) {
            manualNavigationRequested();
            controller.first();
        }
    }

    function previousFrame() {
        if (canPrevious) {
            manualNavigationRequested();
            controller.previous();
        }
    }

    function nextFrame() {
        if (canNext) {
            manualNavigationRequested();
            controller.next();
        }
    }

    function lastFrame() {
        if (canLast) {
            manualNavigationRequested();
            controller.last();
        }
    }

    function stepBackwardFive() {
        if (canPrevious) {
            manualNavigationRequested();
            controller.stepFrames(-5);
        }
    }

    function stepForwardFive() {
        if (canNext) {
            manualNavigationRequested();
            controller.stepFrames(5);
        }
    }

    function stepBackwardSecond() {
        if (canPrevious) {
            manualNavigationRequested();
            controller.stepFrames(-Math.max(1, oneSecondStepFrames));
        }
    }

    function stepForwardSecond() {
        if (canNext) {
            manualNavigationRequested();
            controller.stepFrames(Math.max(1, oneSecondStepFrames));
        }
    }

    function stepSeconds(seconds) {
        if ((seconds < 0 && canPrevious) || (seconds > 0 && canNext)) {
            manualNavigationRequested();
            controller.stepFrames(seconds * Math.max(1, oneSecondStepFrames));
        }
    }

    function togglePlayback() {
        if (canTogglePlayback)
            controller.togglePlayback();
    }

    function setPlaybackRate(rate) {
        if (controller && typeof controller.setPlaybackRate === "function")
            controller.setPlaybackRate(rate);
    }

    // E04 long-press walk rate: 0.5x/1x/2x real-time multiples, clamped by the controller's jog
    // state machine. It paces the same held walk as the step keys and buttons, so it never
    // touches playback state.
    function setJogRate(rate) {
        if (controller && typeof controller.setJogRate === "function")
            controller.setJogRate(rate);
    }

    function moveWipe(delta) {
        if (!wipeEnabled)
            return;
        wipePositionRequested(Math.max(0, Math.min(1, wipePosition + delta)));
    }

    property var preferences: null

    // The transport toggle explains its own consequence the moment it is used: the policy names
    // alone do not say what "smooth" or "complete" costs, so Main.qml turns this into a HUD note.
    signal playbackContinuityPolicyChanged(int policy)

    function setPlaybackContinuityPolicy(policy) {
        if (preferences)
            preferences.playbackContinuityPolicy = policy;
        if (controller && typeof controller.setPlaybackContinuityPolicy === "function")
            controller.setPlaybackContinuityPolicy(policy);
        playbackContinuityPolicyChanged(policy);
    }

    function stepFrames(delta) {
        if (controller && typeof controller.stepFrames === "function") {
            manualNavigationRequested();
            controller.stepFrames(delta);
        }
    }

    // E04 long-press jog. The controller owns the state machine: press steps once immediately
    // and arms the 250 ms hold window; release stops feeding. A same-direction press while
    // already held (OS auto-repeat) is ignored by the controller; the opposite direction ends
    // the current jog first. Boundaries are gated here as well so a press at frame 0/last
    // neither steps nor triggers the manual-navigation side effects.
    function jogPress(direction) {
        if (!controller || typeof controller.jogPress !== "function")
            return false;
        const allowed = direction < 0 ? canPrevious : canNext;
        if (!allowed)
            return false;
        manualNavigationRequested();
        return controller.jogPress(direction);
    }

    function jogRelease() {
        if (controller && typeof controller.jogRelease === "function")
            controller.jogRelease();
    }

    // Non-release end: Esc, focus loss, dialog open, window close.
    function jogCancel() {
        if (controller && typeof controller.jogCancel === "function")
            controller.jogCancel();
    }
}
