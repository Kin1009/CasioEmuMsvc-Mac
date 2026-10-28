#include <PluginApi.h>

#include <cstring>

class NotesWindow : public UIWindow {
public:
	NotesWindow() : UIWindow("Notes") {
		inital_size = ImVec2(520, 360);
	}

	void RenderCore() override {
		ImGui::TextDisabled("Scratch notes for this session. Not persisted by the example.");
		ImGui::Separator();
		ImGui::InputTextMultiline("##notes", buffer_, sizeof(buffer_), ImVec2(-1.0f, -1.0f));
	}

private:
	char buffer_[4096]{};
};

extern "C" void fPluginLoad(PluginApi* api) {
	api->RegisterPlugin("com.casioemu.tab.notes", "Session Notes", "1.0.0", "CasioEmuMsvc", "Example native tab");
	api->AddWindow(new NotesWindow());
}
