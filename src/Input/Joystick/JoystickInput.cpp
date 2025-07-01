
#include "JoystickInput.h"
#include <iostream>
#include <limits>
#include <math.h>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include "ColorPalette.h"
#include "DasherTypes.h"
#include "Parameters.h"
#include "SDL.h"
#include "SDL_joystick.h"
#include "SettingsStore.h"

#pragma clang optimize off

JoystickInput::JoystickInput(Dasher::CDasherInterfaceBase* interface, Dasher::CSettingsStore* settings) : interface(interface), settings(settings), Dasher::CScreenCoordInput("Joystick") {
    PoplulateInputMaps();
    settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter p){
        static std::vector<Dasher::Parameter> Buttons = {Dasher::Parameter::SP_JOYSTICK_DEVICE};
        if(std::find(Buttons.begin(), Buttons.end(), p) != Buttons.end()){
            PoplulateInputMaps();
            OpenNeededControllers();
        }
    });
}

std::string convertIDtoGUID(const SDL_JoystickID& id){
    SDL_GUID GUID = SDL_GetJoystickGUIDForID(id);
    std::string stringification = std::string(33,'\0');
    SDL_GUIDToString(GUID, stringification.data(), 33);
    stringification.resize(32); // trim 0 termination
    return stringification;
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
                        // std::cout << "The value of axis " << unsigned(event.jaxis.axis) << " was changed to " << event.jaxis.value << ".\n";
                        if(XAxis.second == event.jaxis.axis && XAxis.first == openedControllers[event.jdevice.which]){
                            lastRelativeX = static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                        } else if(YAxis.second == event.jaxis.axis && YAxis.first == openedControllers[event.jdevice.which]){
                            lastRelativeY = -static_cast<double>(event.jaxis.value) / std::numeric_limits<Sint16>().max();
                        }
                        // std::cout << "X: " << lastRelativeX << ", Y: " << lastRelativeY << std::endl;
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                    {
                        std::cout << "The button with index " << unsigned(event.jbutton.button) << " was pressed.\n";
                        std::unordered_map<Uint8, Dasher::Keys::VirtualKey>& buttonMap = GUIDButtonMap[openedControllers[event.jdevice.which]];
                        if(buttonMap.count(event.jbutton.button)){
                            interface->KeyDown(0, buttonMap[event.jbutton.button]); //Todo replace time
                        }
                    }
                    break;
                case SDL_EVENT_JOYSTICK_BUTTON_UP:
                    {
                        std::cout << "The button with index " << unsigned(event.jbutton.button) << " was released.\n";
                        std::unordered_map<Uint8, Dasher::Keys::VirtualKey>& buttonMap = GUIDButtonMap[openedControllers[event.jdevice.which]];
                        if(buttonMap.count(event.jbutton.button)){
                            interface->KeyUp(0, buttonMap[event.jbutton.button]); //Todo replace time
                        }
                    }
                    break;
                case SDL_EVENT_JOYSTICK_ADDED:
                    {
                        const std::string GUID = convertIDtoGUID(event.jdevice.which);
                        if(GUIDButtonMap.count(GUID) && openedControllers.count(event.jdevice.which) == 0 && SDL_OpenJoystick(event.jdevice.which)){
                            std::cout << "Opened Controller " << GUID << " for reading." << std::endl;
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
    std::map<std::string, std::string> res;
    
    if(!sdl_initialized) return res; //Not initialized

    int count;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    for(int i = 0; i < count; i++){
        res[convertIDtoGUID(joysticks[i])] = SDL_GetJoystickNameForID(joysticks[i]);
    }
    SDL_free(joysticks);

    return res;
}

void JoystickInput::OpenNeededControllers(){
    std::scoped_lock lock(sdl_external);
    if(!sdl_initialized) return;

    int count;
    SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    for(int i = 0; i < count; i++){
        const std::string GUID = convertIDtoGUID(joysticks[i]);
        // Do we need this one and did we not open it so far?
        if(GUIDButtonMap.count(GUID) && openedControllers.count(joysticks[i]) == 0 && SDL_OpenJoystick(joysticks[i])){
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
    GUIDButtonMap = {{"0300fa675e0400008e02000014017801",{
        {0,Dasher::Keys::Primary_Input},
        {1,Dasher::Keys::Secondary_Input}
    }}};

    XAxis = {"0300fa675e0400008e02000014017801", 0};
    YAxis = {"0300fa675e0400008e02000014017801", 1};
}

bool JoystickInput::GetScreenCoords(Dasher::screenint& iX, Dasher::screenint& iY, Dasher::CDasherView* pView)
{
	const Dasher::CDasherView::DasherCoordScreenRegion visibleRegion = pView->VisibleRegion();

	// const double vectorLength = static_cast<double>(std::min(Dasher::CDasherModel::ORIGIN_Y - screenRegion.minY, screenRegion.maxY - Dasher::CDasherModel::ORIGIN_Y));
	// ;

	// pView->Dasher2Screen(
	// 	Dasher::CDasherModel::ORIGIN_X - static_cast<Dasher::myint>(lastRelativeX * (normalization > 1.0 ? vectorLength / normalization : vectorLength)),
	// 	Dasher::CDasherModel::ORIGIN_Y - static_cast<Dasher::myint>(lastRelativeY * (normalization > 1.0 ? vectorLength / normalization : vectorLength)),
	// 	iX, iY);

    // Dasher::myint x,y;
    // pView->Polar2Dasher(
    //     sqrt(lastRelativeX * lastRelativeX + lastRelativeY * lastRelativeY),
    //     atan2(lastRelativeY,lastRelativeX),
    //     x,y);
    // pView->Dasher2Screen(x, y, iX, iY);

    // const DasherCoordScreenRegion visibleRegion = VisibleRegion();

	const Dasher::myint minX = std::min(std::abs(visibleRegion.maxX - Dasher::CDasherModel::ORIGIN_X), std::abs(visibleRegion.minX - Dasher::CDasherModel::ORIGIN_X));
	const Dasher::myint minY = std::min(std::abs(visibleRegion.maxX - Dasher::CDasherModel::ORIGIN_Y), std::abs(visibleRegion.minX - Dasher::CDasherModel::ORIGIN_Y));
	const Dasher::myint minExtend = std::min(minX,minY);

    const double normalization = sqrt(lastRelativeX * lastRelativeX + lastRelativeY * lastRelativeY);

	pView->Dasher2Screen(
		Dasher::CDasherModel::ORIGIN_X - static_cast<Dasher::myint>(lastRelativeX * minExtend / (normalization > 1.0 ? normalization : 1)),
		Dasher::CDasherModel::ORIGIN_Y - static_cast<Dasher::myint>(lastRelativeY * minExtend / (normalization > 1.0 ? normalization : 1)),
		iX, iY);

	return true;
}


#pragma clang optimize on