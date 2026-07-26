#ifndef BROWSER_MANAGER_H
#define BROWSER_MANAGER_H

#include <stdbool.h>

#include "types.h"

struct Mod;
struct ModFile;
struct Object;

#define BROWSER_ID_NONE 0

struct BrowserCreateOptions {
    bool transparent;
    bool audio;
    int onLoad;
    int onError;
    int onConsole;
    int onMessage;
    int onPaint;
};

struct BrowserHudOptions {
    bool hasFilter;
    bool filter;
    bool hasRotation;
    f32 rotation;
    f32 pivotX;
    f32 pivotY;
};

struct BrowserRenderWorldOptions {
    f32 width;
    f32 height;
    Vec3f offset;
    Vec3f rotation;
    s16 layer;
    bool depth;
    bool twoSided;
    bool emissive;
    bool filter;
    bool hidden;
};

void browser_manager_init(void);
void browser_manager_tick(void);
void browser_manager_on_lua_shutdown(void);
void browser_manager_shutdown(void);

bool browser_manager_available(void);

s32 browser_manager_create(u32 width, u32 height, const struct BrowserCreateOptions *opts, struct Mod *ownerMod, struct ModFile *ownerFile);
bool browser_manager_destroy(s32 id, struct Mod *ownerMod);
void browser_manager_destroy_all_for_mod(struct Mod *ownerMod);
void browser_manager_destroy_all(void);

bool browser_manager_open_url(s32 id, struct Mod *ownerMod, const char *url);
bool browser_manager_set_html(s32 id, struct Mod *ownerMod, const char *html, const char *baseUrl);
bool browser_manager_reload(s32 id, struct Mod *ownerMod);
bool browser_manager_stop(s32 id, struct Mod *ownerMod);
bool browser_manager_go_back(s32 id, struct Mod *ownerMod);
bool browser_manager_go_forward(s32 id, struct Mod *ownerMod);
bool browser_manager_run_js(s32 id, struct Mod *ownerMod, const char *code, int callbackRef);
bool browser_manager_add_function(s32 id, struct Mod *ownerMod, const char *objectName, const char *functionName, int callbackRef);
bool browser_manager_set_volume(s32 id, struct Mod *ownerMod, f32 volume);
bool browser_manager_set_muted(s32 id, struct Mod *ownerMod, bool muted);
bool browser_manager_resize(s32 id, struct Mod *ownerMod, u32 width, u32 height);

void browser_manager_render_hud(s32 id, struct Mod *ownerMod, f32 x, f32 y, f32 width, f32 height, const struct BrowserHudOptions *opts);
bool browser_manager_render_world(s32 id, struct Mod *ownerMod, const struct BrowserRenderWorldOptions *opts);
bool browser_manager_attach_object(struct Object *obj, s32 id, struct Mod *ownerMod, const struct BrowserRenderWorldOptions *opts);
void browser_manager_detach_object(struct Object *obj, s32 id, struct Mod *ownerMod);
void browser_manager_detach_object_all(struct Object *obj);
void browser_manager_render_object(struct Object *obj);
bool browser_manager_pick_object(struct Object *obj, s32 id, struct Mod *ownerMod, f32 screenX, f32 screenY, s32 *browserX, s32 *browserY);

bool browser_manager_send_mouse_move(s32 id, struct Mod *ownerMod, s32 x, s32 y, u32 modifiers);
bool browser_manager_send_mouse_button(s32 id, struct Mod *ownerMod, s32 x, s32 y, s32 button, bool down, s32 clickCount, u32 modifiers);
bool browser_manager_send_mouse_wheel(s32 id, struct Mod *ownerMod, s32 x, s32 y, s32 deltaX, s32 deltaY, u32 modifiers);
bool browser_manager_send_key(s32 id, struct Mod *ownerMod, s32 eventType, s32 keyCode, s32 nativeCode, u32 modifiers, const char *text);

#endif
