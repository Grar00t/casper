"""Dependency-free trainer contracts; optional PyTorch numeric regression."""
import ast
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
TRAINER = ROOT / 'tools/train_casper.py'
SELECTED = {'CasperTokenizer', 'verify_tokenizer_contract', 'ModelConfig', 'Record',
            'SFTDataset', 'dataset_sha256', 'load_records', 'split_records', 'validate_checkpoint_payload'}


def load_pure_contracts():
    tree = ast.parse(TRAINER.read_text(encoding='utf-8'))
    nodes = [node for node in tree.body if isinstance(node, ast.Assign) or
             isinstance(node, (ast.ClassDef, ast.FunctionDef)) and node.name in SELECTED]
    future = ast.parse('from __future__ import annotations').body
    module = types.ModuleType('casper_pure_contracts')
    module.__dict__.update(dataclass=dataclass, math=math, hashlib=hashlib, json=json,
                           Path=Path, Dataset=object)
    sys.modules[module.__name__] = module
    exec(compile(ast.Module(body=future + nodes, type_ignores=[]), str(TRAINER), 'exec'), module.__dict__)
    return module


m = load_pure_contracts()


class PureTrainerContracts(unittest.TestCase):
    def test_tokenizer_roundtrips(self):
        tok = m.CasperTokenizer()
        self.assertEqual(tok.vocab_size, 1756)
        m.verify_tokenizer_contract(tok)
        for text in ['THE data', 'بسم الله', '🧠 汉字', ''.join(chr(i) for i in range(1, 128))]:
            self.assertEqual(tok.decode(tok.encode(text)), text)

    def test_ascii_control_is_not_word_separator(self):
        tok = m.CasperTokenizer()
        for control in ['\x1c', '\x1d', '\x1e', '\x1f']:
            text = 'the' + control + 'data'
            self.assertEqual(tok.encode(text, bos=False, eos=False), [1500 + b for b in text.encode()])
        self.assertEqual(tok.encode('the data', bos=False, eos=False),
                         [tok.lookup_exact['the'], 1532, tok.lookup_exact['data']])

    def test_preserves_response_suffix_and_mask(self):
        tok = m.CasperTokenizer()
        for instruction in ['short', 'very long question ' * 100]:
            inputs, labels = m.SFTDataset([m.Record('1', instruction, 'بسم الله')], tok, 64)[0]
            first = next(i for i, value in enumerate(labels) if value != -100)
            prompt = tok.decode(inputs[:first + 1])
            self.assertTrue(prompt.startswith('Instruction:\n'))
            self.assertTrue(prompt.endswith('\nResponse:\n'))
            self.assertEqual(labels[-1], m.TOK_EOS)
            self.assertLessEqual(len(inputs), 64)
            self.assertEqual(len(inputs), len(labels))
            self.assertTrue(all(value == -100 for value in labels[:first]))

    def test_too_short_context_fails(self):
        with self.assertRaisesRegex(ValueError, 'ctx_len'):
            m.SFTDataset([m.Record('1', 'hello', 'world')], m.CasperTokenizer(), 4)

    def test_invalid_model_configuration(self):
        for field, value in [('ffn_mult', 0), ('rope_theta', float('nan')), ('rms_eps', -1)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                m.ModelConfig(vocab_size=1756, **{field: value}).validate()
        m.ModelConfig(vocab_size=1756).validate()

    def test_dataset_validation_and_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'records.jsonl'
            rows = [{'id': str(i), 'instruction': 'question', 'response': 'answer'} for i in range(3)]
            path.write_text(''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')
            records = m.load_records(path)
            self.assertEqual(m.dataset_sha256(path), hashlib.sha256(path.read_bytes()).hexdigest())
            train, validation = m.split_records(records, 0.1)
            self.assertFalse({r.rid for r in train} & {r.rid for r in validation})
            path.write_text(json.dumps(rows[0]) + '\n' + json.dumps(rows[0]), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                m.load_records(path)

    def test_checkpoint_validation(self):
        payload = {'format': 'CASPER-TRAIN-CHECKPOINT-V1', 'epoch': 1, 'best_val': 2.5,
                   'dataset_sha256': 'a' * 64, 'config': {}, 'model': {}, 'optimizer': {}}
        self.assertEqual(m.validate_checkpoint_payload(payload), payload)
        for key, value in [('epoch', True), ('best_val', float('nan')), ('dataset_sha256', 'bad')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                m.validate_checkpoint_payload({**payload, key: value})


@unittest.skipUnless(importlib.util.find_spec('torch'), 'PyTorch not installed; numerical check not run')
class TorchTrainerContracts(unittest.TestCase):
    def test_evaluation_is_token_weighted(self):
        import torch
        spec = importlib.util.spec_from_file_location('casper_numeric_trainer', TRAINER)
        trainer = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = trainer
        spec.loader.exec_module(trainer)
        torch.set_num_threads(1)
        torch.manual_seed(17)
        model = trainer.CasperModel(trainer.ModelConfig(vocab_size=32, ctx_len=8,
            embed_dim=8, n_layers=1, n_heads=2, n_kv_heads=1, ffn_mult=2))
        items = [([0, 4, 5], [-100, 5, 1]), ([0, 6, 7, 8, 9], [-100, 7, 8, 9, 1])]
        a = torch.utils.data.DataLoader(items, batch_size=1, collate_fn=trainer.collate)
        b = torch.utils.data.DataLoader(items, batch_size=2, collate_fn=trainer.collate)
        self.assertAlmostEqual(trainer.evaluate(model, a, torch.device('cpu')),
                               trainer.evaluate(model, b, torch.device('cpu')), places=5)


if __name__ == '__main__':
    unittest.main()
