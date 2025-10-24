#include "ohos_wrapper_linker.h"
#include <dlfcn.h>
#include "log.h"

bool OhosWrapperLinker::Init()
{
    wrapperModule_ = dlopen(WRAPPER_LIB_NAME, RTLD_NOW | RTLD_NOLOAD);
    if (wrapperModule_ != nullptr) {
        Log::debug("Module '%s' already loaded \n", WRAPPER_LIB_NAME);
    } else {
        Log::debug("Loading module %s\n", WRAPPER_LIB_NAME);
        wrapperModule_ = dlopen(WRAPPER_LIB_NAME, RTLD_NOW);
        if (wrapperModule_ == nullptr) {
            Log::debug("Failed to load module: %s \n", dlerror());
            return false;
        }
    }

    using InitFunc = bool (*)(WrapperFunc *funcs);
    InitFunc func = reinterpret_cast<InitFunc>(dlsym(wrapperModule_, WRAPPER_FUNC_GET));
    if (func == nullptr) {
        Log::debug("Failed to lookup %s function: %s\n", WRAPPER_FUNC_GET, dlerror());
        dlclose(wrapperModule_);
        return false;
    }
    if (func(&wapperFuncs_)) {
        wrapper_ = wapperFuncs_.CreateWindowWrapper();
    } else {
        Log::debug("can not get wrapper functions \n");
        return false;
    }
    if (wrapper_ != nullptr) {
        Log::debug("wrapper init success\n");
        return true;
    }
    return false;
}

bool OhosWrapperLinker::CreateWindow(uint32_t w, uint32_t h)
{
    return wapperFuncs_.CreateWindow(wrapper_, w, h);
}

void* OhosWrapperLinker::GetWindow()
{
    return wapperFuncs_.GetNativeWindow(wrapper_);
}

void OhosWrapperLinker::SetVisibility(bool visible)
{
    wapperFuncs_.SetVisibility(wrapper_, visible);
}
