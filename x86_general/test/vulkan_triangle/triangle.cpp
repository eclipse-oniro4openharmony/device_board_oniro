/*
* Vulkan Example - Basic indexed triangle rendering
*
* Note:
*	This is a "pedal to the metal" example to show off how to get Vulkan up and displaying something
*	Contrary to the other examples, this one won't make use of helper functions or initializers
*	Except in a few cases (swap chain setup e.g.)
*
* Copyright (C) 2016-2017 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <iostream>
#include <string>
#include <vector>
#include <exception>

#include <vulkan/vulkan.h>
#include "surface.h"
#include "refbase.h"
#include "render_context/render_context.h"
#include "transaction/rs_transaction.h"
#include "ui/rs_surface_extractor.h"
#include "ui/rs_surface_node.h"
#include "wm/window.h"
#include "window.h"
#include "vulkanexamplebase.h"
#include "vulkanexample.h"
#include "VulkanUtils.h"


VulkanExample *vulkanExample;

constexpr int DEFAULT_DISPLAY_ID = 0;
constexpr int WINDOW_LEFT = 100;
constexpr int WINDOW_TOP = 200;
constexpr int WINDOW_WIDTH = 360;
constexpr int WINDOW_HEIGHT = 360;
int main(const int argc, const char *argv[])
{

    OHOS::Rosen::RSSurfaceNodeConfig surfaceNodeConfig = {
        .SurfaceNodeName = "vulkan_triangle_demo",
        .additionalData = nullptr,
        .isTextureExportNode = false,
        .surfaceId = 0
    };
    OHOS::Rosen::RSSurfaceNodeType surfaceNodeType = OHOS::Rosen::RSSurfaceNodeType::SELF_DRAWING_WINDOW_NODE;
    auto surfaceNode = OHOS::Rosen::RSSurfaceNode::Create(surfaceNodeConfig, surfaceNodeType);
    if (!surfaceNode) {
        std::cout << "triangle :: rsSurfaceNode_ is nullptr" << std::endl;
        return -1;
    }

    surfaceNode->SetFrameGravity(OHOS::Rosen::Gravity::RESIZE_ASPECT_FILL);
    surfaceNode->SetPositionZ(OHOS::Rosen::RSSurfaceNode::POINTER_WINDOW_POSITION_Z);
    surfaceNode->SetBounds(WINDOW_LEFT, WINDOW_TOP, WINDOW_WIDTH, WINDOW_HEIGHT);
    surfaceNode->AttachToDisplay(DEFAULT_DISPLAY_ID);
    OHOS::Rosen::RSTransaction::FlushImplicitTransaction();

    OHOS::sptr<OHOS::Surface> surf = surfaceNode->GetSurface();
    OHNativeWindow* nativeWindow = CreateNativeWindowFromSurface(&surf);
    NativeWindowHandleOpt(nativeWindow, SET_BUFFER_GEOMETRY, WINDOW_WIDTH, WINDOW_HEIGHT);
    if (nativeWindow == nullptr) {
        std::cout << "CreateNativeWindowFromSurface Failed" << std::endl;
        return -1;
    }

    vulkanExample = new VulkanExample();
    vulkanExample->initVulkan();
    vulkanExample->setupWindow(nativeWindow);
    vulkanExample->prepare();
    vulkanExample->renderLoop();

    surfaceNode->DetachToDisplay(DEFAULT_DISPLAY_ID);
    OHOS::Rosen::RSTransaction::FlushImplicitTransaction();


    delete(vulkanExample);
    return 0;
}

