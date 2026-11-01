"""Real HTTP tests with an explicit fake bridge; these do not execute native C."""
import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.casper_local_bridge import BridgeError
from tools.casper_local_server import make_server


class FakeBridge:
    def __init__(self, root):
        self.store_root = Path(root)
        self.receipt = self.store_root / 'test.receipt'
        self.receipt.write_text('TEST RECEIPT ONLY', encoding='utf-8')

    def run_document(self, document, question, mode):
        if not isinstance(document, str) or not document.strip() or not isinstance(question, str) or not question.strip():
            raise BridgeError('invalid input')
        if document == 'backend-error':
            raise BridgeError('test backend failure')
        return {'result': {'status': 'TEST_ONLY', 'evidence': document},
                'receipt_integrity': 'INVALID' if document == 'unverified' else 'VALID',
                'receipt_path': str(self.store_root.parent / 'outside.receipt') if document == 'outside' else str(self.receipt)}


class LocalHttpContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.server = make_server(FakeBridge(cls.folder.name), 0)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.folder.cleanup()

    def request(self, method='GET', path='/', data=None, headers=None, raw=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=5)
        try:
            body = raw if raw is not None else json.dumps(data).encode() if data is not None else None
            connection.request(method, path, body, headers or {'Content-Type': 'application/json'})
            response = connection.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            connection.close()

    def test_page_and_script(self):
        code, page, headers = self.request()
        self.assertEqual(code, 200)
        self.assertIn('كاسبر'.encode(), page)
        self.assertIn("frame-ancestors 'none'", headers['Content-Security-Policy'])
        self.assertEqual(headers['Cache-Control'], 'no-store')
        code, script, _ = self.request(path='/app.js')
        self.assertEqual(code, 200)
        self.assertNotIn(b'innerHTML', script)
        self.assertIn(b'textContent', script)

    def test_fake_result_and_receipt(self):
        code, body, _ = self.request('POST', '/api/evaluate', {'document': '<script>text</script>', 'question': 'text'})
        self.assertEqual(code, 200)
        data = json.loads(body)
        self.assertEqual(data['result']['status'], 'TEST_ONLY')
        self.assertEqual(data['receipt_text'], 'TEST RECEIPT ONLY')

    def test_foreign_origins_and_hosts(self):
        for header in [{'Origin': 'https://example.com'}, {'Origin': 'null'}, {'Host': 'attacker.test'}]:
            self.assertEqual(self.request(headers=header)[0], 403)

    def test_errors_fail_closed(self):
        for data in [{}, [], {'document': 'backend-error', 'question': 'q'},
                     {'document': 'unverified', 'question': 'q'}, {'document': 'outside', 'question': 'q'},
                     {'document': 'text', 'question': 'q', 'mode': {}}]:
            code, body, _ = self.request('POST', '/api/evaluate', data)
            self.assertEqual(code, 400)
            self.assertEqual(json.loads(body)['receipt_integrity'], 'NOT_VERIFIED')

    def test_invalid_json_and_content_type(self):
        self.assertEqual(self.request('POST', '/api/evaluate', raw=b'{broken')[0], 400)
        self.assertEqual(self.request('POST', '/api/evaluate', {}, {'Content-Type': 'text/plain'})[0], 415)

    def test_oversize_and_paths(self):
        code, _, _ = self.request('POST', '/api/evaluate', raw=b'',
            headers={'Content-Type': 'application/json', 'Content-Length': '99999999999999999999'})
        self.assertEqual(code, 413)
        self.assertEqual(self.request(path='/../../.git/config')[0], 404)
        self.assertEqual(self.request('POST', '/unknown', {})[0], 404)


if __name__ == '__main__':
    unittest.main()
