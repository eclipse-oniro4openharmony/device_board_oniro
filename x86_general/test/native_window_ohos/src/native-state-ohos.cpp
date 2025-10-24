#include "native-state-ohos.h"
#include "log.h"

/* Initializes the native display */
bool NativeStateOhos::init_display()
{
    //Log::debug("%s@%s:%d", __FUNCTION__, __FILE__, __LINE__);
    return wrapper.Init();
}

/* Gets the native display */
void *NativeStateOhos::display()
{
    //Log::debug("%s@%s:%d", __FUNCTION__, __FILE__, __LINE__);
    return nullptr;
}

/* Creates (or recreates) the native window */
bool NativeStateOhos::create_window(WindowProperties const &properties)
{
    properties_ = properties;
    return wrapper.CreateWindow(properties.width, properties.height);
}

/*
 * Gets the native window and its properties.
 * The dimensions may be different than the ones requested.
 */
void *NativeStateOhos::window(WindowProperties &properties)
{
    properties = properties_;
    return wrapper.GetWindow();
}

/* Sets the visibility of the native window */
void NativeStateOhos::visible(bool v)
{
    wrapper.SetVisibility(v);
    return;
}

/* Whether the user has requested an exit */
bool NativeStateOhos::should_quit()
{
    return false;
}

/* Flips the display */
void NativeStateOhos::flip()
{
    return;
}