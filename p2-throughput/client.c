// ============================================================
// client_v2.c
// ------------------------------------------------------------
// 角色：Client
// - 和 Router 建立 TCP 連線，定期送出小封包 (test[256])
// - 透過 UDP 收到「影片資料」，再由 udp_ack thread 回 ACK 給 Router
// - 在 tcp_ack thread 中：
//     - 用 Queue 保存「送出 TCP 封包時的時間戳」
//     - 收到來自 router 的 TCP ACK 後，計算 RTT / ETE / AvgETE
//     - 額外計算本次封包 Throughput = bits / RTT(ms)
// ============================================================

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>    
#include <unistd.h>
#include <arpa/inet.h>
#include <stdbool.h> 
#include <sys/time.h> 

// ---------------------- 常數設定 ----------------------
// 三方溝通使用的 port，client 只會用到其中幾個
#define PORT 8957                     // client 連到 router 的 TCP port
#define SERVER_PORT 8956              // router 連到 server 的 TCP port（client 不直接用）
#define CLIENT_PORT 8955              // client 端接收「影片 UDP 封包」的 port
#define TCP_ACK_TO_ROUTER_PORT 9000   // server → router 的 TCP ACK (UDP) port（client 不直接用）
#define TCP_ACK_TO_CLIENT_PORT 9001   // router → client 的 TCP ACK (UDP) port
#define UDP_ACK_TO_ROUTER_PORT 9002   // client → router 的 UDP ACK port
#define UDP_ACK_TO_SERVER_PORT 9003   // router → server 的 UDP ACK port（client 不直接用）
#define IP "127.0.0.1"                // 本機 loopback
#define QUEUE_SIZE 100                // 儲存時間戳的 queue 大小

// ---------------------- udp_ack 同步旗標 ----------------------
// 用來在 udp_socket() 和 udp_ack() 之間同步「該送 ACK 了沒」
bool udp_ack_bool = false;
pthread_mutex_t udp_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t udp_ack_cond = PTHREAD_COND_INITIALIZER;

// ---------------------- 時間戳 Queue ----------------------
// 此 Queue 專門存「送出 TCP 封包當下的時間」(struct timeval)
struct Queue {
    struct timeval data[QUEUE_SIZE];  // 環狀陣列存時間戳
    int front;                        // 取出位置
    int rear;                         // 放入位置
    int size;                         // 目前元素數量
    pthread_mutex_t mutex;           // 保護 queue 的 mutex
    pthread_cond_t cond;             // queue 空/滿的同步條件變數
};


// 初始化 Queue
void initQueue(struct Queue *queue) {
    queue->front = 0;
    queue->rear = 0;
    queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
}

// 將一個時間戳放入 queue（阻塞直到有空間）
void enqueue(struct Queue *queue, struct timeval *item) {
    pthread_mutex_lock(&queue->mutex);
    while(queue->size >= QUEUE_SIZE){
        // queue 滿了就等待，有空間時被喚醒
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    queue->data[queue->rear] = *item;               // 複製時間戳
    queue->rear = (queue->rear+1) % QUEUE_SIZE;     // 環狀索引
    queue->size++;
    pthread_cond_signal(&queue->cond);              // 通知可能在等的 dequeue
    pthread_mutex_unlock(&queue->mutex);
}

// 從 queue 取出一個時間戳（阻塞直到有資料）
struct timeval dequeue(struct Queue *queue) {
    struct timeval item;
    pthread_mutex_lock(&queue->mutex);
    while(queue->size <= 0){
        // queue 空時等待，有資料時被喚醒
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    item = queue->data[queue->front];
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->size--;
    pthread_cond_signal(&queue->cond);              // 通知可能在等的 enqueue
    pthread_mutex_unlock(&queue->mutex);
    return item;
};


// 銷毀 Queue 所使用的同步原語
void destroyQueue(struct Queue *queue) {
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
}

// ------------------------------------------------------------
// Thread 1: tcp_socket
// - 建立 TCP socket → 連線到 router (PORT)
// - 每隔 2.5ms 送出一個 256 bytes 的封包
// - 送出前先把「送出時間」丟到 Queue，給 tcp_ack() 之後算 RTT 用
// ------------------------------------------------------------
void *tcp_socket(void *argu){
	int client_fd;
   	struct sockaddr_in address;
   	//socklen_t address_length;
   	//address_length = sizeof(address);

    	// 建立 TCP socket
    	client_fd = socket(AF_INET, SOCK_STREAM, 0);
    	if(client_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	
    	address.sin_family = AF_INET;           // IPv4
	address.sin_port = htons(PORT);  // Router 端的 TCP 監聽 port
    	// 將 "127.0.0.1" 轉成 binary IP
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}
	// 嘗試連線到 router
	if (connect(client_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { //(socket, ip and port , size)
		printf("connect error\n");
		pthread_exit(0);
	}

	char test[256];                               // 實驗用 payload，內容不重要
	int counter = 0;
	struct Queue *queue = (struct Queue *)argu;   // 指向共享的時間戳 queue
	struct timeval current_time;

	while(counter<30){
		// 1. 記錄這個封包送出的時間，存進 queue
		gettimeofday(&current_time, NULL);// 封包從 client 出發的時間
		enqueue(queue, &current_time);

		// 2. 送出 TCP 資料
		send(client_fd, test, sizeof(test), 0);// 發送封包到 router

		counter = counter+1;

		// 3. 稍微 sleep，避免送太快不好觀察
		usleep(2500);// 2.5 ms
	}

	close(client_fd);	
	return NULL;
}

// ------------------------------------------------------------
// Thread 2: udp_socket
// - 在 CLIENT_PORT 上用 UDP 收「影片封包」
// - 每收到一個封包，就喚醒 udp_ack thread 去送 UDP ACK 給 router
// ------------------------------------------------------------
void *udp_socket(void *argu){
	int client_fd;
	struct sockaddr_in address;
    	socklen_t address_length;
    	address_length = sizeof(address);

    	// 建立 UDP socket
    	client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(client_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;        // 綁在本機所有 IP
    	address.sin_port = htons(CLIENT_PORT);       // client 收影片的 port

    	// bind 到指定 port
    	if (bind(client_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	char test[256];
    	int counter = 0;

   	while(counter<30){
   		// 等待 router 送來的 UDP 影片封包
   		recvfrom(client_fd, test, sizeof(test) , 0, (struct sockaddr*)&address,  &address_length);

   		// 收到資料後，用條件變數通知 udp_ack thread 可以送 ACK 了
   		pthread_mutex_lock(&udp_ack_mutex);
		udp_ack_bool = true;
		pthread_cond_signal(&udp_ack_cond);
       		pthread_mutex_unlock(&udp_ack_mutex);

		counter = counter + 1;
   	}

   	close(client_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 3: tcp_ack
// - 在 TCP_ACK_TO_CLIENT_PORT 上收 router 轉送過來的 TCP ACK (UDP)
// - 每收到一個 ACK：
//    1. 從 Queue 取出對應封包送出時間戳
//    2. 算 RTT / ETE / AvgETE
//    3. 額外算本次封包 Throughput = bits / RTT(ms)
//    4. 把結果印出
// ------------------------------------------------------------
void *tcp_ack(void *argu){
	int client_fd;
	struct sockaddr_in address;
    	socklen_t address_length;
    	address_length = sizeof(address);

    	// 建立 UDP socket 用來接收 ACK
    	client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(client_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;               // 本機所有 IP
    	address.sin_port = htons(TCP_ACK_TO_CLIENT_PORT);   // router → client 的 ACK port

    	// 綁定 port
    	if (bind(client_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	char test[256];
    	int counter = 1;                       // GET 編號從 1 開始印
    	float RTT, ETE, AvgETE, Throughput;   // RTT / 一半的 ETE / 平滑後的 AvgETE / 本封包 throughput
    	struct Queue *queue = (struct Queue *)argu;   // 共用時間戳 queue
    	struct timeval current_time;
    	struct timeval timestamp;

   	while(counter<31){
   		// 1. 收一個 ACK 封包
   		recvfrom(client_fd, test, sizeof(test) , 0, (struct sockaddr*)&address,  &address_length);

		// 2. 拿出對應的送出時間戳，計算 RTT / ETE
		timestamp = dequeue(queue);
		gettimeofday(&current_time, NULL);

		// RTT 以毫秒為單位
		RTT = (current_time.tv_sec - timestamp.tv_sec)*1000
		    +(current_time.tv_usec-timestamp.tv_usec)/1000.000;

		// 單向 ETE 近似為 RTT/2
		ETE = RTT/2.000;

   		// 3. 用 0.7 舊值 + 0.3 新值 做指數平滑的 AvgETE
   		if(counter==1){
   			// 第一個封包，沒有舊值，直接用 ETE 當基準
   			AvgETE = ETE * 0.7 + ETE * 0.3;
   		}
   		else{
   			AvgETE = AvgETE * 0.7 + ETE * 0.3;
   		}

		////////////////////////////
		// 4. 本封包 Throughput：bits / RTT(ms)
		float bits = (float)(sizeof(test) * 8);
		float Throughput_bpm = bits / RTT;              // bits per ms
		float Throughput_Mbps = Throughput_bpm / 1000;  // Mbps
		////////////////////////////

   		// 5. 印出結果
   		printf("-----GET--%02d--ACK-----\n",counter);
   		printf("RTT:%.3fms\n", RTT);
   		printf("ETE:%.3fms\n", ETE);
   		printf("AvgETE:%.3fms\n", AvgETE);
		printf("Throughput:%.6f Mbps\n", Throughput_Mbps);
   		printf("----------------------\n");

		counter = counter + 1;
   	}

   	close(client_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 4: udp_ack
// - 等待 udp_socket() 告訴它「有資料收到了」(udp_ack_bool == true)
// - 然後經由 UDP 把 ACK 封包送回 Router (UDP_ACK_TO_ROUTER_PORT)
// ------------------------------------------------------------
void *udp_ack(void *argu){
	int server_fd;
   	struct sockaddr_in address;
   	socklen_t address_length;
   	address_length = sizeof(address);

    	// 建立 UDP socket，用來把 ACK 傳回 router
    	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(server_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	
    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(UDP_ACK_TO_ROUTER_PORT);  // router 監聽 client ACK 的 port
	//address.sin_addr.s_addr = inet_addr("127.0.0.1");
    	//  IP address 轉成二進位 (127.0.0.1)
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	char test[256];      // ACK payload 大小固定即可
	int counter = 0;

	while(counter<30){
		pthread_mutex_lock(&udp_ack_mutex);
		if(udp_ack_bool == false){
			// 沒有新的資料要 ACK，就睡在條件變數上
			pthread_cond_wait(&udp_ack_cond, &udp_ack_mutex);
		}
		else{
			// 有資料要 ACK，送出一個 UDP 封包給 router
			sendto(server_fd, test, sizeof(test), 0,
			       (struct sockaddr*)&address, (socklen_t)address_length);
			udp_ack_bool = false;   // 這次 ACK 已送出
			counter = counter+1;
		}
		pthread_mutex_unlock(&udp_ack_mutex);
	}	

	close(server_fd);
	return NULL;
}

// ------------------------------------------------------------
// main
// - 建立一個 Queue 用來存 TCP 送出時間
// - 建立四個 thread：tcp_socket / udp_socket / tcp_ack / udp_ack
// - 等待四個 thread 結束後銷毀 Queue
// ------------------------------------------------------------
int main(){
	struct Queue timestampQueue;
    	initQueue(&timestampQueue);

	pthread_t thread1, thread2,thread3, thread4;    

	pthread_create(&thread1, NULL, &tcp_socket, &timestampQueue);    
	pthread_create(&thread2, NULL, &udp_socket, NULL);
	pthread_create(&thread3, NULL, &tcp_ack, &timestampQueue);
	pthread_create(&thread4, NULL, &udp_ack, NULL);

	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	pthread_join(thread3, NULL);
	pthread_join(thread4, NULL);

	destroyQueue(&timestampQueue);
	return 0;
}
