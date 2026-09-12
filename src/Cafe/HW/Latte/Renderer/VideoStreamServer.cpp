#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Renderer/VideoStreamServer.h"
#include "Cafe/HW/Latte/Renderer/StreamingCapture.h"
#include "Cemu/Logging/CemuLogging.h"

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#endif

VideoStreamServer& VideoStreamServer::GetInstance()
{
	static VideoStreamServer s_instance;
	return s_instance;
}

VideoStreamServer::VideoStreamServer()
{
#if defined(_WIN32)
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

VideoStreamServer::~VideoStreamServer()
{
	Stop();
#if defined(_WIN32)
	WSACleanup();
#endif
}

bool VideoStreamServer::Start(uint16 port)
{
	if (m_isRunning)
		return true;

	m_port = port;
	m_isRunning = true;
	m_serverThread = std::thread(&VideoStreamServer::ServerThreadFunc, this);
	cemuLog_log(LogType::Force, "VideoStreamServer: Started on TCP port {}", port);
	return true;
}

void VideoStreamServer::Stop()
{
	if (!m_isRunning)
		return;

	m_isRunning = false;

	{
		std::lock_guard<std::mutex> lock(m_clientsMutex);
		for (auto sock : m_clientSockets)
		{
#if defined(_WIN32)
			closesocket((SOCKET)sock);
#else
			close((int)sock);
#endif
		}
		m_clientSockets.clear();
	}

	if (m_serverThread.joinable())
		m_serverThread.join();

	for (auto& t : m_rxThreads)
	{
		if (t.joinable())
			t.join();
	}
	m_rxThreads.clear();
	cemuLog_log(LogType::Force, "VideoStreamServer: Stopped");
}

bool VideoStreamServer::HasActiveClient() const
{
	std::lock_guard<std::mutex> lock(m_clientsMutex);
	return !m_clientSockets.empty();
}

void VideoStreamServer::BroadcastFrame(uint8 packetType, uint64 ptsUs, const uint8* data, size_t size)
{
	if (!data || size == 0)
		return;

	std::vector<uint8> packet;
	packet.reserve(13 + size);

	// 1 byte: packet type (0x01 = H264 NAL)
	packet.push_back(packetType);

	// 4 bytes: size (little-endian uint32)
	uint32 len = static_cast<uint32>(size);
	packet.push_back(static_cast<uint8>(len & 0xFF));
	packet.push_back(static_cast<uint8>((len >> 8) & 0xFF));
	packet.push_back(static_cast<uint8>((len >> 16) & 0xFF));
	packet.push_back(static_cast<uint8>((len >> 24) & 0xFF));

	// 8 bytes: ptsUs (little-endian uint64)
	for (int i = 0; i < 8; ++i)
		packet.push_back(static_cast<uint8>((ptsUs >> (i * 8)) & 0xFF));

	// Payload
	packet.insert(packet.end(), data, data + size);

	std::lock_guard<std::mutex> lock(m_clientsMutex);
	for (auto it = m_clientSockets.begin(); it != m_clientSockets.end();)
	{
		uintptr_t s = *it;
		int sent = send((SOCKET)s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0);
		if (sent <= 0)
		{
			cemuLog_log(LogType::Force, "VideoStreamServer: Client disconnected on send error");
#if defined(_WIN32)
			closesocket((SOCKET)s);
#else
			close((int)s);
#endif
			it = m_clientSockets.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void VideoStreamServer::ServerThreadFunc()
{
	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Failed to create listen socket");
		return;
	}

	int opt = 1;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(m_port);

	if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Bind failed on port {}", m_port);
		closesocket(listenSock);
		return;
	}

	if (listen(listenSock, 4) == SOCKET_ERROR)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Listen failed");
		closesocket(listenSock);
		return;
	}

	// Set 500ms accept timeout so we can exit cleanly
	DWORD timeout = 500;
	setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

	while (m_isRunning)
	{
		sockaddr_in clientAddr{};
		int addrLen = sizeof(clientAddr);
		SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);

		if (clientSock == INVALID_SOCKET)
			continue;

		// Set TCP_NODELAY for immediate packet dispatch (disable Nagle's algorithm)
		int noDelay = 1;
		setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

		// Set large send buffer for smooth 60fps streaming
		int sndBuf = 1024 * 512;
		setsockopt(clientSock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndBuf, sizeof(sndBuf));

		cemuLog_log(LogType::Force, "VideoStreamServer: Android client connected to video stream!");

		{
			std::lock_guard<std::mutex> lock(m_clientsMutex);
			m_clientSockets.push_back((uintptr_t)clientSock);
		}

		// Request an immediate keyframe so the client can begin decoding right away
		StreamingCapture::GetInstance().RequestKeyframe();

		// Spawn thread to listen for reverse control packets (e.g. IDR_REQUEST = 0x10)
		m_rxThreads.emplace_back(&VideoStreamServer::ClientRxThreadFunc, this, (uintptr_t)clientSock);
	}

	closesocket(listenSock);
}

void VideoStreamServer::ClientRxThreadFunc(uintptr_t clientSocket)
{
	SOCKET s = (SOCKET)clientSocket;
	uint8 opcode = 0;

	while (m_isRunning)
	{
		int bytesRead = recv(s, reinterpret_cast<char*>(&opcode), 1, 0);
		if (bytesRead <= 0)
			break;

		if (opcode == 0x10)
		{
			cemuLog_log(LogType::Force, "VideoStreamServer: Received IDR_REQUEST opcode (0x10) from Android client!");
			StreamingCapture::GetInstance().RequestKeyframe();
		}
	}
}
