// DR-1: does a 32-bit OpenXR client work on this machine's runtimes
// (VDXR via Virtual Desktop, SteamVR via Steam Link)?
//
// Exit codes: 0 = full pass (session ran), 1 = loader/runtime failure,
// 2 = partial pass (runtime OK but no headset connected).

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <vector>

static XrInstance g_instance = XR_NULL_HANDLE;

static const char* Res(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && xrResultToString(g_instance, r, buf) == XR_SUCCESS)
        return buf;
    sprintf_s(buf, "XrResult(%d)", (int)r);
    return buf;
}

int main() {
    printf("xr-hello32: 32-bit OpenXR probe (pointer size: %zu bytes)\n\n", sizeof(void*));

    // 1. Extensions
    uint32_t extCount = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) {
        printf("FAIL: xrEnumerateInstanceExtensionProperties: %s\n", Res(r));
        printf("      (no 32-bit runtime reachable by the loader)\n");
        return 1;
    }
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasD3D11 = false;
    for (const auto& e : exts)
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    printf("runtime extensions: %u (XR_KHR_D3D11_enable: %s)\n", extCount, hasD3D11 ? "YES" : "NO");
    if (!hasD3D11) {
        printf("FAIL: runtime lacks D3D11 support\n");
        return 1;
    }

    // 2. Instance
    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "xr-hello32");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "bioshock-vr");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    r = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(r)) {
        printf("FAIL: xrCreateInstance: %s\n", Res(r));
        return 1;
    }

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    printf("runtime: %s (version %u.%u.%u)\n", ip.runtimeName,
           XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
           XR_VERSION_PATCH(ip.runtimeVersion));

    // 3. System (needs the headset connected)
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(g_instance, &sgi, &system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        printf("\nPARTIAL PASS: 32-bit loader + runtime work, but no headset is connected.\n");
        printf("Connect the Quest 3 (Virtual Desktop or Steam Link) and rerun for the full test.\n");
        xrDestroyInstance(g_instance);
        return 2;
    }
    if (XR_FAILED(r)) {
        printf("FAIL: xrGetSystem: %s\n", Res(r));
        xrDestroyInstance(g_instance);
        return 1;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, system, &sp);
    printf("system: %s (max layers %u, max swapchain %ux%u)\n", sp.systemName,
           sp.graphicsProperties.maxLayerCount,
           sp.graphicsProperties.maxSwapchainImageWidth,
           sp.graphicsProperties.maxSwapchainImageHeight);

    // 4. D3D11 device on the runtime's required adapter
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    r = getReqs(g_instance, system, &reqs);
    if (XR_FAILED(r)) {
        printf("FAIL: xrGetD3D11GraphicsRequirementsKHR: %s\n", Res(r));
        xrDestroyInstance(g_instance);
        return 1;
    }
    printf("required min feature level: 0x%X, adapter LUID %08lX-%08lX\n",
           reqs.minFeatureLevel, reqs.adapterLuid.HighPart, reqs.adapterLuid.LowPart);

    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* found = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.HighPart == reqs.adapterLuid.HighPart &&
            desc.AdapterLuid.LowPart == reqs.adapterLuid.LowPart) {
            found = adapter;
            printf("matched adapter: %ls\n", desc.Description);
            break;
        }
        adapter->Release();
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(found, found ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
    if (FAILED(hr)) {
        printf("FAIL: D3D11CreateDevice: 0x%08lX\n", hr);
        xrDestroyInstance(g_instance);
        return 1;
    }

    // 5. Session
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession(g_instance, &sci, &session);
    if (XR_FAILED(r)) {
        printf("FAIL: xrCreateSession: %s\n", Res(r));
        xrDestroyInstance(g_instance);
        return 1;
    }
    printf("session created\n");

    // 6. Wait for READY, run a short empty frame loop, shut down
    bool ready = false;
    bool running = false;
    int frames = 0;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 15000 && frames < 60) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                if (sc->state == XR_SESSION_STATE_READY) ready = true;
                if (sc->state == XR_SESSION_STATE_STOPPING) frames = 60;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (ready && !running) {
            XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
            sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            r = xrBeginSession(session, &sbi);
            if (XR_FAILED(r)) { printf("FAIL: xrBeginSession: %s\n", Res(r)); break; }
            printf("session running; pumping %d empty frames...\n", 60);
            running = true;
        }
        if (running) {
            XrFrameState fs{XR_TYPE_FRAME_STATE};
            XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
            if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) break;
            XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
            xrBeginFrame(session, &fbi);
            XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = 0; // headset shows black; loop mechanics are what we test
            xrEndFrame(session, &fei);
            ++frames;
        } else {
            Sleep(50);
        }
    }

    if (running && frames >= 60) {
        printf("\nFULL PASS: 32-bit OpenXR session ran %d frames on '%s'.\n", frames, ip.runtimeName);
    } else if (!ready) {
        printf("\nPARTIAL: session created but never reached READY in 15 s\n");
        printf("(headset idle/standby? wake it and rerun)\n");
    }

    xrDestroySession(session);
    context->Release();
    device->Release();
    if (found) found->Release();
    factory->Release();
    xrDestroyInstance(g_instance);
    return (running && frames >= 60) ? 0 : 2;
}
