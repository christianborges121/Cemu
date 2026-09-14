#pragma once

#include "Common/precompiled.h"
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include "Cafe/HW/Latte/Renderer/VideoPixelFormat.h"

#if defined(_WIN32)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#endif

enum class VideoCodec
{
	H264 = 0,
	HEVC = 1,
};

class VideoEncoder
{
public:
	static VideoEncoder& GetInstance();

	bool Initialize(uint32 width = 854, uint32 height = 480, uint32 fps = 60, uint32 bitrate = 6000000, VideoCodec codec = VideoCodec::H264);
	void Shutdown();

	using FrameOutputCallback = std::function<void(const uint8* data, size_t size, uint64 ptsUs, bool isKeyframe)>;

	bool EncodeFrame(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat, uint64 ptsUs, bool forceKeyframe, const FrameOutputCallback& onFrameOutput);
	bool EncodeFrame(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat, uint64 ptsUs, bool forceKeyframe, std::vector<uint8>& outH264);

	void RequestKeyframe();
	bool IsInitialized() const { return m_isInitialized; }
	bool WasLastFrameKeyframe() const { return m_lastFrameWasKeyframe; }

	// Runtime reconfiguration (Phase 4.2 & 7.3). Safe to call from any thread.
	// SetBitrate updates the live MFT target when streaming; the value is
	// always stored so a later Initialize() picks it up.
	bool SetBitrate(uint32 bitrateBps);
	// SetResolution reinitializes the encoder for an allowlisted target
	// (854x480, 1280x720, 1920x1080). Returns false for unsupported sizes.
	bool SetResolution(uint16 width, uint16 height);
	// SetCodec dynamically switches between H.264 and HEVC.
	bool SetCodec(VideoCodec codec);
	VideoCodec GetCodec() const { return m_codec; }

private:
	VideoEncoder();
	~VideoEncoder();

	void ConvertRGBAToNV12(const uint8* pixels, uint32 srcWidth, uint32 srcHeight, uint32 pitch, StreamingPixelFormat pixelFormat, uint8* nv12Y, uint8* nv12UV);
#if defined(_WIN32)
	struct MFTCandidate
	{
		IMFTransform* pTransform{ nullptr };
		std::string name;
		bool isHardware{ false };
	};
	std::vector<MFTCandidate> CreateEncoderCandidates(VideoCodec codec);
#endif

	bool m_isInitialized{ false };
	uint32 m_width{ 854 };
	uint32 m_height{ 480 };
	uint32 m_fps{ 60 };
	uint32 m_bitrate{ 6000000 };
	VideoCodec m_codec{ VideoCodec::H264 };

	std::mutex m_encoderMutex;
	bool m_forceKeyframeNext{ false };
	bool m_lastFrameWasKeyframe{ false };

	std::vector<uint8> m_nv12Buffer;

#if defined(_WIN32)
	IMFTransform* m_pTransform{ nullptr };
	ICodecAPI* m_pCodecAPI{ nullptr };
	DWORD m_inStreamId{ 0 };
	DWORD m_outStreamId{ 0 };
	DWORD m_outBufferSize{ 1024 * 1024 };
#endif
};
