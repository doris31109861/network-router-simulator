# Changelog

## 2026-10-09 — 加入 Makefile 與 CI

- **內容**：新增 `Makefile`：`make` 編譯兩個版本共 6 支程式到 `bin/`；`make run` / `make run-p2` 依序啟動 server → router → client（背景執行、逾時自動關閉），輸出存到 `logs/`。新增 GitHub Actions（`.github/workflows/build.yml`）在 Ubuntu 上自動編譯並跑一次模擬、上傳 log。README 編譯步驟改用 make。
- **原因**：原本要手動編譯三支程式、開三個終端機，別人很難直接試跑。
- **測試**：本機（Windows）只用 `mingw32-make -n` 確認指令展開正確；實際編譯與執行交給 GitHub Actions 驗證（結果見下一筆紀錄）。
