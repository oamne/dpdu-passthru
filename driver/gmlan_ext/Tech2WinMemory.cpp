/**
 * Tech2Win Memory Manager
 * Emulates the 44-byte internal Tech2Win PDU_RESULT_DATA structure for CANDi / Tech2Win emulator.exe.
 *
 * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
 * Inspired by the Saab community - saabiste.net
 *
 * Source: https://github.com/oamne/dpdu-passthru
 */

#include "Tech2WinMemory.h"
#include <new>

// Global dummy buffer ensuring pCopTag is never NULL when dereferenced by emulator.exe (0x0046475a)
uint8_t s_dummyCopFlags[16] = { 0 };

bool SaabExt_IsTech2WinResult(const void* pData)
{
    if (pData == nullptr)
    {
        return false;
    }

    const TECH2WIN_PDU_RESULT_DATA* pRes = reinterpret_cast<const TECH2WIN_PDU_RESULT_DATA*>(pData);
    uint32_t magic = *reinterpret_cast<const uint32_t*>(pRes->Reserved);
    return (magic == TECH2WIN_MAGIC_MARKER || pRes->RxFlag == TECH2WIN_MAGIC_MARKER);
}

void SaabExt_DestroyTech2WinResult(void* pData)
{
    if (pData == nullptr)
    {
        return;
    }

    TECH2WIN_PDU_RESULT_DATA* pRes = reinterpret_cast<TECH2WIN_PDU_RESULT_DATA*>(pData);

    if (pRes->pDataBytes != nullptr)
    {
        delete[] pRes->pDataBytes;
        pRes->pDataBytes = nullptr;
    }

    pRes->RxFlag = 0;
    pRes->pCopTag = nullptr;
    pRes->NumDataBytes = 0;

    delete pRes;
}

TECH2WIN_PDU_RESULT_DATA* SaabExt_CreateTech2WinResult(
    void* pCopTag,
    uint32_t timestamp,
    uint8_t dataLength,
    uint32_t canId,
    const uint8_t* payloadData,
    uint32_t payloadLength,
    bool isSwCan)
{
    TECH2WIN_PDU_RESULT_DATA* pRes = new (std::nothrow) TECH2WIN_PDU_RESULT_DATA();
    if (pRes == nullptr)
    {
        return nullptr;
    }

    std::memset(pRes, 0, sizeof(TECH2WIN_PDU_RESULT_DATA));

    uint32_t totalBytes = 4 + payloadLength;
    uint8_t* pBytes = new (std::nothrow) uint8_t[totalBytes];
    if (pBytes == nullptr)
    {
        delete pRes;
        return nullptr;
    }

    // 4-byte CAN ID header (with bit 0x80 applied for all USDT diagnostic responses)
    // Conforming to OpenSAAB-T2 CANDi firmware and stn_bridge: prevents "P bus manquant" and "Modèle/Année incorrecte" under Tech2Win
    bool isUudt = (canId >= 0x500 && canId <= 0x5FF);
    bool applyBit80 = !isUudt; // Bit 0x80 is universal for all USDT responses (0x7E8 -> 87 E8, 0x641 -> 86 41)

    pBytes[0] = static_cast<uint8_t>((canId >> 24) & 0xFF);
    pBytes[1] = static_cast<uint8_t>((canId >> 16) & 0xFF);
    pBytes[2] = static_cast<uint8_t>(((canId >> 8) & 0xFF) | (applyBit80 ? 0x80 : 0x00));
    pBytes[3] = static_cast<uint8_t>(canId & 0xFF);

    if (payloadData != nullptr && payloadLength > 0)
    {
        std::memcpy(pBytes + 4, payloadData, payloadLength);
    }

    pRes->RxFlag = 0;
    *reinterpret_cast<uint32_t*>(pRes->Reserved) = TECH2WIN_MAGIC_MARKER;
    pRes->pCopTag = (pCopTag != nullptr) ? pCopTag : s_dummyCopFlags;
    pRes->Timestamp = timestamp;
    // For UUDT telemetry streams, DataLength MUST be 0 (CANDi opcode 0x29 requirement)
    pRes->DataLength = isUudt ? 0 : dataLength;
    pRes->NumDataBytes = totalBytes;
    pRes->pDataBytes = pBytes;

    return pRes;
}

PDU_EVENT_ITEM* SaabExt_CreateTech2WinResultEvent(
    uint32_t hCop,
    void* pCopTag,
    uint32_t timestamp,
    uint8_t dataLength,
    uint32_t canId,
    const uint8_t* payloadData,
    uint32_t payloadLength,
    bool isSwCan)
{
    TECH2WIN_PDU_RESULT_DATA* pRes = SaabExt_CreateTech2WinResult(
        pCopTag,
        timestamp,
        dataLength,
        canId,
        payloadData,
        payloadLength,
        isSwCan);

    if (pRes == nullptr)
    {
        return nullptr;
    }

    PDU_EVENT_ITEM* pEvt = new (std::nothrow) PDU_EVENT_ITEM();
    if (pEvt == nullptr)
    {
        SaabExt_DestroyTech2WinResult(pRes);
        return nullptr;
    }

    std::memset(pEvt, 0, sizeof(PDU_EVENT_ITEM));
    pEvt->ItemType = PDU_IT_RESULT;
    pEvt->hCop = hCop;
    pEvt->pCoPTag = (pCopTag != nullptr) ? pCopTag : s_dummyCopFlags;
    pEvt->Timestamp = timestamp;
    pEvt->pData = pRes;

    return pEvt;
}

PDU_EVENT_ITEM* SaabExt_CreateTech2WinStatusEvent(
    uint32_t statusCode,
    uint32_t timestamp,
    void* pTag)
{
    PDU_EVENT_ITEM* pEvt = new (std::nothrow) PDU_EVENT_ITEM();
    if (pEvt == nullptr)
    {
        return nullptr;
    }

    PDU_STATUS_DATA* pStatusData = new (std::nothrow) PDU_STATUS_DATA(static_cast<T_PDU_STATUS>(statusCode), timestamp);
    if (pStatusData == nullptr)
    {
        delete pEvt;
        return nullptr;
    }

    std::memset(pEvt, 0, sizeof(PDU_EVENT_ITEM));
    pEvt->ItemType = PDU_IT_STATUS;
    pEvt->hCop = PDU_HANDLE_UNDEF;
    pEvt->pCoPTag = (pTag != nullptr) ? pTag : s_dummyCopFlags;
    pEvt->Timestamp = timestamp;
    pEvt->pData = pStatusData;

    return pEvt;
}

TECH2WIN_PDU_RESULT_DATA* SaabExt_CreateRawTech2WinResult(
    void* pCopTag,
    uint32_t timestamp,
    const uint8_t* payloadData,
    uint32_t payloadLength)
{
    TECH2WIN_PDU_RESULT_DATA* pRes = new (std::nothrow) TECH2WIN_PDU_RESULT_DATA();
    if (pRes == nullptr)
    {
        return nullptr;
    }

    std::memset(pRes, 0, sizeof(TECH2WIN_PDU_RESULT_DATA));

    uint8_t* pBytes = nullptr;
    if (payloadLength > 0)
    {
        pBytes = new (std::nothrow) uint8_t[payloadLength];
        if (pBytes == nullptr)
        {
            delete pRes;
            return nullptr;
        }
        if (payloadData != nullptr)
        {
            std::memcpy(pBytes, payloadData, payloadLength);
        }
    }

    pRes->RxFlag = 0;
    *reinterpret_cast<uint32_t*>(pRes->Reserved) = TECH2WIN_MAGIC_MARKER;
    pRes->pCopTag = (pCopTag != nullptr) ? pCopTag : s_dummyCopFlags;
    pRes->Timestamp = timestamp;
    pRes->DataLength = (payloadLength <= 0xFF) ? static_cast<uint8_t>(payloadLength) : 0xFF;
    pRes->NumDataBytes = payloadLength;
    pRes->pDataBytes = pBytes;

    return pRes;
}

PDU_EVENT_ITEM* SaabExt_CreateRawTech2WinResultEvent(
    uint32_t hCop,
    void* pCopTag,
    uint32_t timestamp,
    const uint8_t* payloadData,
    uint32_t payloadLength)
{
    TECH2WIN_PDU_RESULT_DATA* pRes = SaabExt_CreateRawTech2WinResult(
        pCopTag,
        timestamp,
        payloadData,
        payloadLength);

    if (pRes == nullptr)
    {
        return nullptr;
    }

    PDU_EVENT_ITEM* pEvt = new (std::nothrow) PDU_EVENT_ITEM();
    if (pEvt == nullptr)
    {
        SaabExt_DestroyTech2WinResult(pRes);
        return nullptr;
    }

    std::memset(pEvt, 0, sizeof(PDU_EVENT_ITEM));
    pEvt->ItemType = PDU_IT_RESULT;
    pEvt->hCop = hCop;
    pEvt->pCoPTag = (pCopTag != nullptr) ? pCopTag : s_dummyCopFlags;
    pEvt->Timestamp = timestamp;
    pEvt->pData = pRes;

    return pEvt;
}

