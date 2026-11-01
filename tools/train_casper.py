#!/usr/bin/env python3
"""Train the Casper transformer and export the native NIYAH binary layout."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    from torch.utils.data import DataLoader, Dataset
except ImportError as exc:
    raise SystemExit("PyTorch is required; install a CPU or CUDA build appropriate for your device.") from exc

NIYAH_MAGIC = 0x4E595148
NIYAH_VER = 0x0005
TOK_BOS, TOK_EOS, TOK_PAD, TOK_UNK = 0, 1, 2, 3
WORDS = [
    "the","a","an","and","or","is","in","of","to","for","with","on","at","by","from",
    "that","this","it","are","was","be","as","not","but","have","has","had","we","i","you",
    "they","he","she","can","will","would","do","does","did","if","when","then","so","all",
    "no","up","out","than",
    "more","less","very","also","only","model","data","train","training","layer","layers","weight",
    "weights","token","tokens","embed","embedding","head","heads","attention","output","input","loss",
    "gradient","optimizer","matrix","vector","kernel","cpu","gpu","memory","float","int","size","context",
    "vocab","local","code","file","build","run","test","hash","proof","rule","query","fact",
    "function","class","struct","type","return","void","static","const","malloc","calloc","free","pointer",
    "buffer","stack","heap","pool","forward","backward","sample","generate","decode","encode","norm","softmax",
    "relu","silu","gelu","linear","bias","scale","sum","dot","compute","algorithm","system","engine","core",
    "base","key","value","arabic","quran","bismillah","inference","symbolic","logic","constraint","solver",
    "rational","arithmetic","sha","cryptographic","niyah","casper","khwarizmi","adam","rope","swiglu","rmsnorm",
    "gqa","zero","one","two","three","four","five","six","seven","eight","nine","ten","hundred","thousand",
    "million","billion",
]
PUNCTUATION = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
ARABIC_RANGES = [(0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF), (0xFB50, 0xFDFF), (0xFE70, 0xFEFF)]
ASCII_WHITESPACE = " \t\n\r\v\f"


class CasperTokenizer:
    """Python mirror of tokenizer.c vocabulary v2."""
    def __init__(self) -> None:
        self.tokens: list[str] = ["<BOS>", "<EOS>", "<PAD>", "<UNK>"]
        self.tokens += list("0123456789") + list(PUNCTUATION)
        self.tokens += list("abcdefghijklmnopqrstuvwxyz") + list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + WORDS
        self.word_end = len(self.tokens)
        self.lookup_exact = {tok: i for i, tok in enumerate(self.tokens[:self.word_end])}
        self.char_base = len(self.tokens)
        self.codepoint_to_id: dict[int, int] = {}
        for lo, hi in ARABIC_RANGES:
            for cp in range(lo, hi + 1):
                self.codepoint_to_id[cp] = len(self.tokens)
                self.tokens.append(chr(cp))
        self.byte_base = len(self.tokens)
        self.tokens += [f"<{b:02X}>" for b in range(256)]
        if self.char_base != 268 or self.byte_base != 1500 or len(self.tokens) != 1756:
            raise RuntimeError("tokenizer vocabulary contract drift")

    @property
    def vocab_size(self) -> int:
        return len(self.tokens)

    def _lookup_ascii(self, text: str) -> int:
        return self.lookup_exact.get(text, TOK_UNK)

    def encode_bytes(self, data: bytes) -> list[int]:
        text = data.decode("utf-8", errors="strict")
        out: list[int] = []
        i = 0
        while i < len(text):
            ch = text[i]
            cp = ord(ch)
            if cp in self.codepoint_to_id:
                out.append(self.codepoint_to_id[cp])
                i += 1
                continue
            raw = ch.encode("utf-8")
            if cp >= 128:
                out.extend(self.byte_base + b for b in raw)
                i += 1
                continue
            if ch in ASCII_WHITESPACE:
                out.append(self.byte_base + raw[0])
                i += 1
                continue
            if ch in PUNCTUATION:
                out.append(self._lookup_ascii(ch))
                i += 1
                continue
            j = i
            while j < len(text):
                c = text[j]
                if ord(c) >= 128 or c in ASCII_WHITESPACE or c in PUNCTUATION:
                    break
                j += 1
            word = text[i:j]
            if not word:
                out.append(self.byte_base + raw[0])
                i += 1
                continue
            tid = self._lookup_ascii(word)
            if tid != TOK_UNK:
                out.append(tid)
            else:
                out.extend(self.byte_base + b for b in word.encode("ascii"))
            i = j
        return out

    def encode(self, text: str, *, bos: bool = True, eos: bool = True) -> list[int]:
        ids = self.encode_bytes(text.encode("utf-8"))
        if bos:
            ids.insert(0, TOK_BOS)
        if eos:
            ids.append(TOK_EOS)
        return ids

    def decode(self, ids: Iterable[int]) -> str:
        raw = bytearray()
        chunks: list[bytes] = []
        def flush_raw() -> None:
            if raw:
                chunks.append(bytes(raw))
                raw.clear()
        for tid in ids:
            if tid in (TOK_BOS, TOK_EOS, TOK_PAD):
                continue
            if self.byte_base <= tid < self.byte_base + 256:
                raw.append(tid - self.byte_base)
                continue
            flush_raw()
            chunks.append(self.tokens[tid].encode("utf-8") if 0 <= tid < self.vocab_size else b"<UNK>")
        flush_raw()
        return b"".join(chunks).decode("utf-8", errors="replace")


def verify_tokenizer_contract(tok: CasperTokenizer) -> None:
    for probe in ("malloc allocates heap memory", "unknown teacher vocabulary survives exactly",
                  "SHA-256 Casper TCP UDP", "بسم الله", "casper نية engine",
                  "emoji: 🧠 CJK: 汉字", "line one\nline two\tindent"):
        if tok.decode(tok.encode(probe)) != probe:
            raise RuntimeError(f"tokenizer round-trip failure: {probe!r}")


@dataclass
class ModelConfig:
    vocab_size: int
    ctx_len: int = 256
    embed_dim: int = 128
    n_layers: int = 4
    n_heads: int = 8
    n_kv_heads: int = 4
    ffn_mult: int = 4
    rope_theta: float = 10000.0
    rms_eps: float = 1e-5

    def validate(self) -> None:
        if self.embed_dim <= 0 or self.n_heads <= 0 or self.n_layers <= 0:
            raise ValueError("invalid zero model dimension")
        if self.embed_dim % self.n_heads:
            raise ValueError("embed_dim must be divisible by n_heads")
        if not (0 < self.n_kv_heads <= self.n_heads):
            raise ValueError("n_kv_heads must be in 1..n_heads")
        if self.ffn_mult <= 0:
            raise ValueError("ffn_mult must be positive")
        if not math.isfinite(self.rope_theta) or self.rope_theta <= 0:
            raise ValueError("rope_theta must be finite and positive")
        if not math.isfinite(self.rms_eps) or self.rms_eps <= 0:
            raise ValueError("rms_eps must be finite and positive")
        if self.ctx_len <= 0 or self.ctx_len > 8192:
            raise ValueError("ctx_len must be in 1..8192")
        if self.vocab_size <= 0 or self.vocab_size > 131072:
            raise ValueError("vocab_size out of range")
        if (self.embed_dim // self.n_heads) % 2:
            raise ValueError("head_dim must be even for RoPE")


class RMSNorm(nn.Module):
    def __init__(self, dim: int, eps: float) -> None:
        super().__init__()
        self.weight = nn.Parameter(torch.ones(dim))
        self.eps = eps

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        scale = torch.rsqrt(x.float().pow(2).mean(dim=-1, keepdim=True) + self.eps)
        return x * scale.to(dtype=x.dtype) * self.weight


def apply_rope(x: torch.Tensor, theta: float) -> torch.Tensor:
    hd = x.shape[-1]
    if hd % 2:
        raise ValueError("head_dim must be even for RoPE")
    pair_index = torch.arange(0, hd, 2, device=x.device, dtype=torch.float32)
    inv_freq = torch.pow(torch.tensor(theta, device=x.device), -pair_index / float(hd))
    pos = torch.arange(x.shape[-2], device=x.device, dtype=torch.float32)
    angle = pos[:, None] * inv_freq[None, :]
    cos = angle.cos()[None, None, :, :].to(dtype=x.dtype)
    sin = angle.sin()[None, None, :, :].to(dtype=x.dtype)
    even, odd = x[..., 0::2], x[..., 1::2]
    return torch.stack((even * cos - odd * sin, even * sin + odd * cos), dim=-1).flatten(-2)


class Block(nn.Module):
    def __init__(self, cfg: ModelConfig) -> None:
        super().__init__()
        d = cfg.embed_dim
        kd = cfg.n_kv_heads * (d // cfg.n_heads)
        f = d * cfg.ffn_mult
        self.cfg = cfg
        self.rms_att = RMSNorm(d, cfg.rms_eps)
        self.wq = nn.Linear(d, d, bias=False)
        self.wk = nn.Linear(d, kd, bias=False)
        self.wv = nn.Linear(d, kd, bias=False)
        self.wo = nn.Linear(d, d, bias=False)
        self.rms_ffn = RMSNorm(d, cfg.rms_eps)
        self.w_gate = nn.Linear(d, f, bias=False)
        self.w_up = nn.Linear(d, f, bias=False)
        self.w_down = nn.Linear(f, d, bias=False)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        cfg = self.cfg
        b, t, d = x.shape
        hd = d // cfg.n_heads
        y = self.rms_att(x)
        q = self.wq(y).view(b, t, cfg.n_heads, hd).transpose(1, 2)
        k = self.wk(y).view(b, t, cfg.n_kv_heads, hd).transpose(1, 2)
        v = self.wv(y).view(b, t, cfg.n_kv_heads, hd).transpose(1, 2)
        q = apply_rope(q, cfg.rope_theta)
        k = apply_rope(k, cfg.rope_theta)
        kv_map = (torch.arange(cfg.n_heads, device=x.device) * cfg.n_kv_heads) // cfg.n_heads
        kg, vg = k[:, kv_map, :, :], v[:, kv_map, :, :]
        scores = torch.matmul(q, kg.transpose(-2, -1)) / math.sqrt(float(hd))
        causal = torch.triu(torch.ones(t, t, device=x.device, dtype=torch.bool), diagonal=1)
        scores = scores.clamp(-80.0, 80.0)
        scores = scores.masked_fill(causal[None, None, :, :], float("-inf"))
        attn = torch.softmax(scores.float(), dim=-1).to(dtype=q.dtype)
        a = torch.matmul(attn, vg).transpose(1, 2).contiguous().view(b, t, d)
        x = x + self.wo(a)
        y = self.rms_ffn(x)
        return x + self.w_down(F.silu(self.w_gate(y)) * self.w_up(y))


class CasperModel(nn.Module):
    def __init__(self, cfg: ModelConfig) -> None:
        super().__init__()
        cfg.validate()
        self.cfg = cfg
        self.token_embed = nn.Embedding(cfg.vocab_size, cfg.embed_dim)
        self.layers = nn.ModuleList([Block(cfg) for _ in range(cfg.n_layers)])
        self.rms_final = RMSNorm(cfg.embed_dim, cfg.rms_eps)
        self.lm_head = nn.Linear(cfg.embed_dim, cfg.vocab_size, bias=False)
        self.reset_parameters()

    def reset_parameters(self) -> None:
        for module in self.modules():
            if isinstance(module, (nn.Linear, nn.Embedding)):
                nn.init.xavier_uniform_(module.weight)
            elif isinstance(module, RMSNorm):
                nn.init.ones_(module.weight)

    def forward(self, ids: torch.Tensor) -> torch.Tensor:
        if ids.shape[1] > self.cfg.ctx_len:
            raise ValueError("sequence exceeds configured context")
        x = self.token_embed(ids)
        for layer in self.layers:
            x = layer(x)
        return self.lm_head(self.rms_final(x))


@dataclass
class Record:
    rid: str
    instruction: str
    response: str


def dataset_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_records(path: Path) -> list[Record]:
    records: list[Record] = []
    seen: set[str] = set()
    with path.open("r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            if not raw.strip():
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: expected JSONL record") from exc
            if not isinstance(obj, dict):
                raise ValueError(f"{path}:{lineno}: record must be an object")
            instruction, response = obj.get("instruction"), obj.get("response")
            rid = str(obj.get("id", f"line-{lineno}"))
            if not isinstance(instruction, str) or not instruction.strip():
                raise ValueError(f"{path}:{lineno}: missing instruction")
            if not isinstance(response, str) or not response.strip():
                raise ValueError(f"{path}:{lineno}: missing response")
            if rid in seen:
                raise ValueError(f"{path}:{lineno}: duplicate id {rid!r}")
            seen.add(rid)
            records.append(Record(rid, instruction.strip(), response.strip()))
    if len(records) < 2:
        raise ValueError("training requires at least two records")
    return records


def split_records(records: list[Record], val_ratio: float) -> tuple[list[Record], list[Record]]:
    ordered = sorted(records, key=lambda r: hashlib.sha256(r.rid.encode("utf-8")).digest())
    n_val = min(max(1, int(round(len(ordered) * val_ratio))), len(ordered) - 1)
    return ordered[n_val:], ordered[:n_val]


class SFTDataset(Dataset):
    def __init__(self, records: list[Record], tok: CasperTokenizer, ctx_len: int) -> None:
        self.items: list[tuple[list[int], list[int]]] = []
        max_total = ctx_len + 1
        prefix = tok.encode("Instruction:\n", bos=False, eos=False)
        suffix = tok.encode("\nResponse:\n", bos=False, eos=False)
        max_prompt = max(len(prefix) + len(suffix), ctx_len // 2)
        max_instruction = max_prompt - len(prefix) - len(suffix)
        if max_total - 2 - max_prompt < 1:
            raise ValueError("ctx_len is too small for the SFT delimiters and response")
        for rec in records:
            instruction_ids = tok.encode(rec.instruction, bos=False, eos=False)[:max_instruction]
            prompt_ids = prefix + instruction_ids + suffix
            room = max_total - 2 - len(prompt_ids)
            response_ids = tok.encode(rec.response, bos=False, eos=False)[:room]
            seq = [TOK_BOS] + prompt_ids + response_ids + [TOK_EOS]
            response_start = 1 + len(prompt_ids)
            inputs, labels = seq[:-1], seq[1:]
            labels[:response_start - 1] = [-100] * (response_start - 1)
            self.items.append((inputs, labels))
        if not self.items:
            raise ValueError("no trainable samples remain after tokenization/truncation")

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, idx: int) -> tuple[list[int], list[int]]:
        return self.items[idx]


def collate(batch: list[tuple[list[int], list[int]]]) -> tuple[torch.Tensor, torch.Tensor]:
    max_len = max(len(x[0]) for x in batch)
    ids = torch.full((len(batch), max_len), TOK_PAD, dtype=torch.long)
    labels = torch.full((len(batch), max_len), -100, dtype=torch.long)
    for i, (inp, lab) in enumerate(batch):
        ids[i, :len(inp)] = torch.tensor(inp, dtype=torch.long)
        labels[i, :len(lab)] = torch.tensor(lab, dtype=torch.long)
    return ids, labels


def evaluate(model: CasperModel, loader: DataLoader, device: torch.device) -> float:
    model.eval()
    total, count = 0.0, 0
    with torch.no_grad():
        for ids, labels in loader:
            ids, labels = ids.to(device), labels.to(device)
            logits = model(ids)
            loss = F.cross_entropy(logits.reshape(-1, logits.shape[-1]), labels.reshape(-1), ignore_index=-100)
            tokens = int((labels != -100).sum())
            total += float(loss) * tokens
            count += tokens
    return total / max(count, 1)


def export_bin(model: CasperModel, path: Path) -> str:
    cfg = model.cfg
    header = struct.pack("<9I2fI16s", NIYAH_MAGIC, NIYAH_VER, cfg.embed_dim,
                         cfg.n_heads, cfg.n_kv_heads, cfg.n_layers, cfg.ffn_mult,
                         cfg.vocab_size, cfg.ctx_len, cfg.rope_theta, cfg.rms_eps, 0, b"\0" * 16)
    if len(header) != 64:
        raise AssertionError("NIYAH header must be 64 bytes")
    tensors: list[torch.Tensor] = []
    for layer in model.layers:
        tensors.extend([layer.wq.weight, layer.wk.weight, layer.wv.weight, layer.wo.weight,
                        layer.w_gate.weight, layer.w_up.weight, layer.w_down.weight,
                        layer.rms_att.weight, layer.rms_ffn.weight])
    tensors.extend([model.token_embed.weight, model.rms_final.weight, model.lm_head.weight])
    tmp = path.with_suffix(path.suffix + ".tmp")
    h = hashlib.sha256()
    with tmp.open("wb") as f:
        f.write(header)
        h.update(header)
        for tensor in tensors:
            data = tensor.detach().float().cpu().contiguous().numpy().astype("<f4", copy=False).tobytes(order="C")
            f.write(data)
            h.update(data)
    tmp.replace(path)
    return h.hexdigest()


def parameter_count(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters())


def save_checkpoint(path: Path, model: CasperModel, optimizer: torch.optim.Optimizer,
                    epoch: int, best_val: float, dataset_hash: str) -> None:
    payload = {"format": "CASPER-TRAIN-CHECKPOINT-V1", "epoch": epoch, "best_val": best_val,
               "dataset_sha256": dataset_hash, "config": asdict(model.cfg),
               "model": model.state_dict(), "optimizer": optimizer.state_dict()}
    tmp = path.with_suffix(path.suffix + ".tmp")
    torch.save(payload, tmp)
    tmp.replace(path)


def validate_checkpoint_payload(payload: object) -> dict:
    if not isinstance(payload, dict):
        raise ValueError("checkpoint payload must be a dictionary")
    required = {"format", "epoch", "best_val", "dataset_sha256", "config", "model", "optimizer"}
    missing = sorted(required.difference(payload))
    if missing:
        raise ValueError(f"checkpoint missing required fields: {', '.join(missing)}")
    if payload.get("format") != "CASPER-TRAIN-CHECKPOINT-V1":
        raise ValueError("unsupported checkpoint format")
    epoch = payload.get("epoch")
    if not isinstance(epoch, int) or isinstance(epoch, bool) or epoch < 0:
        raise ValueError("checkpoint epoch must be a non-negative integer")
    best_val = payload.get("best_val")
    if not isinstance(best_val, (int, float)) or isinstance(best_val, bool) or not math.isfinite(float(best_val)):
        raise ValueError("checkpoint best_val must be finite")
    dataset_hash = payload.get("dataset_sha256")
    if not isinstance(dataset_hash, str) or len(dataset_hash) != 64 or any(c not in "0123456789abcdef" for c in dataset_hash):
        raise ValueError("checkpoint dataset_sha256 must be lowercase SHA-256 hex")
    if not isinstance(payload.get("config"), dict):
        raise ValueError("checkpoint config must be a dictionary")
    if not isinstance(payload.get("model"), dict) or not isinstance(payload.get("optimizer"), dict):
        raise ValueError("checkpoint model and optimizer states must be dictionaries")
    return payload


def load_checkpoint(path: Path, device: torch.device) -> dict:
    try:
        payload = torch.load(path, map_location=device, weights_only=True)
    except TypeError as exc:
        raise RuntimeError("safe checkpoint resume requires PyTorch with weights_only support") from exc
    return validate_checkpoint_payload(payload)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Train the full Casper transformer and export NIYAH .bin")
    p.add_argument("dataset", type=Path, help="JSONL with instruction/response fields")
    p.add_argument("--output", type=Path, default=Path("casper_trained.bin"))
    p.add_argument("--checkpoint", type=Path, default=Path("casper_training.pt"))
    p.add_argument("--resume", action="store_true")
    for name, default in (("epochs", 12), ("batch-size", 4), ("grad-accum", 4),
                          ("ctx-len", 256), ("embed-dim", 128), ("layers", 4),
                          ("heads", 8), ("kv-heads", 4), ("ffn-mult", 4), ("seed", 1337)):
        p.add_argument("--" + name, type=int, default=default)
    for name, default in (("lr", 3e-4), ("weight-decay", 0.01), ("clip-grad", 1.0), ("val-ratio", 0.1)):
        p.add_argument("--" + name, type=float, default=default)
    p.add_argument("--device", default="auto", choices=["auto", "cuda", "cpu"])
    p.add_argument("--no-amp", action="store_true")
    p.add_argument("--deterministic", action="store_true")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if not args.dataset.is_file():
        raise SystemExit(f"dataset not found: {args.dataset}")
    if args.epochs <= 0 or args.batch_size <= 0 or args.grad_accum <= 0:
        raise SystemExit("epochs, batch-size and grad-accum must be positive")
    if any(not math.isfinite(value) or value <= 0 for value in (args.lr, args.clip_grad)):
        raise SystemExit("lr and clip-grad must be finite and positive")
    if not math.isfinite(args.weight_decay) or args.weight_decay < 0:
        raise SystemExit("weight-decay must be finite and non-negative")
    if not (0.0 < args.val_ratio < 1.0):
        raise SystemExit("val-ratio must be between 0 and 1")
    if args.device == "cuda" and not torch.cuda.is_available():
        raise SystemExit("CUDA requested but torch.cuda.is_available() is false")
    device = torch.device("cuda" if args.device == "cuda" or (args.device == "auto" and torch.cuda.is_available()) else "cpu")
    random.seed(args.seed)
    torch.manual_seed(args.seed)
    if device.type == "cuda":
        torch.cuda.manual_seed_all(args.seed)
        torch.backends.cuda.matmul.allow_tf32 = True
    if args.deterministic:
        torch.use_deterministic_algorithms(True)
    tok = CasperTokenizer()
    verify_tokenizer_contract(tok)
    cfg = ModelConfig(vocab_size=tok.vocab_size, ctx_len=args.ctx_len, embed_dim=args.embed_dim,
                      n_layers=args.layers, n_heads=args.heads, n_kv_heads=args.kv_heads, ffn_mult=args.ffn_mult)
    cfg.validate()
    records = load_records(args.dataset)
    train_records, val_records = split_records(records, args.val_ratio)
    train_ds, val_ds = SFTDataset(train_records, tok, cfg.ctx_len), SFTDataset(val_records, tok, cfg.ctx_len)
    generator = torch.Generator().manual_seed(args.seed)
    train_loader = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True, generator=generator, collate_fn=collate)
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False, collate_fn=collate)
    model = CasperModel(cfg).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    start_epoch, best_val = 1, float("inf")
    dsha = dataset_sha256(args.dataset)
    if args.resume:
        if not args.checkpoint.is_file():
            raise SystemExit(f"checkpoint not found: {args.checkpoint}")
        ckpt = load_checkpoint(args.checkpoint, device)
        if asdict(ModelConfig(**ckpt["config"])) != asdict(cfg):
            raise SystemExit("checkpoint config differs from requested model config")
        if ckpt.get("dataset_sha256") != dsha:
            raise SystemExit("checkpoint dataset hash differs; refusing unsafe resume")
        model.load_state_dict(ckpt["model"])
        optimizer.load_state_dict(ckpt["optimizer"])
        start_epoch, best_val = int(ckpt["epoch"]) + 1, float(ckpt["best_val"])
    if start_epoch > args.epochs:
        raise SystemExit("checkpoint already reached requested epochs; increase --epochs")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.checkpoint.parent.mkdir(parents=True, exist_ok=True)
    amp_enabled = device.type == "cuda" and not args.no_amp
    scaler = torch.cuda.amp.GradScaler(enabled=amp_enabled)
    print(f"device={device} amp={amp_enabled}")
    if device.type == "cuda":
        print(f"gpu={torch.cuda.get_device_name(0)}")
    print(f"records={len(records)} train={len(train_ds)} validation={len(val_ds)}")
    print(f"tokenizer_vocab={tok.vocab_size} parameters={parameter_count(model)}")
    print(f"dataset_sha256={dsha}")
    for epoch in range(start_epoch, args.epochs + 1):
        model.train()
        optimizer.zero_grad(set_to_none=True)
        running, supervised_tokens, window_tokens = 0.0, 0, 0
        for step, (ids, labels) in enumerate(train_loader, 1):
            ids, labels = ids.to(device), labels.to(device)
            with torch.cuda.amp.autocast(enabled=amp_enabled, dtype=torch.float16):
                logits = model(ids)
                loss = F.cross_entropy(logits.reshape(-1, logits.shape[-1]), labels.reshape(-1), ignore_index=-100)
                batch_tokens = int((labels != -100).sum())
                scaled_loss = loss * batch_tokens
            scaler.scale(scaled_loss).backward()
            window_tokens += batch_tokens
            if step % args.grad_accum == 0 or step == len(train_loader):
                scaler.unscale_(optimizer)
                for parameter in model.parameters():
                    if parameter.grad is not None:
                        parameter.grad.div_(window_tokens)
                torch.nn.utils.clip_grad_norm_(model.parameters(), args.clip_grad)
                scaler.step(optimizer)
                scaler.update()
                optimizer.zero_grad(set_to_none=True)
                window_tokens = 0
            running += float(loss.detach()) * batch_tokens
            supervised_tokens += batch_tokens
        train_loss = running / max(supervised_tokens, 1)
        val_loss = evaluate(model, val_loader, device)
        best_val = min(best_val, val_loss)
        save_checkpoint(args.checkpoint, model, optimizer, epoch, best_val, dsha)
        model_hash = export_bin(model, args.output)
        manifest = {"format": "CASPER-TRAIN-MANIFEST-V1", "epoch": epoch, "dataset_sha256": dsha,
                    "records": len(records), "train_records": len(train_ds), "validation_records": len(val_ds),
                    "train_loss": train_loss, "validation_loss": val_loss, "best_validation_loss": best_val,
                    "model_sha256": model_hash, "parameters": parameter_count(model), "config": asdict(cfg),
                    "tokenizer_vocab_size": tok.vocab_size, "full_model_optimizer": "AdamW"}
        args.output.with_suffix(args.output.suffix + ".manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"epoch={epoch} train_loss={train_loss:.6f} val_loss={val_loss:.6f} best={best_val:.6f} model_sha256={model_hash}")
    print(f"PASS output={args.output} checkpoint={args.checkpoint}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
