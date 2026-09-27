pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Resize (resample) and canvas-fill dialogs for the still-image editor.
//
// Both are plain Popups rather than Dialogs: a Dialog with a `title` draws style chrome of
// its own around any custom background, which shows up as stray bars in the dark theme.
// Both share one shell so the two panels cannot drift apart.
Item {
    id: control

    objectName: "imageEditDialogs"

    required property var imageEdit
    required property int imageWidth
    required property int imageHeight
    required property int maximumEdge
    required property int minimumEdge

    readonly property bool anyVisible: scaleDialog.visible || fillDialog.visible

    // Declared properties so the dialogs can be driven from outside (tests, menus) and so the
    // last value survives a close: reopening a size dialog on a surprising default is worse
    // than reopening it on what the user last typed.
    property int scaleWidth: imageWidth
    property int scaleHeight: imageHeight
    property bool scaleLockAspect: true
    property bool scaleSmooth: true
    property int fillWidth: imageWidth
    property int fillHeight: imageHeight
    property color fillColor: "#ff000000"
    property int fillColorIndex: 0

    // Opaque-first palette: a transparent fill only works on an image that already has an
    // alpha channel, and an accidental transparent fill on an opaque photo is invisible
    // damage, so the safe choices come first.
    readonly property var fillChoices: [
        {
            "label": qsTr("黑"),
            "color": Theme.canvas
        },
        {
            "label": qsTr("白"),
            "color": Theme.inverseText
        },
        {
            "label": qsTr("中灰"),
            "color": Theme.controlBorder
        },
        {
            "label": qsTr("透明"),
            "color": Qt.rgba(0, 0, 0, 0)
        }
    ]
    readonly property color transparentFill: Qt.rgba(0, 0, 0, 0)

    signal scaleApplied(int width, int height, bool smooth)
    signal fillApplied(int width, int height, color fillColor)

    function openScale() {
        const width = imageWidth > 0 ? imageWidth : 1;
        const height = imageHeight > 0 ? imageHeight : 1;
        scaleDialog.aspectRatio = width / Math.max(1, height);
        control.scaleWidth = width;
        control.scaleHeight = height;
        scaleDialog.open();
    }

    function openFill() {
        control.fillWidth = imageWidth > 0 ? imageWidth : 1;
        control.fillHeight = imageHeight > 0 ? imageHeight : 1;
        fillDialog.open();
    }

    // A shared spinner keeps both fields identical: the same bounds, the same width, and the
    // same "typing past the limit is clamped, not rejected" behaviour as the controller.
    component EdgeField: SpinBox {
        id: field

        property string accessibleLabel: ""

        implicitWidth: 132
        implicitHeight: 30
        editable: true
        from: control.minimumEdge
        to: control.maximumEdge
        Accessible.name: field.accessibleLabel

        contentItem: TextInput {
            id: fieldInput

            text: field.textFromValue(field.value, field.locale)
            color: Theme.primaryText
            font.pixelSize: 13
            selectByMouse: true
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator {
                bottom: control.minimumEdge
                top: control.maximumEdge
            }
            onEditingFinished: field.value = Math.max(control.minimumEdge, Math.min(control.maximumEdge, field.valueFromText(fieldInput.text, field.locale)))
        }

        background: Rectangle {
            radius: 5
            color: Theme.control
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? Theme.focus : Theme.controlBorder
        }

        up.indicator: Item {}
        down.indicator: Item {}
    }

    component DialogShell: Popup {
        id: shell

        property string title: ""
        property string subtitle: ""
        property string confirmText: qsTr("应用")
        property string confirmObjectName: ""

        default property alias shellBody: bodySlot.data
        readonly property alias bodyWidth: bodySlot.width

        signal confirmed

        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        popupType: Popup.Item
        modal: true
        dim: true
        focus: true
        padding: 0
        width: Math.min(440, (parent ? parent.width : 800) - 48)
        closePolicy: Popup.CloseOnEscape

        Overlay.modal: Rectangle {
            color: Theme.modalScrim
        }

        background: Rectangle {
            color: Theme.menu
            radius: 10
            border.width: 1
            border.color: Theme.menuBorder
        }

        contentItem: Item {
            implicitWidth: 392
            implicitHeight: shellColumn.implicitHeight

            Column {
                id: shellColumn

                width: parent.width

                Rectangle {
                    width: parent.width
                    height: 48
                    color: Theme.panel
                    radius: 10

                    Rectangle {
                        height: 10
                        color: parent.color
                        anchors {
                            left: parent.left
                            right: parent.right
                            bottom: parent.bottom
                        }
                    }

                    Text {
                        text: shell.title
                        color: Theme.primaryText
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        anchors {
                            left: parent.left
                            leftMargin: 20
                            verticalCenter: parent.verticalCenter
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: bodySlot.implicitHeight + 28

                    Column {
                        id: bodySlot

                        width: parent.width - 40
                        anchors {
                            top: parent.top
                            topMargin: 16
                            horizontalCenter: parent.horizontalCenter
                        }
                        spacing: 12

                        Text {
                            width: parent.width
                            visible: shell.subtitle.length > 0
                            text: shell.subtitle
                            color: Theme.mutedText
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: footerRow.implicitHeight + 24
                    color: Theme.panel
                    radius: 10

                    Rectangle {
                        height: 10
                        color: parent.color
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                        }
                    }

                    Row {
                        id: footerRow

                        spacing: 8
                        anchors {
                            right: parent.right
                            rightMargin: 12
                            verticalCenter: parent.verticalCenter
                        }

                        ReviewActionButton {
                            objectName: shell.objectName + "CancelButton"
                            text: qsTr("取消")
                            implicitWidth: 84
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: shell.close()
                        }
                        ReviewActionButton {
                            objectName: shell.confirmObjectName
                            text: shell.confirmText
                            prominent: true
                            implicitWidth: 84
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: shell.confirmed()
                        }
                    }
                }
            }
        }
    }

    // Size in pixels, not viewport zoom: the working copy is resampled to exactly this many
    // pixels on apply, which is what a reviewer needs before shipping an asset.
    DialogShell {
        id: scaleDialog

        objectName: "imageEditScaleDialog"
        title: qsTr("缩放图片")
        subtitle: qsTr("输入新的像素尺寸。锁定比例时改一边会按原比例推算另一边。")
        confirmObjectName: "imageEditScaleApplyButton"

        // Width divided by height of the size the dialog opened with; presets multiply both
        // edges by a factor so "200%" means twice as large in each direction.
        property real aspectRatio: 1

        function applyPreset(factor) {
            const width = Math.max(control.minimumEdge, Math.min(control.maximumEdge, Math.round(control.imageWidth * factor)));
            const height = Math.max(control.minimumEdge, Math.min(control.maximumEdge, Math.round(control.imageHeight * factor)));
            control.scaleWidth = width;
            control.scaleHeight = height;
        }

        onConfirmed: {
            control.scaleApplied(control.scaleWidth, control.scaleHeight, control.scaleSmooth);
            scaleDialog.close();
        }

        Column {
            spacing: 10

            Row {
                spacing: 12

                Text {
                    width: 44
                    text: qsTr("宽")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
                EdgeField {
                    objectName: "imageEditScaleWidthField"
                    accessibleLabel: qsTr("宽度像素")
                    value: control.scaleWidth
                    onValueModified: {
                        control.scaleWidth = value;
                        if (control.scaleLockAspect)
                            control.scaleHeight = Math.max(control.minimumEdge, Math.min(control.maximumEdge, Math.round(value / scaleDialog.aspectRatio)));
                    }
                }
                Text {
                    text: qsTr("px")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: 12

                Text {
                    width: 44
                    text: qsTr("高")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
                EdgeField {
                    objectName: "imageEditScaleHeightField"
                    accessibleLabel: qsTr("高度像素")
                    value: control.scaleHeight
                    onValueModified: {
                        control.scaleHeight = value;
                        if (control.scaleLockAspect)
                            control.scaleWidth = Math.max(control.minimumEdge, Math.min(control.maximumEdge, Math.round(value * scaleDialog.aspectRatio)));
                    }
                }
                Text {
                    text: qsTr("px")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: 8

                ReviewActionButton {
                    objectName: "imageEditScaleLockAspect"
                    checkable: true
                    checked: control.scaleLockAspect
                    text: qsTr("锁定比例")
                    implicitHeight: 28
                    implicitWidth: 96
                    leftPadding: 10
                    rightPadding: 10
                    helpText: qsTr("勾选后改一边尺寸会按当前宽高比推算另一边。")
                    onClicked: {
                        control.scaleLockAspect = checked;
                        if (checked)
                            scaleDialog.aspectRatio = control.scaleWidth / Math.max(1, control.scaleHeight);
                    }
                }
                ReviewActionButton {
                    objectName: "imageEditScalePreset50"
                    text: qsTr("50%")
                    implicitHeight: 28
                    implicitWidth: 66
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: scaleDialog.applyPreset(0.5)
                }
                ReviewActionButton {
                    objectName: "imageEditScalePreset100"
                    text: qsTr("100%")
                    implicitHeight: 28
                    implicitWidth: 66
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: scaleDialog.applyPreset(1.0)
                }
                ReviewActionButton {
                    objectName: "imageEditScalePreset200"
                    text: qsTr("200%")
                    implicitHeight: 28
                    implicitWidth: 66
                    leftPadding: 8
                    rightPadding: 8
                    onClicked: scaleDialog.applyPreset(2.0)
                }
            }

            ReviewActionButton {
                objectName: "imageEditScaleSmooth"
                checkable: true
                checked: control.scaleSmooth
                text: control.scaleSmooth ? qsTr("平滑重采样") : qsTr("最近邻（像素精确）")
                implicitHeight: 28
                implicitWidth: 188
                leftPadding: 10
                rightPadding: 10
                helpText: qsTr("平滑适合缩放照片；最近邻只复制原像素，适合像素画与逐像素对比。")
                onClicked: control.scaleSmooth = checked
            }
        }
    }

    DialogShell {
        id: fillDialog

        objectName: "imageEditFillDialog"
        title: qsTr("填充画布")
        subtitle: qsTr("原图居中放在新画布上，四周用所选颜色补齐。画布不能小于当前图片。")
        confirmObjectName: "imageEditFillApplyButton"

        onConfirmed: {
            control.fillApplied(control.fillWidth, control.fillHeight, control.fillColor);
            fillDialog.close();
        }

        Column {
            spacing: 10

            Row {
                spacing: 12

                Text {
                    width: 44
                    text: qsTr("画布宽")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
                EdgeField {
                    objectName: "imageEditFillWidthField"
                    accessibleLabel: qsTr("画布宽度像素")
                    value: control.fillWidth
                    onValueModified: control.fillWidth = value
                }
                Text {
                    text: qsTr("px")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: 12

                Text {
                    width: 44
                    text: qsTr("画布高")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
                EdgeField {
                    objectName: "imageEditFillHeightField"
                    accessibleLabel: qsTr("画布高度像素")
                    value: control.fillHeight
                    onValueModified: control.fillHeight = value
                }
                Text {
                    text: qsTr("px")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Row {
                spacing: 12

                Text {
                    width: 44
                    text: qsTr("填充色")
                    color: Theme.mutedText
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                Row {
                    spacing: 6

                    Repeater {
                        model: control.fillChoices

                        delegate: Rectangle {
                            id: swatch

                            required property int index
                            required property var modelData

                            objectName: "imageEditFillColor" + swatch.index
                            width: 26
                            height: 26
                            radius: 4
                            color: swatch.modelData.color
                            border.width: control.fillColorIndex === swatch.index ? 2 : 1
                            border.color: control.fillColorIndex === swatch.index ? Theme.focus : Theme.controlBorder

                            // Transparency has no colour of its own to show, so the swatch is
                            // laid over the checkerboard the viewport already uses.
                            Image {
                                anchors.fill: parent
                                anchors.margins: 1
                                visible: swatch.modelData.color === control.transparentFill
                                sourceSize: Qt.size(48, 48)
                                fillMode: Image.Tile
                                source: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAIklEQVR42mPU0tJigAEhISE4m4kBByBdggXZ3Hfv3tHCDgCVFwPG8VA+nQAAAABJRU5ErkJggg=="
                            }

                            Accessible.name: qsTr("填充色：%1").arg(swatch.modelData.label)

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    control.fillColorIndex = swatch.index;
                                    control.fillColor = swatch.modelData.color;
                                }
                            }

                            VcsToolTip {
                                visible: swatchHover.hovered
                                text: swatch.modelData.label
                            }

                            HoverHandler {
                                id: swatchHover
                            }
                        }
                    }
                }
            }

            Text {
                width: parent.width
                visible: control.fillColorIndex === control.fillChoices.length - 1
                text: qsTr("透明填充只在当前图片已有透明通道时可用，否则会提示并保持原图不变。")
                color: Theme.warning
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }
}
