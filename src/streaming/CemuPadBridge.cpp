#include "Common/precompiled.h"
#include "streaming/CemuPadBridge.h"
#include "streaming/DiscoveryServer.h"
#include "Cemu/Logging/CemuLogging.h"
#include "input/InputManager.h"
#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/DSU/DSUController.h"
#include "input/emulated/VPADController.h"

CemuPadBridge& CemuPadBridge::GetInstance()
{
	static CemuPadBridge s_instance;
	return s_instance;
}

void CemuPadBridge::Initialize()
{
	DiscoveryServer::GetInstance().Start(DiscoveryServer::kDefaultPort);
	m_isActive = true;
	cemuLog_log(LogType::Force, "CemuPadBridge: Subsystem initialized");
}

void CemuPadBridge::Shutdown()
{
	m_isActive = false;
	{
		std::lock_guard<std::mutex> lock(m_handlerMutex);
		m_frameHandler = nullptr;
		m_audioHandler = nullptr;
		m_rumbleHandler = nullptr;
		m_rumbleClearHandler = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(m_targetMutex);
		m_streamingTarget.clear();
	}
	DiscoveryServer::GetInstance().Stop();
	cemuLog_log(LogType::Force, "CemuPadBridge: Subsystem stopped");
}

bool CemuPadBridge::IsActive() const
{
	return m_isActive.load();
}

void CemuPadBridge::OnGamepadFrame(const uint8_t* rgbaPixels, uint32_t width, uint32_t height, uint64_t ptsUs)
{
	if (!m_isActive.load() || !rgbaPixels || width == 0 || height == 0)
		return;
	FrameHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_handlerMutex);
		handler = m_frameHandler;
	}
	if (handler)
		handler(rgbaPixels, width, height, ptsUs);
}

void CemuPadBridge::OnAudioDMA(const void* pcmData, size_t byteSize)
{
	if (!m_isActive.load() || !pcmData || byteSize == 0)
		return;
	AudioHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_handlerMutex);
		handler = m_audioHandler;
	}
	if (handler)
		handler(pcmData, byteSize);
}

void CemuPadBridge::OnVPADRumble(uint8_t channel, const uint8_t* pattern, uint8_t length)
{
	if (!m_isActive.load() || !pattern || length == 0)
		return;
	RumbleHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_handlerMutex);
		handler = m_rumbleHandler;
	}
	if (handler)
		handler(channel, pattern, length);
}

void CemuPadBridge::OnVPADClearRumble(uint8_t channel)
{
	if (!m_isActive.load())
		return;
	RumbleClearHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_handlerMutex);
		handler = m_rumbleClearHandler;
	}
	if (handler)
		handler(channel);
}

void CemuPadBridge::SetFrameHandler(FrameHandler handler)
{
	std::lock_guard<std::mutex> lock(m_handlerMutex);
	m_frameHandler = std::move(handler);
}

void CemuPadBridge::SetAudioHandler(AudioHandler handler)
{
	std::lock_guard<std::mutex> lock(m_handlerMutex);
	m_audioHandler = std::move(handler);
}

void CemuPadBridge::SetRumbleHandler(RumbleHandler handler)
{
	std::lock_guard<std::mutex> lock(m_handlerMutex);
	m_rumbleHandler = std::move(handler);
}

void CemuPadBridge::SetRumbleClearHandler(RumbleClearHandler handler)
{
	std::lock_guard<std::mutex> lock(m_handlerMutex);
	m_rumbleClearHandler = std::move(handler);
}

// Default CemuPad DSU gamepad mapping (DSU button ids -> VPAD mapping ids).
// Mirrors the XInput default layout so the phone behaves like a standard
// Wii U GamePad: the Android app reports face buttons as Triangle (Y),
// Circle (B), Cross (A) and Square (X).
static void ApplyCemuPadDSUDefaultMapping(const std::shared_ptr<VPADController>& vpad, const std::shared_ptr<ControllerBase>& controller)
{
	using Mapping = std::pair<uint64, uint64>;
	static const Mapping kMapping[] = {
		{VPADController::kButtonId_A, kButton14}, // Cross
		{VPADController::kButtonId_B, kButton13}, // Circle
		{VPADController::kButtonId_X, kButton15}, // Square
		{VPADController::kButtonId_Y, kButton12}, // Triangle
		{VPADController::kButtonId_L, kButton10},
		{VPADController::kButtonId_R, kButton11},
		{VPADController::kButtonId_ZL, kTriggerXP}, // analog L2
		{VPADController::kButtonId_ZR, kTriggerYP}, // analog R2
		{VPADController::kButtonId_Plus, kButton3},  // Options
		{VPADController::kButtonId_Minus, kButton0}, // Share
		{VPADController::kButtonId_Up, kButton4},
		{VPADController::kButtonId_Down, kButton6},
		{VPADController::kButtonId_Left, kButton7},
		{VPADController::kButtonId_Right, kButton5},
		{VPADController::kButtonId_StickL, kButton1},
		{VPADController::kButtonId_StickR, kButton2},
		{VPADController::kButtonId_StickL_Up, kAxisYP},
		{VPADController::kButtonId_StickL_Down, kAxisYN},
		{VPADController::kButtonId_StickL_Left, kAxisXN},
		{VPADController::kButtonId_StickL_Right, kAxisXP},
		{VPADController::kButtonId_StickR_Up, kRotationYP},
		{VPADController::kButtonId_StickR_Down, kRotationYN},
		{VPADController::kButtonId_StickR_Left, kRotationXN},
		{VPADController::kButtonId_StickR_Right, kRotationXP},
		{VPADController::kButtonId_Mic, kButton16}, // touch / blow button
	};
	for (const auto& [mappingId, buttonId] : kMapping)
	{
		// Only fill empty slots so existing user customizations are preserved
		if (vpad->get_mapping_controller(mappingId) == nullptr)
			vpad->set_mapping(mappingId, controller, buttonId);
	}
}

bool CemuPadBridge::AutoConfigureDSUController(const std::string& deviceIp, uint16_t dsuPort)
{
	if (deviceIp.empty() || dsuPort == 0)
	{
		cemuLog_log(LogType::Force, "CemuPadBridge: Refusing to configure DSU controller with empty IP/port");
		return false;
	}

	auto& inputMgr = InputManager::instance();
	auto vpad = inputMgr.get_vpad_controller(0);
	if (!vpad)
	{
		// No emulated GamePad on slot 0 yet; create one.
		cemuLog_log(LogType::Force, "CemuPadBridge: No VPAD on slot 0, creating emulated Wii U GamePad");
		auto created = inputMgr.set_controller(0, EmulatedController::Type::VPAD);
		vpad = std::dynamic_pointer_cast<VPADController>(created);
		if (!vpad)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Failed to create VPAD controller 0");
			return false;
		}
	}

	try
	{
		// The DSUController constructor binds (or reuses) the official
		// DSUControllerProvider for these settings, pointing Cemu at the phone.
		DSUProviderSettings settings(deviceIp, dsuPort);
		auto controller = std::make_shared<DSUController>(0, settings);
		if (!controller)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Failed to create DSU controller for {}:{}", deviceIp, dsuPort);
			return false;
		}

		vpad->clear_controllers();
		vpad->add_controller(controller);

		// set_default_mapping() has no DSU branch upstream, so apply the
		// explicit CemuPad mapping. Call it first for future compatibility.
		vpad->set_default_mapping(controller);
		ApplyCemuPadDSUDefaultMapping(vpad, controller);

		if (!inputMgr.save(0))
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: DSU controller attached but controller0.xml could not be saved");
			return false;
		}

		cemuLog_log(LogType::Force, "CemuPadBridge: Controller 0 configured as Wii U GamePad with DSU {}:{}", deviceIp, dsuPort);
		return true;
	}
	catch (const std::exception& e)
	{
		cemuLog_log(LogType::Force, "CemuPadBridge: Exception configuring DSU controller: {}", e.what());
		return false;
	}
}

bool CemuPadBridge::StartStreaming(const std::string& clientIp)
{
	if (clientIp.empty())
		return false;
	{
		std::lock_guard<std::mutex> lock(m_targetMutex);
		m_streamingTarget = clientIp;
	}
	m_isActive = true;
	cemuLog_log(LogType::Force, "CemuPadBridge: Streaming session started for {}", clientIp);
	return true;
}

void CemuPadBridge::StopStreaming()
{
	{
		std::lock_guard<std::mutex> lock(m_targetMutex);
		m_streamingTarget.clear();
	}
	cemuLog_log(LogType::Force, "CemuPadBridge: Streaming session stopped");
}

std::string CemuPadBridge::GetStreamingTarget() const
{
	std::lock_guard<std::mutex> lock(m_targetMutex);
	return m_streamingTarget;
}
