extern "C" {
#include "smlua.h"
#include "smlua_json.h"
}

#include "pc/utils/json.hpp"

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

using json = nlohmann::json;

namespace {

char sJsonNullSentinel = 0;

typedef std::unordered_set<const void*> LuaTableSet;

struct LuaTableGuard {
    LuaTableSet& tables;
    const void* table;
    bool active;

    LuaTableGuard(LuaTableSet& tableSet, const void* tablePtr)
        : tables(tableSet), table(tablePtr), active(true) {
    }

    ~LuaTableGuard() {
        if (active) {
            tables.erase(table);
        }
    }
};

static int smlua_json_return_error(lua_State* L, const std::string& error) {
    lua_pushnil(L);
    lua_pushlstring(L, error.c_str(), error.size());
    return 2;
}

static void smlua_json_push_null(lua_State* L) {
    lua_pushlightuserdata(L, &sJsonNullSentinel);
}

static bool smlua_json_is_null(lua_State* L, int index) {
    return lua_type(L, index) == LUA_TLIGHTUSERDATA && lua_touserdata(L, index) == &sJsonNullSentinel;
}

static void smlua_json_push_value(lua_State* L, const json& value) {
    if (value.is_null()) {
        smlua_json_push_null(L);
        return;
    }

    if (value.is_boolean()) {
        lua_pushboolean(L, value.get<bool>() ? 1 : 0);
        return;
    }

    if (value.is_number_integer()) {
        json::number_integer_t integerValue = value.get<json::number_integer_t>();
        if (integerValue >= std::numeric_limits<lua_Integer>::min() &&
            integerValue <= std::numeric_limits<lua_Integer>::max()) {
            lua_pushinteger(L, static_cast<lua_Integer>(integerValue));
        } else {
            lua_pushnumber(L, static_cast<lua_Number>(integerValue));
        }
        return;
    }

    if (value.is_number_unsigned()) {
        json::number_unsigned_t integerValue = value.get<json::number_unsigned_t>();
        if (integerValue <= static_cast<json::number_unsigned_t>(std::numeric_limits<lua_Integer>::max())) {
            lua_pushinteger(L, static_cast<lua_Integer>(integerValue));
        } else {
            lua_pushnumber(L, static_cast<lua_Number>(integerValue));
        }
        return;
    }

    if (value.is_number_float()) {
        lua_pushnumber(L, static_cast<lua_Number>(value.get<double>()));
        return;
    }

    if (value.is_string()) {
        const std::string& stringValue = value.get_ref<const std::string&>();
        lua_pushlstring(L, stringValue.c_str(), stringValue.size());
        return;
    }

    if (value.is_array()) {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        for (size_t i = 0; i < value.size(); i++) {
            smlua_json_push_value(L, value[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return;
    }

    lua_newtable(L);
    for (json::const_iterator it = value.begin(); it != value.end(); ++it) {
        lua_pushlstring(L, it.key().c_str(), it.key().size());
        smlua_json_push_value(L, it.value());
        lua_settable(L, -3);
    }
}

static std::string smlua_json_key_to_string(lua_State* L, int index) {
    index = lua_absindex(L, index);

    switch (lua_type(L, index)) {
        case LUA_TSTRING: {
            size_t length = 0;
            const char* value = lua_tolstring(L, index, &length);
            return std::string(value, length);
        }

        case LUA_TBOOLEAN:
            return lua_toboolean(L, index) ? "true" : "false";

        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                return std::to_string(static_cast<long long>(lua_tointeger(L, index)));
            } else {
                lua_Number value = lua_tonumber(L, index);
                if (!std::isfinite(static_cast<double>(value))) {
                    throw std::runtime_error("json_encode does not support NaN or infinity table keys");
                }
                std::ostringstream stream;
                stream.precision(std::numeric_limits<lua_Number>::max_digits10);
                stream << static_cast<double>(value);
                return stream.str();
            }

        default:
            break;
    }

    throw std::runtime_error(
        std::string("json_encode does not support table key type '") +
        lua_typename(L, lua_type(L, index)) + "'");
}

static bool smlua_json_is_array_table(lua_State* L, int index, size_t* outLength) {
    index = lua_absindex(L, index);

    size_t count = 0;
    size_t maxKey = 0;

    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (!lua_isinteger(L, -2)) {
            lua_pop(L, 2);
            return false;
        }

        lua_Integer key = lua_tointeger(L, -2);
        if (key < 1) {
            lua_pop(L, 2);
            return false;
        }

        size_t arrayKey = static_cast<size_t>(key);
        if (arrayKey > maxKey) {
            maxKey = arrayKey;
        }
        count++;

        lua_pop(L, 1);
    }

    if (count == 0 || maxKey != count) {
        return false;
    }

    *outLength = maxKey;
    return true;
}

static json smlua_json_to_value(lua_State* L, int index, LuaTableSet& activeTables);

static json smlua_json_table_to_value(lua_State* L, int index, LuaTableSet& activeTables) {
    index = lua_absindex(L, index);

    const void* tablePtr = lua_topointer(L, index);
    if (!activeTables.insert(tablePtr).second) {
        throw std::runtime_error("json_encode does not support recursive tables");
    }

    LuaTableGuard guard(activeTables, tablePtr);

    size_t arrayLength = 0;
    if (smlua_json_is_array_table(L, index, &arrayLength)) {
        json arrayValue = json::array();
        for (size_t i = 1; i <= arrayLength; i++) {
            lua_rawgeti(L, index, static_cast<lua_Integer>(i));
            arrayValue.push_back(smlua_json_to_value(L, -1, activeTables));
            lua_pop(L, 1);
        }
        return arrayValue;
    }

    json objectValue = json::object();
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        lua_pushvalue(L, -2);
        std::string key = smlua_json_key_to_string(L, -1);
        lua_pop(L, 1);

        objectValue[key] = smlua_json_to_value(L, -1, activeTables);
        lua_pop(L, 1);
    }
    return objectValue;
}

static json smlua_json_to_value(lua_State* L, int index, LuaTableSet& activeTables) {
    index = lua_absindex(L, index);

    switch (lua_type(L, index)) {
        case LUA_TNIL:
            return nullptr;

        case LUA_TBOOLEAN:
            return lua_toboolean(L, index) != 0;

        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                return static_cast<long long>(lua_tointeger(L, index));
            } else {
                lua_Number value = lua_tonumber(L, index);
                if (!std::isfinite(static_cast<double>(value))) {
                    throw std::runtime_error("json_encode does not support NaN or infinity");
                }
                return static_cast<double>(value);
            }

        case LUA_TSTRING: {
            size_t length = 0;
            const char* value = lua_tolstring(L, index, &length);
            return std::string(value, length);
        }

        case LUA_TLIGHTUSERDATA:
            if (smlua_json_is_null(L, index)) {
                return nullptr;
            }
            throw std::runtime_error("json_encode only supports json_null for lightuserdata values");

        case LUA_TTABLE:
            return smlua_json_table_to_value(L, index, activeTables);

        default:
            break;
    }

    throw std::runtime_error(
        std::string("json_encode does not support type '") +
        lua_typename(L, lua_type(L, index)) + "'");
}

static int smlua_func_json_decode(lua_State* L) {
    int startTop = lua_gettop(L);

    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING) {
        return smlua_json_return_error(L, "json_decode expects 1 string argument");
    }

    size_t length = 0;
    const char* input = lua_tolstring(L, 1, &length);

    try {
        json value = json::parse(input, input + length);
        smlua_json_push_value(L, value);
        return 1;
    } catch (const std::exception& error) {
        lua_settop(L, startTop);
        return smlua_json_return_error(L, error.what());
    }
}

static int smlua_func_json_encode(lua_State* L) {
    int startTop = lua_gettop(L);

    if (lua_gettop(L) != 1) {
        return smlua_json_return_error(L, "json_encode expects 1 value argument");
    }

    try {
        LuaTableSet activeTables = {};
        std::string encoded = smlua_json_to_value(L, 1, activeTables).dump();
        lua_pushlstring(L, encoded.c_str(), encoded.size());
        return 1;
    } catch (const std::exception& error) {
        lua_settop(L, startTop);
        return smlua_json_return_error(L, error.what());
    }
}

} // namespace

void smlua_json_bind_functions(void) {
    lua_State* L = gLuaState;

    lua_pushcfunction(L, smlua_func_json_decode);
    lua_setglobal(L, "json_decode");

    lua_pushcfunction(L, smlua_func_json_encode);
    lua_setglobal(L, "json_encode");

    smlua_json_push_null(L);
    lua_setglobal(L, "json_null");
}
