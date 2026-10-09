# Local Chronicle document interface

The optional Python interface runs the compiled Chronicle executable through
checked subprocesses. It accepts original text and a search/query, presents
source passages as plain text, and verifies the native integrity receipt before
showing a result. It does not generate neural text or validate arbitrary rules.

## Start

Build the native engine using the repository build instructions. Use a Python
environment with Gradio installed; the interface does not install dependencies
or download weights. The bridge and its tests use only the Python standard library.

```powershell
python tools/casper_local_app.py --binary build/casper-chronicle.exe --port 7864
```

On Linux, pass `--binary build/casper-chronicle`. Open
`http://127.0.0.1:7864`. The launcher binds to loopback, disables analytics, and
does not create a public share. If the requested port is occupied, launch on a
different port; the app does not stop an existing service.

Paths are resolved against the project containing the adapter, independently of
the current working directory. `--binary` and `--store` override the native
executable and evidence directory. Their environment equivalents are
`CASPER_CHRONICLE_BIN` and `CASPER_CHRONICLE_STORE`. These settings are not web
form inputs.

## Read a result

1. Replace the example with the original text to inspect.
2. Enter literal search terms, or select debt assessment and enter a supported
   question identifying the exact borrower/lender pair.
3. Run the query. The interface shows the status, debt and counted settlement
   when a debt was selected, and the exact source passages with line coordinates.
4. Expand the technical details to inspect complete native JSON and SHA-256
   values. Download the receipt or use the re-verification button.

Amounts retain exact integer/rational form. Missing debt is displayed as
unresolved, never as an established zero debt. The displayed passages include
relevant pending/conflicting evidence returned by the engine; merely appearing
in the evidence list does not mean a payment was counted. User source passages
are displayed in readonly text controls, not inserted as executable HTML.

The default example has a debt of 100 SAR, an acknowledged intermediary receipt
of 40 SAR, and a pending transfer of 60 SAR. Its debt assessment is `PARTIAL`.
See [the bounded Arabic grammar](CHRONICLE_ARABIC.md) and
[question selection](CHRONICLE_QUESTIONS.md) for accepted forms. Literal search
does not perform general semantic understanding. A `VALID` receipt establishes
file/result integrity, not the real-world truth of the source.

## Files and failures

Pasted text is encoded as UTF-8 without normalization. Source files, native
stores and receipts remain under:

```text
build/local-chronicle/<native-executable-sha256>/<document-sha256>/
```

The result includes both hashes. New executable bytes get a separate store
namespace so changing the store/parser version cannot overwrite an earlier
import. Existing evidence is preserved. The UI does not automatically migrate
old stores or attest to their compatibility with a new executable.

Missing or unloadable native binaries, malformed input, native nonzero exits,
timeouts, and rejected receipts produce an explicit error and `NOT_VERIFIED`.
They do not produce substitute text or successful rule-validation messages.
Queries are bounded to 1,024 UTF-8 bytes; source text is bounded to 8 MiB, with
the native engine's combined capacity limits still applying.

Importing `tools/casper_local_app.py` does not create a UI or launch a server.
`build_app()` constructs the interface; `main()` launches it. The old local
two-argument neural/rule mock is not part of this adapter.

## Adapter oracle

```powershell
python tests/test_local_bridge.py build/casper-chronicle.exe
```

The oracle uses the real native executable. It checks source bytes and hashes,
bounded Arabic debt results, rejection and error states, receipt tampering,
working-directory independence, unchanged repeated imports, backend upgrades
preserving earlier stores, readable amounts/evidence, and import safety.
It is separate from browser layout and keyboard verification.
