#include "wxgui/input/CemuPadPairingDialog.h"

#include <wx/sizer.h>
#include <wx/msgdlg.h>

#include "streaming/CemuPadBridge.h"
#include "streaming/DiscoveryServer.h"

CemuPadPairingDialog::CemuPadPairingDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, "Pair CemuPad Android GamePad",
		wxDefaultPosition, wxSize(480, 320),
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	InitUI();
	// Make sure the isolated discovery responder owns UDP 26763, then probe.
	DiscoveryServer::GetInstance().Start(DiscoveryServer::kDefaultPort);
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
	m_deviceList->AppendColumn("Device Name", wxLIST_FORMAT_LEFT, 200);
	m_deviceList->AppendColumn("IP Address", wxLIST_FORMAT_LEFT, 130);
	m_deviceList->AppendColumn("Status", wxLIST_FORMAT_LEFT, 100);
	rootSizer->Add(m_deviceList, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

	m_statusText = new wxStaticText(this, wxID_ANY, "Scanning for CemuPad devices on UDP 26763...");
	rootSizer->Add(m_statusText, 0, wxALL, 10);

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

	if (!CemuPadBridge::GetInstance().AutoConfigureDSUController(entry.ip, entry.dsuPort))
	{
		wxMessageBox("Failed to configure DSU controller. Ensure Cemu input settings are not locked.",
			"Error", wxOK | wxICON_ERROR, this);
		return;
	}

	CemuPadBridge::GetInstance().StartStreaming(entry.ip);
	EndModal(wxID_OK);
}
