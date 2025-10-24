#include <cstdint>
extern "C" {
    typedef struct {
        void* (*CreateWindowWrapper)();
        bool (*CreateWindow)(void* wrapper, uint32_t w, uint32_t h);
        void* (*GetNativeWindow)(void* wrapper);
        void (*SetVisibility)(void* wrapper, bool visible);
        void (*DestroyWindowWrapper)(void* wrapper);
    } WrapperFunc; 
}

class OhosWrapperLinker
{
public:
    bool Init();
    bool CreateWindow(uint32_t w, uint32_t h);
    void *GetWindow();
    void SetVisibility(bool visible);

private:
    static constexpr const char *WRAPPER_LIB_NAME = "libnative_window_wrapper.z.so";
    static constexpr const char *WRAPPER_FUNC_GET = "GetWrapperFunc";
    WrapperFunc wapperFuncs_;
    void* wrapper_ = nullptr;
    void *wrapperModule_ = nullptr;
};