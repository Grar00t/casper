"""Local document evidence UI. Importing this module does not start a server."""

import argparse
import os
from pathlib import Path

if __package__:
    from .casper_local_bridge import BridgeError, ChronicleBridge
else:
    from casper_local_bridge import BridgeError, ChronicleBridge

TITLE = "Casper Chronicle | سجل الأدلة المحلي"
DESCRIPTION = """
ألصق النص الأصلي ثم ابحث عن عباراته أو افحص سجلات الدين والسداد المنظمة.
تظهر المقاطع مع أرقام السطور وبصماتها، ويُعاد فحص إيصال السلامة لكل نتيجة.
البحث حرفي؛ فحص الدين يقتصر على الصيغ الموثقة. لا توجد أوزان نموذج أو توليد عصبي هنا.
سلامة الإيصال لا تثبت صحة أحداث النص في الواقع.
"""
CSS = """
.gradio-container {font-family: system-ui, sans-serif !important; max-width: 1100px !important;}
.rtl, .rtl textarea, .rtl label {direction: rtl; text-align: right;}
button {min-height: 44px !important;}
.rtl textarea {font-family: system-ui, sans-serif !important;}
.gradio-container button.primary {background: #1d4ed8 !important; color: #fff !important; border-color: #1d4ed8 !important;}
.gradio-container button.primary:hover {background: #1e40af !important; border-color: #1e40af !important;}
.rtl label:has(input[type="radio"]) {min-height: 44px !important; align-items: center;}
.gradio-container textarea:not([readonly]) {min-height: 44px !important;}
.gradio-container footer, .gradio-container footer a, .gradio-container footer span {color: #475569 !important;}
.gradio-container footer a {min-height: 44px; display: inline-flex; align-items: center;}
.gradio-container :is(button, input, textarea, summary, a):focus-visible {outline: 2px solid #1d4ed8; outline-offset: 2px;}
"""


def _summary(result):
    status = result["result"]["status"]
    if result["operation"] == "find":
        count = len(result["result"].get("matches", []))
        return f"{status} — {count} مقاطع مطابقة. سلامة الإيصال: VALID."
    labels = {"PARTIAL": "سداد جزئي بحسب النص", "SUPPORTED": "السداد مغطى بحسب النص",
              "CONFLICT": "تعارض في الأدلة", "UNKNOWN": "الأدلة غير كافية للحسم"}
    return f"{status} — {labels.get(status, status)}. سلامة الإيصال: VALID."


def _rational(value):
    if not isinstance(value, dict) or not value.get("den"):
        return "غير محدد"
    return str(value["num"]) if value["den"] == 1 else f"{value['num']}/{value['den']}"


def _account_text(result):
    if "debt" not in result:
        return "البحث يعرض النص الأصلي دون حساب دين."
    debt = result["debt"]
    if not isinstance(debt, dict) or not debt.get("borrower") or not debt.get("lender"):
        return "لم يحدد المحرك ديناً واحداً صالحاً للتقييم."
    currency = debt.get("currency") or "عملة غير محددة"
    return (f"المدين: {debt['borrower']}\nالدائن: {debt['lender']}\n"
            f"الدين: {_rational(debt)} {currency}\n"
            f"السداد المحتسب: {_rational(result.get('settled'))} {currency}")


def present_result(payload):
    if payload.get("status") == "ERROR":
        return "", ""
    result = payload["result"]
    items = result.get("evidence", result.get("matches", []))
    passages = []
    for item in items:
        line = item.get("line_start", item.get("line"))
        header = f"السطر {line} · البايتات {item['byte_start']}–{item['byte_end']}"
        passages.append(header + "\n" + item["text"])
    return _account_text(result), "\n\n".join(passages) or "لا توجد مقاطع أدلة مطابقة."


def evaluate_document(bridge, document, question, mode):
    try:
        result = bridge.run_document(document, question, mode)
        return _summary(result), result, result["receipt_path"]
    except (BridgeError, OSError) as error:
        detail = str(error)
        return (f"ERROR — {detail}",
                {"status": "ERROR", "receipt_integrity": "NOT_VERIFIED", "error": detail},
                None)


def verify_receipt(bridge, receipt):
    if not receipt:
        return "NOT_VERIFIED — شغّل استعلاماً للحصول على إيصال أولاً."
    try:
        return f"{bridge.verify(receipt)} — أعيد فحص سلامة الملف والنتيجة."
    except (BridgeError, OSError) as error:
        return f"NOT_VERIFIED — {error}"


def _input_controls(gr):
    fixture = Path(__file__).resolve().parents[1] / "tests/fixtures/chronicle_ahmed_prose_ar.txt"
    example = fixture.read_text(encoding="utf-8") if fixture.is_file() else ""
    document = gr.Textbox(label="النص الأصلي (يمكن استبدال المثال)", value=example, lines=9, elem_classes="rtl")
    question = gr.Textbox(label="كلمات البحث أو سؤال الدين والسداد", value="أحمد خالد", elem_classes="rtl")
    mode = gr.Radio(choices=[("بحث في النص الأصلي", "find"),
                            ("فحص سجلات الدين والسداد", "query")],
                    value="find", label="العملية", elem_classes="rtl")
    submit = gr.Button("استخرج الأدلة", variant="primary")
    return document, question, mode, submit


def _presentation(gr):
    theme = gr.themes.Base(font=["system-ui", "sans-serif"],
                           font_mono=["Consolas", "monospace"])
    return {"css": CSS, "theme": theme}


def _result_controls(gr):
    summary = gr.Textbox(label="الحالة", lines=3, interactive=False, elem_classes="rtl")
    account = gr.Textbox(label="الحساب المستند إلى النص", lines=4, interactive=False, elem_classes="rtl")
    evidence = gr.Textbox(label="مقاطع الأدلة من النص الأصلي", lines=10, interactive=False, elem_classes="rtl")
    with gr.Accordion("التفاصيل التقنية والبصمات", open=False):
        result = gr.JSON(label="النتيجة الكاملة")
    receipt = gr.File(label="تنزيل إيصال السلامة", interactive=False)
    state = gr.State(None)
    verify = gr.Button("أعد التحقق من الإيصال")
    verified = gr.Textbox(label="إعادة التحقق", lines=2, interactive=False, elem_classes="rtl")
    return [summary, account, evidence, result, receipt, state, verified], verify


def build_app(bridge=None):
    import gradio as gr
    bridge = bridge or ChronicleBridge()
    options = _presentation(gr) if int(gr.__version__.split(".")[0]) < 6 else {}
    with gr.Blocks(title=TITLE, analytics_enabled=False, **options) as app:
        gr.Markdown("# " + TITLE, elem_classes="rtl")
        gr.Markdown(DESCRIPTION, elem_classes="rtl")
        document, question, mode, submit = _input_controls(gr)
        outputs, verify = _result_controls(gr)
        def evaluate(document, question, mode):
            summary, result, receipt = evaluate_document(bridge, document, question, mode)
            account, evidence = present_result(result)
            return summary, account, evidence, result, receipt, receipt, ""
        submit.click(evaluate, [document, question, mode], outputs)
        verify.click(lambda receipt: verify_receipt(bridge, receipt), [outputs[-2]], [outputs[-1]])
    return app


def main():
    parser = argparse.ArgumentParser(description="Local Chronicle evidence browser")
    parser.add_argument("--binary", help="Path to the native Chronicle executable")
    parser.add_argument("--store", help="Directory for local source files, stores and receipts")
    parser.add_argument("--port", type=int, default=7860)
    args = parser.parse_args()
    os.environ["GRADIO_ANALYTICS_ENABLED"] = "False"
    import gradio as gr
    options = _presentation(gr) if int(gr.__version__.split(".")[0]) >= 6 else {}
    bridge = ChronicleBridge(binary=args.binary, store_root=args.store)
    build_app(bridge).launch(server_name="127.0.0.1", server_port=args.port,
                             share=False, inbrowser=False, show_error=True,
                             allowed_paths=[str(bridge.store_root)], **options)


if __name__ == "__main__":
    main()
