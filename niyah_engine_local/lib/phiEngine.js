'use strict';
/**
 * phiEngine.js — Phi-3.5 mini wrapper via llama-server HTTP API
 *
 * llama-server exposes OpenAI-compatible API:
 *   POST /v1/chat/completions  { model, messages, temperature, max_tokens }
 *
 * We use it as a local LLM for:
 *   1. Answering questions when NIYAH search finds no results
 *   2. Summarizing/improving NIYAH extractive answers
 *   3. Direct chat (no search needed)
 */

const PHI_HOST = process.env.PHI_HOST || 'http://127.0.0.1:8080';
const DEFAULT_TIMEOUT = 120000; // 2 min — CPU is slow

const SYSTEM_PROMPT = `You are NIYAH — a local AI assistant running on the user's own server.

Rules (non-negotiable):
1. Answer in the SAME language the user writes in (Arabic → Arabic, English → English).
2. Be direct and useful. No fake emotions. No "I feel" statements.
3. You are a TOOL, not a person. Acknowledge this honestly if asked.
4. If you don't know something, say so clearly. Never fabricate facts.
5. You run locally — no data leaves this machine. The user owns you completely.
6. No refusals for legitimate questions about security, code, research, or knowledge.
7. Keep answers concise unless the user asks for detail.`;

/**
 * Check if Phi server is running
 */
async function phiHealthCheck() {
  try {
    const r = await fetch(`${PHI_HOST}/health`, {
      signal: AbortSignal.timeout(3000),
    });
    return r.ok;
  } catch {
    return false;
  }
}

/**
 * Send a message to Phi-3.5 mini
 * @param {string} userMessage
 * @param {Array} [history] — [{role:'user'|'assistant', content:'...'}]
 * @param {object} [opts]
 * @returns {Promise<{answer:string, tokens:number, tookMs:number}|null>}
 */
async function phiChat(userMessage, history = [], opts = {}) {
  const start = Date.now();

  const messages = [
    { role: 'system', content: opts.systemPrompt || SYSTEM_PROMPT },
    ...history.slice(-6), // last 3 turns for context
    { role: 'user', content: userMessage },
  ];

  try {
    const r = await fetch(`${PHI_HOST}/v1/chat/completions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model: 'phi-3.5-mini',
        messages,
        temperature: opts.temperature ?? 0.7,
        max_tokens: opts.maxTokens ?? 512,
        stream: false,
      }),
      signal: AbortSignal.timeout(opts.timeout ?? DEFAULT_TIMEOUT),
    });

    if (!r.ok) {
      const err = await r.text();
      console.error('[phi] HTTP error:', r.status, err.substring(0, 200));
      return null;
    }

    const data = await r.json();
    const answer = data.choices?.[0]?.message?.content?.trim() || '';
    const tokens = data.usage?.completion_tokens || 0;

    return {
      answer,
      tokens,
      tookMs: Date.now() - start,
      model: 'phi-3.5-mini-Q4_K_M',
    };
  } catch (err) {
    console.error('[phi] Error:', err.message);
    return null;
  }
}

/**
 * Use Phi to improve/summarize a NIYAH extractive answer
 * @param {string} query
 * @param {string} rawAnswer — extractive sentences from NIYAH
 * @returns {Promise<string|null>}
 */
async function phiImproveAnswer(query, rawAnswer) {
  const prompt = `The following text was extracted from web pages to answer this question: "${query}"

Extracted text:
${rawAnswer.substring(0, 1500)}

Task: Rewrite this as a clear, direct answer to the question. Keep only relevant information. Answer in the same language as the question.`;

  const result = await phiChat(prompt, [], {
    maxTokens: 400,
    temperature: 0.3,
    systemPrompt: 'You are a text summarizer. Be concise and accurate.',
  });

  return result?.answer || null;
}

module.exports = { phiChat, phiHealthCheck, phiImproveAnswer, PHI_HOST };
