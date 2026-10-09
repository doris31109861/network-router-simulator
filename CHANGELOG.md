# Changelog

## 2026-10-09 — 封包數與 port 改成命令列參數

- **內容**：`packet.h` 新增 `parse_args()`：`-n <封包數>`、`-p <基準 port>`（Server = 基準、Router = +2、Client = +3），預設仍為 23 個封包、port 9000；`LOOP_COUNT` 與各 port 巨集改為讀取全域變數，三支程式的 `main` 開頭呼叫 `parse_args`。Makefile 新增 `ARGS` 變數（`make run ARGS="-n 50 -p 9100"`）。CI 多跑一次 `-n 10 -p 9100` 並檢查 client 收到 10 個 TCP ACK。
- **原因**：原本要改封包數或 port 必須改程式重新編譯。
- **測試**：本機以 C 與 C++ 各編譯一個測試程式，確認 `-n`、`-p` 與預設值正確、不合法參數會回傳 1，`sizeof(Packet)` 為 1516；實際模擬由 CI 驗證。

## 2026-10-09 — 抽出共用標頭檔 packet.h（並修正封包大小不一致）

- **內容**：新增 `p1p2/packet.h`，集中 MTU、PACKET_SIZE、port、虛擬 IP 與 `IPHeader`／`UDPHeader`／`MACHeader`／`TCPHeader`／`Packet` 定義；client、server、router 刪除各自的副本改為 include，寫死的虛擬 IP 改用常數。加上 `static_assert` 確保 `sizeof(Packet) <= PACKET_SIZE`。Makefile 加上對 `packet.h` 的相依。
- **原因**：三份結構原本不一致：router 的 `IPHeader` 多一個 `options` 欄位，三邊 `Packet` 的 payload 分別是 1472／1460／1472 bytes。client 回傳的 ACK 有 1520 bytes，但 router 佇列每格只有 1518 bytes，`enqueue` 的 `memcpy` 會寫出陣列 2 bytes。統一後封包為 48 bytes 表頭＋1468 bytes payload（1516 bytes）。
- **測試**：GitHub Actions 編譯並執行模擬，和重構前的 CI 結果相比，client 23 筆、router 69 筆紀錄數相同，平均 RTT 72.37 → 72.34 ms、UDP 平均排隊延遲 428.62 → 428.78 ms，行為一致。

## 2026-10-09 — p1p2 三支程式補上檔案說明註解

- **內容**：`p1p2/client.cpp`、`server.cpp`、`router.c` 開頭加上說明：各執行緒的角色、使用的 port、量測哪些指標（p2-throughput 版本原本就有完整檔頭）。
- **原因**：統一各檔案的註解風格。
- **測試**：只加註解；CI 重新編譯並執行模擬。

## 2026-10-09 — README 加入實驗結果圖表

- **內容**：把 GitHub Actions 實際跑出來的結果（`docs/delay.png`、`docs/client_metrics.csv`、`docs/router_metrics.csv`）放進 repo，README 新增「結果分析」段落，說明 RTT、TCP／UDP 佇列排隊延遲的差異。
- **原因**：讓 README 一打開就有圖，並展現對結果的分析。
- **測試**：圖表與數據來自 CI 實際執行（ubuntu-latest）。

## 2026-10-09 — 加入結果分析與圖表

- **內容**：新增 `scripts/plot_results.py`，把 `make run` 的 client／router log 解析成 `results/client_metrics.csv`、`results/router_metrics.csv`，並畫出 RTT／ETE 與排隊延遲的折線圖 `results/delay.png`；CI 跑完模擬後自動畫圖並上傳。
- **原因**：讓模擬結果可以量化呈現，而不只是終端機輸出。
- **測試**：本機以 README 範例 log 測試解析，CSV 正確；畫圖在 CI（安裝 matplotlib）執行。

## 2026-10-09 — 加入 Makefile 與 CI

- **內容**：新增 `Makefile`：`make` 編譯兩個版本共 6 支程式到 `bin/`；`make run` / `make run-p2` 依序啟動 server → router → client（背景執行、逾時自動關閉），輸出存到 `logs/`。新增 GitHub Actions（`.github/workflows/build.yml`）在 Ubuntu 上自動編譯並跑一次模擬、上傳 log。README 編譯步驟改用 make。
- **原因**：原本要手動編譯三支程式、開三個終端機，別人很難直接試跑。
- **測試**：本機（Windows）只用 `mingw32-make -n` 確認指令展開正確；GitHub Actions（ubuntu-latest）實測：6 支程式皆編譯成功（只有未使用變數等 warning），`make run` 與 `make run-p2` 都正常跑完，p1p2 的 23 個 TCP ACK 全部收到（RTT ≈ 30.5 ms、平均排隊延遲 ≈ 30.2 ms）。
