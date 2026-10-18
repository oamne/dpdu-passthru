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

#include <windows.h>
#include <cstring>
#include "PassThruChannelManager.h"
#include "j2534/shim_loader.h"
#include "Logger.h"
#include <chrono>

PassThruChannelManager& PassThruChannelManager::Instance()
{
    static PassThruChannelManager s_instance;
    return s_instance;
}

PassThruChannelManager::PassThruChannelManager()
{
}

PassThruChannelManager::~PassThruChannelManager()
{
    Reset();
}

long PassThruChannelManager::AcquireChannel(
    unsigned long deviceId,
    SaabBusType busType,
    ICllFrameReceiver* pReceiver,
    unsigned long& outChannelId)
{
    std::lock_guard<std::recursive_mutex> switchLock(m_switchMutex);

    m_activeDeviceId = deviceId;

    // Check if channel is already logically registered
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_channels.find(busType);
        if (it != m_channels.end() && it->second.refCount > 0)
        {
            it->second.refCount++;
            if (pReceiver != nullptr)
            {
                std::lock_guard<std::mutex> chLock(it->second.channelMutex);
                it->second.receivers.insert(pReceiver);
            }
            outChannelId = it->second.channelId;
            LOGGER.logInfo("PassThruChannelManager", "Reusing logical channel for busType %d (channelId: %lu, refCount: %d)",
                           static_cast<int>(busType), outChannelId, it->second.refCount);
            return STATUS_NOERROR;
        }
    }

    // Point 1 & Point 3 requirement: MS-CAN (Pins 3/11) must run in pure virtual quiet mode
    // (zero J2534 calls to avoid hardware contention with Scanmatik single CAN controller)
    if (busType == SaabBusType::BUS_MSCAN)
    {
        LOGGER.logInfo("PassThruChannelManager", "BUS_MSCAN running in pure virtual quiet mode (zero J2534 calls)");
        std::lock_guard<std::mutex> lock(m_managerMutex);
        ChannelEntry& entry = m_channels[busType];
        entry.busType = busType;
        entry.deviceId = deviceId;
        entry.channelId = 0;
        entry.filterId = 0;
        entry.protocolId = 0;
        entry.refCount = 1;
        entry.isRunning.store(true);

        if (pReceiver != nullptr)
        {
            std::lock_guard<std::mutex> chLock(entry.channelMutex);
            entry.receivers.insert(pReceiver);
        }

        outChannelId = 0;
        return STATUS_NOERROR;
    }

    if (busType != SaabBusType::BUS_SWCAN && busType != SaabBusType::BUS_HSCAN)
    {
        LOGGER.logWarn("PassThruChannelManager", "Unsupported busType %d for channel multiplexing", static_cast<int>(busType));
        return ERR_NOT_SUPPORTED;
    }

    // Register logical channel entry
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        ChannelEntry& entry = m_channels[busType];
        entry.busType = busType;
        entry.deviceId = deviceId;
        entry.channelId = 0;
        entry.filterId = 0;
        entry.protocolId = (busType == SaabBusType::BUS_SWCAN) ? SW_CAN_PS : CAN;
        entry.refCount = 1;
        entry.isRunning.store(false);

        if (pReceiver != nullptr)
        {
            std::lock_guard<std::mutex> chLock(entry.channelMutex);
            entry.receivers.insert(pReceiver);
        }
    }

    // Dynamic physical arbitration:
    // If no physical bus is active yet, connect this bus immediately.
    // If another bus is already active, keep this logical channel dormant until needed.
    if (m_activePhysicalBus == SaabBusType::BUS_NONE)
    {
        long status = SwitchPhysicalBus(busType);
        if (status == STATUS_NOERROR)
        {
            std::lock_guard<std::mutex> lock(m_managerMutex);
            outChannelId = m_channels[busType].channelId;
        }
        else
        {
            outChannelId = 0;
        }
    }
    else
    {
        outChannelId = 0;
        LOGGER.logInfo("PassThruChannelManager", "Logical channel acquired for busType %d (dormant physical, active bus: %d)",
                       static_cast<int>(busType), static_cast<int>(m_activePhysicalBus));
    }

    return STATUS_NOERROR;
}

long PassThruChannelManager::SwitchPhysicalBus(SaabBusType targetBus)
{
    if (targetBus != SaabBusType::BUS_SWCAN && targetBus != SaabBusType::BUS_HSCAN)
    {
        return STATUS_NOERROR;
    }

    std::lock_guard<std::recursive_mutex> switchLock(m_switchMutex);

    // Fast check: is targetBus already the active physical bus?
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        if (m_activePhysicalBus == targetBus)
        {
            auto it = m_channels.find(targetBus);
            if (it != m_channels.end() && it->second.channelId != 0)
            {
                return STATUS_NOERROR;
            }
        }
    }

    LOGGER.logInfo("PassThruChannelManager", "SwitchPhysicalBus: Switching hardware from busType %d to %d",
                   static_cast<int>(m_activePhysicalBus), static_cast<int>(targetBus));

    // Step 1: Disconnect old physical bus if active
    std::jthread threadToJoin;
    unsigned long oldChannelId = 0;
    unsigned long oldFilterId = 0;
    SaabBusType oldBus = m_activePhysicalBus;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        if (m_activePhysicalBus != SaabBusType::BUS_NONE)
        {
            auto it = m_channels.find(m_activePhysicalBus);
            if (it != m_channels.end() && it->second.channelId != 0)
            {
                ChannelEntry& oldEntry = it->second;
                oldEntry.isRunning.store(false);
                oldChannelId = oldEntry.channelId;
                oldFilterId = oldEntry.filterId;
                oldEntry.channelId = 0;
                oldFilterId = 0;
                threadToJoin = std::move(oldEntry.rxThread);
            }
        }
        m_activePhysicalBus = SaabBusType::BUS_NONE;
    }

    if (threadToJoin.joinable())
    {
        threadToJoin.request_stop();
        threadToJoin.join();
    }

    if (oldFilterId != 0 && oldChannelId != 0)
    {
        _PassThruStopMsgFilter(oldChannelId, oldFilterId);
    }

    if (oldChannelId != 0)
    {
        _PassThruDisconnect(oldChannelId);
        LOGGER.logInfo("PassThruChannelManager", "Physical channel %lu for busType %d disconnected for switch",
                       oldChannelId, static_cast<int>(oldBus));
    }

    // Hardware transceiver / relay settle delay (Scanmatik SM2 multiplexer relay)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step 2: Connect target physical bus
    unsigned long protocolId = 0;
    unsigned long baudRate = 0;
    unsigned long flags = 0;

    if (targetBus == SaabBusType::BUS_SWCAN)
    {
        protocolId = SW_CAN_PS; // 0x8008 - Single Wire CAN Pin Selectable (Pin 1)
        baudRate = 33333;       // GM SW-CAN standard speed: 33.333 kbps
        flags = 0;              // Standard connect flags
    }
    else // BUS_HSCAN
    {
        protocolId = CAN;       // Protocol ID 5 - Raw CAN (Pins 6/14)
        baudRate = 500000;     // Powertrain CAN standard speed: 500 kbps
        flags = CAN_ID_BOTH;    // Accept both 11-bit and 29-bit CAN identifiers
    }

    unsigned long newChannelId = 0;
    unsigned long deviceId = m_activeDeviceId;

    long status = _PassThruConnect(deviceId, protocolId, flags, baudRate, &newChannelId);
    if (status != STATUS_NOERROR)
    {
        char errBuf[256] = { 0 };
        _PassThruGetLastError(errBuf);
        LOGGER.logError("PassThruChannelManager", "SwitchPhysicalBus: _PassThruConnect failed (status: %ld, proto: 0x%lX, err: %s) for busType %d",
                        status, protocolId, errBuf, static_cast<int>(targetBus));
        return status;
    }

    // For SW-CAN (Pin Selectable), explicitly route physical transceiver to DLC Pin 1 (J1962_PINS)
    if (targetBus == SaabBusType::BUS_SWCAN)
    {
        SCONFIG pinConfig;
        pinConfig.Parameter = J1962_PINS; // 0x8001
        pinConfig.Value = 0x0100;         // J2534-2 format: Pin(+) = 1 (DLC Pin 1), Pin(-) = 0

        SCONFIG_LIST configList;
        configList.NumOfParams = 1;
        configList.ConfigPtr = &pinConfig;

        long pinStatus = _PassThruIoctl(newChannelId, SET_CONFIG, &configList, nullptr);
        if (pinStatus != STATUS_NOERROR)
        {
            char errBuf[256] = { 0 };
            _PassThruGetLastError(errBuf);
            LOGGER.logWarn("PassThruChannelManager", "SwitchPhysicalBus: SET_CONFIG J1962_PINS failed (status: %ld, err: %s)",
                           pinStatus, errBuf);
        }
        else
        {
            LOGGER.logInfo("PassThruChannelManager", "SwitchPhysicalBus: Routed SW-CAN channel %lu to DLC Pin 1 (J1962_PINS: 0x0100)", newChannelId);
        }
    }

    // Install all-pass message filter (mask = 0, pattern = 0)
    PASSTHRU_MSG maskMsg;
    std::memset(&maskMsg, 0, sizeof(maskMsg));
    maskMsg.ProtocolID = protocolId;
    maskMsg.DataSize = 4; // Check the 4-byte CAN identifier

    PASSTHRU_MSG patternMsg;
    std::memset(&patternMsg, 0, sizeof(patternMsg));
    patternMsg.ProtocolID = protocolId;
    patternMsg.DataSize = 4;

    unsigned long newFilterId = 0;
    long filterStatus = _PassThruStartMsgFilter(newChannelId, PASS_FILTER, &maskMsg, &patternMsg, nullptr, &newFilterId);
    if (filterStatus != STATUS_NOERROR)
    {
        LOGGER.logWarn("PassThruChannelManager", "SwitchPhysicalBus: PassThruStartMsgFilter warning: status %ld", filterStatus);
    }
    else
    {
        LOGGER.logInfo("PassThruChannelManager", "SwitchPhysicalBus: All-pass filter active (FilterID: %lu) on channel %lu",
                       newFilterId, newChannelId);
    }

    // Step 3: Update channel entry and launch RxWorkerLoop
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        ChannelEntry& entry = m_channels[targetBus];
        entry.busType = targetBus;
        entry.deviceId = deviceId;
        entry.channelId = newChannelId;
        entry.filterId = newFilterId;
        entry.protocolId = protocolId;
        entry.isRunning.store(true);

        entry.rxThread = std::jthread([this, targetBus](std::stop_token st) {
            this->RxWorkerLoop(st, targetBus);
        });

        m_activePhysicalBus = targetBus;
    }

    LOGGER.logInfo("PassThruChannelManager", "SwitchPhysicalBus: Successfully switched physical hardware to busType %d (channel %lu)",
                   static_cast<int>(targetBus), newChannelId);

    return STATUS_NOERROR;
}

void PassThruChannelManager::ClosePhysicalChannel(SaabBusType busType)
{
    std::lock_guard<std::recursive_mutex> switchLock(m_switchMutex);

    std::jthread threadToJoin;
    unsigned long channelToClose = 0;
    unsigned long filterToStop = 0;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_channels.find(busType);
        if (it == m_channels.end() || it->second.channelId == 0)
        {
            return;
        }

        ChannelEntry& entry = it->second;
        entry.isRunning.store(false);
        channelToClose = entry.channelId;
        filterToStop = entry.filterId;
        entry.channelId = 0;
        entry.filterId = 0;
        threadToJoin = std::move(entry.rxThread);

        if (m_activePhysicalBus == busType)
        {
            m_activePhysicalBus = SaabBusType::BUS_NONE;
        }
    }

    if (threadToJoin.joinable())
    {
        threadToJoin.request_stop();
        threadToJoin.join();
    }

    if (filterToStop != 0 && channelToClose != 0)
    {
        _PassThruStopMsgFilter(channelToClose, filterToStop);
    }

    if (channelToClose != 0)
    {
        _PassThruDisconnect(channelToClose);
        LOGGER.logInfo("PassThruChannelManager", "Physical channel %lu for busType %d closed cleanly",
                       channelToClose, static_cast<int>(busType));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

long PassThruChannelManager::ReleaseChannel(SaabBusType busType, ICllFrameReceiver* pReceiver)
{
    std::lock_guard<std::recursive_mutex> switchLock(m_switchMutex);

    std::jthread threadToJoin;
    unsigned long channelToDisconnect = 0;
    unsigned long filterToStop = 0;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);

        auto it = m_channels.find(busType);
        if (it == m_channels.end() || it->second.refCount <= 0)
        {
            return STATUS_NOERROR;
        }

        ChannelEntry& entry = it->second;

        if (pReceiver != nullptr)
        {
            std::lock_guard<std::mutex> chLock(entry.channelMutex);
            size_t erased = entry.receivers.erase(pReceiver);
            if (erased == 0)
            {
                LOGGER.logInfo("PassThruChannelManager", "Receiver %p already released for busType %d (refCount: %d)",
                               pReceiver, static_cast<int>(busType), entry.refCount);
                return STATUS_NOERROR;
            }
        }

        entry.refCount--;
        LOGGER.logInfo("PassThruChannelManager", "Released channel %lu for busType %d (refCount remaining: %d)",
                       entry.channelId, static_cast<int>(busType), entry.refCount);

        if (entry.refCount <= 0)
        {
            entry.isRunning.store(false);
            channelToDisconnect = entry.channelId;
            filterToStop = entry.filterId;
            threadToJoin = std::move(entry.rxThread);

            if (m_activePhysicalBus == busType)
            {
                m_activePhysicalBus = SaabBusType::BUS_NONE;
            }

            m_channels.erase(it);
        }
    }

    // Safely stop and join receiver thread WITHOUT holding m_managerMutex
    if (threadToJoin.joinable())
    {
        threadToJoin.request_stop();
        threadToJoin.join();
    }

    if (filterToStop != 0 && channelToDisconnect != 0)
    {
        _PassThruStopMsgFilter(channelToDisconnect, filterToStop);
    }

    if (channelToDisconnect != 0)
    {
        _PassThruDisconnect(channelToDisconnect);
        LOGGER.logInfo("PassThruChannelManager", "Physical channel %lu disconnected cleanly", channelToDisconnect);
    }

    return STATUS_NOERROR;
}

long PassThruChannelManager::TransmitCanFrame(SaabBusType busType, uint32_t canId, const uint8_t* pData, uint32_t length)
{
    // If MS-CAN or unsupported, absorb silently in virtual quiet mode
    if (busType != SaabBusType::BUS_SWCAN && busType != SaabBusType::BUS_HSCAN)
    {
        return STATUS_NOERROR;
    }

    // Check if physical bus switch is required
    if (m_activePhysicalBus != busType)
    {
        long switchStatus = SwitchPhysicalBus(busType);
        if (switchStatus != STATUS_NOERROR)
        {
            LOGGER.logWarn("PassThruChannelManager", "TransmitCanFrame: Failed to switch physical bus to %d (status: %ld)",
                           static_cast<int>(busType), switchStatus);
            return switchStatus;
        }
    }

    unsigned long channelId = 0;
    unsigned long protocolId = 0;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_channels.find(busType);
        if (it == m_channels.end() || it->second.channelId == 0)
        {
            LOGGER.logWarn("PassThruChannelManager", "TransmitCanFrame: No active physical channel for busType %d",
                           static_cast<int>(busType));
            return STATUS_NOERROR;
        }
        channelId = it->second.channelId;
        protocolId = it->second.protocolId;
    }

    PASSTHRU_MSG txMsg;
    std::memset(&txMsg, 0, sizeof(txMsg));
    txMsg.ProtocolID = protocolId;
    txMsg.RxStatus = 0;
    txMsg.TxFlags = 0;

    // Standard 4-byte CAN ID header
    txMsg.Data[0] = static_cast<unsigned char>((canId >> 24) & 0xFF);
    txMsg.Data[1] = static_cast<unsigned char>((canId >> 16) & 0xFF);
    txMsg.Data[2] = static_cast<unsigned char>((canId >> 8) & 0xFF);
    txMsg.Data[3] = static_cast<unsigned char>(canId & 0xFF);

    uint32_t totalBytes = 4;
    if (pData != nullptr && length > 0)
    {
        uint32_t copyBytes = (length > (sizeof(txMsg.Data) - 4)) ? (sizeof(txMsg.Data) - 4) : length;
        std::memcpy(&txMsg.Data[4], pData, copyBytes);
        totalBytes += copyBytes;
    }
    txMsg.DataSize = totalBytes;

    unsigned long numMsgs = 1;
    long status = _PassThruWriteMsgs(channelId, &txMsg, &numMsgs, 100);
    if (status != STATUS_NOERROR)
    {
        LOGGER.logWarn("PassThruChannelManager", "PassThruWriteMsgs failed (status: %ld) on channel %lu for busType %d",
                       status, channelId, static_cast<int>(busType));
    }

    return status;
}

long PassThruChannelManager::SendHighVoltageWakeup(SaabBusType busType)
{
    if (busType != SaabBusType::BUS_SWCAN)
    {
        return STATUS_NOERROR;
    }

    // High Voltage Wakeup MUST be on physical SW-CAN
    if (m_activePhysicalBus != SaabBusType::BUS_SWCAN)
    {
        long switchStatus = SwitchPhysicalBus(SaabBusType::BUS_SWCAN);
        if (switchStatus != STATUS_NOERROR)
        {
            LOGGER.logWarn("PassThruChannelManager", "SendHighVoltageWakeup: Failed to switch physical bus to SW-CAN (%ld)", switchStatus);
            return switchStatus;
        }
    }

    unsigned long channelId = 0;
    unsigned long protocolId = 0;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_channels.find(busType);
        if (it == m_channels.end() || it->second.channelId == 0)
        {
            return STATUS_NOERROR;
        }
        channelId = it->second.channelId;
        protocolId = it->second.protocolId;
    }

    LOGGER.logInfo("PassThruChannelManager", "Emitting SW-CAN High-Voltage wakeup on channel %lu", channelId);

    // Attempt SW_CAN High-Voltage pulse via TxFlags (MDI / Mongoose standard)
    PASSTHRU_MSG hvMsg;
    std::memset(&hvMsg, 0, sizeof(hvMsg));
    hvMsg.ProtocolID = protocolId;
    hvMsg.TxFlags = SW_CAN_HV_TX; // High-Voltage Transmit flag (0x00000400)
    hvMsg.Data[0] = 0x00;
    hvMsg.Data[1] = 0x00;
    hvMsg.Data[2] = 0x01;
    hvMsg.Data[3] = 0x00;
    hvMsg.DataSize = 4;

    unsigned long numMsgs = 1;
    long status = _PassThruWriteMsgs(channelId, &hvMsg, &numMsgs, 250);
    if (status != STATUS_NOERROR)
    {
        LOGGER.logWarn("PassThruChannelManager", "High-Voltage wakeup rejected or timed out (%ld)", status);
    }

    return STATUS_NOERROR;
}

void PassThruChannelManager::Reset()
{
    std::lock_guard<std::recursive_mutex> switchLock(m_switchMutex);

    std::vector<std::jthread> threadsToJoin;
    std::vector<std::pair<unsigned long, unsigned long>> channelsToClose;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);

        for (auto& pair : m_channels)
        {
            ChannelEntry& entry = pair.second;
            entry.isRunning.store(false);

            if (entry.rxThread.joinable())
            {
                threadsToJoin.push_back(std::move(entry.rxThread));
            }

            if (entry.channelId != 0)
            {
                channelsToClose.push_back({entry.channelId, entry.filterId});
            }

            entry.refCount = 0;
            entry.receivers.clear();
        }

        m_channels.clear();
        m_activePhysicalBus = SaabBusType::BUS_NONE;
        m_activeDeviceId = 0;
    }

    // Join all threads outside of m_managerMutex
    for (auto& th : threadsToJoin)
    {
        if (th.joinable())
        {
            th.request_stop();
            th.join();
        }
    }

    // Clean up filters and disconnect physical channels
    for (auto& ch : channelsToClose)
    {
        if (ch.second != 0)
        {
            _PassThruStopMsgFilter(ch.first, ch.second);
        }
        _PassThruDisconnect(ch.first);
        LOGGER.logInfo("PassThruChannelManager", "Reset: channel %lu disconnected", ch.first);
    }
}

void PassThruChannelManager::RxWorkerLoop(std::stop_token stopToken, SaabBusType busType)
{
    LOGGER.logInfo("PassThruChannelManager", "RxWorkerLoop started for busType %d", static_cast<int>(busType));

    while (!stopToken.stop_requested())
    {
        unsigned long channelId = 0;
        {
            std::lock_guard<std::mutex> lock(m_managerMutex);
            auto it = m_channels.find(busType);
            if (it == m_channels.end() || !it->second.isRunning.load())
            {
                break;
            }
            channelId = it->second.channelId;
        }

        if (channelId == 0)
        {
            break;
        }

        PASSTHRU_MSG rxMsg;
        std::memset(&rxMsg, 0, sizeof(rxMsg));
        unsigned long numMsgs = 1;

        // 20ms read timeout
        long status = _PassThruReadMsgs(channelId, &rxMsg, &numMsgs, 20);
        if (status == STATUS_NOERROR && numMsgs > 0)
        {
            // Drop TX loopback echo frames (flag 0x01 = TX_MSG_TYPE according to SAE J2534)
            if (rxMsg.RxStatus & 0x01)
            {
                continue;
            }

            if (rxMsg.DataSize >= 4)
            {
                uint32_t canId = (static_cast<uint32_t>(rxMsg.Data[0]) << 24) |
                                 (static_cast<uint32_t>(rxMsg.Data[1]) << 16) |
                                 (static_cast<uint32_t>(rxMsg.Data[2]) << 8)  |
                                 static_cast<uint32_t>(rxMsg.Data[3]);

                const uint8_t* pPayload = &rxMsg.Data[4];
                uint32_t payloadLen = rxMsg.DataSize - 4;

                // Take a snapshot of registered receivers under channel lock
                std::vector<ICllFrameReceiver*> receiversSnapshot;
                {
                    std::lock_guard<std::mutex> lock(m_managerMutex);
                    auto it = m_channels.find(busType);
                    if (it != m_channels.end())
                    {
                        std::lock_guard<std::mutex> chLock(it->second.channelMutex);
                        receiversSnapshot.assign(it->second.receivers.begin(), it->second.receivers.end());
                    }
                }

                // Dispatch frame to all active receivers
                for (ICllFrameReceiver* pReceiver : receiversSnapshot)
                {
                    if (pReceiver != nullptr)
                    {
                        pReceiver->OnCanFrameReceived(canId, pPayload, payloadLen, busType);
                    }
                }
            }
        }
        else if (status != STATUS_NOERROR && status != ERR_BUFFER_EMPTY)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    LOGGER.logInfo("PassThruChannelManager", "RxWorkerLoop exiting for busType %d", static_cast<int>(busType));
}
