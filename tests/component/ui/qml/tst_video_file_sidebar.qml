import QtQuick
import QtTest
import QtQuick.Controls
import "../../../../src/ui_qml/qml" as Dvs
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

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
        property string testOpenReason: ""
        property string testCompareReason: ""
        fileActionState: url => ({openReason: testOpenReason, compareReason: testCompareReason})
    }

    SignalSpy { id: recentSpy; target: sidebar; signalName: "recentFileRequested" }
    SignalSpy { id: folderSpy; target: sidebar; signalName: "fileRequested" }
    SignalSpy { id: closeSpy; target: sidebar; signalName: "closeRequested" }

    SignalSpy { id: contextOpenSpy; target: sidebar; signalName: "contextOpenRequested" }
    SignalSpy { id: compareSpy; target: sidebar; signalName: "compareRequested" }

    TestCase {
        name: "VideoFileSidebar"
        when: windowShown

        function init() {
            findChild(sidebar, "videoFileContextMenu").close();
            sidebar.testOpenReason = "";
            sidebar.testCompareReason = "";
            sidebar.actionRevision = "";
            contextOpenSpy.clear();
            compareSpy.clear();
            sidebar.height = 640;
            sidebar.showRecent = false;
            sidebar.errorText = "";
            folder.scanning = false;
            recentSpy.clear();
            folderSpy.clear();
            closeSpy.clear();
            wait(30);
        }

        function cleanup() {
            findChild(sidebar, "videoFileContextMenu").close();
            wait(30);
        }

        function settleContextMenu() {
            // opened=true fires before the menu's content column completes its first
            // layout pass: on Windows/Qt 6.11.1 the content height stays 0 for another
            // frame or two, and mouseClick on an item mapped through the un-laid-out
            // column misses the popup (probe evidence: content height 0 -> 70 within
            // 50 ms). Wait for the real layout before any menu-item mouse click.
            const menu = findChild(sidebar, "videoFileContextMenu");
            tryVerify(function() { return menu.contentItem.height > 8; });
        }

        function test_rightClickDoesNotOpenAndUsesCapturedUrl_data() {
            return [{tag: "folder", recent: false}, {tag: "recent", recent: true}];
        }

        function test_rightClickDoesNotOpenAndUsesCapturedUrl(data) {
            sidebar.showRecent = data.recent;
            const list = findChild(sidebar, data.recent ? "videoRecentFileList" : "videoFolderFileList");
            let row = null;
            tryVerify(function() { row = list.itemAtIndex(0); return row !== null; });
            const expected = row.contextUrl.toString();
            mouseClick(row, 20, 20, Qt.RightButton);
            const menu = findChild(sidebar, "videoFileContextMenu");
            tryCompare(menu, "opened", true);
            compare(folderSpy.count, 0);
            compare(recentSpy.count, 0);
            compare(contextOpenSpy.count, 0);
            compare(compareSpy.count, 0);
            compare(menu.targetUrl.toString(), expected);
            compare(folder.currentRow, 1);
            // The list index may move after the context target has been captured.
            list.currentIndex = 1;
            const originalRecent = folder.recentFiles;
            if (data.recent)
                folder.recentFiles = originalRecent.slice().reverse();
            settleContextMenu();
            mouseClick(findChild(sidebar, "videoFileContextCompare"));
            if (data.recent)
                folder.recentFiles = originalRecent;
            compare(compareSpy.count, 1);
            compare(compareSpy.signalArguments[0][0].toString(), expected);
            compare(folderSpy.count, 0);
            compare(recentSpy.count, 0);
        }

        function test_keyboardMenuOpenAndEscape_data() {
            return [
                {tag: "folder-menu", recent: false, key: Qt.Key_Menu, modifiers: Qt.NoModifier},
                {tag: "recent-shift-f10", recent: true, key: Qt.Key_F10, modifiers: Qt.ShiftModifier}
            ];
        }

        function test_keyboardMenuOpenAndEscape(data) {
            sidebar.showRecent = data.recent;
            const list = findChild(sidebar, data.recent ? "videoRecentFileList" : "videoFolderFileList");
            list.currentIndex = 1;
            list.forceActiveFocus();
            keyClick(data.key, data.modifiers);
            const menu = findChild(sidebar, "videoFileContextMenu");
            tryCompare(menu, "opened", true);
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "visible", false);
            compare(folderSpy.count, 0);
            compare(recentSpy.count, 0);
            compare(compareSpy.count, 0);
            list.forceActiveFocus();
            keyClick(data.key, data.modifiers);
            tryCompare(menu, "opened", true);
            settleContextMenu();
            mouseClick(findChild(sidebar, "videoFileContextOpen"));
            compare(contextOpenSpy.count, 1);
            compare(contextOpenSpy.signalArguments[0][0].toString(), list.itemAtIndex(1).contextUrl.toString());
            compare(folderSpy.count, 0);
            compare(recentSpy.count, 0);
        }

        function test_disabledReasonAndRevisionClose() {
            sidebar.testCompareReason = "当前已有 3 个视频，请先移除一个";
            const row = findChild(sidebar, "videoFolderFileList").itemAtIndex(0);
            mouseClick(row, 20, 20, Qt.RightButton);
            const menu = findChild(sidebar, "videoFileContextMenu");
            tryCompare(menu, "opened", true);
            const compareItem = findChild(sidebar, "videoFileContextCompare");
            verify(!compareItem.enabled);
            verify(findChild(sidebar, "videoFileContextOpen").enabled);
            compare(findChild(sidebar, "videoFileContextReason").text, sidebar.testCompareReason);
            mouseClick(compareItem);
            compare(compareSpy.count, 0);
            menu.close();
            tryCompare(menu, "visible", false);
            mouseClick(row, 20, 20, Qt.RightButton);
            tryCompare(menu, "opened", true);
            sidebar.actionRevision = "new-session";
            tryCompare(menu, "visible", false);
            sidebar.testOpenReason = "视频已移动、删除或不可读取";
            mouseClick(row, 20, 20, Qt.RightButton);
            tryCompare(menu, "opened", true);
            verify(!findChild(sidebar, "videoFileContextOpen").enabled);
            compare(findChild(sidebar, "videoFileContextReason").text, sidebar.testOpenReason);
            keyClick(Qt.Key_Escape);
            compare(contextOpenSpy.count, 0);
            compare(folderSpy.count, 0);
        }

        function test_keyboardActionDoesNotLeakToList_data() {
            return [{tag: "folder-open", recent: false, downs: 1},
                    {tag: "recent-compare", recent: true, downs: 2}];
        }
        function test_keyboardActionDoesNotLeakToList(data) {
            sidebar.showRecent = data.recent;
            const list = findChild(sidebar, data.recent ? "videoRecentFileList" : "videoFolderFileList");
            list.currentIndex = 1;
            list.forceActiveFocus();
            keyClick(Qt.Key_Menu);
            const menu = findChild(sidebar, "videoFileContextMenu");
            tryCompare(menu, "opened", true);
            for (let i = 0; i < data.downs; ++i)
                keyClick(Qt.Key_Down);
            keyClick(Qt.Key_Return);
            tryCompare(menu, "visible", false);
            compare(folderSpy.count, 0);
            compare(recentSpy.count, 0);
            compare(contextOpenSpy.count, data.downs === 1 ? 1 : 0);
            compare(compareSpy.count, data.downs === 2 ? 1 : 0);
        }

        function test_keyboardSelectionShowsTargetWithoutChangingPlayingRow_data() {
            return [{tag: "folder", recent: false}, {tag: "recent", recent: true}];
        }

        function test_keyboardSelectionShowsTargetWithoutChangingPlayingRow(data) {
            sidebar.showRecent = data.recent;
            const list = findChild(sidebar, data.recent ? "videoRecentFileList" : "videoFolderFileList");
            list.currentIndex = 1;
            list.forceActiveFocus();
            keyClick(Qt.Key_Up);
            compare(list.currentIndex, 0);
            const target = list.itemAtIndex(0);
            const playing = list.itemAtIndex(1);
            verify(target !== null && playing !== null);
            verify(!target.highlighted && playing.highlighted);
            compare(target.background.border.width, 1);
            compare(String(target.background.border.color), Theme.focus);
            compare(playing.background.border.width, 0);
            keyClick(Qt.Key_Return);
            const requested = data.recent ? recentSpy : folderSpy;
            compare(requested.count, 1);
            compare(requested.signalArguments[0][0], 0);
            keyClick(Qt.Key_Down);
            compare(list.currentIndex, 1);
            compare(target.background.border.width, 0);
            compare(playing.background.border.width, 1);
        }

        function test_focusedDelegateAndKeyboardMenuShareOneTarget_data() {
            return [{tag: "folder", recent: false}, {tag: "recent", recent: true}];
        }

        function test_focusedDelegateAndKeyboardMenuShareOneTarget(data) {
            sidebar.showRecent = data.recent;
            const list = findChild(sidebar, data.recent ? "videoRecentFileList" : "videoFolderFileList");
            list.currentIndex = 1;
            const row = list.itemAtIndex(0);
            verify(row !== null);
            row.forceActiveFocus(Qt.TabFocusReason);
            tryCompare(row, "activeFocus", true);
            compare(list.currentIndex, 0);
            compare(row.background.border.width, 1);
            keyClick(Qt.Key_Menu);
            const menu = findChild(sidebar, "videoFileContextMenu");
            tryCompare(menu, "opened", true);
            compare(menu.targetUrl.toString(), row.contextUrl.toString());
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "visible", false);
            compare(folderSpy.count + recentSpy.count + contextOpenSpy.count + compareSpy.count, 0);
        }

        function test_leavingListRemovesFocusRingWithoutOpening_data() {
            return [{tag: "folder", recent: false}, {tag: "recent", recent: true}];
        }

        function test_leavingListRemovesFocusRingWithoutOpening(data) {
            sidebar.showRecent = data.recent;
            const list = findChild(sidebar, data.recent ? "videoRecentFileList" : "videoFolderFileList");
            list.currentIndex = 0;
            list.forceActiveFocus();
            const row = list.itemAtIndex(0);
            verify(row !== null);
            compare(row.background.border.width, 1);
            sidebar.forceActiveFocus();
            tryCompare(list, "activeFocus", false);
            compare(row.background.border.width, 0);
            compare(list.currentIndex, 0);
            compare(folderSpy.count + recentSpy.count, 0);
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
