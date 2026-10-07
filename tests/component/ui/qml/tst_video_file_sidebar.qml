import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    width: 280
    height: 640

    ListModel {
        id: folder
        property int currentRow: 1
        property int pendingRow: -1
        property bool openPending: false
        property bool scanning: false
        property string folderName: "videos"
        property string folderPath: "/videos"
        property url folderUrl: "file:///videos"
        property int fileCount: count
        property int recentCurrentRow: 1
        property var recentFiles: [
            {fileName: "same-name.mp4", fileUrl: "file:///older/same-name.mp4"},
            {fileName: "very-long-current-video-name-to-ellide.mp4", fileUrl: "file:///videos/current.mp4"}
        ]
        ListElement { fileName: "clip1.mp4"; fileUrl: "file:///videos/clip1.mp4" }
        ListElement { fileName: "clip2.mp4"; fileUrl: "file:///videos/clip2.mp4" }
    }

    Dvs.VideoFolderSidebar {
        id: sidebar
        width: 260
        height: parent.height
        folderModel: folder
    }

    SignalSpy { id: recentSpy; target: sidebar; signalName: "recentFileRequested" }
    SignalSpy { id: folderSpy; target: sidebar; signalName: "fileRequested" }
    SignalSpy { id: closeSpy; target: sidebar; signalName: "closeRequested" }

    TestCase {
        name: "VideoFileSidebar"
        when: windowShown

        function init() {
            sidebar.height = 640;
            sidebar.showRecent = false;
            sidebar.errorText = "";
            folder.scanning = false;
            recentSpy.clear();
            folderSpy.clear();
            closeSpy.clear();
            wait(30);
        }

        function test_tabsRowsAndCurrentHighlight() {
            const fileList = findChild(sidebar, "videoFolderFileList");
            const recentList = findChild(sidebar, "videoRecentFileList");
            verify(fileList.visible);
            verify(!recentList.visible);
            mouseClick(findChild(sidebar, "videoRecentTab"));
            verify(sidebar.showRecent);
            verify(!fileList.visible);
            verify(recentList.visible);
            tryCompare(recentList, "count", 2);
            let row = null;
            tryVerify(function() { row = recentList.itemAtIndex(1); return row !== null; });
            verify(row.highlighted);
            compare(row.text, "very-long-current-video-name-to-ellide.mp4");
            mouseClick(row);
            compare(recentSpy.count, 1);
            compare(recentSpy.signalArguments[0][0], 1);
            compare(folderSpy.count, 0);
            mouseClick(findChild(sidebar, "videoFolderTab"));
            verify(!sidebar.showRecent);
            tryVerify(function() { row = fileList.itemAtIndex(1); return row !== null; });
            verify(row.highlighted);
            mouseClick(row);
            compare(folderSpy.count, 1);
            compare(folderSpy.signalArguments[0][0], 1);
        }

        function test_historyRemainsUsableDuringFolderScanAndError() {
            sidebar.showRecent = true;
            folder.scanning = true;
            sidebar.errorText = "无法打开该视频，原视频保持不变。";
            const list = findChild(sidebar, "videoRecentFileList");
            let row = null;
            tryVerify(function() { row = list.itemAtIndex(0); return row !== null; });
            verify(row.enabled);
            verify(!row.highlighted);
            verify(list.height > 100);
            mouseClick(row);
            compare(recentSpy.count, 1);
            compare(recentSpy.signalArguments[0][0], 0);
            list.forceActiveFocus();
            list.currentIndex = 1;
            keyClick(Qt.Key_Return);
            compare(recentSpy.count, 2);
            compare(recentSpy.signalArguments[1][0], 1);
        }

        function test_compactFolderKeepsClickableRowsWithError() {
            sidebar.height = 320;
            sidebar.errorText = "视频已被移动或无法读取，请检查文件路径后重新打开，原视频保持不变。";
            wait(30);
            const list = findChild(sidebar, "videoFolderFileList");
            verify(list.height >= 42);
            const row = list.itemAtIndex(1);
            verify(row !== null);
            verify(row.highlighted);
            mouseClick(row);
            compare(folderSpy.count, 1);
        }

        function test_repeatedTabsKeepSelectionAndNoImplicitOpen() {
            for (let index = 0; index < 3; ++index) {
                mouseClick(findChild(sidebar, "videoRecentTab"));
                mouseClick(findChild(sidebar, "videoRecentTab"));
                verify(sidebar.showRecent);
                verify(findChild(sidebar, "videoRecentTab").checked);
                mouseClick(findChild(sidebar, "videoFolderTab"));
                verify(!sidebar.showRecent);
            }
            compare(recentSpy.count, 0);
            compare(folderSpy.count, 0);
            compare(folder.currentRow, 1);
        }
    }
}
