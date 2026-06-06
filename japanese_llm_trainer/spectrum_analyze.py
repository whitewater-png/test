#!/usr/bin/env python3
"""
Spectrum: Signal-to-Noise比に基づく学習レイヤー選択
論文: Spectrum: Targeted Training on Signal to Noise Ratio (2024)

仕組み:
  各レイヤーの重み行列のSNR（信号対雑音比）をランダム行列理論
  （Marchenko-Pastur分布）で推定する。SNRが高い＝情報量が多いレイヤー
  だけを学習対象にし、低SNRレイヤーは凍結する。
  → 同じメモリで効率的に品質を上げられる（学習する重みを絞るため）。

出力:
  spectrum_layers.json  : 学習対象レイヤーの推奨リスト
  人間可読のSNRレポート

使い方:
  python spectrum_analyze.py --model ./models/qwen3-14b --top-fraction 0.3
  → 上位30%のSNRを持つレイヤーを学習対象として推奨

  生成された spectrum_layers.json は 2d_train_neftune.py が
  --spectrum-layers spectrum_layers.json で読み込める。
"""

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path


def load_weight_tensors(model_path: str):
    """safetensorsから重みテンソルを遅延ロードする"""
    from safetensors import safe_open
    import glob

    files = sorted(glob.glob(str(Path(model_path) / "*.safetensors")))
    if not files:
        raise FileNotFoundError(f"safetensorsが見つかりません: {model_path}")

    tensors = {}
    for fpath in files:
        with safe_open(fpath, framework="numpy") as f:
            for key in f.keys():
                tensors[key] = fpath  # キー → ファイルのマップ（遅延ロード）
    return tensors, files


def estimate_snr(weight) -> float:
    """
    Marchenko-Pastur分布を用いて重み行列のSNRを推定する。
    特異値分布のうち、MPの理論的上限を超える成分を「信号」、
    残りを「ノイズ」とみなす。
    """
    import numpy as np

    W = np.asarray(weight, dtype=np.float32)
    if W.ndim != 2:
        return 0.0

    m, n = W.shape
    if m < 2 or n < 2:
        return 0.0

    # 特異値（大きい行列は計算コスト削減のためサンプリング）
    try:
        # 経済的SVD
        s = np.linalg.svd(W, compute_uv=False)
    except np.linalg.LinAlgError:
        return 0.0

    # Marchenko-Pastur 上限: sigma^2 * (1 + sqrt(q))^2,  q = min/max
    q = min(m, n) / max(m, n)
    # ノイズ分散の推定（中央値ベースで頑健に）
    sigma2 = np.median(s ** 2) / (1 + math.sqrt(q)) ** 2
    if sigma2 <= 0:
        return 0.0
    mp_upper = sigma2 * (1 + math.sqrt(q)) ** 2

    # 信号 = MP上限を超える特異値の二乗和、ノイズ = 残り
    signal = np.sum((s ** 2)[s ** 2 > mp_upper])
    noise = np.sum((s ** 2)[s ** 2 <= mp_upper])
    if noise <= 0:
        return float("inf")
    return float(signal / noise)


def parse_layer_index(key: str) -> int | None:
    """重みキーからレイヤー番号を抽出（例: model.layers.12.self_attn.q_proj.weight → 12）"""
    import re
    m = re.search(r"layers\.(\d+)\.", key)
    return int(m.group(1)) if m else None


# 解析対象とする射影タイプ（attention と MLP の主要線形層）
TARGET_PROJECTIONS = [
    "self_attn.q_proj", "self_attn.k_proj", "self_attn.v_proj", "self_attn.o_proj",
    "mlp.gate_proj", "mlp.up_proj", "mlp.down_proj",
]


def main():
    parser = argparse.ArgumentParser(description="Spectrum SNR レイヤー解析")
    parser.add_argument("--model", default="./models/qwen3-14b")
    parser.add_argument("--top-fraction", type=float, default=0.3,
                        help="学習対象とする上位SNRレイヤーの割合（0.3 = 上位30%）")
    parser.add_argument("--output", default="spectrum_layers.json")
    parser.add_argument("--report", default="spectrum_report.txt")
    args = parser.parse_args()

    print("=" * 60)
    print("Spectrum SNR レイヤー解析")
    print(f"  モデル: {args.model}")
    print(f"  上位割合: {args.top_fraction:.0%}")
    print("=" * 60)

    try:
        import numpy as np  # noqa: F401
        from safetensors import safe_open
    except ImportError:
        print("依存パッケージが不足しています:")
        print("  pip install numpy safetensors")
        return

    tensor_map, files = load_weight_tensors(args.model)

    # レイヤー × 射影 ごとにSNRを計算
    print(f"\n{len(tensor_map)}個のテンソルを解析中...")

    layer_snr: dict[int, dict[str, float]] = defaultdict(dict)
    import glob
    from safetensors import safe_open

    # ファイルごとに開いて該当テンソルを処理（メモリ効率）
    processed = 0
    for fpath in files:
        with safe_open(fpath, framework="numpy") as f:
            for key in f.keys():
                layer_idx = parse_layer_index(key)
                if layer_idx is None:
                    continue
                proj = next((p for p in TARGET_PROJECTIONS if p in key), None)
                if proj is None or not key.endswith(".weight"):
                    continue
                tensor = f.get_tensor(key)
                snr = estimate_snr(tensor)
                layer_snr[layer_idx][proj] = snr
                processed += 1
                if processed % 20 == 0:
                    print(f"  {processed}層処理...", end="\r")

    print(f"  {processed}個の射影を解析完了")

    # レイヤーごとの平均SNRを計算
    layer_avg_snr = {
        idx: sum(projs.values()) / len(projs)
        for idx, projs in layer_snr.items()
        if projs
    }

    # SNR降順にソート
    ranked = sorted(layer_avg_snr.items(), key=lambda x: x[1], reverse=True)
    n_select = max(1, int(len(ranked) * args.top_fraction))
    selected_layers = sorted([idx for idx, _ in ranked[:n_select]])

    # レポート生成
    lines = []
    lines.append("=" * 60)
    lines.append("Spectrum SNR レポート")
    lines.append(f"モデル: {args.model}")
    lines.append(f"総レイヤー数: {len(ranked)}")
    lines.append(f"学習対象（上位{args.top_fraction:.0%}）: {n_select}レイヤー")
    lines.append("=" * 60)
    lines.append("\nレイヤー別 平均SNR（降順）:")
    for rank, (idx, snr) in enumerate(ranked, 1):
        mark = "★学習" if idx in selected_layers else "  凍結"
        bar = "█" * min(int(snr * 2), 30)
        lines.append(f"  [{mark}] Layer {idx:3d}: SNR={snr:6.3f} {bar}")

    report = "\n".join(lines)
    print("\n" + report)

    with open(args.report, "w", encoding="utf-8") as f:
        f.write(report)

    # JSON出力（学習スクリプトが消費）
    output = {
        "model": args.model,
        "total_layers": len(ranked),
        "top_fraction": args.top_fraction,
        "selected_layers": selected_layers,
        "layer_snr": {str(k): round(v, 4) for k, v in layer_avg_snr.items()},
    }
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(output, f, ensure_ascii=False, indent=2)

    print(f"\n学習対象レイヤー: {selected_layers}")
    print(f"出力: {args.output} / {args.report}")
    print("\n次のステップ:")
    print(f"  python 2d_train_neftune.py --config config_14b.yaml --spectrum-layers {args.output}")


if __name__ == "__main__":
    main()
