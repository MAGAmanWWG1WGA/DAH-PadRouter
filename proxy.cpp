// DAHPadRouter (standalone) - XInput proxy DLL for Destroy All Humans! (2020, UE 4.22)
//
// Built as xinput1_3.dll / xinput1_4.dll / xinput9_1_0.dll and placed next to the game exe.
// The game loads it instead of the Windows one. It:
//   * forwards every XInput call to the real Windows XInput DLL,
//   * makes the game see Player 2's pad (HidePadIndex) as connected but idle,
//   * feeds that pad's real buttons/sticks/triggers into Unreal's own input routing
//     (UGameViewportClient::InputKey / InputAxis) as ControllerId P2ControllerId, so the
//     engine delivers them to Player 2's PlayerController like a normal pad.
// The viewport address and the gamepad key-name IDs come from the DAHPadLink Lua mod
// (UE4SS), which writes them to padrouter_handshake.txt.
//
// Hotkeys (game focused): Ctrl+Shift+P = probe for the InputKey/InputAxis slots,
//                         Ctrl+Shift+O = toggle on/off.
// Files next to the game exe: DAHPadRouter.ini (settings), DAHPadRouter.log (log).

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00  // Windows 10: makes Xinput.h declare the battery/keystroke types
#define WINVER 0x0A00
#include <Windows.h>
#include <Xinput.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
// Paths and logging
// ============================================================================
static HMODULE g_Self = nullptr;
static std::wstring g_ExeDir;
static std::wstring g_SelfName;  // e.g. "xinput1_3" (without .dll)
static std::mutex g_LogMutex;
static FILE* g_Log = nullptr;

static std::wstring DirOf(const wchar_t* path)
{
    std::wstring s(path);
    auto pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

static void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (!g_Log)
    {
        g_Log = _wfopen((g_ExeDir + L"\\DAHPadRouter_" + g_SelfName + L".log").c_str(), L"w");
        if (!g_Log) { return; }
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::fprintf(g_Log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_Log, fmt, args);
    va_end(args);
    std::fputc('\n', g_Log);
    std::fflush(g_Log);
}

// ============================================================================
// Config (DAHPadRouter.ini next to the game exe)
// ============================================================================
struct Config
{
    bool Enabled = true;
    int HidePadIndex = 1;          // XInput slot of Player 2's pad (0 = first pad)
    int P2ControllerId = 1;        // Unreal ControllerId of Player 2 (DAH2P creates it with 1)
    int InputKeySlot = 0;          // found by the probe; 0 = not set (pad is only hidden)
    int InputAxisSlot = 0;         // found by the probe
    int ViewportClientOffset = 0;  // 0 = auto-detect the FViewportClient part of the viewport object
    int ViewportPtrOffset = 0x98;  // UGameViewportClient::Viewport (from UE4SS.log member offsets)
    int Mode = 1;                  // 1 = feed Player 2's PlayerInput directly, 0 = go through the viewport
    int PlayerInputKeySlot = 0;    // found by the PlayerInput probe (Ctrl+Shift+I)
    int PlayerInputAxisSlot = 0;
    int AimFix = 1;                // 1 = each split-screen player aims at the middle of their own view
    // Where each player aims, as a fraction of the whole screen, measured from the corner of
    // that player's own view (the game measures from there). Re-read while the game runs.
    float AimP1X = 0.25f, AimP2X = 0.25f;   // left/right split
    float AimP1Y0 = 0.0f, AimP2Y0 = 0.0f;   // top/bottom split: added to the half-height aim point
    int PlayerContext = 1;         // 1 = the game's code asking for "the player" while working on
                                   //     Player 2's things gets Player 2 (lock-on, aiming, pickups)
};
static Config g_Cfg;

static void LoadConfig()
{
    FILE* f = _wfopen((g_ExeDir + L"\\DAHPadRouter.ini").c_str(), L"r");
    if (!f)
    {
        Log("DAHPadRouter.ini not found next to the game exe, using defaults");
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == ';') { continue; }
        char* eq = std::strchr(line, '=');
        if (!eq) { continue; }
        *eq = 0;
        std::string key(line);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) { key.pop_back(); }
        long v = std::strtol(eq + 1, nullptr, 0);  // base 0: accepts 0x..
        const float fv = float(std::strtod(eq + 1, nullptr));
        if (key == "Enabled") { g_Cfg.Enabled = v != 0; }
        else if (key == "HidePadIndex") { g_Cfg.HidePadIndex = int(v); }
        else if (key == "P2ControllerId") { g_Cfg.P2ControllerId = int(v); }
        else if (key == "InputKeySlot") { g_Cfg.InputKeySlot = int(v); }
        else if (key == "InputAxisSlot") { g_Cfg.InputAxisSlot = int(v); }
        else if (key == "ViewportClientOffset") { g_Cfg.ViewportClientOffset = int(v); }
        else if (key == "ViewportPtrOffset") { g_Cfg.ViewportPtrOffset = int(v); }
        else if (key == "Mode") { g_Cfg.Mode = int(v); }
        else if (key == "PlayerInputKeySlot") { g_Cfg.PlayerInputKeySlot = int(v); }
        else if (key == "PlayerInputAxisSlot") { g_Cfg.PlayerInputAxisSlot = int(v); }
        else if (key == "AimFix") { g_Cfg.AimFix = int(v); }
        else if (key == "AimP1X") { g_Cfg.AimP1X = fv; }
        else if (key == "PlayerContext") { g_Cfg.PlayerContext = int(v); }
        else if (key == "AimP2X") { g_Cfg.AimP2X = fv; }
        else if (key == "AimP1Y0") { g_Cfg.AimP1Y0 = fv; }
        else if (key == "AimP2Y0") { g_Cfg.AimP2Y0 = fv; }
    }
    std::fclose(f);
    Log("Config: Enabled=%d HidePadIndex=%d Mode=%d (%s) P2ControllerId=%d InputKeySlot=%d InputAxisSlot=%d "
        "PlayerInputKeySlot=%d PlayerInputAxisSlot=%d",
        g_Cfg.Enabled, g_Cfg.HidePadIndex, g_Cfg.Mode, g_Cfg.Mode == 1 ? "PlayerInput" : "viewport", g_Cfg.P2ControllerId,
        g_Cfg.InputKeySlot, g_Cfg.InputAxisSlot, g_Cfg.PlayerInputKeySlot, g_Cfg.PlayerInputAxisSlot);
}

// ============================================================================
// Real XInput (loaded from System32)
// ============================================================================
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using XInputEnableFn = void(WINAPI*)(BOOL);
using XInputGetDSoundAudioDeviceGuidsFn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
using XInputGetBatteryInformationFn = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
using XInputGetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
using XInputGetAudioDeviceIdsFn = DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
using XInputWaitForGuideButtonFn = DWORD(WINAPI*)(DWORD, DWORD, LPVOID);
using XInputUserIndexFn = DWORD(WINAPI*)(DWORD);

static struct RealXInput
{
    HMODULE module = nullptr;
    XInputGetStateFn GetState = nullptr;
    XInputGetStateFn GetStateEx = nullptr;  // ordinal 100
    XInputSetStateFn SetState = nullptr;
    XInputGetCapabilitiesFn GetCapabilities = nullptr;
    XInputEnableFn Enable = nullptr;
    XInputGetDSoundAudioDeviceGuidsFn GetDSoundAudioDeviceGuids = nullptr;
    XInputGetBatteryInformationFn GetBatteryInformation = nullptr;
    XInputGetKeystrokeFn GetKeystroke = nullptr;
    XInputGetAudioDeviceIdsFn GetAudioDeviceIds = nullptr;
    XInputWaitForGuideButtonFn WaitForGuideButton = nullptr;  // ordinal 101
    XInputUserIndexFn CancelGuideButtonWait = nullptr;        // ordinal 102
    XInputUserIndexFn PowerOffController = nullptr;           // ordinal 103
} g_Real;

static void StartWorker();
static void InitModuleRanges();

static void LoadRealXInput()
{
    static std::once_flag once;
    std::call_once(once, [] {
        LoadConfig();

        wchar_t selfPath[MAX_PATH]{};
        GetModuleFileNameW(g_Self, selfPath, MAX_PATH);
        std::wstring selfName(selfPath);
        selfName = selfName.substr(selfName.find_last_of(L"\\/") + 1);

        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        const std::wstring candidates[] = {selfName, L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const auto& name : candidates)
        {
            g_Real.module = LoadLibraryW((std::wstring(sysDir) + L"\\" + name).c_str());
            if (g_Real.module)
            {
                Log("Loaded as %ls; forwarding to %ls\\%ls", selfName.c_str(), sysDir, name.c_str());
                break;
            }
        }
        if (!g_Real.module)
        {
            Log("ERROR: no Windows XInput DLL found in %ls", sysDir);
            return;
        }
        auto get = [](const char* n) { return GetProcAddress(g_Real.module, n); };
        g_Real.GetState = reinterpret_cast<XInputGetStateFn>(get("XInputGetState"));
        g_Real.GetStateEx = reinterpret_cast<XInputGetStateFn>(GetProcAddress(g_Real.module, MAKEINTRESOURCEA(100)));
        g_Real.SetState = reinterpret_cast<XInputSetStateFn>(get("XInputSetState"));
        g_Real.GetCapabilities = reinterpret_cast<XInputGetCapabilitiesFn>(get("XInputGetCapabilities"));
        g_Real.Enable = reinterpret_cast<XInputEnableFn>(get("XInputEnable"));
        g_Real.GetDSoundAudioDeviceGuids = reinterpret_cast<XInputGetDSoundAudioDeviceGuidsFn>(get("XInputGetDSoundAudioDeviceGuids"));
        g_Real.GetBatteryInformation = reinterpret_cast<XInputGetBatteryInformationFn>(get("XInputGetBatteryInformation"));
        g_Real.GetKeystroke = reinterpret_cast<XInputGetKeystrokeFn>(get("XInputGetKeystroke"));
        g_Real.GetAudioDeviceIds = reinterpret_cast<XInputGetAudioDeviceIdsFn>(get("XInputGetAudioDeviceIds"));
        g_Real.WaitForGuideButton = reinterpret_cast<XInputWaitForGuideButtonFn>(GetProcAddress(g_Real.module, MAKEINTRESOURCEA(101)));
        g_Real.CancelGuideButtonWait = reinterpret_cast<XInputUserIndexFn>(GetProcAddress(g_Real.module, MAKEINTRESOURCEA(102)));
        g_Real.PowerOffController = reinterpret_cast<XInputUserIndexFn>(GetProcAddress(g_Real.module, MAKEINTRESOURCEA(103)));

        InitModuleRanges();
        StartWorker();
    });
}

// ============================================================================
// Game module layout helpers
// ============================================================================
struct Range
{
    uintptr_t begin = 0, end = 0;
    bool contains(uintptr_t p) const { return p >= begin && p < end; }
};
static Range g_Text, g_RData;

static void InitModuleRanges()
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        char name[9]{};
        std::memcpy(name, sec->Name, 8);
        Range r{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (std::strcmp(name, ".text") == 0) { g_Text = r; }
        else if (std::strcmp(name, ".rdata") == 0) { g_RData = r; }
    }
    Log("Game exe: .text %p-%p  .rdata %p-%p", (void*)g_Text.begin, (void*)g_Text.end, (void*)g_RData.begin, (void*)g_RData.end);
}

static bool SafeReadPtr(uintptr_t addr, uintptr_t& out)
{
    __try
    {
        out = *reinterpret_cast<uintptr_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool LooksLikeVTable(uintptr_t vt)
{
    uintptr_t first{};
    return g_RData.contains(vt) && SafeReadPtr(vt, first) && g_Text.contains(first);
}

// UGameViewportClient inherits UObject first and FViewportClient second, so
// InputKey/InputAxis live in a second vtable inside the object. Find it.
static uintptr_t FindViewportClientPart(uintptr_t obj, int& outOffset)
{
    if (g_Cfg.ViewportClientOffset > 0)
    {
        uintptr_t vt{};
        if (SafeReadPtr(obj + g_Cfg.ViewportClientOffset, vt) && LooksLikeVTable(vt))
        {
            outOffset = g_Cfg.ViewportClientOffset;
            return obj + outOffset;
        }
        return 0;
    }
    for (int off = 0x28; off <= 0x60; off += 8)
    {
        uintptr_t vt{};
        if (SafeReadPtr(obj + off, vt) && LooksLikeVTable(vt))
        {
            outOffset = off;
            return obj + off;
        }
    }
    return 0;
}

static int CountVTableEntries(uintptr_t vt)
{
    int n = 0;
    for (; n < 400; ++n)
    {
        uintptr_t fn{};
        if (!SafeReadPtr(vt + n * sizeof(void*), fn) || !g_Text.contains(fn)) { break; }
    }
    return n;
}

static void WriteSlot(uintptr_t vt, int index, uintptr_t value)
{
    auto slot = reinterpret_cast<uintptr_t*>(vt + index * sizeof(void*));
    DWORD old{};
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), reinterpret_cast<PVOID>(value));
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// ============================================================================
// Unreal input structs (UE 4.22, x64)
// ============================================================================
struct FKeyRaw
{
    uint64_t Name;        // FName (ComparisonIndex + Number)
    void* DetailsObject;  // TSharedPtr<FKeyDetails>; null = engine looks it up by name
    void* DetailsRefController;
};
static_assert(sizeof(FKeyRaw) == 0x18, "FKey size");

struct FInputKeyEventArgsRaw
{
    void* Viewport;
    int32_t ControllerId;
    int32_t Pad0;
    FKeyRaw Key;
    int32_t Event;  // EInputEvent: 0 pressed, 1 released
    float AmountDepressed;
    bool bIsTouchEvent;
};
static_assert(offsetof(FInputKeyEventArgsRaw, Key) == 0x10, "Key offset");
static_assert(offsetof(FInputKeyEventArgsRaw, Event) == 0x28, "Event offset");

using InputKeyFn = bool (*)(void* self, FInputKeyEventArgsRaw* args);
using InputAxisFn = bool (*)(void* self, void* viewport, int32_t controllerId, FKeyRaw* key,
                             float delta, float deltaTime, int32_t numSamples, bool bGamepad);

// UPlayerInput::InputKey(FKey Key, EInputEvent Event, float AmountDepressed, bool bGamepad)
// UPlayerInput::InputAxis(FKey Key, float Delta, float DeltaTime, int32 NumSamples, bool bGamepad)
using PIKeyFn = bool (*)(void* self, FKeyRaw* key, int32_t event, float amountDepressed, bool bGamepad);
using PIAxisFn = bool (*)(void* self, FKeyRaw* key, float delta, float deltaTime, int32_t numSamples, bool bGamepad);
static std::atomic<uintptr_t> g_P1Input{0};   // Player 1's PlayerInput (used by the probe)
static std::atomic<uintptr_t> g_P2Input{0};   // Player 2's PlayerInput (where pad 2 goes)
static std::atomic<uintptr_t> g_InputVTable{0};
static std::atomic<PIKeyFn> g_PIKey{nullptr};
static std::atomic<PIAxisFn> g_PIAxis{nullptr};
static std::atomic<bool> g_ResetPad{false};
static std::atomic<bool> g_SpawnRequested{false};  // Start pressed on pad 2 while Player 2 doesn't exist   // Player 2 changed: forget held buttons without sending

static std::atomic<uintptr_t> g_ViewportObj{0};
static std::atomic<uintptr_t> g_ViewportPart{0};
static std::atomic<InputKeyFn> g_InputKey{nullptr};
static std::atomic<InputAxisFn> g_InputAxis{nullptr};
static std::atomic<bool> g_Enabled{true};
static std::atomic<bool> g_Probing{false};

// ============================================================================
// Pad -> Unreal key mapping
// ============================================================================
enum ButtonKey
{
    BK_DPadUp, BK_DPadDown, BK_DPadLeft, BK_DPadRight,
    BK_Start, BK_Back, BK_LeftThumb, BK_RightThumb,
    BK_LeftShoulder, BK_RightShoulder, BK_A, BK_B, BK_X, BK_Y,
    BK_LeftTrigger, BK_RightTrigger,
    BK_LStickUp, BK_LStickDown, BK_LStickLeft, BK_LStickRight,
    BK_RStickUp, BK_RStickDown, BK_RStickLeft, BK_RStickRight,
    BK_Count
};
enum AxisKey { AK_LeftX, AK_LeftY, AK_RightX, AK_RightY, AK_LeftTrigger, AK_RightTrigger, AK_Count };

static const char* const kButtonNames[BK_Count] = {
    "Gamepad_DPad_Up", "Gamepad_DPad_Down", "Gamepad_DPad_Left", "Gamepad_DPad_Right",
    "Gamepad_Special_Right", "Gamepad_Special_Left", "Gamepad_LeftThumbstick", "Gamepad_RightThumbstick",
    "Gamepad_LeftShoulder", "Gamepad_RightShoulder",
    "Gamepad_FaceButton_Bottom", "Gamepad_FaceButton_Right", "Gamepad_FaceButton_Left", "Gamepad_FaceButton_Top",
    "Gamepad_LeftTrigger", "Gamepad_RightTrigger",
    "Gamepad_LeftStick_Up", "Gamepad_LeftStick_Down", "Gamepad_LeftStick_Left", "Gamepad_LeftStick_Right",
    "Gamepad_RightStick_Up", "Gamepad_RightStick_Down", "Gamepad_RightStick_Left", "Gamepad_RightStick_Right",
};
static const char* const kAxisNames[AK_Count] = {
    "Gamepad_LeftX", "Gamepad_LeftY", "Gamepad_RightX", "Gamepad_RightY",
    "Gamepad_LeftTriggerAxis", "Gamepad_RightTriggerAxis",
};
static uint64_t g_ButtonFNames[BK_Count]{};
static uint64_t g_AxisFNames[AK_Count]{};
static std::atomic<bool> g_NamesReady{false};

static float ShortToFloat(SHORT v) { return v <= 0 ? v / 32768.0f : v / 32767.0f; }

static void ComputeButtons(const XINPUT_GAMEPAD& g, std::array<bool, BK_Count>& b)
{
    const WORD w = g.wButtons;
    b[BK_DPadUp] = (w & XINPUT_GAMEPAD_DPAD_UP) != 0;
    b[BK_DPadDown] = (w & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    b[BK_DPadLeft] = (w & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    b[BK_DPadRight] = (w & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    b[BK_Start] = (w & XINPUT_GAMEPAD_START) != 0;
    b[BK_Back] = (w & XINPUT_GAMEPAD_BACK) != 0;
    b[BK_LeftThumb] = (w & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    b[BK_RightThumb] = (w & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    b[BK_LeftShoulder] = (w & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    b[BK_RightShoulder] = (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    b[BK_A] = (w & XINPUT_GAMEPAD_A) != 0;
    b[BK_B] = (w & XINPUT_GAMEPAD_B) != 0;
    b[BK_X] = (w & XINPUT_GAMEPAD_X) != 0;
    b[BK_Y] = (w & XINPUT_GAMEPAD_Y) != 0;
    b[BK_LeftTrigger] = g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    b[BK_RightTrigger] = g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    b[BK_LStickUp] = g.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    b[BK_LStickDown] = g.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    b[BK_LStickLeft] = g.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    b[BK_LStickRight] = g.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    b[BK_RStickUp] = g.sThumbRY > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    b[BK_RStickDown] = g.sThumbRY < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    b[BK_RStickLeft] = g.sThumbRX < -XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    b[BK_RStickRight] = g.sThumbRX > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
}

static void ComputeAxes(const XINPUT_GAMEPAD& g, std::array<float, AK_Count>& a)
{
    a[AK_LeftX] = ShortToFloat(g.sThumbLX);
    a[AK_LeftY] = ShortToFloat(g.sThumbLY);
    a[AK_RightX] = ShortToFloat(g.sThumbRX);
    a[AK_RightY] = ShortToFloat(g.sThumbRY);
    a[AK_LeftTrigger] = g.bLeftTrigger / 255.0f;
    a[AK_RightTrigger] = g.bRightTrigger / 255.0f;
}

// Game-thread state
static std::array<bool, BK_Count> g_PrevButtons{};
static std::array<float, AK_Count> g_PrevAxes{};
static LARGE_INTEGER g_LastInjectTime{};
static bool g_LoggedFirstInject = false;

static bool PlayerInputValid(uintptr_t input)
{
    uintptr_t vt{};
    return input && SafeReadPtr(input, vt) && vt == g_InputVTable.load();
}

static void SendKey(int index, bool pressed)
{
    if (g_Cfg.Mode == 1)
    {
        auto fn = g_PIKey.load();
        const uintptr_t input = g_P2Input.load();
        if (!fn || !PlayerInputValid(input)) { return; }
        FKeyRaw key{};  // fresh every call: passed by value, the engine destroys its copy
        key.Name = g_ButtonFNames[index];
        fn(reinterpret_cast<void*>(input), &key, pressed ? 0 : 1, 1.0f, true);
        return;
    }

    auto fn = g_InputKey.load();
    auto self = reinterpret_cast<void*>(g_ViewportPart.load());
    uintptr_t viewport{};
    if (!fn || !self || !SafeReadPtr(g_ViewportObj.load() + g_Cfg.ViewportPtrOffset, viewport)) { return; }

    FInputKeyEventArgsRaw args{};  // fresh every call: the engine may fill Key's details
    args.Viewport = reinterpret_cast<void*>(viewport);
    args.ControllerId = g_Cfg.P2ControllerId;
    args.Key.Name = g_ButtonFNames[index];
    args.Event = pressed ? 0 : 1;
    args.AmountDepressed = 1.0f;
    fn(self, &args);
}

static void SendAxis(int index, float value, float dt)
{
    if (g_Cfg.Mode == 1)
    {
        const uintptr_t input = g_P2Input.load();
        if (!PlayerInputValid(input)) { return; }
        FKeyRaw key{};
        key.Name = g_AxisFNames[index];
        if (auto axisFn = g_PIAxis.load())
        {
            axisFn(reinterpret_cast<void*>(input), &key, value, dt, 1, true);
        }
        else if (auto keyFn = g_PIKey.load())
        {
            // UPlayerInput::InputKey stores AmountDepressed as the key's raw value, so a
            // "press" with the stick/trigger value sets that axis, and a release sets it to 0.
            keyFn(reinterpret_cast<void*>(input), &key, value != 0.0f ? 0 : 1, value, true);
        }
        return;
    }

    auto fn = g_InputAxis.load();
    auto self = reinterpret_cast<void*>(g_ViewportPart.load());
    uintptr_t viewport{};
    if (!fn || !self || !SafeReadPtr(g_ViewportObj.load() + g_Cfg.ViewportPtrOffset, viewport)) { return; }

    FKeyRaw key{};  // fresh every call: passed by value, the engine destroys its copy
    key.Name = g_AxisFNames[index];
    fn(self, reinterpret_cast<void*>(viewport), g_Cfg.P2ControllerId, &key, value, dt, 1, true);
}

static bool InjectionReady()
{
    if (g_Probing || !g_NamesReady) { return false; }
    if (g_Cfg.Mode == 1) { return g_PIKey.load() && g_P2Input.load(); }  // axis slot is optional
    return g_InputKey.load() && g_InputAxis.load() && g_ViewportPart.load();
}

// Called on the game thread every time the game polls Player 2's pad.
static void RoutePad(const XINPUT_GAMEPAD* pad)
{
    if (g_ResetPad.exchange(false))
    {
        g_PrevButtons = {};
        g_PrevAxes = {};
    }
    std::array<bool, BK_Count> buttons{};
    std::array<float, AK_Count> axes{};
    if (pad)
    {
        ComputeButtons(*pad, buttons);
        ComputeAxes(*pad, axes);
    }

    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_LastInjectTime.QuadPart ? float(double(now.QuadPart - g_LastInjectTime.QuadPart) / double(freq.QuadPart)) : 0.016f;
    if (dt <= 0.0f || dt > 0.25f) { dt = 0.016f; }
    g_LastInjectTime = now;

    for (int i = 0; i < BK_Count; ++i)
    {
        if (buttons[i] != g_PrevButtons[i]) { SendKey(i, buttons[i]); }
    }
    for (int i = 0; i < AK_Count; ++i)
    {
        if (axes[i] != g_PrevAxes[i]) { SendAxis(i, axes[i], dt); }
    }
    g_PrevButtons = buttons;
    g_PrevAxes = axes;

    if (pad && !g_LoggedFirstInject)
    {
        g_LoggedFirstInject = true;
        Log("Routing pad %d to ControllerId %d (first frame sent)", g_Cfg.HidePadIndex, g_Cfg.P2ControllerId);
    }
}

static bool AnyHeld()
{
    for (bool b : g_PrevButtons) { if (b) { return true; } }
    for (float a : g_PrevAxes) { if (a != 0.0f) { return true; } }
    return false;
}

// ============================================================================
// Who is calling, and pad diagnostics
// ============================================================================
static std::atomic<DWORD> g_GameThreadId{0};  // thread that owns the game window (UE's game thread)
static ULONGLONG g_MonitorUntil = 0;          // log pad activity until this tick count
static std::atomic<int> g_MonitorLines{0};

static std::string ModuleNameOf(void* address)
{
    HMODULE mod{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &mod))
    {
        return "?";
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    return name ? name + 1 : path;
}

static bool CalledByGame(void* returnAddress)
{
    const DWORD gameThread = g_GameThreadId.load();
    if (gameThread && GetCurrentThreadId() == gameThread) { return true; }
    return g_Text.contains(reinterpret_cast<uintptr_t>(returnAddress));
}

// Logs, per XInput slot: first time it is read (and by whom), connect/disconnect, and
// button changes during the monitor window. Called on every XInputGetState.
static void MonitorPad(DWORD userIndex, DWORD result, const XINPUT_STATE* state, void* returnAddress)
{
    if (userIndex > 3) { return; }
    static std::atomic<int> lastResult[4] = {-1, -1, -1, -1};
    static WORD lastButtons[4]{};
    static bool loggedCaller[4]{};

    if (!loggedCaller[userIndex])
    {
        loggedCaller[userIndex] = true;
        Log("Slot %lu first read by %s (thread %lu, game thread %lu)", userIndex,
            ModuleNameOf(returnAddress).c_str(), GetCurrentThreadId(), g_GameThreadId.load());
    }
    if (lastResult[userIndex].exchange(int(result)) != int(result))
    {
        Log("Slot %lu: %s", userIndex, result == ERROR_SUCCESS ? "pad CONNECTED" : "no pad");
    }
    if (result == ERROR_SUCCESS && state && GetTickCount64() < g_MonitorUntil)
    {
        const WORD b = state->Gamepad.wButtons;
        if (b != lastButtons[userIndex] && g_MonitorLines.fetch_add(1) < 300)
        {
            Log("Slot %lu buttons: 0x%04X%s", userIndex, b, b & XINPUT_GAMEPAD_A ? "  (A held)" : "");
        }
        lastButtons[userIndex] = b;
    }
}

// ============================================================================
// Exported XInput functions
// ============================================================================
extern "C" DWORD WINAPI Proxy_XInputGetState(DWORD userIndex, XINPUT_STATE* state)
{
    LoadRealXInput();
    if (!g_Real.GetState) { return ERROR_DEVICE_NOT_CONNECTED; }
    DWORD result = g_Real.GetState(userIndex, state);
    void* const caller = _ReturnAddress();
    MonitorPad(userIndex, result, state, caller);
    if (int(userIndex) != g_Cfg.HidePadIndex) { return result; }

    if (!g_Enabled)
    {
        if (CalledByGame(caller) && AnyHeld() && InjectionReady())
        {
            RoutePad(nullptr);  // release anything still held
        }
        return result;
    }

    // Only the game's own polling (game thread) may drive Player 2. Other callers, like an
    // overlay, still see the pad as idle but never inject input.
    const bool fromGame = CalledByGame(caller);

    const bool connected = result == ERROR_SUCCESS && state;

    // Start on Player 2's pad before Player 2 exists = ask DAH2P to spawn Player 2.
    static bool startWasDown = false;
    const bool startDown = connected && (state->Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
    if (fromGame && startDown && !startWasDown && !g_P2Input.load()) { g_SpawnRequested = true; }
    startWasDown = startDown;

    XINPUT_GAMEPAD pad{};
    if (connected)
    {
        pad = state->Gamepad;
        state->Gamepad = XINPUT_GAMEPAD{};  // the game sees a connected pad that is never touched
        state->dwPacketNumber = 1;
    }
    if (fromGame && InjectionReady()) { RoutePad(connected ? &pad : nullptr); }
    return result;
}

extern "C" DWORD WINAPI Proxy_XInputGetStateEx(DWORD userIndex, XINPUT_STATE* state)
{
    LoadRealXInput();
    if (!g_Real.GetStateEx) { return Proxy_XInputGetState(userIndex, state); }
    DWORD result = g_Real.GetStateEx(userIndex, state);
    if (int(userIndex) == g_Cfg.HidePadIndex && g_Enabled && result == ERROR_SUCCESS && state)
    {
        state->Gamepad = XINPUT_GAMEPAD{};
        state->dwPacketNumber = 1;
    }
    return result;
}

extern "C" DWORD WINAPI Proxy_XInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration)
{
    LoadRealXInput();
    return g_Real.SetState ? g_Real.SetState(userIndex, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputGetCapabilities(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    LoadRealXInput();
    return g_Real.GetCapabilities ? g_Real.GetCapabilities(userIndex, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" void WINAPI Proxy_XInputEnable(BOOL enable)
{
    LoadRealXInput();
    if (g_Real.Enable) { g_Real.Enable(enable); }
}

extern "C" DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD userIndex, GUID* render, GUID* capture)
{
    LoadRealXInput();
    return g_Real.GetDSoundAudioDeviceGuids ? g_Real.GetDSoundAudioDeviceGuids(userIndex, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD userIndex, BYTE devType, XINPUT_BATTERY_INFORMATION* info)
{
    LoadRealXInput();
    return g_Real.GetBatteryInformation ? g_Real.GetBatteryInformation(userIndex, devType, info) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputGetKeystroke(DWORD userIndex, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{
    LoadRealXInput();
    if (int(userIndex) == g_Cfg.HidePadIndex && g_Enabled) { return ERROR_EMPTY; }
    return g_Real.GetKeystroke ? g_Real.GetKeystroke(userIndex, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputGetAudioDeviceIds(DWORD userIndex, LPWSTR render, UINT* renderCount, LPWSTR capture, UINT* captureCount)
{
    LoadRealXInput();
    return g_Real.GetAudioDeviceIds ? g_Real.GetAudioDeviceIds(userIndex, render, renderCount, capture, captureCount) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD userIndex, DWORD flags, LPVOID overlapped)
{
    LoadRealXInput();
    return g_Real.WaitForGuideButton ? g_Real.WaitForGuideButton(userIndex, flags, overlapped) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD userIndex)
{
    LoadRealXInput();
    return g_Real.CancelGuideButtonWait ? g_Real.CancelGuideButtonWait(userIndex) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputPowerOffController(DWORD userIndex)
{
    LoadRealXInput();
    return g_Real.PowerOffController ? g_Real.PowerOffController(userIndex) : ERROR_DEVICE_NOT_CONNECTED;
}

// ============================================================================
// Handshake from the DAHPadLink Lua mod
// ============================================================================
// ============================================================================
// Split-screen aim fix
// ----------------------------------------------------------------------------
// The game asks one function (on BFGActorComponent_TargetSystem) for the screen point
// that every weapon, lock-on and ability aims at. Stock code returns the middle of the
// WHOLE screen (raised by m_fCrosshairOffsetY), which in split screen is the line between
// the two views. We replace that function with a copy that returns the middle of the
// calling player's own half. Which player owns which target component comes from the
// DAHPadLink handshake (p1target= / p2target= / layout=). Everything else is unchanged.
// The function is found by its bytes, so the fix keeps working if the exe moves code.
// ============================================================================
struct FVec2 { float X, Y; };
struct FIntPt { int32_t X, Y; };
using GetSizeXYFn = FIntPt* (*)(void* viewport, FIntPt* out);

static std::atomic<uintptr_t> g_P1Target{0};
static std::atomic<uintptr_t> g_P2Target{0};
static std::atomic<int> g_LayoutVertical{1};   // 1 = left/right, 0 = top/bottom
static uintptr_t* g_GEnginePtr = nullptr;      // address of the GEngine global
static int32_t g_OffGameViewport = 0, g_OffViewport = 0, g_OffAltFlag = 0, g_OffAltOffset = 0, g_OffOffset = 0;
static std::atomic<bool> g_AimFixOn{false};
static std::atomic<long> g_AimCalls{0};

static FVec2* AimCrosshairHook(uint8_t* tc, FVec2* out)
{
    out->X = 0.0f;
    out->Y = 0.0f;
    const uintptr_t engine = g_GEnginePtr ? *g_GEnginePtr : 0;
    if (!engine) { return out; }
    const uintptr_t gv = *reinterpret_cast<uintptr_t*>(engine + g_OffGameViewport);
    if (!gv) { return out; }
    void* vp = *reinterpret_cast<void**>(gv + g_OffViewport);
    if (!vp) { return out; }
    auto vtable = *reinterpret_cast<uintptr_t**>(vp);
    FIntPt size{};
    const FIntPt* s = reinterpret_cast<GetSizeXYFn>(vtable[3])(vp, &size);
    const float w = float(s->X), h = float(s->Y);
    const float off = tc[g_OffAltFlag] ? *reinterpret_cast<float*>(tc + g_OffAltOffset)
                                       : *reinterpret_cast<float*>(tc + g_OffOffset);
    float x = w * 0.5f;
    float y = (off + 1.0f) * h * 0.5f;   // same as the game: offset is a fraction of half the height

    const uintptr_t self = reinterpret_cast<uintptr_t>(tc);
    const uintptr_t p1 = g_P1Target.load(std::memory_order_relaxed);
    const uintptr_t p2 = g_P2Target.load(std::memory_order_relaxed);
    if (p2 && (self == p1 || self == p2))
    {
        const bool second = self == p2;
        if (g_LayoutVertical.load(std::memory_order_relaxed))
        {
            x = w * (second ? g_Cfg.AimP2X : g_Cfg.AimP1X);   // middle of the player's own half
        }
        else
        {
            const float half = (off + 1.0f) * h * 0.25f;      // same point inside a half-height view
            y = h * (second ? g_Cfg.AimP2Y0 : g_Cfg.AimP1Y0) + half;
        }
        g_AimCalls.fetch_add(1, std::memory_order_relaxed);
    }
    out->X = x;
    out->Y = y;
    return out;
}

static uintptr_t g_AimFn = 0;
static void InstallAimFix()
{
    // 48 89 5C 24 08 57 48 83 EC 20 48 8B 05 <GEngine> 48 8B DA 48 8B F9 ... (see the comment above)
    static const char* kPattern =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B DA 48 8B F9 48 85 C0 0F 84 ?? ?? ?? ?? "
        "48 8B 80 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 48 8D 54 24 40 "
        "FF 50 18 48 8B 08 66 0F 6E D1 48 C1 E9 20 80 BF ?? ?? ?? ?? 00 0F 5B D2 66 0F 6E D9 0F 5B DB 74 0A "
        "F3 0F 10 8F ?? ?? ?? ?? EB 08 F3 0F 10 8F ?? ?? ?? ?? F3 0F 58 0D";
    std::vector<int> pat;
    for (const char* p = kPattern; *p;)
    {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { pat.push_back(-1); p += 2; continue; }
        pat.push_back(int(std::strtol(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    if (!g_Text.begin) { Log("Aim fix: game code not located"); return; }
    uintptr_t found = 0;
    int matches = 0;
    for (uintptr_t a = g_Text.begin; a + pat.size() < g_Text.end; ++a)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(a);
        if (b[0] != 0x48 || b[1] != 0x89) { continue; }
        size_t i = 0;
        for (; i < pat.size(); ++i) { if (pat[i] >= 0 && b[i] != pat[i]) { break; } }
        if (i == pat.size()) { if (!found) { found = a; } ++matches; }
    }
    if (matches != 1)
    {
        Log("Aim fix: aim-point function %s (%d matches); split-screen aim stays stock",
            matches ? "ambiguous" : "not found", matches);
        return;
    }
    auto rd32 = [&](int at) { int32_t v; std::memcpy(&v, reinterpret_cast<void*>(found + at), 4); return v; };
    g_GEnginePtr = reinterpret_cast<uintptr_t*>(found + 17 + rd32(13));
    g_OffGameViewport = rd32(35);
    g_OffViewport = rd32(47);
    g_OffAltFlag = rd32(80);
    g_OffAltOffset = rd32(101);
    g_OffOffset = rd32(111);

    // Jump from the start of the game's function to ours:  jmp qword ptr [rip+0] ; <address>
    uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const uintptr_t target = reinterpret_cast<uintptr_t>(&AimCrosshairHook);
    std::memcpy(patch + 6, &target, 8);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(found), sizeof(patch), PAGE_EXECUTE_READWRITE, &old))
    {
        Log("Aim fix: could not unprotect the game code");
        return;
    }
    std::memcpy(reinterpret_cast<void*>(found), patch, sizeof(patch));
    VirtualProtect(reinterpret_cast<void*>(found), sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(found), sizeof(patch));
    g_AimFixOn = true;
    g_AimFn = found;
    Log("Aim fix: installed at %p (GEngine %p, offsets %X/%X/%X/%X/%X)", (void*)found, (void*)g_GEnginePtr,
        g_OffGameViewport, g_OffViewport, g_OffAltFlag, g_OffAltOffset, g_OffOffset);
}

// ============================================================================
// Player context: "which player is this code working for?"
// ----------------------------------------------------------------------------
// The game's own code (BFGCore) asks the world for "the first player controller" in
// ~130 places: to aim, lock on, project targets onto the screen, pick things up... In
// split screen that always answers Player 1. We replace that question with a version
// that looks at what the calling code is working on (the CPU registers that hold its
// objects): if it's one of Player 2's objects (character, components, weapons,
// abilities...), it gets Player 2's controller. Player 2's code then also measures the
// screen from the corner of Player 2's own view (project/deproject below), the same way
// the aim point does. The object lists come from the DAHPadLink handshake (p1obj=/p2obj=).
// ============================================================================
struct PlayerSets
{
    std::vector<uintptr_t> p1, p2;  // sorted
    uintptr_t p2pc = 0;
};
static std::atomic<const PlayerSets*> g_Sets{nullptr};
static uintptr_t g_BfgBegin = 0, g_BfgEnd = 0;     // where the game's own code lives
static uintptr_t g_GfpcWeakGet = 0;                 // TWeakObjectPtr::Get used by GetFirstPlayerController
static int32_t g_OffPcList = 0, g_OffPcCount = 0;   // UWorld::PlayerControllerList data / count
static uintptr_t g_ProjectInner = 0, g_DeprojectInner = 0;
static std::atomic<long> g_CtxP2Hits{0};

using WeakGetFn = void* (*)(void* weakPtr);
static void* OriginalFirstPC(uint8_t* world)
{
    if (!world || *reinterpret_cast<int32_t*>(world + g_OffPcCount) <= 0) { return nullptr; }
    void* data = *reinterpret_cast<void**>(world + g_OffPcList);
    return reinterpret_cast<WeakGetFn>(g_GfpcWeakGet)(data);
}

static bool InSet(const std::vector<uintptr_t>& v, uintptr_t x)
{
    return x && std::binary_search(v.begin(), v.end(), x);
}

// regs: rbx, rdi, rsi, r14, r15, r12, r13, rbp (in that order: most likely "this" first)
static void* FirstPCHook(uint8_t* world, const uintptr_t* regs, uintptr_t ret)
{
    const PlayerSets* s = g_Sets.load(std::memory_order_acquire);
    if (s && s->p2pc && g_Cfg.PlayerContext && ret >= g_BfgBegin && ret < g_BfgEnd)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (InSet(s->p2, regs[i])) { g_CtxP2Hits.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<void*>(s->p2pc); }
            if (InSet(s->p1, regs[i])) { break; }
        }
    }
    return OriginalFirstPC(world);
}

// UGameInstance::GetFirstLocalPlayerController: the HUD (radar, shield bar, threat display)
// asks this one instead of the world. Same rule as above; the original runs from a small
// copy of its first instructions (the "trampoline").
using FirstLocalPCFn = void* (*)(void* gameInstance);
static FirstLocalPCFn g_FirstLocalPCOrig = nullptr;
static void* FirstLocalPCHook(void* gameInstance, const uintptr_t* regs, uintptr_t ret)
{
    const PlayerSets* s = g_Sets.load(std::memory_order_acquire);
    if (s && s->p2pc && g_Cfg.PlayerContext && ret >= g_BfgBegin && ret < g_BfgEnd)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (InSet(s->p2, regs[i])) { g_CtxP2Hits.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<void*>(s->p2pc); }
            if (InSet(s->p1, regs[i])) { break; }
        }
    }
    return g_FirstLocalPCOrig(gameInstance);
}

static bool IsP2Controller(const void* pc)
{
    const PlayerSets* s = g_Sets.load(std::memory_order_acquire);
    return s && s->p2pc && reinterpret_cast<uintptr_t>(pc) == s->p2pc;
}

static bool ViewportSizeF(float& w, float& h)
{
    const uintptr_t engine = g_GEnginePtr ? *g_GEnginePtr : 0;
    if (!engine) { return false; }
    const uintptr_t gv = *reinterpret_cast<uintptr_t*>(engine + g_OffGameViewport);
    if (!gv) { return false; }
    void* vp = *reinterpret_cast<void**>(gv + g_OffViewport);
    if (!vp) { return false; }
    auto vtable = *reinterpret_cast<uintptr_t**>(vp);
    FIntPt size{};
    const FIntPt* r = reinterpret_cast<GetSizeXYFn>(vtable[3])(vp, &size);
    w = float(r->X);
    h = float(r->Y);
    return w > 0 && h > 0;
}

struct FVec3 { float X, Y, Z; };
using ProjectInnerFn = bool (*)(const void* pc, const FVec3* world, FVec2* screen, bool relative);
using DeprojectInnerFn = bool (*)(const void* pc, const FVec2* screen, FVec3* loc, FVec3* dir);

// APlayerController::ProjectWorldLocationToScreen: Player 2 gets positions measured
// from the corner of their own view, like their aim point.
static bool ProjectHook(const void* pc, const FVec3* world, FVec2* screen, bool relative)
{
    if (g_Cfg.PlayerContext && IsP2Controller(pc)) { relative = true; }
    return reinterpret_cast<ProjectInnerFn>(g_ProjectInner)(pc, world, screen, relative);
}

// APlayerController::DeprojectScreenPositionToWorld: the engine wants whole-screen pixels,
// so add the corner of Player 2's view back on.
static bool DeprojectHook(const void* pc, float x, float y, FVec3* loc, FVec3* dir)
{
    if (g_Cfg.PlayerContext && IsP2Controller(pc))
    {
        float w = 0, h = 0;
        if (ViewportSizeF(w, h))
        {
            if (g_LayoutVertical.load(std::memory_order_relaxed)) { x += w * 0.5f; }
            else { y += h * 0.5f; }
        }
    }
    const FVec2 sp{x, y};
    return reinterpret_cast<DeprojectInnerFn>(g_DeprojectInner)(pc, &sp, loc, dir);
}

// BFGActorComponent_TargetSystem's own "my player controller" helper. Its owner isn't in a
// register any more when it asks the world, so it is answered here from 'this' directly.
static uintptr_t g_TcGetWorldSlow = 0;
static int32_t g_OffCompWorld = 0;
using GetWorldFn = void* (*)(void* component);
static void* TargetSystemPCHook(uint8_t* tc)
{
    const PlayerSets* s = g_Sets.load(std::memory_order_acquire);
    if (s && s->p2pc && g_Cfg.PlayerContext && InSet(s->p2, reinterpret_cast<uintptr_t>(tc)))
    {
        g_CtxP2Hits.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<void*>(s->p2pc);
    }
    void* world = *reinterpret_cast<void**>(tc + g_OffCompWorld);
    if (!world) { world = reinterpret_cast<GetWorldFn>(g_TcGetWorldSlow)(tc); }
    return OriginalFirstPC(static_cast<uint8_t*>(world));
}

static std::vector<int> ParsePattern(const char* text)
{
    std::vector<int> pat;
    for (const char* p = text; *p;)
    {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { pat.push_back(-1); p += 2; continue; }
        pat.push_back(int(std::strtol(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    return pat;
}

static uintptr_t FindUnique(const char* text, const char* what)
{
    const std::vector<int> pat = ParsePattern(text);
    uintptr_t found = 0;
    int matches = 0;
    for (uintptr_t a = g_Text.begin; a + pat.size() < g_Text.end; ++a)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(a);
        if (b[0] != pat[0]) { continue; }
        size_t i = 1;
        for (; i < pat.size(); ++i) { if (pat[i] >= 0 && b[i] != pat[i]) { break; } }
        if (i == pat.size()) { if (!found) { found = a; } ++matches; }
    }
    if (matches != 1) { Log("Player context: %s %s (%d matches)", what, matches ? "ambiguous" : "not found", matches); return 0; }
    return found;
}

static uintptr_t Rel32Target(uintptr_t at)  // at = address of the E8/E9 byte
{
    int32_t d;
    std::memcpy(&d, reinterpret_cast<void*>(at + 1), 4);
    return at + 5 + d;
}

static bool WriteCode(uintptr_t at, const void* bytes, size_t n)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) { return false; }
    std::memcpy(reinterpret_cast<void*>(at), bytes, n);
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

static bool JumpTo(uintptr_t at, uintptr_t target)
{
    uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(patch + 6, &target, 8);
    return WriteCode(at, patch, sizeof(patch));
}

static void InstallPlayerContext(uintptr_t aimFn)
{
    if (!aimFn || !g_GEnginePtr) { Log("Player context: needs the aim fix; skipped"); return; }
    const uintptr_t gfpc = FindUnique("83 B9 ?? ?? ?? ?? 00 7E ?? 48 8B 89 ?? ?? ?? ?? E9 ?? ?? ?? ?? 33 C0 C3",
                                      "GetFirstPlayerController");
    const uintptr_t project = FindUnique(
        "48 83 EC 28 E8 ?? ?? ?? ?? 48 83 C4 28 C3 CC CC 48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 "
        "48 81 EC 20 01 00 00 48 8B 99", "ProjectWorldLocationToScreen");
    const uintptr_t deproject = FindUnique(
        "48 83 EC 38 4D 8B C1 F3 0F 11 4C 24 20 4C 8B 4C 24 60 48 8D 54 24 20 F3 0F 11 54 24 24 E8 ?? ?? ?? ?? "
        "48 83 C4 38 C3", "DeprojectScreenPositionToWorld");
    if (!gfpc || !project || !deproject) { Log("Player context: not installed (game code differs)"); return; }

    std::memcpy(&g_OffPcCount, reinterpret_cast<void*>(gfpc + 2), 4);
    std::memcpy(&g_OffPcList, reinterpret_cast<void*>(gfpc + 12), 4);
    g_GfpcWeakGet = Rel32Target(gfpc + 16);
    g_ProjectInner = Rel32Target(project + 4);
    g_DeprojectInner = Rel32Target(deproject + 29);
    g_BfgBegin = aimFn - 0x180000;
    g_BfgEnd = aimFn + 0x300000;

    // Register-capturing stub for GetFirstPlayerController (see FirstPCHook).
    static const uint8_t kStub[] = {
        0x48, 0x83, 0xEC, 0x68,              // sub rsp, 0x68
        0x48, 0x89, 0x5C, 0x24, 0x20,        // mov [rsp+20h], rbx
        0x48, 0x89, 0x7C, 0x24, 0x28,        // mov [rsp+28h], rdi
        0x48, 0x89, 0x74, 0x24, 0x30,        // mov [rsp+30h], rsi
        0x4C, 0x89, 0x74, 0x24, 0x38,        // mov [rsp+38h], r14
        0x4C, 0x89, 0x7C, 0x24, 0x40,        // mov [rsp+40h], r15
        0x4C, 0x89, 0x64, 0x24, 0x48,        // mov [rsp+48h], r12
        0x4C, 0x89, 0x6C, 0x24, 0x50,        // mov [rsp+50h], r13
        0x48, 0x89, 0x6C, 0x24, 0x58,        // mov [rsp+58h], rbp
        0x48, 0x8D, 0x54, 0x24, 0x20,        // lea rdx, [rsp+20h]
        0x4C, 0x8B, 0x44, 0x24, 0x68,        // mov r8, [rsp+68h]   (return address)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, FirstPCHook
        0xFF, 0xD0,                          // call rax
        0x48, 0x83, 0xC4, 0x68,              // add rsp, 0x68
        0xC3,                                // ret
    };
    auto stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) { Log("Player context: no memory for the stub"); return; }
    std::memcpy(stub, kStub, sizeof(kStub));
    const uintptr_t hook = reinterpret_cast<uintptr_t>(&FirstPCHook);
    std::memcpy(stub + 56, &hook, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(kStub));

    const bool ok = JumpTo(gfpc, reinterpret_cast<uintptr_t>(stub)) &&
                    JumpTo(project, reinterpret_cast<uintptr_t>(&ProjectHook)) &&
                    JumpTo(deproject, reinterpret_cast<uintptr_t>(&DeprojectHook));
    // The TargetSystem helper: same bytes as the game's, and its 2nd call must go to the
    // first-player function found above.
    {
        const std::vector<int> pat = ParsePattern(
            "40 53 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 48 85 C0 75 05 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ??");
        uintptr_t helper = 0;
        int count = 0;
        for (uintptr_t a = g_Text.begin; a + pat.size() < g_Text.end; ++a)
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(a);
            if (b[0] != 0x40 || b[1] != 0x53) { continue; }
            size_t i = 2;
            for (; i < pat.size(); ++i) { if (pat[i] >= 0 && b[i] != pat[i]) { break; } }
            if (i != pat.size()) { continue; }
            if (Rel32Target(a + 0x1a) != gfpc) { continue; }
            helper = a;
            ++count;
        }
        if (count == 1)
        {
            std::memcpy(&g_OffCompWorld, reinterpret_cast<void*>(helper + 9), 4);
            g_TcGetWorldSlow = Rel32Target(helper + 0x12);
            Log("Player context: TargetSystem helper %p %s", (void*)helper,
                JumpTo(helper, reinterpret_cast<uintptr_t>(&TargetSystemPCHook)) ? "hooked" : "FAILED");
        }
        else
        {
            Log("Player context: TargetSystem helper not found (%d matches)", count);
        }
    }
    // GetFirstLocalPlayerController (HUD widgets).
    {
        const uintptr_t flpc = FindUnique("48 83 EC 38 48 8B 41 30 48 85 C0 74 09 48 8B 88 ?? ?? ?? ?? EB 02 33 C9 48 8D 54 24 20 E8",
                                          "GetFirstLocalPlayerController");
        auto mem = flpc ? static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) : nullptr;
        if (mem)
        {
            // trampoline: the original's first 20 bytes, then back into it
            uint8_t* t = mem;
            std::memcpy(t, reinterpret_cast<void*>(flpc), 11);      // sub rsp,38h / mov rax,[rcx+30h] / test rax,rax
            t[11] = 0x74; t[12] = 21;                               // je  -> null path below
            std::memcpy(t + 13, reinterpret_cast<void*>(flpc + 13), 7); // mov rcx,[rax+off]
            uint8_t jmp1[14] = {0xFF, 0x25, 0, 0, 0, 0};
            const uintptr_t back1 = flpc + 0x14, back2 = flpc + 0x16;
            std::memcpy(jmp1 + 6, &back1, 8);
            std::memcpy(t + 20, jmp1, 14);                          // jmp back after the copied code
            std::memcpy(jmp1 + 6, &back2, 8);
            std::memcpy(t + 34, jmp1, 14);                          // null path: jmp to the original's "xor ecx,ecx"
            g_FirstLocalPCOrig = reinterpret_cast<FirstLocalPCFn>(t);

            // register-capturing stub (same layout as the first-player one)
            uint8_t* st = mem + 64;
            std::memcpy(st, kStub, sizeof(kStub));
            const uintptr_t hook2 = reinterpret_cast<uintptr_t>(&FirstLocalPCHook);
            std::memcpy(st + 56, &hook2, 8);
            FlushInstructionCache(GetCurrentProcess(), mem, 4096);
            Log("Player context: HUD first-local-player %p %s", (void*)flpc,
                JumpTo(flpc, reinterpret_cast<uintptr_t>(st)) ? "hooked" : "FAILED");
        }
    }
    Log("Player context: %s (first-player %p, project %p, deproject %p, game code %p-%p)",
        ok ? "installed" : "FAILED", (void*)gfpc, (void*)project, (void*)deproject, (void*)g_BfgBegin, (void*)g_BfgEnd);
}

static FILETIME g_HandshakeTime{};
static uintptr_t g_HandshakeViewport = 0;

static bool ReadHandshake(std::vector<std::string>& lines)
{
    const std::wstring paths[] = {
        L"padrouter_handshake.txt",                       // UE4SS's working folder (same process)
        g_ExeDir + L"\\ue4ss\\padrouter_handshake.txt",
        g_ExeDir + L"\\padrouter_handshake.txt",
    };
    for (const auto& path : paths)
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) { continue; }
        if (CompareFileTime(&attr.ftLastWriteTime, &g_HandshakeTime) == 0) { return false; }  // unchanged

        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) { continue; }
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f))
        {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) { s.pop_back(); }
            lines.push_back(s);
        }
        std::fclose(f);
        if (lines.empty() || lines.back() != "end") { lines.clear(); return false; }  // half-written; retry later
        g_HandshakeTime = attr.ftLastWriteTime;
        return true;
    }
    return false;
}

static void ApplyHandshake()
{
    std::vector<std::string> lines;
    if (!ReadHandshake(lines)) { return; }

    uintptr_t viewport = 0;
    uintptr_t p1input = 0, p2input = 0;
    uintptr_t p1target = 0, p2target = 0, p2ctrl = 0;
    int layoutVertical = 1;
    std::vector<uintptr_t> p1objs, p2objs;
    int found = 0;
    for (const auto& line : lines)
    {
        if (line.rfind("viewport=", 0) == 0)
        {
            viewport = static_cast<uintptr_t>(std::strtoull(line.c_str() + 9, nullptr, 0));
        }
        else if (line.rfind("p1input=", 0) == 0)
        {
            p1input = static_cast<uintptr_t>(std::strtoull(line.c_str() + 8, nullptr, 0));
        }
        else if (line.rfind("p2input=", 0) == 0)
        {
            p2input = static_cast<uintptr_t>(std::strtoull(line.c_str() + 8, nullptr, 0));
        }
        else if (line.rfind("p1target=", 0) == 0)
        {
            p1target = static_cast<uintptr_t>(std::strtoull(line.c_str() + 9, nullptr, 0));
        }
        else if (line.rfind("p2target=", 0) == 0)
        {
            p2target = static_cast<uintptr_t>(std::strtoull(line.c_str() + 9, nullptr, 0));
        }
        else if (line.rfind("p1obj=", 0) == 0)
        {
            p1objs.push_back(static_cast<uintptr_t>(std::strtoull(line.c_str() + 6, nullptr, 0)));
        }
        else if (line.rfind("p2obj=", 0) == 0)
        {
            p2objs.push_back(static_cast<uintptr_t>(std::strtoull(line.c_str() + 6, nullptr, 0)));
        }
        else if (line.rfind("p2ctrl=", 0) == 0)
        {
            p2ctrl = static_cast<uintptr_t>(std::strtoull(line.c_str() + 7, nullptr, 0));
        }
        else if (line.rfind("layout=", 0) == 0)
        {
            layoutVertical = line.compare(7, 8, "vertical") == 0 ? 1 : 0;
        }
        else if (line.rfind("key ", 0) == 0)
        {
            auto eq = line.find('=');
            if (eq == std::string::npos) { continue; }
            std::string name = line.substr(4, eq - 4);
            uint64_t index = std::strtoull(line.c_str() + eq + 1, nullptr, 0);
            uint64_t raw = index & 0xFFFFFFFFull;  // ComparisonIndex, Number = 0
            for (int i = 0; i < BK_Count; ++i) { if (name == kButtonNames[i]) { g_ButtonFNames[i] = raw; ++found; } }
            for (int i = 0; i < AK_Count; ++i) { if (name == kAxisNames[i]) { g_AxisFNames[i] = raw; ++found; } }
        }
    }
    Log("Handshake read: viewport=%p, %d of %d key names", (void*)viewport, found, int(BK_Count + AK_Count));
    {
        // New object lists for the player-context hook (old lists are kept: they are tiny,
        // and a game thread may still be reading one).
        auto sets = new PlayerSets();
        sets->p1 = p1objs;
        sets->p2 = p2objs;
        if (p1target) { sets->p1.push_back(p1target); }
        if (p2target) { sets->p2.push_back(p2target); }
        std::sort(sets->p1.begin(), sets->p1.end());
        std::sort(sets->p2.begin(), sets->p2.end());
        sets->p2pc = p2objs.empty() ? 0 : p2ctrl;
        g_Sets.store(sets, std::memory_order_release);
        Log("Player context: %zu Player 1 objects, %zu Player 2 objects, P2 controller %p, %ld P2 answers so far",
            sets->p1.size(), sets->p2.size(), (void*)sets->p2pc, g_CtxP2Hits.load());
    }
    if (p1target != g_P1Target.load() || p2target != g_P2Target.load() || layoutVertical != g_LayoutVertical.load())
    {
        g_P1Target = p1target;
        g_P2Target = p2target;
        g_LayoutVertical = layoutVertical;
        Log("Aim fix: P1 target %p, P2 target %p, layout %s%s", (void*)p1target, (void*)p2target,
            layoutVertical ? "left/right" : "top/bottom",
            g_AimFixOn.load() ? (p2target ? " -> each player aims at the middle of their own view" : " (one player: stock aim)")
                              : " (fix not installed)");
    }
    if (found == BK_Count + AK_Count) { g_NamesReady = true; }
    else { Log("WARNING: some key names missing from the handshake; routing stays off"); }

    // Player 1's / Player 2's PlayerInput objects (both use the same class, so one vtable).
    if (p1input != g_P1Input.load() || p2input != g_P2Input.load())
    {
        uintptr_t vt{};
        const uintptr_t any = p1input ? p1input : p2input;
        if (any && SafeReadPtr(any, vt) && LooksLikeVTable(vt))
        {
            g_InputVTable = vt;
            const int slots = CountVTableEntries(vt);
            PIKeyFn keyFn = nullptr;
            PIAxisFn axisFn = nullptr;
            if (g_Cfg.PlayerInputKeySlot > 0 && g_Cfg.PlayerInputKeySlot < slots)
            {
                uintptr_t k{};
                SafeReadPtr(vt + g_Cfg.PlayerInputKeySlot * sizeof(void*), k);
                keyFn = reinterpret_cast<PIKeyFn>(k);
            }
            if (g_Cfg.PlayerInputAxisSlot > 0 && g_Cfg.PlayerInputAxisSlot < slots)
            {
                uintptr_t a{};
                SafeReadPtr(vt + g_Cfg.PlayerInputAxisSlot * sizeof(void*), a);
                axisFn = reinterpret_cast<PIAxisFn>(a);
            }
            g_PIKey = keyFn;
            g_PIAxis = axisFn;
            uintptr_t vt2{};
            const bool sameClass = !p1input || !p2input || (SafeReadPtr(p2input, vt2) && vt2 == vt);
            Log("PlayerInput: P1 %p, P2 %p, %d vtable slots%s", (void*)p1input, (void*)p2input, slots,
                sameClass ? "" : "  WARNING: P1 and P2 use different PlayerInput classes");
            if (g_Cfg.Mode == 1)
            {
                if (keyFn && p2input)
                {
                    Log("Routing ready (PlayerInput): key slot %d, %s", g_Cfg.PlayerInputKeySlot,
                        axisFn ? "axis slot set" : "sticks/triggers sent through the key slot");
                }
                else if (!keyFn) { Log("PlayerInput slots not set. Load a save and press Ctrl+Shift+I to probe them."); }
                else { Log("Player 2 not spawned yet (press Ctrl+Y)"); }
            }
        }
        else if (any)
        {
            Log("WARNING: PlayerInput address from the handshake doesn't look like a game object");
        }
        g_P1Input = p1input;
        if (p2input != g_P2Input.load()) { g_ResetPad = true; }
        g_P2Input = p2input;
    }

    if (viewport && viewport != g_HandshakeViewport)
    {
        g_HandshakeViewport = viewport;
        uintptr_t vptr{};
        if (!SafeReadPtr(viewport, vptr) || !LooksLikeVTable(vptr))
        {
            Log("WARNING: viewport address from the handshake doesn't look like a game object");
            return;
        }
        int offset = 0;
        uintptr_t part = FindViewportClientPart(viewport, offset);
        if (!part)
        {
            Log("ERROR: could not find the FViewportClient part of the viewport");
            return;
        }
        uintptr_t vt{};
        SafeReadPtr(part, vt);
        const int slots = CountVTableEntries(vt);
        Log("Viewport %p: FViewportClient part at +0x%X, %d vtable slots", (void*)viewport, offset, slots);

        InputKeyFn keyFn = nullptr;
        InputAxisFn axisFn = nullptr;
        if (g_Cfg.InputKeySlot > 0 && g_Cfg.InputAxisSlot > 0 && g_Cfg.InputKeySlot < slots && g_Cfg.InputAxisSlot < slots)
        {
            uintptr_t k{}, a{};
            SafeReadPtr(vt + g_Cfg.InputKeySlot * sizeof(void*), k);
            SafeReadPtr(vt + g_Cfg.InputAxisSlot * sizeof(void*), a);
            keyFn = reinterpret_cast<InputKeyFn>(k);
            axisFn = reinterpret_cast<InputAxisFn>(a);
        }
        g_ViewportObj = viewport;
        g_ViewportPart = part;
        g_InputKey = keyFn;
        g_InputAxis = axisFn;
        if (g_Cfg.Mode == 0)
        {
            if (keyFn) { Log("Routing ready (viewport): InputKey slot %d, InputAxis slot %d", g_Cfg.InputKeySlot, g_Cfg.InputAxisSlot); }
            else { Log("Viewport slots not set: pad %d is hidden but not routed. Press Ctrl+Shift+P in-game to probe.", g_Cfg.HidePadIndex); }
        }
    }
}

// ============================================================================
// Probe: find the InputKey / InputAxis slots
// ============================================================================
struct alignas(16) ProbeThunk
{
    uint8_t code[16];
    volatile LONG64 count;
    void* target;
};
static_assert(sizeof(ProbeThunk) == 32, "thunk size");

class Probe
{
    enum class Phase { Idle, HandsOff, PressA, MoveStick };
    static constexpr ULONGLONG kPhaseMs = 6000;

    Phase m_phase = Phase::Idle;
    ULONGLONG m_phaseStart = 0;
    uintptr_t m_vtable = 0;
    int m_slots = 0;
    ProbeThunk* m_thunks = nullptr;
    std::vector<uintptr_t> m_original;
    std::vector<std::array<LONG64, 3>> m_counts;
    std::vector<LONG64> m_snapshot;

    void Snapshot() { for (int i = 0; i < m_slots; ++i) { m_snapshot[i] = m_thunks[i].count; } }
    void Record(int phase) { for (int i = 0; i < m_slots; ++i) { m_counts[i][phase] = m_thunks[i].count - m_snapshot[i]; } }

    static void Beep(int times) { for (int i = 0; i < times; ++i) { MessageBeep(MB_OK); Sleep(250); } }

public:
    bool Running() const { return m_phase != Phase::Idle; }

    const char* m_keyName = "InputKeySlot";
    const char* m_axisName = "InputAxisSlot";

    // objectWithVTable: any object whose vtable to probe. Names: the ini keys to suggest.
    void Start(uintptr_t objectWithVTable, const char* keyName, const char* axisName)
    {
        m_keyName = keyName;
        m_axisName = axisName;
        uintptr_t vt{};
        if (!SafeReadPtr(objectWithVTable, vt)) { return; }
        m_vtable = vt;
        m_slots = CountVTableEntries(vt);
        if (m_slots < 8)
        {
            Log("PROBE: vtable looks wrong (%d entries), aborting", m_slots);
            return;
        }
        if (!m_thunks)
        {
            m_thunks = static_cast<ProbeThunk*>(VirtualAlloc(nullptr, 400 * sizeof(ProbeThunk), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!m_thunks) { return; }
        }
        m_original.assign(m_slots, 0);
        m_counts.assign(m_slots, {0, 0, 0});
        m_snapshot.assign(m_slots, 0);

        g_Probing = true;
        for (int i = 1; i < m_slots; ++i)  // slot 0 is the destructor
        {
            SafeReadPtr(vt + i * sizeof(void*), m_original[i]);
            ProbeThunk& t = m_thunks[i];
            const uint8_t code[16] = {
                0xF0, 0x48, 0xFF, 0x05, 0x08, 0x00, 0x00, 0x00,  // lock inc qword ptr [rip+8]  -> count
                0xFF, 0x25, 0x0A, 0x00, 0x00, 0x00,              // jmp qword ptr [rip+10]      -> target
                0xCC, 0xCC};
            std::memcpy(t.code, code, sizeof(code));
            t.count = 0;
            t.target = reinterpret_cast<void*>(m_original[i]);
            FlushInstructionCache(GetCurrentProcess(), &t, sizeof(t));
            WriteSlot(vt, i, reinterpret_cast<uintptr_t>(&t));
        }
        Log("PROBE started on %d slots. Step 1/3 (6 s): hands OFF everything (1 beep)", m_slots);
        Beep(1);
        m_phase = Phase::HandsOff;
        m_phaseStart = GetTickCount64();
        Snapshot();
    }

    void Update()
    {
        if (m_phase == Phase::Idle || GetTickCount64() - m_phaseStart < kPhaseMs) { return; }
        if (m_phase == Phase::HandsOff)
        {
            Record(0);
            Log("PROBE step 2/3 (6 s): tap A repeatedly on PLAYER 1's pad (2 beeps)");
            Beep(2);
            m_phase = Phase::PressA;
        }
        else if (m_phase == Phase::PressA)
        {
            Record(1);
            Log("PROBE step 3/3 (6 s): spin the LEFT stick on PLAYER 1's pad (3 beeps)");
            Beep(3);
            m_phase = Phase::MoveStick;
        }
        else
        {
            Record(2);
            Finish();
            return;
        }
        m_phaseStart = GetTickCount64();
        Snapshot();
    }

    void Finish()
    {
        for (int i = 1; i < m_slots; ++i) { WriteSlot(m_vtable, i, m_original[i]); }
        m_phase = Phase::Idle;
        g_Probing = false;
        // Thunk memory is never freed: a call may still be passing through it.

        Log("PROBE results (slot: hands-off / tap-A / stick):");
        int bestKey = 0, bestAxis = 0;
        LONG64 bestKeyScore = 0, bestAxisScore = 0;
        for (int i = 1; i < m_slots; ++i)
        {
            const auto& c = m_counts[i];
            if (c[0] || c[1] || c[2]) { Log("  slot %d: %lld / %lld / %lld", i, c[0], c[1], c[2]); }
            LONG64 keyScore = c[1] - c[0];
            if (keyScore > bestKeyScore) { bestKeyScore = keyScore; bestKey = i; }
        }
        for (int i = 1; i < m_slots; ++i)
        {
            const auto& c = m_counts[i];
            // InputAxis stays silent while only A is tapped. Slots that also fire on button
            // presses are helpers that both InputKey and InputAxis call; skip those.
            if (i == bestKey || c[1] - c[0] > 0) { continue; }
            LONG64 axisScore = c[2] - c[0];
            if (axisScore > bestAxisScore) { bestAxisScore = axisScore; bestAxis = i; }
        }
        if (bestKey && bestAxis)
        {
            Log("PROBE suggestion: %s=%d  %s=%d", m_keyName, bestKey, m_axisName, bestAxis);
            Log("Put those in DAHPadRouter.ini and restart the game. Send this log if unsure.");
            Beep(1);
        }
        else
        {
            Log("PROBE could not decide. Did player 1's pad reach the game? Send this log.");
        }
    }
};

// ============================================================================
// Worker thread: handshake, hotkeys, probe
// ============================================================================
static bool GameHasFocus()
{
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM)
{
    DWORD pid{};
    const DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) { return TRUE; }
    char cls[64]{};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (std::strcmp(cls, "UnrealWindow") != 0) { return TRUE; }
    g_GameThreadId = thread;
    return FALSE;
}

static DWORD WINAPI Worker(LPVOID)
{
    g_MonitorUntil = GetTickCount64() + 180000;
    Log("Pad monitor ON for 3 minutes: press A on each pad to see which slot it is.");
    g_Enabled = g_Cfg.Enabled;
    Log("Ctrl+Shift+I = probe PlayerInput slots | Ctrl+Shift+P = probe viewport slots | Ctrl+Shift+O = toggle on/off. "
        "Hiding pad %d from the game.", g_Cfg.HidePadIndex);

    if (g_Cfg.AimFix) { InstallAimFix(); }
    else { Log("Aim fix: off (AimFix=0 in DAHPadRouter.ini)"); }
    InstallPlayerContext(g_AimFn);

    Probe probe;
    bool probeWas = false, toggleWas = false, probeInputWas = false;
    ULONGLONG lastHandshake = 0;
    bool warnedNoHandshake = false;
    const ULONGLONG started = GetTickCount64();

    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - lastHandshake > 1000)
        {
            lastHandshake = now;
            ApplyHandshake();
            if (!g_ViewportPart.load() && !warnedNoHandshake && now - started > 120000)
            {
                warnedNoHandshake = true;
                Log("No handshake after 2 minutes: is the DAHPadLink Lua mod enabled in mods.txt?");
            }
        }

        if (!g_GameThreadId.load())
        {
            EnumWindows(&FindGameWindow, 0);
            if (g_GameThreadId.load()) { Log("Game window found; game thread is %lu", g_GameThreadId.load()); }
        }

        // Pick up DAHPadRouter.ini edits while the game runs (aim position tweaks).
        static ULONGLONG lastIniCheck = 0;
        static FILETIME iniTime{};
        if (now - lastIniCheck > 2000)
        {
            lastIniCheck = now;
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (GetFileAttributesExW((g_ExeDir + L"\\DAHPadRouter.ini").c_str(), GetFileExInfoStandard, &attr))
            {
                if (iniTime.dwLowDateTime || iniTime.dwHighDateTime)
                {
                    if (CompareFileTime(&attr.ftLastWriteTime, &iniTime) != 0)
                    {
                        LoadConfig();
                        Log("DAHPadRouter.ini changed: aim P1 x=%.3f P2 x=%.3f (left/right), P1 y0=%.3f P2 y0=%.3f (top/bottom)",
                            g_Cfg.AimP1X, g_Cfg.AimP2X, g_Cfg.AimP1Y0, g_Cfg.AimP2Y0);
                    }
                }
                iniTime = attr.ftLastWriteTime;
            }
        }

        probe.Update();

        // Pass a "spawn Player 2" request to the DAH2P Lua mod (it polls for this file).
        static ULONGLONG lastSpawnRequest = 0;
        if (g_SpawnRequested.exchange(false) && now - lastSpawnRequest > 3000)
        {
            lastSpawnRequest = now;
            const std::wstring paths[] = {L"padrouter_spawn_request.txt", g_ExeDir + L"\\ue4ss\\padrouter_spawn_request.txt"};
            bool written = false;
            for (const auto& path : paths)
            {
                if (FILE* f = _wfopen(path.c_str(), L"w"))
                {
                    std::fputs("spawn\n", f);
                    std::fclose(f);
                    written = true;
                    break;
                }
            }
            Log(written ? "Start on pad %d: asked DAH2P to spawn Player 2" : "Start on pad %d: could not write the spawn request",
                g_Cfg.HidePadIndex);
        }

        const bool chord = GameHasFocus() && Down(VK_CONTROL) && Down(VK_SHIFT);
        const bool probeKey = chord && Down('P');
        const bool probeInputKey = chord && Down('I');
        const bool toggleKey = chord && Down('O');
        if (probeKey && !probeWas)
        {
            if (probe.Running()) { Log("Probe already running"); }
            else if (!g_ViewportPart.load()) { Log("Probe: no viewport yet (handshake missing?)"); }
            else { Log("Probing the VIEWPORT"); probe.Start(g_ViewportPart.load(), "InputKeySlot", "InputAxisSlot"); }
        }
        if (probeInputKey && !probeInputWas)
        {
            if (probe.Running()) { Log("Probe already running"); }
            else if (!g_P1Input.load()) { Log("Probe: Player 1's PlayerInput not known yet (load a save first)"); }
            else { Log("Probing Player 1's PLAYERINPUT"); probe.Start(g_P1Input.load(), "PlayerInputKeySlot", "PlayerInputAxisSlot"); }
        }
        probeInputWas = probeInputKey;
        if (toggleKey && !toggleWas)
        {
            g_Enabled = !g_Enabled;
            Log(g_Enabled ? "ON: pad routed to Player 2" : "OFF: pad behaves like the stock game");
            MessageBeep(g_Enabled ? MB_OK : MB_ICONHAND);
        }
        probeWas = probeKey;
        toggleWas = toggleKey;
        Sleep(30);
    }
}

static void StartWorker()
{
    HANDLE h = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_Self = instance;
        DisableThreadLibraryCalls(instance);
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_ExeDir = DirOf(exe);
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(instance, self, MAX_PATH);
        std::wstring name(self);
        name = name.substr(name.find_last_of(L"\\/") + 1);
        auto dot = name.find_last_of(L'.');
        g_SelfName = dot == std::wstring::npos ? name : name.substr(0, dot);
    }
    return TRUE;
}
