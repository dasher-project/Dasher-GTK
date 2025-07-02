#pragma once

#include "DasherInput.h"
#include "SettingsStore.h"
#include <memory>
#include <mutex>
#include <thread>
#include "SDL_joystick.h"
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
    void OpenNeededControllers();
    void PoplulateInputMaps();
    std::map<std::string, std::string> GetAvailableDevices(); 

protected:
    DasherController* interface;
    Dasher::CSettingsStore* settings;
    Event<> JoysticksChanged;
    
    typedef std::string JoystickGUID;
    std::unordered_set<JoystickGUID> requestedControllers;
    std::unordered_map<SDL_JoystickID, JoystickGUID> openedControllers; //Known ControllerIDs to GUID Mapping
    std::pair<std::string, Uint8> XAxis; // GUID to AxisNum
    std::pair<std::string, Uint8> YAxis; // GUID to AxisNum
    static JoystickGUID convertIDtoGUID(const SDL_JoystickID& id);
    static std::string GUIDButtonName(const JoystickInput::JoystickGUID id, Uint8 button);

    //Input Vector
    double lastRelativeX = 0;
	double lastRelativeY = 0;
    
    //Threading
    std::unique_ptr<std::thread> eventThread;
    bool keepThreadAlive = true;
    bool sdl_initialized = false;
    std::mutex sdl_external;
};