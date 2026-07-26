# Browser Surfaces

The Lua browser API is exposed through engine-neutral `browser_*` functions.

Always gate browser usage with:

```lua
if not browser_available() then
    return
end
```

## Creation

```lua
local id = browser_create(512, 288, {
    transparent = true,
    audio = true,
    on_load = function(browserId, url) end,
    on_error = function(browserId, message, details) end,
    on_console = function(browserId, message, level) end,
    on_message = function(browserId, message) end,
    on_paint = function(browserId, width, height) end,
})
```

`browser_create()` returns `0` on failure.

## Loading content

Use `browser_open_url(id, url)` for:

- `http://...`
- `https://...`
- `about:...`
- `data:...`
- `mod:///...` for files in the owning mod
- `mod://<mod-relative-path>/...`

`file://` URLs are intentionally blocked.

## Rendering

HUD:

```lua
browser_render_hud(id, 32, 32, 320, 180, {
    filter = true,
    rotation = 0,
    pivot_x = 0.5,
    pivot_y = 0.5,
})
```

World/object space:

```lua
browser_render_world(id, {
    width = 120,
    height = 68,
    offset = { x = 0, y = 80, z = 0 },
    rotation = { x = 0, y = 180, z = 0 },
    layer = LAYER_TRANSPARENT,
    depth = true,
    two_sided = true,
    emissive = true,
    filter = true,
})
```

Object attachment:

```lua
browser_attach_object(obj, id, {
    width = 120,
    height = 68,
    offset = { x = 0, y = 80, z = 0 },
})
```

Attached browsers are cleaned up when the browser, object, or owning mod unloads.

## Audio

```lua
browser_set_volume(id, 0.75)
browser_set_muted(id, false)
```

`browser_set_volume()` allows for gain control of browser audio.
When `Fade Distant Sounds` is enabled in the in-game audio settings menu, world-rendered and object-attached
browsers use the engine's distance attenuation curve.

## Input and JS

```lua
browser_run_js(id, "document.title", function(browserId, success, result, errorMessage)
end)

browser_add_function(id, "console", "luaprint", function(str)
    djui_chat_message_create(tostring(str))
end)

browser_run_js(id, "console.luaprint('Hello from JavaScript')")

browser_send_mouse_move(id, x, y, modifiers)
browser_send_mouse_button(id, x, y, button, down, clickCount, modifiers)
browser_send_mouse_wheel(id, x, y, dx, dy, modifiers)
browser_send_key(id, "down", keyCode, nativeCode, modifiers, text)
```

`browser_run_js()` is the `RunJavascript` equivalent.

`browser_add_function()` is the `AddFunction` equivalent. JavaScript values are forwarded to Lua as positional callback arguments. Objects and arrays are passed as JSON strings; use `json_decode(...)` to turn them into Lua tables.
