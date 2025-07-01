#pragma once

#include "DasherInput.h"
#include "DasherInterfaceBase.h"
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "DasherTypes.h"
#include "SDL.h"

class JoystickInput : public Dasher::CScreenCoordInput
{
public:
    JoystickInput(Dasher::CDasherInterfaceBase* interface, Dasher::CSettingsStore* settings);

    virtual bool GetScreenCoords(Dasher::screenint &iX, Dasher::screenint &iY, Dasher::CDasherView *pView) override;

    virtual void Activate() override;
    virtual void Deactivate() override;

    // bool OpenCurrentJoystick();
    void OpenNeededControllers();
    void PoplulateInputMaps();
    std::map<std::string, std::string> GetAvailableDevices(); 

protected:
    Dasher::CDasherInterfaceBase* interface;
    Dasher::CSettingsStore* settings;
    Event<> JoysticksChanged;
    
    std::unordered_map<SDL_JoystickID, std::string> openedControllers; //Known ControllerIDs to GUID Mapping
    std::unordered_map<std::string, std::unordered_map<Uint8, Dasher::Keys::VirtualKey>> GUIDButtonMap; // GUID to Keymap
    std::pair<std::string, Uint8> XAxis; // GUID to AxisNum
    std::pair<std::string, Uint8> YAxis; // GUID to AxisNum

    //Input Vector
    double lastRelativeX = 0;
	double lastRelativeY = 0;
    
    //Threading
    std::unique_ptr<std::thread> eventThread;
    bool keepThreadAlive = true;
    bool sdl_initialized = false;
    std::mutex sdl_external;
};