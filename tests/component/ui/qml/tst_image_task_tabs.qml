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

    SignalSpy {
        id: closeSpy

        target: tabs
        signalName: "tabCloseRequested"
    }

    TestCase {
        name: "ImageTaskTabs"
        when: windowShown

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
