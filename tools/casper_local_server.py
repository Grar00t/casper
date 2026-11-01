#!/usr/bin/env python3
"""Single-user loopback UI for the native Chronicle bridge; standard library only."""
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path

if __package__:
    from .casper_local_bridge import BridgeError, ChronicleBridge
else:
    from casper_local_bridge import BridgeError, ChronicleBridge

MAX_BODY = 52 * 1024 * 1024
PAGE = """<!doctype html><html lang="ar" dir="rtl"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>كاسبر المحلي</title>
<style>body{font:18px system-ui;max-width:900px;margin:auto;padding:24px;background:#101827;color:#edf2ff}textarea,input,select,button{font:inherit;box-sizing:border-box;width:100%;padding:12px;margin:8px 0;background:#20304a;color:inherit;border:1px solid #a0b6db}textarea{min-height:230px}label{display:block;margin-top:16px}pre{white-space:pre-wrap;overflow-wrap:anywhere}a{color:#9fceff}:focus-visible{outline:3px solid #ffd479}</style>
<h1>كاسبر · الأدلة المحلية</h1>
<p>بحث حرفي وفحص صيغ الدين المدعومة عبر محرك C. ليست دردشة عامة. الإيصال يثبت سلامة البيانات، لا صدق أحداث النص.</p>
<p>تُحفظ النصوص والإيصالات على جهاز تشغيل الخادم في مجلد التخزين المحلي.</p>
<label for="document">النص الأصلي</label><textarea id="document"></textarea>
<label for="question">كلمات البحث أو سؤال الدين المدعوم</label><input id="question">
<label for="mode">العملية</label><select id="mode"><option value="find">بحث حرفي</option><option value="query">فحص الدين</option></select>
<button id="run">افحص النص</button><p id="status" role="status" aria-live="polite">جاهز</p>
<a id="receipt" hidden download="casper.receipt">تنزيل إيصال السلامة</a>
<h2>النتيجة والأدلة الأصلية</h2><pre id="result"></pre><script src="/app.js"></script></html>"""
SCRIPT = """'use strict';
const el = id => document.getElementById(id);
let receiptUrl;
el('run').onclick = async () => {
  el('run').disabled = true;
  el('status').textContent = 'جارٍ الفحص والتحقق من الإيصال...';
  el('result').textContent = ''; el('receipt').hidden = true;
  if (receiptUrl) URL.revokeObjectURL(receiptUrl);
  try {
    const response = await fetch('/api/evaluate', {method:'POST',
      headers:{'Content-Type':'application/json'}, body:JSON.stringify({
        document:el('document').value, question:el('question').value, mode:el('mode').value})});
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || 'فشل الفحص');
    el('status').textContent = data.result.status + ' — سلامة الإيصال: ' + data.receipt_integrity;
    const receipt = data.receipt_text; delete data.receipt_text;
    el('result').textContent = JSON.stringify(data, null, 2);
    receiptUrl = URL.createObjectURL(new Blob([receipt], {type:'text/plain;charset=utf-8'}));
    el('receipt').href = receiptUrl; el('receipt').hidden = false;
  } catch (error) { el('status').textContent = 'ERROR — ' + error.message; }
  finally { el('run').disabled = false; }
};"""


def make_server(bridge, port=8765):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def trusted(self):
            hosts = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
            origins = {'http://' + host for host in hosts}
            return (len(self.headers.get_all('Host', [])) == 1 and self.headers.get('Host') in hosts
                    and len(self.headers.get_all('Origin', [])) <= 1
                    and (self.headers.get('Origin') is None or self.headers.get('Origin') in origins))

        def send(self, code, data, mime='application/json; charset=utf-8'):
            body = (json.dumps(data, ensure_ascii=False) if isinstance(data, dict) else data).encode('utf-8')
            self.send_response(code)
            for key, value in {'Content-Type': mime, 'Content-Length': str(len(body)),
                'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff',
                'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"}.items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if not self.trusted():
                return self.send(403, {'error': 'local origin required'})
            if self.path == '/':
                return self.send(200, PAGE, 'text/html; charset=utf-8')
            if self.path == '/app.js':
                return self.send(200, SCRIPT, 'application/javascript; charset=utf-8')
            return self.send(404, {'error': 'not found'})

        def do_POST(self):
            if not self.trusted():
                return self.send(403, {'error': 'local origin required'})
            if self.path != '/api/evaluate':
                return self.send(404, {'error': 'not found'})
            if self.headers.get('Content-Type', '').split(';')[0].strip().lower() != 'application/json':
                return self.send(415, {'error': 'application/json required'})
            lengths = self.headers.get_all('Content-Length', [])
            if self.headers.get('Transfer-Encoding') or len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
                return self.send(400, {'error': 'one numeric Content-Length required'})
            if len(lengths[0]) > 9:
                return self.send(413, {'error': 'request too large'})
            length = int(lengths[0])
            if not 0 < length <= MAX_BODY:
                return self.send(413, {'error': 'request size outside allowed range'})
            try:
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise ValueError('incomplete request')
                data = json.loads(raw.decode('utf-8', errors='strict'))
                if not isinstance(data, dict):
                    raise ValueError('request must be a JSON object')
                mode = data.get('mode', 'find')
                if not isinstance(mode, str) or mode not in ('find', 'query'):
                    raise ValueError('mode must be find or query')
                result = bridge.run_document(data.get('document'), data.get('question'), mode)
                receipt = Path(result['receipt_path']).resolve()
                receipt.relative_to(bridge.store_root.resolve())
                if result.get('receipt_integrity') != 'VALID':
                    raise BridgeError('receipt was not verified')
                text = receipt.read_text(encoding='utf-8')
                return self.send(200, {**result, 'receipt_text': text})
            except (ValueError, UnicodeError, BridgeError, OSError) as error:
                return self.send(400, {'status': 'ERROR', 'receipt_integrity': 'NOT_VERIFIED', 'error': str(error)})

    class LocalServer(HTTPServer):
        def get_request(self):
            connection, address = super().get_request()
            connection.settimeout(35)
            return connection, address

    return LocalServer(('127.0.0.1', port), Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--binary')
    parser.add_argument('--store-root')
    args = parser.parse_args()
    bridge = ChronicleBridge(args.binary, args.store_root)
    bridge._require_backend()
    with make_server(bridge, args.port) as httpd:
        print(f'Casper local UI: http://127.0.0.1:{httpd.server_port}', flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
