#include "Common/precompiled.h"
#include "Cafe/HW/Latte/Renderer/VideoStreamServer.h"
#include "Cafe/HW/Latte/Renderer/StreamingCapture.h"
#include "Cafe/HW/Latte/Renderer/VideoEncoder.h"
#include "streaming/CemuPadBridge.h"
#include "Cemu/Logging/CemuLogging.h"

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#endif

namespace
{
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

inline void SetSocketNonBlocking(SOCKET s)
{
#if defined(_WIN32)
	u_long nonBlocking = 1;
	ioctlsocket(s, FIONBIO, &nonBlocking);
#else
	int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline void SetSocketRecvTimeout(SOCKET s, int timeoutMs)
{
#if defined(_WIN32)
	DWORD timeout = static_cast<DWORD>(timeoutMs);
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
	struct timeval timeout{};
	timeout.tv_sec = timeoutMs / 1000;
	timeout.tv_usec = (timeoutMs % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#endif
}
}

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
		SetSocketNonBlocking(m_udpSock);
		setsockopt(m_udpSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof(sndBuf));
		cemuLog_log(LogType::Force, "VideoStreamServer: UDP video ready on port {}", port);
	}

	m_serverThread = std::thread(&VideoStreamServer::ServerThreadFunc, this);
	m_micThread = std::thread(&VideoStreamServer::MicRxThreadFunc, this);
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
			CloseSocket(static_cast<SOCKET>(client.socket));
		}
		m_clients.clear();
	}

	if (m_udpSock != INVALID_SOCKET)
	{
		CloseSocket(m_udpSock);
		m_udpSock = INVALID_SOCKET;
	}

	if (m_serverThread.joinable())
		m_serverThread.join();

	if (m_micThread.joinable())
		m_micThread.join();

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
		int sent = sendto(m_udpSock, reinterpret_cast<const char*>(datagram), static_cast<int>(UDP_HEADER_SIZE + chunk), 0, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
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
		// Session security: never stream media to unauthenticated clients.
		if (!client.authorized)
			continue;
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
			int sent = send(static_cast<SOCKET>(s), buf, remaining, kSendFlags);
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
			CloseSocket(static_cast<SOCKET>(s));
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
		if (!client.authorized)
			continue;
		send(static_cast<SOCKET>(client.socket), reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), kSendFlags);
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
			if (!c.authorized)
				continue;
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
			sendto(m_udpSock, reinterpret_cast<const char*>(packet), static_cast<int>(16 + curChunk), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
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
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(m_port);

	if (bind(listenSock, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Bind failed on port {}", m_port);
		CloseSocket(listenSock);
		return;
	}

	if (listen(listenSock, 4) == SOCKET_ERROR)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Listen failed");
		CloseSocket(listenSock);
		return;
	}

	// Set 500ms accept timeout so we can exit cleanly
	SetSocketRecvTimeout(listenSock, 500);

	while (m_isRunning)
	{
		sockaddr_in clientAddr{};
#if defined(_WIN32)
		int addrLen = sizeof(clientAddr);
#else
		socklen_t addrLen = sizeof(clientAddr);
#endif
		SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);

		if (clientSock == INVALID_SOCKET)
			continue;

		// Set TCP_NODELAY for immediate packet dispatch (disable Nagle's algorithm)
		int noDelay = 1;
		setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

		// Set large send buffer for smooth 60fps streaming
		int sndBuf = 1024 * 512;
		setsockopt(clientSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof(sndBuf));

		// Reset receive timeout to 0 (infinite)
		SetSocketRecvTimeout(clientSock, 0);

		cemuLog_log(LogType::Force, "VideoStreamServer: Android client connected to video stream!");

		{
			std::lock_guard<std::mutex> lock(m_clientsMutex);
			ClientInfo info{};
			info.socket = (uintptr_t)clientSock;
			info.addr = clientAddr;
			info.useUdp = false;
			// PIN disabled (default): clients stream immediately. Otherwise
			// they stay muted until OPCODE_AUTH_REQUEST succeeds.
			info.authorized = !CemuPadBridge::GetInstance().IsPinRequired();
			m_clients.push_back(info);
		}

		// Request an immediate keyframe so the client can begin decoding right away
		StreamingCapture::GetInstance().RequestKeyframe();

		// Spawn detached thread to listen for reverse control packets
		std::thread(&VideoStreamServer::ClientRxThreadFunc, this, (uintptr_t)clientSock).detach();
	}

	CloseSocket(listenSock);
}

void VideoStreamServer::SetClientAuthorized(uintptr_t clientSocket, bool authorized)
{
	std::lock_guard<std::mutex> lock(m_clientsMutex);
	for (auto& client : m_clients)
	{
		if (client.socket == clientSocket)
		{
			client.authorized = authorized;
			break;
		}
	}
}

void VideoStreamServer::ClientRxThreadFunc(uintptr_t clientSocket)
{
	SOCKET s = (SOCKET)clientSocket;
	uint8 opcode = 0;

	auto readExact = [&](void* buffer, size_t size) -> bool {
		uint8* dst = static_cast<uint8*>(buffer);
		size_t received = 0;
		while (received < size)
		{
			int r = recv(s, reinterpret_cast<char*>(dst + received), static_cast<int>(size - received), 0);
			if (r <= 0)
				return false;
			received += static_cast<size_t>(r);
		}
		return true;
	};

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

		// Session security gate: unauthorized clients only progress via
		// OPCODE_AUTH_REQUEST. Other opcodes are consumed and ignored so the
		// TCP stream stays aligned (fail-closed when the entry is missing).
		bool authorized = false;
		{
			std::lock_guard<std::mutex> lock(m_clientsMutex);
			for (const auto& client : m_clients)
			{
				if (client.socket == clientSocket)
				{
					authorized = client.authorized;
					break;
				}
			}
		}
		if (!authorized && opcode != OPCODE_AUTH_REQUEST)
		{
			if (opcode == OPCODE_MIC_BLOW)
			{
				uint8 ignored = 0;
				recv(s, reinterpret_cast<char*>(&ignored), 1, 0);
			}
			else if (opcode == OPCODE_SET_BITRATE || opcode == OPCODE_SET_RESOLUTION)
			{
				uint8 ignored[4]{};
				if (!readExact(ignored, sizeof(ignored)))
					break;
			}
			continue;
		}

		if (opcode == OPCODE_IDR_REQUEST)
		{
			cemuLog_log(LogType::Force, "VideoStreamServer: Received IDR_REQUEST opcode (0x10) from Android client!");
			StreamingCapture::GetInstance().RequestKeyframe();
		}
		else if (opcode == OPCODE_AUTH_REQUEST)
		{
			// Authenticated at any time: lets phones rotate tokens proactively.
			uint8 payload[8]{};
			if (!readExact(payload, sizeof(payload)))
				break;
			uint64 credential = 0;
			for (int i = 0; i < 8; ++i)
				credential |= (static_cast<uint64>(payload[i]) << (i * 8));
			uint64 token = 0;
			const bool ok = CemuPadBridge::GetInstance().Authenticate(credential, token);
			uint8 response[9];
			response[0] = ok ? 0x00 : 0x01;
			for (int i = 0; i < 8; ++i)
				response[1 + i] = static_cast<uint8>((token >> (i * 8)) & 0xFF);
			if (send(s, reinterpret_cast<const char*>(response), sizeof(response), kSendFlags) != sizeof(response))
				break;
			if (ok)
			{
				cemuLog_log(LogType::Force, "VideoStreamServer: Client authenticated");
				SetClientAuthorized(clientSocket, true);
			}
			else
			{
				cemuLog_log(LogType::Force, "VideoStreamServer: Client auth failed, disconnecting");
				break;
			}
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
		else if (opcode == OPCODE_SET_BITRATE)
		{
			uint8 payload[4]{};
			if (readExact(payload, sizeof(payload)))
			{
				const uint32 bitrate =
					static_cast<uint32>(payload[0]) |
					(static_cast<uint32>(payload[1]) << 8) |
					(static_cast<uint32>(payload[2]) << 16) |
					(static_cast<uint32>(payload[3]) << 24);
				cemuLog_log(LogType::Force, "VideoStreamServer: Received SET_BITRATE = {} bps", bitrate);
				VideoEncoder::GetInstance().SetBitrate(bitrate);
			}
		}
		else if (opcode == OPCODE_SET_RESOLUTION)
		{
			uint8 payload[4]{};
			if (readExact(payload, sizeof(payload)))
			{
				const uint16 width = static_cast<uint16>(payload[0] | (payload[1] << 8));
				const uint16 height = static_cast<uint16>(payload[2] | (payload[3] << 8));
				cemuLog_log(LogType::Force, "VideoStreamServer: Received SET_RESOLUTION = {}x{}", width, height);
				if (VideoEncoder::GetInstance().SetResolution(width, height))
				{
					// New SPS/PPS: force a keyframe so the phone re-syncs immediately.
					VideoEncoder::GetInstance().RequestKeyframe();
				}
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
	CloseSocket(s);
}

// Note: UDP discovery now lives in streaming/DiscoveryServer (Phase 4.0).

// Voice microphone receiver (Phase 4.3): 32 kHz 16-bit mono PCM datagrams
// from the phone on UDP 26764. Packets carry an 8-byte little-endian header
// (uint32 sequence, uint32 sampleCount) followed by int16 LE samples.
// Decoded chunks are queued into CemuPadBridge; the Cafe audio thread
// consumes them in mic_updateOnAXFrame (single ringbuffer writer).
void VideoStreamServer::MicRxThreadFunc()
{
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Failed to create mic socket");
		return;
	}

	sockaddr_in bindAddr{};
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bindAddr.sin_port = htons(MIC_PORT);
	if (bind(sock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0)
	{
		cemuLog_log(LogType::Force, "VideoStreamServer: Mic bind failed on UDP port {}", MIC_PORT);
		CloseSocket(sock);
		return;
	}

	SetSocketRecvTimeout(sock, 500);

	cemuLog_log(LogType::Force, "VideoStreamServer: Voice mic listening on UDP port {}", MIC_PORT);

	char buffer[4096];
	while (m_isRunning)
	{
		sockaddr_in sender{};
#if defined(_WIN32)
		int senderLen = sizeof(sender);
		const int bytes = recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&sender), &senderLen);
#else
		socklen_t senderLen = sizeof(sender);
		const ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&sender), &senderLen);
		const int bytes = static_cast<int>(received);
#endif
		if (bytes <= 8)
			continue;

		uint32 sampleCount =
			static_cast<uint32>(static_cast<uint8>(buffer[4])) |
			(static_cast<uint32>(static_cast<uint8>(buffer[5])) << 8) |
			(static_cast<uint32>(static_cast<uint8>(buffer[6])) << 16) |
			(static_cast<uint32>(static_cast<uint8>(buffer[7])) << 24);
		if (sampleCount == 0 || sampleCount * sizeof(int16_t) > static_cast<size_t>(bytes - 8))
			continue;

		CemuPadBridge::GetInstance().QueueMicSamples(
			reinterpret_cast<const int16_t*>(buffer + 8), static_cast<size_t>(sampleCount));
	}

	CloseSocket(sock);
}
