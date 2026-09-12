#pragma once

#include "Common/precompiled.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "Cafe/HW/Latte/Renderer/VideoPixelFormat.h"

class LatteTextureView;

class StreamingCapture
{
public:
	static StreamingCapture& GetInstance();

	void Initialize();
	void Shutdown();

	bool IsStreamingActive() const;
	void RequestKeyframe();

	void OnNewDRCFrame(LatteTextureView* texView);
	void ProcessFramePixels(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat);

private:
	StreamingCapture();
	~StreamingCapture();
	void EncodeWorker();

	struct CapturedFrame
	{
		std::vector<uint8> pixels;
		uint32 width = 0;
		uint32 height = 0;
		uint32 pitch = 0;
		uint64 ptsUs = 0;
		StreamingPixelFormat pixelFormat = StreamingPixelFormat::Rgba8;
	};

	std::atomic<bool> m_isInitialized{ false };
	std::atomic<bool> m_workerStopping{ false };
	std::chrono::steady_clock::time_point m_startTime;

	std::mutex m_queueMutex;
	std::condition_variable m_queueCondition;
	std::deque<CapturedFrame> m_frameQueue;
	std::thread m_workerThread;
	std::vector<uint8> m_h264Buffer;
};
