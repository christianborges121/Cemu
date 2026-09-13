#pragma once

#include "Common/precompiled.h"
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
using SOCKET = int;
constexpr int INVALID_SOCKET = -1;
#endif

class VideoStreamServer
{
public:
	static VideoStreamServer& GetInstance();

	bool Start(uint16 port = 26761);
	void Stop();

	bool HasActiveClient() const;
	void BroadcastFrame(uint8 packetType, uint64 ptsUs, const uint8* data, size_t size, bool isKeyframe);
	void BroadcastRumble(bool active, uint8 intensity = 255, uint16 durationMs = 50);
	void BroadcastAudio(const void* data, size_t size);

	bool IsMicBlowActive() const { return m_micBlowActive.load(); }

	// Packet types (Cemu -> phone)
	static constexpr uint8 PACKET_TYPE_VIDEO = 0x01;
	static constexpr uint8 PACKET_TYPE_AUDIO = 0x02;
	static constexpr uint8 PACKET_TYPE_RUMBLE = 0x03;

	// Transport opcodes phone -> Cemu on the TCP control channel
	static constexpr uint8 OPCODE_IDR_REQUEST = 0x10;
	static constexpr uint8 OPCODE_TRANSPORT_UDP = 0x11;
	static constexpr uint8 OPCODE_TRANSPORT_TCP = 0x12;
	static constexpr uint8 OPCODE_MIC_BLOW = 0x13;

	// Ports
	static constexpr uint16 AUDIO_PORT = 26762;
	static constexpr uint16 DISCOVERY_PORT = 26763;

	// UDP video datagram protocol v1 (all multi-byte little-endian)
	static constexpr uint16 UDP_MAGIC = 0x5043; // 'C','P'
	static constexpr uint8 UDP_VERSION = 1;
	static constexpr uint8 UDP_FLAG_START = 0x01;
	static constexpr uint8 UDP_FLAG_END = 0x02;
	static constexpr uint8 UDP_FLAG_IDR = 0x04;
	static constexpr size_t UDP_HEADER_SIZE = 24;
	static constexpr size_t UDP_MAX_PAYLOAD = 1400;

private:
	VideoStreamServer();
	~VideoStreamServer();

	void ServerThreadFunc();
	void DiscoveryThreadFunc();
	void ClientRxThreadFunc(uintptr_t clientSocket);
	void PruneFinishedRxThreads();
	void SendUdpFrame(const sockaddr_in& destAddr, uint64 ptsUs, const uint8* data, size_t size, bool isKeyframe);

	struct ClientInfo
	{
		uintptr_t socket;
		sockaddr_in addr; // peer address for UDP video target
		bool useUdp{ false };
	};

	std::atomic<bool> m_isRunning{ false };
	std::atomic<bool> m_micBlowActive{ false };
	uint16 m_port{ 26761 };

	std::thread m_serverThread;
	std::thread m_discoveryThread;
	std::vector<std::thread> m_rxThreads;

	SOCKET m_udpSock{ INVALID_SOCKET };
	SOCKET m_discoverySock{ INVALID_SOCKET };
	std::atomic<uint32> m_frameId{ 0 };
	std::atomic<uint32> m_seq{ 0 };
	std::atomic<uint32> m_audioSeq{ 0 };
	std::atomic<uint64> m_udpFrames{ 0 };
	std::atomic<uint64> m_udpPackets{ 0 };
	std::atomic<uint64> m_udpSendErrors{ 0 };

	mutable std::mutex m_clientsMutex;
	std::vector<ClientInfo> m_clients;
};
