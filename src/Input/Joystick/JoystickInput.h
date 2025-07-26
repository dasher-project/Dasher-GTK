#pragma once

#include "DasherInput.h"
#include "Parameters.h"
#include "SettingsStore.h"
#include <memory>
#include <mutex>
#include <thread>
#include "SDL3/SDL_joystick.h"
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "gtkmm/togglebutton.h"
#include "gtkmm/label.h"
#include "UIComponents/SyncedTextbox.h"
#include "Preferences/DeviceSettingsProvider.h"
#include "UIComponents/PopoverMenuButtonInfo.h"

class DasherController;

class JoystickInput : public Dasher::CDasherVectorInput, public DeviceSettingsProvider
{
public:
    JoystickInput(DasherController* interface, std::shared_ptr<Dasher::CSettingsStore> settings);

    virtual bool GetVectorCoords(float &VectorX, float &VectorY) override;

    virtual void Activate() override;
    virtual void Deactivate() override;

    void OpenNeededControllers(bool openAll = false);
    void PoplulateInputMaps();

    bool FillInputDeviceSettings(Gtk::Grid* grid) override;

protected:
    DasherController* interface;
    std::shared_ptr<Dasher::CSettingsStore> settings;
    Event<> JoysticksChanged;
    
    // Listen for Axis stuff
    void StartListenForAxis(bool startStopListen, Dasher::Parameter listenParam);
    bool listeningForAxis = false;
    Dasher::Parameter listeningParameter = Dasher::Parameter::PM_INVALID;
    Event<Dasher::Parameter, std::string> AxisSelected;
    std::unordered_map<SDL_JoystickID, std::unordered_map<Uint8, double>> listeningAxisStartingValueMap; //Stores the starting value (zero position) for each axis when listening for movement

    //Options Stuff
    Gtk::Label nameLabelAxisX = Gtk::Label("Controller Axis for X-Input");
    Gtk::Label nameLabelAxisY = Gtk::Label("Controller Axis for Y-Input");
    SyncedTextbox valueAxisX;
    SyncedTextbox valueAxisY;
    Gtk::ToggleButton recordAxisButtonX = Gtk::ToggleButton("Record Axis");
    Gtk::ToggleButton recordAxisButtonY = Gtk::ToggleButton("Record Axis");
    PopoverMenuButtonInfo descriptionX = PopoverMenuButtonInfo("Click the record button and move the desired axis on your controller");
    PopoverMenuButtonInfo descriptionY = PopoverMenuButtonInfo(descriptionX.GetText());

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