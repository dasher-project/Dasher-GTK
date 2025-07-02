#pragma once
#include <DashIntfScreenMsgs.h>
#include "DasherTypes.h"
#include "ScreenGameModule.h"
#include <functional>
#include <memory>
#include <unordered_set>
#include "FakeInput.h"

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

class DasherController : public Dasher::CDashIntfSettings
{
public:
	DasherController(std::shared_ptr<Dasher::CSettingsStore> pSettingsStore);

	void editOutput(const std::string& strText, Dasher::CDasherNode* pNode) override;
	void editDelete(const std::string& strText, Dasher::CDasherNode* pNode) override;
	unsigned ctrlMove(bool bForwards, Dasher::EditDistance dist) override;
	unsigned ctrlDelete(bool bForwards, Dasher::EditDistance dist) override;
	std::string GetContext(unsigned iStart, unsigned iLength) override;
	std::string GetAllContext() override;
	std::string GetTextAroundCursor(Dasher::EditDistance iDist) override;
	int GetAllContextLenght() override;

	virtual void CreateModules() override;

	void Initialize();
	void Render(unsigned long iTime);

	std::string* GetBufferRef() { return &Buffer; }

	std::function<void(const std::string strText, const bool timedMessage)> OnMessage;
	std::function<bool()> OnUnpause;
	Event<const std::string&> OnBufferChange;

	void Message(const std::string &strText, bool bInterrupt) override;
	
	///Flush any modal messages that have been displayed before resuming.
	void onUnpause(unsigned long lTime) override;

	Dasher::CGameModule *CreateGameModule() override {
		return new Dasher::CScreenGameModule(m_pSettingsStore, this, GetView(), m_pDasherModel);
	};

	void MappedKeyDown(unsigned long iTime, const std::string& key);
  	void MappedKeyUp(unsigned long iTime, const std::string& key);
	void InitButtonMap();
	void WriteButtonMap();
	void AddKeyToButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping);
	void RemoveKeyFromButtonMap(Dasher::Keys::VirtualKey key, const std::string& mapping);
	bool ignoreChangesInButtonMap = false;
	//Key Mappings from settings
	std::unordered_set<std::pair<Dasher::Keys::VirtualKey, std::string>,hash_pair> keyMappings;
	Event<> ButtonMappingsChanged;

private:
	//Cursor position in the output buffer
	unsigned int Cursor = 0;
	//Output Buffer
	std::string Buffer;
	//Accumulated deltaTime
	unsigned long Time;


	std::unique_ptr<FakeInput> testInput;
};
