/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "v4l2_source_node.h"
#include "metadata_controller.h"
#include <unistd.h>
#include "v4l2_utils.h"

namespace OHOS::Camera {
V4L2SourceNode::V4L2SourceNode(const std::string& name, const std::string& type, const std::string &cameraId)
    : SourceNode(name, type, cameraId), NodeBase(name, type, cameraId)
{
    CAMERA_LOGI("%s enter, type(%s)\n", name_.c_str(), type_.c_str());
    RetCode rc = RC_OK;
    constexpr int itemCapacitySize = 30;
    constexpr int dataCapacitySize = 1000;
    deviceManager_ = IDeviceManager::GetInstance();
    if (deviceManager_ == nullptr) {
        CAMERA_LOGE("get device manager failed.");
        return;
    }
    rc = GetDeviceController();
    if (rc == RC_ERROR) {
        CAMERA_LOGE("GetDeviceController failed.");
        return;
    }
    meta_ = std::make_shared<CameraMetadata>(itemCapacitySize, dataCapacitySize);
}

RetCode V4L2SourceNode::GetDeviceController()
{
    CameraId cameraId = CAMERA_FIRST;
    sensorController_ = std::static_pointer_cast<SensorController>
        (deviceManager_->GetController(cameraId, DM_M_SENSOR, DM_C_SENSOR));
    if (sensorController_ == nullptr) {
        CAMERA_LOGE("get device controller failed");
        return RC_ERROR;
    }
    return RC_OK;
}

RetCode V4L2SourceNode::Init(const int32_t streamId)
{
    return RC_OK;
}

RetCode V4L2SourceNode::Start(const int32_t streamId)
{
    RetCode rc = RC_OK;
    deviceManager_ = IDeviceManager::GetInstance();
    if (deviceManager_ == nullptr) {
        CAMERA_LOGE("get device manager failed.");
        return RC_ERROR;
    }
    rc = GetDeviceController();
    if (rc == RC_ERROR) {
        CAMERA_LOGE("GetDeviceController failed.");
        return RC_ERROR;
    }
    std::vector<std::shared_ptr<IPort>> outPorts = GetOutPorts();
    for (const auto& it : outPorts) {
        DeviceFormat format;
        format.fmtdesc.pixelformat = V4L2Utils::ConvertPixfmtHal2V4l2(
            static_cast<OHOS::Camera::CameraBufferFormat>(it->format_.format_));
        format.fmtdesc.width = wide_;
        format.fmtdesc.height = high_;
        int bufCnt = it->format_.bufferCount_;
        rc = sensorController_->Start(bufCnt, format);
        if (rc == RC_ERROR) {
            CAMERA_LOGE("start failed.");
            return RC_ERROR;
        }
    }
    isAdjust_ = true;
    if (meta_ != nullptr) {
        sensorController_->ConfigFps(meta_);
    }

    rc = SourceNode::Start(streamId);
    return rc;
}

V4L2SourceNode::~V4L2SourceNode()
{
    CAMERA_LOGV("%{public}s, v4l2 source node dtor.", __FUNCTION__);
}

RetCode V4L2SourceNode::Flush(const int32_t streamId)
{
    RetCode rc;

    if (sensorController_ != nullptr) {
        rc = sensorController_->Flush(streamId);
        CHECK_IF_NOT_EQUAL_RETURN_VALUE(rc, RC_OK, RC_ERROR);
    }
    rc = SourceNode::Flush(streamId);

    return rc;
}

RetCode V4L2SourceNode::Stop(const int32_t streamId)
{
    RetCode rc;

    if (sensorController_ != nullptr) {
        rc = sensorController_->Stop();
        CHECK_IF_NOT_EQUAL_RETURN_VALUE(rc, RC_OK, RC_ERROR);
    }

    return SourceNode::Stop(streamId);
}

RetCode V4L2SourceNode::SetCallback()
{
    MetadataController &metaDataController = MetadataController::GetInstance();
    metaDataController.AddNodeCallback([this](const std::shared_ptr<CameraMetadata> &metadata) {
        OnMetadataChanged(metadata);
    });
    return RC_OK;
}

int32_t V4L2SourceNode::GetStreamId(const CaptureMeta &meta)
{
    common_metadata_header_t *data = meta->get();
    if (data == nullptr) {
        CAMERA_LOGE("data is nullptr");
        return RC_ERROR;
    }
    camera_metadata_item_t entry;
    int32_t streamId = -1;
    int rc = FindCameraMetadataItem(data, OHOS_CAMERA_STREAM_ID, &entry);
    if (rc == 0) {
        streamId = *entry.data.i32;
    }
    return streamId;
}

void V4L2SourceNode::OnMetadataChanged(const std::shared_ptr<CameraMetadata>& metadata)
{
    if (metadata == nullptr) {
        CAMERA_LOGE("meta is nullptr");
        return;
    }
    constexpr uint32_t DEVICE_STREAM_ID = 0;
    if (sensorController_ != nullptr) {
        if (GetStreamId(metadata) == DEVICE_STREAM_ID) {
            sensorController_->Configure(metadata);
        }
    } else {
        CAMERA_LOGE("V4L2SourceNode sensorController_ is null");
    }
    GetUpdateFps(metadata);
}

void V4L2SourceNode::SetBufferCallback()
{
    sensorController_->SetNodeCallBack([&](std::shared_ptr<FrameSpec> frameSpec) {
            OnPackBuffer(frameSpec);
    });
    return;
}

RetCode V4L2SourceNode::ProvideBuffers(std::shared_ptr<FrameSpec> frameSpec)
{
    CAMERA_LOGI("provide buffers enter.");
    if (sensorController_->SendFrameBuffer(frameSpec) == RC_OK) {
        CAMERA_LOGI("sendframebuffer success bufferpool id = %llu", frameSpec->bufferPoolId_);
        return RC_OK;
    }
    return RC_ERROR;
}

void V4L2SourceNode::GetUpdateFps(const std::shared_ptr<CameraMetadata>& metadata)
{
    common_metadata_header_t *data = metadata->get();
    camera_metadata_item_t entry;
    int ret = FindCameraMetadataItem(data, OHOS_CONTROL_FPS_RANGES, &entry);
    if (ret == 0) {
        std::vector<int32_t> fpsRange;
        for (int i = 0; i < entry.count; i++) {
            fpsRange.push_back(*(entry.data.i32 + i));
        }
        meta_->addEntry(OHOS_CONTROL_FPS_RANGES, fpsRange.data(), fpsRange.size());
    }
}
void V4L2SourceNode::DeliverBuffer(std::shared_ptr<IBuffer>& buffer)
{
    if (buffer == nullptr) {
        CAMERA_LOGE("UvcNode::DeliverBuffer frameSpec is null");
        return;
    }
    CAMERA_LOGI("V4L2SourceNode::DeliverBuffer Begin, streamId[%{public}d], index[%{public}d]",
        buffer->GetStreamId(), buffer->GetIndex());
    buffer->SetCurFormat(CAMERA_FORMAT_YCRCB_420_P);
    buffer->SetCurWidth(wide_);
    buffer->SetCurHeight(high_);
    SourceNode::DeliverBuffer(buffer);
}

REGISTERNODE(V4L2SourceNode, {"v4l2_source"})
} // namespace OHOS::Camera
