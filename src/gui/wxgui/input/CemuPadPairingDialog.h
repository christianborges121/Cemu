#pragma once
// CemuPadPairingDialog — 1-click discovery & pairing UI for CemuPad phones.
// Lives in CemuWxGui (needs wxWidgets); talks to the isolated streaming
// subsystem only through CemuPadBridge / DiscoveryServer.
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/timer.h>

#include <cstdint>
#include <string>
#include <vector>

class CemuPadPairingDialog : public wxDialog
{
public:
	explicit CemuPadPairingDialog(wxWindow* parent);
	~CemuPadPairingDialog() override;

private:
	void InitUI();
	void RefreshDeviceList();
	void OnPairClicked(wxCommandEvent& event);
	void OnRescanClicked(wxCommandEvent& event);
	void OnTimer(wxTimerEvent& event);
	void OnPinToggle(wxCommandEvent& event);
	void UpdatePinDisplay();

	wxListView* m_deviceList{nullptr};
	wxButton* m_pairButton{nullptr};
	wxButton* m_rescanButton{nullptr};
	wxStaticText* m_statusText{nullptr};
	wxStaticText* m_pinLabel{nullptr};
	wxCheckBox* m_pinCheckbox{nullptr};
	wxTimer m_pollTimer;
	int m_pollTicks{0};

	struct DeviceEntry
	{
		std::string name;
		std::string ip;
		uint16_t dsuPort{26760};
	};
	std::vector<DeviceEntry> m_devices;
};

class CemuPadPinWaitDialog : public wxDialog
{
public:
	CemuPadPinWaitDialog(wxWindow* parent, const std::string& deviceName, const std::string& ip, uint32_t pin);
	~CemuPadPinWaitDialog() override;

private:
	void OnTimer(wxTimerEvent& event);
	void OnAllow(wxCommandEvent& event);
	void OnCancel(wxCommandEvent& event);

	std::string m_ip;
	uint32_t m_pin;
	wxTimer m_timer;
	wxStaticText* m_statusLabel{nullptr};
	int m_ticks{0};
};
