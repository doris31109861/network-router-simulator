// ============================================================
// server_v2.c
// ------------------------------------------------------------
// 角色：Server
// - UDP 方向：
//     server →(UDP)→ router → client
//   udp_socket thread：
//     - 定期送出「影片封包」給 router (PORT)
//     - 送出前把送出時間丟進 Queue，給 udp_ack thread 算 RTT／ETE
// - TCP 方向：
//     client → router →(TCP)→ server
//   tcp_socket thread：
//     - 接收 router 轉來的 TCP 封包，並通知 tcp_ack thread 送 ACK 給 router
// - ACK 方向：
//   tcp_ack thread：把 ACK 送回 router
//   udp_ack thread：
//     - 在 UDP_ACK_TO_SERVER_PORT 上收來自 router 的 ACK
//     - 用 Queue 算 RTT / ETE / AvgETE
//     - 額外算 Throughput = bits / RTT(ms)
// ============================================================

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdbool.h> 
#include <pthread.h>    
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h> 

#define PORT 8957                     // server 送影片給 router 的 UDP port
#define SERVER_PORT 8956              // server 接收 router 連線的 TCP port
#define CLIENT_PORT 8955              // client 的 UDP port（server 直接不使用）
#define TCP_ACK_TO_ROUTER_PORT 9000   // server 將 TCP ACK 送給 router 的 UDP port
#define TCP_ACK_TO_CLIENT_PORT 9001   // router 轉給 client 的 ACK port（server 不直接用）
#define UDP_ACK_TO_ROUTER_PORT 9002   // client 對 router 的 UDP ACK port（server 不直接用）
#define UDP_ACK_TO_SERVER_PORT 9003   // router 對 server 的 UDP ACK port
#define IP "127.0.0.1"
#define QUEUE_SIZE 100

// ---------------------- TCP ACK 同步旗標 ----------------------
bool tcp_ack_bool = false;
pthread_mutex_t tcp_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t tcp_ack_cond = PTHREAD_COND_INITIALIZER;

// ---------------------- 時間戳 Queue ----------------------
// server 只需要一個 Queue，用在「影片 UDP 封包」的 RTT 計算
struct Queue {
    struct timeval data[QUEUE_SIZE];  // 存送出影片封包的時間戳
    int front;
    int rear;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};


// 初始化 Queue
void initQueue(struct Queue *queue) {
    queue->front = 0;
    queue->rear = 0;
    queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
}

// enqueue：加一個時間戳進 queue
void enqueue(struct Queue *queue, struct timeval *item) {
    pthread_mutex_lock(&queue->mutex);
    while(queue->size >= QUEUE_SIZE){
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    queue->data[queue->rear] = *item;
    queue->rear = (queue->rear+1) % QUEUE_SIZE;
    queue->size++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

// dequeue：取出一個時間戳
struct timeval dequeue(struct Queue *queue) {
    struct timeval item;
    pthread_mutex_lock(&queue->mutex);
    while(queue->size <= 0){
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    item = queue->data[queue->front];
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->size--;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return item;
};

void destroyQueue(struct Queue *queue) {
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
}

// ------------------------------------------------------------
// Thread 1: udp_socket
// - server 端的「影片資料 sender」
// - 建立 UDP socket 連到 router(PORT)
// - 每次送出封包前，把當下時間放到 Queue，給 udp_ack thread 算 RTT
// ------------------------------------------------------------
void *udp_socket(void *argu){
	int server_fd;
   	struct sockaddr_in address;
   	socklen_t address_length;
   	address_length = sizeof(address);

    	// 建立 UDP socket
    	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(server_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(PORT);  // router 收影片的 UDP port
	//address.sin_addr.s_addr = inet_addr("127.0.0.1");
    	// 轉換 IP (127.0.0.1)
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	char test[256];                          // 影片封包內容（實驗用）
	int counter = 0;
	sleep(1);                                // 等 router / client 都起來再送

	struct Queue *queue = (struct Queue *)argu;  // timestampQueue
	struct timeval current_time;

	while(counter<30){
		// 1. 封包送出前紀錄時間，丟進 queue
		gettimeofday(&current_time, NULL);//封包從 server 出發的時間
		enqueue(queue, &current_time);

		// 2. 實際送出 UDP 封包給 router
		sendto(server_fd, test, sizeof(test), 0,
		       (struct sockaddr*)&address, (socklen_t)address_length);//從server回傳給router

		counter = counter+1;

		// 3. 稍微延遲，方便觀察
		usleep(2500);
	}	

	close(server_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 2: tcp_socket
// - server 端的「TCP receiver」
// - 在 SERVER_PORT 上 listen，接受 router 的 TCP 連線
// - 收到每個 TCP 封包後，透過條件變數通知 tcp_ack thread 去送 ACK
// ------------------------------------------------------------
void *tcp_socket(void *argu){
	char test[256];
	int server_fd, new_socket;
	struct sockaddr_in address;
    	int addrlen = sizeof(address);

    	// 建立 TCP socket
    	server_fd = socket(AF_INET, SOCK_STREAM, 0);
    	if(server_fd < 0){
		pthread_mutex_unlock(&tcp_ack_mutex);
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;
    	address.sin_port = htons(SERVER_PORT);

    	// bind 到指定 port
    	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	// listen
   	 if (listen(server_fd, 3) < 0) {
    		printf("listen error\n");
    		pthread_exit(0);
   	 }
   	 
   	// 等 router 連線進來
    	if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
    		printf("accept error\n");
    		pthread_exit(0);
   	}

	int counter = 0;

   	while(counter<30){
   		// 收到來自 router 的 TCP 封包
   		recv(new_socket, test, sizeof(test), 0);

		// 通知 tcp_ack thread「可以送 ACK 了」
	   	pthread_mutex_lock(&tcp_ack_mutex);
		tcp_ack_bool = true;
		pthread_cond_signal(&tcp_ack_cond);
       		pthread_mutex_unlock(&tcp_ack_mutex);

		counter = counter + 1;
   	}

   	close(new_socket);
   	close(server_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 3: tcp_ack
// - server 端的「TCP ACK sender」
// - 等待 tcp_socket() 通知 (tcp_ack_bool)
// - 每次被通知就送一個 UDP 封包到 router (TCP_ACK_TO_ROUTER_PORT)
// ------------------------------------------------------------
void *tcp_ack(void *argu){
	int server_fd;
   	struct sockaddr_in address;
   	socklen_t address_length;
   	address_length = sizeof(address);

    	// 建立 UDP socket，用來把 TCP ACK 送回 router
    	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(server_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	
    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(TCP_ACK_TO_ROUTER_PORT);  // router 收 TCP ACK 的 port
	//address.sin_addr.s_addr = inet_addr("127.0.0.1");
    	// 轉換 IP
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	char test[256];
	int counter = 0;

	while(counter<30){
		pthread_mutex_lock(&tcp_ack_mutex);
		if(tcp_ack_bool == false){
			// 沒有新的 TCP 封包要 ACK，就睡在條件變數
			pthread_cond_wait(&tcp_ack_cond, &tcp_ack_mutex);
		}
		else{
			// 有新的 TCP 封包，需要送 ACK 給 router
			sendto(server_fd, test, sizeof(test), 0,
			       (struct sockaddr*)&address, (socklen_t)address_length);
			tcp_ack_bool = false;
			counter = counter+1;
		}
		pthread_mutex_unlock(&tcp_ack_mutex);
	}	

	close(server_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 4: udp_ack
// - server 端的「影片封包回 ACK 接收者」
// - 在 UDP_ACK_TO_SERVER_PORT 上收 router 轉來的 ACK
// - 搭配 udp_socket() 送出時的時間戳計算 RTT / ETE / AvgETE
// - 額外計算 Throughput = bits / RTT(ms)，每個封包一行輸出
// ------------------------------------------------------------
void *udp_ack(void *argu){
	int server_fd;
	struct sockaddr_in address;
    	socklen_t address_length;
    	address_length = sizeof(address);

    	// 建立 UDP socket，用來接收 router 的 ACK
    	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(server_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;
    	address.sin_port = htons(UDP_ACK_TO_SERVER_PORT);

    	// bind 到指定的 ACK port
    	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	char test[256];
    	int counter = 1;                       // GET 編號從 1 開始
    	float RTT, ETE, AvgETE, Throughput;   // RTT / ETE / 平滑 ETE / 本封包 throughput
    	struct Queue *queue = (struct Queue *)argu; // timestampQueue
    	struct timeval current_time;
    	struct timeval timestamp;

   	while(counter<31){
   		// 1. 收到 router 轉送來的 UDP ACK
   		recvfrom(server_fd, test, sizeof(test) , 0,
		         (struct sockaddr*)&address,  &address_length);

   		// 2. 從 queue 取出對應的送出時間，計算 RTT / ETE
		timestamp = dequeue(queue);
		gettimeofday(&current_time, NULL);

		RTT = (current_time.tv_sec - timestamp.tv_sec)*1000
		    +(current_time.tv_usec-timestamp.tv_usec)/1000.000;

		ETE = RTT/2.000;

   		if(counter==1){
   			// 第一個封包，用 ETE 自己當基準
   			AvgETE = ETE * 0.7 + ETE * 0.3;
   		}
   		else{
   			AvgETE = AvgETE * 0.7 + ETE * 0.3;
   		}

		//////////////////////////////////////////
		// 3. 計算本封包 Throughput = bits / RTT(ms)
		float bits = (float)(sizeof(test) * 8);
		float Throughput_bpm = bits / RTT;              // bits per ms
		float Throughput_Mbps = Throughput_bpm / 1000;  // Mbps
		/////////////////////////////////////////////   		


		// 4. 印出結果
		printf("-----GET--%02d--ACK-----\n",counter);
   		printf("RTT:%.3fms\n", RTT);
   		printf("ETE:%.3fms\n", ETE);
   		printf("AvgETE:%.3fms\n", AvgETE);
		printf("Throughput:%.6f Mbps\n", Throughput_Mbps);
   		printf("----------------------\n");

		counter = counter + 1;
   	}

   	close(server_fd);
	return NULL;
}

// ------------------------------------------------------------
// main
// - 建立 timestampQueue 給 UDP 影片封包使用
// - 啟動 4 個 thread：tcp_socket / udp_socket / tcp_ack / udp_ack
// ------------------------------------------------------------
int main(){
	struct Queue timestampQueue;
    	initQueue(&timestampQueue);

	pthread_t thread1, thread2,thread3, thread4;    

	pthread_create(&thread1, NULL, &tcp_socket, NULL);    
	pthread_create(&thread2, NULL, &udp_socket, &timestampQueue);
	pthread_create(&thread3, NULL, &tcp_ack, NULL);
	pthread_create(&thread4, NULL, &udp_ack, &timestampQueue);

	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	pthread_join(thread3, NULL);
	pthread_join(thread4, NULL);

	return 0;
}
