if not browser_available() then
    return
end

local browserId = browser_create(512, 288, {
    transparent = true,
    on_message = function(id, message)
        log_to_console(string.format("browser %d message: %s", id, message))
    end,
})

if browserId == 0 then
    return
end

browser_add_function(browserId, "console", "luaprint", function(text)
    log_to_console(tostring(text))
end)

browser_open_url(browserId, "about:blank")
browser_run_js(browserId, "console.luaprint('Hello from JavaScript')")

hook_event(HOOK_ON_HUD_RENDER, function()
    browser_render_hud(browserId, 32, 32, 320, 180, {
        filter = true,
    })
end)
