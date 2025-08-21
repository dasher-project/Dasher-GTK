#pragma once

#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_haptic.h"
#include <string>
#include <unordered_map>

class SDLJoystick {
public:
    typedef std::string GUID;
    typedef SDL_JoystickID ID;

    SDLJoystick(const GUID guid);;
    ~SDLJoystick();

    //ID Conversion Stuff
    const GUID GetGUID() const;
    const ID GetID() const;
    void SetID(ID id);

    // Store Axis Values
    void SetAxisMemoryValue(Uint8 axisID, double value);
    bool HasAxisMemoryValue(Uint8 axisID) const;
    double GetAxisMemoryValue(Uint8 axisID) const;
    void ClearAxisMemoryValues();

    // Handling of opening
    bool IsOpened() const;
    bool IsRequested() const;
    void SetRequested(bool b);

    //Haptics
    void PlayRumble(float strength, Uint32 duration_ms) const;

private:
    GUID joystickGUID;
    ID joystickID = 0;
    bool opened = false;
    bool requested = false;
    SDL_Joystick* joystickInstance = nullptr;
    SDL_Gamepad* gamepadInstance = nullptr;
    SDL_Haptic* hapticInstance = nullptr;
    std::unordered_map<Uint8, double> memoryAxisValues;

public:    
    bool Open();
    void Close();

    std::string AxisToString(Uint8 specifier) const;
    inline std::string ButtonToString(Uint8 specifier) const { //Handled the same as Axis
        return AxisToString(specifier);
    }; 
    
    static GUID IDtoGUID(const SDL_JoystickID id);
    // Converts to format "JOY-<GUID-32>-<TwoDigitSpecifier>" 
    static std::string GUIDAndSpecifierToString(const SDLJoystick::GUID id, Uint8 specifier);
    // Parsing "JOY-<GUID-32>-<TwoDigitSpecifier>" format into tuple
    static std::pair<SDLJoystick::GUID, Uint8> GUIDAndSpecifierFromString(const std::string& InputString);
};