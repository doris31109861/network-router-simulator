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
#define LOOP_COUNT 23 

#define CLIENT_IP "127.0.0.1"
#define SERVER_PORT 9000 // Server 監聽 Port
#define ROUTER_PORT 9002 // Router Port
#define QUEUE_SIZE 100

// --- 時間戳佇列 (用於 UDP RTT 計算) ---
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
    while (queue->size == QUEUE_SIZE){
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
    while (queue->size == 0){
		pthread_cond_wait(&queue->cond, &queue->mutex);
	}
    item = queue->data[queue->front];
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->size--;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return item;
}

// 封包表頭結構
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
    char buffer[MTU - 40];
}Packet;

struct Queue timestampQueue; // 存放 UDP 發送時間
int udp_sock_fd_global;      // 全域 UDP socket 供發送與接收執行緒使用

// --- UDP 發送函式: 主動發送封包給 Client ---
void udp_msg_sender(struct sockaddr* dst) {
    int payload_size;
    struct MACHeader* machdr = (struct MACHeader*)malloc(sizeof(struct MACHeader));
    machdr->fram_typ = 0x0000;
    struct IPHeader* iphdr = (struct IPHeader*)malloc(sizeof(struct IPHeader));
    iphdr->version_ihl = 0x45; iphdr->total_length = MTU; iphdr->protocol = 0x11; // Protocol 17 = UDP
    iphdr->source_ip = 0x0A115945; // Server 虛擬 IP
    iphdr->destination_ip = 0x0A000301; // Client 虛擬 IP
    struct UDPHeader* udphdr = (struct UDPHeader*)malloc(sizeof(struct UDPHeader));
    udphdr->source_port = 10000; udphdr->dest_port = 10010;

    payload_size = MTU - sizeof(*iphdr) - sizeof(*udphdr) - sizeof(*machdr);
    struct Packet* packet = (struct Packet*)malloc(sizeof(struct Packet));

    int cnt = 0;
    struct timeval current_time;

    while (cnt < LOOP_COUNT) {
        cnt += 1;
        packet->ipheader = *iphdr; packet->udpheader = *udphdr; packet->macheader = *machdr;

        gettimeofday(&current_time, NULL);
        enqueue(&timestampQueue, &current_time); // 記錄發送時間

        // 發送到 Router
        sendto(udp_sock_fd_global, packet, sizeof(*packet), 0, dst, sizeof(*dst));
        printf("server send UDP packet %d !\n", cnt);
        usleep(10000); // 傳送間隔
    }
}

// --- UDP 接收函式: 接收 Client 回傳的 ACK 並計算 RTT ---
void* udp_ack_receiver(void* argu) {
    char test[2048];
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);

    int counter = 1;
    float RTT, ETE, AvgETE = 0, throughput;
    struct timeval current_time, timestamp;

    while (counter <= LOOP_COUNT) {
        // 接收 Router 轉發回來的封包
        int r = recvfrom(udp_sock_fd_global, test, sizeof(test), 0, (struct sockaddr*)&address, &address_length);
        if (r > 0) {
            timestamp = dequeue(&timestampQueue); // 取出時間戳
            gettimeofday(&current_time, NULL);

            // 計算 RTT
            RTT = (current_time.tv_sec - timestamp.tv_sec) * 1000.0 + (current_time.tv_usec - timestamp.tv_usec) / 1000.0;
            ETE = RTT / 2.0;
            throughput = (r * 8) / RTT;

            if (counter == 1){
				AvgETE = ETE;
			}else{
				AvgETE = AvgETE * 0.7 + ETE * 0.3;
			}
			
            printf("-----UDP ACK %02d-----\n", counter);
            printf("RTT:%.3fms\n", RTT);
            printf("ETE:%.3fms\n", ETE);
            printf("AvgETE:%.3fms\n", AvgETE);
            printf("Throughput:%.3fkbps\n", throughput);
            printf("----------------------\n");
            counter++;
        }
    }
    return NULL;
}

// --- TCP 執行緒: 接收 Client 訊息並回傳 (Echo) ---
void* tcp_socket(void* argu) {
    int count = 0;
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[PACKET_SIZE] = { 0 };

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0){
		pthread_exit(NULL);
	}
    int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT); // 綁定 Port 9000

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        close(server_fd); pthread_exit(NULL);
    }
    if (listen(server_fd, 3) < 0) {
        close(server_fd); pthread_exit(NULL);
    }
    // 接受 Router 轉發過來的 Client 連線
    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
        close(server_fd); pthread_exit(NULL);
    }

    while (count < LOOP_COUNT) {
        memset(buffer, 0, sizeof(buffer));
        int valread = recv(new_socket, buffer, sizeof(buffer), 0);
        if (valread <= 0){
			break;
		}

        // 計算 Payload 的位置 (跳過自訂表頭)
        char* payload_ptr = buffer + sizeof(MACHeader) + sizeof(IPHeader) + sizeof(TCPHeader);

        printf("server received payload %d: %s\n", count + 1, payload_ptr);

        // 將接收到的封包原封不動送回 (Echo)
        send(new_socket, buffer, valread, 0);
        count++;
    }
    close(new_socket);
    close(server_fd);
    return NULL;
}

// --- UDP 執行緒初始化 ---
void* udp_socket(void* argu) {
    sleep(2); // 等待其他組件就緒
    struct sockaddr_in ser_addr;
    struct sockaddr_in my_addr;

    udp_sock_fd_global = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(SERVER_PORT); // 綁定 UDP Port 9000

    bind(udp_sock_fd_global, (struct sockaddr*)&my_addr, sizeof(my_addr));

    memset(&ser_addr, 0, sizeof(ser_addr));
    ser_addr.sin_family = AF_INET;
    ser_addr.sin_addr.s_addr = inet_addr(CLIENT_IP);
    ser_addr.sin_port = htons(ROUTER_PORT); // 目標是 Router

    pthread_t recv_thread;
    // 啟動接收 ACK 的執行緒
    pthread_create(&recv_thread, NULL, &udp_ack_receiver, NULL);

    // 開始發送 UDP 封包
    udp_msg_sender((struct sockaddr*)&ser_addr);

    pthread_join(recv_thread, NULL);
    close(udp_sock_fd_global);

    return NULL;
}

int main() {
    initQueue(&timestampQueue);
    pthread_t thread1, thread2;
    // 啟動 TCP 和 UDP 處理執行緒
    pthread_create(&thread1, NULL, &tcp_socket, NULL);
    pthread_create(&thread2, NULL, &udp_socket, NULL);
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    return 0;
}