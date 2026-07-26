#include <stdbool.h>
#include <string.h>

#include "browser_lua.h"
#include "browser_manager.h"

#include "pc/lua/smlua.h"
#include "pc/mods/mod.h"

static struct Mod *browser_lua_get_owner_mod(void) {
    if (gLuaActiveMod != NULL) { return gLuaActiveMod; }
    if (gLuaLoadingMod != NULL) { return gLuaLoadingMod; }
    return gLuaLastHookMod;
}

static void browser_lua_release_ref(int *ref) {
    if (ref == NULL || *ref == LUA_NOREF || *ref == LUA_REFNIL) { return; }
    luaL_unref(gLuaState, LUA_REGISTRYINDEX, *ref);
    *ref = LUA_NOREF;
}

static bool browser_lua_expect_table_or_nil(lua_State *L, int index, const char *functionName, int param) {
    int type = lua_type(L, index);
    if (type == LUA_TNIL || type == LUA_TNONE || type == LUA_TTABLE) {
        return true;
    }
    LOG_LUA_LINE("%s() called with an invalid type for param %d: %s", functionName, param, luaL_typename(L, index));
    return false;
}

static bool browser_lua_expect_string_or_nil(lua_State *L, int index, const char *functionName, int param) {
    int type = lua_type(L, index);
    if (type == LUA_TNIL || type == LUA_TNONE || type == LUA_TSTRING) {
        return true;
    }
    LOG_LUA_LINE("%s() called with an invalid type for param %d: %s", functionName, param, luaL_typename(L, index));
    return false;
}

static bool browser_lua_parse_function_field(lua_State *L, int tableIndex, const char *fieldName, int *outRef, const char *functionName) {
    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, fieldName);
    if (lua_type(L, -1) == LUA_TNIL) {
        lua_pop(L, 1);
        return true;
    }
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        LOG_LUA_LINE("%s() field '%s' must be a function", functionName, fieldName);
        lua_pop(L, 1);
        return false;
    }
    *outRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

static void browser_lua_parse_vec3_table(lua_State *L, int tableIndex, Vec3f out) {
    tableIndex = lua_absindex(L, tableIndex);
    const char *keys[3] = { "x", "y", "z" };
    for (u32 i = 0; i < 3; i++) {
        lua_getfield(L, tableIndex, keys[i]);
        if (lua_type(L, -1) == LUA_TNUMBER) {
            out[i] = (f32) lua_tonumber(L, -1);
            lua_pop(L, 1);
            continue;
        }
        lua_pop(L, 1);
        lua_rawgeti(L, tableIndex, i + 1);
        if (lua_type(L, -1) == LUA_TNUMBER) {
            out[i] = (f32) lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    }
}

static void browser_lua_parse_world_options(lua_State *L, int tableIndex, struct BrowserRenderWorldOptions *opts) {
    *opts = (struct BrowserRenderWorldOptions) {
        .width = 0.0f,
        .height = 0.0f,
        .offset = { 0.0f, 0.0f, 0.0f },
        .rotation = { 0.0f, 0.0f, 0.0f },
        .layer = -1,
        .depth = true,
        .twoSided = false,
        .emissive = true,
        .filter = false,
        .hidden = false,
    };
    if (lua_type(L, tableIndex) != LUA_TTABLE) { return; }

    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, "width");
    if (lua_type(L, -1) == LUA_TNUMBER) { opts->width = (f32) lua_tonumber(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "height");
    if (lua_type(L, -1) == LUA_TNUMBER) { opts->height = (f32) lua_tonumber(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "offset");
    if (lua_type(L, -1) == LUA_TTABLE) { browser_lua_parse_vec3_table(L, -1, opts->offset); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "rotation");
    if (lua_type(L, -1) == LUA_TTABLE) { browser_lua_parse_vec3_table(L, -1, opts->rotation); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "layer");
    if (lua_type(L, -1) == LUA_TNUMBER) { opts->layer = (s16) lua_tointeger(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "depth");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->depth = lua_toboolean(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "two_sided");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->twoSided = lua_toboolean(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "emissive");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->emissive = lua_toboolean(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "filter");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->filter = lua_toboolean(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "hidden");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->hidden = lua_toboolean(L, -1); }
    lua_pop(L, 1);
}

static void browser_lua_parse_hud_options(lua_State *L, int tableIndex, struct BrowserHudOptions *opts) {
    *opts = (struct BrowserHudOptions) {
        .hasFilter = false,
        .filter = false,
        .hasRotation = false,
        .rotation = 0.0f,
        .pivotX = 0.0f,
        .pivotY = 0.0f,
    };
    if (lua_type(L, tableIndex) != LUA_TTABLE) { return; }

    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, "filter");
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        opts->hasFilter = true;
        opts->filter = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "rotation");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        opts->hasRotation = true;
        opts->rotation = (f32) lua_tonumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "pivot_x");
    if (lua_type(L, -1) == LUA_TNUMBER) { opts->pivotX = (f32) lua_tonumber(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "pivot_y");
    if (lua_type(L, -1) == LUA_TNUMBER) { opts->pivotY = (f32) lua_tonumber(L, -1); }
    lua_pop(L, 1);
}

static bool browser_lua_parse_create_options(lua_State *L, int tableIndex, struct BrowserCreateOptions *opts) {
    *opts = (struct BrowserCreateOptions) {
        .transparent = false,
        .audio = true,
        .onLoad = LUA_NOREF,
        .onError = LUA_NOREF,
        .onConsole = LUA_NOREF,
        .onMessage = LUA_NOREF,
        .onPaint = LUA_NOREF,
    };
    if (lua_type(L, tableIndex) != LUA_TTABLE) { return true; }

    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, "transparent");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->transparent = lua_toboolean(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "audio");
    if (lua_type(L, -1) == LUA_TBOOLEAN) { opts->audio = lua_toboolean(L, -1); }
    lua_pop(L, 1);

    if (!browser_lua_parse_function_field(L, tableIndex, "on_load", &opts->onLoad, "browser_create")
     || !browser_lua_parse_function_field(L, tableIndex, "on_error", &opts->onError, "browser_create")
     || !browser_lua_parse_function_field(L, tableIndex, "on_console", &opts->onConsole, "browser_create")
     || !browser_lua_parse_function_field(L, tableIndex, "on_message", &opts->onMessage, "browser_create")
     || !browser_lua_parse_function_field(L, tableIndex, "on_paint", &opts->onPaint, "browser_create")) {
        browser_lua_release_ref(&opts->onLoad);
        browser_lua_release_ref(&opts->onError);
        browser_lua_release_ref(&opts->onConsole);
        browser_lua_release_ref(&opts->onMessage);
        browser_lua_release_ref(&opts->onPaint);
        return false;
    }

    return true;
}

static int browser_lua_parse_key_event_type(lua_State *L, int index) {
    if (lua_type(L, index) == LUA_TNUMBER) {
        return (int) lua_tointeger(L, index);
    }

    const char *eventType = luaL_checkstring(L, index);
    if (strcmp(eventType, "down") == 0 || strcmp(eventType, "key_down") == 0) { return 0; }
    if (strcmp(eventType, "up") == 0 || strcmp(eventType, "key_up") == 0) { return 1; }
    if (strcmp(eventType, "char") == 0 || strcmp(eventType, "text") == 0) { return 2; }
    if (strcmp(eventType, "raw") == 0 || strcmp(eventType, "raw_key_down") == 0) { return 3; }
    return 0;
}

static int smlua_func_browser_available(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 0)) { return 0; }
    lua_pushboolean(L, browser_manager_available());
    return 1;
}

static int smlua_func_browser_create(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 2, 3)) { return 0; }
    if (!browser_lua_expect_table_or_nil(L, 3, "browser_create", 3)) { return 0; }

    struct Mod *ownerMod = browser_lua_get_owner_mod();
    if (ownerMod == NULL) {
        LOG_LUA_LINE("browser_create() requires an active mod context");
        return 0;
    }

    u32 width = (u32) luaL_checkinteger(L, 1);
    u32 height = (u32) luaL_checkinteger(L, 2);
    struct BrowserCreateOptions opts = { 0 };
    if (!browser_lua_parse_create_options(L, 3, &opts)) { return 0; }

    s32 id = browser_manager_create(width, height, &opts, ownerMod, gLuaActiveModFile);
    if (id == BROWSER_ID_NONE) {
        browser_lua_release_ref(&opts.onLoad);
        browser_lua_release_ref(&opts.onError);
        browser_lua_release_ref(&opts.onConsole);
        browser_lua_release_ref(&opts.onMessage);
        browser_lua_release_ref(&opts.onPaint);
    }

    lua_pushinteger(L, id);
    return 1;
}

static int smlua_func_browser_destroy(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 1)) { return 0; }
    browser_manager_destroy((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_destroy_all(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 0)) { return 0; }
    browser_manager_destroy_all_for_mod(browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_open_url(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 2)) { return 0; }
    browser_manager_open_url((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), luaL_checkstring(L, 2));
    return 0;
}

static int smlua_func_browser_set_html(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 2, 3)) { return 0; }
    if (!browser_lua_expect_string_or_nil(L, 3, "browser_set_html", 3)) { return 0; }

    const char *baseUrl = NULL;
    if (lua_type(L, 3) == LUA_TSTRING) {
        baseUrl = lua_tostring(L, 3);
    }

    browser_manager_set_html((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), luaL_checkstring(L, 2), baseUrl);
    return 0;
}

static int smlua_func_browser_reload(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 1)) { return 0; }
    browser_manager_reload((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_stop(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 1)) { return 0; }
    browser_manager_stop((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_go_back(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 1)) { return 0; }
    browser_manager_go_back((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_go_forward(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 1)) { return 0; }
    browser_manager_go_forward((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_run_js(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 2, 3)) { return 0; }
    int callbackRef = LUA_NOREF;
    if (lua_type(L, 3) == LUA_TFUNCTION) {
        lua_pushvalue(L, 3);
        callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else if (lua_type(L, 3) != LUA_TNIL && lua_type(L, 3) != LUA_TNONE) {
        LOG_LUA_LINE("browser_run_js() called with an invalid type for param 3: %s", luaL_typename(L, 3));
        return 0;
    }
    if (!browser_manager_run_js((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), luaL_checkstring(L, 2), callbackRef)) {
        browser_lua_release_ref(&callbackRef);
    }
    return 0;
}

static int smlua_func_browser_add_function(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 4)) { return 0; }
    if (lua_type(L, 4) != LUA_TFUNCTION) {
        LOG_LUA_LINE("browser_add_function() called with an invalid type for param 4: %s", luaL_typename(L, 4));
        lua_pushboolean(L, false);
        return 1;
    }

    lua_pushvalue(L, 4);
    int callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
    bool result = browser_manager_add_function(
        (s32) luaL_checkinteger(L, 1),
        browser_lua_get_owner_mod(),
        luaL_checkstring(L, 2),
        luaL_checkstring(L, 3),
        callbackRef);
    if (!result) {
        browser_lua_release_ref(&callbackRef);
    }

    lua_pushboolean(L, result);
    return 1;
}

static int smlua_func_browser_set_volume(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 2)) { return 0; }
    browser_manager_set_volume((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), (f32) luaL_checknumber(L, 2));
    return 0;
}

static int smlua_func_browser_set_muted(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 2)) { return 0; }
    browser_manager_set_muted((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), lua_toboolean(L, 2));
    return 0;
}

static int smlua_func_browser_resize(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 3)) { return 0; }
    browser_manager_resize((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), (u32) luaL_checkinteger(L, 2), (u32) luaL_checkinteger(L, 3));
    return 0;
}

static int smlua_func_browser_render_hud(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 5, 6)) { return 0; }
    if (!browser_lua_expect_table_or_nil(L, 6, "browser_render_hud", 6)) { return 0; }
    struct BrowserHudOptions opts = { 0 };
    browser_lua_parse_hud_options(L, 6, &opts);
    browser_manager_render_hud((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), (f32) luaL_checknumber(L, 2), (f32) luaL_checknumber(L, 3), (f32) luaL_checknumber(L, 4), (f32) luaL_checknumber(L, 5), &opts);
    return 0;
}

static int smlua_func_browser_render_world(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 1, 2)) { return 0; }
    if (!browser_lua_expect_table_or_nil(L, 2, "browser_render_world", 2)) { return 0; }
    struct BrowserRenderWorldOptions opts = { 0 };
    browser_lua_parse_world_options(L, 2, &opts);
    browser_manager_render_world((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), &opts);
    return 0;
}

static int smlua_func_browser_attach_object(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 2, 3)) { return 0; }
    if (!browser_lua_expect_table_or_nil(L, 3, "browser_attach_object", 3)) { return 0; }
    struct Object *obj = smlua_to_cobject(L, 1, LOT_OBJECT);
    if (!gSmLuaConvertSuccess || obj == NULL) {
        LOG_LUA_LINE("browser_attach_object() failed to convert param 1");
        return 0;
    }
    struct BrowserRenderWorldOptions opts = { 0 };
    browser_lua_parse_world_options(L, 3, &opts);
    browser_manager_attach_object(obj, (s32) luaL_checkinteger(L, 2), browser_lua_get_owner_mod(), &opts);
    return 0;
}

static int smlua_func_browser_detach_object(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 2)) { return 0; }
    struct Object *obj = smlua_to_cobject(L, 1, LOT_OBJECT);
    if (!gSmLuaConvertSuccess || obj == NULL) {
        LOG_LUA_LINE("browser_detach_object() failed to convert param 1");
        return 0;
    }
    browser_manager_detach_object(obj, (s32) luaL_checkinteger(L, 2), browser_lua_get_owner_mod());
    return 0;
}

static int smlua_func_browser_pick_object(lua_State *L) {
    if (!smlua_functions_valid_param_count(L, 4)) { return 0; }
    struct Object *obj = smlua_to_cobject(L, 1, LOT_OBJECT);
    if (!gSmLuaConvertSuccess || obj == NULL) {
        LOG_LUA_LINE("browser_pick_object() failed to convert param 1");
        return 0;
    }
    s32 browserX = 0;
    s32 browserY = 0;
    bool hit = browser_manager_pick_object(obj, (s32) luaL_checkinteger(L, 2), browser_lua_get_owner_mod(), (f32) luaL_checknumber(L, 3), (f32) luaL_checknumber(L, 4), &browserX, &browserY);
    lua_pushboolean(L, hit);
    lua_pushinteger(L, browserX);
    lua_pushinteger(L, browserY);
    return 3;
}

static int smlua_func_browser_send_mouse_move(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 3, 4)) { return 0; }
    browser_manager_send_mouse_move((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), (s32) luaL_checkinteger(L, 2), (s32) luaL_checkinteger(L, 3), (u32) (lua_type(L, 4) == LUA_TNUMBER ? lua_tointeger(L, 4) : 0));
    return 0;
}

static int smlua_func_browser_send_mouse_button(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 6, 7)) { return 0; }
    browser_manager_send_mouse_button((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), (s32) luaL_checkinteger(L, 2), (s32) luaL_checkinteger(L, 3), (s32) luaL_checkinteger(L, 4), lua_toboolean(L, 5), (s32) luaL_checkinteger(L, 6), (u32) (lua_type(L, 7) == LUA_TNUMBER ? lua_tointeger(L, 7) : 0));
    return 0;
}

static int smlua_func_browser_send_mouse_wheel(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 5, 6)) { return 0; }
    browser_manager_send_mouse_wheel((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), (s32) luaL_checkinteger(L, 2), (s32) luaL_checkinteger(L, 3), (s32) luaL_checkinteger(L, 4), (s32) luaL_checkinteger(L, 5), (u32) (lua_type(L, 6) == LUA_TNUMBER ? lua_tointeger(L, 6) : 0));
    return 0;
}

static int smlua_func_browser_send_key(lua_State *L) {
    if (!smlua_functions_valid_param_range(L, 5, 6)) { return 0; }
    const char *text = NULL;
    if (lua_type(L, 6) == LUA_TSTRING) {
        text = lua_tostring(L, 6);
    }
    browser_manager_send_key((s32) luaL_checkinteger(L, 1), browser_lua_get_owner_mod(), browser_lua_parse_key_event_type(L, 2), (s32) luaL_checkinteger(L, 3), (s32) luaL_checkinteger(L, 4), (u32) luaL_checkinteger(L, 5), text);
    return 0;
}

void browser_lua_bind_functions(void) {
    lua_State *L = gLuaState;
    smlua_bind_function(L, "browser_available", smlua_func_browser_available);
    smlua_bind_function(L, "browser_create", smlua_func_browser_create);
    smlua_bind_function(L, "browser_destroy", smlua_func_browser_destroy);
    smlua_bind_function(L, "browser_destroy_all", smlua_func_browser_destroy_all);
    smlua_bind_function(L, "browser_open_url", smlua_func_browser_open_url);
    smlua_bind_function(L, "browser_set_html", smlua_func_browser_set_html);
    smlua_bind_function(L, "browser_reload", smlua_func_browser_reload);
    smlua_bind_function(L, "browser_stop", smlua_func_browser_stop);
    smlua_bind_function(L, "browser_go_back", smlua_func_browser_go_back);
    smlua_bind_function(L, "browser_go_forward", smlua_func_browser_go_forward);
    smlua_bind_function(L, "browser_run_js", smlua_func_browser_run_js);
    smlua_bind_function(L, "browser_add_function", smlua_func_browser_add_function);
    smlua_bind_function(L, "browser_set_volume", smlua_func_browser_set_volume);
    smlua_bind_function(L, "browser_set_muted", smlua_func_browser_set_muted);
    smlua_bind_function(L, "browser_resize", smlua_func_browser_resize);
    smlua_bind_function(L, "browser_render_hud", smlua_func_browser_render_hud);
    smlua_bind_function(L, "browser_render_world", smlua_func_browser_render_world);
    smlua_bind_function(L, "browser_attach_object", smlua_func_browser_attach_object);
    smlua_bind_function(L, "browser_detach_object", smlua_func_browser_detach_object);
    smlua_bind_function(L, "browser_pick_object", smlua_func_browser_pick_object);
    smlua_bind_function(L, "browser_send_mouse_move", smlua_func_browser_send_mouse_move);
    smlua_bind_function(L, "browser_send_mouse_button", smlua_func_browser_send_mouse_button);
    smlua_bind_function(L, "browser_send_mouse_wheel", smlua_func_browser_send_mouse_wheel);
    smlua_bind_function(L, "browser_send_key", smlua_func_browser_send_key);
}
