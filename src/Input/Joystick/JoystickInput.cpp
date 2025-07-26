#include "JoystickInput.h"
#include "Parameters.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_joystick.h"
#include "SettingsStore.h"
#include "DasherController.h"
#include "gtkmm/togglebutton.h"
#include <iostream>
#include <limits>
#include <math.h>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <cstring>

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
    OpenNeededControllers(listeningForAxis);
}

JoystickInput::JoystickGUID JoystickInput::IDtoGUID(const SDL_JoystickID& id){
    SDL_GUID GUID = SDL_GetJoystickGUIDForID(id);
    JoystickGUID stringification = std::string(33,'\0');
    SDL_GUIDToString(GUID, stringification.data(), 33);
    stringification.resize(32); // trim 0 termination
    return stringification;
}

// Converts to format "JOY-<GUID-32>-<TwoDigitSpecifier>" 
std::string JoystickInput::GUIDAndSpecifierToString(const JoystickInput::JoystickGUID id, Uint8 specifier){
    if(id.size() != 32) return "";
    std::string res = std::string("JOY-00000000000000000000000000000000-00"); 
    std::memcpy(&res[4], id.data(), id.size());
    res[37] = char('0' + specifier / 10);
    res[38] = char('0' + specifier % 10);
    return res;
}

// Parsing "JOY-<GUID-32>-<TwoDigitSpecifier>" format into tuple
std::pair<JoystickInput::JoystickGUID, Uint8> JoystickInput::GUIDAndSpecifierFromString(const std::string& InputString){
    if(InputString.size() != 39 ||
        !(InputString[0] == 'J' && InputString[1] == 'O' && InputString[2] == 'Y' && InputString[3] == '-')){
            return {"", 0}; //Input format wrong
    }
    
    std::pair<JoystickInput::JoystickGUID, Uint8> res = {"00000000000000000000000000000000", 0};
    std::memcpy(res.first.data(), &InputString[4], res.first.size());
    res.second = (InputString[37] - '0') * 10 + (InputString[38] - '0');
    return res;
}

void JoystickInput::Activate() {
    if(eventThread) return;
    keepThreadAlive = true;
    eventThread = std::make_unique<std::thread>([this](){
        if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
            std::cout << "Unable to initialize the joystick subsystem." << std::endl;
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
                        if(listeningForAxis){
                            const double value = static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                            if(listeningAxisStartingValueMap[event.jdevice.which].count(event.jaxis.axis) == 0){
                                listeningAxisStartingValueMap[event.jdevice.which][event.jaxis.axis] = value;
                            }
                            const double difference = std::abs(listeningAxisStartingValueMap[event.jdevice.which][event.jaxis.axis] - value);
                            if(difference > 0.5){
                                AxisSelected.Broadcast(listeningParameter, GUIDAndSpecifierToString(openedControllers[event.jdevice.which], event.jbutton.button));
                                StartListenForAxis(false, Dasher::Parameter::PM_INVALID);
                                listeningAxisStartingValueMap[event.jdevice.which].clear();
                            }
                        } else {
                            if(XAxis.second == event.jaxis.axis && XAxis.first == openedControllers[event.jdevice.which]){
                                lastRelativeX = static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                            } else if(YAxis.second == event.jaxis.axis && YAxis.first == openedControllers[event.jdevice.which]){
                                lastRelativeY = -static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                            }
                        }
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                    {
                        interface->GetButtonMapper()->MappedKeyDown(GUIDAndSpecifierToString(openedControllers[event.jdevice.which], event.jbutton.button));
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_UP:
                    {
                        interface->GetButtonMapper()->MappedKeyUp(GUIDAndSpecifierToString(openedControllers[event.jdevice.which], event.jbutton.button));
                    }
                    break;
                case SDL_EVENT_JOYSTICK_ADDED:
                    {
                        const JoystickGUID GUID = IDtoGUID(event.jdevice.which);
                        if(requestedControllers.count(GUID) && openedControllers.count(event.jdevice.which) == 0 && SDL_OpenJoystick(event.jdevice.which)){
                            openedControllers[event.jdevice.which] = GUID;
                        }
                        JoysticksChanged.Broadcast();
                    }
                    break;
                case SDL_EVENT_JOYSTICK_REMOVED:
                    {
                        JoysticksChanged.Broadcast();
                    }
                    break;
                }
                std::flush(std::cout);
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
        const JoystickGUID GUID = IDtoGUID(joysticks[i]);
        // Do we need this one (requested or openAll)?
        if(requestedControllers.count(GUID) || openAll){
            // Was it not opened before?
            if(openedControllers.count(joysticks[i]) == 0){
                SDL_OpenJoystick(joysticks[i]);
                openedControllers[joysticks[i]] = GUID;
            }
        } else
        //Opened before but not needed anymore
        if(openedControllers.count(joysticks[i])) {
            SDL_CloseJoystick(SDL_GetJoystickFromID(joysticks[i]));
            openedControllers.erase(joysticks[i]);
        }
    }
    SDL_free(joysticks);
}

void JoystickInput::PoplulateInputMaps(){
    requestedControllers.clear();

    // Read axis mappings
    XAxis = GUIDAndSpecifierFromString(interface->GetStringParameter(Dasher::Parameter::SP_JOYSTICK_XAXIS));
    YAxis = GUIDAndSpecifierFromString(interface->GetStringParameter(Dasher::Parameter::SP_JOYSTICK_YAXIS));
    
    // Find controllers to open
    if(!XAxis.first.empty()) requestedControllers.insert(XAxis.first);
    if(!YAxis.first.empty()) requestedControllers.insert(YAxis.first);

    for(auto& [virtualKey, deviceKey] : interface->GetButtonMapper()->GetKeyMap()){
        const std::pair<JoystickInput::JoystickGUID, Uint8> res = GUIDAndSpecifierFromString(deviceKey);
        if(!res.first.empty()) requestedControllers.insert(res.first);
    }
}

bool JoystickInput::GetVectorCoords(float &VectorX, float &VectorY){
    VectorX = lastRelativeX;
    VectorY = lastRelativeY;
    return true;
}


#pragma clang optimize on