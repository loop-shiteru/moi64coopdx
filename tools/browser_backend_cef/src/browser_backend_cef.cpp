#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#else
#include <X11/Xlib.h>
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/cef_audio_handler.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_parser.h"
#include "include/cef_process_message.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_request.h"
#include "include/cef_scheme.h"
#include "include/cef_stream.h"
#include "include/cef_version_info.h"
#include "include/internal/cef_types.h"
#if defined(_WIN32)
#include "include/internal/cef_win.h"
#endif
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include "browser_cef_messages.h"
#include "src/pc/utils/miniaudio.h"
#include "src/pc/browser/browser_backend_api.h"

namespace {

#if defined(_WIN32)
using BrowserProcessId = DWORD;
#define BROWSER_BACKEND_EXPORT __declspec(dllexport)
#else
using BrowserProcessId = uint32_t;
#define BROWSER_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

struct BrowserBridgeState {
    BrowserBackendApiCallbacks callbacks{};
    bool initialized = false;
    bool cefInitialized = false;
#if defined(_WIN32)
    bool comInitialized = false;
#endif
    bool shuttingDown = false;
    int openBrowsers = 0;
    CefRefPtr<CefApp> app;
    std::filesystem::path cachePath;
#if defined(__linux__)
    std::vector<std::string> mainArgStorage;
    std::vector<char*> mainArgv;
#endif
};

BrowserBridgeState gState;
std::filesystem::path gInitTracePath;

constexpr char kBrowserCacheRootDirName[] = "browser_cache";
constexpr char kBrowserCacheInstancePrefix[] = "instance-";
constexpr char kBrowserLogPrefix[] = "browser_cef-";
constexpr char kBrowserLogSuffix[] = ".log";
constexpr char kBrowserInitTracePrefix[] = "browser_cef_init_trace-";
constexpr char kBrowserInitTraceSuffix[] = ".txt";
constexpr char kBrowserRuntimeDirName[] = "cef_resources";
constexpr char kBrowserLocalesDirName[] = "locales";
#if defined(_WIN32)
constexpr char kBrowserSubprocessName[] = "browser_subprocess.exe";
#else
constexpr char kBrowserSubprocessName[] = "browser_subprocess";
#endif

std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(const std::string& path);
void append_init_trace(const std::filesystem::path& tracePath, const char* format, ...);

class BrowserSession;

class BrowserAudioMixer final {
public:
    static constexpr ma_uint32 kSampleRate = 48000;
    static constexpr ma_uint32 kChannels = 2;
    static constexpr ma_uint32 kFramesPerChunk = 512;

    static BrowserAudioMixer& Instance();

    bool Init();
    void Shutdown();
    bool IsReady() const;

    void RegisterSession(CefRefPtr<BrowserSession> session);
    void UnregisterSession(BrowserSession* session);

private:
    static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);
    void Mix(float* pOutput, ma_uint32 frameCount);

    ma_device device_{};
    bool deviceInitialized_ = false;
    bool deviceStarted_ = false;
    std::atomic<bool> ready_{false};
    std::mutex stateMutex_;
    std::mutex sessionsMutex_;
    std::vector<CefRefPtr<BrowserSession>> sessions_;
};

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }

    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        std::string fallback;
        fallback.reserve(value.size());
        for (wchar_t ch : value) {
            fallback.push_back((ch >= 0 && ch <= 0x7F) ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}
#endif

std::filesystem::path get_executable_path() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return std::filesystem::current_path();
        }

        if (written < buffer.size() - 1) {
            buffer.resize(written);
            return std::filesystem::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(4096, '\0');
    for (;;) {
        ssize_t written = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (written < 0) {
            return std::filesystem::current_path();
        }

        if (static_cast<size_t>(written) < buffer.size() - 1) {
            buffer[static_cast<size_t>(written)] = '\0';
            return std::filesystem::path(buffer.data());
        }

        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::filesystem::path get_executable_directory() {
    return get_executable_path().parent_path();
}

std::filesystem::path get_cef_runtime_directory(const std::filesystem::path& executableDirectory) {
    std::error_code ec;
    std::filesystem::path runtimeDirectory = executableDirectory / kBrowserRuntimeDirName;
    if (std::filesystem::exists(runtimeDirectory, ec) && !ec) {
        return runtimeDirectory;
    }
    return executableDirectory;
}

std::string get_process_id_text(BrowserProcessId processId) {
    return std::to_string(processId);
}

BrowserProcessId get_current_process_id() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<BrowserProcessId>(getpid());
#endif
}

std::filesystem::path get_browser_cache_root_path(const std::filesystem::path& executableDirectory) {
    return executableDirectory / kBrowserCacheRootDirName;
}

std::filesystem::path get_browser_cache_path(const std::filesystem::path& executableDirectory, BrowserProcessId processId) {
    return get_browser_cache_root_path(executableDirectory)
        / (std::string(kBrowserCacheInstancePrefix) + get_process_id_text(processId));
}

std::filesystem::path get_browser_log_path(const std::filesystem::path& executableDirectory, BrowserProcessId processId) {
    return executableDirectory / (std::string(kBrowserLogPrefix) + get_process_id_text(processId) + kBrowserLogSuffix);
}

std::filesystem::path get_browser_init_trace_path(const std::filesystem::path& executableDirectory, BrowserProcessId processId) {
    return executableDirectory / (std::string(kBrowserInitTracePrefix) + get_process_id_text(processId) + kBrowserInitTraceSuffix);
}

bool try_parse_process_id(const std::string& text, BrowserProcessId& outProcessId) {
    if (text.empty()) {
        return false;
    }

    uint64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }

        value = (value * 10u) + static_cast<uint64_t>(ch - '0');
        if (value > std::numeric_limits<BrowserProcessId>::max()) {
            return false;
        }
    }

    outProcessId = static_cast<BrowserProcessId>(value);
    return outProcessId != 0;
}

bool try_extract_process_id_from_named_artifact(const std::filesystem::path& artifactPath,
                                                const char* prefix,
                                                const char* suffix,
                                                BrowserProcessId& outProcessId) {
    const std::string fileName = artifactPath.filename().string();
    const std::string prefixText = prefix != nullptr ? prefix : "";
    const std::string suffixText = suffix != nullptr ? suffix : "";

    if (fileName.size() <= (prefixText.size() + suffixText.size())) {
        return false;
    }

    if (fileName.rfind(prefixText, 0) != 0) {
        return false;
    }

    if (!suffixText.empty() && fileName.substr(fileName.size() - suffixText.size()) != suffixText) {
        return false;
    }

    const std::string processIdText = fileName.substr(
        prefixText.size(),
        fileName.size() - prefixText.size() - suffixText.size());
    return try_parse_process_id(processIdText, outProcessId);
}

bool is_process_running(BrowserProcessId processId) {
    if (processId == 0) {
        return false;
    }

#if defined(_WIN32)
    HANDLE processHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (processHandle == nullptr) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }

    DWORD waitResult = WaitForSingleObject(processHandle, 0);
    CloseHandle(processHandle);
    return waitResult != WAIT_OBJECT_0;
#else
    int result = kill(static_cast<pid_t>(processId), 0);
    return result == 0 || errno == EPERM;
#endif
}

void cleanup_stale_pid_files(const std::filesystem::path& directory,
                             const std::filesystem::path& tracePath,
                             BrowserProcessId currentProcessId,
                             const char* prefix,
                             const char* suffix) {
    std::vector<std::pair<std::filesystem::path, BrowserProcessId>> staleFiles;
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(directory, iteratorError);
    if (iteratorError) {
        return;
    }

    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(iteratorError)) {
        if (iteratorError) {
            return;
        }

        std::error_code statusError;
        if (!iterator->is_regular_file(statusError) || statusError) {
            continue;
        }

        BrowserProcessId processId = 0;
        if (!try_extract_process_id_from_named_artifact(iterator->path(), prefix, suffix, processId)) {
            continue;
        }

        if (processId == currentProcessId || is_process_running(processId)) {
            continue;
        }

        staleFiles.emplace_back(iterator->path(), processId);
    }

    for (const auto& [filePath, processId] : staleFiles) {
        std::error_code removeError;
        if (std::filesystem::remove(filePath, removeError)) {
            append_init_trace(tracePath, "cleanup: removed stale file %s pid=%u", path_to_utf8(filePath).c_str(), static_cast<unsigned int>(processId));
        } else if (removeError) {
            append_init_trace(
                tracePath,
                "cleanup: failed to remove stale file %s pid=%u error=%d",
                path_to_utf8(filePath).c_str(),
                static_cast<unsigned int>(processId),
                removeError.value());
        }
    }
}

void cleanup_stale_cache_instances(const std::filesystem::path& cacheRootPath,
                                   const std::filesystem::path& tracePath,
                                   BrowserProcessId currentProcessId) {
    std::vector<std::pair<std::filesystem::path, BrowserProcessId>> staleDirectories;
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(cacheRootPath, iteratorError);
    if (iteratorError) {
        return;
    }

    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(iteratorError)) {
        if (iteratorError) {
            return;
        }

        std::error_code statusError;
        if (!iterator->is_directory(statusError) || statusError) {
            continue;
        }

        BrowserProcessId processId = 0;
        if (!try_extract_process_id_from_named_artifact(iterator->path(), kBrowserCacheInstancePrefix, "", processId)) {
            continue;
        }

        if (processId == currentProcessId || is_process_running(processId)) {
            continue;
        }

        staleDirectories.emplace_back(iterator->path(), processId);
    }

    for (const auto& [directoryPath, processId] : staleDirectories) {
        std::error_code removeError;
        uintmax_t removedCount = std::filesystem::remove_all(directoryPath, removeError);
        if (!removeError) {
            append_init_trace(
                tracePath,
                "cleanup: removed stale cache %s pid=%u entries=%llu",
                path_to_utf8(directoryPath).c_str(),
                static_cast<unsigned int>(processId),
                static_cast<unsigned long long>(removedCount));
        } else {
            append_init_trace(
                tracePath,
                "cleanup: failed to remove stale cache %s pid=%u error=%d",
                path_to_utf8(directoryPath).c_str(),
                static_cast<unsigned int>(processId),
                removeError.value());
        }
    }
}

void cleanup_stale_runtime_artifacts(const std::filesystem::path& executableDirectory,
                                     const std::filesystem::path& tracePath,
                                     BrowserProcessId currentProcessId) {
    const std::filesystem::path cacheRootPath = get_browser_cache_root_path(executableDirectory);
    cleanup_stale_cache_instances(cacheRootPath, tracePath, currentProcessId);
    cleanup_stale_pid_files(executableDirectory, tracePath, currentProcessId, kBrowserLogPrefix, kBrowserLogSuffix);
    cleanup_stale_pid_files(executableDirectory, tracePath, currentProcessId, kBrowserInitTracePrefix, kBrowserInitTraceSuffix);
}

CefString path_to_cef_string(const std::filesystem::path& path) {
#if defined(_WIN32)
    return CefString(path.wstring());
#else
    return CefString(path.string());
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    return wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

std::filesystem::path path_from_utf8(const std::string& path) {
#if defined(_WIN32)
    return std::filesystem::path(utf8_to_wide(path));
#else
    return std::filesystem::path(path);
#endif
}

void set_cef_path(cef_string_t* target, const std::filesystem::path& path) {
    if (target == nullptr) {
        return;
    }

#if defined(_WIN32)
    CefString(target).FromWString(path.wstring());
#else
    CefString(target).FromString(path.string());
#endif
}

void sleep_millis(unsigned int millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

#if defined(__linux__)
int browser_x11_error_handler(Display* display, XErrorEvent* event) {
    static_cast<void>(display);
    static_cast<void>(event);
    return 0;
}

int browser_x11_io_error_handler(Display* display) {
    static_cast<void>(display);
    return 0;
}

bool capture_main_args(std::vector<std::string>& storage, std::vector<char*>& argv) {
    storage.clear();
    argv.clear();

    FILE* file = std::fopen("/proc/self/cmdline", "rb");
    if (file != nullptr) {
        std::vector<char> raw;
        char chunk[512];
        size_t bytesRead = 0;
        while ((bytesRead = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            raw.insert(raw.end(), chunk, chunk + bytesRead);
        }
        std::fclose(file);

        size_t start = 0;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\0') {
                if (i > start) {
                    storage.emplace_back(raw.data() + start, i - start);
                }
                start = i + 1;
            }
        }
        if (start < raw.size()) {
            storage.emplace_back(raw.data() + start, raw.size() - start);
        }
    }

    if (storage.empty()) {
        storage.push_back(path_to_utf8(get_executable_path()));
    }

    argv.reserve(storage.size() + 1);
    for (std::string& entry : storage) {
        argv.push_back(entry.data());
    }
    argv.push_back(nullptr);
    return !storage.empty();
}
#endif

void append_init_trace(const std::filesystem::path& tracePath, const char* format, ...) {
#ifdef _DEBUG
    FILE* file = std::fopen(path_to_utf8(tracePath).c_str(), "a");
    if (file == nullptr) {
        return;
    }

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
#endif
}

std::string cef_to_utf8(const CefString& value) {
    return value.ToString();
}

std::string double_to_string(double value) {
    char buffer[64] = { 0 };
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return std::string(buffer);
}

std::string strip_url_query_and_fragment(const std::string& url) {
    size_t end = url.find_first_of("?#");
    return end == std::string::npos ? url : url.substr(0, end);
}

std::string sanitize_mod_url(const std::string& url) {
    std::string sanitized = strip_url_query_and_fragment(url);
    if (sanitized.rfind("mod://", 0) != 0) {
        return sanitized;
    }

    std::string decoded = cef_to_utf8(CefURIDecode(sanitized.substr(6), true, UU_NORMAL));
    return "mod://" + decoded;
}

bool resolve_url_to_path(int32_t browserId, const std::string& url, std::filesystem::path& outPath) {
    if (gState.callbacks.resolveUrlToPath == nullptr) {
        return false;
    }

    std::string sanitized = sanitize_mod_url(url);
    char pathBuffer[4096] = { 0 };
    if (!gState.callbacks.resolveUrlToPath(browserId, sanitized.c_str(), pathBuffer, static_cast<uint32_t>(sizeof(pathBuffer)))) {
        return false;
    }

    if (pathBuffer[0] == '\0') {
        return false;
    }

    outPath = path_from_utf8(pathBuffer);
    return true;
}

std::string get_mime_type(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (extension == ".html" || extension == ".htm") return "text/html";
    if (extension == ".css") return "text/css";
    if (extension == ".js" || extension == ".mjs") return "text/javascript";
    if (extension == ".json") return "application/json";
    if (extension == ".txt" || extension == ".log") return "text/plain";
    if (extension == ".xml") return "application/xml";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".woff") return "font/woff";
    if (extension == ".woff2") return "font/woff2";
    if (extension == ".ttf") return "font/ttf";
    if (extension == ".otf") return "font/otf";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".ogg") return "audio/ogg";
    if (extension == ".wav") return "audio/wav";
    if (extension == ".mp4") return "video/mp4";
    if (extension == ".webm") return "video/webm";
    return "application/octet-stream";
}

bool validate_mod_url(int32_t browserId, const char* url) {
    if (url == nullptr) {
        return false;
    }

    if (std::strncmp(url, "mod://", 6) != 0) {
        return true;
    }

    std::filesystem::path ignored;
    return resolve_url_to_path(browserId, url, ignored);
}

cef_mouse_button_type_t to_cef_mouse_button(int32_t button) {
    switch (button) {
        case 1: return MBT_MIDDLE;
        case 2: return MBT_RIGHT;
        case 0:
        default:
            return MBT_LEFT;
    }
}

char16_t utf8_first_code_unit(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }

    const unsigned char first = static_cast<unsigned char>(text[0]);
    if (first < 0x80) {
        return static_cast<char16_t>(first);
    }

    uint32_t codePoint = 0;
    int extraBytes = 0;
    if ((first & 0xE0) == 0xC0) {
        codePoint = first & 0x1F;
        extraBytes = 1;
    } else if ((first & 0xF0) == 0xE0) {
        codePoint = first & 0x0F;
        extraBytes = 2;
    } else if ((first & 0xF8) == 0xF0) {
        codePoint = first & 0x07;
        extraBytes = 3;
    } else {
        return static_cast<char16_t>(first);
    }

    for (int i = 0; i < extraBytes; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[i + 1]);
        if ((next & 0xC0) != 0x80) {
            return static_cast<char16_t>(first);
        }
        codePoint = (codePoint << 6) | static_cast<uint32_t>(next & 0x3F);
    }

    if (codePoint <= 0xFFFF) {
        return static_cast<char16_t>(codePoint);
    }

    codePoint -= 0x10000;
    return static_cast<char16_t>(0xD800 + ((codePoint >> 10) & 0x3FF));
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

void append_marshaled_segment(std::string& out, char type, const std::string& data) {
    out.push_back(type);
    out += std::to_string(data.size());
    out.push_back(':');
    out += data;
}

std::string encode_marshaled_arguments(CefRefPtr<CefListValue> arguments) {
    const size_t argumentCount = arguments != nullptr ? arguments->GetSize() : 0;
    std::string encoded = std::to_string(argumentCount);
    encoded.push_back(';');

    for (size_t i = 0; i < argumentCount; ++i) {
        CefRefPtr<CefListValue> arg = arguments->GetList(i);
        if (arg == nullptr || arg->GetSize() < 1) {
            append_marshaled_segment(encoded, 'z', std::string());
            continue;
        }

        MarshaledValueType type = static_cast<MarshaledValueType>(arg->GetInt(0));
        switch (type) {
            case MarshaledValueType::Null:
                append_marshaled_segment(encoded, 'z', std::string());
                break;
            case MarshaledValueType::Boolean:
                append_marshaled_segment(encoded, 'b', arg->GetBool(1) ? "1" : "0");
                break;
            case MarshaledValueType::Int:
                append_marshaled_segment(encoded, 'i', std::to_string(arg->GetInt(1)));
                break;
            case MarshaledValueType::Double:
                append_marshaled_segment(encoded, 'n', double_to_string(arg->GetDouble(1)));
                break;
            case MarshaledValueType::String:
                append_marshaled_segment(encoded, 's', cef_to_utf8(arg->GetString(1)));
                break;
            case MarshaledValueType::Json:
                append_marshaled_segment(encoded, 'j', cef_to_utf8(arg->GetString(1)));
                break;
            default:
                append_marshaled_segment(encoded, 'z', std::string());
                break;
        }
    }

    return encoded;
}

class ModSchemeHandlerFactory final : public CefSchemeHandlerFactory {
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         const CefString& scheme_name,
                                         CefRefPtr<CefRequest> request) override {
        CEF_REQUIRE_IO_THREAD();
        static_cast<void>(browser);
        static_cast<void>(frame);
        static_cast<void>(scheme_name);

        std::filesystem::path resolvedPath;
        int32_t browserId = browser != nullptr ? browser->GetIdentifier() : 0;
        if (!resolve_url_to_path(browserId, cef_to_utf8(request->GetURL()), resolvedPath)) {
            return nullptr;
        }

        std::error_code ec;
        if (std::filesystem::is_directory(resolvedPath, ec)) {
            resolvedPath /= "index.html";
        }

        if (ec || !std::filesystem::exists(resolvedPath) || !std::filesystem::is_regular_file(resolvedPath)) {
            return nullptr;
        }

        CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForFile(path_to_cef_string(resolvedPath));
        if (stream == nullptr) {
            return nullptr;
        }

        return new CefStreamResourceHandler(CefString(get_mime_type(resolvedPath)), stream);
    }

    IMPLEMENT_REFCOUNTING(ModSchemeHandlerFactory);
};

class BrowserProcessApp final : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override {
        if (command_line != nullptr) {
#if defined(__linux__)
            command_line->RemoveSwitch("enable-crash-reporter");
            if (!command_line->HasSwitch("disable-crash-reporter")) {
                command_line->AppendSwitch("disable-crash-reporter");
            }
            
            if (!command_line->HasSwitch("disable-vulkan")) {
                command_line->AppendSwitch("disable-vulkan");
            }
            
            if (!command_line->HasSwitch("disable-features")) {
                command_line->AppendSwitchWithValue("disable-features", "OptimizationGuideOnDeviceModel");
            } else {
                std::string disabledFeatures = cef_to_utf8(command_line->GetSwitchValue("disable-features"));
                if (disabledFeatures.find("OptimizationGuideOnDeviceModel") == std::string::npos) {
                    disabledFeatures += ",OptimizationGuideOnDeviceModel";
                    command_line->AppendSwitchWithValue("disable-features", disabledFeatures);
                }
            }
#endif
            if (!command_line->HasSwitch("autoplay-policy")) {
                command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
            }
        }

        append_init_trace(
            gInitTracePath,
            "browser_app: OnBeforeCommandLineProcessing type=%s cmd=%s",
            cef_to_utf8(process_type).c_str(),
            command_line != nullptr ? cef_to_utf8(command_line->GetCommandLineString()).c_str() : "(null)");
    }

    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        append_init_trace(gInitTracePath, "browser_app: OnRegisterCustomSchemes");
        registrar->AddCustomScheme(
            kBrowserCefModScheme,
            CEF_SCHEME_OPTION_STANDARD |
            CEF_SCHEME_OPTION_SECURE |
            CEF_SCHEME_OPTION_CORS_ENABLED |
            CEF_SCHEME_OPTION_FETCH_ENABLED);
    }

    void OnContextInitialized() override {
        CEF_REQUIRE_UI_THREAD();
        bool registered =
            CefRegisterSchemeHandlerFactory(kBrowserCefModScheme, CefString(), new ModSchemeHandlerFactory());
        append_init_trace(gInitTracePath, "browser_app: OnContextInitialized register_mod_scheme=%d", registered ? 1 : 0);
    }

    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                     const CefString& current_directory) override {
        append_init_trace(
            gInitTracePath,
            "browser_app: OnAlreadyRunningAppRelaunch cwd=%s cmd=%s",
            cef_to_utf8(current_directory).c_str(),
            command_line != nullptr ? cef_to_utf8(command_line->GetCommandLineString()).c_str() : "(null)");
        return false;
    }

    IMPLEMENT_REFCOUNTING(BrowserProcessApp);
};

class BrowserSession final : public CefClient,
                             public CefAudioHandler,
                             public CefDisplayHandler,
                             public CefLifeSpanHandler,
                             public CefLoadHandler,
                             public CefRequestHandler,
                             public CefRenderHandler {
public:
    BrowserSession(BrowserBackendApiSurface surface, const BrowserBackendApiCreateParams& params)
        : browserId_(params.browserId),
          transparent_(params.transparent != 0),
          audioEnabled_(params.audio != 0),
          surface_(surface) {
    }

    void AttachBrowser(CefRefPtr<CefBrowser> browser) {
        browser_ = browser;
        AttachToAudioMixer();
        ApplyAudioState();
        if (browser_ != nullptr && browser_->GetHost() != nullptr) {
            browser_->GetHost()->SetFocus(true);
        }
    }

    void Close() {
        CEF_REQUIRE_UI_THREAD();
        if (closing_) {
            return;
        }
        closing_ = true;
        surface_.pixels = nullptr;
        DetachFromAudioMixer();

        if (browser_ != nullptr && browser_->GetHost() != nullptr) {
            browser_->StopLoad();
            browser_->GetHost()->SetAudioMuted(true);
            browser_->GetHost()->CloseBrowser(true);
            return;
        }

        closed_ = true;
    }

    bool IsClosed() const {
        return closed_;
    }

    bool OpenUrl(const char* url) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || browser_->GetMainFrame() == nullptr || !validate_mod_url(browserId_, url)) {
            return false;
        }

        browser_->GetMainFrame()->LoadURL(url);
        return true;
    }

    bool Reload() {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr) { return false; }
        browser_->Reload();
        return true;
    }

    bool Stop() {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr) { return false; }
        browser_->StopLoad();
        return true;
    }

    bool GoBack() {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || !browser_->CanGoBack()) { return false; }
        browser_->GoBack();
        return true;
    }

    bool GoForward() {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || !browser_->CanGoForward()) { return false; }
        browser_->GoForward();
        return true;
    }

    bool RunJs(const char* code, int32_t requestId) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || browser_->GetMainFrame() == nullptr || code == nullptr) {
            return false;
        }

        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBrowserCefMessageRunJs);
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        args->SetInt(0, requestId);
        args->SetString(1, CefString(code));
        browser_->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
        return true;
    }

    bool AddFunction(int32_t bindingId, const char* objectName, const char* functionName) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || bindingId == 0 || objectName == nullptr || functionName == nullptr) {
            return false;
        }
        if (objectName[0] == '\0' || functionName[0] == '\0') {
            return false;
        }

        RegisteredFunctionBinding binding;
        binding.bindingId = bindingId;
        binding.objectName = objectName;
        binding.functionName = functionName;

        bool updated = false;
        for (RegisteredFunctionBinding& existing : functionBindings_) {
            if (existing.bindingId != bindingId) {
                continue;
            }
            existing = binding;
            updated = true;
            break;
        }
        if (!updated) {
            functionBindings_.push_back(binding);
        }

        SyncFunctionBinding(binding);
        return true;
    }

    bool SetVolume(float volume) {
        CEF_REQUIRE_UI_THREAD();
        volume_.store(volume, std::memory_order_relaxed);
        if (IsTearingDown()) {
            return true;
        }
        ApplyAudioState();
        return true;
    }

    bool SetMuted(bool muted) {
        CEF_REQUIRE_UI_THREAD();
        muted_.store(muted, std::memory_order_relaxed);
        if (IsTearingDown()) {
            return true;
        }
        ApplyAudioState();
        return true;
    }

    bool Resize(BrowserBackendApiSurface surface) {
        CEF_REQUIRE_UI_THREAD();
        surface_ = surface;
        if (IsTearingDown()) {
            return true;
        }
        if (browser_ != nullptr && browser_->GetHost() != nullptr) {
            browser_->GetHost()->WasResized();
            browser_->GetHost()->Invalidate(PET_VIEW);
        }
        return true;
    }

    bool SendMouseMove(int32_t x, int32_t y, uint32_t modifiers) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || browser_->GetHost() == nullptr) { return false; }

        CefMouseEvent event;
        event.x = x;
        event.y = y;
        event.modifiers = modifiers;
        browser_->GetHost()->SendMouseMoveEvent(event, false);
        return true;
    }

    bool SendMouseButton(int32_t x, int32_t y, int32_t button, bool down, int32_t clickCount, uint32_t modifiers) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || browser_->GetHost() == nullptr) { return false; }

        CefMouseEvent event;
        event.x = x;
        event.y = y;
        event.modifiers = modifiers;
        browser_->GetHost()->SetFocus(true);
        browser_->GetHost()->SendMouseClickEvent(event, to_cef_mouse_button(button), !down, clickCount);
        return true;
    }

    bool SendMouseWheel(int32_t x, int32_t y, int32_t deltaX, int32_t deltaY, uint32_t modifiers) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || browser_->GetHost() == nullptr) { return false; }

        CefMouseEvent event;
        event.x = x;
        event.y = y;
        event.modifiers = modifiers;
        browser_->GetHost()->SendMouseWheelEvent(event, deltaX, deltaY);
        return true;
    }

    bool SendKey(int32_t eventType, int32_t keyCode, int32_t nativeCode, uint32_t modifiers, const char* text) {
        CEF_REQUIRE_UI_THREAD();
        if (IsTearingDown() || browser_ == nullptr || browser_->GetHost() == nullptr) { return false; }

        CefKeyEvent event = {};
        event.type = static_cast<cef_key_event_type_t>(eventType);
        event.modifiers = modifiers;
        event.windows_key_code = keyCode;
        event.native_key_code = nativeCode;
        event.is_system_key = (modifiers & EVENTFLAG_ALT_DOWN) != 0;
        event.character = utf8_first_code_unit(text);
        event.unmodified_character = event.character;
        event.focus_on_editable_field = 1;

        browser_->GetHost()->SetFocus(true);
        browser_->GetHost()->SendKeyEvent(event);
        return true;
    }

    void MixAudio(float* output, ma_uint32 frameCount) {
        if (output == nullptr || frameCount == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(audioMutex_);
        if (audioBuffer_.empty() || audioBufferedFrames_ == 0) {
            return;
        }

        const float gain = std::clamp(volume_.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const bool audible = !muted_.load(std::memory_order_relaxed) && gain > 0.0001f;
        const ma_uint32 framesToProcess = std::min<ma_uint32>(frameCount, static_cast<ma_uint32>(audioBufferedFrames_));

        for (ma_uint32 i = 0; i < framesToProcess; ++i) {
            const size_t srcOffset = audioReadFrame_ * BrowserAudioMixer::kChannels;
            const float left = audioBuffer_[srcOffset + 0];
            const float right = audioBuffer_[srcOffset + 1];

            if (audible) {
                output[(i * BrowserAudioMixer::kChannels) + 0] += left * gain;
                output[(i * BrowserAudioMixer::kChannels) + 1] += right * gain;
            }

            audioReadFrame_ = (audioReadFrame_ + 1) % kAudioBufferFrames;
            audioBufferedFrames_--;
        }

        if (!streamStarted_ && audioBufferedFrames_ == 0) {
            audioReadFrame_ = 0;
            audioWriteFrame_ = 0;
        }
    }

    CefRefPtr<CefAudioHandler> GetAudioHandler() override {
        if (!audioEnabled_ || !BrowserAudioMixer::Instance().IsReady()) {
            return nullptr;
        }
        return this;
    }

    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    CefRefPtr<CefRequestHandler> GetRequestHandler() override {
        return this;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return this;
    }

    bool GetAudioParameters(CefRefPtr<CefBrowser> browser, CefAudioParameters& params) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);

        if (!audioEnabled_ || !BrowserAudioMixer::Instance().IsReady()) {
            return false;
        }

        params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
        params.sample_rate = static_cast<int>(BrowserAudioMixer::kSampleRate);
        params.frames_per_buffer = static_cast<int>(BrowserAudioMixer::kFramesPerChunk);
        return true;
    }

    void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                              const CefAudioParameters& params,
                              int channels) override {
        static_cast<void>(browser);
        static_cast<void>(params);

        std::lock_guard<std::mutex> lock(audioMutex_);
        sourceChannels_ = std::max(channels, 1);
        streamStarted_ = true;
        ResetAudioBufferLocked();
    }

    void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser,
                             const float** data,
                             int frames,
                             int64_t pts) override {
        static_cast<void>(browser);
        static_cast<void>(pts);

        if (data == nullptr || frames <= 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(audioMutex_);
        if (!streamStarted_) {
            return;
        }

        WriteAudioPacketLocked(data, sourceChannels_, frames);
    }

    void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);

        std::lock_guard<std::mutex> lock(audioMutex_);
        streamStarted_ = false;
        ResetAudioBufferLocked();
    }

    void OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString& message) override {
        static_cast<void>(browser);
        static_cast<void>(message);

        std::lock_guard<std::mutex> lock(audioMutex_);
        streamStarted_ = false;
        ResetAudioBufferLocked();
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        static_cast<void>(frame);

        if (source_process != PID_RENDERER || message == nullptr) {
            return false;
        }
        if (IsTearingDown()) {
            return true;
        }

        std::string name = cef_to_utf8(message->GetName());
        CefRefPtr<CefListValue> args = message->GetArgumentList();

        if (name == kBrowserCefMessagePostMessage) {
            if (gState.callbacks.notifyMessage != nullptr) {
                std::string payload = cef_to_utf8(args->GetString(0));
                gState.callbacks.notifyMessage(browserId_, payload.c_str());
            }
            return true;
        }

        if (name == kBrowserCefMessageCallFunction) {
            if (gState.callbacks.notifyFunctionCall != nullptr) {
                std::string payload = encode_marshaled_arguments(args->GetList(1));
                gState.callbacks.notifyFunctionCall(browserId_, args->GetInt(0), payload.c_str());
            }
            return true;
        }

        if (name == kBrowserCefMessageJsResult) {
            if (gState.callbacks.notifyJsResult != nullptr) {
                std::string result = cef_to_utf8(args->GetString(2));
                std::string error = cef_to_utf8(args->GetString(3));
                gState.callbacks.notifyJsResult(
                    browserId_,
                    args->GetInt(0),
                    args->GetBool(1) ? 1 : 0,
                    result.c_str(),
                    error.c_str());
            }
            return true;
        }

        return false;
    }

    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        static_cast<void>(source);
        static_cast<void>(line);

        if (!IsTearingDown() && gState.callbacks.notifyConsole != nullptr) {
            std::string text = cef_to_utf8(message);
            gState.callbacks.notifyConsole(browserId_, text.c_str(), static_cast<int32_t>(level));
        }
        return false;
    }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool user_gesture,
                        bool is_redirect) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        static_cast<void>(frame);
        static_cast<void>(user_gesture);
        static_cast<void>(is_redirect);

        if (IsTearingDown() || request == nullptr || gState.callbacks.shouldLoadUrl == nullptr) {
            return false;
        }

        std::string url = cef_to_utf8(request->GetURL());
        return gState.callbacks.shouldLoadUrl(browserId_, url.c_str()) == 0;
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int popup_id,
                       const CefString& target_url,
                       const CefString& target_frame_name,
                       cef_window_open_disposition_t target_disposition,
                       bool user_gesture,
                       const CefPopupFeatures& popupFeatures,
                       CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extra_info,
                       bool* no_javascript_access) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        static_cast<void>(frame);
        static_cast<void>(popup_id);
        static_cast<void>(target_url);
        static_cast<void>(target_frame_name);
        static_cast<void>(target_disposition);
        static_cast<void>(user_gesture);
        static_cast<void>(popupFeatures);
        static_cast<void>(windowInfo);
        static_cast<void>(client);
        static_cast<void>(settings);
        static_cast<void>(extra_info);
        static_cast<void>(no_javascript_access);
        return true;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_UI_THREAD();
        if (browser_ == nullptr) {
            AttachBrowser(browser);
        }
        SyncAllFunctionBindings();
        gState.openBrowsers++;
        if (IsTearingDown() && browser_ != nullptr && browser_->GetHost() != nullptr) {
            browser_->GetHost()->CloseBrowser(true);
        }
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        CEF_REQUIRE_UI_THREAD();
        DetachFromAudioMixer();
        if (browser_ != nullptr && browser_->GetIdentifier() == browser->GetIdentifier()) {
            browser_ = nullptr;
        }
        surface_.pixels = nullptr;
        closed_ = true;
        gState.openBrowsers = std::max(0, gState.openBrowsers - 1);
    }

    void OnLoadStart(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     TransitionType transition_type) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        static_cast<void>(transition_type);

        if (IsTearingDown() || frame == nullptr || !frame->IsMain()) {
            return;
        }

        SyncAllFunctionBindings();
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        static_cast<void>(httpStatusCode);

        if (!frame->IsMain()) {
            return;
        }

        SyncAllFunctionBindings();
        if (IsTearingDown() || gState.callbacks.notifyLoad == nullptr) {
            return;
        }
        std::string url = cef_to_utf8(frame->GetURL());
        gState.callbacks.notifyLoad(browserId_, url.c_str());
    }

    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode errorCode,
                     const CefString& errorText,
                     const CefString& failedUrl) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);

        if (!frame->IsMain() || errorCode == ERR_ABORTED || IsTearingDown() || gState.callbacks.notifyError == nullptr) {
            return;
        }

        std::string message = cef_to_utf8(errorText);
        std::string details = cef_to_utf8(failedUrl);
        gState.callbacks.notifyError(browserId_, message.c_str(), details.c_str());
    }

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);
        rect = CefRect(
            0,
            0,
            static_cast<int>(std::max<uint32_t>(1u, surface_.width)),
            static_cast<int>(std::max<uint32_t>(1u, surface_.height)));
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);

        screen_info.device_scale_factor = 1.0f;
        screen_info.depth = 32;
        screen_info.depth_per_component = 8;
        screen_info.is_monochrome = false;
        screen_info.rect = CefRect(
            0,
            0,
            static_cast<int>(std::max<uint32_t>(1u, surface_.width)),
            static_cast<int>(std::max<uint32_t>(1u, surface_.height)));
        screen_info.available_rect = screen_info.rect;
        return true;
    }

    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType type,
                 const RectList& dirtyRects,
                 const void* buffer,
                 int width,
                 int height) override {
        CEF_REQUIRE_UI_THREAD();
        static_cast<void>(browser);

        if (IsTearingDown() || type != PET_VIEW || surface_.pixels == nullptr || buffer == nullptr || width <= 0 || height <= 0) {
            return;
        }

        const uint8_t* src = static_cast<const uint8_t*>(buffer);
        const int maxWidth = std::min<int>(width, static_cast<int>(surface_.surfaceWidth));
        const int maxHeight = std::min<int>(height, static_cast<int>(surface_.surfaceHeight));

        RectList rects = dirtyRects;
        if (rects.empty()) {
            rects.push_back(CefRect(0, 0, maxWidth, maxHeight));
        }

        for (const CefRect& rect : rects) {
            const int left = std::max(0, rect.x);
            const int top = std::max(0, rect.y);
            const int right = std::min(maxWidth, rect.x + rect.width);
            const int bottom = std::min(maxHeight, rect.y + rect.height);

            for (int y = top; y < bottom; y++) {
                const uint8_t* srcRow = src + ((static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(left)) * 4;
                uint8_t* dstRow = surface_.pixels + ((static_cast<size_t>(y) * static_cast<size_t>(surface_.surfaceWidth)) + static_cast<size_t>(left)) * 4;

                for (int x = left; x < right; x++) {
                    dstRow[0] = srcRow[2];
                    dstRow[1] = srcRow[1];
                    dstRow[2] = srcRow[0];
                    dstRow[3] = srcRow[3];
                    dstRow += 4;
                    srcRow += 4;
                }
            }
        }

        if (!IsTearingDown() && gState.callbacks.notifyPaint != nullptr) {
            gState.callbacks.notifyPaint(browserId_, width, height);
        }
    }

private:
    bool IsTearingDown() const {
        return closing_ || gState.shuttingDown;
    }

    void SyncFunctionBinding(const RegisteredFunctionBinding& binding) {
        if (IsTearingDown() || browser_ == nullptr || browser_->GetMainFrame() == nullptr) {
            return;
        }

        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBrowserCefMessageAddFunction);
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        args->SetInt(0, binding.bindingId);
        args->SetString(1, CefString(binding.objectName));
        args->SetString(2, CefString(binding.functionName));
        browser_->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
    }

    void SyncAllFunctionBindings() {
        if (IsTearingDown()) {
            return;
        }
        for (const RegisteredFunctionBinding& binding : functionBindings_) {
            SyncFunctionBinding(binding);
        }
    }

    void AttachToAudioMixer() {
        if (!audioEnabled_ || audioRegistered_ || !BrowserAudioMixer::Instance().IsReady()) {
            return;
        }

        BrowserAudioMixer::Instance().RegisterSession(CefRefPtr<BrowserSession>(this));
        audioRegistered_ = true;
    }

    void DetachFromAudioMixer() {
        if (!audioRegistered_) {
            return;
        }

        BrowserAudioMixer::Instance().UnregisterSession(this);
        audioRegistered_ = false;

        std::lock_guard<std::mutex> lock(audioMutex_);
        streamStarted_ = false;
        ResetAudioBufferLocked();
    }

    void ResetAudioBufferLocked() {
        if (audioBuffer_.empty()) {
            audioBuffer_.resize(kAudioBufferFrames * BrowserAudioMixer::kChannels, 0.0f);
        }

        audioReadFrame_ = 0;
        audioWriteFrame_ = 0;
        audioBufferedFrames_ = 0;
    }

    void WriteAudioPacketLocked(const float** data, int channels, int frames) {
        if (data == nullptr || channels <= 0 || frames <= 0) {
            return;
        }

        if (audioBuffer_.empty()) {
            audioBuffer_.resize(kAudioBufferFrames * BrowserAudioMixer::kChannels, 0.0f);
        }

        int firstFrame = std::max(0, frames - static_cast<int>(kAudioBufferFrames));
        for (int frameIndex = firstFrame; frameIndex < frames; ++frameIndex) {
            float left = data[0][frameIndex];
            float right = left;

            if (channels >= 2 && data[1] != nullptr) {
                right = data[1][frameIndex];
            }

            if (audioBufferedFrames_ == kAudioBufferFrames) {
                audioReadFrame_ = (audioReadFrame_ + 1) % kAudioBufferFrames;
                audioBufferedFrames_--;
            }

            const size_t dstOffset = audioWriteFrame_ * BrowserAudioMixer::kChannels;
            audioBuffer_[dstOffset + 0] = left;
            audioBuffer_[dstOffset + 1] = right;
            audioWriteFrame_ = (audioWriteFrame_ + 1) % kAudioBufferFrames;
            audioBufferedFrames_++;
        }
    }

    void ApplyAudioState() {
        if (IsTearingDown() || browser_ == nullptr || browser_->GetHost() == nullptr) {
            return;
        }

        const bool usingNativeMixer = audioEnabled_ && audioRegistered_ && BrowserAudioMixer::Instance().IsReady();
        if (usingNativeMixer) {
            browser_->GetHost()->SetAudioMuted(true);
            return;
        }

        const float clampedVolume = std::clamp(volume_.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const bool muted = muted_.load(std::memory_order_relaxed);
        browser_->GetHost()->SetAudioMuted(!audioEnabled_ || muted || clampedVolume <= 0.0001f);
    }

    const int32_t browserId_;
    const bool transparent_;
    const bool audioEnabled_;
    static constexpr size_t kAudioBufferFrames = static_cast<size_t>(BrowserAudioMixer::kSampleRate) * 2;
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    bool audioRegistered_ = false;
    bool closing_ = false;
    bool closed_ = false;
    BrowserBackendApiSurface surface_{};
    CefRefPtr<CefBrowser> browser_;
    std::vector<RegisteredFunctionBinding> functionBindings_;
    std::mutex audioMutex_;
    std::vector<float> audioBuffer_;
    size_t audioReadFrame_ = 0;
    size_t audioWriteFrame_ = 0;
    size_t audioBufferedFrames_ = 0;
    int sourceChannels_ = 2;
    bool streamStarted_ = false;

    IMPLEMENT_REFCOUNTING(BrowserSession);
};

BrowserAudioMixer& BrowserAudioMixer::Instance() {
    static BrowserAudioMixer mixer;
    return mixer;
}

bool BrowserAudioMixer::Init() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (ready_.load(std::memory_order_relaxed)) {
        return true;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kSampleRate;
    config.periodSizeInFrames = kFramesPerChunk;
    config.dataCallback = DataCallback;
    config.pUserData = this;

    if (ma_device_init(NULL, &config, &device_) != MA_SUCCESS) {
        append_init_trace(gInitTracePath, "browser_audio: ma_device_init failed");
        return false;
    }

    deviceInitialized_ = true;

    if (ma_device_start(&device_) != MA_SUCCESS) {
        append_init_trace(gInitTracePath, "browser_audio: ma_device_start failed");
        ma_device_uninit(&device_);
        deviceInitialized_ = false;
        return false;
    }

    deviceStarted_ = true;
    ready_.store(true, std::memory_order_relaxed);
    return true;
}

void BrowserAudioMixer::Shutdown() {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    ready_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> sessionsLock(sessionsMutex_);
        sessions_.clear();
    }

    if (deviceInitialized_) {
        deviceInitialized_ = false;
        deviceStarted_ = false;
        ma_device_uninit(&device_);
    } else {
        deviceStarted_ = false;
    }
}

bool BrowserAudioMixer::IsReady() const {
    return ready_.load(std::memory_order_relaxed);
}

void BrowserAudioMixer::RegisterSession(CefRefPtr<BrowserSession> session) {
    if (session == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const CefRefPtr<BrowserSession>& existing : sessions_) {
        if (existing.get() == session.get()) {
            return;
        }
    }
    sessions_.push_back(session);
}

void BrowserAudioMixer::UnregisterSession(BrowserSession* session) {
    if (session == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_.erase(
        std::remove_if(
            sessions_.begin(),
            sessions_.end(),
            [session](const CefRefPtr<BrowserSession>& existing) {
                return existing.get() == session;
            }),
        sessions_.end());
}

void BrowserAudioMixer::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    static_cast<void>(pInput);

    if (pDevice == nullptr || pOutput == nullptr) {
        return;
    }

    BrowserAudioMixer* mixer = static_cast<BrowserAudioMixer*>(pDevice->pUserData);
    if (mixer == nullptr) {
        return;
    }

    mixer->Mix(static_cast<float*>(pOutput), frameCount);
}

void BrowserAudioMixer::Mix(float* pOutput, ma_uint32 frameCount) {
    if (pOutput == nullptr || frameCount == 0) {
        return;
    }

    const size_t sampleCount = static_cast<size_t>(frameCount) * kChannels;
    std::fill_n(pOutput, sampleCount, 0.0f);

    if (!ready_.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const CefRefPtr<BrowserSession>& session : sessions_) {
        if (session != nullptr) {
            session->MixAudio(pOutput, frameCount);
        }
    }

    for (size_t i = 0; i < sampleCount; ++i) {
        pOutput[i] = std::clamp(pOutput[i], -1.0f, 1.0f);
    }
}

struct BrowserHandle {
    CefRefPtr<BrowserSession> session;
};

int32_t backend_init(const BrowserBackendApiCallbacks* callbacks) {
    if (gState.initialized) {
        return 1;
    }

    if (callbacks != nullptr) {
        gState.callbacks = *callbacks;
    } else {
        std::memset(&gState.callbacks, 0, sizeof(gState.callbacks));
    }
    gState.shuttingDown = false;

    std::filesystem::path executableDirectory = get_executable_directory();
    std::filesystem::path runtimeDirectory = get_cef_runtime_directory(executableDirectory);
    std::filesystem::path subprocessPath = runtimeDirectory / kBrowserSubprocessName;
    BrowserProcessId processId = get_current_process_id();
    std::filesystem::path tracePath = get_browser_init_trace_path(executableDirectory, processId);
    std::filesystem::path resourcesPath = runtimeDirectory;
    std::filesystem::path localesPath = runtimeDirectory / kBrowserLocalesDirName;
    std::filesystem::path cacheRootPath = get_browser_cache_root_path(executableDirectory);
    std::filesystem::path cachePath = get_browser_cache_path(executableDirectory, processId);
    std::filesystem::path logPath = get_browser_log_path(executableDirectory, processId);
    gInitTracePath = tracePath;
    cleanup_stale_runtime_artifacts(executableDirectory, tracePath, processId);

    std::error_code cacheError;
    std::filesystem::create_directories(cacheRootPath, cacheError);
    std::filesystem::create_directories(cachePath, cacheError);

#if defined(_WIN32)
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    gState.comInitialized = (comResult == S_OK || comResult == S_FALSE);

    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
#else
    XSetErrorHandler(browser_x11_error_handler);
    XSetIOErrorHandler(browser_x11_io_error_handler);
    capture_main_args(gState.mainArgStorage, gState.mainArgv);

    CefMainArgs mainArgs(
        static_cast<int>(gState.mainArgv.empty() ? 0 : gState.mainArgv.size() - 1),
        gState.mainArgv.empty() ? nullptr : gState.mainArgv.data());
#endif
    CefSettings settings = {};
    settings.no_sandbox = true;
    settings.external_message_pump = true;
    settings.windowless_rendering_enabled = true;
    settings.command_line_args_disabled = false;
    set_cef_path(&settings.browser_subprocess_path, subprocessPath);
    set_cef_path(&settings.resources_dir_path, resourcesPath);
    set_cef_path(&settings.locales_dir_path, localesPath);
    set_cef_path(&settings.root_cache_path, cachePath);
    set_cef_path(&settings.cache_path, cachePath);
    set_cef_path(&settings.log_file, logPath);
    CefString(&settings.locale).FromASCII("en-US");
    settings.log_severity = LOGSEVERITY_DEFAULT;

    gState.app = new BrowserProcessApp();
    int executeProcessResult = CefExecuteProcess(mainArgs, gState.app, nullptr);
    append_init_trace(tracePath, "backend_init: CefExecuteProcess=%d", executeProcessResult);
    if (executeProcessResult >= 0) {
        gState.app = nullptr;
#if defined(_WIN32)
        if (gState.comInitialized) {
            CoUninitialize();
            gState.comInitialized = false;
        }
#endif
        return 0;
    }
    gState.cefInitialized = CefInitialize(mainArgs, settings, gState.app, nullptr);
    append_init_trace(tracePath, "backend_init: CefInitialize=%d exit_code=%d", gState.cefInitialized ? 1 : 0, CefGetExitCode());
    gState.initialized = gState.cefInitialized;
    gState.openBrowsers = 0;
    gState.cachePath = gState.cefInitialized ? cachePath : std::filesystem::path();

    if (gState.cefInitialized) {
        BrowserAudioMixer::Instance().Init();
    }

    if (!gState.cefInitialized) {
        gState.app = nullptr;
#if defined(_WIN32)
        if (gState.comInitialized) {
            CoUninitialize();
            gState.comInitialized = false;
        }
#endif
    }

    return gState.cefInitialized ? 1 : 0;
}

void backend_tick(void) {
    if (gState.cefInitialized) {
        CefDoMessageLoopWork();
    }
}

void backend_shutdown(void) {
    if (!gState.initialized) {
        return;
    }

    gState.shuttingDown = true;
    if (gState.cefInitialized) {
        for (int i = 0; i < 200 && gState.openBrowsers > 0; ++i) {
            CefDoMessageLoopWork();
            sleep_millis(10);
        }
        CefClearSchemeHandlerFactories();
        CefShutdown();
    }
    BrowserAudioMixer::Instance().Shutdown();

    std::memset(&gState.callbacks, 0, sizeof(gState.callbacks));
    gState.app = nullptr;
    gState.initialized = false;
    gState.cefInitialized = false;
    gState.shuttingDown = false;
    gState.openBrowsers = 0;
    gState.cachePath.clear();
#if defined(__linux__)
    gState.mainArgStorage.clear();
    gState.mainArgv.clear();
#endif
#if defined(_WIN32)
    if (gState.comInitialized) {
        CoUninitialize();
        gState.comInitialized = false;
    }
#endif
}

int32_t backend_available(void) {
    return gState.cefInitialized ? 1 : 0;
}

void* backend_create(BrowserBackendApiSurface* surface, const BrowserBackendApiCreateParams* params) {
    CEF_REQUIRE_UI_THREAD();
    if (!gState.cefInitialized || surface == nullptr || params == nullptr) {
        return nullptr;
    }

    CefWindowInfo windowInfo;
    windowInfo.SetAsWindowless(kNullWindowHandle);

    CefBrowserSettings settings = {};
    settings.windowless_frame_rate = 60;
    settings.background_color = params->transparent
        ? CefColorSetARGB(0x00, 0x00, 0x00, 0x00)
        : CefColorSetARGB(0xFF, 0xFF, 0xFF, 0xFF);

    CefRefPtr<BrowserSession> session = new BrowserSession(*surface, *params);
    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        windowInfo,
        session,
        CefString("about:blank"),
        settings,
        nullptr,
        nullptr);

    if (browser == nullptr) {
        return nullptr;
    }

    session->AttachBrowser(browser);
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);

    BrowserHandle* handle = new BrowserHandle();
    handle->session = session;
    return handle;
}

void backend_destroy(void* browser) {
    CEF_REQUIRE_UI_THREAD();
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    if (handle == nullptr) {
        return;
    }

    if (handle->session != nullptr) {
        CefRefPtr<BrowserSession> session = handle->session;
        session->Close();
        if (gState.cefInitialized) {
            for (int i = 0; i < 200 && !session->IsClosed(); ++i) {
                CefDoMessageLoopWork();
                sleep_millis(10);
            }
        }
        handle->session = nullptr;
    }

    delete handle;
}

int32_t backend_open_url(void* browser, const char* url) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->OpenUrl(url)) ? 1 : 0;
}

int32_t backend_reload(void* browser) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->Reload()) ? 1 : 0;
}

int32_t backend_stop(void* browser) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->Stop()) ? 1 : 0;
}

int32_t backend_go_back(void* browser) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->GoBack()) ? 1 : 0;
}

int32_t backend_go_forward(void* browser) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->GoForward()) ? 1 : 0;
}

int32_t backend_run_js(void* browser, const char* code, int32_t requestId) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->RunJs(code, requestId)) ? 1 : 0;
}

int32_t backend_add_function(void* browser, int32_t bindingId, const char* objectName, const char* functionName) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr
         && handle->session->AddFunction(bindingId, objectName, functionName)) ? 1 : 0;
}

int32_t backend_set_volume(void* browser, float volume) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->SetVolume(volume)) ? 1 : 0;
}

int32_t backend_set_muted(void* browser, int32_t muted) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->SetMuted(muted != 0)) ? 1 : 0;
}

int32_t backend_resize(void* browser, BrowserBackendApiSurface* surface) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && surface != nullptr && handle->session->Resize(*surface)) ? 1 : 0;
}

int32_t backend_send_mouse_move(void* browser, int32_t x, int32_t y, uint32_t modifiers) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->SendMouseMove(x, y, modifiers)) ? 1 : 0;
}

int32_t backend_send_mouse_button(void* browser, int32_t x, int32_t y, int32_t button, int32_t down, int32_t clickCount, uint32_t modifiers) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->SendMouseButton(x, y, button, down != 0, clickCount, modifiers)) ? 1 : 0;
}

int32_t backend_send_mouse_wheel(void* browser, int32_t x, int32_t y, int32_t deltaX, int32_t deltaY, uint32_t modifiers) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->SendMouseWheel(x, y, deltaX, deltaY, modifiers)) ? 1 : 0;
}

int32_t backend_send_key(void* browser, int32_t eventType, int32_t keyCode, int32_t nativeCode, uint32_t modifiers, const char* text) {
    BrowserHandle* handle = static_cast<BrowserHandle*>(browser);
    return (handle != nullptr && handle->session != nullptr && handle->session->SendKey(eventType, keyCode, nativeCode, modifiers, text)) ? 1 : 0;
}

const BrowserBackendApi gApiImpl = {
    BROWSER_BACKEND_API_VERSION,
    backend_init,
    backend_tick,
    backend_shutdown,
    backend_available,
    backend_create,
    backend_destroy,
    backend_open_url,
    backend_reload,
    backend_stop,
    backend_go_back,
    backend_go_forward,
    backend_run_js,
    backend_add_function,
    backend_set_volume,
    backend_set_muted,
    backend_resize,
    backend_send_mouse_move,
    backend_send_mouse_button,
    backend_send_mouse_wheel,
    backend_send_key,
};

} // namespace

extern "C" BROWSER_BACKEND_EXPORT const BrowserBackendApi* browser_backend_get_api(uint32_t apiVersion) {
    if (apiVersion != BROWSER_BACKEND_API_VERSION) {
        return nullptr;
    }

    return &gApiImpl;
}
