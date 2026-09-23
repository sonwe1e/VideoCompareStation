import QtQml

QtObject {
    function droppedUrlError(errorKey, detail) {
        switch (errorKey) {
        case "drop-empty":
            return qsTr("没有拖入任何文件。");
        case "drop-too-many":
            return qsTr("最多拖入三个视频文件。");
        case "drop-too-many-images":
            return qsTr("一次最多拖入两张图片；请选择两张进行对比。");
        case "drop-mixed-media":
            return qsTr("请分别打开图片和视频，不要混合拖入。");
        case "drop-invalid-local":
            return qsTr("只能拖入本地文件。");
        case "drop-missing":
            return qsTr("拖入的文件不存在：%1").arg(detail);
        case "drop-duplicate":
            return qsTr("同一文件被重复拖入：%1").arg(detail);
        default:
            return qsTr("拖入的文件无法打开。");
        }
    }

    function intentKindText(kind, sourceCount) {
        switch (Number(kind)) {
        case 0:
            return qsTr("打开 %1 个视频").arg(sourceCount);
        case 1:
            return qsTr("替换视频");
        case 2:
            return qsTr("添加视频");
        case 3:
            return qsTr("移除视频");
        case 4:
            return qsTr("更换参考源");
        case 5:
            return qsTr("关闭视频");
        default:
            return qsTr("检查请求");
        }
    }

    function intentErrorText(error) {
        switch (Number(error)) {
        case 2:
            return qsTr("请求队列已满。");
        case 3:
            return qsTr("视频列表已变化，请重试该操作。");
        case 4:
            return qsTr("无法启动检查请求。");
        case 5:
            return qsTr("检查请求失败。");
        default:
            return qsTr("检查请求被拒绝。");
        }
    }

    function errorMessage(errorKey) {
        switch (errorKey) {
        case "invalid-argument":
            return qsTr("请求无效。");
        case "invalid-rate":
            return qsTr("媒体帧率无效。");
        case "invalid-frame-id":
        case "frame-out-of-range":
            return qsTr("请求的帧超出可用范围。");
        case "invalid-frame-count":
            return qsTr("媒体帧数无效。");
        case "invalid-dimensions":
            return qsTr("媒体尺寸无效。");
        case "invalid-duration":
            return qsTr("媒体时长无效。");
        case "invalid-media-descriptor":
            return qsTr("媒体描述信息不完整或无效。");
        case "arithmetic-overflow":
            return qsTr("媒体时间值过大，无法安全处理。");
        case "source-frame-rate-mismatch":
            return qsTr("各源帧率不一致；对比前请先检查对齐。");
        case "source-frame-count-mismatch":
            return qsTr("各源帧数不一致；无法映射的帧保持缺失状态。");
        case "source-duration-mismatch":
            return qsTr("各源时长不一致；对比前请先检查对齐。");
        case "source-resolution-mismatch":
            return qsTr("各源分辨率不一致；对比画面将进行重采样。");
        case "source-color-metadata-mismatch":
            return qsTr("各源色彩元数据不一致；对比时将进行色彩转换。");
        case "source-missing":
            return qsTr("所选源文件不存在或无法读取。");
        case "source-fingerprint-mismatch":
            return qsTr("源文件与其记录的标识不再匹配。");
        case "file-io":
            return qsTr("无法读取或写入文件。");
        case "media-open-failed":
            return qsTr("无法打开媒体文件。");
        case "media-probe-failed":
            return qsTr("无法读取媒体信息。");
        case "invalid-cfr-timing":
            return qsTr("该媒体不支持所需的恒定帧率（CFR）时基。");
        case "unsupported-codec":
            return qsTr("不支持的编解码器。");
        case "unsupported-pixel-format":
            return qsTr("不支持的像素格式。");
        case "media-decode-failed":
            return qsTr("无法解码视频帧。");
        case "frame-timeline-invalid":
            return qsTr("视频时间轴无法精确定位每一帧。");
        case "frame-budget-exceeded":
            return qsTr("帧内存上限已超出。");
        case "graphics-unavailable":
            return qsTr("图形设备不可用。");
        case "graphics-device-lost":
            return qsTr("图形设备已重置或断开。");
        case "frame-presentation-timed-out":
            return qsTr("请求的帧未及时显示。");
        default:
            return qsTr("发生意外的媒体错误。");
        }
    }
}
