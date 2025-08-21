#include "JoystickInput.h"
#include "Parameters.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_joystick.h"
#include "SettingsStore.h"
#include "DasherController.h"
#include "gtkmm/togglebutton.h"
#include <math.h>
#include <memory>
#include <string>
#include <iostream>
#include "SDLJoystick.h"

#pragma clang optimize off

JoystickInput::JoystickInput(DasherController* interface, std::shared_ptr<Dasher::CSettingsStore> settings) :
    interface(interface), settings(settings), Dasher::CDasherVectorInput("Joystick"),
    valueAxisX(Dasher::Parameter::SP_JOYSTICK_XAXIS, settings), valueAxisY(Dasher::Parameter::SP_JOYSTICK_YAXIS, settings)
{
    PoplulateInputMaps();
    settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter p){
        if(p == Dasher::SP_JOYSTICK_XAXIS || p == Dasher::SP_JOYSTICK_YAXIS){
            PoplulateInputMaps();
            OpenNeededControllers();
        }
    });
    interface->GetButtonMapper()->ButtonMappingsChanged.Subscribe(this, [this](){
        PoplulateInputMaps();
        OpenNeededControllers();
    });
    interface->GetButtonMapper()->ListeningForAllKeys.Subscribe(this, [this](bool listening){
        OpenNeededControllers(listening);
    });

    // create options labels etc.
    nameLabelAxisX.set_halign(Gtk::Align::START);
    nameLabelAxisY.set_halign(Gtk::Align::START);
    recordAxisButtonX.property_active().signal_changed().connect([this](){
        if(recordAxisButtonX.get_active() && recordAxisButtonY.get_active()) recordAxisButtonY.set_active(false); //Manual grouping to allow deactivation

        StartListenForAxis(recordAxisButtonX.get_active(), Dasher::Parameter::SP_JOYSTICK_XAXIS);

        if(recordAxisButtonX.get_active()){
            AxisSelected.Subscribe(&recordAxisButtonX, [this](Dasher::Parameter param, std::string selectedAxis){
                if(param != Dasher::Parameter::SP_JOYSTICK_XAXIS) return;
                valueAxisX.SetText(selectedAxis);
                recordAxisButtonX.set_active(false);
            });
        } else {
            AxisSelected.Unsubscribe(&recordAxisButtonX);
        }
    });
    recordAxisButtonY.property_active().signal_changed().connect([this](){
        if(recordAxisButtonX.get_active() && recordAxisButtonY.get_active()) recordAxisButtonX.set_active(false); //Manual grouping to allow deactivation

        StartListenForAxis(recordAxisButtonY.get_active(), Dasher::Parameter::SP_JOYSTICK_YAXIS);

        if(recordAxisButtonY.get_active()){
            AxisSelected.Subscribe(&recordAxisButtonY, [this](Dasher::Parameter param, std::string selectedAxis){
                if(param != Dasher::Parameter::SP_JOYSTICK_YAXIS) return;
                valueAxisY.SetText(selectedAxis);
                recordAxisButtonY.set_active(false);
            });
        } else {
            AxisSelected.Unsubscribe(&recordAxisButtonY);
        }
    });
}

bool JoystickInput::FillInputDeviceSettings(Gtk::Grid* grid){   
    grid->attach(nameLabelAxisX, 0, 0);
    grid->attach(valueAxisX, 1, 0);
    grid->attach(recordAxisButtonX, 2, 0);
    grid->attach(descriptionX, 3, 0);

    grid->attach(nameLabelAxisY, 0, 1);
    grid->attach(valueAxisY, 1, 1);
    grid->attach(recordAxisButtonY, 2, 1);
    grid->attach(descriptionY, 3, 1);
    return true;
}

void JoystickInput::StartListenForAxis(bool startStopListen, Dasher::Parameter listenParam){
    listeningForAxis = startStopListen;
    listeningParameter = listenParam;
    if(!startStopListen){
        for(SDLJoystick& j : controllers){
            j.ClearAxisMemoryValues();
        }
    }
    OpenNeededControllers(startStopListen);
}

SDLJoystick& JoystickInput::GetControllerByID(SDLJoystick::ID id){
    auto controller = std::find_if(controllers.begin(), controllers.end(), [&id](const SDLJoystick& entry) {return entry.GetID() == id; });
    if(controller != controllers.end()) return *controller;

    // return controller by associated GUID and set ID
    SDLJoystick& newController = GetControllerByGUID(SDLJoystick::IDtoGUID(id));
    newController.SetID(id);
    return newController;
}


SDLJoystick& JoystickInput::GetControllerByGUID(SDLJoystick::GUID guid){
    auto controller = std::find_if(controllers.begin(), controllers.end(), [&guid](const SDLJoystick& entry) {return entry.GetGUID() == guid; });
    if(controller != controllers.end()) return *controller;
    controllers.push_back(SDLJoystick(guid));
    return controllers.back();
}

void JoystickInput::Activate() {
    if(eventThread) return;
    keepThreadAlive = true;
    eventThread = std::make_unique<std::thread>([this](){
        if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMEPAD)) {
            std::cout << "Unable to initialize the joystick or haptic subsystem." << std::endl;
            std::cout << SDL_GetError() << std::endl;
            return;
        }
        sdl_initialized = true;

        SDL_Event event;
        while (keepThreadAlive) {
            while (SDL_PollEvent(&event) != 0) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    keepThreadAlive = false;
                    break;
                case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                    {
                        const double value = static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                        SDLJoystick& controller = GetControllerByID(event.jdevice.which);
                        
                        if(listeningForAxis){
                            if(!controller.HasAxisMemoryValue(event.jaxis.axis)){
                                controller.SetAxisMemoryValue(event.jaxis.axis, value);
                            }
                            
                            const double difference = std::abs(controller.GetAxisMemoryValue(event.jaxis.axis) - value);
                            if(difference > 0.5){
                                AxisSelected.Broadcast(listeningParameter, controller.AxisToString(event.jaxis.axis));
                                StartListenForAxis(false, Dasher::Parameter::PM_INVALID);
                            }
                        } else {
                            if(XAxis.second == event.jaxis.axis && XAxis.first == controller.GetGUID()){
                                lastRelativeX = value;
                            } else if(YAxis.second == event.jaxis.axis && YAxis.first == controller.GetGUID()){
                                lastRelativeY = -value;
                            }
                        }
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                    {
                        SDLJoystick& controller = GetControllerByID(event.jdevice.which);
                        interface->GetButtonMapper()->MappedKeyDown(controller.ButtonToString(event.jbutton.button));
                    }
                    break;
                    case SDL_EVENT_JOYSTICK_BUTTON_UP:
                    {
                        SDLJoystick& controller = GetControllerByID(event.jdevice.which);
                        interface->GetButtonMapper()->MappedKeyUp(controller.ButtonToString(event.jbutton.button));
                    }
                    break;
                case SDL_EVENT_JOYSTICK_ADDED:
                    {
                        SDLJoystick& controller = GetControllerByID(event.jdevice.which);
                        if(!controller.IsOpened() && controller.IsRequested() && controller.Open()){
                            JoysticksChanged.Broadcast();
                        }
                    }
                    break;
                case SDL_EVENT_JOYSTICK_REMOVED:
                    {
                        // Remove controller from internal list
                        controllers.erase(std::remove_if(controllers.begin(), controllers.end(), [&event](const SDLJoystick& entry) {return entry.GetID() == event.jdevice.which; }));
                        JoysticksChanged.Broadcast();
                    }
                    break;
                }
            }
        }
        
        sdl_initialized = false;
        // Free up any resources that SDL allocated.
        sdl_external.lock();
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        sdl_external.unlock();
    });
}

void JoystickInput::Deactivate() {
    keepThreadAlive = false;
    if(eventThread->joinable()) eventThread->join();
    eventThread.reset();
}

void JoystickInput::OpenNeededControllers(bool openAll){
    std::scoped_lock lock(sdl_external);
    if(!sdl_initialized) return;

    int count;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    for(int i = 0; i < count; i++){
        SDLJoystick& c = GetControllerByID(joysticks[i]);

        if(c.IsRequested() || openAll){
            if(!c.IsOpened()) c.Open();
        } else
        //Opened before but not needed anymore
        if (c.IsOpened()) {
            controllers.erase(std::remove_if(controllers.begin(), controllers.end(), [&joysticks, i](const SDLJoystick& entry) {return entry.GetID() == joysticks[i]; }));
        }
    }
    SDL_free(joysticks);
}

void JoystickInput::PoplulateInputMaps(){
    for(SDLJoystick& j : controllers){
        j.SetRequested(false);
    }

    // Read axis mappings
    XAxis = SDLJoystick::GUIDAndSpecifierFromString(interface->GetStringParameter(Dasher::Parameter::SP_JOYSTICK_XAXIS));
    YAxis = SDLJoystick::GUIDAndSpecifierFromString(interface->GetStringParameter(Dasher::Parameter::SP_JOYSTICK_YAXIS));
    
    // Add controllers to internal list
    if(!XAxis.first.empty()) GetControllerByGUID(XAxis.first).SetRequested(true);
    if(!YAxis.first.empty()) GetControllerByGUID(YAxis.first).SetRequested(true);

    for(auto& [virtualKey, deviceKey] : interface->GetButtonMapper()->GetKeyMap()){
        const std::pair<SDLJoystick::GUID, Uint8> res = SDLJoystick::GUIDAndSpecifierFromString(deviceKey);
        if(!res.first.empty()) GetControllerByGUID(res.first).SetRequested(true);
    }
}

bool JoystickInput::GetVectorCoords(float &VectorX, float &VectorY){
    VectorX = lastRelativeX;
    VectorY = lastRelativeY;
    return true;
}


#pragma clang optimize on