#!/usr/bin/env python3
"""
ORPO (Odds Ratio Preference Optimization) ファインチューニングスクリプト
SFT + DPO を1ステップで実行する効率的な手法

SFT → DPO の2段階より:
  - 計算時間: 約40%短縮
  - 品質:     同等〜やや向上（参照モデル不要のため安定）
  - メモリ:   DPOより少ない（参照モデルの追加ロード不要）

使い方:
    python 2c_orpo.py [--config config_14b.yaml] [--test-run]

参考論文: ORPO: Monolithic Preference Optimization without Reference Model (2024)
"""

import argparse
import subprocess
import sys
import time
import yaml
from pathlib import Path


DEFAULT_CONFIG = {
    "model_path": "./models/qwen3-14b",
    "dpo_data_dir": "./data/dpo",   # chosenとrejectedを含むDPOデータを流用
    "adapter_path": "./adapters/14b-japanese-orpo",
    "final_model_path": "./models/qwen3-14b-japanese-orpo",
    "beta": 0.1,
    "iters": 1000,
    "batch_size": 2,
    "learning_rate": 1e-4,
    "max_seq_length": 1024,
    "lora_layers": 8,
    "grad_checkpoint": True,
    "seed": 42,
}


def load_config(config_path: str | None) -> dict:
    config = DEFAULT_CONFIG.copy()
    if config_path and Path(config_path).exists():
        with open(config_path, encoding="utf-8") as f:
            raw = yaml.safe_load(f)
        # トップレベル設定を上書き
        for k in ["model_path", "lora_layers", "batch_size", "max_seq_length",
                  "grad_checkpoint", "seed"]:
            if k in raw:
                config[k] = raw[k]
        # orpoセクションがあればそちらを優先
        if "orpo" in raw:
            config.update(raw["orpo"])
    return config


def check_mlx_orpo_support() -> bool:
    """mlx_lm が ORPO をサポートしているか確認"""
    result = subprocess.run(
        [sys.executable, "-m", "mlx_lm.lora", "--help"],
        capture_output=True, text=True, check=False,
    )
    return "--loss" in result.stdout or "orpo" in result.stdout.lower()


def build_orpo_command(config: dict, test_run: bool = False) -> list[str]:
    """
    mlx_lm.lora --loss orpo コマンドを構築。
    ORPOはLoRAのオプションとして --loss orpo で指定する。
    データ形式はDPOと同じ（prompt/chosen/rejected）。
    """
    iters = 30 if test_run else config["iters"]

    cmd = [
        sys.executable, "-m", "mlx_lm.lora",
        "--model", config["model_path"],
        "--train",
        "--loss", "orpo",           # ← ORPO損失関数を指定
        "--data", config["dpo_data_dir"],
        "--adapter-path", config["adapter_path"],
        "--iters", str(iters),
        "--batch-size", str(config["batch_size"]),
        "--learning-rate", str(config["learning_rate"]),
        "--lora-layers", str(config["lora_layers"]),
        "--max-seq-length", str(config["max_seq_length"]),
        "--beta", str(config["beta"]),
        "--seed", str(config["seed"]),
    ]

    if config.get("grad_checkpoint"):
        cmd.append("--grad-checkpoint")

    return cmd


def run_orpo(config: dict, test_run: bool = False):
    Path(config["adapter_path"]).mkdir(parents=True, exist_ok=True)

    # MLXのORPOサポートを確認
    if not check_mlx_orpo_support():
        print("警告: このバージョンのmlx-lmはORPOを直接サポートしていない可能性があります")
        print("  mlx-lm >= 0.21.0 が必要です: pip install --upgrade mlx-lm")
        print("  続行しますが、エラーが出た場合は 2_finetune.py + 2b_dpo.py を使ってください\n")

    cmd = build_orpo_command(config, test_run)

    if test_run:
        print("\n=== ORPO テスト実行（30ステップ） ===")
    else:
        print(f"\n=== ORPO トレーニング開始 ({config['iters']}ステップ） ===")

    print(f"モデル: {config['model_path']}")
    print(f"beta={config['beta']} | batch={config['batch_size']} | lr={config['learning_rate']}")
    print(f"LoRAレイヤー: {config['lora_layers']} | 最大シーケンス長: {config['max_seq_length']}")
    print("\n学習中... (Ctrl+C で中断可能)")
    print("=" * 60)

    start = time.time()
    result = subprocess.run(cmd, check=False)
    elapsed = time.time() - start

    if result.returncode == 0:
        print(f"\n完了！ 経過時間: {elapsed / 60:.1f}分")
        print(f"ORPOアダプター: {config['adapter_path']}")
        return True
    else:
        print(f"\nORPOエラー (終了コード: {result.returncode})")
        print("フォールバック: 2_finetune.py → 2b_dpo.py の順に実行してください")
        return False


def fuse_model(config: dict):
    print("\n=== ORPOモデルのマージ ===")
    final_path = Path(config["final_model_path"])
    final_path.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable, "-m", "mlx_lm.fuse",
        "--model", config["model_path"],
        "--adapter-path", config["adapter_path"],
        "--save-path", str(final_path),
    ]

    result = subprocess.run(cmd, check=False)
    if result.returncode == 0:
        print(f"マージ完了: {final_path}")
    else:
        print("マージ失敗。アダプターを直接使用してください。")


def show_memory_estimate(config: dict):
    model_name = Path(config["model_path"]).name
    is_14b = "14b" in model_name.lower()
    model_mem = "~9GB" if is_14b else "~5GB"
    train_mem = "~20GB" if is_14b else "~10GB"
    free_mem = "~16GB" if is_14b else "~26GB"

    print(f"""
メモリ使用量の目安（M4 Max 36GB）:
  モデル（4bit量子化）: {model_mem}
  ORPOトレーニング合計: {train_mem}  ← DPOより少ない（参照モデル不要）
  他アプリ用空き:        {free_mem}
""")


def main():
    parser = argparse.ArgumentParser(description="ORPO 日本語品質向上トレーニング")
    parser.add_argument("--config", default="config_14b.yaml")
    parser.add_argument("--test-run", action="store_true")
    parser.add_argument("--fuse-only", action="store_true")
    parser.add_argument("--skip-fuse", action="store_true")
    args = parser.parse_args()

    print("=" * 60)
    print("ORPO 日本語品質向上トレーニング")
    print("SFT + DPO を1ステップで実行（効率的・高品質）")
    print("=" * 60)

    config = load_config(args.config)
    show_memory_estimate(config)

    # 前提確認
    dpo_train = Path(config["dpo_data_dir"]) / "train.jsonl"
    if not dpo_train.exists():
        print(f"エラー: DPOデータが見つかりません: {dpo_train}")
        print("→ python 1_prepare_dataset.py を先に実行してください")
        sys.exit(1)

    if not Path(config["model_path"]).exists():
        print(f"エラー: モデルが見つかりません: {config['model_path']}")
        print("→ huggingface-cli download Qwen/Qwen3-14B でダウンロードしてください")
        sys.exit(1)

    if args.fuse_only:
        fuse_model(config)
        return

    success = run_orpo(config, test_run=args.test_run)

    if success and not args.test_run and not args.skip_fuse:
        fuse_model(config)
        print("\n次のステップ: python 3_evaluate.py --model-path", config["final_model_path"], "--compare --use-llm-judge")
    elif args.test_run and success:
        print("\nテスト成功。本番実行:")
        print("  python 2c_orpo.py --config config_14b.yaml")


if __name__ == "__main__":
    main()
