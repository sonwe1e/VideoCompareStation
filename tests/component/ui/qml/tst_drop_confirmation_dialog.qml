import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

Item {
    id: root

    width: 720
    height: 520

    // The header/footer opaque-plate samples must follow the theme token they assert, so a
    // palette change updates this probe without weakening the contract: the point is that the
    // header paints its own opaque panel surface over the magenta plate below.
    readonly property color themePanel: Theme.panel

    Rectangle {
        anchors.fill: parent
        color: "#ffff00ff"
    }

    Dvs.DropConfirmationDialog {
        id: dialog

        pendingVideos: ["file:///C:/video%20one.mp4", "file:///C:/%E8%A7%86%E9%A2%91%20%E4%BA%8C.mp4"]
        fileNameFunction: function (url) {
            return decodeURIComponent(url.toString()).split("/").pop();
        }
        pathNameFunction: function (url) {
            const decoded = decodeURIComponent(url.toString());
            const withoutScheme = decoded.startsWith("file:///") ? decoded.substring(8) : decoded;
            const separator = Math.max(withoutScheme.lastIndexOf("/"), withoutScheme.lastIndexOf("\\"));
            if (separator < 0)
                return "";
            const rest = withoutScheme.substring(0, separator);
            const parentSeparator = Math.max(rest.lastIndexOf("/"), rest.lastIndexOf("\\"));
            return parentSeparator >= 0 ? rest.substring(parentSeparator + 1) : rest;
        }
    }

    SignalSpy {
        id: acceptedSpy

        target: dialog
        signalName: "accepted"
    }

    SignalSpy {
        id: rejectedSpy

        target: dialog
        signalName: "rejected"
    }

    TestCase {
        name: "DropConfirmationDialog"
        when: windowShown

        function test_opaque_complete_popup() {
            const windowContent = root.Window.window.contentItem;
            const beforeOpen = grabImage(windowContent);
            const originalVideos = dialog.pendingVideos.slice();

            dialog.open();
            tryCompare(dialog, "visible", true);
            wait(50);

            const background = findChild(dialog, "dropDialogBackground");
            const header = findChild(dialog, "dropDialogHeader");
            const footer = findChild(dialog, "dropDialogFooter");
            const reference = findChild(dialog, "dropReferenceCombo");
            verify(background !== null);
            verify(header !== null);
            verify(footer !== null);
            verify(reference !== null);
            compare(background.color.a, 1.0);
            compare(header.color.a, 1.0);
            compare(footer.color.a, 1.0);
            verify(dialog.modal);
            verify(dialog.dim);
            compare(dialog.popupType, Popup.Item);
            verify(dialog.width <= root.width - 48);
            verify(dialog.width > 400);

            const okButton = findChild(dialog, "dropFooterOkButton");
            const cancelButton = findChild(dialog, "dropFooterCancelButton");
            verify(okButton !== null);
            verify(cancelButton !== null);
            verify(okButton.visible);
            verify(cancelButton.visible);
            verify(okButton.implicitHeight < 40);
            compare(okButton.background.color.a, 1.0);

            // Header, body, and footer must share one continuous opaque column — no
            // transparent seams between chrome pieces.
            verify(header.width === background.width);
            verify(footer.width === background.width);
            verify(Math.abs(header.y - background.y) < 1.0);
            verify(footer.y + footer.height <= background.y + background.height + 1.0);

            const opened = grabImage(windowContent);
            const scaleX = opened.width / windowContent.width;
            const scaleY = opened.height / windowContent.height;
            const headerPoint = header.mapToItem(windowContent, header.width - 24, 24);
            const headerX = Math.round(headerPoint.x * scaleX);
            const headerY = Math.round(headerPoint.y * scaleY);
            compare(opened.alpha(headerX, headerY), 255);
            // Theme.panel: the header paints its own opaque surface over the magenta.
            fuzzyCompare(opened.red(headerX, headerY), Math.round(root.themePanel.r * 255), 3);
            fuzzyCompare(opened.green(headerX, headerY), Math.round(root.themePanel.g * 255), 3);
            fuzzyCompare(opened.blue(headerX, headerY), Math.round(root.themePanel.b * 255), 3);

            // An opaque backdrop has alpha 255 even through a hole. Check the actual chrome
            // colors on both sides of each seam, including the 10px square corner-fill strips.
            let seamSamples = 0;
            function compareChrome(item, x, y, expected) {
                const point = item.mapToItem(windowContent, x, y);
                const px = Math.round(point.x * scaleX);
                const py = Math.round(point.y * scaleY);
                verify(px >= 0 && px < opened.width && py >= 0 && py < opened.height);
                fuzzyCompare(opened.red(px, py), Math.round(expected.r * 255), 3);
                fuzzyCompare(opened.green(px, py), Math.round(expected.g * 255), 3);
                fuzzyCompare(opened.blue(px, py), Math.round(expected.b * 255), 3);
                seamSamples += 1;
            }
            for (const x of [24, header.width / 2, header.width - 24]) {
                for (const delta of [-6, -2, 2, 6]) {
                    compareChrome(header, x, header.height + delta, delta < 0 ? header.color : background.color);
                    compareChrome(footer, x, delta, delta < 0 ? background.color : footer.color);
                }
            }
            compare(seamSamples, 24);

            const outsideX = Math.round(12 * scaleX);
            const outsideY = Math.round(12 * scaleY);
            verify(opened.red(outsideX, outsideY) < beforeOpen.red(outsideX, outsideY));
            verify(opened.blue(outsideX, outsideY) < beforeOpen.blue(outsideX, outsideY));

            // The spies are file-scope and other tests may have accepted or rejected before
            // this one runs (Qt Quick Test executes in declaration order, not file order);
            // this test owns its click, so it starts its counts from zero.
            acceptedSpy.clear();
            rejectedSpy.clear();
            mouseClick(okButton);
            tryCompare(dialog, "visible", false);
            compare(acceptedSpy.count, 1);
            compare(dialog.pendingVideos, originalVideos);

            dialog.open();
            tryCompare(dialog, "visible", true);
            mouseClick(findChild(dialog, "dropFooterCancelButton"));
            tryCompare(dialog, "visible", false);
            compare(rejectedSpy.count, 1);
            compare(dialog.pendingVideos, originalVideos);
        }

        function test_preserves_initial_reference_identity_while_reordering() {
            dialog.pendingVideos = ["file:///C:/a.mp4", "file:///C:/b.mp4", "file:///C:/c.mp4"];
            dialog.initialReferenceIndex = 1;
            dialog.open();
            tryCompare(dialog, "visible", true);
            compare(dialog.referenceIndex, 1);

            dialog.requestMove(1, 2);
            compare(dialog.referenceIndex, 2);
            dialog.close();

            dialog.initialReferenceIndex = -1;
            dialog.open();
            tryCompare(dialog, "visible", true);
            compare(dialog.referenceIndex, 0);
            dialog.close();
        }

        // The confirm button owns the popup's initial focus: Enter confirms without a Tab
        // walk, while Space stays inert — modal media review keeps Space free of any dialog
        // activation (keyboard ownership contract in MainQmlContractTests).
        function test_enter_confirms_and_space_stays_inert() {
            acceptedSpy.clear();
            rejectedSpy.clear();
            dialog.open();
            tryCompare(dialog, "visible", true);
            const okButton = findChild(dialog, "dropFooterOkButton");
            verify(okButton !== null);
            tryCompare(okButton, "activeFocus", true);

            keyClick(Qt.Key_Space);
            compare(acceptedSpy.count, 0);
            compare(rejectedSpy.count, 0);
            verify(dialog.visible);

            keyClick(Qt.Key_Return);
            tryCompare(dialog, "visible", false);
            compare(acceptedSpy.count, 1);
            compare(rejectedSpy.count, 0);
        }

        function test_same_basename_shows_distinct_parent_paths() {
            dialog.pendingVideos = ["file:///C:/folderA/A.mp4", "file:///C:/folderB/A.mp4"];
            compare(dialog.sourceNameAt(0), "A.mp4");
            compare(dialog.sourceNameAt(1), "A.mp4");
            compare(dialog.sourcePathAt(0), "folderA");
            compare(dialog.sourcePathAt(1), "folderB");
            verify(dialog.sourcePathAt(0) !== dialog.sourcePathAt(1));

            dialog.open();
            tryCompare(dialog, "visible", true);
            wait(80);

            const rootItem = dialog.contentItem;
            verify(rootItem !== null);
            let foundNames = [];
            let foundPaths = [];
            const collect = item => {
                for (let i = 0; i < item.data.length; ++i) {
                    const child = item.data[i];
                    if (!child)
                        continue;
                    const objectName = String(child.objectName || "");
                    if (objectName.indexOf("dropSourceName-") === 0)
                        foundNames.push(String(child.text));
                    if (objectName.indexOf("dropSourcePath-") === 0)
                        foundPaths.push(String(child.text));
                    if (child.data)
                        collect(child);
                }
            };
            collect(rootItem);
            compare(foundNames.length, 2);
            compare(foundPaths.length, 2);
            compare(foundNames[0], "A.mp4");
            compare(foundNames[1], "A.mp4");
            compare(foundPaths[0], "folderA");
            compare(foundPaths[1], "folderB");
            dialog.close();
        }
    }
}
