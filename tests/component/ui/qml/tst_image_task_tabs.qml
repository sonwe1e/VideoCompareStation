import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

// The strip is presentation-only: it must render one clickable tab per task, keep the close
// and middle-click routes distinct, and show the dirty marker only on the owning tab.
Item {
    id: root

    width: 760
    height: 160

    Dvs.ImageTaskTabs {
        id: tabs

        objectName: "imageTaskTabsTest"
        width: parent.width
        height: 34
        activeIndex: 0
        dirtyIndex: 1
        tabsModel: [
            {
                index: 0,
                title: "left.png",
                detail: "C:/images/left.png",
                loading: false,
                active: true,
                primaryPath: "C:/images/left.png",
                secondaryPath: ""
            },
            {
                index: 1,
                title: "right.png  ↔  reference.png",
                detail: "C:/images/right.png\nC:/images/reference.png",
                loading: false,
                active: false,
                primaryPath: "C:/images/right.png",
                secondaryPath: "C:/images/reference.png"
            }
        ]
    }

    SignalSpy {
        id: activatedSpy

        target: tabs
        signalName: "tabActivated"
    }

    Component {
        id: imageStrip
        Dvs.ImageTaskTabs {
            width: 300
            y: 60
            height: 36
            property int closedIndex: -1
            onTabActivated: index => activeIndex = index
            onTabCloseRequested: index => closedIndex = index
        }
    }
    Component {
        id: videoStrip
        Dvs.VideoTaskTabs {
            width: 300
            y: 60
            height: 36
            property int closedIndex: -1
            onTabActivated: index => activeIndex = index
            onTabCloseRequested: index => closedIndex = index
        }
    }

    SignalSpy {
        id: closeSpy

        target: tabs
        signalName: "tabCloseRequested"
    }

    TestCase {
        name: "ImageTaskTabs"
        when: windowShown

        function test_keyboardSelectionAndOverflow_data() {
            return [{ tag: "image", component: imageStrip, prefix: "image" },
                { tag: "video", component: videoStrip, prefix: "video" }];
        }

        function test_keyboardSelectionAndOverflow(data) {
            const model = [];
            for (let i = 0; i < 8; ++i)
                model.push({ title: "long-full-title-" + i + "-reference-and-original-comparison.png", detail: "C:/images/full-original-path-" + i + ".png", loading: false, restoring: false, sourceCount: 2 });
            const strip = createTemporaryObject(data.component, root, { tabsModel: model, activeIndex: 0 });
            verify(strip !== null);
            const list = findChild(strip, data.prefix + "TaskTabList");
            tryCompare(list, "count", 8);
            const first = list.itemAtIndex(0);
            verify(first !== null);
            first.forceActiveFocus(Qt.TabFocusReason);
            tryCompare(first, "activeFocus", true);
            compare(first.Accessible.role, Accessible.PageTab);
            compare(first.Accessible.name, model[0].title);
            verify(first.Accessible.selected);
            keyClick(Qt.Key_End);
            tryCompare(strip, "activeIndex", 7);
            tryVerify(() => list.itemAtIndex(7) !== null && list.itemAtIndex(7).activeFocus);
            const last = list.itemAtIndex(7);
            tryVerify(() => last.x + last.width <= list.contentX + list.width + 1);
            verify(list.contentX > 0);
            compare(last.Accessible.name, model[7].title);
            verify(last.Accessible.selected);
            keyClick(Qt.Key_Left);
            tryCompare(strip, "activeIndex", 6);
            tryVerify(() => list.itemAtIndex(6).activeFocus);
            keyClick(Qt.Key_Delete);
            compare(strip.closedIndex, 6);
            keyClick(Qt.Key_Home);
            tryCompare(strip, "activeIndex", 0);
            tryVerify(() => list.itemAtIndex(0).activeFocus);
            tryVerify(() => list.contentX <= 1);
            keyClick(Qt.Key_Tab);
            const close = findChild(strip, data.prefix + "TaskTabClose-0");
            tryCompare(close, "activeFocus", true);
            compare(close.Accessible.name, "关闭任务：" + model[0].title);
            keyClick(Qt.Key_Space);
            compare(strip.closedIndex, 0);
        }

        function test_rendersOneTabPerTask() {
            let list = findChild(tabs, "imageTaskTabList");
            verify(list !== null);
            compare(list.count, 2);
            verify(findChild(tabs, "imageTaskTab-0") !== null);
            verify(findChild(tabs, "imageTaskTab-1") !== null);
        }

        function test_clickingATabEmitsItsIndex() {
            activatedSpy.clear();
            const second = findChild(tabs, "imageTaskTab-1");
            verify(second !== null);
            mouseClick(second);
            compare(activatedSpy.count, 1);
            compare(activatedSpy.signalArguments[0][0], 1);
        }

        function test_closeButtonDoesNotActivateTheTab() {
            activatedSpy.clear();
            closeSpy.clear();
            const closeButton = findChild(tabs, "imageTaskTabClose-1");
            verify(closeButton !== null);
            mouseClick(closeButton);
            compare(closeSpy.count, 1);
            compare(closeSpy.signalArguments[0][0], 1);
            compare(activatedSpy.count, 0);
        }

        function test_dirtyMarkerFollowsItsTab() {
            const dirtyOne = findChild(tabs, "imageTaskTabDirty-1");
            const dirtyZero = findChild(tabs, "imageTaskTabDirty-0");
            verify(dirtyOne !== null);
            verify(dirtyZero !== null);
            verify(dirtyOne.visible);
            verify(!dirtyZero.visible);
        }
    }
}
