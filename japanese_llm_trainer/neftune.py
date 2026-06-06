#!/usr/bin/env python3
"""
NEFTune (Noisy Embeddings Fine-Tuning) モジュール
論文: NEFTune: Noisy Embeddings Improve Instruction Finetuning (2023)

仕組み:
  学習時、埋め込み層の出力に一様ノイズを加える。
    noise = uniform(-1, 1) * (alpha / sqrt(L * d))
    （L = シーケンス長, d = 埋め込み次元）
  これだけで指示追従性が +5〜10% 向上することが報告されている。
  推論時はノイズを加えない（学習時のみ）。

使い方:
  この関数は 2_finetune.py / 2c_orpo.py から呼ばれ、
  mlx_lm のモデルの埋め込み層をラップしてノイズ注入を有効化する。

  from neftune import apply_neftune
  apply_neftune(model, alpha=5.0)   # 学習前に1回呼ぶ
"""

import math
import mlx.core as mx
import mlx.nn as nn


# グローバルな学習フラグ（推論時はノイズを無効化）
_NEFTUNE_TRAINING = {"active": True}


def set_neftune_training(active: bool):
    """学習中はTrue、推論/検証中はFalseにする"""
    _NEFTUNE_TRAINING["active"] = active


class NEFTuneEmbedding(nn.Module):
    """
    既存の埋め込みモジュールをラップし、出力にNEFTuneノイズを加える。
    元のモジュールの重みはそのまま使う（学習対象は元のまま）。
    """

    def __init__(self, base_embedding: nn.Module, alpha: float = 5.0):
        super().__init__()
        self.base = base_embedding
        self.alpha = alpha

    def __call__(self, x):
        out = self.base(x)
        if not _NEFTUNE_TRAINING["active"] or self.alpha <= 0:
            return out

        # out shape: (..., seq_len, dim)
        seq_len = out.shape[-2]
        dim = out.shape[-1]
        # スケール係数 alpha / sqrt(L * d)
        scale = self.alpha / math.sqrt(seq_len * dim)
        # 一様ノイズ uniform(-1, 1)
        noise = mx.random.uniform(low=-1.0, high=1.0, shape=out.shape, dtype=out.dtype)
        return out + noise * scale

    def as_linear(self, x):
        """一部のモデルは embed_tokens.as_linear() で出力射影を共有する"""
        if hasattr(self.base, "as_linear"):
            return self.base.as_linear(x)
        raise AttributeError("base embedding has no as_linear")

    @property
    def weight(self):
        # 重み共有モデル（tie_word_embeddings）対応
        return self.base.weight


def _find_embedding(model):
    """
    モデルから埋め込み層を探す。
    Qwen系: model.model.embed_tokens
    一般:    model.embed_tokens
    """
    candidates = [
        ("model.embed_tokens", lambda m: m.model.embed_tokens, lambda m, e: setattr(m.model, "embed_tokens", e)),
        ("embed_tokens", lambda m: m.embed_tokens, lambda m, e: setattr(m, "embed_tokens", e)),
    ]
    for name, getter, setter in candidates:
        try:
            emb = getter(model)
            if emb is not None:
                return name, emb, setter
        except AttributeError:
            continue
    return None, None, None


def apply_neftune(model, alpha: float = 5.0) -> bool:
    """
    モデルの埋め込み層をNEFTuneEmbeddingでラップする。
    成功すればTrue、埋め込み層が見つからなければFalse。
    """
    if alpha <= 0:
        print("NEFTune: alpha<=0 のため無効")
        return False

    name, emb, setter = _find_embedding(model)
    if emb is None:
        print("NEFTune: 埋め込み層が見つかりませんでした（スキップ）")
        return False

    if isinstance(emb, NEFTuneEmbedding):
        print("NEFTune: 既に適用済み")
        return True

    wrapped = NEFTuneEmbedding(emb, alpha=alpha)
    setter(model, wrapped)
    print(f"NEFTune: 適用完了（{name}, alpha={alpha}）")
    return True


def remove_neftune(model) -> bool:
    """NEFTuneラッパーを外して元の埋め込みに戻す（マージ/保存前に推奨）"""
    name, emb, setter = _find_embedding(model)
    if isinstance(emb, NEFTuneEmbedding):
        setter(model, emb.base)
        print(f"NEFTune: 解除完了（{name}）")
        return True
    return False
