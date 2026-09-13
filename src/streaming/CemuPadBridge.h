#pragma once
// CemuPadBridge — thin, non-invasive delegate between Cemu core and the
// isolated CemuPad streaming subsystem (Cemu/src/streaming/).
//
// Design rules:
// - This header depends only on the C++ standard library so it can be
//   included from latency-sensitive core files (vpad, snd_core, Latte)
//   without pulling in input/GUI dependencies.
// - Every delegate is a no-op while the subsystem is inactive, giving zero
//   runtime overhead and zero behavior change for upstream developers.
// - Media delegates forward to handler callbacks registered by the streaming
//   servers. This keeps the link graph acyclic: CemuStreaming links
//   CemuInput/CemuComponents, while core libs only reference this header and
//   resolve symbols at the final CemuBin link.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class CemuPadBridge
{
public:
	static CemuPadBridge& GetInstance();

	void Initialize();
	void Shutdown();
	bool IsActive() const;

	// Non-invasive delegate hooks called by Cemu core. Safe to call from
	// emulation/audio threads; each is a no-op when IsActive() == false.
	void OnGamepadFrame(const uint8_t* rgbaPixels, uint32_t width, uint32_t height, uint64_t ptsUs);
	void OnAudioDMA(const void* pcmData, size_t byteSize);
	void OnVPADRumble(uint8_t channel, const uint8_t* pattern, uint8_t length);
	void OnVPADClearRumble(uint8_t channel);

	// Handler registration used by the streaming servers at startup.
	// Handlers run on the caller's thread (emu/audio thread); keep them short.
	using FrameHandler = std::function<void(const uint8_t* rgbaPixels, uint32_t width, uint32_t height, uint64_t ptsUs)>;
	using AudioHandler = std::function<void(const void* pcmData, size_t byteSize)>;
	using RumbleHandler = std::function<void(uint8_t channel, const uint8_t* pattern, uint8_t length)>;
	using RumbleClearHandler = std::function<void(uint8_t channel)>;

	void SetFrameHandler(FrameHandler handler);
	void SetAudioHandler(AudioHandler handler);
	void SetRumbleHandler(RumbleHandler handler);
	void SetRumbleClearHandler(RumbleClearHandler handler);

	// 1-Click programmatic DSU controller configuration.
	// Binds Controller 0 (VPAD) to the official DSUClient provider pointing
	// at <deviceIp>:<dsuPort>, applies the CemuPad default GamePad mapping
	// and persists controllerProfiles/controller0.xml. Returns true on success.
	bool AutoConfigureDSUController(const std::string& deviceIp, uint16_t dsuPort = 26760);

	// Stream session control. Records the active streaming target; the actual
	// video/audio servers keep their existing lifecycles until the subsystem
	// move (Step 3.1 follow-up) is completed and verified.
	bool StartStreaming(const std::string& clientIp);
	void StopStreaming();
	std::string GetStreamingTarget() const;

private:
	CemuPadBridge() = default;
	~CemuPadBridge() = default;
	CemuPadBridge(const CemuPadBridge&) = delete;
	CemuPadBridge& operator=(const CemuPadBridge&) = delete;

	std::atomic<bool> m_isActive{false};

	mutable std::mutex m_handlerMutex;
	FrameHandler m_frameHandler;
	AudioHandler m_audioHandler;
	RumbleHandler m_rumbleHandler;
	RumbleClearHandler m_rumbleClearHandler;

	mutable std::mutex m_targetMutex;
	std::string m_streamingTarget;
};
