#ifndef BROWSER_BACKEND_H
#define BROWSER_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

struct BrowserBackendBrowser;

struct BrowserBackendSurface {
    Texture *pixels;
    u32 width;
    u32 height;
    u32 surfaceWidth;
    u32 surfaceHeight;
};

struct BrowserBackendCreateParams {
    s32 browserId;
    bool transparent;
    bool audio;
};

bool browser_backend_init(void);
void browser_backend_tick(void);
void browser_backend_shutdown(void);
bool browser_backend_available(void);

struct BrowserBackendBrowser *browser_backend_create(struct BrowserBackendSurface *surface, const struct BrowserBackendCreateParams *params);
void browser_backend_destroy(struct BrowserBackendBrowser *browser);
bool browser_backend_open_url(struct BrowserBackendBrowser *browser, const char *url);
bool browser_backend_reload(struct BrowserBackendBrowser *browser);
bool browser_backend_stop(struct BrowserBackendBrowser *browser);
bool browser_backend_go_back(struct BrowserBackendBrowser *browser);
bool browser_backend_go_forward(struct BrowserBackendBrowser *browser);
bool browser_backend_run_js(struct BrowserBackendBrowser *browser, const char *code, s32 requestId);
bool browser_backend_add_function(struct BrowserBackendBrowser *browser, s32 bindingId, const char *objectName, const char *functionName);
bool browser_backend_set_volume(struct BrowserBackendBrowser *browser, f32 volume);
bool browser_backend_set_muted(struct BrowserBackendBrowser *browser, bool muted);
bool browser_backend_resize(struct BrowserBackendBrowser *browser, struct BrowserBackendSurface *surface);
bool browser_backend_send_mouse_move(struct BrowserBackendBrowser *browser, s32 x, s32 y, u32 modifiers);
bool browser_backend_send_mouse_button(struct BrowserBackendBrowser *browser, s32 x, s32 y, s32 button, bool down, s32 clickCount, u32 modifiers);
bool browser_backend_send_mouse_wheel(struct BrowserBackendBrowser *browser, s32 x, s32 y, s32 deltaX, s32 deltaY, u32 modifiers);
bool browser_backend_send_key(struct BrowserBackendBrowser *browser, s32 eventType, s32 keyCode, s32 nativeCode, u32 modifiers, const char *text);

void browser_manager_backend_notify_load(s32 browserId, const char *url);
void browser_manager_backend_notify_error(s32 browserId, const char *message, const char *details);
void browser_manager_backend_notify_console(s32 browserId, const char *message, s32 level);
void browser_manager_backend_notify_message(s32 browserId, const char *message);
void browser_manager_backend_notify_function_call(s32 browserId, s32 bindingId, const char *argsPayload);
void browser_manager_backend_notify_paint(s32 browserId, s32 width, s32 height);
void browser_manager_backend_notify_js_result(s32 browserId, s32 requestId, bool success, const char *result, const char *errorMessage);
bool browser_manager_backend_resolve_url_to_path(s32 browserId, const char *url, char *path, size_t pathCapacity);
bool browser_manager_backend_should_load_url(s32 browserId, const char *url);

#endif
