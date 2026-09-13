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

class VideoEncoder
{
public:
	static VideoEncoder& GetInstance();

	bool Initialize(uint32 width = 854, uint32 height = 480, uint32 fps = 60, uint32 bitrate = 6000000);
	void Shutdown();

	bool EncodeFrame(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat, uint64 ptsUs, bool forceKeyframe, std::vector<uint8>& outH264);

	void RequestKeyframe();
	bool IsInitialized() const { return m_isInitialized; }
	bool WasLastFrameKeyframe() const { return m_lastFrameWasKeyframe; }

private:
	VideoEncoder();
	~VideoEncoder();

	void ConvertRGBAToNV12(const uint8* pixels, uint32 srcWidth, uint32 srcHeight, uint32 pitch, StreamingPixelFormat pixelFormat, uint8* nv12Y, uint8* nv12UV);

	bool m_isInitialized{ false };
	uint32 m_width{ 854 };
	uint32 m_height{ 480 };
	uint32 m_fps{ 60 };
	uint32 m_bitrate{ 6000000 };

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
