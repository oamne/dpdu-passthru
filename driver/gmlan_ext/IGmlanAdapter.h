/**
 * GMLAN Protocol Adapter Interface
 * Polymorphic contract for vehicle-specific diagnostic protocols (GMLAN SW-CAN / HS-CAN).
 *
 * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
 * Inspired by the Saab community - saabiste.net
 *
 * Source: https://github.com/oamne/dpdu-passthru
 */

#ifndef GMLAN_EXT_I_GMLAN_ADAPTER_H
#define GMLAN_EXT_I_GMLAN_ADAPTER_H

#include <cstdint>
#include "PassThruChannelManager.h"
#include "pdu_api.h"

/**
 * Interface through which protocol adapters interact with their parent ComLogicalLink.
 */
class ILogicalLinkEventSink {
public:
    virtual ~ILogicalLinkEventSink() = default;
    virtual void SignalEvent(PDU_EVENT_ITEM* pEvt) = 0;
    virtual void QueueEventOnly(PDU_EVENT_ITEM* pEvt) = 0;
    virtual void NotifyEvents(void* pCopTag = nullptr) = 0;
    virtual unsigned long GetModuleHandle() const = 0;
    virtual unsigned long GetLinkHandle() const = 0;
    virtual void* GetCllTag() const = 0;
};

/**
 * Interface implemented by custom protocol adapters (GmlanCanAdapter).
 */
class IGmlanAdapter : public ICllFrameReceiver {
public:
    virtual ~IGmlanAdapter() = default;

    /**
     * Initializes hardware connection and registers with PassThruChannelManager.
     */
    virtual long Connect(unsigned long deviceId, ILogicalLinkEventSink* pSink) = 0;

    /**
     * Disconnects and releases physical channel reference.
     */
    virtual long Disconnect() = 0;

    /**
     * Executes a D-PDU ComPrimitive (e.g. diagnostic request, tester present, UUDT monitor).
     */
    virtual uint32_t StartComPrimitive(
        uint32_t copType,
        uint32_t copDataSize,
        uint8_t* pCopData,
        PDU_COP_CTRL_DATA* pCopCtrlData,
        void* pCopTag) = 0;

    /**
     * Cancels an ongoing ComPrimitive.
     */
    virtual T_PDU_ERROR Cancel(uint32_t hCop) = 0;

    /**
     * Returns the execution status of a ComPrimitive.
     */
    virtual T_PDU_ERROR GetStatus(uint32_t hCop, T_PDU_STATUS& status) = 0;

    /**
     * Returns the physical bus type used by this adapter.
     */
    virtual SaabBusType GetBusType() const = 0;
};

#endif // GMLAN_EXT_I_GMLAN_ADAPTER_H
