#pragma once

#include "DasherInput.h"
#include "SettingsStore.h"
#include <memory>
#include <mutex>
#include <thread>
#include "SDL3/SDL_joystick.h"
#include <unordered_map>
#include <unordered_set>

class DasherController;

class JoystickInput : public Dasher::CDasherVectorInput
{
public:
    JoystickInput(DasherController* interface, Dasher::CSettingsStore* settings);

    virtual bool GetVectorCoords(float &VectorX, float &VectorY) override;

    virtual void Activate() override;
    virtual void Deactivate() override;

    // bool OpenCurrentJoystick();
    void OpenNeededControllers(bool openAll = false);
    void PoplulateInputMaps();

protected:
    DasherController* interface;
    Dasher::CSettingsStore* settings;
    Event<> JoysticksChanged;
    
    typedef std::string JoystickGUID;
    std::unordered_set<JoystickGUID> requestedControllers;
    std::unordered_map<SDL_JoystickID, JoystickGUID> openedControllers; //Known ControllerIDs to GUID Mapping
    std::pair<std::string, Uint8> XAxis; // GUID to AxisNum
    std::pair<std::string, Uint8> YAxis; // GUID to AxisNum
    static JoystickGUID IDtoGUID(const SDL_JoystickID& id);
    static std::string GUIDAndSpecifierToString(const JoystickInput::JoystickGUID id, Uint8 specifier);
    static std::pair<JoystickInput::JoystickGUID, Uint8> GUIDAndSpecifierFromString(const std::string& InputString);

    //Input Vector
    double lastRelativeX = 0;
	double lastRelativeY = 0;
    
    //Threading
    std::unique_ptr<std::thread> eventThread;
    bool keepThreadAlive = true;
    bool sdl_initialized = false;
    std::mutex sdl_external;
};