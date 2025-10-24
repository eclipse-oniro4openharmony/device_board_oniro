#ifndef NATIVE_STATE_OHOS_H_
#define NATIVE_STATE_OHOS_H_
#include "native-state.h"
#include "ohos_wrapper_linker.h"

class NativeStateOhos : public NativeState
{

    /* Initializes the native display */
    bool init_display();

    /* Gets the native display */
    void* display();

    /* Creates (or recreates) the native window */
    bool create_window(WindowProperties const& properties);

    /* 
     * Gets the native window and its properties.
     * The dimensions may be different than the ones requested.
     */
    void* window(WindowProperties& properties);

    /* Sets the visibility of the native window */
    void visible(bool v);

    /* Whether the user has requested an exit */
    bool should_quit();

    /* Flips the display */
    void flip();
private:
    OhosWrapperLinker wrapper;
    WindowProperties properties_;
};
#endif // NATIVE_STATE_OHOS_H_