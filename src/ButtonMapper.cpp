#include "ButtonMapper.h"
#include "DasherController.h"
#include "DasherTypes.h"
#include "UIComponents/RenderingCanvas.h"

ButtonMapper::ButtonMapper(Dasher::CSettingsStore* settings, DasherController* controller) : controller(controller), settings(settings) {
    settings->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter p){
		if(p == Dasher::Parameter::SP_BUTTON_MAPPINGS) InitButtonMap();
	});
}

bool ButtonMapper::MappedKeyDown(const std::string& key) {
	if(listenForAnyKey){
		AnyKeyPressed.Broadcast(key);
		ListenForAnyKey(false); //disable again
	}

	bool handled = false;
	for(auto& [virtualKey, deviceKey] : keyMappings){
		if(deviceKey == key){
			controller->KeyDown(controller->getCurrentMS(), virtualKey);
			handled = true;
		}
	}
	return handled;
}

bool ButtonMapper::MappedKeyUp(const std::string& key) {
	bool handled = false;
	for(auto& [virtualKey, deviceKey] : keyMappings){
		if(deviceKey == key){
			controller->KeyUp(controller->getCurrentMS(), virtualKey);
			handled = true;
		}
	}
	return handled;
}

void ButtonMapper::AddKeyToButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping){
	keyMappings.insert({key, mapping});
	WriteButtonMap();
	ButtonMappingsChanged.Broadcast();
}

void ButtonMapper::RemoveKeyFromButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping){
	keyMappings.erase({key, mapping});
	WriteButtonMap();
	ButtonMappingsChanged.Broadcast();
}

void ButtonMapper::ListenForAnyKey(bool startStop){
	listenForAnyKey = startStop;
	ListeningForAllKeys.Broadcast(startStop);
}

void ButtonMapper::InitButtonMap() {
	if(ignoreChangesInButtonMap) return;

	const std::string& mappings = controller->GetStringParameter(Dasher::Parameter::SP_BUTTON_MAPPINGS);
	
	keyMappings.clear(); //empty the map first
	auto left = mappings.begin();
	std::string deviceKey;
    for (auto it = left; it != mappings.end(); ++it) 
    { 
        if (*it == ';' || it == std::prev(mappings.end())) 
        {
			const int offset = (*it != ';') ? 1 : 0; // must be last character of string and instead of ';'
			const Dasher::Keys::VirtualKey virtualKey = Dasher::Keys::StringToVirtualKey(std::string(&*left, it - left + offset));
			if(!deviceKey.empty() && virtualKey != Dasher::Keys::Invalid_Key) keyMappings.insert({virtualKey, deviceKey});
			deviceKey = "";
            left = it + 1; 
        }
		if (*it == ':') 
        {
            deviceKey = std::string(&*left, it - left);
            left = it + 1; 
        }
    }

	// Add Mousebuttons if we have nothing else
	if(keyMappings.size() == 0){
		keyMappings.insert({Dasher::Keys::Primary_Input, RenderingCanvas::MouseButtonNamePrimary});
		keyMappings.insert({Dasher::Keys::Secondary_Input, RenderingCanvas::MouseButtonNameSecondary});
	}

	ButtonMappingsChanged.Broadcast();
}

// write button map as "<deviceKey>:<virtualKey>;<deviceKey>:<virtualKey>;[...]", where both keys could potentially be doubled, but any mapping is unique
void ButtonMapper::WriteButtonMap(){
	std::string output;
	for(auto& [virtualKey, deviceKey] : keyMappings){
		output += deviceKey + ":" + Dasher::Keys::VirtualKeyToString(virtualKey) + ";";
	}
	ignoreChangesInButtonMap = true;
	controller->SetStringParameter(Dasher::Parameter::SP_BUTTON_MAPPINGS, output);
	ignoreChangesInButtonMap = false;
}