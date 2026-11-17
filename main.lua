--[[
    DAHPadLink - tells the DAHPadRouter xinput DLL where the game's viewport is and what
    the gamepad key names' internal IDs are. It writes padrouter_handshake.txt into the
    UE4SS folder; the DLL reads it from there.

    Install: ue4ss\Mods\DAHPadLink\Scripts\main.lua, then add "DAHPadLink : 1" to Mods\mods.txt
]]

local UEHelpers = require("UEHelpers")

local MOD = "DAHPadLink"
local FILE = "padrouter_handshake.txt"

local KEYS = {
    "Gamepad_DPad_Up", "Gamepad_DPad_Down", "Gamepad_DPad_Left", "Gamepad_DPad_Right",
    "Gamepad_Special_Right", "Gamepad_Special_Left", "Gamepad_LeftThumbstick", "Gamepad_RightThumbstick",
    "Gamepad_LeftShoulder", "Gamepad_RightShoulder",
    "Gamepad_FaceButton_Bottom", "Gamepad_FaceButton_Right", "Gamepad_FaceButton_Left", "Gamepad_FaceButton_Top",
    "Gamepad_LeftTrigger", "Gamepad_RightTrigger",
    "Gamepad_LeftStick_Up", "Gamepad_LeftStick_Down", "Gamepad_LeftStick_Left", "Gamepad_LeftStick_Right",
    "Gamepad_RightStick_Up", "Gamepad_RightStick_Down", "Gamepad_RightStick_Left", "Gamepad_RightStick_Right",
    "Gamepad_LeftX", "Gamepad_LeftY", "Gamepad_RightX", "Gamepad_RightY",
    "Gamepad_LeftTriggerAxis", "Gamepad_RightTriggerAxis",
}

local function log(fmt, ...)
    print(string.format("[%s] " .. fmt .. "\n", MOD, ...))
end

-- Remove the previous session's handshake so the DLL never reads a stale address.
os.remove(FILE)

local lastWritten = nil
local keyLines = nil
local failedOnce = false

local function buildKeyLines()
    local lines = {}
    for _, name in ipairs(KEYS) do
        local ok, idx = pcall(function() return FName(name):GetComparisonIndex() end)
        if not ok or type(idx) ~= "number" or idx <= 0 then
            if not failedOnce then
                failedOnce = true
                log("ERROR: could not get the ID of key '%s' (%s). Send UE4SS.log.", name, tostring(idx))
            end
            return nil
        end
        lines[#lines + 1] = string.format("key %s=%d", name, idx)
    end
    return lines
end

-- Address of player N's PlayerInput object (0 = none). Index 0 = Player 1, 1 = Player 2.
local function playerInputAddress(index)
    local ok, addr = pcall(function()
        local world = UEHelpers.GetWorld()
        local statics = UEHelpers.GetGameplayStatics()
        if not (world and world:IsValid() and statics and statics:IsValid()) then return 0 end
        local pc = statics:GetPlayerController(world, index)
        if not (pc and pc:IsValid()) then return 0 end
        local input = pc.PlayerInput
        if not (input and input:IsValid()) then return 0 end
        return input:GetAddress()
    end)
    if ok and type(addr) == "number" then return addr end
    return 0
end

local function writeHandshake()
    local vp = FindFirstOf("GameViewportClient")
    if not vp or not vp:IsValid() then return end
    local okA, address = pcall(function() return vp:GetAddress() end)
    if not okA or type(address) ~= "number" then
        if not failedOnce then
            failedOnce = true
            log("ERROR: could not read the viewport address (%s)", tostring(address))
        end
        return
    end
    local p1 = playerInputAddress(0)
    local p2 = playerInputAddress(1)

    local signature = string.format("%X/%X/%X", address, p1, p2)
    if signature == lastWritten then return end

    keyLines = keyLines or buildKeyLines()
    if not keyLines then return end

    local out = {
        string.format("viewport=0x%X", address),
        string.format("p1input=0x%X", p1),
        string.format("p2input=0x%X", p2),
    }
    for _, l in ipairs(keyLines) do out[#out + 1] = l end
    out[#out + 1] = "end"

    local tmp = FILE .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then
        log("ERROR: could not write %s", tmp)
        return
    end
    f:write(table.concat(out, "\n"), "\n")
    f:close()
    os.remove(FILE)
    os.rename(tmp, FILE)

    lastWritten = signature
    log("Handshake written: viewport 0x%X, P1 input 0x%X, P2 input 0x%X, %d key IDs", address, p1, p2, #keyLines)
end

LoopAsync(1000, function()
    ExecuteInGameThread(function()
        local ok, err = pcall(writeHandshake)
        if not ok and not failedOnce then
            failedOnce = true
            log("ERROR: %s", tostring(err))
        end
    end)
    return false  -- keep running (re-sends when Player 2 is added/removed or anything changes)
end)

log("Loaded. Waiting for the game viewport...")
