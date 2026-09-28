#include "Common/precompiled.h"
#include "streaming/CemuPadBridge.h"
#include "streaming/DiscoveryServer.h"
#include "Cafe/HW/Latte/Renderer/VideoStreamServer.h"
#include "Cemu/Logging/CemuLogging.h"
#include "input/InputManager.h"
#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/DSU/DSUController.h"
#include "input/emulated/VPADController.h"
#include "config/ActiveSettings.h"
#include <fstream>
#include <boost/algorithm/string.hpp>

CemuPadBridge& CemuPadBridge::GetInstance()
{
	static CemuPadBridge s_instance;
	return s_instance;
}

void CemuPadBridge::Initialize()
{
	LoadPersistentTokens();
	DiscoveryServer::GetInstance().Start(DiscoveryServer::kDefaultPort);
	VideoStreamServer::GetInstance().Start(26761);
	m_isActive = true;
	if (m_requirePin.load() && m_currentPin.load() == 0)
		RegeneratePin();
	cemuLog_log(LogType::Force, "CemuPadBridge: Subsystem initialized (session PIN {})",
		m_requirePin.load() ? fmt::format("required (PIN {:06d})", m_currentPin.load()) : "not required");
}

void CemuPadBridge::Shutdown()
{
	m_isActive = false;
	{
		std::scoped_lock lock(m_handlerMutex);
		m_frameHandler = nullptr;
		m_audioHandler = nullptr;
		m_rumbleHandler = nullptr;
		m_rumbleClearHandler = nullptr;
	}
	{
		std::scoped_lock lock(m_targetMutex);
		m_streamingTarget.clear();
	}
	DiscoveryServer::GetInstance().Stop();
	VideoStreamServer::GetInstance().Stop();
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
		std::scoped_lock lock(m_handlerMutex);
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
		std::scoped_lock lock(m_handlerMutex);
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
		std::scoped_lock lock(m_handlerMutex);
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
		std::scoped_lock lock(m_handlerMutex);
		handler = m_rumbleClearHandler;
	}
	if (handler)
		handler(channel);
}

void CemuPadBridge::SetFrameHandler(FrameHandler handler)
{
	std::scoped_lock lock(m_handlerMutex);
	m_frameHandler = std::move(handler);
}

void CemuPadBridge::SetAudioHandler(AudioHandler handler)
{
	std::scoped_lock lock(m_handlerMutex);
	m_audioHandler = std::move(handler);
}

void CemuPadBridge::SetRumbleHandler(RumbleHandler handler)
{
	std::scoped_lock lock(m_handlerMutex);
	m_rumbleHandler = std::move(handler);
}

void CemuPadBridge::SetRumbleClearHandler(RumbleClearHandler handler)
{
	std::scoped_lock lock(m_handlerMutex);
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
		std::scoped_lock lock(m_targetMutex);
		m_streamingTarget = clientIp;
	}
	m_isActive = true;
	cemuLog_log(LogType::Force, "CemuPadBridge: Streaming session started for {}", clientIp);
	return true;
}

void CemuPadBridge::StopStreaming()
{
	{
		std::scoped_lock lock(m_targetMutex);
		m_streamingTarget.clear();
	}
	cemuLog_log(LogType::Force, "CemuPadBridge: Streaming session stopped");
}

std::string CemuPadBridge::GetStreamingTarget() const
{
	std::scoped_lock lock(m_targetMutex);
	return m_streamingTarget;
}

void CemuPadBridge::QueueMicSamples(const int16_t* samples, size_t sampleCount)
{
	if (!samples || sampleCount == 0)
		return;
	std::scoped_lock lock(m_micMutex);
	for (size_t i = 0; i < sampleCount; ++i)
	{
		if (m_micQueue.size() >= kMicQueueCapSamples)
			m_micQueue.pop_front();
		m_micQueue.push_back(samples[i]);
	}
}

size_t CemuPadBridge::DequeueMicSamples(int16_t* outSamples, size_t maxSamples)
{
	if (!outSamples || maxSamples == 0)
		return 0;
	std::scoped_lock lock(m_micMutex);
	size_t count = 0;
	while (count < maxSamples && !m_micQueue.empty())
	{
		outSamples[count++] = m_micQueue.front();
		m_micQueue.pop_front();
	}
	return count;
}

void CemuPadBridge::ClearMicQueue()
{
	std::scoped_lock lock(m_micMutex);
	m_micQueue.clear();
}

bool CemuPadBridge::IsPinRequired() const
{
	return m_requirePin.load();
}

void CemuPadBridge::SetRequirePin(bool required)
{
	if (required && m_currentPin.load() == 0)
		RegeneratePin();
	m_requirePin.store(required);
	cemuLog_log(LogType::Force, "CemuPadBridge: Session PIN {}",
		required ? fmt::format("required (PIN {:06d})", m_currentPin.load()) : "not required");
}

uint32_t CemuPadBridge::GetCurrentPin() const
{
	return m_currentPin.load();
}

uint32_t CemuPadBridge::RegeneratePin()
{
	std::uniform_int_distribution<uint32_t> dist(100000, 999999);
	const uint32_t pin = dist(m_tokenRng);
	m_currentPin.store(pin);
	SavePersistentTokens();
	return pin;
}

bool CemuPadBridge::Authenticate(uint64_t credential, uint64_t& outToken, const std::string& clientIp)
{
	// Rate-limit: after 3 consecutive failures, lock out for 30 seconds.
	if (m_authFailCount.load() >= 3)
	{
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastAuthFail).count();
		if (elapsed < 30)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Auth rate-limited ({} seconds remaining)", 30 - elapsed);
			outToken = 0;
			return false;
		}
		m_authFailCount.store(0); // cooldown expired, reset
	}

	outToken = 0;

	auto recordSuccess = [&]() {
		std::scoped_lock lock(m_authSuccessMutex);
		m_pairingSuccess.store(true);
		if (!clientIp.empty())
		{
			m_lastPairedIp = clientIp;
			m_authorizedIpSet.insert(clientIp);
		}
	};

	auto issueToken = [&]() -> uint64_t {
		uint64_t token;
		{
			std::scoped_lock lock(m_tokenMutex);
			do
			{
				token = m_tokenRng();
			} while (token == 0);
			m_sessionTokens.push_back(token);
			while (m_sessionTokens.size() > kMaxSessionTokens)
				m_sessionTokens.erase(m_sessionTokens.begin());
		}
		SavePersistentTokens();
		return token;
	};

	// Open session (PIN disabled): approve anything, including the initial
	// zero credential, and hand out a token so remembered devices stay
	// paired if PIN protection is enabled later.
	if (!m_requirePin.load())
	{
		m_authFailCount.store(0);
		recordSuccess();
		outToken = issueToken();
		return true;
	}

	// Check if this IP has already been approved via "Allow Connection" on PC
	if (!clientIp.empty())
	{
		bool isAuthorizedIp = false;
		{
			std::scoped_lock lock(m_authSuccessMutex);
			isAuthorizedIp = (m_authorizedIpSet.find(clientIp) != m_authorizedIpSet.end());
		}

		if (isAuthorizedIp)
		{
			m_authFailCount.store(0);
			recordSuccess();
			outToken = issueToken();
			return true;
		}
	}

	{
		std::scoped_lock lock(m_tokenMutex);
		for (uint64_t token : m_sessionTokens)
		{
			if (token != 0 && token == credential)
			{
				m_authFailCount.store(0);
				recordSuccess();
				outToken = token;
				return true;
			}
		}
	}

	// Fresh pairing while required: accept the current 6-digit PIN.
	if (credential != 0 && credential == m_currentPin.load())
	{
		m_authFailCount.store(0);
		recordSuccess();
		outToken = issueToken();
		return true;
	}
	if (credential != 0)
	{
		m_authFailCount.fetch_add(1);
		m_lastAuthFail = std::chrono::steady_clock::now();
	}
	else
	{
		TriggerPairingPrompt(clientIp);
	}
	return false;
}

void CemuPadBridge::SetPairingPromptHandler(PairingPromptHandler handler)
{
	std::scoped_lock lock(m_promptMutex);
	m_pairingPromptHandler = std::move(handler);
}

void CemuPadBridge::TriggerPairingPrompt(const std::string& clientIp)
{
	if (!m_requirePin.load() || clientIp.empty())
		return;

	if (IsClientAuthorized(clientIp))
		return;

	auto now = std::chrono::steady_clock::now();
	bool shouldPrompt = false;
	{
		std::scoped_lock lock(m_promptMutex);
		if (m_pairingPromptHandler)
		{
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastPromptTime).count();
			if (elapsed >= 3 || m_lastPromptIp != clientIp)
			{
				m_lastPromptIp = clientIp;
				m_lastPromptTime = now;
				shouldPrompt = true;
			}
		}
	}
	if (shouldPrompt)
	{
		PairingPromptHandler handler;
		{
			std::scoped_lock lock(m_promptMutex);
			handler = m_pairingPromptHandler;
		}
		if (handler)
		{
			try
			{
				handler(clientIp, GetCurrentPin());
			}
			catch (const std::exception& e)
			{
				cemuLog_log(LogType::Force, "CemuPadBridge: Exception in pairing prompt handler: {}", e.what());
			}
			catch (...)
			{
				cemuLog_log(LogType::Force, "CemuPadBridge: Unknown exception in pairing prompt handler");
			}
		}
	}
}

void CemuPadBridge::AuthorizeClient(const std::string& clientIp)
{
	{
		std::scoped_lock lock(m_authSuccessMutex);
		m_pairingSuccess.store(true);
		if (!clientIp.empty())
		{
			m_lastPairedIp = clientIp;
			m_authorizedIpSet.insert(clientIp);
		}
	}
	if (!clientIp.empty())
	{
		VideoStreamServer::GetInstance().AuthorizeClientIp(clientIp);
		AutoConfigureDSUController(clientIp, 26760);
		StartStreaming(clientIp);
	}
}

bool CemuPadBridge::IsClientAuthorized(const std::string& ipStr) const
{
	if (!m_requirePin.load())
		return true;
	{
		std::scoped_lock lock(m_authSuccessMutex);
		if (ipStr.empty())
		{
			if (!m_authorizedIpSet.empty() || m_pairingSuccess.load())
				return true;
		}
		else
		{
			if (m_authorizedIpSet.find(ipStr) != m_authorizedIpSet.end())
				return true;
		}
	}
	return VideoStreamServer::GetInstance().IsClientAuthorized(ipStr);
}

bool CemuPadBridge::HasRecentPairingSuccess() const
{
	return m_pairingSuccess.load();
}

void CemuPadBridge::ClearPairingSuccess()
{
	m_pairingSuccess.store(false);
}

std::string CemuPadBridge::GetLastPairedIp() const
{
	std::scoped_lock lock(m_authSuccessMutex);
	return m_lastPairedIp;
}

void CemuPadBridge::LoadPersistentTokens()
{
	std::error_code ec;
	auto filePath = ActiveSettings::GetConfigPath("cemupad_tokens.txt");
	if (!fs::exists(filePath, ec))
		return;

	std::ifstream file(filePath);
	if (!file.is_open())
		return;

	std::scoped_lock lock(m_tokenMutex);
	m_sessionTokens.clear();
	std::string line;
	while (std::getline(file, line))
	{
		boost::trim(line);
		if (line.empty() || line[0] == '#')
			continue;
		if (boost::starts_with(line, "pin="))
		{
			try
			{
				uint32_t pin = std::stoul(line.substr(4));
				if (pin >= 100000 && pin <= 999999)
					m_currentPin.store(pin);
			}
			catch (...) {}
		}
		else if (boost::starts_with(line, "token="))
		{
			try
			{
				uint64_t token = std::stoull(line.substr(6), nullptr, 16);
				if (token != 0)
					m_sessionTokens.push_back(token);
			}
			catch (...) {}
		}
		else
		{
			try
			{
				uint64_t token = std::stoull(line, nullptr, 16);
				if (token != 0)
					m_sessionTokens.push_back(token);
			}
			catch (...) {}
		}
	}
	cemuLog_log(LogType::Force, "CemuPadBridge: Loaded persistent PIN {:06d} and {} session token(s)",
		m_currentPin.load(), m_sessionTokens.size());
}

void CemuPadBridge::SavePersistentTokens()
{
	try
	{
		auto filePath = ActiveSettings::GetConfigPath("cemupad_tokens.txt");
		std::ofstream file(filePath, std::ios::trunc);
		if (!file.is_open())
			return;

		std::scoped_lock lock(m_tokenMutex);
		file << "# CemuPad paired session tokens and PIN (auto-generated)\n";
		file << fmt::format("pin={:06d}\n", m_currentPin.load());
		for (uint64_t token : m_sessionTokens)
		{
			file << fmt::format("token={:016x}\n", token);
		}
	}
	catch (...) {}
}

bool CemuPadBridge::ApplyPushedMappings(const std::vector<std::pair<uint64, uint64>>& entries, bool clearExisting)
{
	if (entries.empty() || entries.size() > kMaxPushedMappings)
	{
		cemuLog_log(LogType::Force, "CemuPadBridge: ApplyPushedMappings rejected: entry count {}", entries.size());
		return false;
	}
	auto& inputMgr = InputManager::instance();
	auto vpad = inputMgr.get_vpad_controller(0);
	if (!vpad)
	{
		cemuLog_log(LogType::Force, "CemuPadBridge: No VPAD on slot 0, creating emulated Wii U GamePad for pushed mappings");
		auto created = inputMgr.set_controller(0, EmulatedController::Type::VPAD);
		vpad = std::dynamic_pointer_cast<VPADController>(created);
		if (!vpad)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Failed to create VPAD for pushed mappings");
			return false;
		}
	}
	std::string ip = GetStreamingTarget();
	if (ip.empty()) ip = "127.0.0.1";

	// Look for existing DSUController on this slot
	std::shared_ptr<ControllerBase> targetController;
	for (const auto& ctrl : vpad->get_controllers())
	{
		if (ctrl && ctrl->api() == InputAPI::DSUClient)
		{
			targetController = ctrl;
			break;
		}
	}

	if (!targetController)
	{
		try
		{
			DSUProviderSettings settings(ip, 26760);
			targetController = std::make_shared<DSUController>(0, settings);
		}
		catch (const std::exception& e)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Failed to create DSU controller for pushed mappings: {}", e.what());
			return false;
		}
	}

	if (clearExisting)
	{
		// Force overwrite: drop stale controllers and isolate the DSU controller
		vpad->clear_controllers();
		vpad->add_controller(targetController);
		vpad->clear_mappings();
	}
	else
	{
		// Ensure target controller is attached
		auto controllers = vpad->get_controllers();
		if (std::find(controllers.begin(), controllers.end(), targetController) == controllers.end())
		{
			vpad->add_controller(targetController);
		}
	}

	uint64 maxMapping = vpad->get_highest_mapping_id();
	for (auto& [mappingId, buttonId] : entries)
	{
		if (mappingId > maxMapping)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Mapping id {} out of range (max {})", mappingId, maxMapping);
			return false;
		}
		if (buttonId >= kButtonMAX)
		{
			cemuLog_log(LogType::Force, "CemuPadBridge: Button id {} out of range (max {})", buttonId, kButtonMAX);
			return false;
		}
		vpad->set_mapping(mappingId, targetController, buttonId);
		cemuLog_log(LogType::Force, "CemuPadBridge: Mapped {} -> {}", mappingId, buttonId);
	}
	if (!inputMgr.save(0))
	{
		cemuLog_log(LogType::Force, "CemuPadBridge: Failed to save controller0.xml after pushed mappings");
		return false;
	}
	cemuLog_log(LogType::Force, "CemuPadBridge: Applied {} pushed mappings to controller0", entries.size());
	return true;
}
