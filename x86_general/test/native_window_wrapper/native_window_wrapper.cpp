#include "native_window_wrapper.h"
#include "wm_common.h"
#include "window_option.h"
#include "window.h"
#include "surface.h"
#include "ui/rs_surface_node.h"

using namespace OHOS;

class NativeWindowWrapper {
public:
    virtual bool CreateWindow(uint32_t w,uint32_t h);
    
    virtual void* GetWindow()
    {
        return nativeWindow_;
    }
    void SetVisibility(bool visible);
private:
    void* nativeWindow_ = nullptr;
    sptr<Rosen::Window> previewWindow_ = nullptr;
};

extern "C" {
struct NativeWindow* CreateNativeWindowFromSurface(void* pSuface);
int32_t NativeWindowHandleOpt(struct NativeWindow *window, int code, ...);

void* CreateWindowWrapper()
{
    void* wrapper = new NativeWindowWrapper();
    return wrapper;
}

bool CreateWindow(void* wrapper, uint32_t w, uint32_t h)
{
    NativeWindowWrapper* wrapperObj = static_cast<NativeWindowWrapper*>(wrapper);
    return wrapperObj->CreateWindow(w, h);
}

void* GetNativeWindow(void* wrapper)
{
    NativeWindowWrapper* wrapperObj = static_cast<NativeWindowWrapper*>(wrapper);
    return wrapperObj->GetWindow();
}

void SetVisibility(void* wrapper,bool visible)
{
    NativeWindowWrapper* wrapperObj = static_cast<NativeWindowWrapper*>(wrapper);
    return wrapperObj->SetVisibility(visible);
}

void DestroyWindowWrapper(void* wrapper)
{
    if (wrapper != nullptr) {
        delete wrapper;
    }
}

bool GetWrapperFunc(WrapperFunc* funcs)
{
    if(funcs != nullptr) {
        funcs->CreateWindow = CreateWindow;
        funcs->CreateWindowWrapper = CreateWindowWrapper;
        funcs->GetNativeWindow = GetNativeWindow;
        funcs->SetVisibility = SetVisibility;
        funcs->DestroyWindowWrapper = DestroyWindowWrapper;
        return true;
    }
    return false;
}
}

bool NativeWindowWrapper::CreateWindow(uint32_t w, uint32_t h)
{
    sptr<Rosen::WindowOption> option = new Rosen::WindowOption();
    option->SetWindowRect({0, 0, w, h});
    option->SetWindowType(Rosen::WindowType::WINDOW_TYPE_APP_LAUNCHING);
    option->SetWindowMode(Rosen::WindowMode::WINDOW_MODE_FLOATING);
    previewWindow_ = Rosen::Window::Create("native_window", option);
    if (previewWindow_ == nullptr || previewWindow_->GetSurfaceNode() == nullptr) {
        printf("previewWindow_ is nullptr\n");
        return false;
    }

    previewWindow_->Show();
    auto surface = previewWindow_->GetSurfaceNode()->GetSurface();
    nativeWindow_ = CreateNativeWindowFromSurface(&surface);
    NativeWindowHandleOpt(static_cast<struct NativeWindow *>(nativeWindow_), SET_BUFFER_GEOMETRY, w, h);

    printf("CreateWindow nativeWindow_ %p  w: %d, h: %d \n", nativeWindow_, w, h);
    return true;
}

void NativeWindowWrapper::SetVisibility(bool visible)
{
    printf("SetVisibility %s\n", visible ? "true" : "false");
}