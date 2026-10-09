# Makefile — 一次編譯兩個版本的三支程式，並可一鍵啟動模擬
#
#   make            編譯 p1p2 與 p2-throughput（輸出到 bin/）
#   make run        執行 p1p2：依序啟動 server → router → client，輸出存到 logs/
#   make run-p2     執行 p2-throughput 版本
#   make clean      刪除 bin/ 與 logs/
#
# 需要 Linux（POSIX sockets + pthreads）。

CC       := gcc
CXX      := g++
CFLAGS   := -O2 -Wall
LDLIBS   := -lpthread
BIN      := bin
LOGS     := logs
# 每次模擬最多跑幾秒（server / router 的接收迴圈不會自己結束，時間到就關閉）
RUN_SECS ?= 20

P1 := $(BIN)/p1p2-server $(BIN)/p1p2-router $(BIN)/p1p2-client
P2 := $(BIN)/p2-server $(BIN)/p2-router $(BIN)/p2-client

.PHONY: all run run-p2 clean

all: $(P1) $(P2)

$(BIN) $(LOGS):
	mkdir -p $@

# ---- p1p2：router 為 C，server / client 為 C++ ----
$(BIN)/p1p2-router: p1p2/router.c p1p2/packet.h | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/p1p2-server: p1p2/server.cpp p1p2/packet.h | $(BIN)
	$(CXX) $(CFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/p1p2-client: p1p2/client.cpp p1p2/packet.h | $(BIN)
	$(CXX) $(CFLAGS) $< -o $@ $(LDLIBS)

# ---- p2-throughput：三支都是 C ----
$(BIN)/p2-%: p2-throughput/%.c | $(BIN)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# 啟動順序：server 先 listen → router 連到 server → client 連到 router。
# server 與 router 放背景執行，client 跑完（或逾時）後把背景行程一起關掉。
# stdbuf -oL 讓 printf 逐行寫入 log，被 kill 時不會遺失緩衝區內容。
define simulate
	@echo "== $(1): server → router → client（最多 $(RUN_SECS) 秒）"
	@stdbuf -oL ./$(BIN)/$(1)-server > $(LOGS)/$(1)-server.log 2>&1 & echo $$! > $(LOGS)/.server.pid
	@sleep 1
	@stdbuf -oL ./$(BIN)/$(1)-router > $(LOGS)/$(1)-router.log 2>&1 & echo $$! > $(LOGS)/.router.pid
	@sleep 1
	@timeout $(RUN_SECS) stdbuf -oL ./$(BIN)/$(1)-client > $(LOGS)/$(1)-client.log 2>&1 || true
	@kill $$(cat $(LOGS)/.server.pid) $$(cat $(LOGS)/.router.pid) 2>/dev/null || true
	@rm -f $(LOGS)/.server.pid $(LOGS)/.router.pid
	@echo "完成，輸出在 $(LOGS)/$(1)-{server,router,client}.log"
endef

run: $(P1) | $(LOGS)
	$(call simulate,p1p2)

run-p2: $(P2) | $(LOGS)
	$(call simulate,p2)

clean:
	rm -rf $(BIN) $(LOGS)
