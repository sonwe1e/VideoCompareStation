pragma ComponentBehavior: Bound

import QtQuick

Rectangle {
    id: control

    required property var host

    height: control.host.chromeVisible && !control.host.singleMode ? 48 : 0
    visible: control.host.chromeVisible && !control.host.singleMode
    color: control.host.panelColor
    border.color: control.host.borderColor
    Flickable {
        id: comparisonScroller

        objectName: "comparisonScroller"
        clip: true
        contentWidth: comparisonControls.implicitWidth
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        anchors {
            fill: parent
            leftMargin: 14
            rightMargin: 14
        }

        Row {
            id: comparisonControls

            spacing: 8
            anchors.verticalCenter: parent.verticalCenter

            Text {
                text: qsTr("参考源")
                color: control.host.mutedTextColor
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }

            ToolbarCombo {
                id: referenceSourceCombo

                objectName: "referenceSourceCombo"
                implicitWidth: 112
                model: control.host.sourceCount >= 3 ? [qsTr("源 A"), qsTr("源 B"), qsTr("源 C")] : [qsTr("源 A"), qsTr("源 B")]
                currentIndex: Math.max(0, control.host.canonicalSourceIndex)
                Accessible.name: qsTr("基准参考源")
                onActivated: index => {
                    if (!control.host.changeReferenceAtIndex(index))
                        currentIndex = Math.max(0, control.host.canonicalSourceIndex);
                }
            }

            Text {
                text: qsTr("视图")
                color: control.host.mutedTextColor
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }

            ToolbarCombo {
                id: viewModeCombo

                objectName: "viewModeCombo"
                enabled: control.host.sourceCount > 1
                model: control.host.availableViewModes
                textRole: "label"
                valueRole: "value"
                currentIndex: {
                    for (let index = 0; index < control.host.availableViewModes.length; ++index) {
                        if (Number(control.host.availableViewModes[index].value) === control.host.effectiveViewMode)
                            return index;
                    }
                    return 0;
                }
                Accessible.name: qsTr("对比视图")
                onActivated: {
                    if (control.host.preferences)
                        control.host.preferences.viewMode = Number(currentValue);
                }
            }

            Text {
                visible: control.host.differenceMode
                text: qsTr("度量")
                color: control.host.mutedTextColor
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }

            ToolbarCombo {
                id: differenceMetricCombo

                objectName: "differenceMetricCombo"
                visible: control.host.differenceMode
                model: [qsTr("RGB 绝对值"), qsTr("亮度"), qsTr("色度"), qsTr("热力图"), qsTr("精确平面")]
                currentIndex: control.host.preferences ? Number(control.host.preferences.differenceMetric) : 0
                Accessible.name: qsTr("差异度量")
                onActivated: index => {
                    if (control.host.preferences)
                        control.host.preferences.differenceMetric = index;
                }
            }

            ToolbarCombo {
                id: differenceGainCombo

                objectName: "differenceGainCombo"
                visible: control.host.differenceMode
                implicitWidth: 76
                model: ["1x", "2x", "4x", "8x", "16x"]
                currentIndex: control.host.preferences ? Number(control.host.preferences.differenceGain) : 0
                Accessible.name: qsTr("差异增益")
                onActivated: index => {
                    if (control.host.preferences)
                        control.host.preferences.differenceGain = index;
                }
            }

            ToolbarCombo {
                id: differenceEdgeCombo

                objectName: "differenceEdgeCombo"
                visible: control.host.differenceMode || control.host.wipeMode
                implicitWidth: 94
                model: control.host.differenceEdges
                textRole: "label"
                currentIndex: control.host.differenceEdgeIndex(control.host.differenceEdge)
                Accessible.name: qsTr("差异源对")
                onActivated: index => {
                    if (control.host.preferences && index >= 0 && index < control.host.differenceEdges.length)
                        control.host.preferences.differenceEdge = Number(control.host.differenceEdges[index].preferenceValue);
                }
            }

            ToolbarCombo {
                id: differenceFilterCombo

                objectName: "differenceFilterCombo"
                visible: control.host.differenceMode
                implicitWidth: 104
                model: [qsTr("最近邻"), qsTr("双线性"), qsTr("双三次")]
                currentIndex: control.host.preferences ? Number(control.host.preferences.differenceFilter) : 1
                Accessible.name: qsTr("空间重采样滤镜")
                onActivated: index => {
                    if (control.host.preferences)
                        control.host.preferences.differenceFilter = index;
                }
            }

            Text {
                visible: control.host.differenceMode
                text: qsTr("分辨率不一致时会重采样，结果并非逐像素精确。")
                color: control.host.mutedTextColor
                font.pixelSize: 10
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
