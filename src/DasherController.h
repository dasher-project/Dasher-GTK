#pragma once
#include <DashIntfScreenMsgs.h>
#include <memory>

class DasherController : public Dasher::CDashIntfScreenMsgs
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

private:
	//Cursor position in the output buffer
	unsigned int Cursor = 0;
	//Output Buffer
	std::string Buffer;
	//Accumulated deltaTime
	unsigned long Time;
};
