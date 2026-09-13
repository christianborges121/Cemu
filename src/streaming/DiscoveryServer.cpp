#include "Common/precompiled.h"
#include "streaming/DiscoveryServer.h"
#include "Cemu/Logging/CemuLogging.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace
{
	constexpr std::chrono::seconds kDeviceExpiry{10};
	constexpr const char* kBeaconDiscover = "CEMUPAD_DISCOVER";
	constexpr const char* kBeaconHerePrefix = "CEMUPAD_HERE:";
	constexpr const char* kProbeMessage = "CEMU_DISCOVER";

#if defined(_WIN32)
	using NativeSocket = SOCKET;
	constexpr NativeSocket kInvalidNative = INVALID_SOCKET;
#else
	using NativeSocket = int;
	constexpr NativeSocket kInvalidNative = -1;
#endif

	void CloseNative(NativeSocket sock)
	{
		if (sock == kInvalidNative)
			return;
#if defined(_WIN32)
		closesocket(sock);
#else
		::close(sock);
#endif
	}

	std::string HostName()
	{
		char name[128] = "Cemu-PC";
#if defined(_WIN32)
		gethostname(name, sizeof(name));
#else
		gethostname(name, sizeof(name));
#endif
		name[sizeof(name) - 1] = '\0';
		return std::string(name);
	}

	uint16_t ParsePort(const std::string& token, uint16_t fallback)
	{
		try
		{
			unsigned long value = std::stoul(token);
			if (value > 0 && value <= 65535)
				return static_cast<uint16_t>(value);
		}
		catch (...)
		{
		}
		return fallback;
	}
}

DiscoveryServer& DiscoveryServer::GetInstance()
{
	static DiscoveryServer s_instance;
	return s_instance;
}

bool DiscoveryServer::IsRunning() const
{
	return m_isRunning.load();
}

bool DiscoveryServer::Start(uint16_t port)
{
	if (m_isRunning.load())
		return true;

#if defined(_WIN32)
	if (!m_wsaInitialized)
	{
		WSADATA wsaData{};
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			cemuLog_log(LogType::Force, "DiscoveryServer: WSAStartup failed");
			return false;
		}
		m_wsaInitialized = true;
	}
#endif

	NativeSocket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == kInvalidNative)
	{
		cemuLog_log(LogType::Force, "DiscoveryServer: Failed to create UDP socket");
		return false;
	}

	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#if defined(_WIN32)
	BOOL broadcast = TRUE;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
#else
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
#endif

	sockaddr_in bindAddr{};
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bindAddr.sin_port = htons(port);
	if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0)
	{
		cemuLog_log(LogType::Force, "DiscoveryServer: Bind failed on UDP port {}", port);
		CloseNative(sock);
		return false;
	}

	m_port = port;
	m_socket = static_cast<intptr_t>(sock);
	m_isRunning = true;
	m_workerThread = std::thread(&DiscoveryServer::WorkerLoop, this);
	cemuLog_log(LogType::Force, "DiscoveryServer: Listening for CemuPad discovery on UDP port {}", port);
	return true;
}

void DiscoveryServer::Stop()
{
	if (!m_isRunning.exchange(false))
	{
		// Still close a half-opened socket if Start failed midway.
		NativeSocket sock = static_cast<NativeSocket>(m_socket.exchange(static_cast<intptr_t>(kInvalidNative)));
		CloseNative(sock);
		return;
	}

	NativeSocket sock = static_cast<NativeSocket>(m_socket.exchange(static_cast<intptr_t>(kInvalidNative)));
	CloseNative(sock);

	if (m_workerThread.joinable())
		m_workerThread.join();

#if defined(_WIN32)
	if (m_wsaInitialized)
	{
		WSACleanup();
		m_wsaInitialized = false;
	}
#endif
	cemuLog_log(LogType::Force, "DiscoveryServer: Stopped");
}

void DiscoveryServer::BroadcastProbe()
{
	NativeSocket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (probe == kInvalidNative)
		return;

	int opt = 1;
#if defined(_WIN32)
	BOOL broadcast = TRUE;
	setsockopt(probe, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
#else
	setsockopt(probe, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
#endif

	sockaddr_in dest{};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(m_port.load());
	dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(probe, kProbeMessage, static_cast<int>(strlen(kProbeMessage)), 0,
		reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
	CloseNative(probe);
}

std::vector<DiscoveredDevice> DiscoveryServer::GetDiscoveredDevices()
{
	PruneStaleDevices();
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_devices;
}

void DiscoveryServer::SetDeviceDiscoveredCallback(std::function<void(const DiscoveredDevice&)> callback)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_callback = std::move(callback);
}

void DiscoveryServer::PruneStaleDevices()
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(m_mutex);
	m_devices.erase(
		std::remove_if(m_devices.begin(), m_devices.end(),
			[&](const DiscoveredDevice& dev) { return (now - dev.lastSeen) > kDeviceExpiry; }),
		m_devices.end());
}

void DiscoveryServer::UpsertDevice(const std::string& ip, const std::string& name,
	uint16_t dsuPort, uint16_t videoPort, uint16_t audioPort)
{
	std::function<void(const DiscoveredDevice&)> callback;
	DiscoveredDevice snapshot;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		bool isNew = true;
		for (auto& dev : m_devices)
		{
			if (dev.ip == ip)
			{
				dev.name = name;
				dev.dsuPort = dsuPort;
				dev.videoPort = videoPort;
				dev.audioPort = audioPort;
				dev.lastSeen = now;
				isNew = false;
				snapshot = dev;
				break;
			}
		}
		if (isNew)
		{
			DiscoveredDevice dev;
			dev.ip = ip;
			dev.name = name;
			dev.dsuPort = dsuPort;
			dev.videoPort = videoPort;
			dev.audioPort = audioPort;
			dev.lastSeen = now;
			m_devices.push_back(dev);
			snapshot = dev;
		}
		callback = m_callback;
	}
	cemuLog_log(LogType::Force, "DiscoveryServer: Discovered CemuPad '{}' at {}", name, ip);
	if (callback)
		callback(snapshot);
}

void DiscoveryServer::HandlePacket(const char* data, int length, uint32_t senderIpHostOrder, uint16_t senderPort)
{
	(void)senderPort;
	if (data == nullptr || length <= 0)
		return;

	std::string payload(data, static_cast<size_t>(length));

	// Phone looking for this PC -> answer directly.
	if (payload.compare(0, strlen(kBeaconDiscover), kBeaconDiscover) == 0)
	{
		NativeSocket sock = static_cast<NativeSocket>(m_socket.load());
		if (sock == kInvalidNative)
			return;
		const std::string reply = fmt::format("CEMUPAD_HERE:{}:{}:{}:{}",
			HostName(), kDsuPort, kVideoPort, kAudioPort);
		sockaddr_in dest{};
		dest.sin_family = AF_INET;
		dest.sin_port = htons(senderPort);
		dest.sin_addr.s_addr = htonl(senderIpHostOrder);
		sendto(sock, reply.c_str(), static_cast<int>(reply.size()), 0,
			reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
		return;
	}

	// Phone announcing itself -> track it.
	if (payload.compare(0, strlen(kBeaconHerePrefix), kBeaconHerePrefix) == 0)
	{
		std::string body = payload.substr(strlen(kBeaconHerePrefix));
		// Trim trailing whitespace/newlines phones may append.
		while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
			body.pop_back();

		std::vector<std::string> parts;
		size_t start = 0;
		while (true)
		{
			size_t pos = body.find(':', start);
			if (pos == std::string::npos)
			{
				parts.push_back(body.substr(start));
				break;
			}
			parts.push_back(body.substr(start, pos - start));
			start = pos + 1;
		}
		if (parts.empty())
			return;

		const std::string name = parts[0].empty() ? "CemuPad" : parts[0];
		const uint16_t dsuPort = parts.size() > 1 ? ParsePort(parts[1], kDsuPort) : kDsuPort;
		const uint16_t videoPort = parts.size() > 2 ? ParsePort(parts[2], kVideoPort) : kVideoPort;
		const uint16_t audioPort = parts.size() > 3 ? ParsePort(parts[3], kAudioPort) : kAudioPort;

		in_addr addr{};
		addr.s_addr = htonl(senderIpHostOrder);
		char ipBuf[64] = {};
#if defined(_WIN32)
		InetNtopA(AF_INET, &addr, ipBuf, sizeof(ipBuf));
#else
		inet_ntop(AF_INET, &addr, ipBuf, sizeof(ipBuf));
#endif
		UpsertDevice(ipBuf[0] ? ipBuf : "unknown", name, dsuPort, videoPort, audioPort);
	}
	// "CEMU_DISCOVER" probes (ours or the phone's mirrored copy) are ignored.
}

void DiscoveryServer::WorkerLoop()
{
	while (m_isRunning.load())
	{
		NativeSocket sock = static_cast<NativeSocket>(m_socket.load());
		if (sock == kInvalidNative)
			break;

		fd_set readSet{};
		FD_ZERO(&readSet);
#if defined(_WIN32)
#pragma warning(push)
#pragma warning(disable : 4548)
		FD_SET(sock, &readSet);
#pragma warning(pop)
		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 500 * 1000;
		const int ready = select(0, &readSet, nullptr, nullptr, &tv);
#else
		FD_SET(sock, &readSet);
		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 500 * 1000;
		const int ready = select(sock + 1, &readSet, nullptr, nullptr, &tv);
#endif
		if (!m_isRunning.load())
			break;
		if (ready <= 0)
			continue;

		char buffer[512];
		sockaddr_in sender{};
#if defined(_WIN32)
		int senderLen = sizeof(sender);
		const int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
			reinterpret_cast<sockaddr*>(&sender), &senderLen);
#else
		socklen_t senderLen = sizeof(sender);
		const ssize_t received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
			reinterpret_cast<sockaddr*>(&sender), &senderLen);
#endif
		if (received > 0)
		{
			buffer[received] = '\0';
			HandlePacket(buffer, static_cast<int>(received),
				ntohl(sender.sin_addr.s_addr), ntohs(sender.sin_port));
		}
	}
}
