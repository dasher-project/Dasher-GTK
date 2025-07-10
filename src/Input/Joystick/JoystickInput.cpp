#include "JoystickInput.h"
#include "Parameters.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_joystick.h"
#include "SettingsStore.h"
#include "DasherController.h"
#include <iostream>
#include <limits>
#include <math.h>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <cstring>

#pragma clang optimize off

JoystickInput::JoystickInput(DasherController* interface, Dasher::CSettingsStore* settings) : interface(interface), settings(settings), Dasher::CDasherVectorInput("Joystick") {
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
                        if(XAxis.second == event.jaxis.axis && XAxis.first == openedControllers[event.jdevice.which]){
                            lastRelativeX = static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                        } else if(YAxis.second == event.jaxis.axis && YAxis.first == openedControllers[event.jdevice.which]){
                            lastRelativeY = -static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
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
        // Do we need this one (requested or openAll) and did we not open it so far?
        if((requestedControllers.count(GUID) || openAll) && openedControllers.count(joysticks[i]) == 0 && SDL_OpenJoystick(joysticks[i])){
            openedControllers[joysticks[i]] = GUID;
        } else if(openedControllers.count(joysticks[i])) {
            //Opened before but not needed anymore
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