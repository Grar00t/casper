# NIYAH-PROOF-V1: canonical receipt

Scope: byte-level integrity of an input prompt, generated output, and optional original rule file. This format does **not** certify source authenticity, truthful output, compliance with a rule, or model correctness.

## Digest

`hash = lowercase_hex(SHA-256(prompt_utf8 || output_utf8 || rule_file_bytes))`

- `prompt_utf8` and `output_utf8` are the exact bytes of their NUL-terminated UTF-8 strings; no BOM, separators, length prefixes, trimming, or normalization.
- `rule_file_bytes` is the raw byte stream read from the supplied file in binary mode, not the file path or a hash of its contents. With no rule file the suffix is the empty byte string.
- `prompt_hash`, `output_hash`, and `rules_hash` separately hash each component to permit checking it independently.
- `rules_bytes` records the number of raw rule-file bytes. An empty rule file and no rule file have the same digest and byte length.

## Serialized `.proof` file

The exact on-disk representation is eight newline-terminated printable ASCII lines, with LF (`0A`) terminators and fields in this order:

```text
NIYAH-PROOF-V1
hash: <64 lowercase hexadecimal characters>
prompt_hash: <64 lowercase hexadecimal characters>
output_hash: <64 lowercase hexadecimal characters>
rules_hash: <64 lowercase hexadecimal characters>
rules_bytes: <decimal byte count>
prompt_hex: <lowercase hex of prompt bytes>
output_hex: <lowercase hex of output bytes>
```

The angle-bracket expressions describe fields and are not literal file content. The strict parser rejects malformed order, missing or repeated/extra lines, control characters/CRLF, malformed hex, inconsistent component digests, and mismatch with the original rule bytes.

For nonempty rule material, verification requires the original rule file and rejects absent, changed, or unreadable files. V2 receipts use a different domain-separated construction; they are not V1 digests. Legacy V2 verification is retained separately.

## Known-answer check

`prompt = "a"`, `output = "b"`, rule file containing the single ASCII byte `c` produces SHA-256(`abc`):

`ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`

This concatenation is intentional and length-ambiguous without the separately stored component fields. It is an integrity checksum, not a signature or a chained historical audit log.
