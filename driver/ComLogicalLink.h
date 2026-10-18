#pragma once

#include "pdu_api.h"
#include "ComPrimitive.h"
#include "gmlan_ext/IGmlanAdapter.h"

#include <thread>
#include <queue>
#include <mutex>
#include <memory>
#include <vector>

enum Protocol
{
	CLL_ISO14230 = 0x01,
	CLL_KW82 = 0x02,
	CLL_GMLAN_CAN = 0x03,
	CLL_SAAB_CAN = 0x03 // Backward-compatibility alias
};

class ComLogicalLink : public ILogicalLinkEventSink
{
public:

	ComLogicalLink(UNUM32 hMod, UNUM32 hCLL, unsigned long deviceID, enum Protocol protocol);

	long Connect();
	long Disconnect();

	T_PDU_ERROR GetStatus(T_PDU_STATUS& status);
	T_PDU_ERROR GetStatus(UNUM32 hCoP, T_PDU_STATUS& status);
	T_PDU_ERROR Cancel(UNUM32 hCoP);

	UNUM32 StartComPrimitive(UNUM32 CoPType, UNUM32 CoPDataSize, UNUM8* pCoPData, PDU_COP_CTRL_DATA* pCopCtrlData, void* pCoPTag);
	long StartMsgFilter(unsigned long filterType);

	void RegisterEventCallback(CALLBACKFNC cb);
	CALLBACKFNC GetEventCallback() const { return m_eventCallbackFnc; }
	bool GetEvent(PDU_EVENT_ITEM* & pEvt);
	void SignalEvent(PDU_EVENT_ITEM* pEvt) override;
	void QueueEventOnly(PDU_EVENT_ITEM* pEvt) override { QueueEvent(pEvt); }
	void NotifyEvents(void* pCopTag = nullptr) override { SignalEvents(pCopTag); }

	unsigned long GetModuleHandle() const override { return m_hMod; }
	unsigned long GetLinkHandle() const override { return m_hCLL; }

	void SetCllTag(void* tag) { m_pCllTag = tag; }
	void* GetCllTag() const override { return m_pCllTag; }

	void SetGmlanAdapter(std::shared_ptr<IGmlanAdapter> adapter) { m_pGmlanAdapter = adapter; }
	std::shared_ptr<IGmlanAdapter> GetGmlanAdapter() const { return m_pGmlanAdapter; }
	void SetSaabAdapter(std::shared_ptr<IGmlanAdapter> adapter) { SetGmlanAdapter(adapter); }
	std::shared_ptr<IGmlanAdapter> GetSaabAdapter() const { return GetGmlanAdapter(); }

	long SetComParam();

	void QueueEvent(PDU_EVENT_ITEM* pEvt);
	void SignalEvents(void* pCopTag = nullptr);

private:
	void run();

	void ProcessCop(std::shared_ptr<ComPrimitive> cop);
	long StartComm(std::shared_ptr<ComPrimitive> cop);
	long StopComm(std::shared_ptr<ComPrimitive> cop);
	long SendRecv(std::shared_ptr<ComPrimitive> cop);

	CALLBACKFNC m_eventCallbackFnc;

	bool m_running;
	std::jthread m_runLoop;

	std::mutex m_eventLock;
	std::queue<PDU_EVENT_ITEM*> m_eventQueue;

	std::mutex m_copLock;
	std::vector<std::shared_ptr<ComPrimitive>> m_copQueue;

	UNUM32 m_hMod;
	UNUM32 m_hCLL;
	void* m_pCllTag = nullptr;
	T_PDU_STATUS m_status;

	Protocol m_protocol;

	// GMLAN custom protocol adapter
	std::shared_ptr<IGmlanAdapter> m_pGmlanAdapter;

	//J2534 specific
	unsigned long m_deviceID;
	unsigned long m_protocolID;
	unsigned long m_channelID;
};

