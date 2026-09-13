#pragma once
// DiscoveryServer — bidirectional zero-config auto-discovery for CemuPad.
//
// - Listens on UDP 26763 for "CEMUPAD_DISCOVER" beacons from Android phones
//   and answers "CEMUPAD_HERE:<hostname>:<dsu>:<video>:<audio>".
// - Records "CEMUPAD_HERE:..." announcements into a thread-safe registry with
//   last-seen timestamps (stale entries expire after 10 seconds).
// - BroadcastProbe() sends "CEMU_DISCOVER" to the subnet broadcast address so
//   phones can discover this PC; phones answer with "CEMUPAD_HERE:...".
//
// This is the sole owner of UDP port 26763. The legacy discovery thread in
// VideoStreamServer was retired (see Phase 4.0 Step 3.5) to avoid double-bind.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DiscoveredDevice
{
	std::string ip;
	std::string name;
	uint16_t dsuPort{26760};
	uint16_t videoPort{26761};
	uint16_t audioPort{26762};
	std::chrono::steady_clock::time_point lastSeen{};
};

class DiscoveryServer
{
public:
	static DiscoveryServer& GetInstance();

	static constexpr uint16_t kDefaultPort = 26763;
	static constexpr uint16_t kDsuPort = 26760;
	static constexpr uint16_t kVideoPort = 26761;
	static constexpr uint16_t kAudioPort = 26762;

	// Idempotent. Returns false when the port cannot be bound (e.g. another
	// Cemu instance already owns discovery on this machine).
	bool Start(uint16_t port = kDefaultPort);
	void Stop();
	bool IsRunning() const;

	// Send a "CEMU_DISCOVER" probe to 255.255.255.255:<port>.
	void BroadcastProbe();

	// Snapshot of currently visible devices (entries older than 10s are pruned).
	std::vector<DiscoveredDevice> GetDiscoveredDevices();
	void SetDeviceDiscoveredCallback(std::function<void(const DiscoveredDevice&)> callback);

private:
	DiscoveryServer() = default;
	~DiscoveryServer() { Stop(); }
	DiscoveryServer(const DiscoveryServer&) = delete;
	DiscoveryServer& operator=(const DiscoveryServer&) = delete;

	void WorkerLoop();
	void HandlePacket(const char* data, int length, uint32_t senderIpHostOrder, uint16_t senderPort);
	void UpsertDevice(const std::string& ip, const std::string& name, uint16_t dsuPort, uint16_t videoPort, uint16_t audioPort);
	void PruneStaleDevices();

	std::atomic<bool> m_isRunning{false};
	std::thread m_workerThread;
	std::atomic<uint16_t> m_port{kDefaultPort};
	// Native socket handle (SOCKET on Windows, fd otherwise); -1 when closed.
	std::atomic<intptr_t> m_socket{-1};
#if defined(_WIN32)
	bool m_wsaInitialized{false};
#endif

	mutable std::mutex m_mutex;
	std::vector<DiscoveredDevice> m_devices;
	std::function<void(const DiscoveredDevice&)> m_callback;
};
