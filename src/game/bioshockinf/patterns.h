#pragma once
// BioShock Infinite (BioShockInfinite.exe) signature table. This header and
// patterns.cpp are the ONLY files allowed to contain raw addresses/offsets for
// this game; every entry is documented in docs/bioshockinfinite/ENGINE_NOTES.md
// with its derivation method.
//
// Infinite is Unreal Engine 3 build 6829 with large Irrational replacements -
// NOT the Vengeance (UE2.5) tree the two remasters share. NEVER copy a number
// from bioshock1r/patterns.h or bioshock2r/patterns.h: different engine,
// different link, and here even the SHAPES are suspect (UE3 restructured
// UObject, FName, the actor attachment model and the skeleton representation).

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::bsi::patterns {

// ---- host build identity (session 34, PE parse of the shipped exe) ----------
// Every RVA below was derived from ONE build of BioShockInfinite.exe:
// PE TimeDateStamp 0x627BE455 (2022-05-11 18:29:09 UTC), SizeOfImage
// 0x0124F000, 18,368,840 bytes on disk, sha256 C20A4252...14A0E77.
//
// The adapter is chosen by exe BASENAME and every storefront ships the same
// file name, so a differently-linked build is not rejected - it is ACCEPTED and
// then mis-addressed, which is how a mod corrupts a game rather than failing to
// work. Address-dependent features stand down on a mismatch; the log, overlay
// and command seam keep working, and the game stays fully playable.
inline constexpr uint32_t kHostTimeDateStamp = 0x627BE455;
inline constexpr uint32_t kHostSizeOfImage = 0x0124F000;
inline constexpr uint64_t kHostFileBytes = 18368840;
// Logged for the record only - module_id::matches does not test the checksum
// (BS1's exe carries 0, so the field is not universally usable).
inline constexpr uint32_t kHostCheckSum = 0x011590C3;

// ASLR is OFF on this exe (DllCharacteristics 0x8100, no DYNAMIC_BASE), unlike
// both remasters - it loads at a fixed base. That is a convenience, not a
// licence: everything below stays an RVA so a rebased build (or a storefront
// that turns ASLR back on) costs nothing.
inline constexpr uint32_t kExpectedImageBase = 0x00400000;

// ---- the camera seam (derived offline session 34, UNCONFIRMED live) --------
// APlayerController::GetPlayerViewPoint. Reached from the exec thunk, which is
// how it was FOUND and is never a hook target: a static E8 caller census gives
// the thunk 0 callers and the implementation 14 (13 of them native call sites).
// thiscall, 2 stack args (FVector* outLoc, FRotator* outRot), `ret 8` - and
// `ret imm / 4 == 2` is the argument count any probe hook must use, or the RTC
// dialog that writes no crash dump is the result.
//
// I1 consumes these READ-ONLY, to log the bytes actually living there. Hooking
// is I2 (DR-I2).
inline constexpr uint32_t kGetPlayerViewPointRva = 0x1E10C0;
inline constexpr uint32_t kGetPlayerViewPointThunkRva = 0x129280;

// The first 12 bytes living at kGetPlayerViewPointRva, read live in session 35
// and used as an install-time gate: if the running build does not have exactly
// these bytes here, the RVA means something else and the hook REFUSES rather
// than detouring whatever is there.
//   55              push ebp
//   8B EC           mov ebp, esp
//   83 E4 F0        and esp, -0x10        <- aligned stack, for the SSE transform
//   81 EC A4 00 00 00  sub esp, 0xA4
// Session 35 read 16 bytes; only the first 12 are used, because bytes 12-15
// (53 56 8B 75) begin `push ebx; push esi; mov esi,[ebp+disp]` whose
// displacement byte falls outside the window and is not worth pinning.
inline constexpr uint8_t kGetPlayerViewPointPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0,
                                                          0x81, 0xEC, 0xA4, 0x00, 0x00, 0x00};

// `ret 8` - and 8/4 == 2 is the number of stack args any detour on this target
// MUST declare. Verified at install time by finding C2 08 00 in the body.
inline constexpr uint8_t kGetPlayerViewPointRetImm = 8;

// ---- APlayerController / camera object layout ------------------------------
// Decoded from GetPlayerViewPoint's four internal paths (session 34, offline).
// Session 36 promoted +0x240 from INFERRED to OBSERVED: 40 of 40 live path
// samples found the flag clear and this pointer non-null, i.e. every sample
// took path 2 and read the POV out of the camera object.
//
// These live here rather than in camera.cpp because of the standing rule that
// engine offsets exist in patterns.h and nowhere else.
inline constexpr uint32_t kPcCachedPovFlagOffset = 0x248;  // bit 0: use the cached POV
inline constexpr uint32_t kPcCameraOffset = 0x240;         // lazily-created camera object
inline constexpr uint32_t kPcCachedLocOffset = 0x24C;      // FVector, path 1's source
inline constexpr uint32_t kPcCachedRotOffset = 0x258;      // FRotator, path 1's source
inline constexpr uint32_t kPcViewTransformOffset = 0x430;  // 0x40 bytes -> a 4x4 SSE transform
inline constexpr uint32_t kCameraPovLocOffset = 0x3B8;     // FVector, path 2's source
inline constexpr uint32_t kCameraPovRotOffset = 0x3C4;     // FRotator, path 2's source
// AActor, from paths 3 and 4 reading identical offsets off two different
// objects - which is what made the reading credible rather than a guess.
inline constexpr uint32_t kActorLocationOffset = 0x44;
inline constexpr uint32_t kActorRotationOffset = 0x50;

// ---- UE3 reflection (derived offline session 36, DR-I1) --------------------
//
// Derivation, in full, because it is reusable and it is what made the frameless
// functions in this exe tractable:
//   1. Collect EVERY E8 call target in .text (38,638 of them). That set is the
//      complete list of known function entry points, so "the function
//      containing address X" is "greatest entry <= X" by binary search. No
//      `CC CC CC 55 8B EC` backward walk, hence no silent "returned the
//      PREVIOUS function" failure - which is exactly how that heuristic breaks
//      on this exe (the exec thunk at 0x129280 is frameless).
//   2. `Failed to find function` exists as a UTF-16 literal at .rdata 0xD07CC0
//      (appErrorf takes TCHAR). Its only code xref is 0xD1131, which step 1
//      resolves to the function at 0xD1090 = UObject::FindFunctionChecked.
//   3. That function has 426 E8 callers - the generated event stubs. In each,
//      the call that FOLLOWS is ProcessEvent. 407 of 420 decodable callers
//      agree on vtable offset +0x7C.
//   4. Sweeping .rdata/.data for vtable candidates (runs of >= 24 consecutive
//      dwords pointing into an executable section) and reading slot +0x7C from
//      each gives 1038 votes for 0xCFE70 and 175 for 0x19A150.
//
// TRAP worth recording: MSVC hoists the vtable pointer into a register, so the
// stub reads `mov edi,[esi]` ... `call FindFunctionChecked` ...
// `mov edx,[edi+0x7C]` / `call edx`. Searching for `call [reg+disp]` (FF /2)
// finds ZERO of the 426. A decoder must resolve `call reg` through the
// preceding load.
inline constexpr uint32_t kProcessEventRva = 0xCFE70;
inline constexpr uint32_t kActorProcessEventRva = 0x19A150;
inline constexpr uint32_t kFindFunctionCheckedRva = 0xD1090;

// UObject vtable byte offset of ProcessEvent. Recorded because a vtable read
// survives a rebuild that moves the function, while kProcessEventRva does not -
// the RVA is the cross-check, the slot is the durable locator.
inline constexpr uint32_t kProcessEventVtableOffset = 0x7C;
// UObject::FindFunction, read out of FindFunctionChecked's own body
// (`mov eax,[esi]; mov edx,[eax+0x54]; call edx`, then the null test that
// produces the appErrorf above). Consumed by `bsicall` (session 37): the slot
// is the locator, the RVA below is the interlock.
inline constexpr uint32_t kFindFunctionVtableOffset = 0x54;
// UObject::FindFunction implementation RVA, derived twice in agreement:
// (a) offline session 36 - FindFunctionChecked's body dispatches through
// [vtable+0x54], and the function immediately preceding FFC's 0xD1090 in .text
// starts at 0xD1030; (b) live session 36 - slot +0x54 of a latched
// APlayerController's vtable read 0xD1030. `bsicall` refuses to dispatch when
// the live slot disagrees with this, so a re-linked build cannot be called
// through a slot that now means something else.
inline constexpr uint32_t kFindFunctionRva = 0xD1030;

// BOTH are thiscall with 3 stack args, `ret 0xC`, confirmed as the only ret imm
// in each body (ProcessEvent 672 bytes, FindFunctionChecked 304 bytes, both
// int3-padded to the next function). 0xC/4 == 3 is the argument count any
// detour on either MUST declare, or the RTC dialog that writes no crash dump
// is the result.
inline constexpr uint8_t kProcessEventRetImm = 0x0C;
inline constexpr uint8_t kFindFunctionCheckedRetImm = 0x0C;

// ---- GNames / FName (derived offline session 34) ---------------------------
// GNames is a UE3 TArray<FNameEntry*> - the classic { Data, Num, Max } triple -
// traced from the hardcoded UnNames.h string run.
//
// TIMING, and it is load-bearing: our DLL is pulled in from the proxy's
// DllMain during the exe's import resolution, which runs BEFORE the exe's CRT
// static initializers and long before UE3 registers any name. At
// patterns::resolve() time this array is empty. Every reader below must be
// command- or hook-driven, never init-driven.
inline constexpr uint32_t kGNamesDataRva = 0xF9DFEC;
inline constexpr uint32_t kGNamesNumRva = 0xF9DFF0;
inline constexpr uint32_t kGNamesMaxRva = 0xF9DFF4;

// Name hash table: `mov [eax*4 + 0xF58BF8], ebp` after `and eax, 0xfff`.
// RECORDED, NOT CONSUMED - using it means calling the engine's own hash
// (0x80C70 ASCII / 0x80C10 wide), whereas a linear GNames walk is both safer
// and INDEPENDENT of the derivation above, which is what lets the two
// cross-check each other.
inline constexpr uint32_t kNameHashRva = 0xF58BF8;
inline constexpr uint32_t kNameHashBuckets = 4096;

// FNameEntry, decoded from the pool allocator and the registration function.
//
// THE DIFFERENCE FROM BS1 IS WHAT MAKES A NAIVE PORT RETURN GARBAGE: BS1's text
// at +0x10 is ALWAYS UTF-16. Here +0x8 packs (index << 1) | isWide, bit 0
// selects the encoding, and ASCII is the DEFAULT (the allocator copies 5 bytes
// for "None"). Two different hash functions are called on the flag, 0x80C70 vs
// 0x80C10, which is how the bit was identified.
inline constexpr uint32_t kFNameEntryIndexFlagsOffset = 0x8;
inline constexpr uint32_t kFNameEntryHashNextOffset = 0xC;
inline constexpr uint32_t kFNameEntryTextOffset = 0x10;
inline constexpr uint32_t kFNameEntryWideBit = 0x1;

// UE3's NAME_SIZE. Nothing legitimate is longer, and the cap is what stops a
// recycled or corrupt entry walking off the end of its page.
inline constexpr size_t kFNameMaxChars = 1024;
// Below this fname_text() refuses rather than truncating: a truncated name
// silently mis-keys a per-name filter, which is worse than no name at all.
inline constexpr size_t kFNameTextBufMin = 64;

// UObject::Class, from execIsA (`mov eax,[edi+0x20]`).
inline constexpr uint32_t kUObjectClassOffset = 0x20;

// ---- the lens (DR-I3, derived live session 36) -----------------------------
//
// Infinite does NOT carry BS1's and BS2's 7-float screen-ray helper
// (2tanH, 0, -tanH, 0, 0, -2tanV, tanV). It ships a 4x4, so core's
// hud::set_ray_block_offset / decode_ray_block CANNOT consume this and the
// adapter deliberately publishes nothing to them - a Vengeance-shaped decoder
// pointed at float 0 would either fail or, worse, false-positive.
//
// DERIVATION. `dumpframe cb` (frame_inspector mode 3) captured 1891 constant-
// buffer uploads in gameplay. Sweeping every offset of every block for a 4x4
// whose three columns are mutually orthogonal yields 139 candidates; filtering
// on `tanH/tanV == backbuffer aspect` leaves exactly ONE. That filter is
// load-bearing, not decoration: the top four candidates by block count are all
// degenerate tanH==tanV pairs, and the best of them had MORE support (108
// blocks) than the true answer (93). Plurality is not evidence here.
//
// CONFIRMED BY A FALSIFIABLE PREDICTION, which is what promotes it from
// candidate to fact. Moving the in-game FOV slider from minimum to maximum
// moved this block and nothing else about it:
//     slider min:  tanH 0.7674  tanV 0.4317  hFOV 75.01 deg  aspect 1.77762
//     slider max:  tanH 0.8770  tanV 0.4933  hFOV 82.50 deg  aspect 1.77782
// Both axes scaled by the SAME ratio (1.14282 vs 1.14269) with the aspect held
// at 16:9 to 0.002 % - the signature of a FOV change and of nothing else.
//
// tanH = |c3| / |c0| and tanV = |c3| / |c1| for the column triples of the
// row-major 4x4, so the object scale cancels. That is what makes this readable
// from a PER-OBJECT constant buffer, where nothing else is constant.
inline constexpr uint32_t kLensCbBytes = 80;      // the tier that carries it
inline constexpr uint32_t kLensFloatIndex = 0;    // float offset within it
inline constexpr bool kLensRowMajor = true;

// THE FOV LAW (session 37 aspect cross-check, ENGINE_NOTES "The FOV law"):
// the option is VERTICAL-referenced and tanH = tanV x aspect (Hor+). Slider
// min pins tanV at 0.4317 at BOTH tested aspects (16:9 and 4:3), slider max
// at 0.4933; vFOV 46.67..52.63 deg at any aspect. These are the frustum's own
// numbers (the ini's FOVAngle is decorative - see the warning below). I5's
// projection claim is 2*atan(tanV x aspect); the default assumes the slider
// at MINIMUM (the shipped default), correctable live via `bsifov tanv` and
// verifiable against a `dumpframe cb` decode.
inline constexpr float kTanVSliderMin = 0.4317f;
inline constexpr float kTanVSliderMax = 0.4933f;

// ---- the live FOV chain on the camera object (session 41, derived live) ----
//
// DERIVATION (poke/rescan at the attract, ENGINE_NOTES "LIVE RESULTS (session
// 41)"). With the in-game slider at max the frustum decodes tanH 0.8770 and
// the camera object [pc+0x240] carries 82.50f - and tan(82.5/2) = 0.8770
// EXACTLY, so the stored value is the horizontal FOV in degrees at the current
// aspect. Copies live at [cam+0x214] (followed by two 1.3333f floats - the
// UE3 ACamera DefaultFOV / DefaultAspectRatio shape) and [cam+0x3D0] (the
// cached POV: loc at +0x3B8, rot at +0x3C4, fov at +0x3D0 - the FTPOV
// layout). A full writable-memory scan for 82.5f found SIX holders and every
// single one snapped back to 82.5 within a tick of being poked (including
// these two), so the value is RECOMPUTED each tick from the option upstream:
// no memory address is the source, and the console `set` lane never writes
// XUserOptionsManager.FieldOfView at all (a scan for the written value found
// zero stable holders - recorded negative, do not re-try `set` here).
// THE LEVER therefore ENFORCES per camera-detour dispatch: writing both
// copies every GetPlayerViewPoint call outruns the once-per-tick refresh,
// and disarming self-restores - the engine's own recompute is the undo.
inline constexpr uint32_t kCameraDefaultFovOffset = 0x214; // f32 deg, [cam+...]
inline constexpr uint32_t kCameraPovFovOffset = 0x3D0;     // f32 deg, [cam+...]
// The camera degrees value is HORIZONTAL AT A FIXED 16:9 REFERENCE, not at
// the current aspect: the engine pins tanV = tan(deg/2) / (16/9) and derives
// tanH = tanV x actual aspect (Hor+ - the s37 law restated with its anchor).
// MEASURED at 1440x1440 with the lever enforcing 100 deg: both decoders read
// tanH = tanV = 0.6704 = tan(50 deg)/1.7778 exactly (predicting tan(50) =
// 1.19175 from a current-aspect reading was WRONG and the claim audit caught
// it at 43.7% off - session 41). At 16:9 the two readings coincide, which is
// why the 16:9-only measurements could not separate them.
inline constexpr float kFovRefAspect = 16.0f / 9.0f;

// ---- the scene-build root (session 40, derived live) -----------------------
//
// The SequentialReentry seam: the UGameViewportClient::Draw analog. DERIVATION
// (three independent live routes agreeing, ENGINE_NOTES session 40):
//  1. Caller census at the GetPlayerViewPoint detour: return RVA 0x26B499
//     fires EXACTLY once per present (810 in 810 presents) on the game thread,
//     inside a loop that walks a linked list ([esi+0x208]) doing the
//     constant-time UClass-interval IsA check before sampling each player
//     controller's view - the per-frame scene camera sweep.
//  2. One-shot RtlCaptureStackBackTrace from that caller: dispatched from
//     return RVA 0x1FE05F, whose site is `mov ecx,[viewport+0x1C];
//     mov edx,[ecx]; mov edx,[edx+8]; call edx` bracketed by a stack canvas's
//     ctor (0x331110) / dtor (0x3339F0) - the engine's per-tick redraw.
//  3. One-shot live stack probe at that dispatch (`bsicam scenedraw`):
//     client [viewport+0x1C] -> vtable in .rdata at 0xDE6FC8 -> slot 2
//     (byte +0x8) = 0x6F1360, a `jmp 0x26A3E0` stub. Entry confirmed by
//     prologue disassembly; body spans the census call site 0x26B494.
//
// __thiscall, ecx = the viewport client, TWO stack args (viewport, canvas),
// `ret 8`. NOT the doubling root: doubling this function was TRIED first
// (session 40) and produced camera+scene doubling with NO second present -
// the tag ring skewed +1 per tick (self-healed at depth 3) because the
// present is kicked by the CALLER's tail, not by this call tree. Kept as
// derivation facts and for the install-time chain cross-check.
inline constexpr uint32_t kSceneDrawRva = 0x26A3E0;       // client Draw body
inline constexpr uint32_t kSceneDrawStubRva = 0x6F1360;   // vtable slot target (jmp stub)
inline constexpr uint32_t kSceneDrawVtableRva = 0xDE6FC8; // .rdata, slot 2 = +0x8
// The `call edx` return address of the client-Draw dispatch (inside the
// viewport draw root below) - the camera-side scene probe keys on it.
inline constexpr uint32_t kSceneDispatchRetRva = 0x1FE05F;
// The census fact, for reference: the GetPlayerViewPoint call site inside the
// scene draw's controller loop.
inline constexpr uint32_t kSceneGpvpCallSiteRetRva = 0x26B499;

// THE DOUBLING ROOT: the viewport draw - FViewport::Draw(bShouldPresent)'s
// analog, one level ABOVE the client draw. Its body is: stack canvas ctor
// (0x331110) -> client->Draw dispatch (ret 0x1FE05F) -> canvas dtor
// (0x3339F0) -> the present kick (call 0x1E50B0 with arg 1). Doubling THIS
// yields camera + scene + present per eye, which is what the SR tag ring
// requires (one present per pushed eye tag). Entry resolved by walking the
// live stack scrape one frame above the dispatch: ret 0x206309's E8 call
// targets 0x1FDE30 directly, and the forward stream from 0x1FDE30 reaches
// the dispatch at 0x1FE05D (capstone). __thiscall, ecx = the viewport, ONE
// stack arg, `ret 4` (at RVA 0x1FE0AB).
inline constexpr uint32_t kViewportDrawRva = 0x1FDE30;
inline constexpr uint8_t kViewportDrawPrologue[] = {0x6A, 0xFF, 0x68, 0x36, 0xCE, 0x02,
                                                    0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
inline constexpr uint8_t kViewportDrawRetImm = 4;
// The gameplay caller's return RVA (pe-xref: 4 static callers - 0x118086,
// 0x1FFD5C, 0x2017CA, 0x206304; the live census names 0x206309, the return
// of 0x206304's call, as the per-tick dispatcher). Pass 2 is deny-by-default
// on this value, jointly with the camera-silent and present-stall gates
// (BS2's three-gate design).
inline constexpr uint32_t kViewportDrawGameplayRetRva = 0x206309;

// **A CONFIG VALUE IS A CLAIM, NOT A MEASUREMENT.** `XEngine.ini` says
// `FOVAngle=70` and `MaxUserFOVOffsetPercent=15`, which session 34 read as "the
// native slider spans roughly 70 to 80.5 degrees". The RENDERED frustum spans
// **75.01 to 82.50 degrees horizontal** at 16:9. Neither endpoint matches, and
// the tangent ratio across the slider is 1.1428 rather than the 1.2094 that
// 70-to-80.5 predicts. Do not build I5's FOV law on the ini numbers; derive it
// from the frustum, and at two aspects rather than one.

// False when the running exe is not the build the addresses came from. Anything
// consuming a raw RVA must check this first.
bool rva_trusted();

// Force the gate closed at RUNTIME, to exercise the stand-down path without
// building a sabotaged DLL that can be mistaken for a real one (BS1 session 27
// learned that the expensive way). `buildgate off|on|status` on the command
// seam.
void handle_buildgate_command(const char* args);

// What resolve() observed, for the overlay and the status command.
struct Symbols {
    const uint8_t* imageBase = nullptr;
    size_t imageSize = 0;
    bool buildVerified = false;
    // First bytes at kGetPlayerViewPointRva, read-only. Zeroed if the gate is
    // closed or the address is not readable.
    bool viewPointReadable = false;
    uint8_t viewPointBytes[16] = {};
};

// Runs the build gate and the read-only probes. Never hooks, never writes,
// never scans. Returns false when the host build is not the one every RVA came
// from; the adapter stays alive either way (fail soft).
bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

// The last resolve() result, for the overlay and `bsi` status.
const Symbols& symbols();

// Live image base, cached by resolve(). Null before resolve() has run. Every
// RVA above is turned into an address through this and nothing else.
const uint8_t* image_base();
size_t image_size();

// Turns an RVA into a live address, or null when the gate is closed, resolve()
// has not run, or the RVA is outside the image. This is the ONLY sanctioned way
// to consume a constant from this header.
const uint8_t* rva_to_address(uint32_t rva, size_t needBytes);

// ---- GNames readers (session 36) -------------------------------------------
//
// ALL OF THESE FAIL SOFT AND MUST NOT BE CALLED FROM init(). See the timing
// note beside kGNamesDataRva: GNames is empty at DllMain time, so a call that
// arrives too early returns false and says "not populated yet" rather than
// reading a half-built array.

// GNames.Num / GNames.Max, or 0 when unavailable.
int32_t fname_count();
int32_t fname_max();

// Copies GNames[index]'s text into `out` as NUL-terminated printable ASCII (a
// wide entry is transcoded, anything outside 0x20..0x7E becoming '?'), so the
// result is always safe to print with %s. Cannot return a bare pointer the way
// BS1's does: half this pool may be UTF-16, and a caller printing the wrong
// encoding gets mojibake instead of an error.
//
// Returns false - and writes an empty string - on ANY of: the build gate
// closed; GNames not yet populated; index outside [0, Num); the slot, entry or
// text unreadable; THE ENTRY'S OWN INDEX at (+0x8 >> 1) disagreeing with
// `index`; no terminator within kFNameMaxChars; or outSize < kFNameTextBufMin.
bool fname_text(int32_t index, char* out, size_t outSize);

// The encoding flag on its own, so a selftest can report a pool with zero wide
// entries as UNTESTED rather than silently assuming the wide path works.
bool fname_is_wide(int32_t index, bool& outWide);

// Linear, case-insensitive GNames lookup. Returns -1 on miss.
//
// ONE-SHOT ONLY - never on a cadence. BS1 session 22 established that a
// per-frame or per-second scan of an engine-sized table reads to the player as
// "the game freezes every couple of seconds". Deliberately NOT the engine's
// hash table at kNameHashRva: an independent instrument is the whole point,
// because it is what lets the two disagree.
int32_t fname_find(const char* text);

} // namespace bvr::bsi::patterns
