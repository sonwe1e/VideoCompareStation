import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    width: 960
    height: 640

    QtObject {
        id: imageReview

        property bool hasPrimary: true
        property bool hasSecondary: true
        property bool hasPair: true
        property int compareMode: 0
        property int viewMode: 0
        property int contentGeneration: 1
        property int primaryWidth: 64
        property int primaryHeight: 64
        property int secondaryWidth: 64
        property int secondaryHeight: 64
        property real zoom: 1
        property real panX: 0.5
        property real panY: 0.5
        property real wipePosition: 0.5
        property var cursorPixel: ({})
        property string errorText: ""

        function imageUrl(slot) {
            return "";
        }
        function clearCursorPixel() {
        }
        function updateCursorPixel(slot, x, y) {
        }
        function panBy(x, y) {
        }
    }

    QtObject {
        id: pairModel
        property int pairCount: 0
    }

    Dvs.ImageWorkspace {
        id: workspace
        anchors.fill: parent
        controller: imageReview
        pairModel: pairModel
        sidebarVisible: false
    }

    TestCase {
        name: "ImageWorkspaceManual"
        when: windowShown

        function test_canvas_click_switches_pair_without_timer() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            compare(workspace.singleViewShowSecondary, false);
            mouseClick(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.singleViewShowSecondary, true);
            mouseClick(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.singleViewShowSecondary, false);
        }
    }
}
