// ============================================================
// router_v2.c
// ------------------------------------------------------------
// 角色：Router（中間節點）
// - TCP 方向：
//     client →(TCP)→ router →(TCP)→ server
//   收到 client 的 TCP 封包之後，放到 tcpqueue；
//   從 tcpqueue 取出時，計算 queuing time / service time / queuing delay / AvgQueuingDelay。
// - UDP 方向：
//     server →(UDP)→ router →(UDP)→ client
//   收到 server 的 UDP 封包之後，放到 udpqueue；
//   從 udpqueue 取出時一樣計算 queue 相關時間。
// - ACK 方向：
//   - 從 server 收到 TCP ACK (UDP_ACK_TO_ROUTER_PORT)，再轉送給 client
//   - 從 client 收到 UDP ACK (UDP_ACK_TO_ROUTER_PORT)，再轉送給 server
// ============================================================

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>    
#include <unistd.h>
#include <sys/time.h> 
#include <arpa/inet.h>

#define PORT 8957                     // Router 和 client / server 溝通用的 PORT（TCP or UDP）
#define SERVER_PORT 8956              // Router 與 server 建立 TCP 連線時用的 port
#define CLIENT_PORT 8955              // Router 與 client 的 UDP port
#define TCP_ACK_TO_ROUTER_PORT 9000   // Router 收到 server 送來的 TCP ACK 的 UDP port
#define TCP_ACK_TO_CLIENT_PORT 9001   // Router 把 TCP ACK 轉送給 client 的 UDP port
#define UDP_ACK_TO_ROUTER_PORT 9002   // Router 收到 client 送來的 UDP ACK 的 UDP port
#define UDP_ACK_TO_SERVER_PORT 9003   // Router 把 UDP ACK 轉送給 server 的 UDP port
#define IP "127.0.0.1"
#define QUEUE_SIZE 100

// ---------------------- TCP ACK 同步旗標 ----------------------
bool tcp_ack_bool = false;
pthread_mutex_t tcp_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t tcp_ack_cond = PTHREAD_COND_INITIALIZER;

// ---------------------- UDP ACK 同步旗標 ----------------------
bool udp_ack_bool = false;
pthread_mutex_t udp_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t udp_ack_cond = PTHREAD_COND_INITIALIZER;

// ---------------------- 通用 Queue 結構（資料 + 進入時間） ----------------------
struct Queue{
	char *data[QUEUE_SIZE];	          // 指向封包 payload（以 strdup 儲存字串）
	int front;                         // 取出位置
    	int rear;                         // 放入位置
    	int size;                         // 目前封包數量
	struct timeval entry_time[QUEUE_SIZE]; // 每個封包進 queue 時的時間戳
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

// 將一個封包指標及其進入時間放入 queue，並更新 queue 長度
void enqueue(struct Queue *queue, char *item, int *qlength, struct timeval *time) {
    pthread_mutex_lock(&queue->mutex);
    while(queue->size >= QUEUE_SIZE){
        // queue 已滿，等待有空位
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    // 將 payload 指標存入 data，並紀錄入 queue 的時間
    //queue->data[queue->rear] = *item;
    queue->data[queue->rear] = item;
    queue->entry_time[queue->rear] = *time;
    queue->rear = (queue->rear + 1) % QUEUE_SIZE;
    queue->size++;
    *qlength = queue->size;           // 回傳目前 queue 長度
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

// 從 queue 取出封包與對應入 queue 時間，並回傳指向 payload 的指標
char *dequeue(struct Queue *queue, int *qlength, struct timeval *time) {
    pthread_mutex_lock(&queue->mutex);
    while(queue->size <= 0){
        // queue 為空，等待有資料
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    char *item = queue->data[queue->front];
    *time = queue->entry_time[queue->front];          // 把對應的 entry_time 也帶回去
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->size--;
    *qlength = queue->size;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return item;
}

// 銷毀 Queue 的同步原語
void destroyQueue(struct Queue *queue) {
	pthread_mutex_destroy(&queue->mutex);
	pthread_cond_destroy(&queue->cond);
}

// ------------------------------------------------------------
// Thread 1: tcp_socket_client
// - 建立 TCP passive socket（listen）在 PORT 上
// - 接受 client 的 TCP 連線，收到 data 後丟進 tcpqueue
// - 只負責「RecvTCP」端，計算 queue 長度，暫不算 delay
// ------------------------------------------------------------
void *tcp_socket_client(void *argu){
	int router_fd, new_socket;
	struct sockaddr_in address;
    	int addrlen = sizeof(address);

    	// 建立 TCP socket
    	router_fd = socket(AF_INET, SOCK_STREAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	else{
    		printf("create success\n");
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;
    	address.sin_port = htons(PORT);

    	// 綁定 router 的 TCP 接收 port
    	if (bind(router_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	// 開始 listen
   	 if (listen(router_fd, 3) < 0) {
    		printf("listen error\n");
    		pthread_exit(0);
   	 }
   	 
   	// 等 client 連進來
    	if ((new_socket = accept(router_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
    		printf("accept error\n");
    		pthread_exit(0);
   	}

	char temp[256];                    // 收到的 TCP payload
	int qlength;                        // 當前 queue 長度
	struct Queue *queue = (struct Queue*)argu;  // 指向 tcpqueue
	struct timeval  current_time;
	int counter = 0;

   	while(counter<30){
   		// 從 client 收 TCP
   		recv(new_socket, temp, sizeof(temp), 0);

   		// 紀錄進 queue 時間
   		gettimeofday(&current_time, NULL);

   		// 丟進 tcpqueue，使用 strdup 複製字串內容
   		enqueue(queue, strdup(temp), &qlength, &current_time);

   		printf("-----RecvTCP-----\n");
   		printf("QueueLength:%d\n", qlength);
   		printf("----------------\n");

   		counter = counter+1;
   	}

   	close(router_fd);
 	close(new_socket);
	return NULL;
}

// ------------------------------------------------------------
// Thread 2: tcp_socket_server
// - 建立 TCP active socket 連到 server 的 SERVER_PORT
// - 從 tcpqueue 取封包，計算：
//     staytime  = 封包在 queue 中等待的時間
//     servicetime = 模擬從 router 傳到 server 的服務時間 (sleep 30ms)
//     qdelay    = staytime + servicetime
//     avgqdelay = 0.7*舊值 + 0.3*新值
// - 列印上述資訊後將資料送給 server
// ------------------------------------------------------------
void *tcp_socket_server(void *argu){
   	int router_fd;
   	struct sockaddr_in address;

    	// 建立 TCP socket
    	router_fd = socket(AF_INET, SOCK_STREAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	else{
    		printf("create success\n");
    	}
    	
    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(SERVER_PORT);  // server 的 TCP port
	address.sin_addr.s_addr = INADDR_ANY; 
    	// 將 IP 字串轉為二進位形式（這裡用 127.0.0.1）
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	// 連線到 server
	if (connect(router_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { //(socket, ip and port , size)
		printf("connect error\n");
		pthread_exit(0);
	}

	struct Queue *queue = (struct Queue*)argu;  // 指向 tcpqueue
	float staytime;                             // 在 queue 中等待的時間
	float servicetime;                          // 模擬傳送服務時間
	float qdelay = 0;                           // 這次封包的 queueing delay
	float avgqdelay = 0;                        // 平滑平均 queueing delay
	int qlength;
	int counter = 0;
	struct timeval entrytime;                   // 封包進 queue 的時間
	struct timeval currenttime;                 // 封包離開 queue 的時間（開始服務）

	while(counter<30){
		struct timeval servicestart;
		struct timeval serviceend;

		// 從 queue 取出一個封包與其 entrytime
		char *data = dequeue(queue, &qlength, &entrytime);

		// 計算在 queue 裡待的時間 (ms)
		gettimeofday(&currenttime, NULL);// 紀錄出 queue 開始服務的時間
		staytime = (currenttime.tv_sec - entrytime.tv_sec) * 1000
		         + (currenttime.tv_usec - entrytime.tv_usec)/1000.000;

		// 模擬「服務時間」：從 router 到 server 的處理/傳輸時間
		gettimeofday(&servicestart, NULL);
		usleep(30*1000);  // sleep 30ms
		gettimeofday(&serviceend, NULL);

		servicetime = (serviceend.tv_sec - servicestart.tv_sec) * 1000
		            + (serviceend.tv_usec - servicestart.tv_usec)/1000.000;

		// QueueingDelay = 等待時間 + 服務時間
		qdelay = staytime + servicetime;

		// 平滑平均（第一個封包要特別處理）
		if(counter==0){//第一個封包
			avgqdelay = qdelay * 0.7 + qdelay * 0.3;
		}
		else{
			avgqdelay = avgqdelay * 0.7 + qdelay * 0.3; // 0.7 舊值 + 0.3 新值
		}

		// 印出 queueing 統計資訊
		printf("-----SendTCP-----\n");
   		printf("QueueLength:%d\n", qlength);
   		printf("QueuingTime:%.3fms\n", staytime);
   		printf("ServiceTime:%.3fms\n", servicetime);
   		printf("QueuingDelay:%.3fms\n", qdelay);
   		printf("AvgQueuingDelay:%.3fms\n", avgqdelay);
   		printf("-----------------\n");

   		// 將封包送到 server
   		send(router_fd, data, sizeof(data), 0);

   		counter = counter + 1;

   		// data 是用 strdup 配的，要 free
   		free(data);

   		// 稍微 sleep 避免送太快
   		usleep(5000);
   	}

   	close(router_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 3: udp_socket_server
// - 在 PORT 上以 UDP 收到從 server 來的「影片封包」
// - 每收到一個封包就 enqueue 到 udpqueue，並印出 QueueLength
// ------------------------------------------------------------
void *udp_socket_server(void *argu){
	int router_fd;
	struct sockaddr_in address;
    	socklen_t address_length;
    	address_length = sizeof(address);

    	// 建立 UDP socket
    	router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	else{
    		printf("create success\n");
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;
    	address.sin_port = htons(PORT);

    	// 綁定 router 的 UDP port
    	if (bind(router_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	char temp[256];
    	int qlength;
    	int counter = 0;
	struct Queue *queue = (struct Queue*)argu;  // 指向 udpqueue
	struct timeval current_time;

   	while(counter<30){
   		// 收到 server 的 UDP 資料
   		recvfrom(router_fd, temp, sizeof(temp) , 0,
		         (struct sockaddr*)&address,  &address_length);

   		// 封包進入 queue 的時間
   		gettimeofday(&current_time, NULL);
   		counter = counter + 1;

   		// 存入 udpqueue，並印 QueueLength
   		enqueue(queue, strdup(temp), &qlength, &current_time);
   		printf("-----RecvUDP-----\n");
   		printf("QueueLength:%d\n", qlength);
   		printf("----------------\n");
   	}

   	close(router_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 4: udp_socket_client
// - 建立 UDP active socket 連到 client 的 CLIENT_PORT
// - 從 udpqueue 中取出封包，計算 queueing delay（與 TCP 方向類似）
// - 再將封包送到 client
// ------------------------------------------------------------
void *udp_socket_client(void *argu){
	int router_fd;
   	struct sockaddr_in address;
   	socklen_t address_length;
   	address_length = sizeof(address);

    	// 建立 UDP socket
    	router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(CLIENT_PORT);  // client 接收影片的 port

    	// 轉換 IP 位址 (127.0.0.1)
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	struct Queue *queue = (struct Queue*)argu;  // 指向 udpqueue
	float staytime;
	float servicetime;
	float qdelay = 0;
	float avgqdelay = 0;
	int qlength;
	int counter = 0;
	struct timeval entrytime;
	struct timeval currenttime;

	while(counter<30){
		struct timeval servicestart;
		struct timeval serviceend;

		// 從 udpqueue dequeue 一個封包及其 entrytime
		char *data = dequeue(queue, &qlength, &entrytime);

		// 計算在 queue 中等待時間
		gettimeofday(&currenttime, NULL);//紀錄送達時間
		staytime = (currenttime.tv_sec - entrytime.tv_sec) * 1000
		         + (currenttime.tv_usec - entrytime.tv_usec)/1000.000;

		// 模擬服務時間：這裡用 100ms
		gettimeofday(&servicestart, NULL);
		usleep(100*1000);
		gettimeofday(&serviceend, NULL);
		servicetime = (serviceend.tv_sec - servicestart.tv_sec) * 1000
		            + (serviceend.tv_usec - servicestart.tv_usec)/1000.000;

		// QueueingDelay = staytime + servicetime
		qdelay = staytime + servicetime;

		// 更新 AvgQueuingDelay（平滑）
		if(counter==0){
			avgqdelay = qdelay * 0.7 + qdelay * 0.3;
		}
		else{
			avgqdelay = avgqdelay * 0.7 + qdelay * 0.3;
		}

		// 印出 queueing 統計資訊
		printf("-----SendUDP-----\n");
   		printf("QueueLength:%d\n", qlength);
   		printf("QueuingTime:%.3fms\n", staytime);
   		printf("ServiceTime:%.3fms\n", servicetime);
   		printf("QueuingDelay:%.3fms\n", qdelay);
   		printf("AvgQueuingDelay:%.3fms\n", avgqdelay);
   		printf("-----------------\n");

   		// 將資料送到 client
   		sendto(router_fd, data, sizeof(data), 0,
		       (struct sockaddr*)&address, (socklen_t)address_length);
   		counter = counter + 1;

   		// 釋放 strdup 的記憶體
   		usleep(5000);
   		free(data);
   	}

   	close(router_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 5: tcp_ack_to_client
// - 將從 server 收到的「TCP ACK 事件」轉成實際 UDP 封包發給 client
// - 由 tcp_ack_bool / tcp_ack_cond 與 tcp_ack_from_server 同步
// ------------------------------------------------------------
void *tcp_ack_to_client(void *argu){
	int router_fd;
   	struct sockaddr_in address;
   	socklen_t address_length;
   	address_length = sizeof(address);

    	// 建立 UDP socket（往 client 送 ACK）
    	router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	
    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(TCP_ACK_TO_CLIENT_PORT);  // client 收 TCP ACK 的 port
	//address.sin_addr.s_addr = inet_addr("127.0.0.1");
    	// 轉換 IP (127.0.0.1)
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	char test[256];         // ACK payload
	int counter = 0;

	while(counter<30){
		pthread_mutex_lock(&tcp_ack_mutex);
		if(tcp_ack_bool == false){
			// 尚未收到 tcp_ack_from_server 的通知，等待
			pthread_cond_wait(&tcp_ack_cond, &tcp_ack_mutex);
		}
		else{
			// 已經收到通知，往 client 送一個 UDP ACK
			sendto(router_fd, test, sizeof(test), 0,
			       (struct sockaddr*)&address, (socklen_t)address_length);
			tcp_ack_bool = false;
			counter = counter+1;
		}
		pthread_mutex_unlock(&tcp_ack_mutex);
	}	

	close(router_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 6: tcp_ack_from_server
// - 在 TCP_ACK_TO_ROUTER_PORT 上用 UDP 接收 server 傳來的 TCP ACK
// - 每收到一個，就設置 tcp_ack_bool = true 並喚醒 tcp_ack_to_client
// ------------------------------------------------------------
void *tcp_ack_from_server(void *argu){
	int router_fd;
	struct sockaddr_in address;
    	socklen_t address_length;
    	address_length = sizeof(address);

    	// 建立 UDP socket
    	router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;
    	address.sin_port = htons(TCP_ACK_TO_ROUTER_PORT);

    	// bind 到 router 用來收 server ACK 的 port
    	if (bind(router_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	char test[256];
    	int counter = 0;

   	while(counter<30){
   		// 收到 server 發來的 UDP ACK
   		recvfrom(router_fd, test, sizeof(test) , 0,
		         (struct sockaddr*)&address,  &address_length);

   		// 通知 tcp_ack_to_client thread 可以送 ACK 出去了
   		pthread_mutex_lock(&tcp_ack_mutex);
		tcp_ack_bool = true;
		pthread_cond_signal(&tcp_ack_cond);
       		pthread_mutex_unlock(&tcp_ack_mutex);

		counter = counter + 1;
   	}

   	close(router_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 7: udp_ack_to_server
// - 將從 client 收到的「UDP ACK 事件」轉成實際 UDP 封包發給 server
// - 與 udp_ack_from_client 透過 udp_ack_bool / udp_ack_cond 同步
// ------------------------------------------------------------
void *udp_ack_to_server(void *argu){
	int router_fd;
   	struct sockaddr_in address;
   	socklen_t address_length;
   	address_length = sizeof(address);

    	// 建立 UDP socket（往 server 發送 ACK）
    	router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}
    	
    	address.sin_family = AF_INET;           // IPv4   
	address.sin_port = htons(UDP_ACK_TO_SERVER_PORT);  // server 收 ACK 的 port
	//address.sin_addr.s_addr = inet_addr("127.0.0.1");
    	// 轉換 IP (127.0.0.1)
	if(inet_pton(AF_INET, IP, &address.sin_addr) <= 0) { 
		printf("IP error\n");
		pthread_exit(0);
	}

	char test[256];
	int counter = 0;

	while(counter<30){
		pthread_mutex_lock(&udp_ack_mutex);
		if(udp_ack_bool == false){
			// 尚未收到 client ACK，等待
			pthread_cond_wait(&udp_ack_cond, &udp_ack_mutex);
		}
		else{
			// 有 client ACK，要轉送給 server
			sendto(router_fd, test, sizeof(test), 0,
			       (struct sockaddr*)&address, (socklen_t)address_length);
			udp_ack_bool = false;
			counter = counter+1;
		}
		pthread_mutex_unlock(&udp_ack_mutex);
	}	

	close(router_fd);
	return NULL;
}

// ------------------------------------------------------------
// Thread 8: udp_ack_from_client
// - 在 UDP_ACK_TO_ROUTER_PORT 上用 UDP 接收 client 送來的 ACK
// - 每收到一個，就設 udp_ack_bool = true 並喚醒 udp_ack_to_server
// ------------------------------------------------------------
void *udp_ack_from_client(void *argu){
	int router_fd;
	struct sockaddr_in address;
    	socklen_t address_length;
    	address_length = sizeof(address);

    	// 建立 UDP socket
    	router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if(router_fd < 0){
    		printf("create error\n");
    		pthread_exit(0);
    	}

    	address.sin_family = AF_INET;
    	address.sin_addr.s_addr = INADDR_ANY;
    	address.sin_port = htons(UDP_ACK_TO_ROUTER_PORT);

    	// bind 到 router 收 client ACK 的 port
    	if (bind(router_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    		printf("bind error\n");
    		pthread_exit(0);
    	}   

    	char test[256];
    	int counter = 0;

   	while(counter<30){
   		// 接收 client 的 UDP ACK
   		recvfrom(router_fd, test, sizeof(test) , 0,
		         (struct sockaddr*)&address,  &address_length);

   		// 通知 udp_ack_to_server 可以把 ACK 轉送出去
   		pthread_mutex_lock(&udp_ack_mutex);
		udp_ack_bool = true;
		pthread_cond_signal(&udp_ack_cond);
       		pthread_mutex_unlock(&udp_ack_mutex);

		counter = counter + 1;
   	}
   	close(router_fd);
}

// ------------------------------------------------------------
// main
// - 建立兩個 Queue：
//     tcpqueue：存 TCP 方向封包與 entry time
//     udpqueue：存 UDP 方向封包與 entry time
// - 建立 8 個 thread 負責收/送資料與 ACK 轉發
// ------------------------------------------------------------
int main(){
	struct Queue tcpqueue;
	struct Queue udpqueue;
	initQueue(&tcpqueue);
	initQueue(&udpqueue);

	pthread_t thread1, thread2, thread3, thread4, thread5, thread6, thread7, thread8;    

	pthread_create(&thread1, NULL, &tcp_socket_client, &tcpqueue);
	pthread_create(&thread2, NULL, &udp_socket_server, &udpqueue);
	pthread_create(&thread3, NULL, &tcp_socket_server, &tcpqueue);    
	pthread_create(&thread4, NULL, &udp_socket_client, &udpqueue); 
	pthread_create(&thread5, NULL, &tcp_ack_to_client, NULL);
	pthread_create(&thread6, NULL, &tcp_ack_from_server, NULL);
	pthread_create(&thread7, NULL, &udp_ack_to_server, NULL);
	pthread_create(&thread8, NULL, &udp_ack_from_client, NULL);

	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);
	pthread_join(thread3, NULL);
	pthread_join(thread4, NULL);
	pthread_join(thread5, NULL);
	pthread_join(thread6, NULL);
	pthread_join(thread7, NULL);
	pthread_join(thread8, NULL);

	destroyQueue(&tcpqueue);
	destroyQueue(&udpqueue);
	return 0;
}
