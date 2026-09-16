import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 720
    height: 520

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
            fuzzyCompare(opened.red(headerX, headerY), 17, 3);
            fuzzyCompare(opened.green(headerX, headerY), 24, 3);
            fuzzyCompare(opened.blue(headerX, headerY), 35, 3);

            // Sample just below the header and just above the footer: both must stay opaque.
            const midHeaderPoint = header.mapToItem(windowContent, header.width / 2, header.height + 6);
            const midHeaderX = Math.round(midHeaderPoint.x * scaleX);
            const midHeaderY = Math.round(midHeaderPoint.y * scaleY);
            compare(opened.alpha(midHeaderX, midHeaderY), 255);

            const footerTopPoint = footer.mapToItem(windowContent, footer.width / 2, 4);
            const footerTopX = Math.round(footerTopPoint.x * scaleX);
            const footerTopY = Math.round(footerTopPoint.y * scaleY);
            compare(opened.alpha(footerTopX, footerTopY), 255);

            const outsideX = Math.round(12 * scaleX);
            const outsideY = Math.round(12 * scaleY);
            verify(opened.red(outsideX, outsideY) < beforeOpen.red(outsideX, outsideY));
            verify(opened.blue(outsideX, outsideY) < beforeOpen.blue(outsideX, outsideY));

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
