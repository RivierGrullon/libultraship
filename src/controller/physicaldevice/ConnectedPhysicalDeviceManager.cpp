#include "ConnectedPhysicalDeviceManager.h"

#include <algorithm>
#include <string_view>
#include "public/bridge/consolevariablebridge.h"

namespace {
constexpr uint8_t GamepadPortCount = 4;

std::string GetGamepadConfigId(SDL_GameController* gamepad) {
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(gamepad)), guid, sizeof(guid));
    std::string id(guid);
    if (id.find_first_not_of('0') == std::string::npos) {
        return {};
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char* serial = SDL_GameControllerGetSerial(gamepad);
    if (serial != nullptr && serial[0] != '\0') {
        // Encode the serial so punctuation cannot be interpreted as a config path separator.
        constexpr char hex[] = "0123456789abcdef";
        id += '_';
        for (const unsigned char byte : std::string_view(serial)) {
            id += hex[byte >> 4];
            id += hex[byte & 0x0f];
        }
    }
#endif
    return id;
}

std::string GetPortConfigKey(uint8_t portIndex, const std::string& gamepadId) {
    return CVAR_PREFIX_CONTROLLERS ".Port" + std::to_string(portIndex + 1) + ".GamepadOverrides." + gamepadId;
}
} // namespace

namespace Ship {
ConnectedPhysicalDeviceManager::ConnectedPhysicalDeviceManager() {
}

ConnectedPhysicalDeviceManager::~ConnectedPhysicalDeviceManager() {
}

std::unordered_map<int32_t, SDL_GameController*>
ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadsForPort(uint8_t portIndex) {
    std::unordered_map<int32_t, SDL_GameController*> result;

    for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
        if (!PortIsIgnoringInstanceId(portIndex, instanceId)) {
            result[instanceId] = gamepad;
        }
    }

    return result;
}

std::unordered_map<int32_t, std::string> ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadNames() {
    return mConnectedSDLGamepadNames;
}

std::unordered_set<int32_t> ConnectedPhysicalDeviceManager::GetIgnoredInstanceIdsForPort(uint8_t portIndex) {
    return mIgnoredInstanceIds[portIndex];
}

bool ConnectedPhysicalDeviceManager::PortIsIgnoringInstanceId(uint8_t portIndex, int32_t instanceId) {
    const auto port = mIgnoredInstanceIds.find(portIndex);
    return port != mIgnoredInstanceIds.end() && port->second.contains(instanceId);
}

void ConnectedPhysicalDeviceManager::IgnoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    SetInstanceIdIgnoredForPort(portIndex, instanceId, true);
}

void ConnectedPhysicalDeviceManager::UnignoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    SetInstanceIdIgnoredForPort(portIndex, instanceId, false);
}

const std::string* ConnectedPhysicalDeviceManager::GetUniqueGamepadConfigId(int32_t instanceId) const {
    const auto gamepad = mGamepadConfigIds.find(instanceId);
    if (gamepad == mGamepadConfigIds.end() || gamepad->second.empty()) {
        return nullptr;
    }

    // A GUID may identify only the model when SDL does not provide a serial number.
    const auto matches = std::count_if(mGamepadConfigIds.begin(), mGamepadConfigIds.end(),
                                       [&gamepad](const auto& other) { return other.second == gamepad->second; });
    return matches == 1 ? &gamepad->second : nullptr;
}

void ConnectedPhysicalDeviceManager::SetInstanceIdIgnoredForPort(uint8_t portIndex, int32_t instanceId, bool ignored) {
    if (ignored) {
        mIgnoredInstanceIds[portIndex].insert(instanceId);
    } else {
        mIgnoredInstanceIds[portIndex].erase(instanceId);
    }
    mSessionPortOverrides[instanceId][portIndex] = ignored;

    if (const auto* gamepadId = GetUniqueGamepadConfigId(instanceId)) {
        const auto key = GetPortConfigKey(portIndex, *gamepadId);
        if (ignored == (portIndex != 0)) {
            CVarClear(key.c_str());
        } else {
            CVarSetInteger(key.c_str(), ignored);
        }
        CVarSave();
    }
}

void ConnectedPhysicalDeviceManager::RestorePortSettings() {
    mIgnoredInstanceIds.clear();
    std::erase_if(mSessionPortOverrides, [this](const auto& entry) {
        return !mConnectedSDLGamepads.contains(entry.first);
    });

    for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
        const auto* gamepadId = GetUniqueGamepadConfigId(instanceId);
        for (uint8_t portIndex = 0; portIndex < GamepadPortCount; portIndex++) {
            bool ignored = portIndex != 0;
            if (gamepadId != nullptr) {
                const auto key = GetPortConfigKey(portIndex, *gamepadId);
                ignored = CVarGetInteger(key.c_str(), ignored) != 0;
            }
            if (ignored) {
                mIgnoredInstanceIds[portIndex].insert(instanceId);
            }
        }
    }

    for (const auto& [instanceId, overrides] : mSessionPortOverrides) {
        for (const auto& [portIndex, ignored] : overrides) {
            if (ignored) {
                mIgnoredInstanceIds[portIndex].insert(instanceId);
            } else {
                mIgnoredInstanceIds[portIndex].erase(instanceId);
            }
        }
    }
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceConnect(int32_t sdlDeviceIndex) {
    RefreshConnectedSDLGamepads();
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceDisconnect(int32_t sdlJoystickInstanceId) {
    RefreshConnectedSDLGamepads();
}

void ConnectedPhysicalDeviceManager::RefreshConnectedSDLGamepads() {
    mConnectedSDLGamepads.clear();
    mConnectedSDLGamepadNames.clear();
    mGamepadConfigIds.clear();

    for (int32_t i = 0; i < SDL_NumJoysticks(); i++) {
        // skip if this SDL joystick isn't a Gamepad
        if (!SDL_IsGameController(i)) {
            continue;
        }

        auto gamepad = SDL_GameControllerOpen(i);
        if (gamepad == nullptr) {
            continue;
        }
        auto instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad));
        auto name = SDL_GameControllerName(gamepad);

        mConnectedSDLGamepads[instanceId] = gamepad;
        mConnectedSDLGamepadNames[instanceId] = name;
        mGamepadConfigIds[instanceId] = GetGamepadConfigId(gamepad);
    }
    RestorePortSettings();
}
} // namespace Ship
