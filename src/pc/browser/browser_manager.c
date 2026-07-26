#include <math.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(WINSOCK)
#include "pc/network/socket/socket_windows.h"
#else
#include "pc/network/socket/socket_linux.h"
#endif

#include <PR/gbi.h>
#include <PR/gbi_extension.h>

#include "browser_backend.h"
#include "browser_manager.h"

#include "audio/external.h"
#include "engine/math_util.h"
#include "game/camera.h"
#include "game/display.h"
#include "game/memory.h"
#include "game/object_helpers.h"
#include "game/rendering_graph_node.h"
#include "object_constants.h"
#include "pc/configfile.h"
#include "pc/debuglog.h"
#include "pc/djui/djui_gfx.h"
#include "pc/djui/djui_hud_utils.h"
#include "pc/gfx/gfx_pc.h"
#include "pc/lua/smlua.h"
#include "pc/lua/smlua_hooks.h"
#include "pc/mods/mod.h"
#include "pc/mods/mods_utils.h"
#include "pc/pc_main.h"
#include "sm64.h"

#define BROWSER_MAX_INSTANCES 64
#define BROWSER_MAX_ATTACHMENTS 128
#define BROWSER_MAX_JS_CALLBACKS 32
#define BROWSER_MAX_FUNCTION_BINDINGS 64
#define BROWSER_MAX_EVENTS 256
#define BROWSER_TEXT_MAX 1024
#define BROWSER_URL_MAX 2048
#define BROWSER_URL_HOST_MAX 512
#define BROWSER_FUNCTION_OBJECT_MAX 128
#define BROWSER_FUNCTION_NAME_MAX 128
#define BROWSER_WORLD_DEFAULT_SCALE 0.1f

enum BrowserQueuedEventType {
    BROWSER_EVENT_LOAD,
    BROWSER_EVENT_ERROR,
    BROWSER_EVENT_CONSOLE,
    BROWSER_EVENT_MESSAGE,
    BROWSER_EVENT_FUNCTION_CALL,
    BROWSER_EVENT_PAINT,
    BROWSER_EVENT_JS_RESULT,
};

struct BrowserPendingJsCallback {
    bool inUse;
    s32 requestId;
    int callbackRef;
};

struct BrowserQueuedEvent {
    bool inUse;
    enum BrowserQueuedEventType type;
    s32 browserId;
    bool success;
    s32 intValueA;
    s32 intValueB;
    int callbackRef;
    struct Mod *ownerMod;
    struct ModFile *ownerFile;
    char textA[BROWSER_TEXT_MAX];
    char textB[BROWSER_TEXT_MAX];
    char *heapTextA;
    char *heapTextB;
};

struct BrowserFunctionBinding {
    bool inUse;
    s32 bindingId;
    int callbackRef;
    char objectName[BROWSER_FUNCTION_OBJECT_MAX];
    char functionName[BROWSER_FUNCTION_NAME_MAX];
};

struct BrowserAttachment {
    bool inUse;
    struct Object *obj;
    s32 browserId;
    struct Mod *ownerMod;
    struct BrowserRenderWorldOptions options;
};

struct BrowserInstance {
    bool inUse;
    s32 id;
    struct Mod *ownerMod;
    struct ModFile *ownerFile;
    struct BrowserBackendBrowser *backend;
    struct BrowserBackendSurface surface;
    struct TextureInfo textureInfo;
    char textureName[32];
    char lastUrl[BROWSER_URL_MAX];
    u32 width;
    u32 height;
    f32 volume;
    f32 appliedVolume;
    Vec3f lastWorldAudioPosition;
    u32 lastWorldAudioTimestamp;
    bool muted;
    bool appliedMuted;
    bool transparent;
    bool audio;
    bool hasWorldAudioPosition;
    bool surfaceDirty;
    struct BrowserCreateOptions createOptions;
    struct BrowserPendingJsCallback pendingJs[BROWSER_MAX_JS_CALLBACKS];
    struct BrowserFunctionBinding functionBindings[BROWSER_MAX_FUNCTION_BINDINGS];
};

static bool sBrowserManagerInited = false;
static bool sBrowserDnsInited = false;
static s32 sNextBrowserId = 1;
static s32 sNextJsRequestId = 1;
static s32 sNextFunctionBindingId = 1;
static struct BrowserInstance sBrowsers[BROWSER_MAX_INSTANCES] = { 0 };
static struct BrowserAttachment sAttachments[BROWSER_MAX_ATTACHMENTS] = { 0 };
static struct BrowserQueuedEvent sQueuedEvents[BROWSER_MAX_EVENTS] = { 0 };

static bool browser_validate_url(const struct BrowserInstance *browser, const char *url, char *errorMessage, size_t errorMessageSize);

static void browser_copy_string(char *dst, size_t dstSize, const char *src) {
    if (dstSize == 0) { return; }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstSize, "%s", src);
}

static bool browser_path_within_root(const char *path, const char *root) {
    size_t rootLen = strlen(root);
#ifdef _WIN32
    if (_strnicmp(path, root, rootLen) != 0) {
        return false;
    }
#else
    if (strncmp(path, root, rootLen) != 0) {
        return false;
    }
#endif

    return path[rootLen] == '\0' || path[rootLen] == '/' || path[rootLen] == '\\';
}

static bool browser_resolve_mod_url_to_path(const struct BrowserInstance *browser, const char *url, char *path, size_t pathCapacity) {
    if (browser == NULL || browser->ownerMod == NULL || !browser->ownerMod->isDirectory
        || url == NULL || strncmp(url, "mod://", 6) != 0) {
        return false;
    }

    const char *modPath = url + 6;
    char modBasePath[SYS_MAX_PATH] = { 0 };
    char relativePath[SYS_MAX_PATH] = { 0 };
    char resolvedPath[SYS_MAX_PATH] = { 0 };

    if (modPath[0] == '/') {
        browser_copy_string(relativePath, sizeof(relativePath), modPath + 1);
    } else {
        const char *slash = strchr(modPath, '/');
        size_t ownerModPathLen = strlen(browser->ownerMod->relativePath);
        if (strncmp(modPath, browser->ownerMod->relativePath, ownerModPathLen) == 0) {
            if (modPath[ownerModPathLen] == '\0') {
                relativePath[0] = '\0';
            } else if (modPath[ownerModPathLen] == '/') {
                browser_copy_string(relativePath, sizeof(relativePath), modPath + ownerModPathLen + 1);
            } else {
                return false;
            }
        } else if (slash != NULL) {
            // Chromium may canonicalize mod:///foo/bar into mod://foo/bar.
            // Treat that host+path form as owner-relative shorthand.
            browser_copy_string(relativePath, sizeof(relativePath), modPath);
        } else {
            browser_copy_string(relativePath, sizeof(relativePath), modPath);
        }
    }

    browser_copy_string(modBasePath, sizeof(modBasePath), browser->ownerMod->basePath);
    normalize_path(modBasePath);
    normalize_path(relativePath);

    if (relativePath[0] == '\0') {
        browser_copy_string(resolvedPath, sizeof(resolvedPath), modBasePath);
    } else {
        resolve_relative_path(modBasePath, relativePath, resolvedPath);
    }
    normalize_path(resolvedPath);

    if (!browser_path_within_root(resolvedPath, modBasePath)) {
        return false;
    }

    if (path != NULL && pathCapacity > 0) {
        browser_copy_string(path, pathCapacity, resolvedPath);
    }

    return true;
}

static void browser_set_event_text(char *buffer, size_t bufferSize, char **heapText, const char *text) {
    if (bufferSize == 0) { return; }
    if (heapText != NULL && *heapText != NULL) {
        free(*heapText);
        *heapText = NULL;
    }

    buffer[0] = '\0';
    if (text == NULL) {
        return;
    }

    size_t textLen = strlen(text);
    if (textLen < bufferSize) {
        memcpy(buffer, text, textLen + 1);
        return;
    }

    if (heapText == NULL) {
        browser_copy_string(buffer, bufferSize, text);
        return;
    }

    *heapText = calloc(textLen + 1, 1);
    if (*heapText == NULL) {
        browser_copy_string(buffer, bufferSize, text);
        return;
    }

    memcpy(*heapText, text, textLen + 1);
}

static const char *browser_get_event_text(const char *buffer, const char *heapText) {
    return heapText != NULL ? heapText : buffer;
}

static void browser_set_error_message(char *dst, size_t dstSize, const char *src) {
    if (dst == NULL || dstSize == 0) { return; }
    browser_copy_string(dst, dstSize, src);
}

static bool browser_dns_init(void) {
#if defined(WINSOCK)
    if (sBrowserDnsInited) {
        return true;
    }

    WSADATA wsaData = { 0 };
    int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != NO_ERROR) {
        LOG_ERROR("Browser URL filter WSAStartup failed with error %d", rc);
        return false;
    }

    sBrowserDnsInited = true;
#endif
    return true;
}

static void browser_dns_shutdown(void) {
#if defined(WINSOCK)
    if (!sBrowserDnsInited) { return; }
    WSACleanup();
    sBrowserDnsInited = false;
#endif
}

static bool browser_string_equals_case_insensitive(const char *lhs, size_t lhsLen, const char *rhs, size_t rhsLen) {
    if (lhsLen != rhsLen) {
        return false;
    }

    for (size_t i = 0; i < lhsLen; i++) {
        if (tolower((unsigned char) lhs[i]) != tolower((unsigned char) rhs[i])) {
            return false;
        }
    }

    return true;
}

static bool browser_host_is_localhost(const char *host) {
    if (host == NULL || host[0] == '\0') {
        return false;
    }

    size_t hostLen = strlen(host);
    while (hostLen > 0 && host[hostLen - 1] == '.') {
        hostLen--;
    }

    const char localhost[] = "localhost";
    size_t localhostLen = strlen(localhost);
    if (browser_string_equals_case_insensitive(host, hostLen, localhost, localhostLen)) {
        return true;
    }

    return hostLen > localhostLen
        && host[hostLen - localhostLen - 1] == '.'
        && browser_string_equals_case_insensitive(host + (hostLen - localhostLen), localhostLen, localhost, localhostLen);
}

static bool browser_extract_http_host(const char *url, char *host, size_t hostSize) {
    if (url == NULL || host == NULL || hostSize == 0) {
        return false;
    }

    const char *schemeSeparator = strstr(url, "://");
    if (schemeSeparator == NULL) {
        return false;
    }

    const char *authority = schemeSeparator + 3;
    const char *authorityEnd = authority;
    const char *lastAt = NULL;
    while (*authorityEnd != '\0' && *authorityEnd != '/' && *authorityEnd != '?' && *authorityEnd != '#') {
        if (*authorityEnd == '@') {
            lastAt = authorityEnd;
        }
        authorityEnd++;
    }

    const char *hostStart = lastAt != NULL ? lastAt + 1 : authority;
    if (hostStart >= authorityEnd) {
        return false;
    }

    const char *hostEnd = authorityEnd;
    if (*hostStart == '[') {
        hostStart++;
        hostEnd = hostStart;
        while (hostEnd < authorityEnd && *hostEnd != ']') {
            hostEnd++;
        }
        if (hostEnd == authorityEnd || hostStart == hostEnd) {
            return false;
        }
        if ((hostEnd + 1) < authorityEnd && hostEnd[1] != ':') {
            return false;
        }
    } else {
        hostEnd = hostStart;
        while (hostEnd < authorityEnd && *hostEnd != ':') {
            hostEnd++;
        }
        if (hostStart == hostEnd) {
            return false;
        }
    }

    size_t hostLen = (size_t) (hostEnd - hostStart);
    for (size_t i = 0; i < hostLen; i++) {
        if (hostStart[i] == '%') {
            hostLen = i;
            break;
        }
    }
    while (hostLen > 0 && hostStart[hostLen - 1] == '.') {
        hostLen--;
    }

    if (hostLen == 0 || hostLen >= hostSize) {
        return false;
    }

    memcpy(host, hostStart, hostLen);
    host[hostLen] = '\0';
    return true;
}

static bool browser_ipv4_is_disallowed(const struct in_addr *address) {
    if (address == NULL) {
        return false;
    }

    u32 hostOrder = ntohl(address->s_addr);
    return ((hostOrder & 0xff000000u) == 0x00000000u)
        || ((hostOrder & 0xff000000u) == 0x0a000000u)
        || ((hostOrder & 0xff000000u) == 0x7f000000u)
        || ((hostOrder & 0xffff0000u) == 0xa9fe0000u)
        || ((hostOrder & 0xfff00000u) == 0xac100000u)
        || ((hostOrder & 0xffff0000u) == 0xc0a80000u);
}

static bool browser_ipv6_is_all_zeroes(const struct in6_addr *address) {
    if (address == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(address->s6_addr); i++) {
        if (address->s6_addr[i] != 0) {
            return false;
        }
    }

    return true;
}

static bool browser_ipv6_is_loopback(const struct in6_addr *address) {
    if (address == NULL) {
        return false;
    }

    for (size_t i = 0; i < (sizeof(address->s6_addr) - 1); i++) {
        if (address->s6_addr[i] != 0) {
            return false;
        }
    }

    return address->s6_addr[sizeof(address->s6_addr) - 1] == 1;
}

static bool browser_ipv6_is_ipv4_mapped(const struct in6_addr *address) {
    static const u8 prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    return address != NULL && memcmp(address->s6_addr, prefix, sizeof(prefix)) == 0;
}

static bool browser_ipv6_is_disallowed(const struct in6_addr *address) {
    if (address == NULL) {
        return false;
    }

    if (browser_ipv6_is_all_zeroes(address) || browser_ipv6_is_loopback(address)) {
        return true;
    }

    if ((address->s6_addr[0] & 0xfe) == 0xfc) {
        return true;
    }

    if (address->s6_addr[0] == 0xfe && (address->s6_addr[1] & 0xc0) == 0x80) {
        return true;
    }

    if (address->s6_addr[0] == 0xfe && (address->s6_addr[1] & 0xc0) == 0xc0) {
        return true;
    }

    if (browser_ipv6_is_ipv4_mapped(address)) {
        struct in_addr mapped = { 0 };
        memcpy(&mapped.s_addr, &address->s6_addr[12], sizeof(mapped.s_addr));
        return browser_ipv4_is_disallowed(&mapped);
    }

    return false;
}

static bool browser_host_is_disallowed_literal_address(const char *host) {
    if (host == NULL || host[0] == '\0') {
        return false;
    }

    struct in_addr ipv4Address = { 0 };
    if (inet_pton(AF_INET, host, &ipv4Address) == 1) {
        return browser_ipv4_is_disallowed(&ipv4Address);
    }

    struct in6_addr ipv6Address = { 0 };
    if (inet_pton(AF_INET6, host, &ipv6Address) == 1) {
        return browser_ipv6_is_disallowed(&ipv6Address);
    }

    return false;
}

static bool browser_host_resolves_to_disallowed_address(const char *host, bool *outResolved) {
    if (outResolved != NULL) {
        *outResolved = false;
    }

    if (host == NULL || host[0] == '\0') {
        return false;
    }

    if (browser_host_is_localhost(host)) {
        if (outResolved != NULL) {
            *outResolved = true;
        }
        return true;
    }

    if (browser_host_is_disallowed_literal_address(host)) {
        if (outResolved != NULL) {
            *outResolved = true;
        }
        return true;
    }

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
    hints.ai_flags = AI_ADDRCONFIG;
#endif

    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc != 0) {
        LOG_INFO("Browser URL filter could not resolve '%s': %s", host, gai_strerror(rc));
        return false;
    }

    if (outResolved != NULL) {
        *outResolved = true;
    }

    bool disallowed = false;
    for (const struct addrinfo *entry = result; entry != NULL; entry = entry->ai_next) {
        if (entry->ai_addr == NULL) {
            continue;
        }

        if (entry->ai_family == AF_INET) {
            const struct sockaddr_in *address = (const struct sockaddr_in *) entry->ai_addr;
            if (browser_ipv4_is_disallowed(&address->sin_addr)) {
                disallowed = true;
                break;
            }
        } else if (entry->ai_family == AF_INET6) {
            const struct sockaddr_in6 *address = (const struct sockaddr_in6 *) entry->ai_addr;
            if (browser_ipv6_is_disallowed(&address->sin6_addr)) {
                disallowed = true;
                break;
            }
        }
    }

    freeaddrinfo(result);
    return disallowed;
}

static bool browser_is_unreserved_url_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-'
        || c == '.'
        || c == '_'
        || c == '~';
}

static const char *browser_find_case_insensitive(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') { return haystack; }

    size_t needleLen = strlen(needle);
    for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
        size_t i = 0;
        while (i < needleLen
            && cursor[i] != '\0'
            && tolower((unsigned char) cursor[i]) == tolower((unsigned char) needle[i])) {
            i++;
        }
        if (i == needleLen) {
            return cursor;
        }
    }

    return NULL;
}

static char *browser_escape_html_attribute(const char *text) {
    if (text == NULL) { return NULL; }

    size_t escapedLen = 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        switch (*cursor) {
            case '&': escapedLen += strlen("&amp;"); break;
            case '"': escapedLen += strlen("&quot;"); break;
            case '\'': escapedLen += strlen("&#39;"); break;
            case '<': escapedLen += strlen("&lt;"); break;
            case '>': escapedLen += strlen("&gt;"); break;
            default: escapedLen++; break;
        }
    }

    char *escaped = calloc(escapedLen + 1, 1);
    if (escaped == NULL) { return NULL; }

    char *dst = escaped;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        const char *replacement = NULL;
        switch (*cursor) {
            case '&': replacement = "&amp;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            default:
                *(dst++) = *cursor;
                break;
        }
        if (replacement != NULL) {
            size_t len = strlen(replacement);
            memcpy(dst, replacement, len);
            dst += len;
        }
    }

    return escaped;
}

static char *browser_inject_base_tag(const char *html, const char *baseUrl) {
    if (html == NULL) { return NULL; }
    if (baseUrl == NULL || baseUrl[0] == '\0') {
        size_t htmlLen = strlen(html);
        char *copy = calloc(htmlLen + 1, 1);
        if (copy != NULL) {
            memcpy(copy, html, htmlLen);
        }
        return copy;
    }

    char *escapedBaseUrl = browser_escape_html_attribute(baseUrl);
    if (escapedBaseUrl == NULL) { return NULL; }

    const char *baseTagPrefix = "<base href=\"";
    const char *baseTagSuffix = "\">";
    size_t baseTagLen = strlen(baseTagPrefix) + strlen(escapedBaseUrl) + strlen(baseTagSuffix);
    char *baseTag = calloc(baseTagLen + 1, 1);
    if (baseTag == NULL) {
        free(escapedBaseUrl);
        return NULL;
    }
    snprintf(baseTag, baseTagLen + 1, "%s%s%s", baseTagPrefix, escapedBaseUrl, baseTagSuffix);
    free(escapedBaseUrl);

    size_t htmlLen = strlen(html);
    const char *headStart = html;
    while ((headStart = browser_find_case_insensitive(headStart, "<head")) != NULL) {
        char next = headStart[5];
        if (next == '\0' || isspace((unsigned char) next) || next == '>') {
            break;
        }
        headStart += 5;
    }

    if (headStart != NULL) {
        const char *headEnd = strchr(headStart, '>');
        if (headEnd != NULL) {
            size_t prefixLen = (size_t) (headEnd + 1 - html);
            char *result = calloc(htmlLen + baseTagLen + 1, 1);
            if (result != NULL) {
                memcpy(result, html, prefixLen);
                memcpy(result + prefixLen, baseTag, baseTagLen);
                memcpy(result + prefixLen + baseTagLen, html + prefixLen, htmlLen - prefixLen);
            }
            free(baseTag);
            return result;
        }
    }

    const char *headWrapPrefix = "<head>";
    const char *headWrapSuffix = "</head>";
    size_t prefixLen = strlen(headWrapPrefix);
    size_t suffixLen = strlen(headWrapSuffix);
    char *result = calloc(prefixLen + baseTagLen + suffixLen + htmlLen + 1, 1);
    if (result != NULL) {
        memcpy(result, headWrapPrefix, prefixLen);
        memcpy(result + prefixLen, baseTag, baseTagLen);
        memcpy(result + prefixLen + baseTagLen, headWrapSuffix, suffixLen);
        memcpy(result + prefixLen + baseTagLen + suffixLen, html, htmlLen);
    }

    free(baseTag);
    return result;
}

static char *browser_build_html_data_url(const char *html) {
    if (html == NULL) { return NULL; }

    const char *prefix = "data:text/html;charset=utf-8,";
    size_t prefixLen = strlen(prefix);
    size_t encodedLen = 0;
    for (const unsigned char *cursor = (const unsigned char *) html; *cursor != '\0'; cursor++) {
        encodedLen += browser_is_unreserved_url_byte(*cursor) ? 1 : 3;
    }

    char *url = calloc(prefixLen + encodedLen + 1, 1);
    if (url == NULL) { return NULL; }

    memcpy(url, prefix, prefixLen);
    char *dst = url + prefixLen;
    for (const unsigned char *cursor = (const unsigned char *) html; *cursor != '\0'; cursor++) {
        if (browser_is_unreserved_url_byte(*cursor)) {
            *(dst++) = (char) *cursor;
        } else {
            snprintf(dst, 4, "%%%02X", *cursor);
            dst += 3;
        }
    }

    return url;
}

static bool browser_resolve_html_base_url(const struct BrowserInstance *browser, const char *baseUrl, char *dst, size_t dstSize) {
    if (dst == NULL || dstSize == 0) { return false; }
    dst[0] = '\0';

    if (baseUrl != NULL && baseUrl[0] != '\0') {
        if (!browser_validate_url(browser, baseUrl, NULL, 0)) {
            return false;
        }
        browser_copy_string(dst, dstSize, baseUrl);
        return true;
    }

    if (browser != NULL && browser->ownerMod != NULL && browser->ownerMod->isDirectory) {
        snprintf(dst, dstSize, "mod:///");
    }
    return true;
}

static void browser_release_lua_ref(int *ref) {
    if (ref == NULL || *ref == LUA_NOREF || *ref == LUA_REFNIL) { return; }
    if (gLuaState != NULL) {
        luaL_unref(gLuaState, LUA_REGISTRYINDEX, *ref);
    }
    *ref = LUA_NOREF;
}

static u32 browser_next_power_of_two(u32 value) {
    u32 power = 1;
    if (value == 0) { return 0; }
    while (power < value) {
        power <<= 1;
        if (power == 0) { return 0; }
    }
    return power;
}

static u8 browser_power_of_two_exponent(u32 value) {
    u8 power = 0;
    while (value > 1) {
        value >>= 1;
        power++;
    }
    return power;
}

static struct BrowserRenderWorldOptions browser_default_world_options(void) {
    struct BrowserRenderWorldOptions opts = {
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
    return opts;
}

static struct BrowserHudOptions browser_default_hud_options(void) {
    struct BrowserHudOptions opts = {
        .hasFilter = false,
        .filter = false,
        .hasRotation = false,
        .rotation = 0.0f,
        .pivotX = 0.0f,
        .pivotY = 0.0f,
    };
    return opts;
}

static struct BrowserInstance *browser_find(s32 id) {
    if (id == BROWSER_ID_NONE) { return NULL; }
    for (u32 i = 0; i < BROWSER_MAX_INSTANCES; i++) {
        if (sBrowsers[i].inUse && sBrowsers[i].id == id) {
            return &sBrowsers[i];
        }
    }
    return NULL;
}

static bool browser_owned_by(const struct BrowserInstance *browser, struct Mod *ownerMod) {
    if (browser == NULL || ownerMod == NULL) { return false; }
    return browser->ownerMod == ownerMod;
}

static struct BrowserFunctionBinding *browser_find_function_binding_by_id(struct BrowserInstance *browser, s32 bindingId) {
    if (browser == NULL || bindingId == 0) { return NULL; }
    for (u32 i = 0; i < BROWSER_MAX_FUNCTION_BINDINGS; i++) {
        if (browser->functionBindings[i].inUse && browser->functionBindings[i].bindingId == bindingId) {
            return &browser->functionBindings[i];
        }
    }
    return NULL;
}

static struct BrowserFunctionBinding *browser_find_function_binding(struct BrowserInstance *browser, const char *objectName, const char *functionName) {
    if (browser == NULL || objectName == NULL || functionName == NULL) { return NULL; }
    for (u32 i = 0; i < BROWSER_MAX_FUNCTION_BINDINGS; i++) {
        if (!browser->functionBindings[i].inUse) { continue; }
        if (strcmp(browser->functionBindings[i].objectName, objectName) != 0) { continue; }
        if (strcmp(browser->functionBindings[i].functionName, functionName) != 0) { continue; }
        return &browser->functionBindings[i];
    }
    return NULL;
}

static int browser_get_callback_ref(const struct BrowserInstance *browser, enum BrowserQueuedEventType type) {
    if (browser == NULL) { return LUA_NOREF; }
    switch (type) {
        case BROWSER_EVENT_LOAD: return browser->createOptions.onLoad;
        case BROWSER_EVENT_ERROR: return browser->createOptions.onError;
        case BROWSER_EVENT_CONSOLE: return browser->createOptions.onConsole;
        case BROWSER_EVENT_MESSAGE: return browser->createOptions.onMessage;
        case BROWSER_EVENT_FUNCTION_CALL: return LUA_NOREF;
        case BROWSER_EVENT_PAINT: return browser->createOptions.onPaint;
        case BROWSER_EVENT_JS_RESULT: return LUA_NOREF;
    }
    return LUA_NOREF;
}

static void browser_clear_event(struct BrowserQueuedEvent *event) {
    if (event == NULL) { return; }
    browser_release_lua_ref(&event->callbackRef);
    if (event->heapTextA != NULL) {
        free(event->heapTextA);
    }
    if (event->heapTextB != NULL) {
        free(event->heapTextB);
    }
    memset(event, 0, sizeof(*event));
}

static void browser_prune_events_for_browser(s32 browserId) {
    for (u32 i = 0; i < BROWSER_MAX_EVENTS; i++) {
        if (sQueuedEvents[i].inUse && sQueuedEvents[i].browserId == browserId) {
            browser_clear_event(&sQueuedEvents[i]);
        }
    }
}

static struct BrowserQueuedEvent *browser_alloc_event(void) {
    for (u32 i = 0; i < BROWSER_MAX_EVENTS; i++) {
        if (!sQueuedEvents[i].inUse) {
            memset(&sQueuedEvents[i], 0, sizeof(sQueuedEvents[i]));
            sQueuedEvents[i].inUse = true;
            sQueuedEvents[i].callbackRef = LUA_NOREF;
            return &sQueuedEvents[i];
        }
    }
    LOG_ERROR("Browser event queue is full.");
    return NULL;
}

static void browser_mark_surface_dirty(struct BrowserInstance *browser) {
    if (browser == NULL || browser->surface.pixels == NULL) { return; }
    gfx_dynamic_texture_mark_dirty(browser->surface.pixels);
    browser->surfaceDirty = false;
}

static bool browser_get_listener_position(Vec3f outPosition) {
    if (outPosition == NULL) { return false; }

    if (gCamera != NULL) {
        vec3f_copy(outPosition, gCamera->pos);
        return true;
    }

    vec3f_copy(outPosition, gLakituState.pos);
    return true;
}

static void browser_record_world_audio_position(struct BrowserInstance *browser, Vec3f position) {
    if (browser == NULL || position == NULL) { return; }

    vec3f_copy(browser->lastWorldAudioPosition, position);
    browser->lastWorldAudioTimestamp = gGlobalTimer;
    browser->hasWorldAudioPosition = true;
}

static bool browser_get_recent_world_audio_position(const struct BrowserInstance *browser, Vec3f outPosition) {
    if (browser == NULL || outPosition == NULL || !browser->hasWorldAudioPosition) { return false; }
    if (browser->lastWorldAudioTimestamp != gGlobalTimer && browser->lastWorldAudioTimestamp + 1 != gGlobalTimer) {
        return false;
    }

    vec3f_copy(outPosition, browser->lastWorldAudioPosition);
    return true;
}

static bool browser_get_attachment_audio_position(const struct BrowserAttachment *attachment, Vec3f outPosition) {
    if (attachment == NULL || attachment->obj == NULL || outPosition == NULL) { return false; }

    if (!attachment->obj->header.gfx.inited) {
        obj_update_gfx_pos_and_angle(attachment->obj);
        attachment->obj->header.gfx.inited = true;
    }

    Vec3f localOffset = { 0 };
    vec3f_copy(localOffset, attachment->options.offset);
    vec3f_transform(outPosition, localOffset, attachment->obj->header.gfx.pos, attachment->obj->header.gfx.angle, attachment->obj->header.gfx.scale);
    return true;
}

static bool browser_get_spatial_audio_position(const struct BrowserInstance *browser, Vec3f outPosition) {
    if (browser == NULL || outPosition == NULL) { return false; }

    Vec3f listenerPosition = { 0 };
    bool hasListenerPosition = browser_get_listener_position(listenerPosition);
    bool found = false;
    f32 bestDistanceSq = 0.0f;

    for (u32 i = 0; i < BROWSER_MAX_ATTACHMENTS; i++) {
        if (!sAttachments[i].inUse || sAttachments[i].browserId != browser->id) { continue; }

        Vec3f attachmentPosition = { 0 };
        if (!browser_get_attachment_audio_position(&sAttachments[i], attachmentPosition)) { continue; }

        f32 distanceSq = 0.0f;
        if (hasListenerPosition) {
            f32 dx = attachmentPosition[0] - listenerPosition[0];
            f32 dy = attachmentPosition[1] - listenerPosition[1];
            f32 dz = attachmentPosition[2] - listenerPosition[2];
            distanceSq = dx * dx + dy * dy + dz * dz;
        }

        if (!found || (hasListenerPosition && distanceSq < bestDistanceSq)) {
            vec3f_copy(outPosition, attachmentPosition);
            bestDistanceSq = distanceSq;
            found = true;
        }
    }

    if (found) {
        return true;
    }

    return browser_get_recent_world_audio_position(browser, outPosition);
}

static f32 browser_get_effective_volume(const struct BrowserInstance *browser) {
    if (browser == NULL) { return 0.0f; }

    f32 sfxVolume = ((f32) configSfxVolume / 127.0f) * ((f32) gLuaVolumeSfx / 127.0f);
    f32 effectiveVolume = clamp(browser->volume, 0.0f, 1.0f) * clamp(gMasterVolume, 0.0f, 1.0f) * clamp(sfxVolume, 0.0f, 1.0f);
    if (configMuteFocusLoss && gWindowApi != NULL && gWindowApi->has_focus != NULL && !gWindowApi->has_focus()) {
        effectiveVolume = 0.0f;
    }

    if (configFadeoutDistantSounds) {
        Vec3f listenerPosition = { 0 };
        Vec3f spatialAudioPosition = { 0 };
        if (browser_get_listener_position(listenerPosition) && browser_get_spatial_audio_position(browser, spatialAudioPosition)) {
            f32 dx = spatialAudioPosition[0] - listenerPosition[0];
            f32 dy = spatialAudioPosition[1] - listenerPosition[1];
            f32 dz = spatialAudioPosition[2] - listenerPosition[2];
            f32 distance = sqrtf(dx * dx + dy * dy + dz * dz);
            effectiveVolume *= clamp(sound_get_level_intensity(distance), 0.0f, 1.0f);
        }
    }

    return clamp(effectiveVolume, 0.0f, 1.0f);
}

static void browser_sync_audio_state(struct BrowserInstance *browser) {
    if (browser == NULL || browser->backend == NULL) { return; }

    f32 effectiveVolume = browser_get_effective_volume(browser);
    if (fabsf(browser->appliedVolume - effectiveVolume) > 0.0001f) {
        browser_backend_set_volume(browser->backend, effectiveVolume);
        browser->appliedVolume = effectiveVolume;
    }

    if (browser->appliedMuted != browser->muted) {
        browser_backend_set_muted(browser->backend, browser->muted);
        browser->appliedMuted = browser->muted;
    }
}

static bool browser_surface_resize(struct BrowserInstance *browser, u32 width, u32 height, bool preserveContents) {
    if (browser == NULL || width == 0 || height == 0) { return false; }

    u32 surfaceWidth = browser_next_power_of_two(width);
    u32 surfaceHeight = browser_next_power_of_two(height);
    if (surfaceWidth == 0 || surfaceHeight == 0) { return false; }

    size_t bufferSize = (size_t) surfaceWidth * (size_t) surfaceHeight * 4;
    Texture *newPixels = calloc(bufferSize, 1);
    if (newPixels == NULL) {
        LOG_ERROR("Failed to allocate browser surface %ux%u.", width, height);
        return false;
    }

    if (preserveContents && browser->surface.pixels != NULL) {
        u32 copyWidth = min(width, browser->width);
        u32 copyHeight = min(height, browser->height);
        for (u32 y = 0; y < copyHeight; y++) {
            size_t oldOffset = (size_t) y * browser->surface.surfaceWidth * 4;
            size_t newOffset = (size_t) y * surfaceWidth * 4;
            memcpy((u8 *) newPixels + newOffset, (const u8 *) browser->surface.pixels + oldOffset, (size_t) copyWidth * 4);
        }
    }

    if (browser->surface.pixels != NULL) {
        gfx_dynamic_texture_forget(browser->surface.pixels);
        free((void *) browser->surface.pixels);
    }

    browser->width = width;
    browser->height = height;
    browser->surface.pixels = newPixels;
    browser->surface.width = width;
    browser->surface.height = height;
    browser->surface.surfaceWidth = surfaceWidth;
    browser->surface.surfaceHeight = surfaceHeight;
    browser->textureInfo.texture = newPixels;
    browser->textureInfo.name = browser->textureName;
    browser->textureInfo.width = surfaceWidth;
    browser->textureInfo.height = surfaceHeight;
    browser->textureInfo.format = G_IM_FMT_RGBA;
    browser->textureInfo.size = G_IM_SIZ_32b;
    browser->surfaceDirty = true;
    return true;
}

static void browser_clear_callbacks(struct BrowserInstance *browser) {
    if (browser == NULL) { return; }
    browser_release_lua_ref(&browser->createOptions.onLoad);
    browser_release_lua_ref(&browser->createOptions.onError);
    browser_release_lua_ref(&browser->createOptions.onConsole);
    browser_release_lua_ref(&browser->createOptions.onMessage);
    browser_release_lua_ref(&browser->createOptions.onPaint);
}

static void browser_clear_pending_js_callbacks(struct BrowserInstance *browser) {
    if (browser == NULL) { return; }
    for (u32 i = 0; i < BROWSER_MAX_JS_CALLBACKS; i++) {
        if (!browser->pendingJs[i].inUse) { continue; }
        browser_release_lua_ref(&browser->pendingJs[i].callbackRef);
        memset(&browser->pendingJs[i], 0, sizeof(browser->pendingJs[i]));
    }
}

static void browser_clear_function_bindings(struct BrowserInstance *browser) {
    if (browser == NULL) { return; }
    for (u32 i = 0; i < BROWSER_MAX_FUNCTION_BINDINGS; i++) {
        if (!browser->functionBindings[i].inUse) { continue; }
        browser_release_lua_ref(&browser->functionBindings[i].callbackRef);
        memset(&browser->functionBindings[i], 0, sizeof(browser->functionBindings[i]));
    }
}

static void browser_detach_all_for_id(s32 browserId) {
    for (u32 i = 0; i < BROWSER_MAX_ATTACHMENTS; i++) {
        if (sAttachments[i].inUse && sAttachments[i].browserId == browserId) {
            memset(&sAttachments[i], 0, sizeof(sAttachments[i]));
        }
    }
}

static void browser_destroy_internal(struct BrowserInstance *browser) {
    if (browser == NULL || !browser->inUse) { return; }

    browser_prune_events_for_browser(browser->id);
    browser_detach_all_for_id(browser->id);

    if (browser->backend != NULL) {
        browser_backend_destroy(browser->backend);
        browser->backend = NULL;
    }

    browser_clear_pending_js_callbacks(browser);
    browser_clear_function_bindings(browser);
    browser_clear_callbacks(browser);

    if (browser->surface.pixels != NULL) {
        gfx_dynamic_texture_forget(browser->surface.pixels);
        free((void *) browser->surface.pixels);
    }

    memset(browser, 0, sizeof(*browser));
}

static bool browser_validate_url(const struct BrowserInstance *browser, const char *url, char *errorMessage, size_t errorMessageSize) {
    if (browser == NULL || url == NULL || url[0] == '\0') {
        browser_set_error_message(errorMessage, errorMessageSize, "Blocked empty browser URL.");
        return false;
    }

    if (strncmp(url, "file://", 7) == 0) {
        browser_set_error_message(errorMessage, errorMessageSize, "Blocked file:// browser URL.");
        return false;
    }

    if (strncmp(url, "mod://", 6) == 0) {
        if (!browser_resolve_mod_url_to_path(browser, url, NULL, 0)) {
            browser_set_error_message(errorMessage, errorMessageSize, "Blocked cross-mod browser URL.");
            return false;
        }
        return true;
    }

    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        char host[BROWSER_URL_HOST_MAX] = { 0 };
        if (!browser_extract_http_host(url, host, sizeof(host))) {
            browser_set_error_message(errorMessage, errorMessageSize, "Blocked malformed browser URL.");
            return false;
        }

        bool resolved = false;
        if (browser_host_resolves_to_disallowed_address(host, &resolved)) {
            browser_set_error_message(errorMessage, errorMessageSize, "Blocked browser URL targeting localhost or a private network address.");
            return false;
        }

        if (!resolved) {
            browser_set_error_message(errorMessage, errorMessageSize, "Blocked browser URL because the host could not be resolved for safety filtering.");
            return false;
        }

        return true;
    }

    if (strncmp(url, "about:", 6) == 0 || strncmp(url, "data:", 5) == 0) {
        return true;
    }

    browser_set_error_message(errorMessage, errorMessageSize, "Blocked unsupported browser URL scheme.");
    return false;
}

static s16 browser_degrees_to_angle(f32 degrees) {
    return (s16) roundf(degrees * (65536.0f / 360.0f));
}

static void browser_build_local_quad(const struct BrowserRenderWorldOptions *opts, f32 width, f32 height, Vec3f corners[4]) {
    Vec3s rotation = {
        browser_degrees_to_angle(opts->rotation[0]),
        browser_degrees_to_angle(opts->rotation[1]),
        browser_degrees_to_angle(opts->rotation[2]),
    };
    Vec3f unitScale = { 1.0f, 1.0f, 1.0f };
    f32 halfWidth = width * 0.5f;
    f32 halfHeight = height * 0.5f;
    Vec3f baseCorners[4] = {
        { -halfWidth,  halfHeight, 0.0f },
        {  halfWidth,  halfHeight, 0.0f },
        {  halfWidth, -halfHeight, 0.0f },
        { -halfWidth, -halfHeight, 0.0f },
    };

    for (u32 i = 0; i < 4; i++) {
        vec3f_transform(corners[i], baseCorners[i], (f32 *) opts->offset, rotation, unitScale);
    }
}

static void browser_resolve_world_size(const struct BrowserInstance *browser, const struct BrowserRenderWorldOptions *opts, f32 *outWidth, f32 *outHeight) {
    f32 width = opts->width;
    f32 height = opts->height;
    f32 aspect = browser->width != 0 ? (f32) browser->height / (f32) browser->width : 1.0f;

    if (width <= 0.0f && height <= 0.0f) {
        width = (f32) browser->width * BROWSER_WORLD_DEFAULT_SCALE;
        height = width * aspect;
    } else if (width <= 0.0f) {
        width = height / max(aspect, 0.0001f);
    } else if (height <= 0.0f) {
        height = width * aspect;
    }

    *outWidth = max(width, 1.0f);
    *outHeight = max(height, 1.0f);
}

static s16 browser_clamp_coord(f32 value) {
    s32 rounded = (s32) lroundf(value);
    return (s16) clamp(rounded, -32768, 32767);
}

static Gfx *browser_build_world_display_list(struct BrowserInstance *browser, const struct BrowserRenderWorldOptions *opts) {
    if (browser == NULL || browser->surface.pixels == NULL) { return NULL; }

    f32 worldWidth = 0.0f;
    f32 worldHeight = 0.0f;
    Vec3f corners[4] = { 0 };
    browser_resolve_world_size(browser, opts, &worldWidth, &worldHeight);
    browser_build_local_quad(opts, worldWidth, worldHeight, corners);

    Vtx *vtx = alloc_display_list(sizeof(*vtx) * 4);
    Gfx *gfx = alloc_display_list(sizeof(*gfx) * 32);
    if (vtx == NULL || gfx == NULL) { return NULL; }

    f32 uMin = 1.0f;
    f32 vMin = 1.0f;
    f32 uMax = ((f32) browser->width * 2048.0f) / max((f32) browser->surface.surfaceWidth, 1.0f) + 1.0f;
    f32 vMax = ((f32) browser->height * 2048.0f) / max((f32) browser->surface.surfaceHeight, 1.0f) + 1.0f;

    vtx[0] = (Vtx) {{{ browser_clamp_coord(corners[0][0]), browser_clamp_coord(corners[0][1]), browser_clamp_coord(corners[0][2]) }, 0, { (s16) uMin, (s16) vMin }, { 0xFF, 0xFF, 0xFF, 0xFF }}};
    vtx[1] = (Vtx) {{{ browser_clamp_coord(corners[1][0]), browser_clamp_coord(corners[1][1]), browser_clamp_coord(corners[1][2]) }, 0, { (s16) uMax, (s16) vMin }, { 0xFF, 0xFF, 0xFF, 0xFF }}};
    vtx[2] = (Vtx) {{{ browser_clamp_coord(corners[2][0]), browser_clamp_coord(corners[2][1]), browser_clamp_coord(corners[2][2]) }, 0, { (s16) uMax, (s16) vMax }, { 0xFF, 0xFF, 0xFF, 0xFF }}};
    vtx[3] = (Vtx) {{{ browser_clamp_coord(corners[3][0]), browser_clamp_coord(corners[3][1]), browser_clamp_coord(corners[3][2]) }, 0, { (s16) uMin, (s16) vMax }, { 0xFF, 0xFF, 0xFF, 0xFF }}};

    Gfx *head = gfx;
    gSPSaveState(head++, G_STATE_GEOMETRY_MODE
                       | G_STATE_COMBINE_MODE
                       | G_STATE_OTHER_MODE
                       | G_STATE_ENV_COLOR
                       | G_STATE_PRIM_COLOR
                       | G_STATE_FOG_COLOR
                       | G_STATE_FILL_COLOR
                       | G_STATE_FRESNEL
                       | G_STATE_TEXTURES
                       | G_STATE_LIGHTS);
    gDPPipeSync(head++);
    gSPClearGeometryMode(head++, G_LIGHTING | G_CULL_BOTH | G_FOG);
    if (opts->twoSided) {
        gSPClearGeometryMode(head++, G_CULL_BACK);
    } else {
        gSPSetGeometryMode(head++, G_CULL_BACK);
    }
    if (opts->depth) {
        gSPSetGeometryMode(head++, G_ZBUFFER);
        if (browser->transparent) {
            gDPSetRenderMode(head++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
        } else {
            gDPSetRenderMode(head++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
        }
    } else {
        gSPClearGeometryMode(head++, G_ZBUFFER);
        if (browser->transparent) {
            gDPSetRenderMode(head++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        } else {
            gDPSetRenderMode(head++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
        }
    }
    gDPSetCombineMode(head++, G_CC_FADEA, G_CC_FADEA);
    gDPSetTextureFilter(head++, opts->filter ? G_TF_BILERP : G_TF_POINT);
    gSPTexture(head++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPSetTextureOverrideDjui(head++, browser->surface.pixels, browser_power_of_two_exponent(browser->surface.surfaceWidth), browser_power_of_two_exponent(browser->surface.surfaceHeight), G_IM_FMT_RGBA, G_IM_SIZ_32b);
    gDPLoadTextureBlockWithoutTexture(head++, NULL, G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    *(head++) = (Gfx) gsSPExecuteDjui(G_TEXOVERRIDE_DJUI);
    gSPVertexNonGlobal(head++, vtx, 4, 0);
    gSP2TrianglesDjui(head++, 0, 1, 2, 0, 0, 2, 3, 0);
    gSPTexture(head++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gSPLoadState(head++, G_STATE_GEOMETRY_MODE
                       | G_STATE_COMBINE_MODE
                       | G_STATE_OTHER_MODE
                       | G_STATE_ENV_COLOR
                       | G_STATE_PRIM_COLOR
                       | G_STATE_FOG_COLOR
                       | G_STATE_FILL_COLOR
                       | G_STATE_FRESNEL
                       | G_STATE_TEXTURES
                       | G_STATE_LIGHTS);
    gSPEndDisplayList(head++);
    return gfx;
}

static bool browser_render_world_internal(struct BrowserInstance *browser, const struct BrowserRenderWorldOptions *opts) {
    if (browser == NULL || opts == NULL || gCurGraphNodeMasterList == NULL) { return false; }
    if (opts->hidden) {
        return true;
    }
    if (browser->surfaceDirty) {
        browser_mark_surface_dirty(browser);
    }

    Gfx *gfx = browser_build_world_display_list(browser, opts);
    if (gfx == NULL) { return false; }

    s16 layer = opts->layer;
    if (layer < 0 || layer >= GFX_NUM_MASTER_LISTS) {
        layer = browser->transparent ? LAYER_TRANSPARENT : LAYER_OPAQUE;
    }
    geo_append_display_list_ext((void *) VIRTUAL_TO_PHYSICAL(gfx), layer);
    return true;
}

static struct BrowserAttachment *browser_find_attachment(struct Object *obj, s32 browserId, struct Mod *ownerMod) {
    for (u32 i = 0; i < BROWSER_MAX_ATTACHMENTS; i++) {
        if (!sAttachments[i].inUse) { continue; }
        if (sAttachments[i].obj != obj || sAttachments[i].browserId != browserId) { continue; }
        if (ownerMod != NULL && sAttachments[i].ownerMod != ownerMod) { continue; }
        return &sAttachments[i];
    }
    return NULL;
}

static bool browser_make_screen_ray(f32 screenX, f32 screenY, Vec3f origin, Vec3f direction) {
    u32 windowWidth = 0;
    u32 windowHeight = 0;
    gfx_get_dimensions(&windowWidth, &windowHeight);
    if (windowWidth == 0 || windowHeight == 0) { return false; }

    Vec3f forward = { 0 };
    Vec3f right = { 0 };
    Vec3f up = { 0 };
    Vec3f worldUp = { 0.0f, 1.0f, 0.0f };

    vec3f_copy(origin, gLakituState.pos);
    vec3f_dif(forward, gLakituState.focus, gLakituState.pos);
    if (vec3f_length(forward) < 0.0001f) { return false; }
    vec3f_normalize(forward);

    vec3f_cross(right, forward, worldUp);
    if (vec3f_length(right) < 0.0001f) {
        right[0] = 1.0f;
        right[1] = 0.0f;
        right[2] = 0.0f;
    } else {
        vec3f_normalize(right);
    }

    vec3f_cross(up, right, forward);
    vec3f_normalize(up);

    f32 rollCos = coss(gLakituState.roll);
    f32 rollSin = sins(gLakituState.roll);
    Vec3f rolledRight = { 0 };
    Vec3f rolledUp = { 0 };
    for (u32 i = 0; i < 3; i++) {
        rolledRight[i] = right[i] * rollCos + up[i] * rollSin;
        rolledUp[i] = up[i] * rollCos - right[i] * rollSin;
    }

    f32 fovRadians = get_current_fov() * (M_PI / 180.0f);
    f32 tanHalfFov = tanf(fovRadians * 0.5f);
    f32 aspect = (f32) windowWidth / (f32) windowHeight;
    f32 ndcX = ((screenX / (f32) windowWidth) * 2.0f - 1.0f) * aspect * tanHalfFov;
    f32 ndcY = (1.0f - (screenY / (f32) windowHeight) * 2.0f) * tanHalfFov;

    vec3f_copy(direction, forward);
    direction[0] += rolledRight[0] * ndcX + rolledUp[0] * ndcY;
    direction[1] += rolledRight[1] * ndcX + rolledUp[1] * ndcY;
    direction[2] += rolledRight[2] * ndcX + rolledUp[2] * ndcY;
    if (vec3f_length(direction) < 0.0001f) { return false; }
    vec3f_normalize(direction);
    return true;
}

static bool browser_pick_attachment(struct BrowserInstance *browser, const struct BrowserAttachment *attachment, f32 screenX, f32 screenY, s32 *browserX, s32 *browserY) {
    if (browser == NULL || attachment == NULL || attachment->obj == NULL) { return false; }
    if (attachment->options.hidden) { return false; }

    if (!attachment->obj->header.gfx.inited) {
        obj_update_gfx_pos_and_angle(attachment->obj);
        attachment->obj->header.gfx.inited = true;
    }

    f32 quadWidth = 0.0f;
    f32 quadHeight = 0.0f;
    Vec3f localCorners[4] = { 0 };
    Vec3f worldCorners[4] = { 0 };
    Vec3f rayOrigin = { 0 };
    Vec3f rayDirection = { 0 };
    Vec3f objectScale = { 0 };

    browser_resolve_world_size(browser, &attachment->options, &quadWidth, &quadHeight);
    browser_build_local_quad(&attachment->options, quadWidth, quadHeight, localCorners);
    vec3f_copy(objectScale, attachment->obj->header.gfx.scale);

    for (u32 i = 0; i < 4; i++) {
        vec3f_transform(worldCorners[i], localCorners[i], attachment->obj->header.gfx.pos, attachment->obj->header.gfx.angle, objectScale);
    }

    if (!browser_make_screen_ray(screenX, screenY, rayOrigin, rayDirection)) { return false; }

    Vec3f axisX = { 0 };
    Vec3f axisY = { 0 };
    Vec3f planeNormal = { 0 };
    Vec3f hitVector = { 0 };
    Vec3f hitPoint = { 0 };

    vec3f_dif(axisX, worldCorners[1], worldCorners[0]);
    vec3f_dif(axisY, worldCorners[3], worldCorners[0]);
    vec3f_cross(planeNormal, axisX, axisY);
    if (vec3f_length(planeNormal) < 0.0001f) { return false; }
    vec3f_normalize(planeNormal);

    f32 denom = vec3f_dot(planeNormal, rayDirection);
    if (fabsf(denom) < 0.0001f) { return false; }

    Vec3f originToPlane = { 0 };
    vec3f_dif(originToPlane, worldCorners[0], rayOrigin);
    f32 distance = vec3f_dot(originToPlane, planeNormal) / denom;
    if (distance < 0.0f) { return false; }

    vec3f_copy(hitPoint, rayDirection);
    vec3f_mul(hitPoint, distance);
    vec3f_add(hitPoint, rayOrigin);
    vec3f_dif(hitVector, hitPoint, worldCorners[0]);

    f32 axisXLenSq = vec3f_dot(axisX, axisX);
    f32 axisYLenSq = vec3f_dot(axisY, axisY);
    if (axisXLenSq < 0.0001f || axisYLenSq < 0.0001f) { return false; }

    f32 u = vec3f_dot(hitVector, axisX) / axisXLenSq;
    f32 v = vec3f_dot(hitVector, axisY) / axisYLenSq;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) { return false; }

    if (browserX != NULL) {
        *browserX = clamp((s32) lroundf(u * max((f32) browser->width - 1.0f, 0.0f)), 0, (s32) max((s32) browser->width - 1, 0));
    }
    if (browserY != NULL) {
        *browserY = clamp((s32) lroundf(v * max((f32) browser->height - 1.0f, 0.0f)), 0, (s32) max((s32) browser->height - 1, 0));
    }
    return true;
}

static s32 browser_alloc_js_request(struct BrowserInstance *browser, int callbackRef) {
    if (browser == NULL || callbackRef == LUA_NOREF || callbackRef == LUA_REFNIL) { return 0; }
    for (u32 i = 0; i < BROWSER_MAX_JS_CALLBACKS; i++) {
        if (browser->pendingJs[i].inUse) { continue; }
        s32 requestId = sNextJsRequestId++;
        browser->pendingJs[i].inUse = true;
        browser->pendingJs[i].requestId = requestId;
        browser->pendingJs[i].callbackRef = callbackRef;
        return requestId;
    }
    browser_release_lua_ref(&callbackRef);
    LOG_ERROR("Browser JS callback table is full.");
    return 0;
}

static int browser_take_js_callback(struct BrowserInstance *browser, s32 requestId) {
    if (browser == NULL || requestId == 0) { return LUA_NOREF; }
    for (u32 i = 0; i < BROWSER_MAX_JS_CALLBACKS; i++) {
        if (!browser->pendingJs[i].inUse || browser->pendingJs[i].requestId != requestId) { continue; }
        int ref = browser->pendingJs[i].callbackRef;
        memset(&browser->pendingJs[i], 0, sizeof(browser->pendingJs[i]));
        return ref;
    }
    return LUA_NOREF;
}

static bool browser_parse_marshaled_size(const char **cursor, const char *end, size_t *outValue) {
    size_t value = 0;
    bool haveDigits = false;

    while (*cursor < end && isdigit((unsigned char) **cursor)) {
        value = (value * 10u) + (size_t) (**cursor - '0');
        (*cursor)++;
        haveDigits = true;
    }

    if (!haveDigits) {
        return false;
    }

    *outValue = value;
    return true;
}

static void browser_push_number_segment(lua_State *L, const char *data, size_t len, bool integerValue) {
    if (len == 0) {
        if (integerValue) {
            lua_pushinteger(L, 0);
        } else {
            lua_pushnumber(L, 0.0);
        }
        return;
    }

    char *buffer = calloc(len + 1, 1);
    if (buffer == NULL) {
        lua_pushnumber(L, 0.0);
        return;
    }

    memcpy(buffer, data, len);
    if (integerValue) {
        lua_pushinteger(L, strtol(buffer, NULL, 10));
    } else {
        lua_pushnumber(L, strtod(buffer, NULL));
    }
    free(buffer);
}

static int browser_push_marshaled_arguments(lua_State *L, const char *payload) {
    if (payload == NULL || payload[0] == '\0') {
        return 0;
    }

    const char *cursor = payload;
    const char *end = payload + strlen(payload);
    size_t argumentCount = 0;
    if (!browser_parse_marshaled_size(&cursor, end, &argumentCount) || cursor >= end || *cursor != ';') {
        return -1;
    }
    cursor++;

    int pushed = 0;
    for (size_t i = 0; i < argumentCount; i++) {
        if (cursor >= end) {
            return -1;
        }

        char type = *(cursor++);
        size_t len = 0;
        if (!browser_parse_marshaled_size(&cursor, end, &len) || cursor >= end || *cursor != ':') {
            return -1;
        }
        cursor++;

        if ((size_t) (end - cursor) < len) {
            return -1;
        }

        const char *data = cursor;
        cursor += len;

        switch (type) {
            case 'z':
                lua_pushnil(L);
                break;
            case 'b':
                lua_pushboolean(L, len > 0 && data[0] == '1');
                break;
            case 'i':
                browser_push_number_segment(L, data, len, true);
                break;
            case 'n':
                browser_push_number_segment(L, data, len, false);
                break;
            case 's':
            case 'j':
                lua_pushlstring(L, data, len);
                break;
            default:
                return -1;
        }

        pushed++;
    }

    return pushed;
}

static void browser_dispatch_event(struct BrowserQueuedEvent *event) {
    if (event == NULL || !event->inUse) { return; }

    lua_State *L = gLuaState;
    struct BrowserInstance *browser = browser_find(event->browserId);
    int callbackRef = event->callbackRef != LUA_NOREF ? event->callbackRef : browser_get_callback_ref(browser, event->type);
    struct Mod *ownerMod = browser != NULL ? browser->ownerMod : event->ownerMod;
    struct ModFile *ownerFile = browser != NULL ? browser->ownerFile : event->ownerFile;
    const char *textA = browser_get_event_text(event->textA, event->heapTextA);
    const char *textB = browser_get_event_text(event->textB, event->heapTextB);

    if (event->type == BROWSER_EVENT_FUNCTION_CALL && browser != NULL) {
        struct BrowserFunctionBinding *binding = browser_find_function_binding_by_id(browser, event->intValueA);
        callbackRef = binding != NULL ? binding->callbackRef : LUA_NOREF;
    }

    if (L != NULL && callbackRef != LUA_NOREF && callbackRef != LUA_REFNIL) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);

        int nargs = 0;
        switch (event->type) {
            case BROWSER_EVENT_LOAD:
                lua_pushinteger(L, event->browserId);
                lua_pushstring(L, textA);
                nargs = 2;
                break;
            case BROWSER_EVENT_ERROR:
                lua_pushinteger(L, event->browserId);
                lua_pushstring(L, textA);
                if (textB[0] != '\0') {
                    lua_pushstring(L, textB);
                } else {
                    lua_pushnil(L);
                }
                nargs = 3;
                break;
            case BROWSER_EVENT_CONSOLE:
                lua_pushinteger(L, event->browserId);
                lua_pushstring(L, textA);
                lua_pushinteger(L, event->intValueA);
                nargs = 3;
                break;
            case BROWSER_EVENT_MESSAGE:
                lua_pushinteger(L, event->browserId);
                lua_pushstring(L, textA);
                nargs = 2;
                break;
            case BROWSER_EVENT_FUNCTION_CALL:
                nargs = browser_push_marshaled_arguments(L, textA);
                if (nargs < 0) {
                    lua_pushstring(L, textA);
                    nargs = 1;
                }
                break;
            case BROWSER_EVENT_PAINT:
                lua_pushinteger(L, event->browserId);
                lua_pushinteger(L, event->intValueA);
                lua_pushinteger(L, event->intValueB);
                nargs = 3;
                break;
            case BROWSER_EVENT_JS_RESULT:
                lua_pushinteger(L, event->browserId);
                lua_pushboolean(L, event->success);
                if (textA[0] != '\0') {
                    lua_pushstring(L, textA);
                } else {
                    lua_pushnil(L);
                }
                if (textB[0] != '\0') {
                    lua_pushstring(L, textB);
                } else {
                    lua_pushnil(L);
                }
                nargs = 4;
                break;
        }

        if (smlua_call_hook(L, nargs, 0, 0, ownerMod, ownerFile) != 0) {
            LOG_LUA("Failed to dispatch browser callback for browser %d", event->browserId);
        }
    }

    browser_clear_event(event);
}

void browser_manager_backend_notify_load(s32 browserId, const char *url) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }
    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) { return; }
    event->type = BROWSER_EVENT_LOAD;
    event->browserId = browserId;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    browser_set_event_text(event->textA, sizeof(event->textA), &event->heapTextA, url);
}

void browser_manager_backend_notify_error(s32 browserId, const char *message, const char *details) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }
    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) { return; }
    event->type = BROWSER_EVENT_ERROR;
    event->browserId = browserId;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    browser_set_event_text(event->textA, sizeof(event->textA), &event->heapTextA, message);
    browser_set_event_text(event->textB, sizeof(event->textB), &event->heapTextB, details);
}

void browser_manager_backend_notify_console(s32 browserId, const char *message, s32 level) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }
    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) { return; }
    event->type = BROWSER_EVENT_CONSOLE;
    event->browserId = browserId;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    event->intValueA = level;
    browser_set_event_text(event->textA, sizeof(event->textA), &event->heapTextA, message);
}

void browser_manager_backend_notify_message(s32 browserId, const char *message) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }
    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) { return; }
    event->type = BROWSER_EVENT_MESSAGE;
    event->browserId = browserId;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    browser_set_event_text(event->textA, sizeof(event->textA), &event->heapTextA, message);
}

void browser_manager_backend_notify_function_call(s32 browserId, s32 bindingId, const char *argsPayload) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }

    struct BrowserFunctionBinding *binding = browser_find_function_binding_by_id(browser, bindingId);
    if (binding == NULL) { return; }

    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) { return; }

    event->type = BROWSER_EVENT_FUNCTION_CALL;
    event->browserId = browserId;
    event->intValueA = bindingId;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    browser_set_event_text(event->textA, sizeof(event->textA), &event->heapTextA, argsPayload);
}

void browser_manager_backend_notify_paint(s32 browserId, s32 width, s32 height) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }
    browser->surfaceDirty = true;

    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) { return; }
    event->type = BROWSER_EVENT_PAINT;
    event->browserId = browserId;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    event->intValueA = width;
    event->intValueB = height;
}

void browser_manager_backend_notify_js_result(s32 browserId, s32 requestId, bool success, const char *result, const char *errorMessage) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) { return; }

    int callbackRef = browser_take_js_callback(browser, requestId);
    if (callbackRef == LUA_NOREF || callbackRef == LUA_REFNIL) { return; }

    struct BrowserQueuedEvent *event = browser_alloc_event();
    if (event == NULL) {
        browser_release_lua_ref(&callbackRef);
        return;
    }

    event->type = BROWSER_EVENT_JS_RESULT;
    event->browserId = browserId;
    event->callbackRef = callbackRef;
    event->success = success;
    event->ownerMod = browser->ownerMod;
    event->ownerFile = browser->ownerFile;
    browser_set_event_text(event->textA, sizeof(event->textA), &event->heapTextA, result);
    browser_set_event_text(event->textB, sizeof(event->textB), &event->heapTextB, errorMessage);
}

bool browser_manager_backend_resolve_url_to_path(s32 browserId, const char *url, char *path, size_t pathCapacity) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) {
        return false;
    }

    return browser_resolve_mod_url_to_path(browser, url, path, pathCapacity);
}

bool browser_manager_backend_should_load_url(s32 browserId, const char *url) {
    struct BrowserInstance *browser = browser_find(browserId);
    if (browser == NULL) {
        return false;
    }

    char errorMessage[BROWSER_TEXT_MAX] = { 0 };
    if (browser_validate_url(browser, url, errorMessage, sizeof(errorMessage))) {
        return true;
    }

    browser_manager_backend_notify_error(browserId, errorMessage, url);
    return false;
}

void browser_manager_init(void) {
    if (sBrowserManagerInited) { return; }
    memset(sBrowsers, 0, sizeof(sBrowsers));
    memset(sAttachments, 0, sizeof(sAttachments));
    memset(sQueuedEvents, 0, sizeof(sQueuedEvents));
    sNextBrowserId = 1;
    sNextJsRequestId = 1;
    sNextFunctionBindingId = 1;
    browser_dns_init();
    browser_backend_init();
    sBrowserManagerInited = true;
}

void browser_manager_tick(void) {
    if (!sBrowserManagerInited) { return; }

    browser_backend_tick();

    for (u32 i = 0; i < BROWSER_MAX_INSTANCES; i++) {
        if (sBrowsers[i].inUse) {
            browser_sync_audio_state(&sBrowsers[i]);
        }
    }

    for (u32 i = 0; i < BROWSER_MAX_EVENTS; i++) {
        if (sQueuedEvents[i].inUse) {
            browser_dispatch_event(&sQueuedEvents[i]);
        }
    }
}

void browser_manager_on_lua_shutdown(void) {
    if (!sBrowserManagerInited) { return; }
    browser_manager_destroy_all();
    for (u32 i = 0; i < BROWSER_MAX_EVENTS; i++) {
        if (sQueuedEvents[i].inUse) {
            browser_clear_event(&sQueuedEvents[i]);
        }
    }
}

void browser_manager_shutdown(void) {
    if (!sBrowserManagerInited) { return; }
    browser_manager_destroy_all();
    browser_backend_shutdown();
    browser_dns_shutdown();
    memset(sQueuedEvents, 0, sizeof(sQueuedEvents));
    memset(sAttachments, 0, sizeof(sAttachments));
    sBrowserManagerInited = false;
}

bool browser_manager_available(void) {
    return sBrowserManagerInited && browser_backend_available();
}

s32 browser_manager_create(u32 width, u32 height, const struct BrowserCreateOptions *opts, struct Mod *ownerMod, struct ModFile *ownerFile) {
    if (!sBrowserManagerInited || !browser_manager_available() || width == 0 || height == 0 || ownerMod == NULL) {
        return BROWSER_ID_NONE;
    }

    for (u32 i = 0; i < BROWSER_MAX_INSTANCES; i++) {
        if (sBrowsers[i].inUse) { continue; }

        struct BrowserInstance *browser = &sBrowsers[i];
        memset(browser, 0, sizeof(*browser));
        browser->inUse = true;
        browser->id = sNextBrowserId++;
        browser->ownerMod = ownerMod;
        browser->ownerFile = ownerFile;
        browser->volume = 1.0f;
        browser->appliedVolume = -1.0f;
        browser->transparent = opts != NULL && opts->transparent;
        browser->audio = opts == NULL || opts->audio;
        browser->appliedMuted = !browser->muted;
        browser->createOptions = opts != NULL ? *opts : (struct BrowserCreateOptions) {
            .transparent = false,
            .audio = true,
            .onLoad = LUA_NOREF,
            .onError = LUA_NOREF,
            .onConsole = LUA_NOREF,
            .onMessage = LUA_NOREF,
            .onPaint = LUA_NOREF,
        };
        snprintf(browser->textureName, sizeof(browser->textureName), "browser_%d", browser->id);

        if (!browser_surface_resize(browser, width, height, false)) {
            memset(browser, 0, sizeof(*browser));
            return BROWSER_ID_NONE;
        }

        struct BrowserBackendCreateParams params = {
            .browserId = browser->id,
            .transparent = browser->transparent,
            .audio = browser->audio,
        };
        browser->backend = browser_backend_create(&browser->surface, &params);
        if (browser->backend == NULL) {
            browser_destroy_internal(browser);
            return BROWSER_ID_NONE;
        }
        browser_sync_audio_state(browser);
        return browser->id;
    }

    LOG_ERROR("Browser instance limit reached.");
    return BROWSER_ID_NONE;
}

bool browser_manager_destroy(s32 id, struct Mod *ownerMod) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    browser_destroy_internal(browser);
    return true;
}

void browser_manager_destroy_all_for_mod(struct Mod *ownerMod) {
    if (ownerMod == NULL) { return; }
    for (u32 i = 0; i < BROWSER_MAX_INSTANCES; i++) {
        if (sBrowsers[i].inUse && sBrowsers[i].ownerMod == ownerMod) {
            browser_destroy_internal(&sBrowsers[i]);
        }
    }
}

void browser_manager_destroy_all(void) {
    for (u32 i = 0; i < BROWSER_MAX_INSTANCES; i++) {
        if (sBrowsers[i].inUse) {
            browser_destroy_internal(&sBrowsers[i]);
        }
    }
}

bool browser_manager_open_url(s32 id, struct Mod *ownerMod, const char *url) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod) || !browser_manager_backend_should_load_url(id, url)) { return false; }
    browser_copy_string(browser->lastUrl, sizeof(browser->lastUrl), url);
    return browser_backend_open_url(browser->backend, url);
}

bool browser_manager_set_html(s32 id, struct Mod *ownerMod, const char *html, const char *baseUrl) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod) || html == NULL) { return false; }

    char resolvedBaseUrl[BROWSER_URL_MAX] = { 0 };
    if (!browser_resolve_html_base_url(browser, baseUrl, resolvedBaseUrl, sizeof(resolvedBaseUrl))) {
        return false;
    }

    char *htmlWithBase = browser_inject_base_tag(html, resolvedBaseUrl);
    if (htmlWithBase == NULL) { return false; }

    char *dataUrl = browser_build_html_data_url(htmlWithBase);
    free(htmlWithBase);
    if (dataUrl == NULL) { return false; }

    browser_copy_string(
        browser->lastUrl,
        sizeof(browser->lastUrl),
        resolvedBaseUrl[0] != '\0' ? resolvedBaseUrl : "data:text/html");
    bool result = browser_backend_open_url(browser->backend, dataUrl);
    free(dataUrl);
    return result;
}

bool browser_manager_reload(s32 id, struct Mod *ownerMod) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_reload(browser->backend);
}

bool browser_manager_stop(s32 id, struct Mod *ownerMod) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_stop(browser->backend);
}

bool browser_manager_go_back(s32 id, struct Mod *ownerMod) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_go_back(browser->backend);
}

bool browser_manager_go_forward(s32 id, struct Mod *ownerMod) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_go_forward(browser->backend);
}

bool browser_manager_run_js(s32 id, struct Mod *ownerMod, const char *code, int callbackRef) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod) || code == NULL) {
        browser_release_lua_ref(&callbackRef);
        return false;
    }

    s32 requestId = browser_alloc_js_request(browser, callbackRef);
    if (callbackRef != LUA_NOREF && callbackRef != LUA_REFNIL && requestId == 0) { return false; }
    if (browser_backend_run_js(browser->backend, code, requestId)) { return true; }

    if (requestId != 0) {
        int takenRef = browser_take_js_callback(browser, requestId);
        browser_release_lua_ref(&takenRef);
    }
    return false;
}

bool browser_manager_add_function(s32 id, struct Mod *ownerMod, const char *objectName, const char *functionName, int callbackRef) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod) || callbackRef == LUA_NOREF || callbackRef == LUA_REFNIL) {
        return false;
    }
    if (objectName == NULL || objectName[0] == '\0' || functionName == NULL || functionName[0] == '\0') {
        return false;
    }

    struct BrowserFunctionBinding *binding = browser_find_function_binding(browser, objectName, functionName);
    if (binding != NULL) {
        browser_release_lua_ref(&binding->callbackRef);
        binding->callbackRef = callbackRef;
        return true;
    }

    for (u32 i = 0; i < BROWSER_MAX_FUNCTION_BINDINGS; i++) {
        if (browser->functionBindings[i].inUse) { continue; }

        binding = &browser->functionBindings[i];
        memset(binding, 0, sizeof(*binding));
        binding->inUse = true;
        binding->bindingId = sNextFunctionBindingId++;
        binding->callbackRef = callbackRef;
        browser_copy_string(binding->objectName, sizeof(binding->objectName), objectName);
        browser_copy_string(binding->functionName, sizeof(binding->functionName), functionName);

        if (!browser_backend_add_function(browser->backend, binding->bindingId, binding->objectName, binding->functionName)) {
            memset(binding, 0, sizeof(*binding));
            return false;
        }

        return true;
    }

    LOG_ERROR("Browser function binding table is full.");
    return false;
}

bool browser_manager_set_volume(s32 id, struct Mod *ownerMod, f32 volume) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    browser->volume = clamp(volume, 0.0f, 1.0f);
    browser_sync_audio_state(browser);
    return true;
}

bool browser_manager_set_muted(s32 id, struct Mod *ownerMod, bool muted) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    browser->muted = muted;
    browser_sync_audio_state(browser);
    return true;
}

bool browser_manager_resize(s32 id, struct Mod *ownerMod, u32 width, u32 height) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod) || width == 0 || height == 0) { return false; }
    if (!browser_surface_resize(browser, width, height, true)) { return false; }
    return browser_backend_resize(browser->backend, &browser->surface);
}

void browser_manager_render_hud(s32 id, struct Mod *ownerMod, f32 x, f32 y, f32 width, f32 height, const struct BrowserHudOptions *opts) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod) || browser->surface.pixels == NULL) { return; }
    if (browser->surfaceDirty) {
        browser_mark_surface_dirty(browser);
    }

    struct BrowserHudOptions renderOpts = opts != NULL ? *opts : browser_default_hud_options();
    u8 prevFilter = djui_hud_get_filter();
    s16 prevRotation = 0;
    f32 prevPivotX = 0.0f;
    f32 prevPivotY = 0.0f;
    djui_hud_get_rotation(&prevRotation, &prevPivotX, &prevPivotY);

    if (renderOpts.hasFilter) {
        djui_hud_set_filter(renderOpts.filter ? FILTER_LINEAR : FILTER_NEAREST);
    }
    if (renderOpts.hasRotation) {
        djui_hud_set_rotation((s16) lroundf(renderOpts.rotation), renderOpts.pivotX, renderOpts.pivotY);
    }

    djui_hud_render_texture_tile(&browser->textureInfo, x, y, width / max((f32) browser->width, 1.0f), height / max((f32) browser->height, 1.0f), 0, 0, browser->width, browser->height);

    if (renderOpts.hasRotation) {
        djui_hud_set_rotation(prevRotation, prevPivotX, prevPivotY);
    }
    if (renderOpts.hasFilter) {
        djui_hud_set_filter(prevFilter);
    }
}

bool browser_manager_render_world(s32 id, struct Mod *ownerMod, const struct BrowserRenderWorldOptions *opts) {
    struct BrowserInstance *browser = browser_find(id);
    struct BrowserRenderWorldOptions renderOpts = opts != NULL ? *opts : browser_default_world_options();
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    browser_record_world_audio_position(browser, renderOpts.offset);

    bool rendered = browser_render_world_internal(browser, &renderOpts);
    browser_sync_audio_state(browser);
    return rendered;
}

bool browser_manager_attach_object(struct Object *obj, s32 id, struct Mod *ownerMod, const struct BrowserRenderWorldOptions *opts) {
    struct BrowserInstance *browser = browser_find(id);
    if (obj == NULL || browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }

    struct BrowserAttachment *attachment = browser_find_attachment(obj, id, ownerMod);
    if (attachment == NULL) {
        for (u32 i = 0; i < BROWSER_MAX_ATTACHMENTS; i++) {
            if (!sAttachments[i].inUse) {
                attachment = &sAttachments[i];
                memset(attachment, 0, sizeof(*attachment));
                attachment->inUse = true;
                break;
            }
        }
    }
    if (attachment == NULL) { return false; }

    attachment->obj = obj;
    attachment->browserId = id;
    attachment->ownerMod = ownerMod;
    attachment->options = opts != NULL ? *opts : browser_default_world_options();
    return true;
}

void browser_manager_detach_object(struct Object *obj, s32 id, struct Mod *ownerMod) {
    struct BrowserAttachment *attachment = browser_find_attachment(obj, id, ownerMod);
    if (attachment != NULL) {
        memset(attachment, 0, sizeof(*attachment));
    }
}

void browser_manager_detach_object_all(struct Object *obj) {
    if (obj == NULL) { return; }
    for (u32 i = 0; i < BROWSER_MAX_ATTACHMENTS; i++) {
        if (sAttachments[i].inUse && sAttachments[i].obj == obj) {
            memset(&sAttachments[i], 0, sizeof(sAttachments[i]));
        }
    }
}

void browser_manager_render_object(struct Object *obj) {
    if (obj == NULL) { return; }
    for (u32 i = 0; i < BROWSER_MAX_ATTACHMENTS; i++) {
        if (!sAttachments[i].inUse || sAttachments[i].obj != obj) { continue; }
        struct BrowserInstance *browser = browser_find(sAttachments[i].browserId);
        if (browser == NULL) {
            memset(&sAttachments[i], 0, sizeof(sAttachments[i]));
            continue;
        }
        browser_render_world_internal(browser, &sAttachments[i].options);
        browser_sync_audio_state(browser);
    }
}

bool browser_manager_pick_object(struct Object *obj, s32 id, struct Mod *ownerMod, f32 screenX, f32 screenY, s32 *browserX, s32 *browserY) {
    struct BrowserInstance *browser = browser_find(id);
    struct BrowserAttachment *attachment = browser_find_attachment(obj, id, ownerMod);
    if (browser == NULL || attachment == NULL) { return false; }
    return browser_pick_attachment(browser, attachment, screenX, screenY, browserX, browserY);
}

bool browser_manager_send_mouse_move(s32 id, struct Mod *ownerMod, s32 x, s32 y, u32 modifiers) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_send_mouse_move(browser->backend, x, y, modifiers);
}

bool browser_manager_send_mouse_button(s32 id, struct Mod *ownerMod, s32 x, s32 y, s32 button, bool down, s32 clickCount, u32 modifiers) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_send_mouse_button(browser->backend, x, y, button, down, clickCount, modifiers);
}

bool browser_manager_send_mouse_wheel(s32 id, struct Mod *ownerMod, s32 x, s32 y, s32 deltaX, s32 deltaY, u32 modifiers) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_send_mouse_wheel(browser->backend, x, y, deltaX, deltaY, modifiers);
}

bool browser_manager_send_key(s32 id, struct Mod *ownerMod, s32 eventType, s32 keyCode, s32 nativeCode, u32 modifiers, const char *text) {
    struct BrowserInstance *browser = browser_find(id);
    if (browser == NULL || !browser_owned_by(browser, ownerMod)) { return false; }
    return browser_backend_send_key(browser->backend, eventType, keyCode, nativeCode, modifiers, text);
}
