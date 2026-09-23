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
        property int panCalls: 0

        function imageUrl(slot) {
            return "";
        }
        function clearCursorPixel() {
        }
        function updateCursorPixel(slot, x, y) {
        }
        function panBy(x, y) {
            panCalls++;
        }
        function setZoom(value) {
            zoom = value;
        }
        function resetView() {
            zoom = 1;
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

        function init() {
            imageReview.compareMode = 0;
            workspace.singleViewShowSecondary = false;
            workspace.trueSize = false;
        }

        function test_canvas_click_switches_pair_without_timer() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            compare(workspace.singleViewShowSecondary, false);
            mouseClick(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.singleViewShowSecondary, true);
            mouseClick(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.singleViewShowSecondary, false);
        }

        function test_small_pointer_motion_switches_without_panning() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            imageReview.panCalls = 0;
            workspace.singleViewShowSecondary = false;
            const x = canvas.width / 2;
            const y = canvas.height / 2;
            mousePress(canvas, x, y, Qt.LeftButton);
            mouseMove(canvas, x + 2, y);
            mouseRelease(canvas, x + 2, y, Qt.LeftButton);
            compare(imageReview.panCalls, 0);
            compare(workspace.singleViewShowSecondary, true);
        }

        function test_drag_pans_without_switching() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            imageReview.panCalls = 0;
            workspace.singleViewShowSecondary = false;
            const x = canvas.width / 2;
            const y = canvas.height / 2;
            mousePress(canvas, x, y, Qt.LeftButton);
            mouseMove(canvas, x + 10, y);
            mouseRelease(canvas, x + 10, y, Qt.LeftButton);
            verify(imageReview.panCalls > 0);
            compare(workspace.singleViewShowSecondary, false);
        }

        function test_double_click_zoom_keeps_selected_source() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            workspace.singleViewShowSecondary = false;
            workspace.trueSize = false;
            mouseDoubleClickSequence(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.trueSize, true);
            compare(workspace.singleViewShowSecondary, false);
        }

        function test_long_pixel_readout_stays_inside_available_status_width() {
            imageReview.cursorPixel = ({ "valid": true, "x": 8000, "y": 8000,
                "r": 255, "g": 255, "b": 255, "a": 255, "hex": "#FFFFFFFF",
                "alphaPercent": 100, "nativeBitDepth": 16, "r16": 65535,
                "g16": 65535, "b16": 65535 });
            const details = findChild(workspace, "imageStatusDetails");
            const hints = findChild(workspace, "imageStatusHints");
            const pixel = findChild(workspace, "imagePixelReadout");
            verify(details !== null);
            verify(hints !== null);
            verify(pixel !== null);
            tryCompare(pixel, "visible", true);
            verify(details.x + details.width <= hints.x);
            verify(pixel.width <= details.width);
            imageReview.cursorPixel = ({});
        }
    }
}
