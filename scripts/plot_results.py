"""
plot_results.py — 把模擬輸出的 log 整理成 CSV，並畫成折線圖

讀取 `make run` 產生的 logs/p1p2-client.log 與 logs/p1p2-router.log：
  - client：每個 ACK 的 RTT、ETE（≈ RTT/2）、EWMA 平均 ETE、Throughput
  - router：每個封包的 QueuingDelay 與 EWMA 平均 AvgQueuingDelay
輸出：
  results/client_metrics.csv、results/router_metrics.csv
  results/delay.png（RTT / ETE / 排隊延遲隨封包序號的變化）

用法：
    make run
    python3 scripts/plot_results.py            # 預設讀 logs/、輸出到 results/
    python3 scripts/plot_results.py --prefix p2  # 改讀 p2-throughput 版本的 log
"""

import argparse
import csv
import re
from pathlib import Path

# 每一行的格式都是「名稱:數值單位」，例如 "RTT:30.473ms"、"Throughput:16.802kbps"
LINE_RE = re.compile(r"^\s*(\w+)\s*:\s*([-+]?\d+(?:\.\d+)?)")


def parse_log(path, fields, start_field):
    """把 log 解析成一筆筆紀錄。

    每遇到 start_field（例如 RTT）就開始一筆新紀錄，之後出現的 fields 都歸到這一筆。
    """
    rows, cur = [], None
    for line in Path(path).read_text(encoding="utf-8", errors="ignore").splitlines():
        m = LINE_RE.match(line)
        if not m or m.group(1) not in fields:
            continue
        key, value = m.group(1), float(m.group(2))
        if key == start_field:
            if cur:
                rows.append(cur)
            cur = {}
        if cur is not None:
            cur[key] = value
    if cur:
        rows.append(cur)
    return rows


def write_csv(path, rows, fields):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["packet"] + fields)
        for i, r in enumerate(rows, 1):
            w.writerow([i] + [r.get(k, "") for k in fields])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="logs")
    ap.add_argument("--out", default="results")
    ap.add_argument("--prefix", default="p1p2", help="log 檔名前綴：p1p2 或 p2")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(exist_ok=True)

    client_fields = ["RTT", "ETE", "AvgETE", "Throughput"]
    router_fields = ["QueuingTime", "ServiceTime", "QueuingDelay", "AvgQueuingDelay"]
    client = parse_log(Path(args.logs) / f"{args.prefix}-client.log", client_fields, "RTT")
    router = parse_log(Path(args.logs) / f"{args.prefix}-router.log", router_fields, "QueuingTime")

    write_csv(out / "client_metrics.csv", client, client_fields)
    write_csv(out / "router_metrics.csv", router, router_fields)
    print(f"client: {len(client)} 筆, router: {len(router)} 筆 → {out}/*.csv")

    import matplotlib
    matplotlib.use("Agg")  # 不開視窗，直接存檔（CI 環境沒有螢幕）
    import matplotlib.pyplot as plt

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4))

    # 左圖：client 端的 RTT 與 ETE
    for key in ("RTT", "ETE", "AvgETE"):
        ys = [r[key] for r in client if key in r]
        ax1.plot(range(1, len(ys) + 1), ys, marker="o", markersize=3, label=key)
    ax1.set(title="Client: RTT / end-to-end delay", xlabel="ACK #", ylabel="ms")
    ax1.legend()
    ax1.grid(alpha=0.3)

    # 右圖：router 端的排隊延遲（含 30ms 服務時間）與 EWMA 平均
    for key in ("QueuingDelay", "AvgQueuingDelay"):
        ys = [r[key] for r in router if key in r]
        ax2.plot(range(1, len(ys) + 1), ys, marker="o", markersize=3, label=key)
    ax2.set(title="Router: queuing delay", xlabel="packet #", ylabel="ms")
    ax2.legend()
    ax2.grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(out / "delay.png", dpi=120)
    print(f"圖表已存到 {out}/delay.png")


if __name__ == "__main__":
    main()
