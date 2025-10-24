#ifndef NATIVE_WINDOW_WRAPPER
#define NATIVE_WINDOW_WRAPPER
#include <cstdint>

extern "C" {
    typedef struct {
        void* (*CreateWindowWrapper)();
        bool (*CreateWindow)(void* wrapper, uint32_t w, uint32_t h);
        void* (*GetNativeWindow)(void* wrapper);
        void (*SetVisibility)(void* wrapper, bool visible);
        void (*DestroyWindowWrapper)(void* wrapper);
    } WrapperFunc;
    bool GetWrapperFunc(WrapperFunc* funcs);    
}

#endif // NATIVE_WINDOW_WRAPPER