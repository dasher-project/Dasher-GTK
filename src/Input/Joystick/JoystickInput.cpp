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
        static std::vector<Dasher::Parameter> Buttons = {Dasher::Parameter::SP_JOYSTICK_DEVICE};
        if(std::find(Buttons.begin(), Buttons.end(), p) != Buttons.end()){
            PoplulateInputMaps();
            OpenNeededControllers();
        }
    });
}

JoystickInput::JoystickGUID JoystickInput::convertIDtoGUID(const SDL_JoystickID& id){
    SDL_GUID GUID = SDL_GetJoystickGUIDForID(id);
    JoystickGUID stringification = std::string(33,'\0');
    SDL_GUIDToString(GUID, stringification.data(), 33);
    stringification.resize(32); // trim 0 termination
    return stringification;
}

std::string JoystickInput::GUIDButtonName(const JoystickInput::JoystickGUID id, Uint8 button){
    if(id.size() != 32) return "";
    std::string res = std::string("JOY-00000000000000000000000000000000-00"); // "JOY-<GUID-32>-<TwoDigitButton>"
    std::memcpy(&res[4], id.data(), id.size());
    res[37] = char('0' + button / 10);
    res[38] = char('0' + button % 10);
    return res;
}

void JoystickInput::Activate() {
    if(eventThread) return;
    eventThread = std::make_unique<std::thread>([this](){
        if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
            std::cout << "Unable to initialize the joystick subsystem." << std::endl;
            std::cout << SDL_GetError() << std::endl;
            return;
        }
        sdl_initialized = true;

        GetAvailableDevices();

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
                        interface->MappedKeyDown(0, GUIDButtonName(openedControllers[event.jdevice.which], event.jbutton.button));
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_UP:
                    {
                        interface->MappedKeyUp(0, GUIDButtonName(openedControllers[event.jdevice.which], event.jbutton.button));
                    }
                    break;
                case SDL_EVENT_JOYSTICK_ADDED:
                    {
                        const JoystickGUID GUID = convertIDtoGUID(event.jdevice.which);
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

std::map<std::string, std::string> JoystickInput::GetAvailableDevices() {
    std::scoped_lock lock(sdl_external);
    std::map<JoystickGUID, std::string> deviceMap;
    
    if(!sdl_initialized) return deviceMap; //Not initialized

    int count;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    for(int i = 0; i < count; i++){
        deviceMap[convertIDtoGUID(joysticks[i])] = SDL_GetJoystickNameForID(joysticks[i]);
    }
    SDL_free(joysticks);

    return deviceMap;
}

void JoystickInput::OpenNeededControllers(){
    std::scoped_lock lock(sdl_external);
    if(!sdl_initialized) return;

    int count;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    for(int i = 0; i < count; i++){
        const JoystickGUID GUID = convertIDtoGUID(joysticks[i]);
        // Do we need this one and did we not open it so far?
        if(requestedControllers.count(GUID) && openedControllers.count(joysticks[i]) == 0 && SDL_OpenJoystick(joysticks[i])){
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
    requestedControllers = {"0300fa675e0400008e02000014017801"};

    XAxis = {"0300fa675e0400008e02000014017801", 0};
    YAxis = {"0300fa675e0400008e02000014017801", 1};
}

bool JoystickInput::GetVectorCoords(float &VectorX, float &VectorY){
    VectorX = lastRelativeX;
    VectorY = lastRelativeY;
    return true;
}


#pragma clang optimize on