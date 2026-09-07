#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SYSTEM_PROMPT = (
    "You are generating supervision data for a small offline language model. "
    "Answer the instruction directly and self-containedly. Do not expose chain-of-thought, "
    "hidden reasoning, scratch work, or internal deliberation. Prefer concise, technically "
    "correct answers. Use code only when the instruction asks for code."
)


@dataclass(frozen=True)
class Prompt:
    pid: str
    category: str
    instruction: str


def _json_request(url: str, payload: dict | None, timeout: float) -> dict:
    data = None
    headers = {"Accept": "application/json"}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {body[:1000]}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"cannot reach {url}: {exc.reason}") from exc
    try:
        obj = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"invalid JSON from {url}") from exc
    if not isinstance(obj, dict):
        raise RuntimeError(f"unexpected response from {url}")
    return obj


def discover_model(base_url: str, timeout: float) -> str:
    obj = _json_request(base_url.rstrip("/") + "/v1/models", None, timeout)
    data = obj.get("data")
    if not isinstance(data, list) or not data or not isinstance(data[0], dict):
        raise RuntimeError("teacher /v1/models returned no model")
    model_id = data[0].get("id")
    if not isinstance(model_id, str) or not model_id:
        raise RuntimeError("teacher /v1/models returned an invalid model id")
    return model_id


def teacher_completion(
    base_url: str,
    model: str,
    instruction: str,
    timeout: float,
    max_tokens: int,
    temperature: float,
    top_p: float,
) -> str:
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": instruction},
        ],
        "temperature": temperature,
        "top_p": top_p,
        "max_tokens": max_tokens,
        "reasoning_effort": "none",
        "stream": False,
    }
    obj = _json_request(base_url.rstrip("/") + "/v1/chat/completions", payload, timeout)
    choices = obj.get("choices")
    if not isinstance(choices, list) or not choices or not isinstance(choices[0], dict):
        raise RuntimeError("teacher returned no choices")
    message = choices[0].get("message")
    if not isinstance(message, dict):
        raise RuntimeError("teacher returned no message")
    content = message.get("content")
    if not isinstance(content, str) or not content.strip():
        raise RuntimeError("teacher returned empty final content")
    return content.strip()


def _make_id(category: str, instruction: str) -> str:
    digest = hashlib.sha256((category + "\0" + instruction).encode("utf-8")).hexdigest()[:16]
    return f"{category}-{digest}"


def _prompt(category: str, instruction: str) -> Prompt:
    return Prompt(_make_id(category, instruction), category, instruction)


def curriculum(count: int, seed: int) -> list[Prompt]:
    rng = random.Random(seed)
    prompts: list[Prompt] = []
    seen: set[str] = set()

    systems_questions = [
        "Explain the difference between stack and heap allocation in C11 in no more than six sentences.",
        "Explain what undefined behavior means in C and give three concrete examples.",
        "Write a C11 function that safely computes the dot product of two float arrays and validates null pointers.",
        "Write a C11 function that parses an unsigned 32-bit decimal integer and rejects overflow.",
        "Explain why bounds checks must happen before pointer arithmetic in memory-safe C code.",
        "Explain the difference between a process and a thread on Linux.",
        "Explain what a file descriptor is and how open, read, write, and close relate to it.",
        "Explain the purpose of mmap and one case where ordinary read calls are preferable.",
        "Describe a deterministic binary file format header that includes magic, version, dimensions, and flags.",
        "Explain why model file loaders must validate sizes before allocating or reading tensors.",
        "Explain RMSNorm and how it differs from LayerNorm.",
        "Explain grouped-query attention and why n_kv_heads can be smaller than n_heads.",
        "Explain rotary positional embeddings without using hidden chain-of-thought.",
        "Explain AdamW and the difference between weight decay and L2 regularization in adaptive optimizers.",
        "Explain gradient clipping and when it is useful during neural-network training.",
        "Explain causal attention masking for autoregressive language models.",
        "Explain teacher distillation and distinguish response SFT from logit distillation.",
        "Explain why a small student model cannot preserve all capabilities of a much larger teacher.",
        "Explain train/validation data leakage and how stable record IDs can help create a deterministic split.",
        "Explain why checkpoint resume should verify both model configuration and dataset hash.",
    ]

    crypto_questions = [
        "Explain the security properties provided by SHA-256 and the properties it does not provide.",
        "Explain the difference between hashing, encryption, and digital signatures.",
        "Describe how to hash a file in streaming chunks without loading the entire file into memory.",
        "Explain why comparing a stored SHA-256 digest can detect accidental or malicious file modification but cannot prove who created the file.",
        "Explain what domain separation means in cryptographic hashing and give a compact example.",
        "Explain why a proof artifact should include a versioned format identifier.",
        "Explain the difference between a checksum and a cryptographic hash.",
        "Explain replay attacks and name two common defenses.",
        "Explain nonce reuse risk in authenticated encryption at a high level.",
        "Explain constant-time comparison and why ordinary early-exit byte comparison can leak information.",
    ]

    algorithms_questions = [
        "Give the time and space complexity of binary search and state its precondition.",
        "Compare a hash table and a balanced binary search tree for lookup, insertion, ordering, and worst-case behavior.",
        "Explain topological sorting and when a directed graph has no valid topological order.",
        "Explain Dijkstra's algorithm and why negative edge weights violate its assumptions.",
        "Explain breadth-first search and when it finds a shortest path.",
        "Explain stable sorting and give one situation where stability matters.",
        "Explain a ring buffer and give one practical systems use case.",
        "Explain the producer-consumer problem and one bounded-queue solution.",
        "Explain integer overflow in fixed-width arithmetic and how to check multiplication safely.",
        "Explain why deterministic iteration order can matter for reproducible builds and tests.",
    ]

    linux_network_questions = [
        "Explain the difference between TCP and UDP without saying one is universally better.",
        "Explain the TCP three-way handshake and what state each endpoint establishes.",
        "Explain DNS A and AAAA records and how they differ.",
        "Explain the difference between 127.0.0.1, 0.0.0.0, and a host LAN address when binding a server.",
        "Explain what a Linux default route is and what the gateway field means.",
        "Explain NAT at a high level and why inbound connections often require explicit forwarding.",
        "Explain TLS certificate hostname validation.",
        "Explain what an HTTP status code 429 means and how a client should react.",
        "Explain the difference between a container image and a running container.",
        "Explain why pinning dependency versions improves reproducibility but does not by itself guarantee security.",
    ]

    logic_questions = [
        "Represent the rule 'if A and B then C' using a compact symbolic notation and explain how forward chaining applies it.",
        "Explain the difference between a fact, a rule, and a constraint in a symbolic reasoning engine.",
        "Explain unification in first-order logic using a small variable substitution example.",
        "Explain why cyclic rules can cause nontermination and name one way to prevent it.",
        "Explain satisfiable versus unsatisfiable constraints with a numeric example.",
        "Explain why provenance should be attached to derived facts in an auditable reasoning system.",
        "Explain the closed-world assumption and how it differs from the open-world assumption.",
        "Explain monotonic reasoning and give one example of a non-monotonic update.",
    ]

    arabic_questions = [
        "اشرح بالعربية الفصحى الفرق بين الذاكرة المؤقتة والذاكرة الدائمة في الحاسوب بإيجاز.",
        "اشرح بالعربية الفصحى ما هي دالة التجزئة SHA-256 وما الذي لا توفره من ضمانات.",
        "اشرح بالعربية الفصحى الفرق بين TCP وUDP بشكل تقني ومختصر.",
        "اشرح بالعربية الفصحى معنى التدريب تحت إشراف نموذج معلّم لطالب أصغر.",
        "اشرح بالعربية الفصحى لماذا يجب التحقق من حدود المصفوفة قبل القراءة أو الكتابة في لغة C.",
        "حوّل العبارة التالية إلى إنجليزية تقنية واضحة: يجب أن يبقى النظام قابلاً للعمل دون اتصال بالإنترنت.",
        "Translate into concise Arabic: The checkpoint is rejected when the dataset hash does not match.",
        "اشرح بالعربية معنى التنفيذ الحتمي ولماذا يفيد في الاختبارات القابلة لإعادة الإنتاج.",
    ]

    static_groups = [
        ("systems", systems_questions),
        ("crypto", crypto_questions),
        ("algorithms", algorithms_questions),
        ("network", linux_network_questions),
        ("logic", logic_questions),
        ("arabic", arabic_questions),
    ]

    for category, questions in static_groups:
        for instruction in questions:
            p = _prompt(category, instruction)
            if p.pid not in seen:
                prompts.append(p)
                seen.add(p.pid)

    while len(prompts) < count:
        category = rng.choice(("arithmetic", "bitwise", "constraints", "format", "c-code", "bilingual"))
        if category == "arithmetic":
            a = rng.randint(-50000, 50000)
            b = rng.randint(-50000, 50000)
            op = rng.choice(("+", "-", "*"))
            instruction = f"Return only the exact integer result: {a} {op} {b}"
        elif category == "bitwise":
            a = rng.randint(0, 0xFFFF)
            b = rng.randint(0, 0xFFFF)
            op = rng.choice(("AND", "OR", "XOR"))
            instruction = (
                f"Compute 0x{a:04X} {op} 0x{b:04X}. Return only the result as uppercase hexadecimal with 0x prefix."
            )
        elif category == "constraints":
            x = rng.randint(-100, 100)
            delta = rng.randint(1, 50)
            instruction = (
                f"Given integer constraints x >= {x} and x <= {x + delta}, state the inclusive valid interval in the form [min,max]."
            )
        elif category == "format":
            name = rng.choice(("casper", "niyah", "runtime", "trainer", "proof", "model"))
            version = rng.randint(1, 9)
            enabled = rng.choice((True, False))
            instruction = (
                "Return one compact JSON object and nothing else with keys name, version, enabled using values "
                f"name={name!r}, version={version}, enabled={str(enabled).lower()}."
            )
        elif category == "c-code":
            typename = rng.choice(("uint32_t", "uint64_t", "size_t", "int32_t"))
            instruction = (
                f"Write a strictly C11 function named max_value that returns the larger of two {typename} values. "
                "Return only the function definition."
            )
        else:
            term = rng.choice((
                "deterministic execution",
                "causal attention",
                "gradient clipping",
                "cryptographic hash",
                "memory safety",
                "constraint solver",
                "offline inference",
                "model checkpoint",
            ))
            instruction = f"Explain '{term}' in one concise English sentence, then one concise Arabic sentence."

        p = _prompt(category, instruction)
        if p.pid not in seen:
            prompts.append(p)
            seen.add(p.pid)

    rng.shuffle(prompts)
    return prompts[:count]


def load_prompts(path: Path) -> list[Prompt]:
    prompts: list[Prompt] = []
    seen: set[str] = set()
    if path.suffix.lower() == ".jsonl":
        with path.open("r", encoding="utf-8") as f:
            for lineno, raw in enumerate(f, 1):
                line = raw.strip()
                if not line:
                    continue
                obj = json.loads(line)
                if not isinstance(obj, dict):
                    raise ValueError(f"{path}:{lineno}: expected JSON object")
                instruction = obj.get("instruction", obj.get("prompt"))
                category = str(obj.get("category", "custom"))
                if not isinstance(instruction, str) or not instruction.strip():
                    raise ValueError(f"{path}:{lineno}: missing instruction")
                p = _prompt(category, instruction.strip())
                if p.pid not in seen:
                    prompts.append(p)
                    seen.add(p.pid)
    else:
        with path.open("r", encoding="utf-8") as f:
            for raw in f:
                instruction = raw.strip()
                if not instruction:
                    continue
                p = _prompt("custom", instruction)
                if p.pid not in seen:
                    prompts.append(p)
                    seen.add(p.pid)
    if not prompts:
        raise ValueError(f"no prompts found in {path}")
    return prompts


def completed_ids(path: Path) -> set[str]:
    ids: set[str] = set()
    if not path.is_file():
        return ids
    with path.open("r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid existing JSONL") from exc
            rid = obj.get("id") if isinstance(obj, dict) else None
            if isinstance(rid, str) and rid:
                ids.add(rid)
    return ids


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate Casper SFT JSONL from a local OpenAI-compatible teacher")
    p.add_argument("--base-url", default="http://127.0.0.1:8081")
    p.add_argument("--model", default="auto")
    p.add_argument("--output", type=Path, default=Path("Data_Training/teacher.jsonl"))
    p.add_argument("--prompts", type=Path)
    p.add_argument("--count", type=int, default=512)
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--max-tokens", type=int, default=256)
    p.add_argument("--temperature", type=float, default=0.2)
    p.add_argument("--top-p", type=float, default=0.9)
    p.add_argument("--timeout", type=float, default=180.0)
    p.add_argument("--retries", type=int, default=3)
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if args.count <= 0:
        raise SystemExit("--count must be positive")
    if args.max_tokens <= 0:
        raise SystemExit("--max-tokens must be positive")
    if args.retries <= 0:
        raise SystemExit("--retries must be positive")
    if not (0.0 <= args.temperature <= 2.0):
        raise SystemExit("--temperature must be in [0,2]")
    if not (0.0 < args.top_p <= 1.0):
        raise SystemExit("--top-p must be in (0,1]")

    prompts = load_prompts(args.prompts) if args.prompts else curriculum(args.count, args.seed)
    if args.prompts and args.count < len(prompts):
        prompts = prompts[: args.count]

    model = discover_model(args.base_url, args.timeout) if args.model == "auto" else args.model
    args.output.parent.mkdir(parents=True, exist_ok=True)
    done = completed_ids(args.output)

    print(f"teacher_base_url={args.base_url}")
    print(f"teacher_model={model}")
    print(f"prompts={len(prompts)} completed={len(done)}")

    written = 0
    with args.output.open("a", encoding="utf-8", buffering=1) as out:
        for index, prompt in enumerate(prompts, 1):
            if prompt.pid in done:
                continue

            last_error: Exception | None = None
            response = ""
            for attempt in range(1, args.retries + 1):
                try:
                    response = teacher_completion(
                        args.base_url,
                        model,
                        prompt.instruction,
                        args.timeout,
                        args.max_tokens,
                        args.temperature,
                        args.top_p,
                    )
                    break
                except Exception as exc:
                    last_error = exc
                    if attempt < args.retries:
                        time.sleep(min(2.0 ** (attempt - 1), 8.0))

            if not response:
                raise RuntimeError(
                    f"teacher failed for {prompt.pid} after {args.retries} attempts: {last_error}"
                )

            record = {
                "id": prompt.pid,
                "category": prompt.category,
                "instruction": prompt.instruction,
                "response": response,
                "teacher_model": model,
            }
            out.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
            done.add(prompt.pid)
            written += 1
            print(f"record={index}/{len(prompts)} id={prompt.pid} response_chars={len(response)}")

    print(f"written={written}")
    print(f"dataset={args.output}")
    print(f"dataset_sha256={file_sha256(args.output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
