// armcap32 - ground-truth arm capture. Head + both controllers -> CSV.
//
// WHAT THIS IS FOR. A two-bone arm has exactly ONE free parameter once the
// shoulder, the hand and the segment lengths are fixed: the swivel of the elbow
// about the shoulder-to-hand axis. Every elbow model this project has - our
// `elbow out` / `elbow follows wrist` sliders, and FRIK's forty lines of
// behind/crossing/lifted constraints - is a hand-tuned GUESS at that one angle.
// Neither has ever been measured against a real elbow.
//
// So measure one. Strap a controller to the elbow, hold the other in the hand of
// the SAME arm, and record where both actually go across as many poses as the
// wearer can reach. See docs/bioshock1/IK-RESEARCH.md section 7.
//
// WHY IT IS NOT IN THE MOD. This needs no BioShock, no injection and no game
// state, and running it outside means the data is clean of every game-side
// transform - worldScale, recenterYaw, the rig DrawScale - which are exactly the
// frames that have cost this project three sessions. It is 32-bit only because
// everything here is; the OpenXR loader path is the same one the mod uses, so a
// pass here is also evidence about the mod's runtime.
//
// WHAT IT RECORDS, and the one field that matters most: TRACKED, not just VALID.
// A Quest controller strapped to an elbow spends much of its life out of the
// headset's cameras, occluded by the arm. When that happens the runtime does NOT
// stop reporting a pose - it dead-reckons from the IMU and drifts, silently, and
// a drifted sample is indistinguishable from a real one downstream. VALID stays
// set through all of it. TRACKED is the bit that separates an observation from a
// guess, so every sample carries both and the fit must discard anything with
// trk=0. (The mod's own openxr_input.cpp checks only VALID - noted in the
// research doc as worth changing regardless of whether this capture happens.)
//
// CONTROLS, because the wearer cannot see this console:
//   EITHER trigger  - toggle recording on/off
//   EITHER grip     - drop a numbered marker row (use it to separate pose runs)
// Feedback is haptic on BOTH controllers, since which one is held is unknown:
//   1 buzz  = recording started        2 buzzes = recording stopped
//   tick    = still recording (15 s)   3 buzzes = auto-stopped or shutting down

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

XrInstance g_instance = XR_NULL_HANDLE;

// Safety stop: a forgotten session should not fill a disk overnight.
constexpr double kMaxRecordSeconds = 20.0 * 60.0;
constexpr uint64_t kAliveTickMs = 15000;

const char* Res(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && xrResultToString(g_instance, r, buf) == XR_SUCCESS)
        return buf;
    sprintf_s(buf, "XrResult(%d)", static_cast<int>(r));
    return buf;
}

bool Fail(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return false;
    printf("FAIL: %s: %s\n", what, Res(r));
    return true;
}

// Both TRACKED bits. This is the filter the fit must apply - see the banner.
bool FullyTracked(XrSpaceLocationFlags f) {
    return (f & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
           (f & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
}

std::string OutputPath() {
    char base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    std::string dir = (n > 0 && n < MAX_PATH) ? std::string(base) : std::string(".");
    dir += "\\BioshockVR";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\armcap";
    CreateDirectoryA(dir.c_str(), nullptr);

    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[64];
    strftime(stamp, sizeof stamp, "armcap-%Y%m%d-%H%M%S.csv", &tm);
    return dir + "\\" + stamp;
}

struct HandActions {
    XrAction pose = XR_NULL_HANDLE;
    XrAction toggle = XR_NULL_HANDLE;
    XrAction marker = XR_NULL_HANDLE;
    XrAction haptic = XR_NULL_HANDLE;
};

void Buzz(XrSession session, XrAction haptic, const XrPath* subPaths, float seconds,
          float amplitude) {
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.duration = static_cast<XrDuration>(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = amplitude;
    // Both controllers: which one is held is unknown, and the elbow-mounted one
    // buzzing is harmless confirmation that it is still awake.
    for (int i = 0; i < 2; ++i) {
        XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
        hai.action = haptic;
        hai.subactionPath = subPaths[i];
        xrApplyHapticFeedback(session, &hai, reinterpret_cast<const XrHapticBaseHeader*>(&v));
    }
}

// A countable buzz pattern, spread across FRAMES rather than blocking.
//
// The first cut slept between pulses, which put a Sleep of up to 400 ms between
// xrBeginFrame and xrEndFrame - holding a compositor frame open for 30-odd
// display intervals every time the trigger was pulled. Runtimes are entitled to
// treat that as a dropped frame or a hung app. Nothing in a frame loop may
// block; the pattern is state, pumped once per frame.
struct BuzzPattern {
    int pulsesLeft = 0;
    uint64_t nextAtMs = 0;

    void start(int pulses) {
        pulsesLeft = pulses;
        nextAtMs = 0; // fire the first one on the next pump
    }

    void pump(XrSession session, XrAction haptic, const XrPath* subPaths, uint64_t nowMs) {
        if (pulsesLeft <= 0 || nowMs < nextAtMs) return;
        Buzz(session, haptic, subPaths, 0.09f, 1.0f);
        --pulsesLeft;
        nextAtMs = nowMs + 220; // long enough to feel as separate pulses
    }
};

} // namespace

int main(int argc, char** argv) {
    // Which physical controller is strapped to the elbow. Recorded in the CSV
    // header only - both controllers are logged raw either way, so a wrong flag
    // is a relabel offline, not a lost run.
    const char* elbowSide = "left";
    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "--elbow") == 0 && i + 1 < argc) elbowSide = argv[++i];
    }
    const bool elbowIsLeft = _stricmp(elbowSide, "left") == 0;

    printf("armcap32: arm ground-truth capture (%zu-bit)\n", sizeof(void*) * 8);
    printf("elbow controller: %s  |  hand controller: %s\n\n", elbowIsLeft ? "LEFT" : "RIGHT",
           elbowIsLeft ? "RIGHT" : "LEFT");

    // ---- instance ----------------------------------------------------------
    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "armcap32");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "bioshock-vr");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    if (Fail(xrCreateInstance(&ici, &g_instance), "xrCreateInstance")) {
        printf("      (no 32-bit OpenXR runtime reachable - start Virtual Desktop or Steam Link)\n");
        return 1;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    printf("runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
           XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrResult r = xrGetSystem(g_instance, &sgi, &system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        printf("\nNo headset connected. Connect the Quest 3 and rerun.\n");
        xrDestroyInstance(g_instance);
        return 2;
    }
    if (Fail(r, "xrGetSystem")) { xrDestroyInstance(g_instance); return 1; }

    // ---- D3D11 device on the runtime's adapter -----------------------------
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!getReqs || Fail(getReqs(g_instance, system, &reqs), "xrGetD3D11GraphicsRequirementsKHR")) {
        xrDestroyInstance(g_instance);
        return 1;
    }
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* found = nullptr;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.AdapterLuid.HighPart == reqs.adapterLuid.HighPart &&
            d.AdapterLuid.LowPart == reqs.adapterLuid.LowPart) {
            found = adapter;
            break;
        }
        adapter->Release();
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(found, found ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device,
                                 nullptr, &context))) {
        printf("FAIL: D3D11CreateDevice\n");
        xrDestroyInstance(g_instance);
        return 1;
    }

    // ---- session -----------------------------------------------------------
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (Fail(xrCreateSession(g_instance, &sci, &session), "xrCreateSession")) {
        xrDestroyInstance(g_instance);
        return 1;
    }

    // ---- actions -----------------------------------------------------------
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asci.actionSetName, "armcap");
    strcpy_s(asci.localizedActionSetName, "Arm Capture");
    asci.priority = 0;
    if (Fail(xrCreateActionSet(g_instance, &asci, &actionSet), "xrCreateActionSet")) return 1;

    XrPath hands[2]{};
    xrStringToPath(g_instance, "/user/hand/left", &hands[0]);
    xrStringToPath(g_instance, "/user/hand/right", &hands[1]);

    HandActions act;
    auto mkAction = [&](XrActionType type, const char* name, const char* label, XrAction* out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, label);
        aci.countSubactionPaths = 2;
        aci.subactionPaths = hands;
        return xrCreateAction(actionSet, &aci, out);
    };
    if (Fail(mkAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose", &act.pose), "grip") ||
        Fail(mkAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "toggle", "Toggle Record", &act.toggle), "tog") ||
        Fail(mkAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "marker", "Marker", &act.marker), "mark") ||
        Fail(mkAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic", &act.haptic), "haptic"))
        return 1;

    // Bind against Touch first (Quest 3 through VDXR or SteamVR) and the simple
    // controller as a fallback, so a runtime that only advertises the KHR
    // profile still gives a working trigger.
    // Touch first (Quest 3 through VDXR or SteamVR), then the KHR simple
    // controller as a fallback so a runtime that advertises only that profile
    // still gets a working trigger. The simple profile has no trigger/value or
    // squeeze at all, which is why the two binding sets differ.
    auto bindProfile = [&](const char* profile, const char* togglePath, const char* markerPath) {
        XrPath prof{};
        if (XR_FAILED(xrStringToPath(g_instance, profile, &prof))) return;
        std::vector<XrActionSuggestedBinding> b;
        auto add = [&](XrAction a, const char* side, const char* suffix) {
            XrPath path{};
            const std::string p = std::string("/user/hand/") + side + suffix;
            if (XR_SUCCEEDED(xrStringToPath(g_instance, p.c_str(), &path))) b.push_back({a, path});
        };
        for (const char* side : {"left", "right"}) {
            add(act.pose, side, "/input/grip/pose");
            add(act.toggle, side, togglePath);
            add(act.marker, side, markerPath);
            add(act.haptic, side, "/output/haptic");
        }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = prof;
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings = b.data();
        const XrResult sr = xrSuggestInteractionProfileBindings(g_instance, &sb);
        printf("bindings %-48s : %s\n", profile, XR_SUCCEEDED(sr) ? "ok" : Res(sr));
    };
    bindProfile("/interaction_profiles/oculus/touch_controller", "/input/trigger/value",
                "/input/squeeze/value");
    bindProfile("/interaction_profiles/khr/simple_controller", "/input/select/click",
                "/input/menu/click");

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    if (Fail(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) return 1;

    // ---- spaces ------------------------------------------------------------
    // LOCAL: gravity-aligned, origin at the head where the session started -
    // the right choice for a seated capture and it needs no configured play
    // area. Every pose in the CSV is in THIS space, so all three are comparable.
    XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (Fail(xrCreateReferenceSpace(session, &rsci, &localSpace), "LOCAL space")) return 1;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (Fail(xrCreateReferenceSpace(session, &rsci, &viewSpace), "VIEW space")) return 1;

    XrSpace handSpace[2]{};
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asp.action = act.pose;
        asp.subactionPath = hands[i];
        asp.poseInActionSpace.orientation.w = 1.0f;
        if (Fail(xrCreateActionSpace(session, &asp, &handSpace[i]), "action space")) return 1;
    }

    // ---- csv ---------------------------------------------------------------
    const std::string path = OutputPath();
    FILE* csv = nullptr;
    if (fopen_s(&csv, path.c_str(), "w") != 0 || !csv) {
        printf("FAIL: cannot write %s\n", path.c_str());
        return 1;
    }
    fprintf(csv, "# armcap32 capture\n");
    fprintf(csv, "# runtime=%s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    fprintf(csv, "# space=LOCAL (gravity-aligned, origin at session start), units=metres\n");
    fprintf(csv, "# elbow_controller=%s hand_controller=%s\n", elbowIsLeft ? "left" : "right",
            elbowIsLeft ? "right" : "left");
    fprintf(csv, "# trk=1 means BOTH position and orientation TRACKED. DISCARD trk=0 rows -\n");
    fprintf(csv, "#   they are IMU dead-reckoning, not observation, and they drift silently.\n");
    fprintf(csv, "# marker increments on a grip press; use it to separate pose runs.\n");
    fprintf(csv,
            "sample,t_ns,rec,marker,"
            "head_px,head_py,head_pz,head_qx,head_qy,head_qz,head_qw,head_trk,head_flags,"
            "l_px,l_py,l_pz,l_qx,l_qy,l_qz,l_qw,l_trk,l_flags,"
            "r_px,r_py,r_pz,r_qx,r_qy,r_qz,r_qw,r_trk,r_flags\n");

    printf("\nwriting %s\n\n", path.c_str());
    printf("  EITHER TRIGGER = start/stop recording   (1 buzz on, 2 buzzes off)\n");
    printf("  EITHER GRIP    = drop a marker          (separates pose runs)\n");
    printf("  a tick every 15 s means it is still recording\n");
    printf("  close this window, or take the headset off, to finish\n\n");

    // ---- frame loop --------------------------------------------------------
    bool ready = false, running = false, recording = false, quit = false;
    bool prevToggle = false, prevMarker = false;
    int marker = 0;
    uint64_t samples = 0, kept = 0, dropped = 0;
    uint64_t recStartMs = 0, lastTickMs = 0, lastStatusMs = 0;
    BuzzPattern buzz;

    while (!quit) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                if (sc->state == XR_SESSION_STATE_READY) ready = true;
                if (sc->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                    quit = true;
                }
                if (sc->state == XR_SESSION_STATE_EXITING ||
                    sc->state == XR_SESSION_STATE_LOSS_PENDING)
                    quit = true;
            }
            if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) quit = true;
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (quit) break;

        if (ready && !running) {
            XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
            sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (Fail(xrBeginSession(session, &sbi), "xrBeginSession")) break;
            running = true;
            printf("session running - put the headset on and pull a trigger to start\n");
        }
        if (!running) {
            Sleep(50);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) break;
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        // No layers submitted: the headset stays black on purpose. Nothing here
        // needs to be seen, and rendering would only add a way to fail.
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 0;

        {
            // Synced every frame, not only when shouldRender - actions read as
            // inactive while unfocused anyway, and gating input on the render
            // hint is how a trigger press gets silently eaten.
            XrActiveActionSet aas{actionSet, XR_NULL_PATH};
            XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
            si.countActiveActionSets = 1;
            si.activeActionSets = &aas;
            xrSyncActions(session, &si);

            auto readBool = [&](XrAction a) {
                bool any = false;
                for (int i = 0; i < 2; ++i) {
                    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                    gi.action = a;
                    gi.subactionPath = hands[i];
                    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
                    if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive &&
                        st.currentState)
                        any = true;
                }
                return any;
            };

            const uint64_t nowMs = GetTickCount64();
            buzz.pump(session, act.haptic, hands, nowMs);

            // Rising edge only - a held trigger must not toggle every frame.
            const bool tog = readBool(act.toggle);
            if (tog && !prevToggle) {
                recording = !recording;
                if (recording) {
                    recStartMs = nowMs;
                    lastTickMs = nowMs;
                    printf("[   0.0s] RECORDING\n");
                } else {
                    printf("[%6.1fs] stopped (%llu kept, %llu dropped untracked)\n",
                           (nowMs - recStartMs) / 1000.0, kept, dropped);
                    fflush(csv);
                }
                buzz.start(recording ? 1 : 2);
            }
            prevToggle = tog;

            const bool mk = readBool(act.marker);
            if (mk && !prevMarker && recording) {
                ++marker;
                printf("[%6.1fs] marker %d\n", (nowMs - recStartMs) / 1000.0, marker);
                Buzz(session, act.haptic, hands, 0.05f, 0.6f);
            }
            prevMarker = mk;

            if (recording) {
                XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
                xrLocateSpace(viewSpace, localSpace, fs.predictedDisplayTime, &head);
                XrSpaceLocation hand[2] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
                for (int i = 0; i < 2; ++i)
                    xrLocateSpace(handSpace[i], localSpace, fs.predictedDisplayTime, &hand[i]);

                const bool allTracked = FullyTracked(head.locationFlags) &&
                                        FullyTracked(hand[0].locationFlags) &&
                                        FullyTracked(hand[1].locationFlags);
                allTracked ? ++kept : ++dropped;

                auto wr = [&](const XrSpaceLocation& l) {
                    fprintf(csv, ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,0x%X", l.pose.position.x,
                            l.pose.position.y, l.pose.position.z, l.pose.orientation.x,
                            l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w,
                            FullyTracked(l.locationFlags) ? 1 : 0,
                            static_cast<unsigned>(l.locationFlags));
                };
                fprintf(csv, "%llu,%lld,1,%d", samples, static_cast<long long>(fs.predictedDisplayTime),
                        marker);
                wr(head);
                wr(hand[0]);
                wr(hand[1]);
                fputc('\n', csv);
                ++samples;

                if (nowMs - lastTickMs >= kAliveTickMs) {
                    lastTickMs = nowMs;
                    Buzz(session, act.haptic, hands, 0.04f, 0.4f);
                }
                if (nowMs - lastStatusMs >= 5000) {
                    lastStatusMs = nowMs;
                    fflush(csv);
                    printf("[%6.1fs] %llu samples, %llu tracked (%.0f%%), marker %d\n",
                           (nowMs - recStartMs) / 1000.0, samples, kept,
                           samples ? 100.0 * kept / samples : 0.0, marker);
                }
                if ((nowMs - recStartMs) / 1000.0 > kMaxRecordSeconds) {
                    printf("auto-stop: %.0f minute safety limit reached\n", kMaxRecordSeconds / 60);
                    recording = false;
                    fflush(csv);
                    buzz.start(3);
                }
            }
        }

        xrEndFrame(session, &fei);
    }

    // One long buzz on the way out. No pattern here - the loop that would pump
    // it has already exited.
    if (running) Buzz(session, act.haptic, hands, 0.6f, 1.0f);

    printf("\n%llu samples written, %llu fully tracked (%.1f%%), %llu dropped, %d markers\n",
           samples, kept, samples ? 100.0 * kept / samples : 0.0, dropped, marker);
    if (samples && kept * 10 < samples * 6)
        printf("WARNING: under 60%% tracked. The elbow controller was out of the cameras for\n"
               "         most of this run - keep the arm further forward and retry.\n");
    printf("csv: %s\n", path.c_str());

    fclose(csv);
    if (running) xrEndSession(session);
    xrDestroySession(session);
    if (context) context->Release();
    if (device) device->Release();
    if (found) found->Release();
    if (factory) factory->Release();
    xrDestroyInstance(g_instance);
    return samples > 0 ? 0 : 2;
}
