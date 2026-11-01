'use strict';

// Optional llama-server adapter. No probes or requests without explicit opt-in.
const EXTERNAL_LLM_ENABLED = process.env.CASPER_ENABLE_EXTERNAL_LLM === '1';
const PHI_HOST = process.env.PHI_HOST || 'http://127.0.0.1:8080';
const configuredTimeout = Number.parseInt(process.env.PHI_TIMEOUT, 10);
const PHI_TIMEOUT = Number.isFinite(configuredTimeout) && configuredTimeout > 0 ? configuredTimeout : 60000;
const SYSTEM_PROMPT = `You are NIYAH, a search and knowledge assistant.
Answer in the user's language. Be factual and concise. Preserve source citation
numbers when given search context. Never fabricate information; state uncertainty.`;

async function phiHealthCheck() {
  if (!EXTERNAL_LLM_ENABLED) return false;
  try {
    const res = await fetch(`${PHI_HOST}/health`, { signal: AbortSignal.timeout(3000) });
    return res.ok && (await res.json()).status === 'ok';
  } catch (_) { return false; }
}

async function phiModelInfo() {
  if (!EXTERNAL_LLM_ENABLED) return null;
  try {
    const res = await fetch(`${PHI_HOST}/v1/models`, { signal: AbortSignal.timeout(3000) });
    if (!res.ok) return null;
    const model = ((await res.json()).data || [])[0];
    return model ? { id: model.id, owned_by: model.owned_by || 'local' } : null;
  } catch (_) { return null; }
}

async function phiChat(message, history = [], opts = {}) {
  if (!EXTERNAL_LLM_ENABLED) return null;
  if (typeof message !== 'string' || !Array.isArray(history)) return null;
  const start = Date.now();
  try {
    const res = await fetch(`${PHI_HOST}/v1/chat/completions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model: 'local',
        messages: [{ role: 'system', content: opts.systemPrompt || SYSTEM_PROMPT },
          ...history.slice(-6), { role: 'user', content: message }],
        max_tokens: opts.maxTokens || 512,
        temperature: opts.temperature ?? 0.7,
        top_p: 0.9,
        stop: opts.stop || undefined,
      }),
      signal: AbortSignal.timeout(PHI_TIMEOUT),
    });
    if (!res.ok) return null;
    const data = await res.json();
    const choice = (data.choices || [])[0];
    if (!choice) return null;
    return { answer: choice.message?.content || '', model: data.model || 'local',
      tokens: data.usage || {}, tookMs: Date.now() - start };
  } catch (_) { return null; }
}

async function phiImproveAnswer(query, searchAnswer, citations = []) {
  if (!EXTERNAL_LLM_ENABLED) return null;
  const sources = citations.map(c => `[${c.n}] ${c.title}: ${c.url}`).join('\n');
  const prompt = `Answer the question using only these search results.
Question: ${query}
Search results: ${searchAnswer}
Sources: ${sources}
Preserve citation numbers. Use the question's language. State missing evidence.`;
  const result = await phiChat(prompt, [], { maxTokens: 600, temperature: 0.3 });
  return result?.answer || null;
}

module.exports = { phiChat, phiHealthCheck, phiModelInfo, phiImproveAnswer, EXTERNAL_LLM_ENABLED };
