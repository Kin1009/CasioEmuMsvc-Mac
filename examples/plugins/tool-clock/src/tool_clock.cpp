#include <PluginApi.h>

#include <ctime>
#include <cstdio>

class ClockWindow : public UIWindow {
public:
	ClockWindow() : UIWindow("Clock") {
		inital_size = ImVec2(320, 180);
	}

	void RenderCore() override {
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_r(&now, &local);
		char text[32]{};
		std::strftime(text, sizeof(text), "%H:%M:%S", &local);
		ImGui::TextUnformatted("Local time");
		ImGui::Separator();
		ImGui::Text("%s", text);
		ImGui::TextDisabled("Example native plugin.");
	}
};

extern "C" void fPluginLoad(PluginApi* api) {
	api->RegisterPlugin("com.casioemu.tool.clock", "Clock Tool", "1.0.0", "CasioEmuMsvc", "Example native tool");
	api->AddWindow(new ClockWindow());
}
