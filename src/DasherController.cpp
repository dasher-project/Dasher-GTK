#include "DasherController.h"
#include "DasherInterfaceBase.h"
#include "DasherTypes.h"
#include "Input/Joystick/JoystickInput.h"
#include "Parameters.h"
#include <iostream>
#include <iterator>
#include <string>

class XmlServerStore;

DasherController::DasherController(std::shared_ptr<Dasher::CSettingsStore> pSettingsStore): CDashIntfSettings(pSettingsStore.get())
{
	m_pSettingsStore->OnParameterChanged.Subscribe(this, [this](Dasher::Parameter p){
		Dasher::CDasherInterfaceBase::HandleParameterChange(p);
		if(p == Dasher::Parameter::SP_BUTTON_MAPPINGS) InitButtonMap();
	});
}

void DasherController::CreateModules(){
	CDashIntfSettings::CreateModules();
	GetModuleManager()->RegisterInputDeviceModule(new JoystickInput(this, m_pSettingsStore));

	testInput = std::make_unique<FakeInput>();
    GetModuleManager()->RegisterInputDeviceModule(testInput.get(), true);
}

void DasherController::editOutput(const std::string& strText, Dasher::CDasherNode* pNode) {

	if (Buffer.capacity() < strText.length() + Buffer.size()) {
		Buffer.reserve((strText.length() + Buffer.size()) * 2);
	}
	Buffer.append(strText);
	OnBufferChange.Broadcast(Buffer);
	Cursor += static_cast<unsigned int>(strText.length());
	CDasherInterfaceBase::editOutput(strText, pNode);
}

void DasherController::editDelete(const std::string& strText, Dasher::CDasherNode* pNode) {
	if(0 == Buffer.compare(Buffer.length() - strText.length(), strText.length(), strText))
	{
		Buffer.erase(Buffer.length() - strText.length(), strText.length());
		OnBufferChange.Broadcast(Buffer);
	}
	CDasherInterfaceBase::editDelete(strText, pNode);
}

unsigned DasherController::ctrlMove(bool bForwards, Dasher::EditDistance dist)
{
	if (dist == Dasher::EditDistance::EDIT_CHAR) {
		if (bForwards) Cursor++;
		else Cursor--;
	}
	return Cursor;
}

unsigned DasherController::ctrlDelete(bool bForwards, Dasher::EditDistance dist)
{
	if(dist == Dasher::EditDistance::EDIT_CHAR) {

		Buffer.erase(Cursor - (bForwards ? 0 : 1), 1);
		OnBufferChange.Broadcast(Buffer);
	}
	if(!bForwards) Cursor--;
	return Cursor;
}

std::string DasherController::GetContext(unsigned iStart, unsigned iLength)
{
	return Buffer.substr(iStart, iLength);
}

std::string DasherController::GetAllContext()
{
	std::string CurrentBuffer = Buffer;
	SetBuffer(0);
	return CurrentBuffer;
}

std::string DasherController::GetTextAroundCursor(Dasher::EditDistance iDist) {
	if (Buffer.length() > Cursor && Buffer.length() >= 2) {
		if (iDist == Dasher::EditDistance::EDIT_CHAR) {
			return Buffer.substr(Cursor - 1, 2);
		}

		std::cerr << "tried to get more than just a char" << std::endl;
		return "";
	}
	std::cerr << "Cursor out of bounds" << std::endl;
	return "";
}


//TODO: Some day fix typo in function name
int DasherController::GetAllContextLenght()
{
	return static_cast<int>(Buffer.length());
}

void DasherController::Initialize()
{
	InitButtonMap();
	Realize(0);
}

void DasherController::Render(unsigned long iTime){
	NewFrame(iTime, true);
}

void DasherController::Message(const std::string &strText, bool bInterrupt){
	if(OnMessage) OnMessage(strText, !bInterrupt);
	if(bInterrupt && GetActiveInputMethod()) GetActiveInputMethod()->pause();
}	
	
void DasherController::onUnpause(unsigned long lTime){
	if(OnUnpause && !OnUnpause()){
		//there are more, not-yet displayed
		if(GetActiveInputMethod()) GetActiveInputMethod()->pause();
      	return;
	}
	CDasherInterfaceBase::onUnpause(lTime);
}

#pragma clang optimize off
void DasherController::MappedKeyDown(unsigned long iTime, const std::string& key) {
	for(auto& [virtualKey, deviceKey] : keyMappings){
		if(deviceKey == key) KeyDown(iTime, virtualKey);
	}
}

void DasherController::MappedKeyUp(unsigned long iTime, const std::string& key) {
	for(auto& [virtualKey, deviceKey] : keyMappings){
		if(deviceKey == key) KeyUp(iTime, virtualKey);
	}
}

void DasherController::AddKeyToButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping){
	keyMappings.insert({key, mapping});
	WriteButtonMap();
}

void DasherController::RemoveKeyFromButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping){
	keyMappings.erase({key, mapping});
	WriteButtonMap();
}

void DasherController::InitButtonMap() {
	if(ignoreChangesInButtonMap) return;

	const std::string& mappings = GetStringParameter(Dasher::Parameter::SP_BUTTON_MAPPINGS);
	
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
	ButtonMappingsChanged.Broadcast();
}

// write button map as "<deviceKey>:<virtualKey>;<deviceKey>:<virtualKey>;[...]", where both keys could potentially be doubled, but any mapping is unique
void DasherController::WriteButtonMap(){
	std::string output;
	for(auto& [virtualKey, deviceKey] : keyMappings){
		output += deviceKey + ":" + Dasher::Keys::VirtualKeyToString(virtualKey) + ";";
	}
	ignoreChangesInButtonMap = true;
	SetStringParameter(Dasher::Parameter::SP_BUTTON_MAPPINGS, output);
	ignoreChangesInButtonMap = false;
}

#pragma clang optimize on