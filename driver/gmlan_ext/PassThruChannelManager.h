/**
 * PassThru Channel Manager
 * Thread-safe multi-CLL physical channel multiplexer for J2534 PassThru hardware.
 * Manages raw SW-CAN (Pin 1) and HS-CAN (Pins 6/14) channels, reference-counting,
 * and asynchronous frame distribution.
 *
 * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
 * Inspired by the Saab community - saabiste.net
 *
 * Source: https://github.com/oamne/dpdu-passthru
 */

#ifndef GMLAN_EXT_PASSTHRU_CHANNEL_MANAGER_H
#define GMLAN_EXT_PASSTHRU_CHANNEL_MANAGER_H

#include <cstdint>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include "j2534/j2534_v0404.h"

enum class SaabBusType {
    BUS_NONE = -1,
    BUS_SWCAN = 0, // Single-Wire CAN (Pin 1 DLC, 33.333 kbps, GM SW-CAN)
    BUS_HSCAN = 1, // High-Speed CAN (Pins 6/14 DLC, 500 kbps, Powertrain CAN)
    BUS_MSCAN = 2, // Mid-Speed CAN (Pins 3/11 DLC, 95.238 kbps, Infotainment CAN)
    BUS_KLINE = 3  // K-Line (Pins 7/8 DLC)
};

/**
 * Interface implemented by CLL adapters to receive incoming CAN frames.
 */
class ICllFrameReceiver {
public:
    virtual ~ICllFrameReceiver() = default;
    virtual void OnCanFrameReceived(uint32_t canId, const uint8_t* pData, uint32_t length, SaabBusType busType) = 0;
};

/**
 * Singleton managing shared physical PassThru channels across multiple Logical Links.
 */
class PassThruChannelManager {
public:
    static PassThruChannelManager& Instance();

    /**
     * Acquires a physical channel for the specified bus.
     * If the channel is already open, increments its reference count and attaches the receiver.
     */
    long AcquireChannel(unsigned long deviceId, SaabBusType busType, ICllFrameReceiver* pReceiver, unsigned long& outChannelId);

    /**
     * Releases a reference to the physical channel.
     * When reference count drops to 0, stops the worker thread and disconnects the physical channel.
     */
    long ReleaseChannel(SaabBusType busType, ICllFrameReceiver* pReceiver);

    /**
     * Transmits a raw CAN frame onto the specified physical bus.
     */
    long TransmitCanFrame(SaabBusType busType, uint32_t canId, const uint8_t* pData, uint32_t length);

    /**
     * Sends a Single-Wire CAN High-Voltage wakeup pulse (Pin 1).
     * Handled non-blockingly: if unsupported by the J2534 interface, logs a warning and proceeds.
     */
    long SendHighVoltageWakeup(SaabBusType busType);

    /**
     * Resets and cleans up all open channels. Called during PDUModuleDisconnect / PDUDestruct.
     */
    void Reset();

    /**
     * Closes the physical J2534 channel for a bus while keeping logical receivers alive in quiet mode.
     * Used for dynamic transceiver switching on single-channel hardware (e.g. Scanmatik SM2).
     */
    void ClosePhysicalChannel(SaabBusType busType);

private:
    PassThruChannelManager();
    ~PassThruChannelManager();

    PassThruChannelManager(const PassThruChannelManager&) = delete;
    PassThruChannelManager& operator=(const PassThruChannelManager&) = delete;

    struct ChannelEntry {
        SaabBusType busType;
        unsigned long deviceId = 0;
        unsigned long channelId = 0;
        unsigned long filterId = 0;
        unsigned long protocolId = 0;
        int refCount = 0;
        std::atomic<bool> isRunning{false};
        std::jthread rxThread;
        std::set<ICllFrameReceiver*> receivers;
        std::mutex channelMutex;
    };

    long SwitchPhysicalBus(SaabBusType targetBus);
    void RxWorkerLoop(std::stop_token stopToken, SaabBusType busType);

    std::mutex m_managerMutex;
    std::recursive_mutex m_switchMutex;
    std::map<SaabBusType, ChannelEntry> m_channels;
    SaabBusType m_activePhysicalBus{SaabBusType::BUS_NONE};
    unsigned long m_activeDeviceId{0};
};

#endif // GMLAN_EXT_PASSTHRU_CHANNEL_MANAGER_H
