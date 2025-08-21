#pragma once

#include "DasherInput.h"
#include "Parameters.h"
#include "SettingsStore.h"
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include "gtkmm/togglebutton.h"
#include "gtkmm/label.h"
#include "UIComponents/SyncedTextbox.h"
#include "Preferences/DeviceSettingsProvider.h"
#include "UIComponents/PopoverMenuButtonInfo.h"
#include "SDLJoystick.h"

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

    //Options Stuff
    Gtk::Label nameLabelAxisX = Gtk::Label("Controller Axis for X-Input");
    Gtk::Label nameLabelAxisY = Gtk::Label("Controller Axis for Y-Input");
    SyncedTextbox valueAxisX;
    SyncedTextbox valueAxisY;
    Gtk::ToggleButton recordAxisButtonX = Gtk::ToggleButton("Record Axis");
    Gtk::ToggleButton recordAxisButtonY = Gtk::ToggleButton("Record Axis");
    PopoverMenuButtonInfo descriptionX = PopoverMenuButtonInfo("Click the record button and move the desired axis on your controller");
    PopoverMenuButtonInfo descriptionY = PopoverMenuButtonInfo(descriptionX.GetText());

    std::vector<SDLJoystick> controllers;
    SDLJoystick& GetControllerByID(SDLJoystick::ID id);
    SDLJoystick& GetControllerByGUID(SDLJoystick::GUID guid);
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