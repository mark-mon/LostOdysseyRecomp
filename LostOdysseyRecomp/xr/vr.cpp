#include <stdafx.h>

#include "xr/vr.h"

#include <os/logger.h>
#include <kernel/memory.h>

#if defined(LO_GPU_PLUME) && (defined(_WIN32) || defined(__linux__)) && !defined(__ANDROID__)
#define LO_VR_VULKAN 1
#endif

#if LO_VR_VULKAN
#include <plume_vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace vr
{
Mode RequestedMode()
{
    static const Mode mode = [] {
        const char* value = getenv("LO_VR");
        if (!value || !*value || value[0] == '0') return Mode::Off;
        const std::string_view text(value);
        if (text == "theater" || text == "1") return Mode::Theater;
        if (text == "mono" || text == "2") return Mode::Mono;
        if (text == "stereo" || text == "3") return Mode::Stereo;
        LOG_WARNING("vr: unknown LO_VR value '{}'; VR stays off", text);
        return Mode::Off;
    }();
    return mode;
}

#if !LO_VR_VULKAN
bool Initialize() { return false; }
plume::VulkanExtensionHooks WrapVulkanHooks(const plume::VulkanExtensionHooks& inner) { return inner; }
std::string PreferredDeviceName() { return {}; }
void OnDeviceCreated(plume::RenderDevice*, plume::RenderCommandQueue*) {}
void SubmitDesktopFrame(plume::RenderTexture*, uint32_t, uint32_t, std::vector<plume::RenderCommandSemaphore*>&) {}
bool AdjustMainView(uint32_t) { return false; }
void RequestRecenter() {}
void SetFrameSource(plume::RenderTexture*, uint32_t, uint32_t) {}
bool StereoViewsActive() { return false; }
bool HudEyeRects(const plume::RenderViewport&, int32_t, int32_t, uint32_t, uint64_t, uint64_t, plume::RenderViewport*, plume::RenderRect*) { return false; }
void SetCurrentEye(int) {}
bool Active() { return false; }
void Shutdown() {}
#else

namespace
{
// ---------------------------------------------------------------- loader and entry points
#define LO_XR_FUNCTIONS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
    X(xrEnumerateInstanceExtensionProperties) X(xrPollEvent) X(xrResultToString) \
    X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrRequestExitSession) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrEnumerateSwapchainFormats) \
    X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) \
    X(xrGetVulkanInstanceExtensionsKHR) X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) \
    X(xrGetVulkanGraphicsRequirementsKHR) X(xrLocateViews) X(xrLocateSpace)

struct Api
{
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
#define LO_XR_DECLARE(name) PFN_##name name = nullptr;
    LO_XR_FUNCTIONS(LO_XR_DECLARE)
#undef LO_XR_DECLARE
};

Api x;

bool OpenLoader()
{
#ifdef _WIN32
    HMODULE module = LoadLibraryW(L"openxr_loader.dll");
    if (!module) { LOG_WARNING("vr: openxr_loader.dll not found beside the game (error {})", GetLastError()); return false; }
    x.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(module, "xrGetInstanceProcAddr"));
#else
    void* module = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!module) module = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!module) { LOG_WARNING("vr: libopenxr_loader.so not found: {}", dlerror()); return false; }
    x.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(module, "xrGetInstanceProcAddr"));
#endif
    if (!x.xrGetInstanceProcAddr) { LOG_WARNING("vr: the OpenXR loader has no xrGetInstanceProcAddr"); return false; }
    // Global functions (no instance yet).
    x.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&x.xrCreateInstance));
    x.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
        reinterpret_cast<PFN_xrVoidFunction*>(&x.xrEnumerateInstanceExtensionProperties));
    return x.xrCreateInstance && x.xrEnumerateInstanceExtensionProperties;
}

bool LoadInstanceFunctions(XrInstance instance)
{
    bool ok = true;
#define LO_XR_LOAD(name) \
    if (XR_FAILED(x.xrGetInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&x.name))) || !x.name) { \
        LOG_WARNING("vr: OpenXR function " #name " is unavailable"); ok = false; }
    LO_XR_FUNCTIONS(LO_XR_LOAD)
#undef LO_XR_LOAD
    return ok;
}

// ---------------------------------------------------------------- state
struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0, height = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
};

struct FrameSlot
{
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::unique_ptr<plume::RenderCommandSemaphore> presentSemaphore;
};

struct State
{
    bool initialized = false, initFailed = false;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    std::string systemName;
    plume::VulkanExtensionHooks inner;

    plume::VulkanDevice* device = nullptr;
    plume::VulkanCommandQueue* queue = nullptr;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;  // the head, for recentering
    Swapchain quad;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<FrameSlot, 3> slots;
    uint32_t slot = 0;
    uint64_t frames = 0, droppedNotRendering = 0;
    int64_t displayPeriodNs = 0;  // from xrWaitFrame: the runtime's frame period
    // Timing of the frame loop since the last stats line (seconds / counts).
    double waitSeconds = 0, endSeconds = 0, betweenSeconds = 0;
    int64_t lastDisplayTime = 0, displayStepSum = 0;
    uint64_t timedFrames = 0, displaySteps = 0;
    std::chrono::steady_clock::time_point lastSubmitEnd{};
    std::chrono::steady_clock::time_point statsTime{};

    // Theater placement (metres, in the LOCAL space: origin at the head when the session started).
    float distance = 2.5f, width = 3.2f;
};

State s;

// ---------------------------------------------------------------- head pose shared with the game thread
// Tangents of the view half-angles (left/down negative), as in XrFovf.
struct Tangents { float left = -1, right = 1, up = 1, down = -1; };

struct HeadPose
{
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};   // head (centre between the eyes)
    XrPosef eyePose[2]{{{0, 0, 0, 1}, {0, 0, 0}}, {{0, 0, 0, 1}, {0, 0, 0}}};
    Tangents eye[2];
    bool valid = false;
};

std::mutex g_poseMutex;
HeadPose g_latest;     // newest prediction from the present thread
HeadPose g_rendered;   // what the game thread last rendered the main view with
uint64_t g_adjustedViews = 0;
thread_local int t_eye = -1;

// Mono renders one 16:9 image (the guest's 1280x720 shape) for both eyes. Its
// frustum is the union of both eyes, widened to 16:9 so it covers them fully.
Tangents MonoTangents(const HeadPose& head)
{
    Tangents t;
    t.left = std::min(head.eye[0].left, head.eye[1].left);
    t.right = std::max(head.eye[0].right, head.eye[1].right);
    t.up = std::max(head.eye[0].up, head.eye[1].up);
    t.down = std::min(head.eye[0].down, head.eye[1].down);
    const float aspect = 16.0f / 9.0f;
    const float w = t.right - t.left, h = t.up - t.down;
    if (w / h < aspect) {
        const float c = (t.right + t.left) * 0.5f, half = h * aspect * 0.5f;
        t.left = c - half; t.right = c + half;
    } else {
        const float c = (t.up + t.down) * 0.5f, half = w / aspect * 0.5f;
        t.down = c - half; t.up = c + half;
    }
    return t;
}

XrFovf ToFov(const Tangents& t)
{
    return {std::atan(t.left), std::atan(t.right), std::atan(t.up), std::atan(t.down)};
}

float WorldScale()
{
    // Unreal units per metre for head translation. LO_VR_SCALE overrides (tuning).
    static const float scale = [] {
        const char* v = getenv("LO_VR_SCALE");
        const float f = v ? float(atof(v)) : 0.0f;
        return f > 0.0f ? f : 50.0f;
    }();
    return scale;
}

// Guest memory, big-endian floats.
float LoadGuestFloat(uint32_t address)
{
    return std::bit_cast<float>(__builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_memory.base + address)));
}
void StoreGuestFloat(uint32_t address, float value)
{
    *reinterpret_cast<uint32_t*>(g_memory.base + address) = __builtin_bswap32(std::bit_cast<uint32_t>(value));
}

using Mat4 = std::array<float, 16>;  // row-major, row vectors (UE3 FMatrix: v' = v * M)

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}

// Camera-space correction for the head pose. UE3's view space is left-handed
// (x right, y up, z forward); OpenXR's is right-handed (z back), so z flips.
// A point c in the game camera's space moves to R^T (c - p) in head space;
// with row vectors that is (c - p) * R, R being the column-convention rotation.
// The headset owns pitch and roll: keep the game camera's position and heading
// but level it, so turning the head never tilts the horizon. UE3 world space is
// Z up; the view's columns are the camera right/up/forward axes in world space.
Mat4 LevelView(const Mat4& v)
{
    const float fx = v[0 * 4 + 2], fy = v[1 * 4 + 2];
    const float length = std::sqrt(fx * fx + fy * fy);
    if (length < 1e-4f) return v;  // looking straight up or down: keep the game's view
    const float f[3] = {fx / length, fy / length, 0.0f};
    const float r[3] = {-f[1], f[0], 0.0f};
    const float u[3] = {0.0f, 0.0f, 1.0f};
    // World camera position: row3 = -pos * M3, M3 orthonormal.
    float pos[3];
    for (int i = 0; i < 3; ++i) pos[i] = -(v[12] * v[i * 4 + 0] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
    Mat4 out{};
    for (int i = 0; i < 3; ++i) { out[i * 4 + 0] = r[i]; out[i * 4 + 1] = u[i]; out[i * 4 + 2] = f[i]; }
    for (int j = 0; j < 3; ++j) out[12 + j] = -(pos[0] * out[0 * 4 + j] + pos[1] * out[1 * 4 + j] + pos[2] * out[2 * 4 + j]);
    out[15] = 1.0f;
    return out;
}

Mat4 HeadCorrection(const XrPosef& pose, float unitsPerMetre)
{
    const float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    float r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    // S R S with S = diag(1, 1, -1)
    r[0][2] = -r[0][2]; r[1][2] = -r[1][2]; r[2][0] = -r[2][0]; r[2][1] = -r[2][1];
    const float p[3] = {pose.position.x * unitsPerMetre, pose.position.y * unitsPerMetre, -pose.position.z * unitsPerMetre};
    Mat4 rotation{r[0][0], r[0][1], r[0][2], 0, r[1][0], r[1][1], r[1][2], 0, r[2][0], r[2][1], r[2][2], 0, 0, 0, 0, 1};
    Mat4 translation{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p[0], -p[1], -p[2], 1};
    return Multiply(translation, rotation);
}

bool Check(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result)) return true;
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    if (x.xrResultToString && s.instance) x.xrResultToString(s.instance, result, text);
    LOG_WARNING("vr: {} failed: {} ({})", what, text[0] ? text : "?", int(result));
    return false;
}

std::vector<std::string> SplitNames(const std::string& text)
{
    std::vector<std::string> names;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) names.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return names;
}

void AppendExtensions(std::vector<VkExtensionProperties>& required, const std::string& names)
{
    for (const auto& name : SplitNames(names)) {
        VkExtensionProperties properties{};
        snprintf(properties.extensionName, sizeof(properties.extensionName), "%s", name.c_str());
        const bool present = std::any_of(required.begin(), required.end(),
            [&](const VkExtensionProperties& e) { return name == e.extensionName; });
        if (!present) required.push_back(properties);
    }
}

bool QueryInstance(void*, std::vector<VkExtensionProperties>& required, std::string& reason)
{
    if (s.inner.queryInstance && !s.inner.queryInstance(s.inner.userData, required, reason)) return false;
    uint32_t size = 0;
    if (!Check(x.xrGetVulkanInstanceExtensionsKHR(s.instance, s.system, 0, &size, nullptr), "xrGetVulkanInstanceExtensionsKHR"))
        return true;  // the game keeps running without VR extensions
    std::string names(size, '\0');
    x.xrGetVulkanInstanceExtensionsKHR(s.instance, s.system, size, &size, names.data());
    names.resize(strnlen(names.c_str(), names.size()));
    LOG_INFO("vr: runtime Vulkan instance extensions: {}", names);
    AppendExtensions(required, names);
    return true;
}

bool QueryDevice(void*, VkInstance instance, VkPhysicalDevice physicalDevice, std::vector<VkExtensionProperties>& required,
                 std::string& reason)
{
    if (s.inner.queryDevice && !s.inner.queryDevice(s.inner.userData, instance, physicalDevice, required, reason)) return false;
    VkPhysicalDevice xrDevice = VK_NULL_HANDLE;
    if (Check(x.xrGetVulkanGraphicsDeviceKHR(s.instance, s.system, instance, &xrDevice), "xrGetVulkanGraphicsDeviceKHR") &&
        xrDevice != physicalDevice)
        LOG_WARNING("vr: the game's GPU is not the one the headset runs on; the session will likely fail");
    uint32_t size = 0;
    if (!Check(x.xrGetVulkanDeviceExtensionsKHR(s.instance, s.system, 0, &size, nullptr), "xrGetVulkanDeviceExtensionsKHR"))
        return true;
    std::string names(size, '\0');
    x.xrGetVulkanDeviceExtensionsKHR(s.instance, s.system, size, &size, names.data());
    names.resize(strnlen(names.c_str(), names.size()));
    LOG_INFO("vr: runtime Vulkan device extensions: {}", names);
    AppendExtensions(required, names);
    return true;
}

// The swap chain image is copied bit for bit, so the headset swapchain uses the
// sRGB format with the same channel order: the runtime then decodes exactly the
// display-encoded values the desktop shows.
int64_t PickFormat(VkFormat source)
{
    uint32_t count = 0;
    x.xrEnumerateSwapchainFormats(s.session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    x.xrEnumerateSwapchainFormats(s.session, count, &count, formats.data());
    const VkFormat wanted = (source == VK_FORMAT_B8G8R8A8_UNORM || source == VK_FORMAT_B8G8R8A8_SRGB)
        ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
    for (int64_t f : formats) if (f == wanted) return f;
    return 0;
}

void DestroySwapchain(Swapchain& chain)
{
    if (chain.handle) x.xrDestroySwapchain(chain.handle);
    chain = {};
}

bool EnsureQuadSwapchain(uint32_t width, uint32_t height, VkFormat source)
{
    if (s.quad.handle && s.quad.width == width && s.quad.height == height) return true;
    if (s.quad.handle) {
        vkQueueWaitIdle(s.queue->queue->vk);
        DestroySwapchain(s.quad);
    }
    const int64_t format = PickFormat(source);
    if (!format) { LOG_WARNING("vr: the runtime offers no swapchain format matching {}", int(source)); return false; }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(x.xrCreateSwapchain(s.session, &info, &s.quad.handle), "xrCreateSwapchain")) return false;
    uint32_t count = 0;
    x.xrEnumerateSwapchainImages(s.quad.handle, 0, &count, nullptr);
    s.quad.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    x.xrEnumerateSwapchainImages(s.quad.handle, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(s.quad.images.data()));
    s.quad.format = format;
    s.quad.width = width;
    s.quad.height = height;
    LOG_NOTICE("vr: theater swapchain {}x{} format {} ({} images)", width, height, format, count);
    return true;
}

void HandleEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (x.xrPollEvent(s.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            s.sessionState = changed.state;
            LOG_NOTICE("vr: session state {}", int(changed.state));
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s.running = Check(x.xrBeginSession(s.session, &begin), "xrBeginSession");
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                x.xrEndSession(s.session);
                s.running = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                s.running = false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOG_WARNING("vr: OpenXR instance loss pending");
            s.running = false;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void LogStats()
{
    const auto now = std::chrono::steady_clock::now();
    if (s.statsTime.time_since_epoch().count() == 0) s.statsTime = now;
    if (now - s.statsTime < std::chrono::seconds(10)) return;
    const double seconds = std::chrono::duration<double>(now - s.statsTime).count();
    static uint64_t lastFrames = 0;
    const double n = s.timedFrames ? double(s.timedFrames) : 1.0;
    LOG_NOTICE("vr: stats {:.1f} fps submitted, runtime period {:.2f} ms ({:.1f} Hz), state {}, not-rendered {}; "
        "per frame: xrWaitFrame {:.2f} ms, xrEndFrame {:.2f} ms, game between submits {:.2f} ms, display time step {:.2f} ms",
        (s.frames - lastFrames) / seconds, s.displayPeriodNs / 1e6, s.displayPeriodNs ? 1e9 / double(s.displayPeriodNs) : 0.0,
        int(s.sessionState), s.droppedNotRendering, s.waitSeconds * 1e3 / n, s.endSeconds * 1e3 / n, s.betweenSeconds * 1e3 / n,
        s.displaySteps ? double(s.displayStepSum) / double(s.displaySteps) / 1e6 : 0.0);
    s.waitSeconds = s.endSeconds = s.betweenSeconds = 0;
    s.timedFrames = s.displaySteps = 0;
    s.displayStepSum = 0;
    lastFrames = s.frames;
    s.statsTime = now;
}
}  // namespace

bool Initialize()
{
    if (s.initialized) return true;
    if (s.initFailed || RequestedMode() == Mode::Off) return false;
    s.initFailed = true;
    if (!OpenLoader()) return false;

    const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "Lost Odyssey Recomp");
    snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "LostOdysseyRecomp");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    if (!Check(x.xrCreateInstance(&info, &s.instance), "xrCreateInstance")) return false;
    if (!LoadInstanceFunctions(s.instance)) return false;

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    x.xrGetInstanceProperties(s.instance, &properties);
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(x.xrGetSystem(s.instance, &systemInfo, &s.system), "xrGetSystem (is the headset on and the runtime running?)"))
        return false;
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    x.xrGetSystemProperties(s.instance, s.system, &systemProperties);
    s.systemName = systemProperties.systemName;
    // Required before a Vulkan session can be created.
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!Check(x.xrGetVulkanGraphicsRequirementsKHR(s.instance, s.system, &requirements), "xrGetVulkanGraphicsRequirementsKHR"))
        return false;
    LOG_NOTICE("vr: OpenXR runtime '{}' {}.{}.{}, system '{}', mode {}", properties.runtimeName,
        XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
        XR_VERSION_PATCH(properties.runtimeVersion), s.systemName, RequestedMode() == Mode::Stereo ? "stereo" : RequestedMode() == Mode::Mono ? "mono" : "theater");
    s.initialized = true;
    s.initFailed = false;
    return true;
}

plume::VulkanExtensionHooks WrapVulkanHooks(const plume::VulkanExtensionHooks& inner)
{
    if (!Initialize()) return inner;
    s.inner = inner;
    plume::VulkanExtensionHooks hooks;
    hooks.userData = nullptr;
    hooks.queryInstance = QueryInstance;
    hooks.queryDevice = QueryDevice;
    return hooks;
}

std::string PreferredDeviceName()
{
    return {};  // single-GPU systems: plume's choice already matches; checked in QueryDevice
}

void OnDeviceCreated(plume::RenderDevice* device, plume::RenderCommandQueue* queue)
{
    if (!s.initialized || s.session || !device || !queue) return;
    s.device = static_cast<plume::VulkanDevice*>(device);
    s.queue = static_cast<plume::VulkanCommandQueue*>(queue);

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = s.device->renderInterface->instance;
    binding.physicalDevice = s.device->physicalDevice;
    binding.device = s.device->vk;
    binding.queueFamilyIndex = s.queue->familyIndex;
    binding.queueIndex = s.queue->queueIndex;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = s.system;
    if (!Check(x.xrCreateSession(s.instance, &info, &s.session), "xrCreateSession")) { s.session = XR_NULL_HANDLE; return; }

    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    Check(x.xrCreateReferenceSpace(s.session, &space, &s.local), "xrCreateReferenceSpace");
    XrReferenceSpaceCreateInfo head{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    head.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    head.poseInReferenceSpace.orientation.w = 1.0f;
    Check(x.xrCreateReferenceSpace(s.session, &head, &s.view), "xrCreateReferenceSpace(view)");

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = s.queue->familyIndex;
    vkCreateCommandPool(s.device->vk, &poolInfo, nullptr, &s.pool);
    for (auto& slot : s.slots) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = s.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers(s.device->vk, &allocate, &slot.commands);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(s.device->vk, &fence, nullptr, &slot.fence);
        slot.presentSemaphore = device->createCommandSemaphore();
    }
    LOG_NOTICE("vr: session created on '{}' queue family {} index {}", s.device->description.name,
        s.queue->familyIndex, s.queue->queueIndex);
}

std::atomic<bool> g_recenterRequested{false};
XrPosef g_recenterBase{{0, 0, 0, 1}, {0, 0, 0}};  // LOCAL space origin relative to the runtime's original LOCAL

XrPosef ComposePose(const XrPosef& a, const XrPosef& b)  // a then b: b expressed in a's frame
{
    const XrQuaternionf& q = a.orientation; const XrQuaternionf& r = b.orientation;
    XrPosef out;
    out.orientation = {q.w * r.x + q.x * r.w + q.y * r.z - q.z * r.y,
                       q.w * r.y - q.x * r.z + q.y * r.w + q.z * r.x,
                       q.w * r.z + q.x * r.y - q.y * r.x + q.z * r.w,
                       q.w * r.w - q.x * r.x - q.y * r.y - q.z * r.z};
    // rotate b.position by q
    const float vx = b.position.x, vy = b.position.y, vz = b.position.z;
    const float tx = 2 * (q.y * vz - q.z * vy), ty = 2 * (q.z * vx - q.x * vz), tz = 2 * (q.x * vy - q.y * vx);
    out.position = {a.position.x + vx + q.w * tx + (q.y * tz - q.z * ty),
                    a.position.y + vy + q.w * ty + (q.z * tx - q.x * tz),
                    a.position.z + vz + q.w * tz + (q.x * ty - q.y * tx)};
    return out;
}

// Rebuilds the LOCAL space so the head's current yaw and position become the origin.
void ApplyRecenter(XrTime time)
{
    if (!g_recenterRequested.exchange(false) || !s.view || !s.local) return;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(x.xrLocateSpace(s.view, s.local, time, &location)) ||
        !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        LOG_WARNING("vr: recenter skipped (head pose not valid)");
        return;
    }
    // Yaw only: the horizon stays level.
    const XrQuaternionf& q = location.pose.orientation;
    const float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x));
    XrPosef offset{{0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}, location.pose.position};
    const XrPosef base = ComposePose(g_recenterBase, offset);
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace = base;
    XrSpace recentered = XR_NULL_HANDLE;
    if (!Check(x.xrCreateReferenceSpace(s.session, &space, &recentered), "xrCreateReferenceSpace(recenter)")) return;
    x.xrDestroySpace(s.local);
    s.local = recentered;
    g_recenterBase = base;
    LOG_NOTICE("vr: recentered (yaw {:.1f} deg, head at {:.2f} {:.2f} {:.2f} m)", yaw * 57.2958f,
        location.pose.position.x, location.pose.position.y, location.pose.position.z);
}

struct FrameSource
{
    plume::RenderTexture* texture = nullptr;
    uint32_t width = 0, height = 0;
    uint64_t frames = 0;  // s.frames when it was handed over (valid for that present only)
};
std::mutex g_sourceMutex;
FrameSource g_source;

VkImageLayout ToVkLayout(plume::RenderTextureLayout layout)
{
    switch (layout) {
    case plume::RenderTextureLayout::GENERAL: return VK_IMAGE_LAYOUT_GENERAL;
    case plume::RenderTextureLayout::SHADER_READ: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case plume::RenderTextureLayout::COLOR_WRITE: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case plume::RenderTextureLayout::COPY_SOURCE:
    case plume::RenderTextureLayout::RESOLVE_SOURCE: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case plume::RenderTextureLayout::COPY_DEST:
    case plume::RenderTextureLayout::RESOLVE_DEST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case plume::RenderTextureLayout::PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

void SetFrameSource(plume::RenderTexture* texture, uint32_t width, uint32_t height)
{
    if (!s.running || RequestedMode() == Mode::Theater) return;
    static uint32_t logged = 0;
    if (texture && logged++ < 1) {
        auto* vk = static_cast<plume::VulkanTexture*>(texture);
        LOG_NOTICE("vr: game frame source {}x{} (texture {}x{} format {})", width, height,
            vk->desc.width, vk->desc.height, int(vk->imageFormat));
    }
    const std::scoped_lock lock(g_sourceMutex);
    g_source = {texture, width, height, s.frames};
}

bool AdjustMainView(uint32_t view)
{
    const Mode mode = RequestedMode();
    const int eye = t_eye;
    if (!s.running || !g_memory.base) return false;
    if (mode != Mode::Mono && !(mode == Mode::Stereo && eye >= 0)) return false;
    HeadPose head;
    {
        const std::scoped_lock lock(g_poseMutex);
        if (!g_latest.valid) return false;
        head = g_latest;
    }
    Mat4 viewMatrix{};
    for (uint32_t i = 0; i < 16; ++i) viewMatrix[i] = LoadGuestFloat(view + 0x40 + i * 4);
    if (g_adjustedViews == 0) {
        Mat4 projection{};
        for (uint32_t i = 0; i < 16; ++i) projection[i] = LoadGuestFloat(view + 0x80 + i * 4);
        LOG_NOTICE("vr: first main view V row3 [{:.1f} {:.1f} {:.1f} {:.3f}] P [{:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f}]",
            viewMatrix[12], viewMatrix[13], viewMatrix[14], viewMatrix[15], projection[0], projection[5],
            projection[8], projection[9], projection[10], projection[11], projection[12], projection[13], projection[14], projection[15]);
    }
    static const bool keepPitch = [] { const char* v = getenv("LO_VR_KEEP_PITCH"); return v && v[0] == '1'; }();
    if (!keepPitch) viewMatrix = LevelView(viewMatrix);
    const XrPosef& pose = eye >= 0 ? head.eyePose[eye] : head.pose;
    const Mat4 adjusted = Multiply(viewMatrix, HeadCorrection(pose, WorldScale()));
    for (uint32_t i = 0; i < 16; ++i) StoreGuestFloat(view + 0x40 + i * 4, adjusted[i]);

    // Projection: keep the game's depth terms (column 2/3), replace the x/y scale
    // and centre with the headset frustum (off-axis allowed).
    const Tangents t = eye >= 0 ? head.eye[eye] : MonoTangents(head);
    StoreGuestFloat(view + 0x80 + 0 * 4, 2.0f / (t.right - t.left));
    StoreGuestFloat(view + 0x80 + 5 * 4, 2.0f / (t.up - t.down));
    StoreGuestFloat(view + 0x80 + 8 * 4, -(t.right + t.left) / (t.right - t.left));
    StoreGuestFloat(view + 0x80 + 9 * 4, -(t.up + t.down) / (t.up - t.down));
    {
        // The left eye's view is built first; the frame is complete once the last view of it is.
        const std::scoped_lock lock(g_poseMutex);
        g_rendered = head;
    }
    ++g_adjustedViews;
    return true;
}

bool StereoViewsActive()
{
    if (RequestedMode() != Mode::Stereo || !s.running) return false;
    const std::scoped_lock lock(g_poseMutex);
    return g_latest.valid;
}

void SetCurrentEye(int eye) { t_eye = eye; }

void RequestRecenter() { g_recenterRequested = true; }

bool HudEyeRects(const plume::RenderViewport& viewport, int32_t targetWidth, int32_t targetHeight, uint32_t guestWidth,
                 uint64_t vertexShader, uint64_t pixelShader, plume::RenderViewport* eyes, plume::RenderRect* scissors)
{
    // The game's full-screen post-processing quads (scene copy, tone map, blur)
    // share one vertex shader; they work on both eye views at once and stay whole.
    constexpr uint64_t FullScreenQuadVS = 0x8bbd4da701845d16ull;
    // Diagnostics: the first wide draws and why they were (not) taken as HUD.
    // One line per distinct shader pair and verdict.
    static std::mutex seenMutex;
    static std::vector<std::tuple<uint64_t, uint64_t, const char*>> seen;
    const bool wide = viewport.width > float(targetWidth) * 0.6f;
    auto note = [&](const char* verdict) {
        bool first = false;
        if (wide) {
            const std::scoped_lock lock(seenMutex);
            const auto entry = std::make_tuple(vertexShader, pixelShader, verdict);
            if (seen.size() < 64 && std::find(seen.begin(), seen.end(), entry) == seen.end()) { seen.push_back(entry); first = true; }
        }
        if (first)
            LOG_NOTICE("vr: hud candidate {} vs {:016x} ps {:016x} viewport ({:.1f},{:.1f} {:.1f}x{:.1f}) target {}x{} guest width {}",
                verdict, vertexShader, pixelShader, viewport.x, viewport.y, viewport.width, viewport.height, targetWidth, targetHeight, guestWidth);
    };
    // Only the guest's full-size output (1280 wide in guest space) carries the HUD.
    if (guestWidth < 1200 || targetWidth <= 0 || targetHeight <= 0) { note("rejected: guest width"); return false; }
    // The output target is padded (1280x736 for 1280x720), so judge by the
    // viewport: it starts at the origin, spans the full width and is 16:9.
    if (viewport.x > 0.5f || viewport.y > 0.5f || viewport.width < float(targetWidth) - 1.0f ||
        viewport.height > float(targetHeight) + 0.5f)
    { note("rejected: not full width"); return false; }
    if (vertexShader == FullScreenQuadVS) { note("rejected: full-screen quad"); return false; }
    // LO_VR_HUD_VS / LO_VR_HUD_PS=<hex>[,<hex>...] (developer): only these shaders count as HUD.
    auto parseList = [](const char* name) {
        std::vector<uint64_t> list;
        if (const char* v = getenv(name))
            for (const char* p = v; *p;) {
                char* end = nullptr;
                const uint64_t value = strtoull(p, &end, 16);
                if (end == p) break;
                list.push_back(value);
                p = *end == ',' ? end + 1 : end;
            }
        return list;
    };
    // Default: the game's 2D canvas vertex shader (UE3 simple elements: HUD,
    // menus, fades). The renderer has already excluded the full-frame scene tile.
    static const std::vector<uint64_t> allowedVs = [&] {
        auto list = parseList("LO_VR_HUD_VS");
        if (list.empty()) list.push_back(0xeb5f611c4321708eull);
        return list;
    }();
    static const std::vector<uint64_t> allowedPs = parseList("LO_VR_HUD_PS");
    if (!allowedVs.empty() && std::find(allowedVs.begin(), allowedVs.end(), vertexShader) == allowedVs.end())
    { note("rejected: not in LO_VR_HUD_VS"); return false; }
    if (!allowedPs.empty() && std::find(allowedPs.begin(), allowedPs.end(), pixelShader) == allowedPs.end())
    { note("rejected: not in LO_VR_HUD_PS"); return false; }
    const float aspect = viewport.width / viewport.height;
    if (std::abs(aspect - 16.0f / 9.0f) > 0.05f) { note("rejected: aspect"); return false; }
    note("taken");
    // LO_VR_HUD_SCALE: panel width as a fraction of one eye's width (default 0.75).
    static const float scale = [] {
        const char* v = getenv("LO_VR_HUD_SCALE");
        const float f = v ? float(atof(v)) : 0.0f;
        return f > 0.05f && f <= 1.0f ? f : 0.75f;
    }();
    const float half = viewport.width * 0.5f, fullHeight = viewport.height;
    float w = half * scale, h = w / aspect;
    if (h > fullHeight) { h = fullHeight; w = h * aspect; }
    for (int e = 0; e < 2; ++e) {
        eyes[e] = viewport;
        eyes[e].x = float(e) * half + (half - w) * 0.5f;
        eyes[e].y = (fullHeight - h) * 0.5f;
        eyes[e].width = w;
        eyes[e].height = h;
        scissors[e] = plume::RenderRect(int32_t(float(e) * half), 0, int32_t(float(e + 1) * half), int32_t(fullHeight));
    }
    return true;
}

bool Active() { return s.running; }

void SubmitDesktopFrame(plume::RenderTexture* image, uint32_t width, uint32_t height,
                        std::vector<plume::RenderCommandSemaphore*>& presentWaits)
{
    if (!s.session || !image || !width || !height) return;
    HandleEvents();
    LogStats();
    if (!s.running) return;

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    const auto waitStart = std::chrono::steady_clock::now();
    if (s.lastSubmitEnd.time_since_epoch().count())
        s.betweenSeconds += std::chrono::duration<double>(waitStart - s.lastSubmitEnd).count();
    if (!Check(x.xrWaitFrame(s.session, &waitInfo, &frameState), "xrWaitFrame")) return;
    s.waitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count();
    s.displayPeriodNs = frameState.predictedDisplayPeriod;
    if (s.lastDisplayTime && frameState.predictedDisplayTime > s.lastDisplayTime) {
        s.displayStepSum += frameState.predictedDisplayTime - s.lastDisplayTime;
        ++s.displaySteps;
    }
    s.lastDisplayTime = frameState.predictedDisplayTime;
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(x.xrBeginFrame(s.session, &beginInfo), "xrBeginFrame")) return;
    ApplyRecenter(frameState.predictedDisplayTime);

    if (RequestedMode() == Mode::Mono || RequestedMode() == Mode::Stereo) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        // The game thread renders about a frame ahead of this present.
        locate.displayTime = frameState.predictedDisplayTime + frameState.predictedDisplayPeriod;
        locate.space = s.local;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t viewCount = 0;
        if (XR_SUCCEEDED(x.xrLocateViews(s.session, &locate, &viewState, 2, &viewCount, views)) && viewCount == 2 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            HeadPose head;
            head.pose.orientation = views[0].pose.orientation;
            head.pose.position = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
            for (int e = 0; e < 2; ++e)
                head.eye[e] = {std::tan(views[e].fov.angleLeft), std::tan(views[e].fov.angleRight),
                               std::tan(views[e].fov.angleUp), std::tan(views[e].fov.angleDown)};
            head.valid = true;
            // Developer check without a moving headset: LO_VR_FAKE_YAW=<degrees> swings the head
            // left and right (4 s period) on top of the tracked pose.
            static const float fakeYaw = [] { const char* v = getenv("LO_VR_FAKE_YAW"); return v ? float(atof(v)) : 0.0f; }();
            if (fakeYaw != 0.0f) {
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const float half = float(std::sin(t * 2.0 * 3.14159265 / 4.0)) * fakeYaw * 3.14159265f / 360.0f;
                const XrQuaternionf q = head.pose.orientation, y{0, std::sin(half), 0, std::cos(half)};
                head.pose.orientation = {y.w * q.x + y.x * q.w + y.y * q.z - y.z * q.y,
                                         y.w * q.y - y.x * q.z + y.y * q.w + y.z * q.x,
                                         y.w * q.z + y.x * q.y - y.y * q.x + y.z * q.w,
                                         y.w * q.w - y.x * q.x - y.y * q.y - y.z * q.z};
            }
            for (int e = 0; e < 2; ++e) {
                head.eyePose[e] = views[e].pose;
                head.eyePose[e].orientation = head.pose.orientation;  // includes any developer fake yaw
            }
            const std::scoped_lock lock(g_poseMutex);
            g_latest = head;
        }
    }

    // Prefer the game's own final frame (internal resolution, before desktop
    // scaling); fall back to the finished desktop image (host menus, no game frame).
    auto* source = static_cast<plume::VulkanTexture*>(image);
    VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    {
        const std::scoped_lock lock(g_sourceMutex);
        if (g_source.texture && g_source.frames == s.frames) {
            auto* frame = static_cast<plume::VulkanTexture*>(g_source.texture);
            const VkImageLayout layout = ToVkLayout(frame->textureLayout);
            // Only when it is larger than the desktop image (an internal resolution
            // above the window). TODO: a Vulkan device loss was seen once with a
            // 1280x720 frame source; investigate before relying on this path.
            if (layout != VK_IMAGE_LAYOUT_UNDEFINED && g_source.width > width && g_source.height > height) {
                source = frame;
                sourceLayout = layout;
                width = g_source.width;
                height = g_source.height;
            }
        }
        g_source.texture = nullptr;
    }
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projectionViews[2] = {};
    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t layerCount = 0;

    if (frameState.shouldRender && EnsureQuadSwapchain(width, height, source->imageFormat)) {
        auto& slot = s.slots[s.slot];
        s.slot = (s.slot + 1) % s.slots.size();
        vkWaitForFences(s.device->vk, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(s.device->vk, 1, &slot.fence);

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        bool acquired;
        {
            const std::scoped_lock lock(*s.queue->queue->mutex);
            acquired = Check(x.xrAcquireSwapchainImage(s.quad.handle, &acquire, &index), "xrAcquireSwapchainImage");
        }
        if (acquired &&
            Check(x.xrWaitSwapchainImage(s.quad.handle, &wait), "xrWaitSwapchainImage")) {
            const VkImage target = s.quad.images[index].image;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandBuffer(slot.commands, 0);
            vkBeginCommandBuffer(slot.commands, &begin);
            VkImageMemoryBarrier toCopy[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
            // The desktop image was rendered by earlier submissions on this queue.
            toCopy[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            toCopy[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toCopy[0].oldLayout = sourceLayout;
            toCopy[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toCopy[0].image = source->vk;
            toCopy[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toCopy[0].srcQueueFamilyIndex = toCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toCopy[1] = toCopy[0];
            toCopy[1].srcAccessMask = 0;
            toCopy[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toCopy[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // OpenXR hands images over in this layout
            toCopy[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toCopy[1].image = target;
            vkCmdPipelineBarrier(slot.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                0, nullptr, 0, nullptr, 2, toCopy);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = region.srcSubresource;
            region.extent = {width, height, 1};
            vkCmdCopyImage(slot.commands, source->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            VkImageMemoryBarrier back[2] = {toCopy[0], toCopy[1]};
            back[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            back[0].dstAccessMask = 0;
            back[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back[0].newLayout = sourceLayout;
            back[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            back[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            back[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            back[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier(slot.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                0, nullptr, 0, nullptr, 2, back);
            vkEndCommandBuffer(slot.commands);

            auto* semaphore = static_cast<plume::VulkanCommandSemaphore*>(slot.presentSemaphore.get());
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &slot.commands;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &semaphore->vk;
            VkResult result;
            {
                const std::scoped_lock lock(*s.queue->queue->mutex);
                result = vkQueueSubmit(s.queue->queue->vk, 1, &submit, slot.fence);
            }
            if (result == VK_SUCCESS) presentWaits.push_back(slot.presentSemaphore.get());
            else {
                LOG_WARNING("vr: copy submit failed {}", int(result));
                const std::scoped_lock lock(*s.queue->queue->mutex);
                vkQueueSubmit(s.queue->queue->vk, 0, nullptr, slot.fence);  // keep the slot's fence signalled
            }

            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            {
                const std::scoped_lock lock(*s.queue->queue->mutex);
                x.xrReleaseSwapchainImage(s.quad.handle, &release);
            }

            if (RequestedMode() == Mode::Mono || RequestedMode() == Mode::Stereo) {
                const bool stereo = RequestedMode() == Mode::Stereo;
                HeadPose rendered;
                {
                    const std::scoped_lock lock(g_poseMutex);
                    rendered = g_rendered;
                }
                if (rendered.valid) {
                    const XrFovf monoFov = ToFov(MonoTangents(rendered));
                    const int32_t half = int32_t(width / 2);
                    for (int e = 0; e < 2; ++e) {
                        projectionViews[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                        projectionViews[e].pose = stereo ? rendered.eyePose[e] : rendered.pose;
                        projectionViews[e].fov = stereo ? ToFov(rendered.eye[e]) : monoFov;
                        projectionViews[e].subImage.swapchain = s.quad.handle;
                        // Stereo: the game drew the left eye into the left half, the right eye into the right half.
                        projectionViews[e].subImage.imageRect = stereo
                            ? XrRect2Di{{e == 0 ? 0 : half, 0}, {half, int32_t(height)}}
                            : XrRect2Di{{0, 0}, {int32_t(width), int32_t(height)}};
                    }
                    projection.space = s.local;
                    projection.viewCount = 2;
                    projection.views = projectionViews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                    layerCount = 1;
                }
            } else {
            quad.space = s.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = s.quad.handle;
            quad.subImage.imageRect = {{0, 0}, {int32_t(width), int32_t(height)}};
            quad.pose.orientation.w = 1.0f;
            quad.pose.position = {0.0f, 0.0f, -s.distance};
            quad.size = {s.width, s.width * float(height) / float(width)};
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            layerCount = 1;
            }
        }
    } else if (!frameState.shouldRender) {
        ++s.droppedNotRendering;
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount ? layers : nullptr;
    const auto endStart = std::chrono::steady_clock::now();
    {
        // The runtime submits on our queue during xrEndFrame.
        const std::scoped_lock lock(*s.queue->queue->mutex);
        Check(x.xrEndFrame(s.session, &endInfo), "xrEndFrame");
    }
    s.lastSubmitEnd = std::chrono::steady_clock::now();
    s.endSeconds += std::chrono::duration<double>(s.lastSubmitEnd - endStart).count();
    ++s.timedFrames;
    ++s.frames;
}

void Shutdown()
{
    if (s.device && s.queue) vkQueueWaitIdle(s.queue->queue->vk);
    DestroySwapchain(s.quad);
    if (s.pool) {
        for (auto& slot : s.slots) {
            if (slot.fence) vkDestroyFence(s.device->vk, slot.fence, nullptr);
            slot.presentSemaphore.reset();
        }
        vkDestroyCommandPool(s.device->vk, s.pool, nullptr);
        s.pool = VK_NULL_HANDLE;
    }
    if (s.local) { x.xrDestroySpace(s.local); s.local = XR_NULL_HANDLE; }
    if (s.view) { x.xrDestroySpace(s.view); s.view = XR_NULL_HANDLE; }
    if (s.session) { x.xrDestroySession(s.session); s.session = XR_NULL_HANDLE; }
    if (s.instance) { x.xrDestroyInstance(s.instance); s.instance = XR_NULL_HANDLE; }
    s.running = false;
    s.initialized = false;
}
#endif
}  // namespace vr
