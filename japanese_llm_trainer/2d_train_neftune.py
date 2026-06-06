#!/usr/bin/env python3
"""
NEFTune対応 in-process LoRA/ORPO学習ランチャー

mlx_lm.lora のCLIはサブプロセスで動くため埋め込み層にNEFTuneノイズを
注入できない。本スクリプトは mlx_lm.tuner の Python API を直接呼び、
学習前にNEFTuneを適用してから学習する。

使い方:
    python 2d_train_neftune.py --config config_14b.yaml [--test-run]

注意:
    mlx_lm の Python API はバージョンで変わることがある。
    本スクリプトはimportを防御的に行い、失敗時は明確に案内する。
    NEFTuneなしで良ければ通常の 2c_orpo.py を使うこと。
"""

import argparse
import sys
import types
from pathlib import Path

import yaml

from neftune import apply_neftune, remove_neftune, set_neftune_training


def load_config(path: str) -> dict:
    cfg = {
        "model_path": "./models/qwen3-14b",
        "data_dir": "./data",
        "dpo_data_dir": "./data/dpo",
        "adapter_path": "./adapters/14b-japanese-neftune",
        "final_model_path": "./models/qwen3-14b-japanese-neftune",
        "lora_layers": 8,
        "lora_rank": 16,
        "lora_scale": 20.0,
        "batch_size": 2,
        "iters": 1000,
        "learning_rate": 1e-4,
        "max_seq_length": 1024,
        "grad_checkpoint": True,
        "seed": 42,
        "loss": "orpo",          # "orpo" か "default"(SFT)
        "neftune_alpha": 5.0,    # 推奨値 5.0（論文では5〜15）
    }
    if Path(path).exists():
        with open(path, encoding="utf-8") as f:
            raw = yaml.safe_load(f)
        for k in list(cfg.keys()):
            if k in raw:
                cfg[k] = raw[k]
        # neftuneセクションがあれば優先
        if "neftune" in raw:
            cfg.update(raw["neftune"])
    return cfg


def import_mlx_tuner():
    """
    mlx_lm の学習APIを防御的にインポートする。
    返り値: (load, linear_to_lora_layers, train, TrainingArgs) または None
    """
    try:
        from mlx_lm import load
        from mlx_lm.tuner.utils import linear_to_lora_layers
        from mlx_lm.tuner.trainer import train, TrainingArgs
        return load, linear_to_lora_layers, train, TrainingArgs
    except Exception as e:
        print("=" * 60)
        print("mlx_lm の Python 学習API をロードできませんでした:")
        print(f"  {e}")
        print("\n対処:")
        print("  1. pip install --upgrade mlx-lm")
        print("  2. それでも失敗する場合は NEFTune を諦め、")
        print("     通常の学習を使ってください:")
        print("       python 2c_orpo.py --config config_14b.yaml")
        print("=" * 60)
        return None


def apply_lora_selective(model, linear_to_lora_layers, layer_indices, lora_config):
    """
    Spectrumで選ばれた特定レイヤーにのみLoRAを適用する。
    linear_to_lora_layers は通常「末尾N層」に適用するため、
    非連続なレイヤー選択にはこのカスタム適用を使う。
    内部APIが合わない場合は False を返す（呼び出し側で従来法にフォールバック）。
    """
    try:
        from mlx_lm.tuner.lora import LoRALinear
    except Exception:
        return False

    try:
        layers = model.model.layers
    except AttributeError:
        return False

    applied = 0
    for idx in layer_indices:
        if idx < 0 or idx >= len(layers):
            continue
        block = layers[idx]
        # ブロック内の nn.Linear を LoRALinear に置換
        for attr_owner, attr_name, module in _iter_linears(block):
            try:
                lora_lin = LoRALinear.from_base(
                    module,
                    r=lora_config["rank"],
                    scale=lora_config["scale"],
                    dropout=lora_config.get("dropout", 0.0),
                )
                setattr(attr_owner, attr_name, lora_lin)
                applied += 1
            except Exception:
                # 旧API: from_linear
                try:
                    lora_lin = LoRALinear.from_linear(module, r=lora_config["rank"])
                    setattr(attr_owner, attr_name, lora_lin)
                    applied += 1
                except Exception:
                    continue

    if applied == 0:
        return False
    print(f"Spectrum: {len(layer_indices)}レイヤーに{applied}個のLoRAを適用")
    return True


def _iter_linears(module, prefix=""):
    """モジュール内の nn.Linear を再帰的に列挙する (owner, attr_name, module)"""
    import mlx.nn as nn
    for name, child in vars(module).items():
        if isinstance(child, nn.Linear):
            yield module, name, child
        elif isinstance(child, nn.Module):
            yield from _iter_linears(child, prefix=f"{prefix}{name}.")


def main():
    parser = argparse.ArgumentParser(description="NEFTune対応 in-process学習")
    parser.add_argument("--config", default="config_14b.yaml")
    parser.add_argument("--test-run", action="store_true")
    parser.add_argument("--spectrum-layers", default=None,
                        help="spectrum_analyze.py が出力したJSON。指定レイヤーのみ学習")
    args = parser.parse_args()

    cfg = load_config(args.config)
    alpha = float(cfg.get("neftune_alpha", 5.0))

    print("=" * 60)
    print("NEFTune対応 LoRA/ORPO 学習")
    print(f"  モデル: {cfg['model_path']}")
    print(f"  損失:   {cfg.get('loss', 'orpo')}")
    print(f"  NEFTune alpha: {alpha}")
    print("=" * 60)

    imported = import_mlx_tuner()
    if imported is None:
        sys.exit(1)
    load, linear_to_lora_layers, train, TrainingArgs = imported

    import mlx.optimizers as optim

    # --- モデルロード ---
    print("\nモデルをロード中...")
    model, tokenizer = load(cfg["model_path"])

    # --- LoRA適用 ---
    model.freeze()
    lora_config = {
        "rank": cfg["lora_rank"],
        "scale": cfg["lora_scale"],
        "dropout": 0.0,
    }

    # Spectrumレイヤー選択があれば優先
    spectrum_applied = False
    if args.spectrum_layers and Path(args.spectrum_layers).exists():
        with open(args.spectrum_layers, encoding="utf-8") as f:
            spec = yaml.safe_load(f)  # JSONもsafe_loadで読める
        selected = spec.get("selected_layers", [])
        print(f"Spectrum適用: {len(selected)}レイヤーを学習対象に選択 {selected}")
        spectrum_applied = apply_lora_selective(model, linear_to_lora_layers, selected, lora_config)
        if not spectrum_applied:
            print("Spectrum選択適用に失敗 → 従来の末尾Nレイヤー方式にフォールバック")

    if not spectrum_applied:
        print(f"LoRAを適用中（末尾{cfg['lora_layers']}レイヤー, rank={cfg['lora_rank']}）...")
        try:
            linear_to_lora_layers(model, cfg["lora_layers"], lora_config)
        except TypeError:
            linear_to_lora_layers(model, cfg["lora_layers"], lora_config, use_dora=False)

    # --- NEFTune適用 ---
    ok = apply_neftune(model, alpha=alpha)
    if not ok:
        print("NEFTuneを適用できませんでしたが、学習は続行します（通常LoRA相当）")
    set_neftune_training(True)

    # --- データ準備 ---
    # 損失がorpoならDPO形式、defaultならSFT(text)形式
    data_path = cfg["dpo_data_dir"] if cfg.get("loss") == "orpo" else cfg["data_dir"]
    print(f"\nデータ: {data_path}")

    Path(cfg["adapter_path"]).mkdir(parents=True, exist_ok=True)

    iters = 30 if args.test_run else cfg["iters"]
    training_args = TrainingArgs(
        batch_size=cfg["batch_size"],
        iters=iters,
        max_seq_length=cfg["max_seq_length"],
        adapter_file=str(Path(cfg["adapter_path"]) / "adapters.safetensors"),
        grad_checkpoint=cfg.get("grad_checkpoint", True),
    )

    optimizer = optim.AdamW(learning_rate=cfg["learning_rate"])

    print(f"\n学習開始（{iters}ステップ）...")
    print("注意: NEFTuneは学習時のみノイズを加えます。保存される重みにノイズは含まれません。")
    print("=" * 60)

    # mlx_lmのtrain APIにデータセットを渡す。
    # データセットのロードはバージョン差があるため、簡易ローダーを使う。
    from mlx_lm.tuner.datasets import load_dataset as load_tuner_dataset

    try:
        train_set, valid_set, _ = load_tuner_dataset(
            types.SimpleNamespace(
                data=data_path,
                train=True,
                test=False,
                prompt_feature=None,
                completion_feature=None,
            ),
            tokenizer,
        )
    except Exception as e:
        print(f"データセットローダーがAPI変更で失敗しました: {e}")
        print("通常の 2c_orpo.py を使うことを推奨します。")
        sys.exit(1)

    train(
        model=model,
        tokenizer=tokenizer,
        optimizer=optimizer,
        train_dataset=train_set,
        val_dataset=valid_set,
        args=training_args,
    )

    # --- NEFTune解除して保存 ---
    set_neftune_training(False)
    remove_neftune(model)

    print(f"\n学習完了。アダプター: {cfg['adapter_path']}")
    print("\nマージ:")
    print(f"  python -m mlx_lm.fuse --model {cfg['model_path']} "
          f"--adapter-path {cfg['adapter_path']} --save-path {cfg['final_model_path']}")
    print("\n評価:")
    print(f"  python 3_evaluate.py --model-path {cfg['final_model_path']} --compare --use-llm-judge")


if __name__ == "__main__":
    main()
