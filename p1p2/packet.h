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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   /* getopt */

/* ---- 共用常數 ---- */
#define MTU 1500            // 最大傳輸單元 (Maximum Transmission Unit)
#define PACKET_SIZE 1518    // 封包總大小上限（含 Ethernet header），router 佇列每格的大小
#define QUEUE_SIZE 100      // 佇列最大容量

/* ---- 可由命令列調整的參數（三支程式必須給相同的值） ----
 *   -n <封包數>   模擬傳送的封包數，預設 23
 *   -p <基準 port> Server 使用此 port，Router = +2、Client = +3、第二個 Client = +4，預設 9000
 * 原本寫死在 #define，改參數要重新編譯；現在改成全域變數，由 parse_args() 在 main 開頭設定。
 */
static int g_loop_count = 23;
static int g_port_base = 9000;

#define LOOP_COUNT g_loop_count                 // 模擬傳送封包的次數

/* ---- 實體 port（全部在本機 127.0.0.1） ---- */
#define SERVER_PORT (g_port_base)               // Server 監聽 TCP / UDP（預設 9000）
#define ROUTER_PORT (g_port_base + 2)           // Router 監聽 TCP / UDP（預設 9002）
#define CLIENT_PORT (g_port_base + 3)           // Client 接收 UDP（預設 9003）
#define CLIENTTWO_PORT (g_port_base + 4)        // 第二個 Client（備用，預設 9004）

/* 解析 -n / -p 參數；給 -h 或不合法的值時印出用法並結束 */
static void parse_args(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "n:p:h")) != -1) {
        switch (opt) {
        case 'n': g_loop_count = atoi(optarg); break;
        case 'p': g_port_base = atoi(optarg); break;
        default:
            fprintf(stderr, "用法: %s [-n 封包數] [-p 基準port]（三支程式要用相同參數）\n", argv[0]);
            exit(opt == 'h' ? 0 : 1);
        }
    }
    if (g_loop_count <= 0 || g_port_base <= 0 || g_port_base > 65531) {
        fprintf(stderr, "參數不合法：封包數需 > 0，基準 port 需在 1~65531\n");
        exit(1);
    }
}

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
