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
#endif

class VideoStreamServer
{
public:
	static VideoStreamServer& GetInstance();

	bool Start(uint16 port = 26761);
	void Stop();

	bool HasActiveClient() const;
	void BroadcastFrame(uint8 packetType, uint64 ptsUs, const uint8* data, size_t size);

private:
	VideoStreamServer();
	~VideoStreamServer();

	void ServerThreadFunc();
	void ClientRxThreadFunc(uintptr_t clientSocket);

	std::atomic<bool> m_isRunning{ false };
	uint16 m_port{ 26761 };

	std::thread m_serverThread;
	std::vector<std::thread> m_rxThreads;

	mutable std::mutex m_clientsMutex;
	std::vector<uintptr_t> m_clientSockets;
};
