#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Renderer/StreamingCapture.h"
#include "Cafe/HW/Latte/Renderer/VideoEncoder.h"
#include "Cafe/HW/Latte/Renderer/VideoStreamServer.h"
#include "streaming/CemuPadBridge.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTextureView.h"
#include "Cemu/Logging/CemuLogging.h"

StreamingCapture& StreamingCapture::GetInstance()
{
	static StreamingCapture s_instance;
	return s_instance;
}

StreamingCapture::StreamingCapture()
{
}

StreamingCapture::~StreamingCapture()
{
	Shutdown();
}

void StreamingCapture::Initialize()
{
	if (m_isInitialized)
		return;

	m_startTime = std::chrono::steady_clock::now();
	VideoStreamServer::GetInstance().Start(26761);
	VideoEncoder::GetInstance().Initialize(854, 480, 60, 6000000);
	m_isInitialized = true;
	m_workerStopping = false;
	m_workerThread = std::thread(&StreamingCapture::EncodeWorker, this);
	cemuLog_log(LogType::Force, "StreamingCapture: Initialized and listening for GamePad streaming connections");
}

void StreamingCapture::Shutdown()
{
	if (!m_isInitialized)
		return;

	m_isInitialized = false;
	m_workerStopping = true;
	m_queueCondition.notify_all();
	if (m_workerThread.joinable())
		m_workerThread.join();

	VideoStreamServer::GetInstance().Stop();
	VideoEncoder::GetInstance().Shutdown();
	cemuLog_log(LogType::Force, "StreamingCapture: Shutdown complete");
}

bool StreamingCapture::IsStreamingActive() const
{
	if (!m_isInitialized)
		return false;

	return VideoStreamServer::GetInstance().HasActiveClient();
}

void StreamingCapture::RequestKeyframe()
{
	VideoEncoder::GetInstance().RequestKeyframe();
}

void StreamingCapture::OnNewDRCFrame(LatteTextureView* texView)
{
	if (!IsStreamingActive() || !texView)
		return;

	// Note: Platform-specific renderers (VulkanRenderer/OpenGLRenderer) hook here to transfer
	// texture memory into host-visible staging buffers, then invoke ProcessFramePixels.
}

void StreamingCapture::ProcessFramePixels(const uint8* pixels, uint32 width, uint32 height, uint32 pitch, StreamingPixelFormat pixelFormat)
{
	if (!IsStreamingActive() || !pixels || width == 0 || height == 0)
		return;

	auto now = std::chrono::steady_clock::now();
	const uint64 ptsUs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(now - m_startTime).count());
	// Non-invasive delegate: no-op until a frame handler is registered (Phase 4.0 follow-up).
	CemuPadBridge::GetInstance().OnGamepadFrame(pixels, width, height, ptsUs);
	CapturedFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = pitch;
	frame.ptsUs = ptsUs;
	frame.pixelFormat = pixelFormat;
	frame.pixels.resize(static_cast<size_t>(pitch) * height);
	for (uint32 y = 0; y < height; ++y)
		memcpy(frame.pixels.data() + static_cast<size_t>(y) * pitch, pixels + static_cast<size_t>(y) * pitch, pitch);

	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		constexpr size_t kMaxQueuedFrames = 2;
		if (m_frameQueue.size() >= kMaxQueuedFrames)
			m_frameQueue.pop_front();
		m_frameQueue.push_back(std::move(frame));
	}
	static uint64 s_capturedCount = 0;
	if (++s_capturedCount % 600 == 0)
	{
		cemuLog_log(LogType::Force, "StreamingCapture: Captured {} DRC frames", s_capturedCount);
	}
	m_queueCondition.notify_one();
}

void StreamingCapture::EncodeWorker()
{
	while (true)
	{
		CapturedFrame frame;
		{
			std::unique_lock<std::mutex> lock(m_queueMutex);
			m_queueCondition.wait(lock, [this] { return m_workerStopping || !m_frameQueue.empty(); });
			if (m_workerStopping && m_frameQueue.empty())
				return;
			frame = std::move(m_frameQueue.front());
			m_frameQueue.pop_front();
		}

		if (!IsStreamingActive())
			continue;

		bool encodedAny = VideoEncoder::GetInstance().EncodeFrame(
			frame.pixels.data(), frame.width, frame.height, frame.pitch, frame.pixelFormat, frame.ptsUs, false,
			[](const uint8* data, size_t size, uint64 ptsUs, bool isKeyframe) {
				VideoStreamServer::GetInstance().BroadcastFrame(0x01, ptsUs, data, size, isKeyframe);
			}
		);
		if (!encodedAny)
		{
			static uint32 s_encodeFailures = 0;
			if (++s_encodeFailures % 60 == 0)
			{
				cemuLog_log(LogType::Force, "StreamingCapture: EncodeFrame produced no output (total={})", s_encodeFailures);
			}
		}
	}
}
