/**
 * Tech2Win Memory Manager
 * Emulates the 44-byte internal Tech2Win PDU_RESULT_DATA structure for CANDi / Tech2Win emulator.exe.
 *
 * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
 * Inspired by the Saab community - saabiste.net
 *
 * Source: https://github.com/oamne/dpdu-passthru
 */

#ifndef GMLAN_EXT_TECH2WIN_MEMORY_H
#define GMLAN_EXT_TECH2WIN_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "pdu_api.h"

#define TECH2WIN_MAGIC_MARKER 0x53414142U // ASCII "SAAB"

// Global dummy buffer ensuring pCopTag / pCllTag is never NULL when dereferenced by emulator.exe
extern uint8_t s_dummyCopFlags[16];

#pragma pack(push, 1)
/**
 * Tech2Win internal PDU_RESULT_DATA structure: exactly 44 bytes on Win32 32-bit.
 * Matches Tech2Win emulator.exe internal parser at address 0x00464750.
 */
typedef struct {
    uint32_t       RxFlag;        // Offset 0  (0x00): Magic marker (0x53414142 = "SAAB")
    void*          pCopTag;       // Offset 4  (0x04): ComPrimitive tag (must NEVER be NULL, dereferenced at 0x0046475a)
    uint32_t       Timestamp;     // Offset 8  (0x08): Millisecond timestamp
    uint8_t        DataLength;    // Offset 12 (0x0C): CANDi frame payload length (copied directly to packet[1] by emulator.exe)
    uint8_t        Reserved[23];  // Offset 13..35: Reserved zero-padding
    uint32_t       NumDataBytes;  // Offset 36 (0x24): Total size in bytes (4 bytes CAN ID + payload data)
    uint8_t*       pDataBytes;    // Offset 40 (0x28): Pointer to heap buffer [CAN_ID(4) | Payload(N)]
} TECH2WIN_PDU_RESULT_DATA;
#pragma pack(pop)

// Compile-time layout verification
static_assert(sizeof(TECH2WIN_PDU_RESULT_DATA) == 44, "TECH2WIN_PDU_RESULT_DATA size must be exactly 44 bytes");
static_assert(offsetof(TECH2WIN_PDU_RESULT_DATA, RxFlag) == 0, "RxFlag offset must be 0");
static_assert(offsetof(TECH2WIN_PDU_RESULT_DATA, pCopTag) == 4, "pCopTag offset must be 4");
static_assert(offsetof(TECH2WIN_PDU_RESULT_DATA, Timestamp) == 8, "Timestamp offset must be 8");
static_assert(offsetof(TECH2WIN_PDU_RESULT_DATA, DataLength) == 12, "DataLength offset must be 12");
static_assert(offsetof(TECH2WIN_PDU_RESULT_DATA, NumDataBytes) == 36, "NumDataBytes offset must be 36");
static_assert(offsetof(TECH2WIN_PDU_RESULT_DATA, pDataBytes) == 40, "pDataBytes offset must be 40");

/**
 * Checks if the given pointer points to an extension-allocated TECH2WIN_PDU_RESULT_DATA
 * by verifying the 0x53414142 ("SAAB") magic marker.
 */
bool SaabExt_IsTech2WinResult(const void* pData);

/**
 * Safely frees a TECH2WIN_PDU_RESULT_DATA structure and its internal buffer.
 */
void SaabExt_DestroyTech2WinResult(void* pData);

/**
 * Creates and initializes a TECH2WIN_PDU_RESULT_DATA structure.
 */
TECH2WIN_PDU_RESULT_DATA* SaabExt_CreateTech2WinResult(
    void* pCopTag,
    uint32_t timestamp,
    uint8_t dataLength,
    uint32_t canId,
    const uint8_t* payloadData,
    uint32_t payloadLength,
    bool isSwCan);

/**
 * Creates a complete PDU_EVENT_ITEM of type PDU_IT_RESULT carrying a TECH2WIN_PDU_RESULT_DATA payload.
 */
PDU_EVENT_ITEM* SaabExt_CreateTech2WinResultEvent(
    uint32_t hCop,
    void* pCopTag,
    uint32_t timestamp,
    uint8_t dataLength,
    uint32_t canId,
    const uint8_t* payloadData,
    uint32_t payloadLength,
    bool isSwCan);

/**
 * Creates and initializes a raw TECH2WIN_PDU_RESULT_DATA structure without CAN header
 * (for K-Line ISO14230 / KW82 / UART protocols).
 */
TECH2WIN_PDU_RESULT_DATA* SaabExt_CreateRawTech2WinResult(
    void* pCopTag,
    uint32_t timestamp,
    const uint8_t* payloadData,
    uint32_t payloadLength);

/**
 * Creates a complete PDU_EVENT_ITEM carrying a raw TECH2WIN_PDU_RESULT_DATA payload
 * (for K-Line ISO14230 / KW82 / UART protocols).
 */
PDU_EVENT_ITEM* SaabExt_CreateRawTech2WinResultEvent(
    uint32_t hCop,
    void* pCopTag,
    uint32_t timestamp,
    const uint8_t* payloadData,
    uint32_t payloadLength);

/**
 * Creates a complete PDU_EVENT_ITEM of type PDU_IT_STATUS.
 */
PDU_EVENT_ITEM* SaabExt_CreateTech2WinStatusEvent(
    uint32_t statusCode,
    uint32_t timestamp,
    void* pTag);

#endif // GMLAN_EXT_TECH2WIN_MEMORY_H
