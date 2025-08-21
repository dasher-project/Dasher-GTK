#include "SDLJoystick.h"
#include <iostream>
#include <cstring>
#include <string>

SDLJoystick::SDLJoystick(const GUID guid): joystickGUID(guid)
{}

SDLJoystick::~SDLJoystick()
{
    Close();
}

const SDLJoystick::GUID SDLJoystick::GetGUID() const
{
    return joystickGUID;
}

const SDLJoystick::ID SDLJoystick::GetID() const
{
    return joystickID;
}

void SDLJoystick::SetID(ID id)
{
    joystickID = id;
}

void SDLJoystick::SetAxisMemoryValue(Uint8 axisID, double value)
{
    memoryAxisValues[axisID] = value;
}

bool SDLJoystick::HasAxisMemoryValue(Uint8 axisID) const
{
    return memoryAxisValues.count(axisID);
}

double SDLJoystick::GetAxisMemoryValue(Uint8 axisID) const
{
    if(memoryAxisValues.count(axisID)){
        return memoryAxisValues.at(axisID);
    }
    return 0;
}

void SDLJoystick::ClearAxisMemoryValues()
{
    memoryAxisValues.clear();
}

bool SDLJoystick::IsOpened() const
{
    return opened;
}

bool SDLJoystick::IsRequested() const
{
    return requested;
}

void SDLJoystick::SetRequested(bool b)
{
    requested = b;
}

void SDLJoystick::PlayRumble(float strength, Uint32 duration_ms) const
{
    if(hapticInstance) SDL_PlayHapticRumble(hapticInstance, strength, duration_ms);
    if(gamepadInstance) SDL_RumbleGamepad(gamepadInstance, 0xFFFF*strength, 0xFFFF*strength, duration_ms);
}

bool SDLJoystick::Open()
{
    if(joystickID == 0) return false;

    joystickInstance = SDL_OpenJoystick(joystickID);
    if(!joystickInstance){
        std::cout << SDL_GetError() << std::endl;
        return false;
    }
    opened = true;

    if(SDL_IsGamepad(joystickID)){
        gamepadInstance = SDL_OpenGamepad(joystickID);
        if(!gamepadInstance){
            std::cout << SDL_GetError() << std::endl;
        }
    } else {
        hapticInstance = SDL_OpenHapticFromJoystick(joystickInstance);
        if(hapticInstance && !SDL_InitHapticRumble(hapticInstance)) hapticInstance = nullptr;
    }

    return true;
}

void SDLJoystick::Close()
{
    if(gamepadInstance) SDL_CloseGamepad(gamepadInstance);
    if(joystickInstance) SDL_CloseJoystick(joystickInstance);
}

SDLJoystick::GUID SDLJoystick::IDtoGUID(const SDL_JoystickID id)
{
    SDLJoystick::GUID stringification = std::string(33,'\0');
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), stringification.data(), 33);
    stringification.resize(32); // trim 0 termination
    return stringification;
}

std::string SDLJoystick::AxisToString(Uint8 specifier) const
{
    return GUIDAndSpecifierToString(joystickGUID, specifier);
}

std::string SDLJoystick::GUIDAndSpecifierToString(const SDLJoystick::GUID id, Uint8 specifier)
{
    if(id.size() != 32) return "";
    std::string res = std::string("JOY-00000000000000000000000000000000-00"); 
    std::memcpy(&res[4], id.data(), id.size());
    res[37] = char('0' + specifier / 10);
    res[38] = char('0' + specifier % 10);
    return res;
}

std::pair<SDLJoystick::GUID, Uint8> SDLJoystick::GUIDAndSpecifierFromString(const std::string& InputString)
{
    if(InputString.size() != 39 ||
        !(InputString[0] == 'J' && InputString[1] == 'O' && InputString[2] == 'Y' && InputString[3] == '-')){
        return {"", 0}; //Input format wrong
    }
        
    std::pair<SDLJoystick::GUID, Uint8> res = {"00000000000000000000000000000000", 0};
    std::memcpy(res.first.data(), &InputString[4], res.first.size());
    res.second = (InputString[37] - '0') * 10 + (InputString[38] - '0');
    return res;
}
