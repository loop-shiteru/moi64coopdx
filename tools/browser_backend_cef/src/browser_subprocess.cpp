#if defined(_WIN32)
#include <windows.h>
#endif

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/cef_process_message.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "include/internal/cef_types.h"
#include "include/wrapper/cef_helpers.h"

#if defined(_WIN32)
#include "include/cef_sandbox_win.h"
#endif

#include "browser_cef_messages.h"

namespace {

std::string cef_to_utf8(const CefString& value) {
    return value.ToString();
}

std::string normalize_host(std::string host) {
    for (char& ch : host) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return host;
}

std::string get_url_host(const CefString& url) {
    CefURLParts parts;
    if (!CefParseURL(url, parts)) {
        return std::string();
    }
    return normalize_host(cef_to_utf8(CefString(&parts.host)));
}

bool host_matches_or_is_subdomain(const std::string& host, const char* suffix) {
    if (suffix == nullptr || suffix[0] == '\0') {
        return false;
    }

    const size_t suffixLength = std::strlen(suffix);
    if (host == suffix) {
        return true;
    }
    if (host.size() <= suffixLength) {
        return false;
    }

    const size_t offset = host.size() - suffixLength;
    return host.compare(offset, suffixLength, suffix) == 0 && host[offset - 1] == '.';
}

bool should_inject_youtube_ad_patch(CefRefPtr<CefFrame> frame) {
    if (frame == nullptr) {
        return false;
    }

    std::string host = get_url_host(frame->GetURL());
    return host_matches_or_is_subdomain(host, "youtube.com");
}

inline constexpr char kYouTubeAdPatchScript[] = R"JS(
(() => {
  const root = globalThis;
  if (!root || root.__sm64coopdxYouTubeAdPatchInstalled) {
    return;
  }

  Object.defineProperty(root, '__sm64coopdxYouTubeAdPatchInstalled', {
    value: true,
    configurable: false,
    enumerable: false,
    writable: false
  });

  const sanitize = (value) => {
    const seen = new WeakSet();
    const visit = (node) => {
      if (!node || typeof node !== 'object' || seen.has(node)) {
        return node;
      }

      seen.add(node);

      if (Object.prototype.hasOwnProperty.call(node, 'adPlacements')) {
        try { node.adPlacements = null; } catch (e) {}
      }
      if (Object.prototype.hasOwnProperty.call(node, 'playerAds')) {
        try { node.playerAds = null; } catch (e) {}
      }
      if (Object.prototype.hasOwnProperty.call(node, 'adSlots')) {
        try { node.adSlots = null; } catch (e) {}
      }

      for (const key of Object.keys(node)) {
        const child = node[key];
        if (child && typeof child === 'object') {
          visit(child);
        }
      }

      return node;
    };

    return visit(value);
  };

  const installAccessor = (name) => {
    let current = sanitize(root[name]);
    try {
      Object.defineProperty(root, name, {
        configurable: true,
        enumerable: true,
        get() {
          return current;
        },
        set(value) {
          current = sanitize(value);
        }
      });
    } catch (e) {}
    current = sanitize(current);
  };

  const wrapJsonMethod = (holder, name) => {
    const original = holder && holder[name];
    if (typeof original !== 'function') {
      return;
    }

    try {
      Object.defineProperty(holder, name, {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function(...args) {
          const result = original.apply(this, args);
          return result && typeof result.then === 'function'
            ? result.then((value) => sanitize(value))
            : sanitize(result);
        }
      });
    } catch (e) {}
  };

  const blockWorkerConstructor = (name) => {
    const original = root[name];
    if (typeof original !== 'function') {
      return;
    }

    const blocked = function() {
      throw new DOMException('Blocked by browser policy', 'SecurityError');
    };
    blocked.prototype = original.prototype;

    try {
      Object.defineProperty(root, name, {
        configurable: true,
        enumerable: false,
        writable: true,
        value: blocked
      });
    } catch (e) {}
  };

  try {
    const originalJsonParse = JSON.parse;
    JSON.parse = function(...args) {
      return sanitize(originalJsonParse.apply(this, args));
    };
  } catch (e) {}

  if (root.Response && root.Response.prototype) {
    wrapJsonMethod(root.Response.prototype, 'json');
  }

  if (root.navigator && root.navigator.serviceWorker && typeof root.navigator.serviceWorker.register === 'function') {
    try {
      const blockedRegister = function() {
        return Promise.reject(new DOMException('Blocked by browser policy', 'SecurityError'));
      };
      root.navigator.serviceWorker.register = blockedRegister;
    } catch (e) {}
  }

  installAccessor('ytInitialPlayerResponse');
  installAccessor('playerResponse');
  installAccessor('ytInitialData');

  sanitize(root.ytInitialPlayerResponse);
  sanitize(root.playerResponse);
  sanitize(root.ytInitialData);

  blockWorkerConstructor('Worker');
  blockWorkerConstructor('SharedWorker');
})();
)JS";

void inject_youtube_ad_patch(CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) {
    if (frame == nullptr || context == nullptr || !should_inject_youtube_ad_patch(frame)) {
        return;
    }

    CefRefPtr<CefV8Value> retval;
    CefRefPtr<CefV8Exception> exception;
    context->Eval(kYouTubeAdPatchScript, frame->GetURL(), 1, retval, exception);
}

std::string double_to_string(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(17);
    stream << value;
    return stream.str();
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);

    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[7] = { 0 };
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(ch));
                    escaped += buffer;
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }

    return escaped;
}

bool serialize_v8_to_json(CefRefPtr<CefV8Value> value,
                          std::string& out,
                          std::unordered_set<const void*>& visited,
                          int depth) {
    if (value == nullptr || value->IsUndefined() || value->IsNull()) {
        out += "null";
        return true;
    }

    if (value->IsBool()) {
        out += value->GetBoolValue() ? "true" : "false";
        return true;
    }

    if (value->IsInt()) {
        out += std::to_string(value->GetIntValue());
        return true;
    }

    if (value->IsUInt()) {
        out += std::to_string(value->GetUIntValue());
        return true;
    }

    if (value->IsDouble()) {
        out += double_to_string(value->GetDoubleValue());
        return true;
    }

    if (value->IsString()) {
        out += "\"";
        out += json_escape(cef_to_utf8(value->GetStringValue()));
        out += "\"";
        return true;
    }

    if (depth <= 0) {
        out += "null";
        return true;
    }

    const void* identity = value.get();
    if (visited.find(identity) != visited.end()) {
        out += "null";
        return true;
    }

    if (value->IsArray()) {
        visited.insert(identity);
        out += "[";
        int length = value->GetArrayLength();
        for (int i = 0; i < length; ++i) {
            if (i > 0) {
                out += ",";
            }
            if (!serialize_v8_to_json(value->GetValue(i), out, visited, depth - 1)) {
                out += "null";
            }
        }
        out += "]";
        visited.erase(identity);
        return true;
    }

    if (value->IsObject()) {
        visited.insert(identity);
        out += "{";

        std::vector<CefString> keys;
        bool haveKeys = value->GetKeys(keys);
        bool first = true;

        if (haveKeys) {
            for (const CefString& key : keys) {
                CefRefPtr<CefV8Value> child = value->GetValue(key);
                if (child == nullptr || child->IsUndefined() || child->IsFunction()) {
                    continue;
                }

                if (!first) {
                    out += ",";
                }
                first = false;
                out += "\"";
                out += json_escape(cef_to_utf8(key));
                out += "\":";
                if (!serialize_v8_to_json(child, out, visited, depth - 1)) {
                    out += "null";
                }
            }
        }

        out += "}";
        visited.erase(identity);
        return true;
    }

    out += "null";
    return true;
}

std::string serialize_v8_value(CefRefPtr<CefV8Value> value) {
    if (value == nullptr || value->IsUndefined() || value->IsNull()) {
        return std::string();
    }

    if (value->IsString()) {
        return cef_to_utf8(value->GetStringValue());
    }

    if (value->IsBool()) {
        return value->GetBoolValue() ? "true" : "false";
    }

    if (value->IsInt()) {
        return std::to_string(value->GetIntValue());
    }

    if (value->IsUInt()) {
        return std::to_string(value->GetUIntValue());
    }

    if (value->IsDouble()) {
        return double_to_string(value->GetDoubleValue());
    }

    std::unordered_set<const void*> visited;
    std::string json;
    serialize_v8_to_json(value, json, visited, 8);
    return json;
}

std::string serialize_v8_error(CefRefPtr<CefV8Value> value) {
    if (value == nullptr || value->IsUndefined() || value->IsNull()) {
        return "Unknown JavaScript error";
    }

    if (value->IsString()) {
        return cef_to_utf8(value->GetStringValue());
    }

    if (value->IsObject()) {
        if (value->HasValue("stack")) {
            CefRefPtr<CefV8Value> stack = value->GetValue("stack");
            if (stack != nullptr && stack->IsString()) {
                return cef_to_utf8(stack->GetStringValue());
            }
        }
        if (value->HasValue("message")) {
            CefRefPtr<CefV8Value> message = value->GetValue("message");
            if (message != nullptr && message->IsString()) {
                return cef_to_utf8(message->GetStringValue());
            }
        }
    }

    std::string serialized = serialize_v8_value(value);
    return serialized.empty() ? "Unknown JavaScript error" : serialized;
}

std::string exception_to_string(CefRefPtr<CefV8Exception> exception) {
    if (exception == nullptr) {
        return "JavaScript evaluation failed";
    }

    std::string message = cef_to_utf8(exception->GetMessage());
    if (message.empty()) {
        message = "JavaScript evaluation failed";
    }

    int line = exception->GetLineNumber();
    if (line > 0) {
        message += " (line ";
        message += std::to_string(line);
        message += ")";
    }
    return message;
}

CefRefPtr<CefFrame> get_message_target_frame(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
    if (frame != nullptr) {
        return frame;
    }
    if (browser != nullptr) {
        return browser->GetMainFrame();
    }
    return nullptr;
}

void send_post_message(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const std::string& payload) {
    CefRefPtr<CefFrame> targetFrame = get_message_target_frame(browser, frame);
    if (targetFrame == nullptr) {
        return;
    }

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBrowserCefMessagePostMessage);
    message->GetArgumentList()->SetString(0, CefString(payload));
    targetFrame->SendProcessMessage(PID_BROWSER, message);
}

void send_js_result(CefRefPtr<CefBrowser> browser,
                    CefRefPtr<CefFrame> frame,
                    int32_t requestId,
                    bool success,
                    const std::string& result,
                    const std::string& error) {
    if (requestId == 0) {
        return;
    }

    CefRefPtr<CefFrame> targetFrame = get_message_target_frame(browser, frame);
    if (targetFrame == nullptr) {
        return;
    }

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBrowserCefMessageJsResult);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetInt(0, requestId);
    args->SetBool(1, success);
    args->SetString(2, CefString(result));
    args->SetString(3, CefString(error));
    targetFrame->SendProcessMessage(PID_BROWSER, message);
}

enum class MarshaledValueType {
    Null = 0,
    Boolean = 1,
    Int = 2,
    Double = 3,
    String = 4,
    Json = 5,
};

struct RegisteredFunctionBinding {
    int32_t bindingId = 0;
    std::string objectName;
    std::string functionName;
};

CefRefPtr<CefListValue> marshal_v8_arguments(const CefV8ValueList& arguments) {
    CefRefPtr<CefListValue> marshaled = CefListValue::Create();
    marshaled->SetSize(arguments.size());

    for (size_t i = 0; i < arguments.size(); ++i) {
        CefRefPtr<CefListValue> arg = CefListValue::Create();
        arg->SetSize(2);

        CefRefPtr<CefV8Value> value = arguments[i];
        if (value == nullptr || value->IsUndefined() || value->IsNull()) {
            arg->SetInt(0, static_cast<int>(MarshaledValueType::Null));
            arg->SetNull(1);
        } else if (value->IsBool()) {
            arg->SetInt(0, static_cast<int>(MarshaledValueType::Boolean));
            arg->SetBool(1, value->GetBoolValue());
        } else if (value->IsInt()) {
            arg->SetInt(0, static_cast<int>(MarshaledValueType::Int));
            arg->SetInt(1, value->GetIntValue());
        } else if (value->IsUInt()) {
            const uint32_t uintValue = value->GetUIntValue();
            if (uintValue <= static_cast<uint32_t>(INT32_MAX)) {
                arg->SetInt(0, static_cast<int>(MarshaledValueType::Int));
                arg->SetInt(1, static_cast<int>(uintValue));
            } else {
                arg->SetInt(0, static_cast<int>(MarshaledValueType::Double));
                arg->SetDouble(1, static_cast<double>(uintValue));
            }
        } else if (value->IsDouble()) {
            arg->SetInt(0, static_cast<int>(MarshaledValueType::Double));
            arg->SetDouble(1, value->GetDoubleValue());
        } else if (value->IsString()) {
            arg->SetInt(0, static_cast<int>(MarshaledValueType::String));
            arg->SetString(1, value->GetStringValue());
        } else {
            arg->SetInt(0, static_cast<int>(MarshaledValueType::Json));
            arg->SetString(1, CefString(serialize_v8_value(value)));
        }

        marshaled->SetList(i, arg);
    }

    return marshaled;
}

void send_function_call(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        int32_t bindingId,
                        const CefV8ValueList& arguments) {
    if (bindingId == 0) {
        return;
    }

    CefRefPtr<CefFrame> targetFrame = get_message_target_frame(browser, frame);
    if (targetFrame == nullptr) {
        return;
    }

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBrowserCefMessageCallFunction);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetInt(0, bindingId);
    args->SetList(1, marshal_v8_arguments(arguments));
    targetFrame->SendProcessMessage(PID_BROWSER, message);
}

CefRefPtr<CefV8Value> ensure_object_path(CefRefPtr<CefV8Value> root, const std::string& objectName) {
    if (root == nullptr || objectName.empty()) {
        return nullptr;
    }

    CefRefPtr<CefV8Value> current = root;
    size_t start = 0;
    while (start < objectName.size()) {
        size_t end = objectName.find('.', start);
        std::string segment = objectName.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty()) {
            return nullptr;
        }

        CefRefPtr<CefV8Value> next = current->GetValue(CefString(segment));
        if (next == nullptr || !next->IsObject()) {
            next = CefV8Value::CreateObject(nullptr, nullptr);
            current->SetValue(CefString(segment), next, V8_PROPERTY_ATTRIBUTE_NONE);
        }

        current = next;
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return current;
}

void install_registered_function(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefV8Context> context,
                                 const RegisteredFunctionBinding& binding);

enum class BridgeHandlerMode {
    PostMessage,
    InvokeFunction,
    ResolveJsResult,
    RejectJsResult,
};

class BrowserBridgeV8Handler final : public CefV8Handler {
public:
    BrowserBridgeV8Handler(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           BridgeHandlerMode mode,
                           int32_t requestId = 0)
        : browser_(std::move(browser)),
          frame_(std::move(frame)),
          mode_(mode),
          requestId_(requestId) {
    }

    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> object,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval,
                 CefString& exception) override {
        CEF_REQUIRE_RENDERER_THREAD();
        static_cast<void>(name);
        static_cast<void>(object);
        static_cast<void>(exception);

        CefRefPtr<CefV8Value> first = arguments.empty() ? nullptr : arguments[0];

        switch (mode_) {
            case BridgeHandlerMode::PostMessage:
                send_post_message(browser_, frame_, serialize_v8_value(first));
                retval = CefV8Value::CreateUndefined();
                return true;

            case BridgeHandlerMode::InvokeFunction:
                send_function_call(browser_, frame_, requestId_, arguments);
                retval = CefV8Value::CreateUndefined();
                return true;

            case BridgeHandlerMode::ResolveJsResult:
                send_js_result(browser_, frame_, requestId_, true, serialize_v8_value(first), std::string());
                retval = CefV8Value::CreateUndefined();
                return true;

            case BridgeHandlerMode::RejectJsResult:
                send_js_result(browser_, frame_, requestId_, false, std::string(), serialize_v8_error(first));
                retval = CefV8Value::CreateUndefined();
                return true;
        }

        return false;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<CefFrame> frame_;
    BridgeHandlerMode mode_;
    int32_t requestId_;

    IMPLEMENT_REFCOUNTING(BrowserBridgeV8Handler);
};

void install_registered_function(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefV8Context> context,
                                 const RegisteredFunctionBinding& binding) {
    if (context == nullptr) {
        return;
    }

    CefRefPtr<CefV8Value> global = context->GetGlobal();
    CefRefPtr<CefV8Value> object = ensure_object_path(global, binding.objectName);
    if (object == nullptr || binding.functionName.empty()) {
        return;
    }

    CefRefPtr<CefV8Value> function = CefV8Value::CreateFunction(
        CefString(binding.functionName),
        new BrowserBridgeV8Handler(browser, frame, BridgeHandlerMode::InvokeFunction, binding.bindingId));
    object->SetValue(CefString(binding.functionName), function, V8_PROPERTY_ATTRIBUTE_NONE);
}

class BrowserSubprocessApp final : public CefApp, public CefRenderProcessHandler {
public:
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override {
        static_cast<void>(process_type);
        if (command_line != nullptr && !command_line->HasSwitch("autoplay-policy")) {
            command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        }
    }

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return this;
    }

    void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDictionaryValue> extra_info) override {
        CEF_REQUIRE_RENDERER_THREAD();
        static_cast<void>(extra_info);
        if (browser == nullptr) {
            return;
        }
        browserRefCounts_[browser->GetIdentifier()]++;
    }

    void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_RENDERER_THREAD();
        if (browser == nullptr) {
            return;
        }

        const int browserId = browser->GetIdentifier();
        auto refIt = browserRefCounts_.find(browserId);
        if (refIt == browserRefCounts_.end()) {
            bindingsByBrowserId_.erase(browserId);
            return;
        }

        refIt->second--;
        if (refIt->second <= 0) {
            browserRefCounts_.erase(refIt);
            bindingsByBrowserId_.erase(browserId);
        }
    }

    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        registrar->AddCustomScheme(
            kBrowserCefModScheme,
            CEF_SCHEME_OPTION_STANDARD |
            CEF_SCHEME_OPTION_SECURE |
            CEF_SCHEME_OPTION_CORS_ENABLED |
            CEF_SCHEME_OPTION_FETCH_ENABLED);
    }

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        CEF_REQUIRE_RENDERER_THREAD();
        if (context == nullptr || !context->Enter()) {
            return;
        }

        inject_youtube_ad_patch(frame, context);

        CefRefPtr<CefV8Value> global = context->GetGlobal();
        if (global != nullptr) {
            CefRefPtr<CefV8Value> internalObject = CefV8Value::CreateObject(nullptr, nullptr);
            CefRefPtr<CefV8Value> publicObject = CefV8Value::CreateObject(nullptr, nullptr);
            CefRefPtr<CefV8Value> postMessageFn = CefV8Value::CreateFunction(
                "postMessage",
                new BrowserBridgeV8Handler(browser, frame, BridgeHandlerMode::PostMessage));

            const CefV8Value::PropertyAttribute hiddenAttrs = static_cast<CefV8Value::PropertyAttribute>(
                V8_PROPERTY_ATTRIBUTE_READONLY |
                V8_PROPERTY_ATTRIBUTE_DONTENUM |
                V8_PROPERTY_ATTRIBUTE_DONTDELETE);

            if (internalObject != nullptr) {
                internalObject->SetValue("postMessage", postMessageFn, hiddenAttrs);
                global->SetValue(kBrowserCefInternalBridgeObject, internalObject, hiddenAttrs);
            }

            if (publicObject != nullptr) {
                publicObject->SetValue("postMessage", postMessageFn, hiddenAttrs);
                global->SetValue(kBrowserCefPublicBridgeObject, publicObject, hiddenAttrs);
            }

            const int browserId = browser != nullptr ? browser->GetIdentifier() : 0;
            auto bindingIt = bindingsByBrowserId_.find(browserId);
            if (bindingIt != bindingsByBrowserId_.end()) {
                for (const RegisteredFunctionBinding& binding : bindingIt->second) {
                    install_registered_function(browser, frame, context, binding);
                }
            }
        }

        context->Exit();
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override {
        CEF_REQUIRE_RENDERER_THREAD();

        if (source_process != PID_BROWSER || message == nullptr) {
            return false;
        }

        std::string name = cef_to_utf8(message->GetName());
        if (name == kBrowserCefMessageAddFunction) {
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            RegisteredFunctionBinding binding;
            binding.bindingId = args->GetInt(0);
            binding.objectName = cef_to_utf8(args->GetString(1));
            binding.functionName = cef_to_utf8(args->GetString(2));

            std::vector<RegisteredFunctionBinding>& bindings =
                bindingsByBrowserId_[browser != nullptr ? browser->GetIdentifier() : 0];
            bool updated = false;
            for (RegisteredFunctionBinding& existing : bindings) {
                if (existing.bindingId != binding.bindingId) {
                    continue;
                }
                existing = binding;
                updated = true;
                break;
            }
            if (!updated) {
                bindings.push_back(binding);
            }

            if (frame != nullptr) {
                CefRefPtr<CefV8Context> context = frame->GetV8Context();
                if (context != nullptr && context->IsValid() && context->Enter()) {
                    install_registered_function(browser, frame, context, binding);
                    context->Exit();
                }
            }
            return true;
        }

        if (name != kBrowserCefMessageRunJs) {
            return false;
        }

        CefRefPtr<CefListValue> args = message->GetArgumentList();
        int32_t requestId = args->GetInt(0);
        std::string code = cef_to_utf8(args->GetString(1));

        if (frame == nullptr) {
            send_js_result(browser, frame, requestId, false, std::string(), "JavaScript frame is unavailable");
            return true;
        }

        CefRefPtr<CefV8Context> context = frame->GetV8Context();
        if (context == nullptr || !context->IsValid() || !context->Enter()) {
            send_js_result(browser, frame, requestId, false, std::string(), "JavaScript context is unavailable");
            return true;
        }

        CefRefPtr<CefV8Value> retval;
        CefRefPtr<CefV8Exception> evalException;
        bool evalOk = context->Eval(code, frame->GetURL(), 1, retval, evalException);
        if (!evalOk) {
            context->Exit();
            send_js_result(browser, frame, requestId, false, std::string(), exception_to_string(evalException));
            return true;
        }

        if (requestId == 0) {
            context->Exit();
            return true;
        }

        CefRefPtr<CefV8Value> thenFn =
            (retval != nullptr && (retval->IsObject() || retval->IsFunction())) ? retval->GetValue("then") : nullptr;
        if (thenFn != nullptr && thenFn->IsFunction()) {
            CefV8ValueList callbackArgs;
            callbackArgs.push_back(CefV8Value::CreateFunction(
                "onResolved",
                new BrowserBridgeV8Handler(browser, frame, BridgeHandlerMode::ResolveJsResult, requestId)));
            callbackArgs.push_back(CefV8Value::CreateFunction(
                "onRejected",
                new BrowserBridgeV8Handler(browser, frame, BridgeHandlerMode::RejectJsResult, requestId)));

            CefRefPtr<CefV8Value> promiseResult = thenFn->ExecuteFunction(retval, callbackArgs);
            if (promiseResult == nullptr) {
                context->Exit();
                send_js_result(browser, frame, requestId, false, std::string(), "Failed to attach JavaScript promise handlers");
                return true;
            }

            context->Exit();
            return true;
        }

        std::string result = serialize_v8_value(retval);
        context->Exit();
        send_js_result(browser, frame, requestId, true, result, std::string());
        return true;
    }

    IMPLEMENT_REFCOUNTING(BrowserSubprocessApp);

private:
    std::unordered_map<int, std::vector<RegisteredFunctionBinding>> bindingsByBrowserId_;
    std::unordered_map<int, int> browserRefCounts_;
};

int run_subprocess(const CefMainArgs& mainArgs, void* sandbox_info) {
    CefRefPtr<BrowserSubprocessApp> app = new BrowserSubprocessApp();
    return CefExecuteProcess(mainArgs, app, sandbox_info);
}

} // namespace

#if defined(_WIN32)
extern "C" CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE hInstance,
                                               LPTSTR lpCmdLine,
                                               int nCmdShow,
                                               void* sandbox_info,
                                               cef_version_info_t* version_info) {
    static_cast<void>(lpCmdLine);
    static_cast<void>(nCmdShow);
    static_cast<void>(version_info);
    CefMainArgs mainArgs(hInstance);
    return run_subprocess(mainArgs, sandbox_info);
}

extern "C" CEF_BOOTSTRAP_EXPORT int RunConsoleMain(int argc,
                                                   char* argv[],
                                                   void* sandbox_info,
                                                   cef_version_info_t* version_info) {
    static_cast<void>(argc);
    static_cast<void>(argv);
    static_cast<void>(version_info);
    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    return run_subprocess(mainArgs, sandbox_info);
}
#else
int main(int argc, char* argv[]) {
    CefMainArgs mainArgs(argc, argv);
    return run_subprocess(mainArgs, nullptr);
}
#endif
