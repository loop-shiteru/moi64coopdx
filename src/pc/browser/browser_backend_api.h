#ifndef BROWSER_BACKEND_API_H
#define BROWSER_BACKEND_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BROWSER_BACKEND_API_VERSION 4u
#define BROWSER_BACKEND_GET_API_NAME "browser_backend_get_api"

struct BrowserBackendApiSurface {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t surfaceWidth;
    uint32_t surfaceHeight;
};

struct BrowserBackendApiCreateParams {
    int32_t browserId;
    int32_t transparent;
    int32_t audio;
};

struct BrowserBackendApiCallbacks {
    void (*notifyLoad)(int32_t browserId, const char *url);
    void (*notifyError)(int32_t browserId, const char *message, const char *details);
    void (*notifyConsole)(int32_t browserId, const char *message, int32_t level);
    void (*notifyMessage)(int32_t browserId, const char *message);
    void (*notifyFunctionCall)(int32_t browserId, int32_t bindingId, const char *argsPayload);
    void (*notifyPaint)(int32_t browserId, int32_t width, int32_t height);
    void (*notifyJsResult)(int32_t browserId, int32_t requestId, int32_t success, const char *result, const char *errorMessage);
    int32_t (*resolveUrlToPath)(int32_t browserId, const char *url, char *path, uint32_t pathCapacity);
    int32_t (*shouldLoadUrl)(int32_t browserId, const char *url);
};

struct BrowserBackendApi {
    uint32_t apiVersion;
    int32_t (*init)(const struct BrowserBackendApiCallbacks *callbacks);
    void (*tick)(void);
    void (*shutdown)(void);
    int32_t (*available)(void);

    void *(*create)(struct BrowserBackendApiSurface *surface, const struct BrowserBackendApiCreateParams *params);
    void (*destroy)(void *browser);
    int32_t (*openUrl)(void *browser, const char *url);
    int32_t (*reload)(void *browser);
    int32_t (*stop)(void *browser);
    int32_t (*goBack)(void *browser);
    int32_t (*goForward)(void *browser);
    int32_t (*runJs)(void *browser, const char *code, int32_t requestId);
    int32_t (*addFunction)(void *browser, int32_t bindingId, const char *objectName, const char *functionName);
    int32_t (*setVolume)(void *browser, float volume);
    int32_t (*setMuted)(void *browser, int32_t muted);
    int32_t (*resize)(void *browser, struct BrowserBackendApiSurface *surface);
    int32_t (*sendMouseMove)(void *browser, int32_t x, int32_t y, uint32_t modifiers);
    int32_t (*sendMouseButton)(void *browser, int32_t x, int32_t y, int32_t button, int32_t down, int32_t clickCount, uint32_t modifiers);
    int32_t (*sendMouseWheel)(void *browser, int32_t x, int32_t y, int32_t deltaX, int32_t deltaY, uint32_t modifiers);
    int32_t (*sendKey)(void *browser, int32_t eventType, int32_t keyCode, int32_t nativeCode, uint32_t modifiers, const char *text);
};

typedef const struct BrowserBackendApi *(*BrowserBackendGetApiFn)(uint32_t apiVersion);

#ifdef __cplusplus
}
#endif

#endif
