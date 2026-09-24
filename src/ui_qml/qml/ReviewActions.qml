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

    function moveWipe(delta) {
        if (!wipeEnabled)
            return;
        wipePositionRequested(Math.max(0, Math.min(1, wipePosition + delta)));
    }

    property var preferences: null

    function setPlaybackContinuityPolicy(policy) {
        if (preferences)
            preferences.playbackContinuityPolicy = policy;
        if (controller && typeof controller.setPlaybackContinuityPolicy === "function")
            controller.setPlaybackContinuityPolicy(policy);
    }

    function stepFrames(delta) {
        if (controller && typeof controller.stepFrames === "function") {
            manualNavigationRequested();
            controller.stepFrames(delta);
        }
    }
}
