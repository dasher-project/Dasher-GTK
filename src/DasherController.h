#pragma once
#include <DashIntfScreenMsgs.h>
#include "ScreenGameModule.h"
#include <functional>
#include <memory>
#include "FakeInput.h"

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

	void Initialize();
	void Render(unsigned long iTime);

	std::string* GetBufferRef() { return &Buffer; }

	std::function<void(const std::string strText, const bool timedMessage)> OnMessage;
	std::function<bool()> OnUnpause;
	Event<const std::string&> OnBufferChange;

	void Message(const std::string &strText, bool bInterrupt) override;
	
	///Flush any modal messages that have been displayed before resuming.
	void onUnpause(unsigned long lTime) override;

	void CreateModules() override;

	Dasher::CGameModule *CreateGameModule() override {
		return new Dasher::CScreenGameModule(m_pSettingsStore, this, GetView(), m_pDasherModel);
	};

private:
	//Cursor position in the output buffer
	unsigned int Cursor = 0;
	//Output Buffer
	std::string Buffer;
	//Accumulated deltaTime
	unsigned long Time;

	std::unique_ptr<FakeInput> testInput;
};
