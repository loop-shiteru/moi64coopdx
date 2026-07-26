#if !defined(ENABLE_BROWSER) || !ENABLE_BROWSER || (!defined(_WIN32) && !defined(__linux__))

#include "browser_backend.h"

struct BrowserBackendBrowser {
    s32 unused;
};

bool browser_backend_init(void) {
    return true;
}

void browser_backend_tick(void) {
}

void browser_backend_shutdown(void) {
}

bool browser_backend_available(void) {
    return false;
}

struct BrowserBackendBrowser *browser_backend_create(UNUSED struct BrowserBackendSurface *surface, UNUSED const struct BrowserBackendCreateParams *params) {
    return NULL;
}

void browser_backend_destroy(UNUSED struct BrowserBackendBrowser *browser) {
}

bool browser_backend_open_url(UNUSED struct BrowserBackendBrowser *browser, UNUSED const char *url) {
    return false;
}

bool browser_backend_reload(UNUSED struct BrowserBackendBrowser *browser) {
    return false;
}

bool browser_backend_stop(UNUSED struct BrowserBackendBrowser *browser) {
    return false;
}

bool browser_backend_go_back(UNUSED struct BrowserBackendBrowser *browser) {
    return false;
}

bool browser_backend_go_forward(UNUSED struct BrowserBackendBrowser *browser) {
    return false;
}

bool browser_backend_run_js(UNUSED struct BrowserBackendBrowser *browser, UNUSED const char *code, UNUSED s32 requestId) {
    return false;
}

bool browser_backend_add_function(UNUSED struct BrowserBackendBrowser *browser, UNUSED s32 bindingId, UNUSED const char *objectName, UNUSED const char *functionName) {
    return false;
}

bool browser_backend_set_volume(UNUSED struct BrowserBackendBrowser *browser, UNUSED f32 volume) {
    return false;
}

bool browser_backend_set_muted(UNUSED struct BrowserBackendBrowser *browser, UNUSED bool muted) {
    return false;
}

bool browser_backend_resize(UNUSED struct BrowserBackendBrowser *browser, UNUSED struct BrowserBackendSurface *surface) {
    return false;
}

bool browser_backend_send_mouse_move(UNUSED struct BrowserBackendBrowser *browser, UNUSED s32 x, UNUSED s32 y, UNUSED u32 modifiers) {
    return false;
}

bool browser_backend_send_mouse_button(UNUSED struct BrowserBackendBrowser *browser, UNUSED s32 x, UNUSED s32 y, UNUSED s32 button, UNUSED bool down, UNUSED s32 clickCount, UNUSED u32 modifiers) {
    return false;
}

bool browser_backend_send_mouse_wheel(UNUSED struct BrowserBackendBrowser *browser, UNUSED s32 x, UNUSED s32 y, UNUSED s32 deltaX, UNUSED s32 deltaY, UNUSED u32 modifiers) {
    return false;
}

bool browser_backend_send_key(UNUSED struct BrowserBackendBrowser *browser, UNUSED s32 eventType, UNUSED s32 keyCode, UNUSED s32 nativeCode, UNUSED u32 modifiers, UNUSED const char *text) {
    return false;
}

#endif
