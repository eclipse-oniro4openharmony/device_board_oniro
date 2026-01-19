# OpenHarmony Graphics Stack Architecture

The OpenHarmony graphics stack, internally known as **Rosen**, is a modern, high-performance rendering subsystem designed to support smooth user interfaces and efficient graphical operations across a wide range of devices. Unlike legacy systems that might rely on X11 or direct framebuffer access, OpenHarmony employs a dedicated compositing service and a scene-graph-based rendering model.

This document provides a technical overview of the stack, intended for developers familiar with Linux or Android graphics architectures.

## High-Level Architecture

The graphics stack operates on a Client-Server model:

1.  **Application Layer (Client)**: Applications (using ArkUI or Native C++) construct a UI scene graph using **RSNodes**.
2.  **Interface Layer (IPC)**: State changes in the scene graph are packaged into **Transactions** and sent to the Render Service via IPC.
3.  **Render Service (Server)**: The `render_service` process receives transactions, reconstructs the scene graph, and executes the rendering pipeline (UniRender).
4.  **Display & Memory Abstraction**: The composed frames are sent to the display hardware via the **HDI** (Hardware Device Interface).

```mermaid
graph TD
    App[Application / ArkUI] -->|RSClient| RSC[Render Service Client]
    RSC -->|IPC (Transaction)| RS[Render Service]
    RS -->|UniRender| GPU[GPU / CPU Renderer]
    RS -->|Buffer Queue| Surface[Surface / Shared Mem]
    Surface -->|HDI| Composer[Display Composer HDI]
    Composer -->|DRM/KMS| Screen[Physical Display]
```

## The Rosen Rendering Subsystem

The core of the graphics stack is the **Rosen** subsystem, located in `foundation/graphic/graphic_2d/rosen`.

### Render Service (RS)
The **Render Service** acts as the system-wide compositor and renderer. It runs as a standalone process (typically `render_service`). Its main responsibilities include:
*   **Scene Graph Management**: Maintaining a shadow copy of the scene graph for every application.
*   **Composition**: merging layers from different applications (System UI, App Windows, Status Bar).
*   **Rendering**: Executing drawing commands to generate the final display output.
*   **VSync Generation**: Managing the vertical synchronization signals via `VSyncController` and `VSyncDistributor`.

### Render Service Client (RSC)
The **Render Service Client** (`render_service_client`) is a library linked into applications. It provides the APIs to manipulate the scene graph. `ArkUI` (the UI framework) and `XComponent` (for raw OpenGL/EGL access) use RSC internals.

### RSNode: The Scene Graph
The fundamental unit of the graphics scene is the **RSNode**.
*   **Tree Structure**: RSNodes are organized in a parent-child hierarchy.
*   **Properties**: Each RSNode holds properties such as Bounds (geometry), Frame (position), Transform (matrix), Alpha, and Clip.
*   **Types**:
    *   `RSCanvasNode`: For custom drawing (e.g., via Skia/Drawing canvas).
    *   `RSSurfaceNode`: Represents a standalone buffer source (like a window or a video stream).
    *   `RSRootNode`: The root of a specific application's tree.

### UniRender
**UniRender** is the modern rendering pipeline used in OpenHarmony.
1.  **Draw**: The application records draw commands into an `RSRecordingCanvas`.
2.  **Submit**: Commands and property updates are batched into a Transaction.
3.  **Flush**: The Transaction is sent to the Render Service.
4.  **Execute**: The Render Service replays the commands and composites the nodes into a single framebuffer using the GPU (via Skia/OpenGL) or CPU.

## Surface and Buffer Management

OpenHarmony uses a producer-consumer model for graphical buffers, similar to Android's BufferQueue.

*   **Surface**: The abstraction representing a queue of buffers.
*   **Shared Memory**: Buffers are allocated in shared memory (often using DMA-BUF mechanisms) to allow zero-copy transfer between the App (Producer) and the Render Service (Consumer).
*   **Sync Fences**: Synchronization between CPU and GPU operations is managed via fences to prevent tearing and race conditions.

Headers for surface management can be found in `foundation/graphic/graphic_surface`.

## Hardware Abstraction (HDI)

The connection to hardware is mediated by the **Hardware Device Interface (HDI)**, which is the OpenHarmony equivalent of a HAL.

### Display HDI
Located typically under `drivers/peripheral/display`, the Display HDI handles:
*   **Layer Composition**: Hardware-accelerated composition (if supported by the text display controller).
*   **Modesetting**: Setting resolution, refresh rate, and power states.
*   **Hotplug**: Detecting screen connections.
*   **Interaction**: Often maps to standard Linux `DRM/KMS` APIs on Linux-based kernels (like Oniro).

### Memory HDI (Gralloc)
Handles the allocation of graphics memory (buffers) with specific usage flags (e.g., passable to WebGL, video decoders, or display controllers).

## Developing GL Drivers for Mobile Hardware

For existing mobile hardware (ARM-based SoCs with Mali, Adreno, PowerVR, etc.), integrating the GPU driver requires understanding the unique **Wrapper Mechanism** of OpenHarmony. Unlike standard Linux distributions where applications link directly against the vendor's `libEGL.so`, OpenHarmony uses a dispatch layer.

### 1. The OpenGL Wrapper Mechanism

The system libraries `libEGL.so` and `libGLESv*.so` components of the **Rosen** subsystem (specifically the `opengl_wrapper`). These are **NOT** the vendor drivers. They act as shims that load the actual implementation at runtime.

### 2. Integration Steps

To enable hardware acceleration:

1.  **Place Vendor Libraries**: You must install the vendor's proprietary blobs or Mesa-built libraries into the device's library path (typically `/vendor/lib/chipsetsdk` or `/vendor/lib64/chipsetsdk`).
2.  **Rename/Symlink**: The wrapper expects specific filenames. You **must** rename or symlink your vendor drivers to:
    *   **EGL**: `libEGL_impl.so`
    *   **GLESv1**: `libGLESv1_impl.so` (if supported)
    *   **GLESv2**: `libGLESv2_impl.so`
    *   **GLESv3**: `libGLESv3_impl.so` (if supported)
    
    *Note: If building with Mesa and `OPENGL_WRAPPER_ENABLE_GL4` is set, the system may also look for `libEGL_mesa.so`.*

### 3. Memory Management (Gralloc) Integration

A critical part of the driver development is the **Memory HDI** (Gralloc). The Render Service uses OpenGL to draw into buffers, which are then passed to the Display HDI for scanout.

*   **DMA-BUF**: The Gralloc implementation must allocate `DMA-BUF` file descriptors that are importable by checking the **GPU Driver** (for rendering) and the **Display Controller** (for scanout).
*   **GBM Compatibility**: If using Mesa/Gallium drivers (e.g., Panfrost, Freedreno), utilizing a `gbm_gralloc` based solution is often the easiest path, as it aligns with the standard Linux graphics stack.

### 4. Display vs. Rendering

*   **Display HDI**: Wraps Linux **DRM/KMS**. It is responsible for Modesetting and interacting with the CRTC/Connector (Physical Screen). It does *not* do 3D rendering.
*   **Render Service**: Uses the **GL Driver** (via the wrapper) to composite layers.
*   **Flow**: `Render Service` -> `GL Driver` -> `Gralloc Buffer` -> `Display HDI` -> `KMS`.

## Obtaining and Building Drivers

### Proprietary Vendor Blobs
Obtaining drivers for mobile SoCs (Mali, Adreno) can be challenging.
*   **Incompatibility Notice**: Drivers extracted directly from Android images **will not work**. Android uses the **Bionic** libc, while OpenHarmony uses **Musl**. Binary blobs must be linked against Musl or use a (complex) compatibility shim (e.g. Libhybris).
*   **Source**: You must obtain OpenHarmony-compatible (Musl-linked) binaries from the chipset vendor's Board Support Package (BSP) or build them from source if available.

### Mesa 3D (Open Source)
OpenHarmony maintains a fork of Mesa 3D at `//third_party/mesa3d`. This is often the best route for devices supported by mainline Linux (e.g., Raspberry Pi 4, Rockchip with Panfrost, Intel/AMD).

#### Architecture & Build
The `third_party/mesa3d` component is integrated into the OpenHarmony build system (GN) but internally wraps the standard **Meson** build system.
1.  **GN Wrapper**: The `BUILD.gn` file defines an action that invokes a Python script (`ohos/build_ohos64.py`).
2.  **Meson Cross-Compilation**: This script configures Meson with a cross-compilation file suitable for the target architecture (ARM64, etc.) and OpenHarmony's Musl environment.
3.  **Outputs**: The build produces standard shared objects like `libEGL_mesa.so` and `libgallium.so`, which the wrapper layer loads as described above.

## Key Source Locations

For those wishing to inspect the code:

*   **Render Service Core**: `//foundation/graphic/graphic_2d/rosen/modules/render_service_base`
*   **Render Service Client**: `//foundation/graphic/graphic_2d/rosen/modules/render_service_client`
*   **Surface implementation**: `//foundation/graphic/graphic_surface`
*   **Display Managers**: `//foundation/window/window_manager`

## Summary for Porting

When porting OpenHarmony (e.g., to Oniro), the critical integration points are:
1.  **Display HDI Implementation**: Ensuring `drivers/peripheral/display` correctly wraps the underlying kernel DRM/KMS drivers.
2.  **Buffer Allocation**: Ensuring the Memory HDI can allocate DMA-BUF handles compatible with the GPU and Display Controller.
3.  **VSync Integration**: Properly routing hardware VSync interrupts to the `VSyncGenerator` in the graphics stack.