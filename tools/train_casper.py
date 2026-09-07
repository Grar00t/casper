#!/usr/bin/env python3
"""Full-model Casper trainer.

Trains the exact C runtime architecture with PyTorch autograd and exports the
native NIYAH .bin layout consumed by Core_CPP/niyah_core.c. This path is for
teacher-distillation/SFT and uses exact full-sequence causal autograd.
"""
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
    raise SystemExit(
        "PyTorch is required for full-model training. Install a CUDA-enabled "
        "PyTorch build, then rerun this command."
    ) from exc

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
ARABIC_RANGES = [
    (0x0600, 0x06FF),
    (0x0750, 0x077F),
    (0x08A0, 0x08FF),
    (0xFB50, 0xFDFF),
    (0xFE70, 0xFEFF),
]


class CasperTokenizer:
    """Python mirror of tokenizer.c vocabulary v2."""

    def __init__(self) -> None:
        self.tokens: list[str] = ["<BOS>", "<EOS>", "<PAD>", "<UNK>"]
        self.tokens += list("0123456789")
        self.tokens += list(PUNCTUATION)
        self.tokens += list("abcdefghijklmnopqrstuvwxyz")
        self.tokens += list("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        self.tokens += WORDS

        self.word_end = len(self.tokens)
        self.lookup_exact = {tok: i for i, tok in enumerate(self.tokens[: self.word_end])}

        self.char_base = len(self.tokens)
        self.codepoint_to_id: dict[int, int] = {}
        for lo, hi in ARABIC_RANGES:
            for cp in range(lo, hi + 1):
                self.codepoint_to_id[cp] = len(self.tokens)
                self.tokens.append(chr(cp))

        self.byte_base = len(self.tokens)
        self.tokens += [f"<{b:02X}>" for b in range(256)]

        if self.char_base != 268 or self.byte_base != 1500 or len(self.tokens) != 1756:
            raise RuntimeError(
                f"tokenizer contract drift: char_base={self.char_base} "
                f"byte_base={self.byte_base} vocab={len(self.tokens)}"
            )

    @property
    def vocab_size(self) -> int:
        return len(self.tokens)

    def _lookup_ascii(self, text: str) -> int:
        # Exact-only lookup is required for a lossless tokenizer contract.
        # Case variants of compact tokens fall back to their original bytes.
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

            if ch.isspace():
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
                if ord(c) >= 128 or c.isspace() or c in PUNCTUATION:
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
            if 0 <= tid < self.vocab_size:
                chunks.append(self.tokens[tid].encode("utf-8"))
            else:
                chunks.append(b"<UNK>")
        flush_raw()
        return b"".join(chunks).decode("utf-8", errors="replace")


def verify_tokenizer_contract(tok: CasperTokenizer) -> None:
    probes = (
        "malloc allocates heap memory",
        "unknown teacher vocabulary survives exactly",
        "SHA-256 Casper TCP UDP",
        "بسم الله",
        "casper نية engine",
        "emoji: 🧠 CJK: 汉字",
        "line one\nline two\tindent",
    )
    for probe in probes:
        roundtrip = tok.decode(tok.encode(probe))
        if roundtrip != probe:
            raise RuntimeError(
                f"tokenizer round-trip failure: expected={probe!r} actual={roundtrip!r}"
            )


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
    # x: [B, H, T, HD]. Matches C: angle=pos/theta^(pair_even_index/HD).
    hd = x.shape[-1]
    if hd % 2:
        raise ValueError("head_dim must be even for RoPE")
    device = x.device
    pair_index = torch.arange(0, hd, 2, device=device, dtype=torch.float32)
    inv_freq = torch.pow(torch.tensor(theta, device=device), -pair_index / float(hd))
    pos = torch.arange(x.shape[-2], device=device, dtype=torch.float32)
    angle = pos[:, None] * inv_freq[None, :]
    cos = angle.cos()[None, None, :, :].to(dtype=x.dtype)
    sin = angle.sin()[None, None, :, :].to(dtype=x.dtype)
    even, odd = x[..., 0::2], x[..., 1::2]
    return torch.stack((even * cos - odd * sin, even * sin + odd * cos), dim=-1).flatten(-2)


class Block(nn.Module):
    def __init__(self, cfg: ModelConfig) -> None:
        super().__init__()
        d = cfg.embed_dim
        hd = d // cfg.n_heads
        kd = cfg.n_kv_heads * hd
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
        kg = k[:, kv_map, :, :]
        vg = v[:, kv_map, :, :]
        scores = torch.matmul(q, kg.transpose(-2, -1)) / math.sqrt(float(hd))
        causal = torch.triu(torch.ones(t, t, device=x.device, dtype=torch.bool), diagonal=1)
        scores = scores.masked_fill(causal[None, None, :, :], float("-inf"))
        attn = torch.softmax(scores.float(), dim=-1).to(dtype=q.dtype)
        a = torch.matmul(attn, vg).transpose(1, 2).contiguous().view(b, t, d)
        x = x + self.wo(a)

        y = self.rms_ffn(x)
        ff = F.silu(self.w_gate(y)) * self.w_up(y)
        return x + self.w_down(ff)


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
            if isinstance(module, nn.Linear):
                nn.init.xavier_uniform_(module.weight)
            elif isinstance(module, nn.Embedding):
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
            line = raw.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: expected JSONL record") from exc
            if not isinstance(obj, dict):
                raise ValueError(f"{path}:{lineno}: record must be an object")
            instruction = obj.get("instruction")
            response = obj.get("response")
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
    n_val = max(1, int(round(len(ordered) * val_ratio)))
    n_val = min(n_val, len(ordered) - 1)
    return ordered[n_val:], ordered[:n_val]


class SFTDataset(Dataset):
    def __init__(self, records: list[Record], tok: CasperTokenizer, ctx_len: int) -> None:
        self.items: list[tuple[list[int], list[int]]] = []
        max_total = ctx_len + 1
        max_prompt = max(8, ctx_len // 2)

        for rec in records:
            prompt = f"Instruction:\n{rec.instruction}\nResponse:\n"
            prompt_ids = tok.encode(prompt, bos=False, eos=False)[:max_prompt]
            response_ids = tok.encode(rec.response, bos=False, eos=False)
            room = max_total - 2 - len(prompt_ids)
            if room < 1:
                continue
            response_ids = response_ids[:room]
            seq = [TOK_BOS] + prompt_ids + response_ids + [TOK_EOS]
            response_start = 1 + len(prompt_ids)
            inputs = seq[:-1]
            labels = seq[1:]
            mask_before = max(0, response_start - 1)
            labels[:mask_before] = [-100] * mask_before
            if not any(v != -100 for v in labels):
                continue
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
        ids[i, : len(inp)] = torch.tensor(inp, dtype=torch.long)
        labels[i, : len(lab)] = torch.tensor(lab, dtype=torch.long)
    return ids, labels


def evaluate(model: CasperModel, loader: DataLoader, device: torch.device) -> float:
    model.eval()
    total = 0.0
    count = 0
    with torch.no_grad():
        for ids, labels in loader:
            ids, labels = ids.to(device), labels.to(device)
            logits = model(ids)
            loss = F.cross_entropy(
                logits.reshape(-1, logits.shape[-1]),
                labels.reshape(-1),
                ignore_index=-100,
            )
            total += float(loss)
            count += 1
    return total / max(count, 1)


def export_bin(model: CasperModel, path: Path) -> str:
    cfg = model.cfg
    header = struct.pack(
        "<9I2fI16s",
        NIYAH_MAGIC,
        NIYAH_VER,
        cfg.embed_dim,
        cfg.n_heads,
        cfg.n_kv_heads,
        cfg.n_layers,
        cfg.ffn_mult,
        cfg.vocab_size,
        cfg.ctx_len,
        cfg.rope_theta,
        cfg.rms_eps,
        0,
        b"\0" * 16,
    )
    if len(header) != 64:
        raise AssertionError("NIYAH header must be 64 bytes")

    tensors: list[torch.Tensor] = []
    for layer in model.layers:
        tensors.extend([
            layer.wq.weight,
            layer.wk.weight,
            layer.wv.weight,
            layer.wo.weight,
            layer.w_gate.weight,
            layer.w_up.weight,
            layer.w_down.weight,
            layer.rms_att.weight,
            layer.rms_ffn.weight,
        ])
    tensors.extend([model.token_embed.weight, model.rms_final.weight, model.lm_head.weight])

    tmp = path.with_suffix(path.suffix + ".tmp")
    h = hashlib.sha256()
    with tmp.open("wb") as f:
        f.write(header)
        h.update(header)
        for tensor in tensors:
            arr = tensor.detach().float().cpu().contiguous().numpy().astype("<f4", copy=False)
            data = arr.tobytes(order="C")
            f.write(data)
            h.update(data)
    tmp.replace(path)
    return h.hexdigest()


def parameter_count(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters())


def save_checkpoint(path: Path, model: CasperModel, optimizer: torch.optim.Optimizer,
                    epoch: int, best_val: float, dataset_hash: str) -> None:
    payload = {
        "format": "CASPER-TRAIN-CHECKPOINT-V1",
        "epoch": epoch,
        "best_val": best_val,
        "dataset_sha256": dataset_hash,
        "config": asdict(model.cfg),
        "model": model.state_dict(),
        "optimizer": optimizer.state_dict(),
    }
    tmp = path.with_suffix(path.suffix + ".tmp")
    torch.save(payload, tmp)
    tmp.replace(path)


def load_checkpoint(path: Path, device: torch.device) -> dict:
    try:
        return torch.load(path, map_location=device, weights_only=False)
    except TypeError:
        return torch.load(path, map_location=device)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Train the full Casper transformer and export NIYAH .bin")
    p.add_argument("dataset", type=Path, help="teacher JSONL with instruction/response fields")
    p.add_argument("--output", type=Path, default=Path("casper_trained.bin"))
    p.add_argument("--checkpoint", type=Path, default=Path("casper_training.pt"))
    p.add_argument("--resume", action="store_true")
    p.add_argument("--epochs", type=int, default=12)
    p.add_argument("--batch-size", type=int, default=4)
    p.add_argument("--grad-accum", type=int, default=4)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--weight-decay", type=float, default=0.01)
    p.add_argument("--clip-grad", type=float, default=1.0)
    p.add_argument("--val-ratio", type=float, default=0.1)
    p.add_argument("--ctx-len", type=int, default=256)
    p.add_argument("--embed-dim", type=int, default=128)
    p.add_argument("--layers", type=int, default=4)
    p.add_argument("--heads", type=int, default=8)
    p.add_argument("--kv-heads", type=int, default=4)
    p.add_argument("--ffn-mult", type=int, default=4)
    p.add_argument("--seed", type=int, default=1337)
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
    if not (0.0 < args.val_ratio < 1.0):
        raise SystemExit("val-ratio must be between 0 and 1")

    if args.device == "cuda":
        if not torch.cuda.is_available():
            raise SystemExit("CUDA requested but torch.cuda.is_available() is false")
        device = torch.device("cuda")
    elif args.device == "cpu":
        device = torch.device("cpu")
    else:
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    random.seed(args.seed)
    torch.manual_seed(args.seed)
    if device.type == "cuda":
        torch.cuda.manual_seed_all(args.seed)
        torch.backends.cuda.matmul.allow_tf32 = True
    if args.deterministic:
        torch.use_deterministic_algorithms(True)

    tok = CasperTokenizer()
    verify_tokenizer_contract(tok)
    cfg = ModelConfig(
        vocab_size=tok.vocab_size,
        ctx_len=args.ctx_len,
        embed_dim=args.embed_dim,
        n_layers=args.layers,
        n_heads=args.heads,
        n_kv_heads=args.kv_heads,
        ffn_mult=args.ffn_mult,
    )
    cfg.validate()

    records = load_records(args.dataset)
    train_records, val_records = split_records(records, args.val_ratio)
    train_ds = SFTDataset(train_records, tok, cfg.ctx_len)
    val_ds = SFTDataset(val_records, tok, cfg.ctx_len)
    generator = torch.Generator().manual_seed(args.seed)
    train_loader = DataLoader(
        train_ds,
        batch_size=args.batch_size,
        shuffle=True,
        generator=generator,
        collate_fn=collate,
    )
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False, collate_fn=collate)

    model = CasperModel(cfg).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    start_epoch = 1
    best_val = float("inf")
    dsha = dataset_sha256(args.dataset)

    if args.resume:
        if not args.checkpoint.is_file():
            raise SystemExit(f"checkpoint not found: {args.checkpoint}")
        ckpt = load_checkpoint(args.checkpoint, device)
        ck_cfg = ModelConfig(**ckpt["config"])
        if asdict(ck_cfg) != asdict(cfg):
            raise SystemExit("checkpoint config differs from requested model config")
        if ckpt.get("dataset_sha256") != dsha:
            raise SystemExit("checkpoint dataset hash differs; refusing unsafe resume")
        model.load_state_dict(ckpt["model"])
        optimizer.load_state_dict(ckpt["optimizer"])
        start_epoch = int(ckpt["epoch"]) + 1
        best_val = float(ckpt["best_val"])

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
        running = 0.0
        batches = 0

        for step, (ids, labels) in enumerate(train_loader, 1):
            ids, labels = ids.to(device), labels.to(device)
            with torch.cuda.amp.autocast(enabled=amp_enabled, dtype=torch.float16):
                logits = model(ids)
                loss = F.cross_entropy(
                    logits.reshape(-1, logits.shape[-1]),
                    labels.reshape(-1),
                    ignore_index=-100,
                )
                scaled_loss = loss / args.grad_accum

            scaler.scale(scaled_loss).backward()
            if step % args.grad_accum == 0 or step == len(train_loader):
                scaler.unscale_(optimizer)
                torch.nn.utils.clip_grad_norm_(model.parameters(), args.clip_grad)
                scaler.step(optimizer)
                scaler.update()
                optimizer.zero_grad(set_to_none=True)

            running += float(loss.detach())
            batches += 1

        train_loss = running / max(batches, 1)
        val_loss = evaluate(model, val_loader, device)
        best_val = min(best_val, val_loss)
        save_checkpoint(args.checkpoint, model, optimizer, epoch, best_val, dsha)
        model_hash = export_bin(model, args.output)

        manifest = {
            "format": "CASPER-TRAIN-MANIFEST-V1",
            "epoch": epoch,
            "dataset_sha256": dsha,
            "records": len(records),
            "train_records": len(train_ds),
            "validation_records": len(val_ds),
            "train_loss": train_loss,
            "validation_loss": val_loss,
            "best_validation_loss": best_val,
            "model_sha256": model_hash,
            "parameters": parameter_count(model),
            "config": asdict(cfg),
            "tokenizer_vocab_size": tok.vocab_size,
            "full_model_optimizer": "AdamW",
        }
        args.output.with_suffix(args.output.suffix + ".manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(
            f"epoch={epoch} train_loss={train_loss:.6f} val_loss={val_loss:.6f} "
            f"best={best_val:.6f} model_sha256={model_hash}"
        )

    print(f"PASS output={args.output} checkpoint={args.checkpoint}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
