/*
 * router.c — 模擬網路中的 Router 節點（Part 1 + 2）
 *
 * 5 個執行緒：TCP 接收、TCP 發送、TCP ACK 轉送、UDP 接收、UDP 發送。
 * 收到的封包放進 TCP / UDP 各自的執行緒安全環狀佇列（mutex + condition variable），
 * 發送端每次出列都模擬 30ms 服務時間，並記錄排隊時間、排隊延遲與 EWMA 平均排隊延遲；
 * 依 IP 表頭的虛擬目的 IP 決定轉送到 Server 或兩個 Client 之一。
 */
#include <pthread.h>     // 引入執行緒庫，用於多執行緒處理
#include <unistd.h>      // 包含標準 POSIX 函式 (如 sleep, close)
#include <stdlib.h>      // 標準庫 (malloc, exit 等)
#include <stdio.h>       // 標準輸入輸出 (printf 等)
#include <sys/types.h>   // 系統資料型別定義
#include <sys/socket.h>  // Socket 相關函式庫
#include <netinet/in.h>  // Internet 地址家族定義 (sockaddr_in 等)
#include <string.h>      // 字串處理 (memcpy, memset 等)
#include <arpa/inet.h>   // IP 地址轉換函式 (inet_pton 等)
#include <sys/time.h>    // 時間相關函式 (gettimeofday)
#include <stdbool.h>     // 布林值定義
#include "packet.h"    // 共用的常數與封包表頭定義


#define SERVER_IP "127.0.0.1" // 伺服器實體 IP (本機回環)

// 定義虛擬 IP 用於路由邏輯判斷 (模擬網路層轉發)


// --- 佇列項目結構 ---
typedef struct QueueItem {
    char data[PACKET_SIZE]; // 存放原始 TCP 緩衝區或 Packet 結構
    int size;               // 資料大小
    struct timeval entry_time; // 進入佇列的時間 (用於計算排隊時間)
    struct sockaddr_in sender_addr; // 發送者地址 (用於 UDP 路由/ACK)
} QueueItem;

// --- 執行緒安全的佇列結構 ---
struct Queue {
    QueueItem items[QUEUE_SIZE]; // 陣列儲存項目
    int front; int rear; int size; // 佇列頭、尾索引與當前大小
    pthread_mutex_t mutex;       // 互斥鎖，保護佇列存取
    pthread_cond_t cond;         // 條件變數，用於執行緒等待/喚醒
};

// 初始化佇列
void initQueue(struct Queue* queue) {
    queue->front = 0; queue->rear = 0; queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL); // 初始化鎖
    pthread_cond_init(&queue->cond, NULL);   // 初始化條件變數
}

// 入列 (Enqueue) - 生產者呼叫
void enqueue(struct Queue* queue, void* data, int data_size, struct timeval* time, struct sockaddr_in* addr, int* qlen) {
    pthread_mutex_lock(&queue->mutex); // 加鎖
    // 如果佇列滿了，等待消費者取出資料 (等待 cond 信號)
    while (queue->size == QUEUE_SIZE) pthread_cond_wait(&queue->cond, &queue->mutex);
    // 複製資料到佇列尾端
    memcpy(queue->items[queue->rear].data, data, data_size);
    queue->items[queue->rear].size = data_size;
    queue->items[queue->rear].entry_time = *time;
    if (addr) {
		queue->items[queue->rear].sender_addr = *addr;
	}
    // 更新尾端索引 (環狀緩衝區)
    queue->rear = (queue->rear + 1) % QUEUE_SIZE;
    queue->size++;
    *qlen = queue->size; // 回傳當前佇列長度
    
    pthread_cond_signal(&queue->cond); // 喚醒正在等待的消費者
    pthread_mutex_unlock(&queue->mutex); // 解鎖
}

// 出列 (Dequeue) - 消費者呼叫
void dequeue(struct Queue* queue, void* buffer, int* data_size, struct timeval* entry_time, struct sockaddr_in* addr, int* qlen) {
    pthread_mutex_lock(&queue->mutex); // 加鎖
    // 如果佇列是空的，等待生產者放入資料
    while (queue->size == 0) {
		pthread_cond_wait(&queue->cond, &queue->mutex);
	}
    // 從佇列前端取出資料
    memcpy(buffer, queue->items[queue->front].data, queue->items[queue->front].size);
    *data_size = queue->items[queue->front].size;
    *entry_time = queue->items[queue->front].entry_time;
    if (addr) {
		*addr = queue->items[queue->front].sender_addr;
	}
    // 更新前端索引
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->size--;
    *qlen = queue->size;

    pthread_cond_signal(&queue->cond); // 喚醒可能因佇列滿而等待的生產者
    pthread_mutex_unlock(&queue->mutex); // 解鎖
}

struct Queue tcpQueue; // TCP 專用佇列
struct Queue udpQueue; // UDP 專用佇列

int router_tcp_fd, client_tcp_fd, server_tcp_fd; // 全域 Socket 描述符

// --- TCP 接收端執行緒: Client -> Router -> Queue ---
void* tcp_receiver(void* argu) {
    int router_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[PACKET_SIZE];
    struct timeval current_time;
    int qlength;

    // 建立 Router 的 TCP Server Socket
    if ((router_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
		perror("TCP Socket failed"); 
		exit(1);
	}
    int opt = 1; 
	setsockopt(router_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 允許重複使用地址

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 監聽所有網卡
    address.sin_port = htons(ROUTER_PORT); // 指定 Port 9002

    if (bind(router_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
		perror("TCP Bind failed"); 
		exit(1); 
	}
    if (listen(router_fd, 3) < 0) { 
		perror("Listen failed"); 
		exit(1); 
	} // 開始監聽

    printf("Router TCP listening...\n");
    // 接受 Client 連線
    if ((new_socket = accept(router_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
        perror("Accept failed"); 
		exit(1);
    }
    client_tcp_fd = new_socket; // 保存 socket 供轉發 ACK 使用

    int count = 0;
    while (count < LOOP_COUNT) {
        // 從 Client 接收資料
        int valread = recv(new_socket, buffer, sizeof(buffer), 0);
        if (valread <= 0) {
			break; // 連線斷開
		}
        gettimeofday(&current_time, NULL); // 記錄接收時間
        // 將資料放入 TCP 佇列
        enqueue(&tcpQueue, buffer, valread, &current_time, NULL, &qlength);

        printf("-----RecvTCP (Queue: %d)-----\n", qlength);
        count++;
    }
    return NULL;
}

// --- TCP 發送端執行緒: Queue -> Router -> Server (模擬延遲) ---
void* tcp_sender(void* argu) {
    int server_fd;
    struct sockaddr_in server_addr;
    char buffer[PACKET_SIZE];
    int data_size, qlength;
    struct timeval entry_time, current_time, svc_start, svc_end;
    float staytime, servicetime, qdelay, avgqdelay = 0;

    // 建立連線到 Server 的 Socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
		perror("Socket creation error"); 
		exit(1); 
	}
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT); // 目標 Port 9000
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) { 
		perror("Invalid address"); 
		exit(1); 
	}
    sleep(1); // 等待接收端準備好
    // 連線到 Server
    if (connect(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { 
		perror("Connection Failed"); 
		exit(1); 
	}
    server_tcp_fd = server_fd; // 保存 fd
    int count = 0;
    while (count < LOOP_COUNT) {
        // 從佇列取出封包
        dequeue(&tcpQueue, buffer, &data_size, &entry_time, NULL, &qlength);

        // 計算排隊時間 (Stay Time): 現在時間 - 進入佇列時間
        gettimeofday(&current_time, NULL);
        staytime = (current_time.tv_sec - entry_time.tv_sec) * 1000.0 + (current_time.tv_usec - entry_time.tv_usec) / 1000.0;

        // 模擬服務時間 (Service Time)
        gettimeofday(&svc_start, NULL);
        usleep(30 * 1000); // 暫停 30ms，模擬處理延遲
        gettimeofday(&svc_end, NULL);
        servicetime = (svc_end.tv_sec - svc_start.tv_sec) * 1000.0 + (svc_end.tv_usec - svc_start.tv_usec) / 1000.0;

        // 總排隊延遲 = 排隊時間 + 服務時間
        qdelay = staytime + servicetime;
        // 計算移動平均延遲 (EWMA)
        if (count == 0) {
			avgqdelay = qdelay;
		}else {
			avgqdelay = avgqdelay * 0.7 + qdelay * 0.3;
		}
        printf("-----SendTCP-----\n");
        printf("QueueLength:%d\n", qlength);
        printf("QueuingTime:%.3fms\n", staytime);
        printf("ServiceTime:%.3fms\n", servicetime);
        printf("QueuingDelay:%.3fms\n", qdelay);
        printf("AvgQueuingDelay:%.3fms\n", avgqdelay);
        printf("-----------------\n");

        // 發送給 Server
        send(server_fd, buffer, data_size, 0);
        count++;
    }
    return NULL;
}

// --- TCP 回傳轉發器: Server -> Router -> Client (ACK) ---
// 這部分不經過佇列模擬延遲，直接轉發，僅用於讓 Client 計算 RTT
void* tcp_ack_forwarder(void* argu) {
    char buffer[PACKET_SIZE];
    int count = 0;
    sleep(2); // 等待連線建立
    while (count < LOOP_COUNT) {
        if (server_tcp_fd > 0 && client_tcp_fd > 0) {
            // 從 Server 接收回應
            int valread = recv(server_tcp_fd, buffer, sizeof(buffer), 0);
            if (valread > 0) {
                // 直接轉發給 Client
                send(client_tcp_fd, buffer, valread, 0);
                count++;
            }
        }else {
            usleep(10000); // 若連線未建立稍作等待
        }
    }
    return NULL;
}

// --- UDP 接收端: Client -> Router -> Queue ---
void* udp_receiver(void* argu) {
    int router_fd;
    struct sockaddr_in address, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    struct Packet packet;
    struct timeval current_time;
    int qlength;

    // 建立 UDP Socket
    if ((router_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { perror("UDP Socket failed"); exit(1); }
    int opt = 1; setsockopt(router_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(ROUTER_PORT); // UDP 監聽 Port 9002

    if (bind(router_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
		perror("UDP Bind failed"); 
		exit(1); 
	}
    int count = 0;
    // 預期接收 Client 送來的和 Server 回傳的 ACK，所以次數 * 2
    while (count < LOOP_COUNT * 2) {
        int valread = recvfrom(router_fd, &packet, sizeof(packet), 0, (struct sockaddr*)&client_addr, &addr_len);
        if (valread > 0) {
            gettimeofday(&current_time, NULL);
            // 放入 UDP 佇列，並保存發送者地址 (以便回傳)
            enqueue(&udpQueue, &packet, valread, &current_time, &client_addr, &qlength);
            printf("-----RecvUDP (Queue: %d)-----\n", qlength);
            count++;
        }
    }
    close(router_fd);
    return NULL;
}

// --- UDP 發送端: Queue -> Router -> Destination (模擬延遲) ---
void* udp_sender(void* argu) {
    int router_fd;
    struct sockaddr_in dest_addr, temp_addr;
    struct Packet packet;
    int data_size, qlength;
    struct timeval entry_time, current_time, svc_start, svc_end;
    float staytime, servicetime, qdelay, avgqdelay = 0;
    if ((router_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { 
		perror("UDP Send Socket failed");
		exit(1); 
	}
    // 設定三個可能的目的地地址
    struct sockaddr_in server_addr, client_addr, client2_addr;
    server_addr.sin_family = AF_INET; server_addr.sin_port = htons(SERVER_PORT); server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    client_addr.sin_family = AF_INET; client_addr.sin_port = htons(CLIENT_PORT); client_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    client2_addr.sin_family = AF_INET; client2_addr.sin_port = htons(CLIENTTWO_PORT); client2_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int count = 0;
    while (count < LOOP_COUNT * 2) {
        // 從佇列取出封包
        dequeue(&udpQueue, &packet, &data_size, &entry_time, &temp_addr, &qlength);

        // 計算排隊時間
        gettimeofday(&current_time, NULL);
        staytime = (current_time.tv_sec - entry_time.tv_sec) * 1000.0 + (current_time.tv_usec - entry_time.tv_usec) / 1000.0;

        // 模擬服務時間 (延遲)
        gettimeofday(&svc_start, NULL);
        usleep(30 * 1000); // 30ms
        gettimeofday(&svc_end, NULL);
        servicetime = (svc_end.tv_sec - svc_start.tv_sec) * 1000.0 + (svc_end.tv_usec - svc_start.tv_usec) / 1000.0;

        qdelay = staytime + servicetime;
        if (count == 0) {
			avgqdelay = qdelay;
        }else {
			avgqdelay = avgqdelay * 0.7 + qdelay * 0.3;
		}
        printf("-----SendUDP-----\n");
        printf("QueueLength:%d\n", qlength);
        printf("QueuingTime:%.3fms\n", staytime);
        printf("ServiceTime:%.3fms\n", servicetime);
        printf("QueuingDelay:%.3fms\n", qdelay);
        printf("AvgQueuingDelay:%.3fms\n", avgqdelay);
        printf("-----------------\n");

        // --- 路由邏輯 ---
        struct IPHeader* ipH = &packet.ipheader;
        // 根據虛擬 IP 決定發送目標
        if (ipH->destination_ip == CLIENT_VIRTUAL_IP) {
            sendto(router_fd, &packet, data_size, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
        }else if (ipH->destination_ip == CLIENT2_VIRTUAL_IP) {
            sendto(router_fd, &packet, data_size, 0, (struct sockaddr*)&client2_addr, sizeof(client2_addr));
        }else if (ipH->destination_ip == SERVER_VIRTUAL_IP) {
            sendto(router_fd, &packet, data_size, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        }
        count++;
    }
    close(router_fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    parse_args(argc, argv); // -n 封包數、-p 基準 port（見 packet.h）
    // 初始化佇列
    initQueue(&tcpQueue);
    initQueue(&udpQueue);

    pthread_t t1, t2, t3, t4, t5;
    // 啟動 5 個執行緒分別處理接收與發送
    pthread_create(&t1, NULL, &tcp_receiver, NULL);
    pthread_create(&t2, NULL, &tcp_sender, NULL);
    pthread_create(&t3, NULL, &tcp_ack_forwarder, NULL);
    pthread_create(&t4, NULL, &udp_receiver, NULL);
    pthread_create(&t5, NULL, &udp_sender, NULL);

    // 等待所有執行緒結束
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
    pthread_join(t5, NULL);

    return 0;
}