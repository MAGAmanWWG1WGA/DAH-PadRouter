// DAHPadRouter - UE4SS C++ mod for Destroy All Humans! (2020, UE 4.22)
//
// Problem: the game sends every gamepad to Player 1, so a second local player
// (created by the DAH2P Lua mod) never gets controller input.
//
// What this mod does:
//   1. Hooks XInputGetState in the game so the game sees the second pad
//      (HidePadIndex) as connected but idle. It stops driving Player 1.
//   2. Reads that pad itself, once per frame on the game thread, and feeds its
//      real buttons/sticks/triggers into Unreal's own input routing
//      (UGameViewportClient::InputKey / InputAxis) as ControllerId P2ControllerId.
//      Unreal then delivers it to Player 2's PlayerController, exactly like a
//      normal pad, so Player 2 gets the game's own analog controls.
//   3. Ctrl+Shift+P runs a probe that finds the InputKey / InputAxis slots in
//      this game build and prints them to UE4SS.log (they go in config.txt).
//   Ctrl+Shift+O toggles the whole thing on/off.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

#define LOG(...) Output::send<LogLevel::Verbose>(__VA_ARGS__)
#define LOGW(...) Output::send<LogLevel::Warning>(__VA_ARGS__)
#define LOGE(...) Output::send<LogLevel::Error>(__VA_ARGS__)

// ============================================================================
// Config (config.txt next to the DLL)
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
};
static Config g_Cfg;

static std::wstring ThisDllDir()
{
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisDllDir), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

static void LoadConfig()
{
    std::ifstream file(ThisDllDir() + L"\\config.txt");
    if (!file)
    {
        LOGW(STR("[DAHPadRouter] config.txt not found next to the DLL, using defaults\n"));
        return;
    }
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') { continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) { continue; }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && key.back() == ' ') { key.pop_back(); }
        int v = 0;
        try { v = std::stoi(val, nullptr, 0); } catch (...) { continue; }  // base 0: accepts 0x..
        if (key == "Enabled") { g_Cfg.Enabled = v != 0; }
        else if (key == "HidePadIndex") { g_Cfg.HidePadIndex = v; }
        else if (key == "P2ControllerId") { g_Cfg.P2ControllerId = v; }
        else if (key == "InputKeySlot") { g_Cfg.InputKeySlot = v; }
        else if (key == "InputAxisSlot") { g_Cfg.InputAxisSlot = v; }
        else if (key == "ViewportClientOffset") { g_Cfg.ViewportClientOffset = v; }
        else if (key == "ViewportPtrOffset") { g_Cfg.ViewportPtrOffset = v; }
    }
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

// UGameViewportClient inherits UObject first and FViewportClient second, so
// InputKey/InputAxis live in a second vtable inside the object. Find it.
static uintptr_t FindViewportClientPart(uintptr_t obj, int& outOffset)
{
    auto looksLikeVTable = [](uintptr_t vt) {
        uintptr_t first{};
        return g_RData.contains(vt) && SafeReadPtr(vt, first) && g_Text.contains(first);
    };
    if (g_Cfg.ViewportClientOffset > 0)
    {
        uintptr_t vt{};
        if (SafeReadPtr(obj + g_Cfg.ViewportClientOffset, vt) && looksLikeVTable(vt))
        {
            outOffset = g_Cfg.ViewportClientOffset;
            return obj + outOffset;
        }
        return 0;
    }
    for (int off = 0x28; off <= 0x60; off += 8)
    {
        uintptr_t vt{};
        if (SafeReadPtr(obj + off, vt) && looksLikeVTable(vt))
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
static_assert(sizeof(FKeyRaw) == 0x18);

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
static_assert(offsetof(FInputKeyEventArgsRaw, Key) == 0x10);
static_assert(offsetof(FInputKeyEventArgsRaw, Event) == 0x28);

using InputKeyFn = bool (*)(void* self, FInputKeyEventArgsRaw* args);
using InputAxisFn = bool (*)(void* self, void* viewport, int32_t controllerId, FKeyRaw* key,
                             float delta, float deltaTime, int32_t numSamples, bool bGamepad);

// Set up by the update thread, used on the game thread.
static std::atomic<uintptr_t> g_ViewportObj{0};   // UGameViewportClient object
static std::atomic<uintptr_t> g_ViewportPart{0};  // its FViewportClient part ("this" for InputKey/Axis)
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

static const wchar_t* const kButtonNames[BK_Count] = {
    STR("Gamepad_DPad_Up"), STR("Gamepad_DPad_Down"), STR("Gamepad_DPad_Left"), STR("Gamepad_DPad_Right"),
    STR("Gamepad_Special_Right"), STR("Gamepad_Special_Left"), STR("Gamepad_LeftThumbstick"), STR("Gamepad_RightThumbstick"),
    STR("Gamepad_LeftShoulder"), STR("Gamepad_RightShoulder"),
    STR("Gamepad_FaceButton_Bottom"), STR("Gamepad_FaceButton_Right"), STR("Gamepad_FaceButton_Left"), STR("Gamepad_FaceButton_Top"),
    STR("Gamepad_LeftTrigger"), STR("Gamepad_RightTrigger"),
    STR("Gamepad_LeftStick_Up"), STR("Gamepad_LeftStick_Down"), STR("Gamepad_LeftStick_Left"), STR("Gamepad_LeftStick_Right"),
    STR("Gamepad_RightStick_Up"), STR("Gamepad_RightStick_Down"), STR("Gamepad_RightStick_Left"), STR("Gamepad_RightStick_Right"),
};
static const wchar_t* const kAxisNames[AK_Count] = {
    STR("Gamepad_LeftX"), STR("Gamepad_LeftY"), STR("Gamepad_RightX"), STR("Gamepad_RightY"),
    STR("Gamepad_LeftTriggerAxis"), STR("Gamepad_RightTriggerAxis"),
};
static uint64_t g_ButtonFNames[BK_Count]{};
static uint64_t g_AxisFNames[AK_Count]{};
static std::atomic<bool> g_NamesReady{false};

static uint64_t MakeFNameRaw(const wchar_t* str)
{
    FName name(str, FNAME_Add);
    static_assert(sizeof(FName) >= 8);
    uint64_t raw{};
    std::memcpy(&raw, &name, 8);
    return raw;
}

static void InitKeyNames()
{
    for (int i = 0; i < BK_Count; ++i) { g_ButtonFNames[i] = MakeFNameRaw(kButtonNames[i]); }
    for (int i = 0; i < AK_Count; ++i) { g_AxisFNames[i] = MakeFNameRaw(kAxisNames[i]); }
    g_NamesReady = true;
}

static float ShortToFloat(SHORT v) { return v <= 0 ? v / 32768.0f : v / 32767.0f; }

static void ComputeButtons(const XINPUT_GAMEPAD& g, std::array<bool, BK_Count>& b)
{
    const WORD w = g.wButtons;
    b[BK_DPadUp] = w & XINPUT_GAMEPAD_DPAD_UP;
    b[BK_DPadDown] = w & XINPUT_GAMEPAD_DPAD_DOWN;
    b[BK_DPadLeft] = w & XINPUT_GAMEPAD_DPAD_LEFT;
    b[BK_DPadRight] = w & XINPUT_GAMEPAD_DPAD_RIGHT;
    b[BK_Start] = w & XINPUT_GAMEPAD_START;
    b[BK_Back] = w & XINPUT_GAMEPAD_BACK;
    b[BK_LeftThumb] = w & XINPUT_GAMEPAD_LEFT_THUMB;
    b[BK_RightThumb] = w & XINPUT_GAMEPAD_RIGHT_THUMB;
    b[BK_LeftShoulder] = w & XINPUT_GAMEPAD_LEFT_SHOULDER;
    b[BK_RightShoulder] = w & XINPUT_GAMEPAD_RIGHT_SHOULDER;
    b[BK_A] = w & XINPUT_GAMEPAD_A;
    b[BK_B] = w & XINPUT_GAMEPAD_B;
    b[BK_X] = w & XINPUT_GAMEPAD_X;
    b[BK_Y] = w & XINPUT_GAMEPAD_Y;
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

static void SendKey(int index, bool pressed)
{
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
    return g_NamesReady && g_InputKey.load() && g_InputAxis.load() && g_ViewportPart.load() && !g_Probing;
}

// Called on the game thread every time the game polls Player 2's pad.
static void RoutePad(const XINPUT_GAMEPAD* pad)
{
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
        LOG(STR("[DAHPadRouter] Routing pad {} to ControllerId {} (first frame sent)\n"), g_Cfg.HidePadIndex, g_Cfg.P2ControllerId);
    }
}

static bool AnyHeld()
{
    for (bool b : g_PrevButtons) { if (b) { return true; } }
    for (float a : g_PrevAxes) { if (a != 0.0f) { return true; } }
    return false;
}

// ============================================================================
// XInputGetState hook
// ============================================================================
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static XInputGetStateFn g_RealXInputGetState = nullptr;

static DWORD WINAPI HookedXInputGetState(DWORD userIndex, XINPUT_STATE* state)
{
    DWORD result = g_RealXInputGetState(userIndex, state);
    if (int(userIndex) != g_Cfg.HidePadIndex) { return result; }

    if (!g_Enabled)
    {
        if (AnyHeld() && InjectionReady()) { RoutePad(nullptr); }  // release anything still held
        return result;
    }

    const bool connected = result == ERROR_SUCCESS && state;
    XINPUT_GAMEPAD pad{};
    if (connected)
    {
        pad = state->Gamepad;
        state->Gamepad = XINPUT_GAMEPAD{};  // the game sees a connected pad that is never touched
        state->dwPacketNumber = 1;
    }
    if (InjectionReady()) { RoutePad(connected ? &pad : nullptr); }
    return result;
}

static std::wstring Widen(const char* s)
{
    std::wstring out;
    for (; *s; ++s) { out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s))); }
    return out;
}

static bool NameStartsWithXInput(const char* name) { return _strnicmp(name, "xinput", 6) == 0; }

static int PatchXInputImports()
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dirs = nt->OptionalHeader.DataDirectory;
    int patched = 0;

    auto patchSlot = [&](uintptr_t* slot, XInputGetStateFn real) {
        if (!real) { return; }
        if (!g_RealXInputGetState) { g_RealXInputGetState = real; }
        DWORD old{};
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
        *slot = reinterpret_cast<uintptr_t>(&HookedXInputGetState);
        VirtualProtect(slot, sizeof(void*), old, &old);
        ++patched;
    };

    // Normal imports
    if (dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress)
    {
        auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (; desc->Name; ++desc)
        {
            auto dll = reinterpret_cast<const char*>(base + desc->Name);
            if (!NameStartsWithXInput(dll) || !desc->OriginalFirstThunk) { continue; }
            auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
            auto slots = reinterpret_cast<uintptr_t*>(base + desc->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) { continue; }
                auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "XInputGetState") == 0)
                {
                    LOG(STR("[DAHPadRouter] Found XInputGetState import from {}\n"), Widen(dll));
                    patchSlot(slots, reinterpret_cast<XInputGetStateFn>(*slots));
                }
            }
        }
    }

    // Delay-loaded imports (resolve the real function ourselves so the delay-load
    // helper can't overwrite our hook later)
    if (dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress)
    {
        auto desc = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress);
        for (; desc->DllNameRVA; ++desc)
        {
            auto dll = reinterpret_cast<const char*>(base + desc->DllNameRVA);
            if (!NameStartsWithXInput(dll)) { continue; }
            auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->ImportNameTableRVA);
            auto slots = reinterpret_cast<uintptr_t*>(base + desc->ImportAddressTableRVA);
            for (; names->u1.AddressOfData; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) { continue; }
                auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "XInputGetState") == 0)
                {
                    LOG(STR("[DAHPadRouter] Found delay-loaded XInputGetState import from {}\n"), Widen(dll));
                    HMODULE mod = LoadLibraryA(dll);
                    patchSlot(slots, mod ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState")) : nullptr);
                }
            }
        }
    }
    return patched;
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
static_assert(sizeof(ProbeThunk) == 32);

class Probe
{
    enum class Phase { Idle, HandsOff, PressA, MoveStick };
    static constexpr double kPhaseSeconds = 6.0;

    Phase m_phase = Phase::Idle;
    ULONGLONG m_phaseStart = 0;
    uintptr_t m_vtable = 0;
    int m_slots = 0;
    ProbeThunk* m_thunks = nullptr;
    std::vector<uintptr_t> m_original;
    std::vector<std::array<LONG64, 3>> m_counts;
    std::vector<LONG64> m_snapshot;

    void Snapshot()
    {
        for (int i = 0; i < m_slots; ++i) { m_snapshot[i] = m_thunks[i].count; }
    }
    void Record(int phase)
    {
        for (int i = 0; i < m_slots; ++i) { m_counts[i][phase] = m_thunks[i].count - m_snapshot[i]; }
    }

public:
    bool Running() const { return m_phase != Phase::Idle; }

    void Start(uintptr_t viewportPart)
    {
        uintptr_t vt{};
        if (!SafeReadPtr(viewportPart, vt)) { return; }
        m_vtable = vt;
        m_slots = CountVTableEntries(vt);
        if (m_slots < 8)
        {
            LOGE(STR("[DAHPadRouter] Probe: vtable looks wrong ({} entries), aborting\n"), m_slots);
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
        LOGW(STR("[DAHPadRouter] PROBE started on {} slots. Do not touch the mouse or keyboard.\n"), m_slots);
        LOGW(STR("[DAHPadRouter] PROBE step 1/3 (6 s): hands OFF all controllers\n"));
        m_phase = Phase::HandsOff;
        m_phaseStart = GetTickCount64();
        Snapshot();
    }

    void Update()
    {
        if (m_phase == Phase::Idle) { return; }
        if (double(GetTickCount64() - m_phaseStart) / 1000.0 < kPhaseSeconds) { return; }

        if (m_phase == Phase::HandsOff)
        {
            Record(0);
            LOGW(STR("[DAHPadRouter] PROBE step 2/3 (6 s): tap A repeatedly on PLAYER 1's pad, sticks untouched\n"));
            m_phase = Phase::PressA;
        }
        else if (m_phase == Phase::PressA)
        {
            Record(1);
            LOGW(STR("[DAHPadRouter] PROBE step 3/3 (6 s): spin the LEFT stick on PLAYER 1's pad, no buttons\n"));
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

        LOGW(STR("[DAHPadRouter] PROBE results (slot: hands-off / tap-A / stick):\n"));
        int bestKey = 0, bestAxis = 0;
        LONG64 bestKeyScore = 0, bestAxisScore = 0;
        for (int i = 1; i < m_slots; ++i)
        {
            const auto& c = m_counts[i];
            if (c[0] || c[1] || c[2]) { LOGW(STR("[DAHPadRouter]   slot {}: {} / {} / {}\n"), i, c[0], c[1], c[2]); }
            LONG64 keyScore = c[1] - c[0];
            if (keyScore > bestKeyScore) { bestKeyScore = keyScore; bestKey = i; }
        }
        for (int i = 1; i < m_slots; ++i)
        {
            const auto& c = m_counts[i];
            LONG64 axisScore = c[2] - c[0];
            if (i != bestKey && axisScore > bestAxisScore) { bestAxisScore = axisScore; bestAxis = i; }
        }
        if (bestKey && bestAxis)
        {
            LOGW(STR("[DAHPadRouter] PROBE suggestion: InputKeySlot={}  InputAxisSlot={}\n"), bestKey, bestAxis);
            LOGW(STR("[DAHPadRouter] Put those in config.txt and restart the game. Send the table above if unsure.\n"));
        }
        else
        {
            LOGE(STR("[DAHPadRouter] PROBE could not decide. Did player 1's pad reach the game? Send this log.\n"));
        }
    }
};

// ============================================================================
// Mod
// ============================================================================
class DAHPadRouter : public CppUserModBase
{
    Probe m_probe;
    bool m_unrealReady = false;
    bool m_loggedViewport = false;
    ULONGLONG m_lastScan = 0;
    bool m_keyProbeWas = false, m_keyToggleWas = false;

    static bool GameHasFocus()
    {
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    void FindViewport()
    {
        UObject* vp = UObjectGlobals::FindFirstOf(STR("GameViewportClient"));
        if (!vp || vp->GetName().rfind(STR("Default__"), 0) == 0) { return; }

        const auto obj = reinterpret_cast<uintptr_t>(vp);
        if (obj == g_ViewportObj.load() && g_ViewportPart.load()) { return; }

        int offset = 0;
        uintptr_t part = FindViewportClientPart(obj, offset);
        if (!part)
        {
            LOGE(STR("[DAHPadRouter] Could not find the FViewportClient part of {}\n"), vp->GetFullName());
            return;
        }
        uintptr_t vt{};
        SafeReadPtr(part, vt);
        const int slots = CountVTableEntries(vt);

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

        g_ViewportObj = obj;
        g_ViewportPart = part;
        g_InputKey = keyFn;
        g_InputAxis = axisFn;

        if (!m_loggedViewport)
        {
            m_loggedViewport = true;
            LOG(STR("[DAHPadRouter] Viewport {} | FViewportClient part at +0x{:X} | {} vtable slots\n"),
                vp->GetFullName(), offset, slots);
            if (keyFn) { LOG(STR("[DAHPadRouter] Routing ready: InputKey slot {}, InputAxis slot {}\n"), g_Cfg.InputKeySlot, g_Cfg.InputAxisSlot); }
            else { LOGW(STR("[DAHPadRouter] Slots not set: pad {} is hidden but not routed yet. Press Ctrl+Shift+P in-game to probe.\n"), g_Cfg.HidePadIndex); }
        }
    }

public:
    DAHPadRouter()
    {
        ModName = STR("DAHPadRouter");
        ModVersion = STR("0.1");
        ModDescription = STR("Gives the second gamepad to the second local player");
        ModAuthors = STR("");
    }
    ~DAHPadRouter() override = default;

    auto on_unreal_init() -> void override
    {
        LoadConfig();
        g_Enabled = g_Cfg.Enabled;
        InitModuleRanges();
        InitKeyNames();

        const int patched = PatchXInputImports();
        if (patched == 0) { LOGE(STR("[DAHPadRouter] No XInputGetState import found in the game exe; pad 2 cannot be hidden\n")); }
        else { LOG(STR("[DAHPadRouter] Hooked XInputGetState ({} import slot(s)); hiding pad {}\n"), patched, g_Cfg.HidePadIndex); }
        LOG(STR("[DAHPadRouter] Ctrl+Shift+P = probe slots | Ctrl+Shift+O = toggle on/off\n"));
        m_unrealReady = true;
    }

    auto on_update() -> void override
    {
        if (!m_unrealReady) { return; }

        const ULONGLONG now = GetTickCount64();
        if (now - m_lastScan > 1000)
        {
            m_lastScan = now;
            FindViewport();
        }

        m_probe.Update();

        const bool focus = GameHasFocus();
        const bool chord = focus && Down(VK_CONTROL) && Down(VK_SHIFT);
        const bool probeKey = chord && Down('P');
        const bool toggleKey = chord && Down('O');

        if (probeKey && !m_keyProbeWas)
        {
            if (m_probe.Running()) { LOGW(STR("[DAHPadRouter] Probe already running\n")); }
            else if (!g_ViewportPart.load()) { LOGE(STR("[DAHPadRouter] Probe: game viewport not found yet\n")); }
            else { m_probe.Start(g_ViewportPart.load()); }
        }
        if (toggleKey && !m_keyToggleWas)
        {
            g_Enabled = !g_Enabled;
            LOGW(STR("[DAHPadRouter] {}\n"), g_Enabled ? STR("ON: pad routed to Player 2") : STR("OFF: pad behaves like the stock game"));
        }
        m_keyProbeWas = probeKey;
        m_keyToggleWas = toggleKey;
    }
};

#define DAH_MOD_API __declspec(dllexport)
extern "C"
{
    DAH_MOD_API CppUserModBase* start_mod() { return new DAHPadRouter(); }
    DAH_MOD_API void uninstall_mod(CppUserModBase* mod) { delete mod; }
}
