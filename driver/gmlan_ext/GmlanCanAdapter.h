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

#ifndef GMLAN_EXT_GMLAN_CAN_ADAPTER_H
#define GMLAN_EXT_GMLAN_CAN_ADAPTER_H

#include "IGmlanAdapter.h"
#include "PassThruChannelManager.h"
#include "Tech2WinMemory.h"
#include <mutex>
#include <atomic>
#include <vector>
#include <map>
#include <cstdint>

class GmlanCanAdapter : public IGmlanAdapter {
public:
    explicit GmlanCanAdapter(SaabBusType busType);
    virtual ~GmlanCanAdapter();

    // IGmlanAdapter implementation
    long Connect(unsigned long deviceId, ILogicalLinkEventSink* pSink) override;
    long Disconnect() override;

    uint32_t StartComPrimitive(
        uint32_t copType,
        uint32_t copDataSize,
        uint8_t* pCopData,
        PDU_COP_CTRL_DATA* pCopCtrlData,
        void* pCopTag) override;

    T_PDU_ERROR Cancel(uint32_t hCop) override;
    T_PDU_ERROR GetStatus(uint32_t hCop, T_PDU_STATUS& status) override;
    SaabBusType GetBusType() const override { return m_busType; }

    // ICllFrameReceiver implementation
    void OnCanFrameReceived(uint32_t canId, const uint8_t* pData, uint32_t length, SaabBusType busType) override;

private:
    struct ActiveCop {
        uint32_t hCop = 0;
        uint32_t copType = 0;
        void* pCopTag = nullptr;
        T_PDU_STATUS status = PDU_COPST_IDLE;
        uint32_t reqCanId = 0;
        uint32_t rspCanId = 0;
        uint32_t timestamp = 0;
    };

    struct RxAssembly {
        uint32_t totalLength = 0;
        std::vector<uint8_t> assembledData;
        uint8_t nextSeqNum = 1;
        uint32_t timestamp = 0;
        uint32_t reqCanId = 0;
        uint32_t hCop = 0;
        void* pCopTag = nullptr;
    };

    struct TxMultiFrame {
        uint32_t targetCanId = 0;
        std::vector<uint8_t> remainingData;
        uint8_t nextSeqNum = 1;
        bool awaitingFlowControl = false;
        uint32_t timestamp = 0;
    };

    void HandleFlowControl(uint32_t canId, const uint8_t* pData, uint32_t length);
    void HandleUudtStream(uint32_t canId, const uint8_t* pData, uint32_t length);
    void HandleSingleFrame(uint32_t canId, const uint8_t* pData, uint32_t length);
    void HandleFirstFrame(uint32_t canId, const uint8_t* pData, uint32_t length);
    void HandleConsecutiveFrame(uint32_t canId, const uint8_t* pData, uint32_t length);

    SaabBusType m_busType;
    unsigned long m_deviceId;
    unsigned long m_channelId;
    ILogicalLinkEventSink* m_pSink;

    std::mutex m_adapterMutex;
    std::atomic<uint32_t> m_nextCopHandle{ 1 };
    std::map<uint32_t, ActiveCop> m_activeCops;
    std::map<uint32_t, RxAssembly> m_rxAssemblies;
    std::map<uint32_t, TxMultiFrame> m_txMultiFrames;
    void* m_lastCopTag;
};

#endif // GMLAN_EXT_GMLAN_CAN_ADAPTER_H
