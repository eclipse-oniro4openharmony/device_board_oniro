/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

#include "surface_image.h"

#include "securec.h"
#include "sandbox_utils.h"
#include "surface_utils.h"

#include <atomic>
#include <sync_fence.h>
#include <unistd.h>
#include <window.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

namespace OHOS {
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
namespace {
// static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
// Get a uniqueID in a process
static int GetProcessUniqueId()
{
    static std::atomic<int> g_counter { 0 };
    return g_counter.fetch_add(1);
}
}

SurfaceImage::SurfaceImage(uint32_t textureId, uint32_t textureTarget)
    : ConsumerSurface("SurfaceImage-" + std::to_string(GetRealPid()) + "-" + std::to_string(GetProcessUniqueId())),
      textureId_(textureId),
      textureTarget_(textureTarget),
      updateSurfaceImage_(false),
      eglDisplay_(EGL_NO_DISPLAY),
      eglContext_(EGL_NO_CONTEXT),
      currentSurfaceImage_(UINT_MAX),
      currentSurfaceBuffer_(nullptr),
      currentTimeStamp_(0)
{
    InitSurfaceImage();
}

SurfaceImage::~SurfaceImage()
{
    for (auto it = imageCacheSeqs_.begin(); it != imageCacheSeqs_.end(); it++) {
        if (it->second.eglImage_ != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR(eglDisplay_, it->second.eglImage_);
            it->second.eglImage_ = EGL_NO_IMAGE_KHR;
        }
        if (it->second.eglSync_ != EGL_NO_SYNC_KHR) {
            eglDestroySyncKHR(eglDisplay_, it->second.eglSync_);
            it->second.eglSync_ = EGL_NO_SYNC_KHR;
        }
    }
}

void SurfaceImage::InitSurfaceImage()
{
    std::string name = "SurfaceImage-" + std::to_string(GetRealPid()) + "-" + std::to_string(GetProcessUniqueId());
    auto ret = ConsumerSurface::Init();
    BLOGI("surfaceimage init");
    if (ret != SURFACE_ERROR_OK) {
        BLOGE("init surfaceimage failed");
    }
    surfaceImageName_ = name;
}

SurfaceError SurfaceImage::SetDefaultSize(int32_t width, int32_t height)
{
    return ConsumerSurface::SetDefaultWidthAndHeight(width, height);
}

void SurfaceImage::UpdateSurfaceInfo(uint32_t seqNum, sptr<SurfaceBuffer> buffer, const sptr<SyncFence> &acquireFence,
                                     int64_t timestamp, Rect damage)
{
    // release old buffer
    int releaseFence = -1;
    if (imageCacheSeqs_.count(currentSurfaceImage_) > 0 &&
        imageCacheSeqs_.at(currentSurfaceImage_).eglSync_ != EGL_NO_SYNC_KHR) {
        // PLANNING: use eglDupNativeFenceFDOHOS in the future.
        releaseFence = eglDupNativeFenceFDANDROID(eglDisplay_, imageCacheSeqs_.at(currentSurfaceImage_).eglSync_);
    }
    // There is no need to close this fd, because in function ReleaseBuffer it will be closed.
    ReleaseBuffer(currentSurfaceBuffer_, releaseFence);

    currentSurfaceImage_ = seqNum;
    currentSurfaceBuffer_ = buffer;
    currentTimeStamp_ = timestamp;
    currentCrop_ = damage;
    currentTransformType_ = ConsumerSurface::GetTransform();
    auto utils = SurfaceUtils::GetInstance();
    utils->ComputeTransformMatrix(currentTransformMatrix_, currentSurfaceBuffer_, currentTransformType_, currentCrop_);

    // wait on this acquireFence.
    if (acquireFence != nullptr) {
        acquireFence->Wait(-1);
    }
}

SurfaceError SurfaceImage::UpdateSurfaceImage()
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);

    // validate egl state
    SurfaceError ret = ValidateEglState();
    if (ret != SURFACE_ERROR_OK) {
        return ret;
    }

    // acquire buffer
    sptr<SurfaceBuffer> buffer = nullptr;
    sptr<SyncFence> acquireFence = SyncFence::INVALID_FENCE;
    int64_t timestamp;
    Rect damage;
    ret = AcquireBuffer(buffer, acquireFence, timestamp, damage);
    if (ret != SURFACE_ERROR_OK) {
        if (ret == SURFACE_ERROR_NO_BUFFER) {
            BLOGE("AcquireBuffer no buffer");
        } else {
            BLOGE("AcquireBuffer failed");
        }
        return ret;
    }

    ret = UpdateEGLImageAndTexture(eglDisplay_, buffer);
    if (ret != SURFACE_ERROR_OK) {
        ReleaseBuffer(buffer, -1);
        return ret;
    }

    uint32_t seqNum = buffer->GetSeqNum();
    UpdateSurfaceInfo(seqNum, buffer, acquireFence, timestamp, damage);
    return SURFACE_ERROR_OK;
}

SurfaceError SurfaceImage::AttachContext(uint32_t textureId)
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);
    // validate egl state
    SurfaceError ret = ValidateEglState();
    if (ret != SURFACE_ERROR_OK) {
        return ret;
    }

    textureId_ = textureId;
    if (imageCacheSeqs_.count(currentSurfaceImage_) > 0) {
        const auto &image = imageCacheSeqs_.at(currentSurfaceImage_).eglImage_;
        glBindTexture(textureTarget_, textureId);
        glEGLImageTargetTexture2DOES(textureTarget_, static_cast<GLeglImageOES>(image));
        if (glGetError() != GL_NO_ERROR) {
            return SURFACE_ERROR_API_FAILED;
        }
    }

    // If there is no EGLImage, we cannot simply return an error.
    // Developers can call OH_NativeImage_UpdateSurfaceImage later to achieve their purpose.
    return SURFACE_ERROR_OK;
}

SurfaceError SurfaceImage::DetachContext()
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);
    // validate egl state
    SurfaceError ret = ValidateEglState();
    if (ret != SURFACE_ERROR_OK) {
        return ret;
    }

    textureId_ = 0;
    glBindTexture(textureTarget_, 0);
    return SURFACE_ERROR_OK;
}

int64_t SurfaceImage::GetTimeStamp()
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);
    return currentTimeStamp_;
}

SurfaceError SurfaceImage::GetTransformMatrix(float matrix[16])
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);
    auto ret = memcpy_s(matrix, sizeof(currentTransformMatrix_),
                        currentTransformMatrix_, sizeof(currentTransformMatrix_));
    if (ret != EOK) {
        BLOGE("GetTransformMatrix: currentTransformMatrix_ memcpy_s failed");
        return SURFACE_ERROR_ERROR;
    }
    return SURFACE_ERROR_OK;
}

SurfaceError SurfaceImage::ValidateEglState()
{
    EGLDisplay disp = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();

    if ((eglDisplay_ != disp && eglDisplay_ != EGL_NO_DISPLAY) || (disp == EGL_NO_DISPLAY)) {
        BLOGE("EGLDisplay is invalid, errno : 0x%{public}x", eglGetError());
        return SURFACE_ERROR_INIT;
    }
    if ((eglContext_ != context && eglContext_ != EGL_NO_CONTEXT) || (context == EGL_NO_CONTEXT)) {
        BLOGE("EGLContext is invalid, errno : 0x%{public}x", eglGetError());
        return SURFACE_ERROR_INIT;
    }

    eglDisplay_ = disp;
    eglContext_ = context;
    return SURFACE_ERROR_OK;
}

EGLImageKHR SurfaceImage::CreateEGLImage(EGLDisplay disp, const sptr<SurfaceBuffer>& buffer)
{
    sptr<SurfaceBuffer> bufferImpl = buffer;
    NativeWindowBuffer* nBuffer = CreateNativeWindowBufferFromSurfaceBuffer(&bufferImpl);
    EGLint attrs[] = {
        EGL_IMAGE_PRESERVED,
        EGL_TRUE,
        EGL_NONE,
    };

    EGLImageKHR img = eglCreateImageKHR(disp, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_OHOS, nBuffer, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        BLOGE("failed, error %{public}d", error);
        eglTerminate(disp);
    }
    DestroyNativeWindowBuffer(nBuffer);
    return img;
}

SurfaceError SurfaceImage::UpdateEGLImageAndTexture(EGLDisplay disp, const sptr<SurfaceBuffer>& buffer)
{
    // private function, buffer is always valid.
    uint32_t seqNum = buffer->GetSeqNum();
    // If there was no eglImage binding to this buffer, we create a new one.
    if (imageCacheSeqs_.count(seqNum) == 0) {
        EGLImageKHR eglImage = CreateEGLImage(eglDisplay_, buffer);
        if (eglImage == EGL_NO_IMAGE_KHR) {
            return SURFACE_ERROR_INIT;
        }
        imageCacheSeqs_[seqNum].eglImage_ = eglImage;
    }

    const auto &image = imageCacheSeqs_.at(seqNum).eglImage_;
    glBindTexture(textureTarget_, textureId_);
    glEGLImageTargetTexture2DOES(textureTarget_, static_cast<GLeglImageOES>(image));
    if (glGetError() != GL_NO_ERROR) {
        return SURFACE_ERROR_API_FAILED;
    }

    auto sync = imageCacheSeqs_.at(seqNum).eglSync_;
    if (sync != EGL_NO_SYNC_KHR) {
        eglDestroySyncKHR(disp, sync);
    }
    sync = eglCreateSyncKHR(disp, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    imageCacheSeqs_.at(seqNum).eglSync_ = sync;
    return SURFACE_ERROR_OK;
}

SurfaceError SurfaceImage::SetOnBufferAvailableListener(void *context, OnBufferAvailableListener listener)
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);
    if (listener == nullptr) {
        BLOGE("listener is nullptr");
        return SURFACE_ERROR_ERROR;
    }

    listener_ = listener;
    context_ = context;
    return SURFACE_ERROR_OK;
}

SurfaceError SurfaceImage::UnsetOnBufferAvailableListener()
{
    std::lock_guard<std::mutex> lockGuard(opMutex_);
    listener_ = nullptr;
    context_ = nullptr;
    return SURFACE_ERROR_OK;
}

SurfaceImageListener::~SurfaceImageListener()
{
    BLOGE("~SurfaceImageListener");
    surfaceImage_ = nullptr;
}

void SurfaceImageListener::OnBufferAvailable()
{
    BLOGD("SurfaceImageListener::OnBufferAvailable");
    auto surfaceImage = surfaceImage_.promote();
    if (surfaceImage == nullptr) {
        BLOGE("surfaceImage promote failed");
        return;
    }

    // check here maybe a messagequeue, flag instead now
    surfaceImage->OnUpdateBufferAvailableState(true);
    if (surfaceImage->listener_ != nullptr) {
        surfaceImage->listener_(surfaceImage->context_);
    }
}
} // namespace OHOS
