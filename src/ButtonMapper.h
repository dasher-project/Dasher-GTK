#pragma once

#include "DasherTypes.h"
#include "SettingsStore.h"
#include "Event.h"
#include <string>
#include <unordered_set>
#include <utility>

class DasherController;

struct hash_pair {
    template <class T1, class T2>
    size_t operator()(const std::pair<T1, T2>& p) const
    {
        return hash_combine(std::hash<T1>{}(p.first), std::hash<T2>{}(p.second));
    }
	// Taken from https://stackoverflow.com/a/27952689
	size_t hash_combine( size_t lhs, size_t rhs ) const {
		if constexpr (sizeof(size_t) >= 8) {
			lhs ^= rhs + 0x517cc1b727220a95 + (lhs << 6) + (lhs >> 2);
		} else {
			lhs ^= rhs + 0x9e3779b9 + (lhs << 6) + (lhs >> 2);
		}
		return lhs;
	}
};

class ButtonMapper {
public:
    ButtonMapper(Dasher::CSettingsStore* settings, DasherController* controller);

    bool MappedKeyDown(const std::string& key);
  	bool MappedKeyUp(const std::string& key);
	void InitButtonMap();
	void WriteButtonMap();
	void AddKeyToButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping);
	void RemoveKeyFromButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping);
	void ListenForAnyKey(bool startStop = true);
	bool IsListeningForAnyKey(){return listenForAnyKey;}

	const std::unordered_set<std::pair<Dasher::Keys::VirtualKey, std::string>,hash_pair>& GetKeyMap() {return keyMappings;}
	Event<> ButtonMappingsChanged;
	Event<std::string> AnyKeyPressed;
	Event<bool> ListeningForAllKeys;
protected:
	bool listenForAnyKey = false;
	bool ignoreChangesInButtonMap = false;
	//Key Mappings from settings
	std::unordered_set<std::pair<Dasher::Keys::VirtualKey, std::string>,hash_pair> keyMappings;
    DasherController* controller;
    Dasher::CSettingsStore* settings;
};