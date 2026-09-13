#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Renderer/VideoEncoder.h"
#include "Cemu/Logging/CemuLogging.h"
#include <algorithm>

#if defined(_WIN32)
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

VideoEncoder& VideoEncoder::GetInstance()
{
	static VideoEncoder s_instance;
	return s_instance;
}

VideoEncoder::VideoEncoder()
{
#if defined(_WIN32)
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	MFStartup(MF_VERSION);
#endif
}

VideoEncoder::~VideoEncoder()
{
	Shutdown();
#if defined(_WIN32)
	MFShutdown();
	CoUninitialize();
#endif
}

#if defined(_WIN32)
IMFTransform* VideoEncoder::CreateBestEncoder()
{
	MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Video, MFVideoFormat_H264 };

	IMFActivate** ppActivate = nullptr;
	UINT32 count = 0;

	// Step 1: Try hardware MFTs first (NVENC, AMF, QuickSync)
	HRESULT hr = MFTEnumEx(
		MFT_CATEGORY_VIDEO_ENCODER,
		MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
		nullptr,        // input type: any
		&outputType,    // output type: H.264
		&ppActivate,
		&count
	);

	if (SUCCEEDED(hr) && count > 0)
	{
		for (UINT32 i = 0; i < count; ++i)
		{
			IMFTransform* pTransform = nullptr;
			hr = ppActivate[i]->ActivateObject(IID_PPV_ARGS(&pTransform));
			if (SUCCEEDED(hr) && pTransform)
			{
				LPWSTR friendlyName = nullptr;
				UINT32 nameLen = 0;
				ppActivate[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendlyName, &nameLen);
				if (friendlyName)
				{
					char nameBuf[256] = {};
					WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
					cemuLog_log(LogType::Force, "VideoEncoder: Using hardware encoder: {}", nameBuf);
					CoTaskMemFree(friendlyName);
				}
				else
				{
					cemuLog_log(LogType::Force, "VideoEncoder: Using hardware encoder (index {})", i);
				}

				for (UINT32 j = 0; j < count; ++j)
					ppActivate[j]->Release();
				CoTaskMemFree(ppActivate);

				return pTransform;
			}
		}
		for (UINT32 j = 0; j < count; ++j)
			ppActivate[j]->Release();
		CoTaskMemFree(ppActivate);
	}

	// Step 2: Try software MFTs as fallback
	hr = MFTEnumEx(
		MFT_CATEGORY_VIDEO_ENCODER,
		MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		nullptr,
		&outputType,
		&ppActivate,
		&count
	);

	if (SUCCEEDED(hr) && count > 0)
	{
		IMFTransform* pTransform = nullptr;
		hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pTransform));
		if (SUCCEEDED(hr) && pTransform)
		{
			cemuLog_log(LogType::Force, "VideoEncoder: Using software H.264 encoder (fallback)");
			for (UINT32 j = 0; j < count; ++j)
				ppActivate[j]->Release();
			CoTaskMemFree(ppActivate);
			return pTransform;
		}
		for (UINT32 j = 0; j < count; ++j)
			ppActivate[j]->Release();
		CoTaskMemFree(ppActivate);
	}

	// Step 3: Last resort — direct CLSID instantiation
	cemuLog_log(LogType::Force, "VideoEncoder: Falling back to CLSID_CMSH264EncoderMFT (software)");
	IMFTransform* pTransform = nullptr;
	hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
		IID_IMFTransform, (void**)&pTransform);
	if (SUCCEEDED(hr))
		return pTransform;

	return nullptr;
}
#endif

bool VideoEncoder::Initialize(uint32 width, uint32 height, uint32 fps, uint32 bitrate)
{
	std::lock_guard<std::mutex> lock(m_encoderMutex);

	if (m_isInitialized)
		Shutdown();

	// Ensure dimensions are even
	m_width = width & ~1;
	m_height = height & ~1;
	m_fps = fps;
	m_bitrate = bitrate;

	m_nv12Buffer.resize((m_width * m_height * 3) / 2);

#if defined(_WIN32)
	HRESULT hr = S_OK;

	m_pTransform = CreateBestEncoder();
	if (!m_pTransform)
	{
		cemuLog_log(LogType::Force, "VideoEncoder: Failed to create H.264 encoder MFT");
		return false;
	}

	// Unlock asynchronous MFTs for synchronous pipeline control
	IMFAttributes* pAttributes = nullptr;
	if (SUCCEEDED(m_pTransform->GetAttributes(&pAttributes)))
	{
		pAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
		pAttributes->Release();
	}

	// Query ICodecAPI for low latency control
	m_pTransform->QueryInterface(IID_PPV_ARGS(&m_pCodecAPI));
	if (m_pCodecAPI)
	{
		VARIANT var;
		VariantInit(&var);

		// Enable ultra low-latency mode (bypasses multi-frame buffering)
		var.vt = VT_BOOL;
		var.boolVal = VARIANT_TRUE;
		m_pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &var);

		// Enable real-time processing
		var.vt = VT_BOOL;
		var.boolVal = VARIANT_TRUE;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncCommonRealTime, &var);

		// Common rate control: CBR
		var.vt = VT_UI4;
		var.ulVal = eAVEncCommonRateControlMode_CBR;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

		// Bitrate
		var.vt = VT_UI4;
		var.ulVal = m_bitrate;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

		// Quality vs Speed: 60 (balanced high-motion quality with low latency)
		var.vt = VT_UI4;
		var.ulVal = 60;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);

		// Explicitly disable B-pictures for real-time low-latency forward-only streaming
		var.vt = VT_UI4;
		var.ulVal = 0;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

		// Enable CABAC entropy encoding for higher compression efficiency during motion
		var.vt = VT_BOOL;
		var.boolVal = VARIANT_TRUE;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncH264CABACEnable, &var);

		// GOP Size: keyframe every 1 second (60 frames at 60 FPS) for fast packet loss recovery
		var.vt = VT_UI4;
		var.ulVal = m_fps;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

		VariantClear(&var);
	}

	// 1. Configure Output Media Type (H.264)
	IMFMediaType* pOutputType = nullptr;
	MFCreateMediaType(&pOutputType);
	pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	pOutputType->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate);
	MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, m_width, m_height);
	MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, m_fps, 1);
	MFSetAttributeRatio(pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

	hr = m_pTransform->SetOutputType(m_outStreamId, pOutputType, 0);
	pOutputType->Release();

	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "VideoEncoder: SetOutputType failed (0x{:08x})", (uint32)hr);
		Shutdown();
		return false;
	}

	// 2. Configure Input Media Type (NV12)
	IMFMediaType* pInputType = nullptr;
	MFCreateMediaType(&pInputType);
	pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, m_width, m_height);
	MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, m_fps, 1);
	MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

	hr = m_pTransform->SetInputType(m_inStreamId, pInputType, 0);
	pInputType->Release();

	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "VideoEncoder: SetInputType failed (0x{:08x})", (uint32)hr);
		Shutdown();
		return false;
	}

	MFT_OUTPUT_STREAM_INFO streamInfo{};
	if (SUCCEEDED(m_pTransform->GetOutputStreamInfo(m_outStreamId, &streamInfo)) && streamInfo.cbSize > 0)
	{
		m_outBufferSize = streamInfo.cbSize;
	}
	else
	{
		m_outBufferSize = 1024 * 1024;
	}

	m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
#endif

	m_isInitialized = true;
	cemuLog_log(LogType::Force, "VideoEncoder: Initialized (854x480 @ 60 FPS)");
	return true;
}

void VideoEncoder::Shutdown()
{
#if defined(_WIN32)
	if (m_pTransform)
	{
		m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		m_pTransform->Release();
		m_pTransform = nullptr;
	}
	if (m_pCodecAPI)
	{
		m_pCodecAPI->Release();
		m_pCodecAPI = nullptr;
	}
#endif
	m_isInitialized = false;
}

void VideoEncoder::RequestKeyframe()
{
	m_forceKeyframeNext = true;
}

bool VideoEncoder::SetBitrate(uint32 bitrateBps)
{
	constexpr uint32 kMinBitrateBps = 500000;
	constexpr uint32 kMaxBitrateBps = 20000000;
	const uint32 clamped = std::min(std::max(bitrateBps, kMinBitrateBps), kMaxBitrateBps);

	std::lock_guard<std::mutex> lock(m_encoderMutex);
	m_bitrate = clamped;

#if defined(_WIN32)
	if (m_pTransform && m_pCodecAPI)
	{
		VARIANT var;
		VariantInit(&var);
		var.vt = VT_UI4;
		var.ulVal = clamped;
		if (SUCCEEDED(m_pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var)))
		{
			cemuLog_log(LogType::Force, "VideoEncoder: Live updated bitrate to {} bps", clamped);
			return true;
		}
		cemuLog_log(LogType::Force, "VideoEncoder: Live bitrate update rejected by MFT, stored {} bps", clamped);
		return false;
	}
#endif
	// Encoder not running: stored value applies at the next Initialize().
	return true;
}

bool VideoEncoder::SetResolution(uint16 width, uint16 height)
{
	// Allowlist matching the Android resolution presets. The capture scaler
	// and MFT output type negotiation are validated for these targets only.
	uint32 targetW = 0;
	uint32 targetH = 0;
	if (width == 854 && height == 480)
	{
		targetW = 854;
		targetH = 480;
	}
	else if (width == 1280 && height == 720)
	{
		targetW = 1280;
		targetH = 720;
	}
	else if (width == 1920 && height == 1080)
	{
		targetW = 1920;
		targetH = 1080;
	}
	else
	{
		cemuLog_log(LogType::Force, "VideoEncoder: Rejected unsupported resolution {}x{}", width, height);
		return false;
	}

	uint32 fps;
	uint32 bitrate;
	{
		std::lock_guard<std::mutex> lock(m_encoderMutex);
		if (m_width == targetW && m_height == targetH)
			return true;
		fps = m_fps;
		bitrate = m_bitrate;
	}

	cemuLog_log(LogType::Force, "VideoEncoder: Reconfiguring resolution to {}x{}", targetW, targetH);
	// Note: Initialize() takes m_encoderMutex internally, so it must be
	// called without holding the lock here (non-recursive mutex).
	return Initialize(targetW, targetH, fps, bitrate);
}

void VideoEncoder::ConvertRGBAToNV12(const uint8* pixels, uint32 srcWidth, uint32 srcHeight, uint32 pitch, StreamingPixelFormat pixelFormat, uint8* nv12Y, uint8* nv12UV)
{
	const bool noScale = (srcWidth == m_width && srcHeight == m_height);
	const bool isA2B10G10R10 = (pixelFormat == StreamingPixelFormat::A2B10G10R10);
	const bool sourceIsBgra = (pixelFormat == StreamingPixelFormat::Bgra8);
	const size_t rIdx = sourceIsBgra ? 2 : 0;
	const size_t bIdx = sourceIsBgra ? 0 : 2;

	// Fast path: 8-bit RGBA/BGRA without scaling (predominant GamePad streaming path)
	if (noScale && !isA2B10G10R10)
	{
		for (uint32 y = 0; y < m_height; ++y)
		{
			const uint8* row = pixels + (y * pitch);
			uint8* yPlaneRow = nv12Y + (y * m_width);
			uint8* uvPlaneRow = nv12UV + ((y / 2) * m_width);
			const bool calcUV = (y % 2 == 0);

			for (uint32 x = 0; x < m_width; ++x)
			{
				const uint8* px = row + (x * 4);
				uint32 r = px[rIdx];
				uint32 g = px[1];
				uint32 b = px[bIdx];

				// Y component (ITU-R BT.601 limited range: [16, 235])
				uint32 yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
				yPlaneRow[x] = static_cast<uint8>(std::clamp<uint32>(yVal, 16, 235));

				// Subsampled UV (2x2)
				if (calcUV && (x % 2 == 0))
				{
					sint32 uVal = ((-38 * (sint32)r - 74 * (sint32)g + 112 * (sint32)b + 128) >> 8) + 128;
					sint32 vVal = ((112 * (sint32)r - 94 * (sint32)g - 18 * (sint32)b + 128) >> 8) + 128;

					uvPlaneRow[x] = static_cast<uint8>(std::clamp<sint32>(uVal, 16, 240));
					uvPlaneRow[x + 1] = static_cast<uint8>(std::clamp<sint32>(vVal, 16, 240));
				}
			}
		}
		return;
	}

	// General path: arbitrary scaling or packed 10-bit formats
	for (uint32 y = 0; y < m_height; ++y)
	{
		uint32 srcY = noScale ? y : ((y * srcHeight) / m_height);
		const uint8* row = pixels + (srcY * pitch);
		uint8* yPlaneRow = nv12Y + (y * m_width);
		uint8* uvPlaneRow = nv12UV + ((y / 2) * m_width);
		const bool calcUV = (y % 2 == 0);

		for (uint32 x = 0; x < m_width; ++x)
		{
			uint32 srcX = noScale ? x : ((x * srcWidth) / m_width);
			uint32 r;
			uint32 g;
			uint32 b;
			if (isA2B10G10R10)
			{
				uint32 packed;
				memcpy(&packed, row + srcX * 4, sizeof(packed));
				const auto to8Bit = [](uint32 value) { return (value * 255 + 511) / 1023; };
				// VK_FORMAT_A2B10G10R10_UNORM_PACK32 stores R in bits 0-9,
				// G in bits 10-19, B in bits 20-29, and A in bits 30-31.
				r = to8Bit(packed & 0x3FF);
				g = to8Bit((packed >> 10) & 0x3FF);
				b = to8Bit((packed >> 20) & 0x3FF);
			}
			else
			{
				const uint8* px = row + (srcX * 4);
				r = px[rIdx];
				g = px[1];
				b = px[bIdx];
			}

			// Y component (ITU-R BT.601 limited range: [16, 235])
			uint32 yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			yPlaneRow[x] = static_cast<uint8>(std::clamp<uint32>(yVal, 16, 235));

			// Subsampled UV (2x2)
			if (calcUV && (x % 2 == 0))
			{
				sint32 uVal = ((-38 * (sint32)r - 74 * (sint32)g + 112 * (sint32)b + 128) >> 8) + 128;
				sint32 vVal = ((112 * (sint32)r - 94 * (sint32)g - 18 * (sint32)b + 128) >> 8) + 128;

				uvPlaneRow[x] = static_cast<uint8>(std::clamp<sint32>(uVal, 16, 240));
				uvPlaneRow[x + 1] = static_cast<uint8>(std::clamp<sint32>(vVal, 16, 240));
			}
		}
	}
}

bool VideoEncoder::EncodeFrame(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat, uint64 ptsUs, bool forceKeyframe, const FrameOutputCallback& onFrameOutput)
{
	std::lock_guard<std::mutex> lock(m_encoderMutex);

	if (!m_isInitialized || !pixels)
		return false;

#if defined(_WIN32)
	if (!m_pTransform)
		return false;

	// Helper lambda to drain one sample and pass to onFrameOutput
	auto drainOneSample = [this, &onFrameOutput, ptsUs](bool keyframeRequested) -> bool {
		MFT_OUTPUT_DATA_BUFFER outputDataBuffer{};
		DWORD status = 0;
		IMFMediaBuffer* pOutBuffer = nullptr;
		MFCreateMemoryBuffer(m_outBufferSize, &pOutBuffer);
		IMFSample* pOutSample = nullptr;
		MFCreateSample(&pOutSample);
		pOutSample->AddBuffer(pOutBuffer);
		outputDataBuffer.pSample = pOutSample;
		outputDataBuffer.dwStreamID = m_outStreamId;

		HRESULT hr = m_pTransform->ProcessOutput(0, 1, &outputDataBuffer, &status);
		bool gotOutput = false;
		if (SUCCEEDED(hr) && outputDataBuffer.pSample)
		{
			LONGLONG sampleTimeHns = 0;
			uint64 framePtsUs = ptsUs;
			if (SUCCEEDED(outputDataBuffer.pSample->GetSampleTime(&sampleTimeHns)) && sampleTimeHns > 0)
			{
				framePtsUs = static_cast<uint64>(sampleTimeHns) / 10;
			}

			UINT32 isCleanPoint = 0;
			outputDataBuffer.pSample->GetUINT32(MFSampleExtension_CleanPoint, &isCleanPoint);

			IMFMediaBuffer* pMediaBuffer = nullptr;
			outputDataBuffer.pSample->ConvertToContiguousBuffer(&pMediaBuffer);
			if (pMediaBuffer)
			{
				BYTE* pBytes = nullptr;
				DWORD curLen = 0;
				pMediaBuffer->Lock(&pBytes, nullptr, &curLen);
				if (pBytes && curLen > 0)
				{
					bool hasIdr = (isCleanPoint != 0);
					if (!hasIdr && keyframeRequested)
					{
						const size_t scanLimit = std::min((size_t)curLen, (size_t)128);
						for (size_t i = 0; i + 4 < scanLimit; ++i)
						{
							if (pBytes[i] == 0 && pBytes[i + 1] == 0)
							{
								size_t offset = 0;
								if (pBytes[i + 2] == 1)
									offset = i + 3;
								else if (pBytes[i + 2] == 0 && pBytes[i + 3] == 1)
									offset = i + 4;

								if (offset != 0 && offset < scanLimit)
								{
									uint8 nalType = pBytes[offset] & 0x1F;
									if (nalType == 5 || nalType == 7)
									{
										hasIdr = true;
										break;
									}
								}
							}
						}
					}

					m_lastFrameWasKeyframe = hasIdr;
					if (hasIdr)
						m_forceKeyframeNext = false;

					if (onFrameOutput)
					{
						onFrameOutput(pBytes, curLen, framePtsUs, hasIdr);
					}
					gotOutput = true;
				}
				pMediaBuffer->Unlock();
				pMediaBuffer->Release();
			}
		}

		if (outputDataBuffer.pEvents)
			outputDataBuffer.pEvents->Release();
		pOutBuffer->Release();
		pOutSample->Release();
		return gotOutput;
	};

	// Convert normalized RGBA/BGRA/packed 10-bit pixels to NV12
	uint8* yPlane = m_nv12Buffer.data();
	uint8* uvPlane = yPlane + (m_width * m_height);
	ConvertRGBAToNV12(pixels, width, height, pitch, pixelFormat, yPlane, uvPlane);

	// Force keyframe if requested
	const bool shouldForceKeyframe = (forceKeyframe || m_forceKeyframeNext);
	if (shouldForceKeyframe && m_pCodecAPI)
	{
		VARIANT var;
		VariantInit(&var);
		var.vt = VT_UI4;
		var.ulVal = 1;
		m_pCodecAPI->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
		VariantClear(&var);
	}

	bool emittedAny = false;

	// 1. Drain any pending output before pushing new input to guarantee unblocked input
	while (drainOneSample(shouldForceKeyframe))
	{
		emittedAny = true;
	}

	// 2. Create input Media Sample and push
	IMFMediaBuffer* pInputBuffer = nullptr;
	DWORD bufSize = static_cast<DWORD>(m_nv12Buffer.size());
	MFCreateMemoryBuffer(bufSize, &pInputBuffer);

	BYTE* pBufferData = nullptr;
	pInputBuffer->Lock(&pBufferData, nullptr, nullptr);
	memcpy(pBufferData, m_nv12Buffer.data(), bufSize);
	pInputBuffer->Unlock();
	pInputBuffer->SetCurrentLength(bufSize);

	IMFSample* pInputSample = nullptr;
	MFCreateSample(&pInputSample);
	pInputSample->AddBuffer(pInputBuffer);
	pInputSample->SetSampleTime(ptsUs * 10); // 100ns units
	pInputSample->SetSampleDuration(10000000 / m_fps);
	if (shouldForceKeyframe)
	{
		pInputSample->SetUINT32(MFSampleExtension_CleanPoint, 1);
	}

	HRESULT hr = m_pTransform->ProcessInput(m_inStreamId, pInputSample, 0);
	pInputBuffer->Release();
	pInputSample->Release();

	if (FAILED(hr))
	{
		static uint32 s_inputFailCount = 0;
		if (++s_inputFailCount % 60 == 0)
			cemuLog_log(LogType::Force, "VideoEncoder: ProcessInput failed (hr=0x{:08X}, count={})", (uint32)hr, s_inputFailCount);
		return emittedAny;
	}

	// 3. Drain output produced by this input
	while (drainOneSample(shouldForceKeyframe))
	{
		emittedAny = true;
	}

	return emittedAny;
#else
	return false;
#endif
}

bool VideoEncoder::EncodeFrame(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat, uint64 ptsUs, bool forceKeyframe, std::vector<uint8>& outH264)
{
	outH264.clear();
	return EncodeFrame(pixels, width, height, pitch, pixelFormat, ptsUs, forceKeyframe,
		[&outH264](const uint8* data, size_t size, uint64 pts, bool isKeyframe) {
			outH264.assign(data, data + size);
		});
}
