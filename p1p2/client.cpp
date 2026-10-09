#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>  
#include <iostream>
#include <sys/time.h> 

using namespace std;

#define MTU 1500
#define PACKET_SIZE 1518
#define BUFF_LEN 10000 
#define LOOP_COUNT 23

#define SERVER_IP "127.0.0.1"
#define ROUTER_PORT 9002 // 連線目標是 Router
#define CLIENT_PORT 9003 // 接收 UDP 的 Port
#define QUEUE_SIZE 100

// --- 用於計算 RTT 的時間戳佇列 ---
// 紀錄發送時間，收到 ACK 時取出對比
struct Queue {
    struct timeval data[QUEUE_SIZE];
    int front; 
	int rear; 
	int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};
void initQueue(struct Queue* queue) {
    queue->front = 0; 
	queue->rear = 0; 
	queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
}
void enqueue(struct Queue* queue, struct timeval* item) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->size == QUEUE_SIZE) {
		pthread_cond_wait(&queue->cond, &queue->mutex);
    }
	queue->data[queue->rear] = *item;
    queue->rear = (queue->rear + 1) % QUEUE_SIZE;
    queue->size++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}
struct timeval dequeue(struct Queue* queue) {
    struct timeval item;
    pthread_mutex_lock(&queue->mutex);
    while (queue->size == 0) {
		pthread_cond_wait(&queue->cond, &queue->mutex);
    }
	item = queue->data[queue->front];
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->size--;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return item;
}

// 封包表頭定義 (需與 Router/Server 一致)
typedef struct IPHeader {
    uint8_t version_ihl; 
	uint8_t type_of_service;
	uint16_t total_length; 
	uint16_t identification;
    uint16_t flags_fragment_offset;
	uint8_t time_to_live; 
	uint8_t protocol; 
	uint16_t header_checksum;
    uint32_t source_ip; 
	uint32_t destination_ip;
}IPHeader;
typedef struct UDPHeader {
    uint32_t source_port : 16, dest_port : 16; 
	uint32_t Segment_Length : 16, Checksum : 16;
}UDPHeader;
typedef struct MACHeader {
    uint8_t sour_mac[6];
	uint8_t des_mac[6]; 
	uint16_t fram_typ; 
	uint32_t crc;
}MACHeader;
typedef struct TCPHeader {
    uint16_t source_port; 
	uint16_t destination_port; 
	uint32_t sequence_number; 
	uint32_t ack_number;
    uint16_t offset_reserved_flags; 
	uint16_t window_size;
	uint16_t checksum; 
	uint16_t urgent_pointer;
}TCPHeader;
typedef struct Packet {
    struct IPHeader ipheader; 
	struct UDPHeader udpheader; 
	struct MACHeader macheader;
	char buffer[PACKET_SIZE - 46];
}Packet;

char last_payload[5000] = "`abc"; // 初始 Payload
struct Queue tcpQueue;

// --- 建構並發送 TCP 封包 ---
void tcp_msg_sender(int fd) {
    char payload[5000];
    strcpy(payload, last_payload);
    // 變更 Payload 內容 (例如 abc -> bcd)，讓每次發送內容不同
    for (int i = 0; i < strlen(payload); i++){
		payload[i]++;
    }
	strncpy(last_payload, payload, 5000); // 更新全域變數

    size_t payload_length = strlen(payload);
    char buffer[PACKET_SIZE] = { 0 };

    // 填寫 MAC 表頭
    struct MACHeader macHeader;
    uint8_t des_mac[6] = { 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };
    uint8_t sour_mac[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    uint16_t fram_typ = 0x0800;
    memcpy(macHeader.des_mac, des_mac, 6);
    memcpy(macHeader.sour_mac, sour_mac, 6);
    macHeader.fram_typ = htons(fram_typ); // 轉為網路位元組順序

    // 填寫 IP 表頭
    struct IPHeader ipHeader;
    ipHeader.version_ihl = 0x45; ipHeader.total_length = htons(sizeof(IPHeader) + sizeof(TCPHeader) + payload_length);
    ipHeader.identification = htons(0xAAAA); ipHeader.flags_fragment_offset = htons(0x4000);
    ipHeader.time_to_live = 64; ipHeader.protocol = 0x06; // Protocol 6 = TCP
    inet_pton(AF_INET, "10.17.164.10", &ipHeader.source_ip);
    inet_pton(AF_INET, "10.17.89.69", &ipHeader.destination_ip);

    // 填寫 TCP 表頭
    struct TCPHeader tcpHeader;
    tcpHeader.source_port = htons(12345); tcpHeader.destination_port = htons(80);
    tcpHeader.sequence_number = htonl(1); tcpHeader.ack_number = htonl(0);

    // 將所有表頭與 Payload 複製到 buffer
    memcpy(buffer, &macHeader, sizeof(MACHeader));
    memcpy(buffer + sizeof(MACHeader), &ipHeader, sizeof(IPHeader));
    memcpy(buffer + sizeof(MACHeader) + sizeof(IPHeader), &tcpHeader, sizeof(TCPHeader));
    memcpy(buffer + sizeof(MACHeader) + sizeof(IPHeader) + sizeof(TCPHeader), payload, payload_length);

    // 發送資料
    send(fd, buffer, sizeof(IPHeader) + sizeof(TCPHeader) + sizeof(MACHeader) + payload_length, 0);
    printf("client send tcp packet\n");
}

// --- 接收 UDP 封包並回傳 ACK ---
void rcv_UDPpacket(int fd, struct sockaddr_in* router_addr) {
    struct Packet* packet = (struct Packet*)malloc(sizeof(struct Packet));
    socklen_t len;
    struct sockaddr_in sender_addr;

    int cnt = 0;
    while (cnt < LOOP_COUNT) {
        cnt += 1;
        len = sizeof(sender_addr);
        // 接收來自 Router 的 UDP 封包
        int r = recvfrom(fd, packet, sizeof(*packet), 0, (struct sockaddr*)&sender_addr, &len);
        if (r == -1) { 
			printf("recv fail\n"); break; 
		}else { 
			printf("client rcv UDP packet %d !\n", cnt); 
		}
        // 回傳 ACK 給 Server (透過 Router)
        packet->ipheader.destination_ip = 0x0A115945; // 設定目的 IP 為 Server 的虛擬 IP
        sendto(fd, packet, sizeof(*packet), 0, (struct sockaddr*)router_addr, sizeof(*router_addr));
    }
}

// --- TCP 執行緒: 連線 Router, 發送並計算 RTT ---
void* tcp_socket(void* argu) {
    int server_fd;
    struct sockaddr_in serv_addr;
    struct timeval current_time, timestamp;
    float RTT, ETE, AvgETE = 0, throughput;
    char buffer[BUFF_LEN];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0){
		pthread_exit(NULL);
	}
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(ROUTER_PORT); // 連接到 Router Port 9002
    inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr);

    if (connect(server_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("TCP connect failed!\n"); close(server_fd); pthread_exit(NULL);
    }
    for (int i = 0; i < LOOP_COUNT; i++) {
        gettimeofday(&current_time, NULL);
        enqueue(&tcpQueue, &current_time); // 記錄發送時間戳
        tcp_msg_sender(server_fd); // 發送封包

        // 接收 Router 轉發回來的 Server ACK
        int valread = recv(server_fd, buffer, sizeof(buffer), 0);
        if (valread > 0) {
            timestamp = dequeue(&tcpQueue); // 取出對應的時間戳
            gettimeofday(&current_time, NULL);
            // 計算 RTT (ms)
            RTT = (current_time.tv_sec - timestamp.tv_sec) * 1000.0 + (current_time.tv_usec - timestamp.tv_usec) / 1000.0;
            ETE = RTT / 2.0; // 估計單向延遲
            throughput = (valread * 8) / RTT; // 計算吞吐量 (bits/ms = kbps)

            if (i == 0) {
				AvgETE = ETE;
            }else {
				AvgETE = AvgETE * 0.7 + ETE * 0.3; // EWMA 平均
			}
            printf("-----TCP ACK %02d-----\n", i + 1);
            printf("RTT:%.3fms\n", RTT);
            printf("ETE:%.3fms\n", ETE);
            printf("AvgETE:%.3fms\n", AvgETE);
            printf("Throughput:%.3fkbps\n", throughput);
            printf("----------------------\n");
        }
        usleep(10000); // 稍作延遲
    }
    close(server_fd);
    return 0;
}

// --- UDP 執行緒: 綁定 Port 接收封包 ---
void* udp_socket(void* argu) {
    int cli_sockfd;
    struct sockaddr_in cli_addr, router_addr;
    cli_sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&cli_addr, 0, sizeof(cli_addr));
    cli_addr.sin_family = AF_INET;
    cli_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    cli_addr.sin_port = htons(CLIENT_PORT); // 綁定 Client Port 9003

    if ((bind(cli_sockfd, (struct sockaddr*)&cli_addr, sizeof(cli_addr))) != 0) {
		return NULL;
	}
    // 設定回傳目標 (Router)
    memset(&router_addr, 0, sizeof(router_addr));
    router_addr.sin_family = AF_INET;
    router_addr.sin_port = htons(ROUTER_PORT);
    inet_pton(AF_INET, SERVER_IP, &router_addr.sin_addr);

    rcv_UDPpacket(cli_sockfd, &router_addr); // 進入接收迴圈
    close(cli_sockfd);
    return NULL;
}

int main() {
    initQueue(&tcpQueue);
    pthread_t thread1, thread2;
    // 啟動 TCP 和 UDP 執行緒
    pthread_create(&thread1, NULL, &tcp_socket, NULL);
    pthread_create(&thread2, NULL, &udp_socket, NULL);
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    return 0;
}