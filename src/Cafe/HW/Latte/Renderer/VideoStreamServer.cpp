#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Renderer/VideoStreamServer.h"
#include "Cafe/HW/Latte/Renderer/StreamingCapture.h"
#include "Cemu/Logging/CemuLogging.h"

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/ioctl.h>
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

	m_udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_udpSock == INVALID_SOCKET)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Failed to create UDP socket");
	}
	else
	{
		// Non-blocking: a full kernel buffer must drop (and count) instead
		// of stalling the encode worker shared with TCP clients.
		int sndBuf = 1024 * 1024;
#if defined(_WIN32)
		u_long nonBlocking = 1;
		ioctlsocket(m_udpSock, FIONBIO, &nonBlocking);
		setsockopt(m_udpSock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndBuf, sizeof(sndBuf));
#else
		int nonBlocking = 1;
		ioctl(m_udpSock, FIONBIO, &nonBlocking);
		setsockopt(m_udpSock, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
#endif
		cemuLog_log(LogType::Force, "VideoStreamServer: UDP video ready on port {}", port);
	}

	m_serverThread = std::thread(&VideoStreamServer::ServerThreadFunc, this);
	// Note: UDP 26763 discovery is owned by streaming/DiscoveryServer
	// (Phase 4.0). The legacy discovery thread was retired to avoid double-bind.
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
		for (auto& client : m_clients)
		{
#if defined(_WIN32)
			closesocket((SOCKET)client.socket);
#else
			close((int)client.socket);
#endif
		}
		m_clients.clear();
	}

	if (m_udpSock != INVALID_SOCKET)
	{
#if defined(_WIN32)
		closesocket(m_udpSock);
#else
		close(m_udpSock);
#endif
		m_udpSock = INVALID_SOCKET;
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
	return !m_clients.empty();
}

void VideoStreamServer::SendUdpFrame(const sockaddr_in& destAddr, uint64 ptsUs, const uint8* data, size_t size, bool isKeyframe)
{
	if (m_udpSock == INVALID_SOCKET)
		return;

	const uint64 frameCount = m_udpFrames.fetch_add(1) + 1;
	const uint32 frameId = m_frameId.fetch_add(1);
	const uint32 packetCount = static_cast<uint32>((size + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD);
	uint8 header[UDP_HEADER_SIZE];
	header[0] = static_cast<uint8>(UDP_MAGIC & 0xFF);
	header[1] = static_cast<uint8>((UDP_MAGIC >> 8) & 0xFF);
	header[2] = UDP_VERSION;

	sockaddr_in dest = destAddr;
	dest.sin_port = htons(m_port);

	for (uint32 i = 0; i < packetCount; ++i)
	{
		const size_t offset = static_cast<size_t>(i) * UDP_MAX_PAYLOAD;
		const size_t chunk = std::min(UDP_MAX_PAYLOAD, size - offset);
		uint8 flags = 0;
		if (i == 0)
			flags |= UDP_FLAG_START;
		if (i + 1 == packetCount)
			flags |= UDP_FLAG_END;
		if (isKeyframe)
			flags |= UDP_FLAG_IDR;
		header[3] = flags;

		const uint32 seq = m_seq.fetch_add(1);
		header[4] = static_cast<uint8>(frameId & 0xFF);
		header[5] = static_cast<uint8>((frameId >> 8) & 0xFF);
		header[6] = static_cast<uint8>((frameId >> 16) & 0xFF);
		header[7] = static_cast<uint8>((frameId >> 24) & 0xFF);
		header[8] = static_cast<uint8>(seq & 0xFF);
		header[9] = static_cast<uint8>((seq >> 8) & 0xFF);
		header[10] = static_cast<uint8>((seq >> 16) & 0xFF);
		header[11] = static_cast<uint8>((seq >> 24) & 0xFF);
		header[12] = static_cast<uint8>(i & 0xFF);
		header[13] = static_cast<uint8>((i >> 8) & 0xFF);
		header[14] = static_cast<uint8>(packetCount & 0xFF);
		header[15] = static_cast<uint8>((packetCount >> 8) & 0xFF);
		for (int b = 0; b < 8; ++b)
			header[16 + b] = static_cast<uint8>((ptsUs >> (b * 8)) & 0xFF);

		// Single stack buffer per datagram: portable and heap-free.
		uint8 datagram[UDP_HEADER_SIZE + UDP_MAX_PAYLOAD];
		memcpy(datagram, header, UDP_HEADER_SIZE);
		memcpy(datagram + UDP_HEADER_SIZE, data + offset, chunk);
		int sent = sendto(m_udpSock, reinterpret_cast<const char*>(datagram), static_cast<int>(UDP_HEADER_SIZE + chunk), 0, (sockaddr*)&dest, sizeof(dest));
		if (sent <= 0)
			m_udpSendErrors.fetch_add(1);
	}
	m_udpPackets.fetch_add(packetCount);
	if ((frameCount % 600) == 0)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: UDP video {} frames, {} packets, {} send errors",
			m_udpFrames.load(), m_udpPackets.load(), m_udpSendErrors.load());
	}
}

void VideoStreamServer::BroadcastFrame(uint8 packetType, uint64 ptsUs, const uint8* data, size_t size, bool isKeyframe)
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

	// Snapshot the client list under the lock
	std::vector<ClientInfo> clientSnapshot;
	{
		std::lock_guard<std::mutex> lock(m_clientsMutex);
		clientSnapshot = m_clients;
	}

	// Send to each client WITHOUT holding the lock
	std::vector<uintptr_t> failedSockets;
	for (auto& client : clientSnapshot)
	{
		if (client.useUdp)
		{
			SendUdpFrame(client.addr, ptsUs, data, size, isKeyframe);
			continue;
		}

		uintptr_t s = client.socket;
		const char* buf = reinterpret_cast<const char*>(packet.data());
		int remaining = static_cast<int>(packet.size());
		bool sendFailed = false;

		while (remaining > 0)
		{
			int sent = send((SOCKET)s, buf, remaining, 0);
			if (sent <= 0)
			{
				sendFailed = true;
				break;
			}
			buf += sent;
			remaining -= sent;
		}

		if (sendFailed)
		{
			cemuLog_log(LogType::Force, "VideoStreamServer: Client disconnected on send error");
#if defined(_WIN32)
			closesocket((SOCKET)s);
#else
			close((int)s);
#endif
			failedSockets.push_back(s);
		}
	}

	// Remove failed clients under the lock
	if (!failedSockets.empty())
	{
		std::lock_guard<std::mutex> lock(m_clientsMutex);
		for (auto failedSocket : failedSockets)
		{
			for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
			{
				if (it->socket == failedSocket)
				{
					m_clients.erase(it);
					break;
				}
			}
		}
	}
}

void VideoStreamServer::BroadcastRumble(bool active, uint8 intensity, uint16 durationMs)
{
	uint8 payload[4];
	payload[0] = active ? 1 : 0;
	payload[1] = intensity;
	payload[2] = static_cast<uint8>(durationMs & 0xFF);
	payload[3] = static_cast<uint8>((durationMs >> 8) & 0xFF);

	std::vector<uint8> packet;
	packet.reserve(17);
	packet.push_back(PACKET_TYPE_RUMBLE);

	uint32 len = 4;
	packet.push_back(static_cast<uint8>(len & 0xFF));
	packet.push_back(static_cast<uint8>((len >> 8) & 0xFF));
	packet.push_back(static_cast<uint8>((len >> 16) & 0xFF));
	packet.push_back(static_cast<uint8>((len >> 24) & 0xFF));

	for (int i = 0; i < 8; ++i)
		packet.push_back(0);

	packet.insert(packet.end(), payload, payload + 4);

	std::vector<ClientInfo> clientSnapshot;
	{
		std::lock_guard<std::mutex> lock(m_clientsMutex);
		clientSnapshot = m_clients;
	}

	for (auto& client : clientSnapshot)
	{
		send((SOCKET)client.socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0);
	}

	cemuLog_log(LogType::Force, "VideoStreamServer: BroadcastRumble active={} intensity={} durationMs={} sent to {} clients",
		active, intensity, durationMs, clientSnapshot.size());
}

void VideoStreamServer::BroadcastAudio(const void* data, size_t size)
{
	if (!data || size == 0 || m_udpSock == INVALID_SOCKET)
		return;

	// Snapshot clients
	std::vector<sockaddr_in> targets;
	{
		std::lock_guard<std::mutex> lock(m_clientsMutex);
		if (m_clients.empty())
			return;
		for (const auto& c : m_clients)
		{
			sockaddr_in target = c.addr;
			target.sin_port = htons(AUDIO_PORT);
			targets.push_back(target);
		}
	}

	const uint8* byteData = reinterpret_cast<const uint8*>(data);
	const size_t chunkSize = 1152;
	const uint64 timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()
	).count();

	uint32 seq = m_audioSeq.fetch_add(1);

	for (size_t offset = 0; offset < size; offset += chunkSize)
	{
		size_t curChunk = (std::min)(chunkSize, size - offset);
		uint8 packet[16 + 1152];

		// Magic: 'A','P'
		packet[0] = 0x41;
		packet[1] = 0x50;
		// Version
		packet[2] = 1;
		// Flags: 0x01 = first, 0x02 = last
		uint8 flags = 0;
		if (offset == 0) flags |= 0x01;
		if (offset + curChunk >= size) flags |= 0x02;
		packet[3] = flags;

		// Sequence (LE uint32)
		packet[4] = static_cast<uint8>(seq & 0xFF);
		packet[5] = static_cast<uint8>((seq >> 8) & 0xFF);
		packet[6] = static_cast<uint8>((seq >> 16) & 0xFF);
		packet[7] = static_cast<uint8>((seq >> 24) & 0xFF);

		// Timestamp (LE uint64)
		for (int i = 0; i < 8; ++i)
			packet[8 + i] = static_cast<uint8>((timestamp >> (i * 8)) & 0xFF);

		// Payload
		memcpy(packet + 16, byteData + offset, curChunk);

		for (const auto& target : targets)
		{
			sendto(m_udpSock, reinterpret_cast<const char*>(packet), static_cast<int>(16 + curChunk), 0, (sockaddr*)&target, sizeof(target));
		}
	}
}

void VideoStreamServer::PruneFinishedRxThreads()
{
	for (auto it = m_rxThreads.begin(); it != m_rxThreads.end();)
	{
		if (it->joinable())
		{
#if defined(_WIN32)
			DWORD result = WaitForSingleObject(it->native_handle(), 0);
			if (result == WAIT_OBJECT_0)
			{
				it->join();
				it = m_rxThreads.erase(it);
				continue;
			}
#endif
		}
		++it;
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
		PruneFinishedRxThreads();

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

		// Accepted sockets inherit SO_RCVTIMEO from listenSock on Windows.
		// Reset receive timeout to 0 (infinite) so reverse control packets are not timed out.
#if defined(_WIN32)
		DWORD zeroTimeout = 0;
		setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&zeroTimeout, sizeof(zeroTimeout));
#else
		struct timeval zeroTimeout{};
		setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&zeroTimeout, sizeof(zeroTimeout));
#endif

		cemuLog_log(LogType::Force, "VideoStreamServer: Android client connected to video stream!");

		{
			std::lock_guard<std::mutex> lock(m_clientsMutex);
			ClientInfo info{};
			info.socket = (uintptr_t)clientSock;
			info.addr = clientAddr;
			info.useUdp = false;
			m_clients.push_back(info);
		}

		// Request an immediate keyframe so the client can begin decoding right away
		StreamingCapture::GetInstance().RequestKeyframe();

		// Spawn thread to listen for reverse control packets
		// (IDR_REQUEST = 0x10, TRANSPORT_UDP = 0x11, TRANSPORT_TCP = 0x12)
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
		{
#if defined(_WIN32)
			if (bytesRead < 0 && (WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK))
				continue;
#else
			if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
#endif
			break;
		}

		if (opcode == OPCODE_IDR_REQUEST)
		{
			cemuLog_log(LogType::Force, "VideoStreamServer: Received IDR_REQUEST opcode (0x10) from Android client!");
			StreamingCapture::GetInstance().RequestKeyframe();
		}
		else if (opcode == OPCODE_TRANSPORT_UDP || opcode == OPCODE_TRANSPORT_TCP)
		{
			const bool useUdp = (opcode == OPCODE_TRANSPORT_UDP);
			std::lock_guard<std::mutex> lock(m_clientsMutex);
			for (auto& client : m_clients)
			{
				if (client.socket == clientSocket)
				{
					client.useUdp = useUdp;
					cemuLog_log(LogType::Force, "VideoStreamServer: Client switched to {} transport", useUdp ? "UDP" : "TCP");
					break;
				}
			}
			StreamingCapture::GetInstance().RequestKeyframe();
		}
		else if (opcode == OPCODE_MIC_BLOW)
		{
			uint8 blowState = 0;
			int r = recv(s, reinterpret_cast<char*>(&blowState), 1, 0);
			if (r > 0)
			{
				m_micBlowActive.store(blowState != 0);
				cemuLog_log(LogType::Force, "VideoStreamServer: Mic blow state = {}", blowState != 0);
			}
		}
	}

	// Control connection dropped: remove the client so UDP-only peers that
	// never fail a TCP send cannot linger forever.
	{
		std::lock_guard<std::mutex> lock(m_clientsMutex);
		for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
		{
			if (it->socket == clientSocket)
			{
				m_clients.erase(it);
				cemuLog_log(LogType::Force, "VideoStreamServer: Client control connection closed");
				break;
			}
		}
	}
#if defined(_WIN32)
	closesocket(s);
#else
	close((int)s);
#endif
}

// Note: UDP discovery now lives in streaming/DiscoveryServer (Phase 4.0).
