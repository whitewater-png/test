#!/usr/bin/env python3
"""
DPO (Direct Preference Optimization) ファインチューニングスクリプト
SFT（2_finetune.py）完了後に実行してください

効果: 自然さ・敬語の正確性・文体一貫性を大幅に向上

使い方:
    python 2b_dpo.py [--config config.yaml] [--test-run]

前提条件:
    - python 1_prepare_dataset.py 実行済み（data/dpo/ が存在すること）
    - python 2_finetune.py 実行済み（SFTアダプターまたはマージ済みモデルが存在すること）
    - mlx-lm >= 0.21.0
"""

import argparse
import subprocess
import sys
import time
import yaml
from pathlib import Path


# ---------------------------------------------------------------------------
# デフォルト設定
# ---------------------------------------------------------------------------
DEFAULT_CONFIG = {
    # SFT後のモデルパス（LoRAマージ済み or ベース + アダプター）
    "model_path": "./models/qwen3-8b-japanese",
    "base_model_path": "./models/qwen3-8b",
    "sft_adapter_path": "./adapters/japanese-v1",

    # DPOデータ
    "dpo_data_dir": "./data/dpo",

    # DPOアダプター出力先
    "dpo_adapter_path": "./adapters/japanese-dpo",
    "final_model_path": "./models/qwen3-8b-japanese-dpo",

    # DPOハイパーパラメータ
    "beta": 0.1,           # DPO温度パラメータ（小さいほど保守的）
    "iters": 500,          # SFTより少なくてよい
    "batch_size": 2,       # DPOはSFTより多くのメモリを使う → 2に設定
    "learning_rate": 5e-5, # SFTより低めに設定
    "max_seq_length": 1024,
    "grad_checkpoint": True,
    "seed": 42,
}


def load_config(config_path: str | None) -> dict:
    config = DEFAULT_CONFIG.copy()
    if config_path and Path(config_path).exists():
        with open(config_path, encoding="utf-8") as f:
            override = yaml.safe_load(f)
        # DPO固有のキーのみ上書き
        for key in ["beta", "dpo_data_dir", "dpo_adapter_path", "final_model_path",
                    "base_model_path", "sft_adapter_path", "model_path", "seed"]:
            if key in override:
                config[key] = override[key]
    return config


def check_prerequisites(config: dict):
    """前提条件チェック"""
    errors = []

    dpo_train = Path(config["dpo_data_dir"]) / "train.jsonl"
    if not dpo_train.exists():
        errors.append(f"DPOデータが見つかりません: {dpo_train}\n  → python 1_prepare_dataset.py を先に実行してください")

    # マージ済みモデルまたはアダプターのどちらかが存在すること
    merged = Path(config["model_path"])
    base = Path(config["base_model_path"])
    adapter = Path(config["sft_adapter_path"])
    if not merged.exists() and not (base.exists() and adapter.exists()):
        errors.append(
            f"SFT済みモデルが見つかりません\n"
            f"  → {merged} または ({base} + {adapter}) が必要です\n"
            f"  → python 2_finetune.py を先に実行してください"
        )

    if errors:
        print("=== 前提条件エラー ===")
        for e in errors:
            print(f"✗ {e}")
        sys.exit(1)

    print("前提条件: OK")


def resolve_model_path(config: dict) -> str:
    """マージ済みモデル優先、なければベース + アダプターを使用"""
    merged = Path(config["model_path"])
    if merged.exists():
        print(f"使用モデル: {merged}（SFTマージ済み）")
        return str(merged)
    else:
        base = config["base_model_path"]
        print(f"使用モデル: {base}（ベース + SFTアダプター）")
        return base


def build_dpo_command(config: dict, model_path: str, test_run: bool = False) -> list[str]:
    """mlx_lm.dpo コマンドの構築"""
    iters = 30 if test_run else config["iters"]

    cmd = [
        sys.executable, "-m", "mlx_lm.dpo",
        "--model", model_path,
        "--train",
        "--data", config["dpo_data_dir"],
        "--adapter-path", config["dpo_adapter_path"],
        "--beta", str(config["beta"]),
        "--iters", str(iters),
        "--batch-size", str(config["batch_size"]),
        "--learning-rate", str(config["learning_rate"]),
        "--max-seq-length", str(config["max_seq_length"]),
        "--seed", str(config["seed"]),
    ]

    # SFTアダプターをベースにDPOを重ねる場合
    base = Path(config["base_model_path"])
    merged = Path(config["model_path"])
    if not merged.exists() and base.exists():
        cmd += ["--adapter-path-start", config["sft_adapter_path"]]

    if config.get("grad_checkpoint"):
        cmd.append("--grad-checkpoint")

    return cmd


def run_dpo(config: dict, test_run: bool = False):
    """DPOトレーニング実行"""
    Path(config["dpo_adapter_path"]).mkdir(parents=True, exist_ok=True)
    model_path = resolve_model_path(config)

    cmd = build_dpo_command(config, model_path, test_run)

    if test_run:
        print("\n=== DPO テスト実行（30ステップ） ===")
    else:
        print(f"\n=== DPO トレーニング開始 ({config['iters']}ステップ） ===")

    print("コマンド:", " ".join(cmd))
    print(f"beta={config['beta']} | batch={config['batch_size']} | lr={config['learning_rate']}")
    print("\n学習中... (Ctrl+C で中断可能)")
    print("=" * 60)

    start = time.time()
    result = subprocess.run(cmd, check=False)
    elapsed = time.time() - start

    if result.returncode == 0:
        print(f"\n完了！ 経過時間: {elapsed / 60:.1f}分")
        print(f"DPOアダプター保存先: {config['dpo_adapter_path']}")
    else:
        print(f"\nエラーが発生しました (終了コード: {result.returncode})")
        sys.exit(result.returncode)


def fuse_dpo_model(config: dict):
    """DPOアダプターをモデルにマージ"""
    print("\n=== DPOモデルのマージ ===")
    final_path = Path(config["final_model_path"])
    final_path.mkdir(parents=True, exist_ok=True)

    model_path = resolve_model_path(config)

    cmd = [
        sys.executable, "-m", "mlx_lm.fuse",
        "--model", model_path,
        "--adapter-path", config["dpo_adapter_path"],
        "--save-path", str(final_path),
    ]

    result = subprocess.run(cmd, check=False)
    if result.returncode == 0:
        print(f"マージ完了: {final_path}")
        print("\n最終モデルパス:", final_path)
        print("評価コマンド: python 3_evaluate.py --model-path", str(final_path), "--compare")
    else:
        print("マージに失敗しました。アダプターのみ使用してください。")


def show_memory_estimate():
    """メモリ使用量の目安を表示"""
    print("""
メモリ使用量の目安（M4 Max 36GB）:
  DPOトレーニング中: ~14GB
  他アプリ用:        ~10GB
  空き:              ~12GB  ← 快適に動作可能
""")


# ---------------------------------------------------------------------------
# エントリーポイント
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="DPO日本語品質向上トレーニング")
    parser.add_argument("--config", default="config.yaml")
    parser.add_argument("--test-run", action="store_true", help="30ステップのテスト実行")
    parser.add_argument("--fuse-only", action="store_true", help="マージのみ実行")
    parser.add_argument("--skip-fuse", action="store_true", help="マージをスキップ")
    args = parser.parse_args()

    print("=" * 60)
    print("DPO 日本語品質向上トレーニング")
    print("SFT後の自然さ・敬語・文体を最適化")
    print("=" * 60)

    config = load_config(args.config)
    show_memory_estimate()
    check_prerequisites(config)

    if args.fuse_only:
        fuse_dpo_model(config)
        return

    run_dpo(config, test_run=args.test_run)

    if not args.test_run and not args.skip_fuse:
        fuse_dpo_model(config)
        print("\n次のステップ: python 3_evaluate.py --model-path", config["final_model_path"], "--compare")
    elif args.test_run:
        print("\nテスト成功。本番実行:")
        print("  python 2b_dpo.py")


if __name__ == "__main__":
    main()
