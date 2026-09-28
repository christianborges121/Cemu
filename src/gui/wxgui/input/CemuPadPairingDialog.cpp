#include "wxgui/input/CemuPadPairingDialog.h"

#include <wx/sizer.h>
#include <wx/msgdlg.h>
#include <wx/checkbox.h>

#include "streaming/CemuPadBridge.h"
#include "streaming/DiscoveryServer.h"
#include "Cafe/HW/Latte/Renderer/VideoStreamServer.h"

CemuPadPairingDialog::CemuPadPairingDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, "Pair CemuPad Android GamePad",
		wxDefaultPosition, wxSize(1360, 880),
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	InitUI();
	// Make sure the isolated discovery responder owns UDP 26763, then probe.
	DiscoveryServer::GetInstance().Start(DiscoveryServer::kDefaultPort);
	VideoStreamServer::GetInstance().Start(26761);
	DiscoveryServer::GetInstance().BroadcastProbe();
	RefreshDeviceList();
	m_pollTimer.Bind(wxEVT_TIMER, &CemuPadPairingDialog::OnTimer, this);
	m_pollTimer.Start(1000);
}

CemuPadPairingDialog::~CemuPadPairingDialog()
{
	if (m_pollTimer.IsRunning())
		m_pollTimer.Stop();
}

void CemuPadPairingDialog::InitUI()
{
	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	auto* headerText = new wxStaticText(this, wxID_ANY, "Discovered CemuPad Devices on Local Wi-Fi:");
	rootSizer->Add(headerText, 0, wxALL, 10);

	m_deviceList = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	m_deviceList->AppendColumn("Device Name", wxLIST_FORMAT_LEFT, 520);
	m_deviceList->AppendColumn("IP Address", wxLIST_FORMAT_LEFT, 340);
	m_deviceList->AppendColumn("Status", wxLIST_FORMAT_LEFT, 300);
	rootSizer->Add(m_deviceList, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

	m_statusText = new wxStaticText(this, wxID_ANY, "Scanning for CemuPad devices on UDP 26763...");
	rootSizer->Add(m_statusText, 0, wxALL, 10);

	// Session PIN security
	auto& bridge = CemuPadBridge::GetInstance();
	m_pinCheckbox = new wxCheckBox(this, wxID_ANY, "Require PIN to connect");
	m_pinCheckbox->SetValue(bridge.IsPinRequired());
	m_pinCheckbox->Bind(wxEVT_CHECKBOX, &CemuPadPairingDialog::OnPinToggle, this);
	rootSizer->Add(m_pinCheckbox, 0, wxLEFT | wxRIGHT, 10);

	uint32_t pin = bridge.GetCurrentPin();
	if (pin == 0) pin = bridge.RegeneratePin();
	m_pinLabel = new wxStaticText(this, wxID_ANY,
		wxString::Format("Session PIN: %06u", pin));
	rootSizer->Add(m_pinLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
	m_pinLabel->Show(bridge.IsPinRequired());

	auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
	m_rescanButton = new wxButton(this, wxID_ANY, "Rescan");
	m_rescanButton->Bind(wxEVT_BUTTON, &CemuPadPairingDialog::OnRescanClicked, this);
	btnSizer->Add(m_rescanButton, 0, wxRIGHT, 8);

	btnSizer->AddStretchSpacer();

	m_pairButton = new wxButton(this, wxID_OK, "Pair & Connect");
	m_pairButton->Bind(wxEVT_BUTTON, &CemuPadPairingDialog::OnPairClicked, this);
	m_pairButton->Disable();
	btnSizer->Add(m_pairButton, 0, wxRIGHT, 8);

	auto* cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
	btnSizer->Add(cancelBtn, 0);

	rootSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);

	m_deviceList->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) {
		m_pairButton->Enable(m_deviceList->GetFirstSelected() != -1);
	});
	m_deviceList->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) {
		m_pairButton->Enable(m_deviceList->GetFirstSelected() != -1);
	});

	SetSizer(rootSizer);
}

void CemuPadPairingDialog::OnRescanClicked(wxCommandEvent&)
{
	DiscoveryServer::GetInstance().BroadcastProbe();
	RefreshDeviceList();
}

void CemuPadPairingDialog::OnTimer(wxTimerEvent&)
{
	RefreshDeviceList();
	// Re-probe while open: phones stop broadcasting once video streams, so a
	// one-shot probe would let entries expire (30s) and strand the dialog.
	if (++m_pollTicks % 5 == 0)
		DiscoveryServer::GetInstance().BroadcastProbe();

	auto& bridge = CemuPadBridge::GetInstance();
	if (bridge.HasRecentPairingSuccess())
	{
		bridge.ClearPairingSuccess();
		std::string pairedIp = bridge.GetLastPairedIp();
		std::string pairedName = "CemuPad";
		uint16_t dsuPort = 26760;
		for (const auto& dev : m_devices)
		{
			if (dev.ip == pairedIp || pairedIp.empty())
			{
				pairedName = dev.name;
				pairedIp = dev.ip;
				dsuPort = dev.dsuPort;
				break;
			}
		}

		bridge.AutoConfigureDSUController(pairedIp, dsuPort);
		bridge.StartStreaming(pairedIp);

		wxWindow* parent = GetParent();
		EndModal(wxID_OK);

		wxMessageBox(
			wxString::Format("CemuPad paired successfully with %s (%s)!\n\nMotion controls and video streaming are now connected.",
				wxString::FromUTF8(pairedName), wxString::FromUTF8(pairedIp)),
			"Pairing Successful",
			wxOK | wxICON_INFORMATION,
			parent);
	}
}

void CemuPadPairingDialog::RefreshDeviceList()
{
	auto devices = DiscoveryServer::GetInstance().GetDiscoveredDevices();

	// Preserve selection across refreshes.
	std::string selectedIp;
	const long prevSel = m_deviceList->GetFirstSelected();
	if (prevSel != -1 && prevSel < static_cast<long>(m_devices.size()))
		selectedIp = m_devices[static_cast<size_t>(prevSel)].ip;

	m_devices.clear();
	m_deviceList->DeleteAllItems();
	long selectRow = -1;
	long idx = 0;
	for (const auto& dev : devices)
	{
		DeviceEntry entry{dev.name, dev.ip, dev.dsuPort};
		m_devices.push_back(entry);
		const long item = m_deviceList->InsertItem(idx, wxString::FromUTF8(dev.name));
		m_deviceList->SetItem(item, 1, wxString::FromUTF8(dev.ip));
		m_deviceList->SetItem(item, 2, "Ready");
		if (!selectedIp.empty() && dev.ip == selectedIp)
			selectRow = item;
		idx++;
	}
	if (selectRow != -1)
		m_deviceList->Select(selectRow);

	if (m_devices.empty())
	{
		m_statusText->SetLabel("Scanning... Open CemuPad on your Android phone.");
	}
	else
	{
		m_statusText->SetLabel(wxString::Format(
			"Found %zu device(s). Select your phone and click Pair & Connect.",
			m_devices.size()));
	}
	m_pairButton->Enable(m_deviceList->GetFirstSelected() != -1);
}

void CemuPadPairingDialog::OnPairClicked(wxCommandEvent&)
{
	const long sel = m_deviceList->GetFirstSelected();
	if (sel == -1 || sel >= static_cast<long>(m_devices.size()))
		return;

	const DeviceEntry entry = m_devices[static_cast<size_t>(sel)];

	auto& bridge = CemuPadBridge::GetInstance();
	bridge.ClearPairingSuccess();
	if (bridge.IsPinRequired() && !bridge.IsClientAuthorized(entry.ip))
	{
		uint32_t pin = bridge.GetCurrentPin();
		if (pin == 0) pin = bridge.RegeneratePin();

		CemuPadPinWaitDialog waitDlg(this, entry.name, entry.ip, pin);
		if (waitDlg.ShowModal() != wxID_OK)
		{
			// User cancelled pairing
			return;
		}
	}

	if (!bridge.AutoConfigureDSUController(entry.ip, entry.dsuPort))
	{
		wxMessageBox("Failed to configure DSU controller. Ensure Cemu input settings are not locked.",
			"Error", wxOK | wxICON_ERROR, this);
		return;
	}

	bridge.StartStreaming(entry.ip);

	wxWindow* parent = GetParent();
	std::string devName = entry.name;
	std::string devIp = entry.ip;
	EndModal(wxID_OK);

	wxMessageBox(
		wxString::Format("CemuPad paired successfully with %s (%s)!\n\nMotion controls and video streaming are now connected.",
			wxString::FromUTF8(devName), wxString::FromUTF8(devIp)),
		"Pairing Successful",
		wxOK | wxICON_INFORMATION,
		parent);
}

CemuPadPinWaitDialog::CemuPadPinWaitDialog(wxWindow* parent, const std::string& deviceName, const std::string& ip, uint32_t pin)
	: wxDialog(parent, wxID_ANY, wxString::Format("Pairing CemuPad - %s", deviceName),
		wxDefaultPosition, wxSize(480, 260), wxDEFAULT_DIALOG_STYLE)
	, m_ip(ip)
	, m_pin(pin)
{
	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	auto* instrText = new wxStaticText(this, wxID_ANY,
		"Enter this 6-digit PIN on your Android device to complete pairing:",
		wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
	rootSizer->Add(instrText, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 15);

	auto* pinText = new wxStaticText(this, wxID_ANY,
		wxString::Format("%06u", pin),
		wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
	wxFont font = pinText->GetFont();
	font.SetPointSize(28);
	font.SetWeight(wxFONTWEIGHT_BOLD);
	pinText->SetFont(font);
	pinText->SetForegroundColour(wxColour(0, 120, 215));
	rootSizer->Add(pinText, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 10);

	m_statusLabel = new wxStaticText(this, wxID_ANY,
		"Waiting for Android device to enter PIN...",
		wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
	rootSizer->Add(m_statusLabel, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 8);

	rootSizer->AddStretchSpacer();

	auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
	btnSizer->AddStretchSpacer();
	auto* allowBtn = new wxButton(this, wxID_OK, "Allow Connection");
	allowBtn->SetDefault();
	allowBtn->Bind(wxEVT_BUTTON, &CemuPadPinWaitDialog::OnAllow, this);
	btnSizer->Add(allowBtn, 0, wxRIGHT, 10);
	auto* cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
	cancelBtn->Bind(wxEVT_BUTTON, &CemuPadPinWaitDialog::OnCancel, this);
	btnSizer->Add(cancelBtn, 0, wxRIGHT, 15);
	rootSizer->Add(btnSizer, 0, wxEXPAND | wxBOTTOM, 15);

	SetSizer(rootSizer);
	CenterOnParent();

	m_timer.Bind(wxEVT_TIMER, &CemuPadPinWaitDialog::OnTimer, this);
	m_timer.Start(200);
}

CemuPadPinWaitDialog::~CemuPadPinWaitDialog()
{
	m_timer.Stop();
}

void CemuPadPinWaitDialog::OnTimer(wxTimerEvent&)
{
	m_ticks++;
	auto& bridge = CemuPadBridge::GetInstance();
	if (bridge.IsClientAuthorized(m_ip) || bridge.IsClientAuthorized("") || bridge.HasRecentPairingSuccess())
	{
		m_timer.Stop();
		m_statusLabel->SetForegroundColour(wxColour(0, 160, 60));
		m_statusLabel->SetLabel("PIN verified! Pairing complete.");
		EndModal(wxID_OK);
		return;
	}

	static const char* dots[] = {
		"Waiting for Android device to enter PIN.",
		"Waiting for Android device to enter PIN..",
		"Waiting for Android device to enter PIN..."
	};
	m_statusLabel->SetLabel(dots[(m_ticks / 3) % 3]);

	if (m_ticks >= 450)
	{
		m_timer.Stop();
		m_statusLabel->SetForegroundColour(*wxRED);
		m_statusLabel->SetLabel("Pairing timed out. Please try again.");
	}
}

void CemuPadPinWaitDialog::OnAllow(wxCommandEvent&)
{
	m_timer.Stop();
	auto& bridge = CemuPadBridge::GetInstance();
	bridge.AuthorizeClient(m_ip);
	m_statusLabel->SetForegroundColour(wxColour(0, 160, 60));
	m_statusLabel->SetLabel("Connection allowed! Pairing complete.");
	EndModal(wxID_OK);
}

void CemuPadPinWaitDialog::OnCancel(wxCommandEvent&)
{
	m_timer.Stop();
	EndModal(wxID_CANCEL);
}

void CemuPadPairingDialog::OnPinToggle(wxCommandEvent&)
{
	bool enabled = m_pinCheckbox->GetValue();
	CemuPadBridge::GetInstance().SetRequirePin(enabled);
	UpdatePinDisplay();
}

void CemuPadPairingDialog::UpdatePinDisplay()
{
	auto& bridge = CemuPadBridge::GetInstance();
	bool pinRequired = bridge.IsPinRequired();
	if (pinRequired)
	{
		uint32_t pin = bridge.GetCurrentPin();
		if (pin == 0) pin = bridge.RegeneratePin();
		m_pinLabel->SetLabel(wxString::Format("Session PIN: %06u", pin));
	}
	m_pinLabel->Show(pinRequired);
	Layout();
}
