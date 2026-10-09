/*
 * packet.h — client / server / router 共用的常數與封包表頭定義
 *
 * 原本三支程式各自複製一份結構，而且內容不一致（router 的 IPHeader 多一個 options 欄位、
 * 三邊 Packet 的 payload 大小也不同），收發同一個封包時長度對不上。
 * 統一放在這裡後，三支程式看到的封包格式保證相同。C 與 C++ 都可以 include。
 */
#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

/* ---- 共用常數 ---- */
#define MTU 1500            // 最大傳輸單元 (Maximum Transmission Unit)
#define PACKET_SIZE 1518    // 封包總大小上限（含 Ethernet header），router 佇列每格的大小
#define QUEUE_SIZE 100      // 佇列最大容量
#define LOOP_COUNT 23       // 模擬傳送封包的次數

/* ---- 實體 port（全部在本機 127.0.0.1） ---- */
#define SERVER_PORT 9000    // Server 監聽 TCP / UDP
#define ROUTER_PORT 9002    // Router 監聽 TCP / UDP
#define CLIENT_PORT 9003    // Client 接收 UDP
#define CLIENTTWO_PORT 9004 // 第二個 Client（備用）

/* ---- 虛擬 IP：router 依 IP 表頭的目的位址決定轉送對象 ---- */
#define SERVER_VIRTUAL_IP 0x0A115945   // 10.17.89.69
#define CLIENT_VIRTUAL_IP 0x0A000301   // 10.0.3.1
#define CLIENT2_VIRTUAL_IP 0x0A000302  // 10.0.3.2

/* ---- 表頭結構 ---- */
typedef struct IPHeader {           // 20 bytes
    uint8_t version_ihl;
    uint8_t type_of_service;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment_offset;
    uint8_t time_to_live;
    uint8_t protocol;               // 6 = TCP, 17 = UDP
    uint16_t header_checksum;
    uint32_t source_ip;
    uint32_t destination_ip;
} IPHeader;

typedef struct UDPHeader {          // 8 bytes
    uint32_t source_port : 16, dest_port : 16;
    uint32_t Segment_Length : 16, Checksum : 16;
} UDPHeader;

typedef struct MACHeader {          // 6 + 6 + 2 (+2 對齊) + 4 = 20 bytes
    uint8_t sour_mac[6];
    uint8_t des_mac[6];
    uint16_t fram_typ;
    uint32_t crc;
} MACHeader;

typedef struct TCPHeader {          // 20 bytes
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence_number;
    uint32_t ack_number;
    uint16_t offset_reserved_flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_pointer;
} TCPHeader;

/* UDP 封包：三個表頭 48 bytes + payload；整體不能超過 router 佇列每格的 PACKET_SIZE */
#define PAYLOAD_SIZE 1468
typedef struct Packet {
    struct IPHeader ipheader;
    struct UDPHeader udpheader;
    struct MACHeader macheader;
    char buffer[PAYLOAD_SIZE];
} Packet;

/* 編譯時檢查：封包大小超過佇列容量就直接編譯失敗，避免 memcpy 寫出陣列範圍 */
#ifdef __cplusplus
static_assert(sizeof(Packet) <= PACKET_SIZE, "Packet larger than PACKET_SIZE");
#else
_Static_assert(sizeof(Packet) <= PACKET_SIZE, "Packet larger than PACKET_SIZE");
#endif

#endif /* PACKET_H */
