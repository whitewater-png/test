#!/usr/bin/env python3
"""
Qwen3-8B 日本語LoRAファインチューニングスクリプト
対象環境: MacBook Pro M4 Max 36GB (Apple Silicon / MLX)

使い方:
    pip install mlx-lm
    python 2_finetune.py [--config config.yaml] [--test-run]

事前準備:
    huggingface-cli login
    python 1_prepare_dataset.py
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
    # モデル設定
    "model": "Qwen/Qwen3-8B",          # HuggingFace モデルID
    "model_path": "./models/qwen3-8b", # ローカルキャッシュパス

    # データ設定
    "data_dir": "./data",
    "train_file": "train.jsonl",
    "valid_file": "valid.jsonl",

    # LoRA設定（M4 Max 36GB最適化）
    "lora_layers": 16,          # LoRAを適用するレイヤー数（多いほど精度UP・VRAM増）
    "lora_rank": 16,            # LoRAランク（8〜32が一般的）
    "lora_scale": 20.0,         # LoRAスケール（通常 rank * 1.25）

    # 学習設定
    "batch_size": 4,            # M4 Max向け（OOMなら2に下げる）
    "iters": 1000,              # 総ステップ数
    "learning_rate": 1e-4,
    "lr_schedule": "cosine_decay",
    "warmup_steps": 100,
    "val_batches": 25,          # 検証に使うバッチ数
    "steps_per_eval": 200,      # 何ステップごとに検証するか
    "save_every": 200,          # 何ステップごとにチェックポイント保存

    # 出力設定
    "adapter_path": "./adapters/japanese-v1",
    "fused_model_path": "./models/qwen3-8b-japanese",

    # その他
    "max_seq_length": 2048,     # コンテキスト長（長いほどVRAM増）
    "grad_checkpoint": True,    # gradient checkpointing（VRAM削減）
    "seed": 42,
}


def load_config(config_path: str | None) -> dict:
    config = DEFAULT_CONFIG.copy()
    if config_path and Path(config_path).exists():
        with open(config_path, encoding="utf-8") as f:
            override = yaml.safe_load(f)
        config.update(override)
        print(f"設定ファイル読み込み: {config_path}")
    return config


def check_dependencies():
    """必要なパッケージの確認"""
    try:
        import mlx
        import mlx_lm
        print(f"MLX バージョン: {mlx.__version__}")
    except ImportError:
        print("エラー: mlx-lm がインストールされていません")
        print("実行: pip install mlx-lm")
        sys.exit(1)

    try:
        import mlx.core as mx
        mem_gb = mx.metal.device_info()["memory_size"] / (1024**3)
        print(f"使用可能メモリ: {mem_gb:.1f} GB")
        if mem_gb < 16:
            print("警告: メモリが少ない可能性があります（推奨: 16GB以上）")
    except Exception:
        pass


def download_model(config: dict):
    """モデルのダウンロード（未取得の場合）"""
    model_path = Path(config["model_path"])
    if model_path.exists() and any(model_path.iterdir()):
        print(f"モデルは既に存在します: {model_path}")
        return

    print(f"モデルをダウンロード中: {config['model']}")
    model_path.mkdir(parents=True, exist_ok=True)
    cmd = [
        "huggingface-cli", "download",
        config["model"],
        "--local-dir", str(model_path),
        "--exclude", "*.pt",   # PyTorch形式は不要
    ]
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        print("HuggingFaceからのダウンロードに失敗しました")
        print(f"手動でダウンロードしてください: {config['model_path']} に配置")
        sys.exit(1)


def build_train_command(config: dict, test_run: bool = False) -> list[str]:
    """mlx_lm.lora コマンドの構築"""
    cmd = [
        sys.executable, "-m", "mlx_lm.lora",
        "--model", config["model_path"],
        "--train",
        "--data", config["data_dir"],
        "--batch-size", str(config["batch_size"]),
        "--lora-layers", str(config["lora_layers"]),
        "--iters", str(50 if test_run else config["iters"]),
        "--learning-rate", str(config["learning_rate"]),
        "--steps-per-eval", str(config["steps_per_eval"]),
        "--val-batches", str(config["val_batches"]),
        "--save-every", str(config["save_every"]),
        "--adapter-path", config["adapter_path"],
        "--max-seq-length", str(config["max_seq_length"]),
        "--seed", str(config["seed"]),
    ]

    if config.get("grad_checkpoint"):
        cmd.append("--grad-checkpoint")

    return cmd


def run_finetuning(config: dict, test_run: bool = False):
    """ファインチューニングの実行"""
    Path(config["adapter_path"]).mkdir(parents=True, exist_ok=True)

    cmd = build_train_command(config, test_run)

    if test_run:
        print("\n=== テスト実行（50ステップ）===")
    else:
        print(f"\n=== ファインチューニング開始 ({config['iters']}ステップ) ===")

    print("コマンド:", " ".join(cmd))
    print("\n学習中... (Ctrl+C で中断可能)")
    print("=" * 60)

    start = time.time()
    result = subprocess.run(cmd, check=False)
    elapsed = time.time() - start

    if result.returncode == 0:
        print(f"\n完了！ 経過時間: {elapsed / 60:.1f}分")
        print(f"アダプター保存先: {config['adapter_path']}")
    else:
        print(f"\nエラーが発生しました (終了コード: {result.returncode})")
        sys.exit(result.returncode)


def fuse_model(config: dict):
    """LoRAアダプターをベースモデルにマージ"""
    print("\n=== モデルのマージ ===")
    fused_path = Path(config["fused_model_path"])
    fused_path.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable, "-m", "mlx_lm.fuse",
        "--model", config["model_path"],
        "--adapter-path", config["adapter_path"],
        "--save-path", str(fused_path),
        "--de-quantize",  # マージ後に量子化を再適用
    ]

    print("コマンド:", " ".join(cmd))
    result = subprocess.run(cmd, check=False)

    if result.returncode == 0:
        print(f"マージ完了: {fused_path}")
    else:
        print("マージに失敗しました。アダプターのみ使用してください。")


def quick_inference_test(config: dict):
    """簡易推論テスト"""
    print("\n=== 簡易推論テスト ===")
    test_prompt = "Pythonでリストの重複を除去する最も効率的な方法を教えてください。"
    print(f"プロンプト: {test_prompt}\n")

    cmd = [
        sys.executable, "-m", "mlx_lm.generate",
        "--model", config["model_path"],
        "--adapter-path", config["adapter_path"],
        "--max-tokens", "300",
        "--prompt", (
            f"<|im_start|>system\n{DEFAULT_CONFIG.get('system_prompt', 'あなたは優秀な日本語AIアシスタントです。')}<|im_end|>\n"
            f"<|im_start|>user\n{test_prompt}<|im_end|>\n"
            f"<|im_start|>assistant\n"
        ),
    ]

    subprocess.run(cmd, check=False)


# ---------------------------------------------------------------------------
# エントリーポイント
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Qwen3-8B 日本語LoRAファインチューニング")
    parser.add_argument("--config", default="config.yaml", help="設定ファイルパス")
    parser.add_argument("--test-run", action="store_true", help="50ステップのテスト実行")
    parser.add_argument("--skip-download", action="store_true", help="モデルダウンロードをスキップ")
    parser.add_argument("--fuse-only", action="store_true", help="マージのみ実行")
    parser.add_argument("--inference-test", action="store_true", help="推論テストのみ実行")
    args = parser.parse_args()

    print("=" * 60)
    print("Qwen3-8B 日本語LoRAファインチューニング")
    print("対象環境: MacBook Pro M4 Max 36GB")
    print("=" * 60)

    check_dependencies()
    config = load_config(args.config)

    # データファイルの確認
    data_dir = Path(config["data_dir"])
    if not (data_dir / config["train_file"]).exists():
        print(f"\nエラー: 学習データが見つかりません: {data_dir / config['train_file']}")
        print("先に実行してください: python 1_prepare_dataset.py")
        sys.exit(1)

    if args.inference_test:
        quick_inference_test(config)
        return

    if args.fuse_only:
        fuse_model(config)
        return

    # 通常フロー
    if not args.skip_download:
        download_model(config)

    run_finetuning(config, test_run=args.test_run)

    if not args.test_run:
        fuse_model(config)
        quick_inference_test(config)
        print("\n次のステップ: python 3_evaluate.py を実行してください")
    else:
        print("\nテスト実行完了。問題なければ本番実行:")
        print("  python 2_finetune.py")


if __name__ == "__main__":
    main()
