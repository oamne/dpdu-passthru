/**
 * GMLAN CAN Protocol Adapter
 * Handles GMLAN Single-Wire CAN (SW-CAN) and High-Speed CAN (HS-CAN) diagnostic communications
 * for Saab 9-3 NG (9440) under Tech2Win.
 *
 * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
 * Inspired by the Saab community - saabiste.net
 *
 * Source: https://github.com/oamne/dpdu-passthru
 */

#include "GmlanCanAdapter.h"
#include "Logger.h"
#include <chrono>
#include <cstring>

static inline uint32_t GetExpectedRspCanId(uint32_t reqCanId)
{
    if (reqCanId >= 0x7E0 && reqCanId <= 0x7E7)
    {
        return reqCanId + 0x08; // HS-CAN ISO 15765-4 / GMLAN (0x7E0 -> 0x7E8)
    }
    if (reqCanId < 0x600)
    {
        return reqCanId + 0x400; // SW-CAN GMLAN (0x241 -> 0x641, 0x245 -> 0x645)
    }
    return reqCanId;
}

static inline uint32_t GetReqCanIdFromRsp(uint32_t rspCanId)
{
    if (rspCanId >= 0x7E8 && rspCanId <= 0x7EF)
    {
        return rspCanId - 0x08; // HS-CAN ISO 15765-4 / GMLAN (0x7E8 -> 0x7E0)
    }
    if (rspCanId >= 0x600 && rspCanId <= 0x6FF)
    {
        return rspCanId - 0x400; // SW-CAN GMLAN (0x641 -> 0x241, 0x645 -> 0x245)
    }
    if (rspCanId >= 0x400)
    {
        return rspCanId - 0x400;
    }
    return rspCanId;
}

GmlanCanAdapter::GmlanCanAdapter(SaabBusType busType)
    : m_busType(busType), m_deviceId(0), m_channelId(0), m_pSink(nullptr), m_lastCopTag(nullptr)
{
}

GmlanCanAdapter::~GmlanCanAdapter()
{
    Disconnect();
}

long GmlanCanAdapter::Connect(unsigned long deviceId, ILogicalLinkEventSink* pSink)
{
    m_deviceId = deviceId;
    m_pSink = pSink;

    LOGGER.logInfo("GmlanCanAdapter", "Connecting adapter for busType %d (DeviceId: %lu)",
                   static_cast<int>(m_busType), deviceId);

    long status = PassThruChannelManager::Instance().AcquireChannel(deviceId, m_busType, this, m_channelId);
    if (status != STATUS_NOERROR)
    {
        LOGGER.logWarn("GmlanCanAdapter", "AcquireChannel returned %ld for busType %d, continuing in quiet mode",
                       status, static_cast<int>(m_busType));
        m_channelId = 0;
        return STATUS_NOERROR;
    }

    LOGGER.logInfo("GmlanCanAdapter", "Adapter connected successfully on channel %lu (busType %d)",
                   m_channelId, static_cast<int>(m_busType));
    return STATUS_NOERROR;
}

long GmlanCanAdapter::Disconnect()
{
    LOGGER.logInfo("GmlanCanAdapter", "Disconnecting adapter for busType %d (ChannelId: %lu)",
                   static_cast<int>(m_busType), m_channelId);

    long status = PassThruChannelManager::Instance().ReleaseChannel(m_busType, this);
    m_channelId = 0;
    m_pSink = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        m_activeCops.clear();
        m_rxAssemblies.clear();
        m_txMultiFrames.clear();
    }

    return status;
}

static void FormatSingleFramePayload(const uint8_t* pPayload, uint32_t payloadLen, uint8_t outFrame[8])
{
    std::memset(outFrame, 0, 8);
    if (pPayload == nullptr || payloadLen == 0)
    {
        return;
    }

    // Extended Addressing: first byte is Target/Extended Address (e.g. 0xFE)
    if (pPayload[0] == 0xFE)
    {
        outFrame[0] = 0xFE;
        // Check if Single-Frame PCI byte is already present (e.g. [FE, 01, 3E] from RAW CLL)
        if (payloadLen >= 2 && pPayload[1] <= 0x07 && pPayload[1] == (payloadLen - 2))
        {
            uint32_t copyLen = (payloadLen > 8) ? 8 : payloadLen;
            std::memcpy(outFrame, pPayload, copyLen);
        }
        else
        {
            // Insert ISO-TP Single-Frame PCI byte between extended address and service payload
            // e.g. [FE, 3E] -> [FE, 01, 3E, 00, 00, 00, 00, 00]
            uint8_t tpLen = static_cast<uint8_t>((payloadLen - 1) & 0x0F);
            outFrame[1] = tpLen;
            uint32_t copyLen = (payloadLen - 1 > 6) ? 6 : (payloadLen - 1);
            if (copyLen > 0)
            {
                std::memcpy(&outFrame[2], &pPayload[1], copyLen);
            }
        }
        return;
    }

    // Normal Addressing: check if payload already contains ISO-TP Single-Frame PCI (0x0N)
    if (payloadLen >= 2 && (pPayload[0] & 0xF0) == 0x00 && (pPayload[0] & 0x0F) == (payloadLen - 1))
    {
        uint32_t copyLen = (payloadLen > 8) ? 8 : payloadLen;
        std::memcpy(outFrame, pPayload, copyLen);
    }
    else
    {
        outFrame[0] = static_cast<uint8_t>(payloadLen & 0x0F);
        uint32_t copyLen = (payloadLen > 7) ? 7 : payloadLen;
        std::memcpy(&outFrame[1], pPayload, copyLen);
    }
}

uint32_t GmlanCanAdapter::StartComPrimitive(
    uint32_t copType,
    uint32_t copDataSize,
    uint8_t* pCopData,
    PDU_COP_CTRL_DATA* pCopCtrlData,
    void* pCopTag)
{
    uint32_t hCop = m_nextCopHandle.fetch_add(1);
    if (pCopTag != nullptr)
    {
        m_lastCopTag = pCopTag;
    }
    uint32_t nowMs = static_cast<uint32_t>(GetTickCount());

    LOGGER.logInfo("GmlanCanAdapter", "StartComPrimitive: hCop %u, type 0x%X, size %u, pCopTag %p",
                   hCop, copType, copDataSize, pCopTag);

    // 1. Intercept Single-Wire CAN High-Voltage pulse pattern (00 00 01 00)
    if (pCopData != nullptr && copDataSize >= 4 &&
        pCopData[0] == 0x00 && pCopData[1] == 0x00 && pCopData[2] == 0x01 && pCopData[3] == 0x00)
    {
        LOGGER.logInfo("GmlanCanAdapter", "Detected High-Voltage wakeup sequence (00 00 01 00)");
        PassThruChannelManager::Instance().SendHighVoltageWakeup(m_busType);

        if (m_pSink != nullptr)
        {
            PDU_EVENT_ITEM* pExecEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_EXECUTING, nowMs, pCopTag);
            if (pExecEvt != nullptr)
            {
                pExecEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pExecEvt);
            }

            PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, pCopTag);
            if (pFinEvt != nullptr)
            {
                pFinEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pFinEvt);
            }
            m_pSink->NotifyEvents(pCopTag);
        }
        return hCop;
    }

    // 2. Handle StartComm (0x8001) and StopComm (0x8002) lifecycle primitives
    if (copType == PDU_COPT_STARTCOMM || copType == PDU_COPT_STOPCOMM)
    {
        LOGGER.logInfo("GmlanCanAdapter", "Handled StartComm/StopComm primitive (type: 0x%04X, hCop: %u)", copType, hCop);
        if (m_pSink != nullptr)
        {
            if (copType == PDU_COPT_STARTCOMM)
            {
                PDU_EVENT_ITEM* pStartedEvt = SaabExt_CreateTech2WinStatusEvent(PDU_CLLST_COMM_STARTED, nowMs, pCopTag);
                if (pStartedEvt != nullptr)
                {
                    pStartedEvt->hCop = hCop;
                    m_pSink->QueueEventOnly(pStartedEvt);
                }
            }
            else if (copType == PDU_COPT_STOPCOMM)
            {
                PDU_EVENT_ITEM* pOnlineEvt = SaabExt_CreateTech2WinStatusEvent(PDU_CLLST_ONLINE, nowMs, pCopTag);
                if (pOnlineEvt != nullptr)
                {
                    pOnlineEvt->hCop = hCop;
                    m_pSink->QueueEventOnly(pOnlineEvt);
                }
            }

            PDU_EVENT_ITEM* pExecEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_EXECUTING, nowMs, pCopTag);
            if (pExecEvt != nullptr)
            {
                pExecEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pExecEvt);
            }

            PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, pCopTag);
            if (pFinEvt != nullptr)
            {
                pFinEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pFinEvt);
            }
            m_pSink->NotifyEvents(pCopTag);
        }
        return hCop;
    }

    // 3. Handle asynchronous receive-only listener (0x8004 with empty payload)
    if (copType == PDU_COPT_SENDRECV && (pCopData == nullptr || copDataSize == 0))
    {
        LOGGER.logInfo("GmlanCanAdapter", "Started asynchronous receive-only listener (hCop: %u)", hCop);
        ActiveCop copEntry;
        copEntry.hCop = hCop;
        copEntry.copType = copType;
        copEntry.pCopTag = pCopTag;
        copEntry.status = PDU_COPST_EXECUTING;
        copEntry.reqCanId = 0;
        copEntry.rspCanId = 0;
        copEntry.timestamp = nowMs;

        {
            std::lock_guard<std::mutex> lock(m_adapterMutex);
            m_activeCops[hCop] = copEntry;
        }

        if (m_pSink != nullptr)
        {
            PDU_EVENT_ITEM* pExecEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_EXECUTING, nowMs, pCopTag);
            if (pExecEvt != nullptr)
            {
                pExecEvt->hCop = hCop;
                m_pSink->SignalEvent(pExecEvt);
            }
        }
        return hCop;
    }

    if (pCopData == nullptr || copDataSize < 4)
    {
        LOGGER.logWarn("GmlanCanAdapter", "StartComPrimitive: empty or undersized payload (%u bytes)", copDataSize);
        if (m_pSink != nullptr)
        {
            PDU_EVENT_ITEM* pExecEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_EXECUTING, nowMs, pCopTag);
            if (pExecEvt != nullptr)
            {
                pExecEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pExecEvt);
            }

            PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, pCopTag);
            if (pFinEvt != nullptr)
            {
                pFinEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pFinEvt);
            }
            m_pSink->NotifyEvents(pCopTag);
        }
        return hCop;
    }

    uint32_t canId = (static_cast<uint32_t>(pCopData[0]) << 24) |
                     (static_cast<uint32_t>(pCopData[1]) << 16) |
                     (static_cast<uint32_t>(pCopData[2]) << 8)  |
                     static_cast<uint32_t>(pCopData[3]);

    const uint8_t* pPayload = (copDataSize > 4) ? &pCopData[4] : nullptr;
    uint32_t payloadLen = (copDataSize > 4) ? (copDataSize - 4) : 0;

    // 2. Functional Broadcast TesterPresent (only if diagnostic service is explicitly 0x3E)
    bool isTesterPresentService = false;
    if (pPayload != nullptr && payloadLen > 0)
    {
        for (uint32_t i = 0; i < payloadLen && i < 4; ++i)
        {
            if (pPayload[i] == 0x3E)
            {
                isTesterPresentService = true;
                break;
            }
        }
    }

    if (isTesterPresentService)
    {
        LOGGER.logInfo("GmlanCanAdapter", "Transmitting functional broadcast TesterPresent (CAN ID 0x%03X)", canId);

        uint8_t txFrame[8] = { 0 };
        if (payloadLen <= 7)
        {
            FormatSingleFramePayload(pPayload, payloadLen, txFrame);
        }
        PassThruChannelManager::Instance().TransmitCanFrame(m_busType, canId, txFrame, 8);

        // Immediately finish without fabricating synthetic $7E replies (prevents CANDi ring buffer overflow)
        if (m_pSink != nullptr)
        {
            PDU_EVENT_ITEM* pExecEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_EXECUTING, nowMs, pCopTag);
            if (pExecEvt != nullptr)
            {
                pExecEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pExecEvt);
            }

            PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, pCopTag);
            if (pFinEvt != nullptr)
            {
                pFinEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pFinEvt);
            }
            m_pSink->NotifyEvents(pCopTag);
        }
        return hCop;
    }

    // 3. Standard Diagnostic Request
    uint32_t expectedRspId = GetExpectedRspCanId(canId);
    LOGGER.logInfo("GmlanCanAdapter", "Transmitting diagnostic request (CAN ID 0x%03X, expected rsp 0x%03X, len %u)",
                   canId, expectedRspId, payloadLen);

    ActiveCop copEntry;
    copEntry.hCop = hCop;
    copEntry.copType = copType;
    copEntry.pCopTag = pCopTag;
    copEntry.status = PDU_COPST_EXECUTING;
    copEntry.reqCanId = canId;
    copEntry.rspCanId = expectedRspId;
    copEntry.timestamp = nowMs;

    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        m_activeCops[hCop] = copEntry;
    }

    if (m_pSink != nullptr)
    {
        PDU_EVENT_ITEM* pExecEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_EXECUTING, nowMs, pCopTag);
        if (pExecEvt != nullptr)
        {
            pExecEvt->hCop = hCop;
            m_pSink->SignalEvent(pExecEvt);
        }
    }

    // Format CAN frame for transmission
    if (payloadLen <= 7)
    {
        uint8_t txFrame[8] = { 0 };
        FormatSingleFramePayload(pPayload, payloadLen, txFrame);

        long txStatus = PassThruChannelManager::Instance().TransmitCanFrame(m_busType, canId, txFrame, 8);
        if (txStatus != STATUS_NOERROR)
        {
            LOGGER.logWarn("GmlanCanAdapter", "TransmitCanFrame failed (%ld) for 0x%03X, completing CoP %u early",
                           txStatus, canId, hCop);
            {
                std::lock_guard<std::mutex> lock(m_adapterMutex);
                m_activeCops.erase(hCop);
            }
            if (m_pSink != nullptr)
            {
                PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, pCopTag);
                if (pFinEvt != nullptr)
                {
                    pFinEvt->hCop = hCop;
                    m_pSink->SignalEvent(pFinEvt);
                }
            }
        }
    }
    else
    {
        // Multi-frame transmission (First Frame)
        uint8_t txFrame[8] = { 0 };
        TxMultiFrame txState;
        txState.targetCanId = canId;
        txState.nextSeqNum = 1;
        txState.awaitingFlowControl = true;
        txState.timestamp = nowMs;

        if (pPayload != nullptr && pPayload[0] == 0xFE)
        {
            uint32_t diagLen = payloadLen - 1;
            txFrame[0] = 0xFE;
            txFrame[1] = 0x10 | static_cast<uint8_t>((diagLen >> 8) & 0x0F);
            txFrame[2] = static_cast<uint8_t>(diagLen & 0xFF);
            std::memcpy(&txFrame[3], &pPayload[1], 5);
            txState.remainingData.assign(pPayload + 6, pPayload + payloadLen);
        }
        else
        {
            txFrame[0] = 0x10 | static_cast<uint8_t>((payloadLen >> 8) & 0x0F);
            txFrame[1] = static_cast<uint8_t>(payloadLen & 0xFF);
            if (pPayload != nullptr)
            {
                std::memcpy(&txFrame[2], pPayload, 6);
                txState.remainingData.assign(pPayload + 6, pPayload + payloadLen);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_adapterMutex);
            m_txMultiFrames[canId] = txState;
        }

        long txStatus = PassThruChannelManager::Instance().TransmitCanFrame(m_busType, canId, txFrame, 8);
        if (txStatus != STATUS_NOERROR)
        {
            LOGGER.logWarn("GmlanCanAdapter", "TransmitCanFrame First Frame failed (%ld) to 0x%03X, completing CoP %u early",
                           txStatus, canId, hCop);
            {
                std::lock_guard<std::mutex> lock(m_adapterMutex);
                m_txMultiFrames.erase(canId);
                m_activeCops.erase(hCop);
            }
            if (m_pSink != nullptr)
            {
                PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, pCopTag);
                if (pFinEvt != nullptr)
                {
                    pFinEvt->hCop = hCop;
                    m_pSink->SignalEvent(pFinEvt);
                }
            }
        }
        else
        {
            LOGGER.logInfo("GmlanCanAdapter", "Transmitted First Frame to 0x%03X (totalLen: %u), awaiting Flow Control",
                           canId, payloadLen);
        }
    }

    return hCop;
}

T_PDU_ERROR GmlanCanAdapter::Cancel(uint32_t hCop)
{
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    auto it = m_activeCops.find(hCop);
    if (it != m_activeCops.end())
    {
        it->second.status = PDU_COPST_CANCELLED;
        void* pTag = it->second.pCopTag;
        m_activeCops.erase(it);

        if (m_pSink != nullptr)
        {
            PDU_EVENT_ITEM* pEvt = SaabExt_CreateTech2WinStatusEvent(
                PDU_COPST_CANCELLED,
                static_cast<uint32_t>(GetTickCount()),
                pTag);
            if (pEvt != nullptr)
            {
                pEvt->hCop = hCop;
                m_pSink->QueueEventOnly(pEvt);
                m_pSink->NotifyEvents(pTag);
            }
        }
        return PDU_STATUS_NOERROR;
    }
    return PDU_ERR_INVALID_HANDLE;
}

T_PDU_ERROR GmlanCanAdapter::GetStatus(uint32_t hCop, T_PDU_STATUS& status)
{
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    auto it = m_activeCops.find(hCop);
    if (it != m_activeCops.end())
    {
        status = it->second.status;
        return PDU_STATUS_NOERROR;
    }
    status = PDU_COPST_FINISHED;
    return PDU_STATUS_NOERROR;
}

void GmlanCanAdapter::OnCanFrameReceived(uint32_t canId, const uint8_t* pData, uint32_t length, SaabBusType busType)
{
    if (m_pSink == nullptr || pData == nullptr || length == 0)
    {
        return;
    }

    // 0. Strict Bus Isolation: ignore frames arriving from another physical bus
    if (busType != m_busType)
    {
        return;
    }

    // 1. Diagnostic Response Whitelist & Anti-Noise Filtering:
    // On HS-CAN (500 kbps, pins 6/14): Diagnostic responses include ISO 15765-4 (0x7E8..0x7EF)
    // and GMLAN USDT modules (0x600..0x6FF: CIM 0x645, TCM 0x647, ESP 0x643, EHPS 0x644, AHL 0x653, etc.).
    // All frames outside this range are regular vehicle broadcast traffic (engine RPM, wheel speeds, etc.)
    // that must NEVER be passed to Tech2Win.
    if (m_busType == SaabBusType::BUS_HSCAN)
    {
        bool isHsDiagRsp = ((canId >= 0x600 && canId <= 0x6FF) || (canId >= 0x7E8 && canId <= 0x7EF));
        if (!isHsDiagRsp)
        {
            return;
        }
    }
    // On SW-CAN (33.333 kbps, pin 1): Diagnostic responses are 0x500..0x5FF (UUDT) or 0x600..0x6FF (USDT).
    else if (m_busType == SaabBusType::BUS_SWCAN)
    {
        if (canId < 0x500 || canId > 0x6FF || (length >= 2 && pData[0] == 0x01 && pData[1] == 0x3E))
        {
            return;
        }
    }
    else
    {
        return;
    }

    // 2. Check for Flow Control frame from ECU during multi-frame transmission (0x30..0x32)
    // Anti-collision guard: only process as Flow Control if an active multi-frame TX is actively waiting for it!
    if ((pData[0] & 0xF0) == 0x30 && length >= 3)
    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        uint32_t reqId = GetReqCanIdFromRsp(canId);
        auto itTx = m_txMultiFrames.find(reqId);
        if (itTx != m_txMultiFrames.end() && itTx->second.awaitingFlowControl)
        {
            HandleFlowControl(canId, pData, length);
            return;
        }
    }

    // 3. Unsegmented UUDT Telemetry Stream (SW-CAN only: 0x500 to 0x5FF, including DPID 0x541)
    if (m_busType == SaabBusType::BUS_SWCAN && canId >= 0x500 && canId <= 0x5FF)
    {
        HandleUudtStream(canId, pData, length);
        return;
    }

    // 4. ISO-TP Segmented USDT Communications
    uint8_t pciType = pData[0] & 0xF0;
    switch (pciType)
    {
        case 0x00: // Single Frame (0x0N)
            HandleSingleFrame(canId, pData, length);
            break;

        case 0x10: // First Frame (0x1X)
            HandleFirstFrame(canId, pData, length);
            break;

        case 0x20: // Consecutive Frame (0x2X)
            HandleConsecutiveFrame(canId, pData, length);
            break;

        default:
            // Non-ISO-TP raw frame: fallback deliver as raw Single Frame
            HandleSingleFrame(canId, pData, length);
            break;
    }
}

void GmlanCanAdapter::HandleFlowControl(uint32_t canId, const uint8_t* pData, uint32_t length)
{
    uint32_t reqId = GetReqCanIdFromRsp(canId);
    auto itTx = m_txMultiFrames.find(reqId);
    if (itTx == m_txMultiFrames.end())
    {
        return;
    }

    TxMultiFrame txState = itTx->second;
    uint8_t flowStatus = pData[0] & 0x0F;
    if (flowStatus != 0) // 0 = CTS (Clear to Send)
    {
        LOGGER.logWarn("GmlanCanAdapter", "Flow Control from 0x%03X returned status %u (not CTS)", canId, flowStatus);
        if (flowStatus == 2) // Overflow / Abort
        {
            m_txMultiFrames.erase(itTx);
        }
        return;
    }

    uint8_t blockSize = (length > 1) ? pData[1] : 0;
    uint8_t stMin = (length > 2) ? pData[2] : 0;

    LOGGER.logInfo("GmlanCanAdapter", "Received Flow Control CTS from 0x%03X (BS: %u, STmin: %u), transmitting Consecutive Frames",
                   canId, blockSize, stMin);

    size_t offset = 0;
    while (offset < txState.remainingData.size())
    {
        size_t chunk = txState.remainingData.size() - offset;
        if (chunk > 7)
        {
            chunk = 7;
        }

        uint8_t cfFrame[8] = { 0 };
        cfFrame[0] = 0x20 | (txState.nextSeqNum & 0x0F);
        std::memcpy(&cfFrame[1], &txState.remainingData[offset], chunk);

        PassThruChannelManager::Instance().TransmitCanFrame(m_busType, txState.targetCanId, cfFrame, 8);
        txState.nextSeqNum = (txState.nextSeqNum + 1) & 0x0F;
        offset += chunk;

        if (stMin > 0)
        {
            Sleep(stMin <= 100 ? stMin : 1);
        }
    }

    m_txMultiFrames.erase(reqId);
    LOGGER.logInfo("GmlanCanAdapter", "Finished transmitting Consecutive Frames to 0x%03X", reqId);
}

void GmlanCanAdapter::HandleUudtStream(uint32_t canId, const uint8_t* pData, uint32_t length)
{
    uint32_t matchedCopHandle = 0;
    void* matchedCopTag = m_lastCopTag;
    uint32_t nowMs = static_cast<uint32_t>(GetTickCount());

    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        for (auto& pair : m_activeCops)
        {
            if (pair.second.rspCanId == canId || pair.second.reqCanId == canId)
            {
                matchedCopHandle = pair.first;
                matchedCopTag = pair.second.pCopTag;
                break;
            }
        }
        if (matchedCopHandle == 0)
        {
            for (auto& pair : m_activeCops)
            {
                if (pair.second.status == PDU_COPST_EXECUTING &&
                    pair.second.reqCanId == 0 && pair.second.rspCanId == 0)
                {
                    matchedCopHandle = pair.first;
                    matchedCopTag = pair.second.pCopTag;
                    break;
                }
            }
        }
    }

    if (matchedCopHandle == 0)
    {
        return;
    }

    bool isSwCan = (m_busType == SaabBusType::BUS_SWCAN);

    // Keep all raw bytes unaltered (typically 8 bytes), DataLength = 0, no bit 0x80 in CAN ID header
    PDU_EVENT_ITEM* pResultEvt = SaabExt_CreateTech2WinResultEvent(
        matchedCopHandle,
        matchedCopTag,
        nowMs,
        0,              // DataLength = 0 for UUDT (CANDi opcode 0x29)
        canId,
        pData,          // Raw unaltered bytes
        length,
        isSwCan);

    if (pResultEvt != nullptr)
    {
        m_pSink->QueueEventOnly(pResultEvt);
        m_pSink->NotifyEvents(matchedCopTag);
    }
}

void GmlanCanAdapter::HandleSingleFrame(uint32_t canId, const uint8_t* pData, uint32_t length)
{
    uint8_t sfLen = pData[0] & 0x0F;
    const uint8_t* pUsefulPayload = &pData[1];
    uint32_t usefulLength = (sfLen > 0 && sfLen <= (length - 1)) ? sfLen : (length - 1);

    // NRC 0x78 (Response Pending) detection (ISO 14229 / ISO 15765-4 / GMLAN):
    // When the ECU needs more time to fetch EEPROM data (e.g. VIN $90, Base Model $CC), it returns 7F <SID> 78.
    // The active CoP MUST remain in PDU_COPST_EXECUTING to await the subsequent response (Single or Multi-Frame),
    // and must NOT deliver 7F .. 78 as final result or send PDU_COPST_FINISHED to Tech2Win!
    if (usefulLength >= 3 && pUsefulPayload[0] == 0x7F && pUsefulPayload[2] == 0x78)
    {
        LOGGER.logInfo("GmlanCanAdapter", "Received NRC 0x78 (Response Pending) from 0x%03X for service 0x%02X - keeping CoP active and awaiting final response",
                       canId, pUsefulPayload[1]);
        return;
    }

    uint32_t matchedCopHandle = 0;
    void* matchedCopTag = m_lastCopTag;
    uint32_t nowMs = static_cast<uint32_t>(GetTickCount());
    bool isListener = false;

    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        // 1. Prioritize active CoP explicitly expecting this response ID
        uint32_t reqId = GetReqCanIdFromRsp(canId);
        for (auto& pair : m_activeCops)
        {
            if (pair.second.status == PDU_COPST_EXECUTING)
            {
                bool match = false;
                if (pair.second.rspCanId == canId)
                {
                    match = true;
                }
                else if (pair.second.reqCanId != 0 && pair.second.reqCanId == reqId)
                {
                    match = true;
                }
                else if (pair.second.reqCanId == 0x101 || pair.second.reqCanId == 0x7DF)
                {
                    if (m_busType == SaabBusType::BUS_HSCAN && ((canId >= 0x600 && canId <= 0x6FF) || (canId >= 0x7E8 && canId <= 0x7EF)))
                    {
                        match = true;
                    }
                    else if (m_busType == SaabBusType::BUS_SWCAN && canId >= 0x600 && canId <= 0x6FF)
                    {
                        match = true;
                    }
                }

                if (match)
                {
                    matchedCopHandle = pair.first;
                    matchedCopTag = pair.second.pCopTag;
                    pair.second.status = PDU_COPST_FINISHED;
                    break;
                }
            }
        }

        // 2. Fallback to passive listener (reqCanId == 0 && rspCanId == 0) if present
        if (matchedCopHandle == 0)
        {
            // Only deliver to passive listener if the CAN ID is a valid diagnostic response (USDT)
            // On Saab 9-3 NG, both 0x600..0x6FF and 0x7E8..0x7EF can appear on either bus
            // while ambient periodic traffic is < 0x500.
            bool isDiagRsp = ((canId >= 0x600 && canId <= 0x6FF) || (canId >= 0x7E8 && canId <= 0x7EF));

            if (isDiagRsp)
            {
                for (auto& pair : m_activeCops)
                {
                    if (pair.second.status == PDU_COPST_EXECUTING &&
                        pair.second.reqCanId == 0 && pair.second.rspCanId == 0)
                    {
                        matchedCopHandle = pair.first;
                        matchedCopTag = pair.second.pCopTag;
                        isListener = true;
                        // Passive listener remains EXECUTING
                        break;
                    }
                }
            }
        }
    }

    // Do NOT deliver result to an adapter that has no matching CoP
    if (matchedCopHandle == 0)
    {
        return;
    }

    bool isSwCan = (m_busType == SaabBusType::BUS_SWCAN);

    // Dispatch PDU_IT_RESULT (TECH2WIN_PDU_RESULT_DATA)
    PDU_EVENT_ITEM* pResultEvt = SaabExt_CreateTech2WinResultEvent(
        matchedCopHandle,
        matchedCopTag,
        nowMs,
        static_cast<uint8_t>(usefulLength),
        canId,
        pUsefulPayload,
        usefulLength,
        isSwCan);

    if (pResultEvt != nullptr)
    {
        char hexBuf[128] = { 0 };
        for (uint32_t i = 0; i < usefulLength && i < 24; ++i)
        {
            snprintf(hexBuf + strlen(hexBuf), sizeof(hexBuf) - strlen(hexBuf), "%02X ", pUsefulPayload[i]);
        }
        LOGGER.logInfo("GmlanCanAdapter", "Delivered Single Frame result to Tech2Win (CAN ID 0x%03X, len %u, hCop %u, payload: %s)",
                       canId, usefulLength, matchedCopHandle, hexBuf);
        m_pSink->QueueEventOnly(pResultEvt);
    }

    // Dispatch PDU_COPST_FINISHED status event only for targeted request (not passive listener)
    if (!isListener)
    {
        PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(PDU_COPST_FINISHED, nowMs, matchedCopTag);
        if (pFinEvt != nullptr)
        {
            pFinEvt->hCop = matchedCopHandle;
            m_pSink->QueueEventOnly(pFinEvt);
        }
    }
    m_pSink->NotifyEvents(matchedCopTag);
}

void GmlanCanAdapter::HandleFirstFrame(uint32_t canId, const uint8_t* pData, uint32_t length)
{
    if (length < 3)
    {
        return;
    }

    uint32_t totalLen = ((static_cast<uint32_t>(pData[0] & 0x0F) << 8) | pData[1]);
    uint32_t firstChunkLen = (length > 2) ? (length - 2) : 0;
    uint32_t nowMs = static_cast<uint32_t>(GetTickCount());

    uint32_t matchedCopHandle = 0;
    void* matchedCopTag = m_lastCopTag;
    uint32_t reqCanId = GetReqCanIdFromRsp(canId);

    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        for (auto& pair : m_activeCops)
        {
            if (pair.second.status == PDU_COPST_EXECUTING)
            {
                bool match = false;
                if (pair.second.rspCanId == canId)
                {
                    match = true;
                }
                else if (pair.second.reqCanId != 0 && pair.second.reqCanId == reqCanId)
                {
                    match = true;
                }
                else if (pair.second.reqCanId == 0x101 || pair.second.reqCanId == 0x7DF)
                {
                    if (m_busType == SaabBusType::BUS_HSCAN && ((canId >= 0x600 && canId <= 0x6FF) || (canId >= 0x7E8 && canId <= 0x7EF)))
                    {
                        match = true;
                    }
                    else if (m_busType == SaabBusType::BUS_SWCAN && canId >= 0x600 && canId <= 0x6FF)
                    {
                        match = true;
                    }
                }

                if (match)
                {
                    matchedCopHandle = pair.first;
                    matchedCopTag = pair.second.pCopTag;
                    break;
                }
            }
        }

        if (matchedCopHandle == 0)
        {
            // Frame not intended for this adapter
            LOGGER.logWarn("GmlanCanAdapter", "First Frame from 0x%03X (totalLen: %u) has no active executing CoP (reqCanId: 0x%03X)",
                           canId, totalLen, reqCanId);
            return;
        }

        RxAssembly assembly;
        assembly.totalLength = totalLen;
        assembly.assembledData.assign(&pData[2], &pData[2 + firstChunkLen]);
        assembly.nextSeqNum = 1;
        assembly.timestamp = nowMs;
        assembly.reqCanId = reqCanId;
        assembly.hCop = matchedCopHandle;
        assembly.pCopTag = matchedCopTag;

        m_rxAssemblies[canId] = assembly;
    }

    LOGGER.logInfo("GmlanCanAdapter", "Received First Frame from 0x%03X (totalLen: %u), transmitting Flow Control CTS to 0x%03X",
                   canId, totalLen, reqCanId);

    // Send Flow Control (CTS: 0x30 0x00 0x00 ...)
    uint8_t fcFrame[8] = { 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    PassThruChannelManager::Instance().TransmitCanFrame(m_busType, reqCanId, fcFrame, 8);
}

void GmlanCanAdapter::HandleConsecutiveFrame(uint32_t canId, const uint8_t* pData, uint32_t length)
{
    if (length < 2)
    {
        return;
    }

    RxAssembly completedAssembly;
    bool isComplete = false;
    uint32_t nowMs = static_cast<uint32_t>(GetTickCount());

    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        auto it = m_rxAssemblies.find(canId);
        if (it == m_rxAssemblies.end())
        {
            LOGGER.logWarn("GmlanCanAdapter", "Consecutive Frame from 0x%03X has no active assembly", canId);
            return;
        }

        RxAssembly& assembly = it->second;
        uint8_t seqNum = pData[0] & 0x0F;

        if (seqNum != (assembly.nextSeqNum & 0x0F))
        {
            LOGGER.logWarn("GmlanCanAdapter", "Sequence number mismatch from 0x%03X (expected %u, got %u)",
                           canId, assembly.nextSeqNum & 0x0F, seqNum);
        }

        uint32_t chunkLen = length - 1;
        uint32_t needed = (assembly.totalLength > assembly.assembledData.size()) ?
                          (assembly.totalLength - assembly.assembledData.size()) : 0;
        if (chunkLen > needed)
        {
            chunkLen = needed;
        }

        assembly.assembledData.insert(assembly.assembledData.end(), &pData[1], &pData[1 + chunkLen]);
        assembly.nextSeqNum = (assembly.nextSeqNum + 1) & 0x0F;

        if (assembly.assembledData.size() >= assembly.totalLength)
        {
            completedAssembly = assembly;
            isComplete = true;
            m_rxAssemblies.erase(it);

            // Mark active CoP finished
            auto itCop = m_activeCops.find(completedAssembly.hCop);
            if (itCop != m_activeCops.end())
            {
                itCop->second.status = PDU_COPST_FINISHED;
            }
        }
    }

    if (isComplete)
    {
        char hexBuf[128] = { 0 };
        for (size_t i = 0; i < completedAssembly.assembledData.size() && i < 24; ++i)
        {
            snprintf(hexBuf + strlen(hexBuf), sizeof(hexBuf) - strlen(hexBuf), "%02X ", completedAssembly.assembledData[i]);
        }
        LOGGER.logInfo("GmlanCanAdapter", "Multi-frame reassembly complete for 0x%03X (size: %u, payload: %s)",
                       canId, static_cast<unsigned int>(completedAssembly.assembledData.size()), hexBuf);

        bool isSwCan = (m_busType == SaabBusType::BUS_SWCAN);
        uint8_t resDataLen = (completedAssembly.totalLength <= 0xFF) ?
                             static_cast<uint8_t>(completedAssembly.totalLength) : 0xFF;

        PDU_EVENT_ITEM* pResultEvt = SaabExt_CreateTech2WinResultEvent(
            completedAssembly.hCop,
            completedAssembly.pCopTag,
            nowMs,
            resDataLen,
            canId,
            completedAssembly.assembledData.data(),
            completedAssembly.assembledData.size(),
            isSwCan);

        if (pResultEvt != nullptr)
        {
            m_pSink->QueueEventOnly(pResultEvt);
        }

        PDU_EVENT_ITEM* pFinEvt = SaabExt_CreateTech2WinStatusEvent(
            PDU_COPST_FINISHED,
            nowMs,
            completedAssembly.pCopTag);
        if (pFinEvt != nullptr)
        {
            pFinEvt->hCop = completedAssembly.hCop;
            m_pSink->QueueEventOnly(pFinEvt);
        }
        m_pSink->NotifyEvents(completedAssembly.pCopTag);
    }
}
