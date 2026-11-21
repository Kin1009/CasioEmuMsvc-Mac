#pragma once
// CG100 Core Controls -- dedicated session tab for the CG100 / SH4 arch-core
// power + diagnostics buttons (Reboot, SERVICE/FACTORY OS diag entry).
//
// These controls used to live in the Calculator panel's controls strip, which
// mixed model power/diagnostics actions with the zoom/display widgets.  They now
// live in their own tool tab so the Calculator panel keeps ONLY zoom/display
// controls (Fit, Original size, zoom slider, dimensions).
//
// Header-only on purpose: the desktop target globs Gui/*.cpp without
// CONFIGURE_DEPENDS, so adding a new translation unit would need a CMake
// reconfigure.  The factory below is compiled into Ui.cpp, which includes this
// header and registers exactly one instance for a live CG100 session.

#include "Ui.hpp"            // ::m_emu, Emulator::ArchReboot/ArchEnterDiag, UIWindow
#include "imgui/imgui.h"

#include <chrono>
#include <string>

class CG100CoreControls : public UIWindow {
public:
	CG100CoreControls() : UIWindow("CG100 Controls") {
		inital_size = ImVec2(760, 420);
	}

	void RenderCore() override {
		if (!m_emu || !m_emu->HasArchitectureCore()) {
			ImGui::TextDisabled("No CG100 architecture-core session is active.");
			ImGui::TextWrapped("Open a CG100 session with the SH4 architecture plugin to enable the Reboot and OS diagnostics entry buttons.");
			return;
		}

		ImGui::TextWrapped("CG100 / SH4 core power and genuine OS diagnostics controls. Each action patches REAL calculator state and lets the OS enter diagnostic mode on its own next boot: no host-side keys are pressed and no diagnostic text is faked.");
		ImGui::Separator();
		ImGui::Spacing();

		// --- Genuine OS diag entry buttons (plugin/arch cores only) ----------
		// The plugin's state-configure+reboot export patches REAL calculator
		// state (mode 0: factory-blank flash model word 0x300 = FF FF FF FF;
		// mode 1: exam marker erased and the boot key-combo held IN THE CORE),
		// resets the CPU to the reset vector, and lets the OS itself enter diag
		// on the next boot. The plugin holds/releases the in-core combo itself.
		const bool svc_clicked = ImGui::Button("To diagnostics mode (SERVICE)");
		const bool svc_tip = ImGui::IsItemHovered();
		ImGui::SameLine();
		const bool fac_clicked = ImGui::Button("To diagnostics mode (FACTORY)");
		const bool fac_tip = ImGui::IsItemHovered();
		if (svc_clicked)
			StartDiag(0); // factory-blank flash 0x300 -> service menu
		if (fac_clicked)
			StartDiag(1); // boot key-combo gate -> DIAGNOSTIC MODE
		if (!diag_status_.empty()) {
			ImGui::Spacing();
			ImGui::TextUnformatted(diag_status_.c_str());
		}
		if (svc_tip)
			ImGui::SetTooltip("Patches the factory-blank flash model word (0x300 = FF FF FF FF) and RESETs. The OS's own boot gate enters the [BaseROM MAIN] SERVICE menu (1.PATTERN 5.SERVICE / 4.VERSION ...). No keys, no fake output.");
		if (fac_tip)
			ImGui::SetTooltip("Erases the exam marker, RESETs, and holds AC + x10^x + ON IN THE CORE. The OS's own boot gate enters '=DIAGNOSTIC MODE= / Factory Use Only!'. The combo is released once the dialog appears; no UI keypresses or fake output.");

		ImGui::Separator();
		ImGui::Spacing();

		// --- Physical RESET ---------------------------------------------------
		const bool reboot_clicked = ImGui::Button("Reboot");
		const bool reboot_tip = ImGui::IsItemHovered();
		if (reboot_clicked)
			m_emu->ArchReboot();
		if (reboot_tip)
			ImGui::SetTooltip("Physical RESET button: true CPU reset to the reset vector (0x80000000), then boot the OS fresh");
	}

private:
	void StartDiag(int mode) {
		if (!m_emu || !m_emu->HasArchitectureCore())
			return;
		// Re-entry guard: do not interrupt an in-progress diag boot.
		if (diag_cooldown_ && std::chrono::steady_clock::now() - diag_last_click_ <
		                          std::chrono::milliseconds(1500))
			return;
		diag_last_click_ = std::chrono::steady_clock::now();
		diag_cooldown_ = true;
		m_emu->ArchEnterDiag(mode);
		diag_status_ = mode == 0
			? "SERVICE diag: factory-blank 0x300, booting..."
			: "FACTORY diag: combo held in-core, booting (auto-release after boot)...";
	}

	bool diag_cooldown_ = false;
	std::chrono::steady_clock::time_point diag_last_click_{};
	std::string diag_status_;
};

inline UIWindow* CreateCG100CoreControls() {
	return new CG100CoreControls();
}
