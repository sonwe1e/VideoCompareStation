import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 960
    height: 640

    Dvs.EmptyReviewView {
        id: emptyView
        anchors.fill: parent
        accentColor: "#4b8df8"
        textColor: "#f3f6fb"
        mutedTextColor: "#93a2ba"
    }

    Dvs.PlayerOsc {
        id: hiddenOsc
        width: root.width
        controllerState: 2
        playing: false
        timelineEnabled: false
        currentFrame: -1
        totalFrames: 0
        progress: 0
        timecodeText: "00:00:00:00"
        markers: []
        actions: QtObject {}
        focusTarget: emptyView
        canFirst: false
        canPrevious: false
        canPlay: false
        canPause: false
        canNext: false
        canLast: false
        anchors.bottom: root.bottom
    }

    TestCase {
        name: "EmptyReviewView"
        when: windowShown

        function test_empty_state_has_no_transport() {
            verify(emptyView.visible);
            verify(!hiddenOsc.visible);
        }

        function test_entry_points_remain_inside_card_at_supported_sizes() {
            const names = ["emptyOpenVideosButton", "emptyOpenImageButton", "emptyOpenVideoFolderButton", "emptyOpenImagePairButton", "emptyCompareFoldersButton"];
            const sizes = [[960, 640], [1280, 720], [1920, 1080]];
            const card = findChild(emptyView, "emptyReviewCard");
            verify(card);
            for (let s = 0; s < sizes.length; ++s) {
                root.width = sizes[s][0];
                root.height = sizes[s][1];
                wait(30);
                for (let i = 0; i < names.length; ++i) {
                    const button = findChild(emptyView, names[i]);
                    verify(button && button.visible && button.enabled, names[i]);
                    const point = button.mapToItem(card, 0, 0);
                    verify(point.x >= 0 && point.y >= 0, names[i]);
                    verify(point.x + button.width <= card.width, names[i]);
                    verify(point.y + button.height <= card.height, names[i]);
                    verify(button.height >= 32, names[i]);
                    verify(button.contentItem.implicitWidth <= button.availableWidth, names[i] + " label fits");
                }
            }
            root.width = 960;
            root.height = 640;
        }
    }
}
