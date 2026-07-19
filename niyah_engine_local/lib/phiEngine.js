'use strict';

/* ══════════════════════════════════════════════════════════════════
   phiEngine.js — Local LLM via llama-server (OpenAI-compatible API)
   
   Connects to llama-server running Phi-3.5-mini or Qwen2.5 locally.
   No external API keys. No cloud dependencies. Real inference.
   ══════════════════════════════════════════════════════════════════ */

const PHI_HOST = process.env.PHI_HOST || 'http://127.0.0.1:8080';
const PHI_TIMEOUT = parseInt(process.env.PHI_TIMEOUT) || 60000;

const SYSTEM_PROMPT = `You are NIYAH, a search and knowledge assistant. Rules:
- Answer in the SAME language the user writes in (Arabic → Arabic, English → English)
- Be concise, factual, and helpful
- If given search context, synthesize it into a clear answer with citation numbers [n]
- Never fabricate information. If unsure, say so.
- You are a tool, not a person. No fake emotions.`;

/**
 * Check if llama-server is alive
 * @returns {Promise<boolean>}
 */
async function phiHealthCheck() {
  try {
    const res = await fetch(`${PHI_HOST}/health`, {
      signal: AbortSignal.timeout(3000),
    });
    if (!res.ok) return false;
    const data = await res.json();
    return data.status === 'ok';
  } catch {
    return false;
  }
}

/**
 * Get model info from llama-server
 * @returns {Promise<{id:string,owned_by:string}|null>}
 */
async function phiModelInfo() {
  try {
    const res = await fetch(`${PHI_HOST}/v1/models`, {
      signal: AbortSignal.timeout(3000),
    });
    if (!res.ok) return null;
    const data = await res.json();
    const m = (data.data || [])[0];
    return m ? { id: m.id, owned_by: m.owned_by || 'local' } : null;
  } catch {
    return null;
  }
}

/**
 * Send a chat completion request to llama-server
 * @param {string} message - User message
 * @param {Array} history - Previous messages [{role,content}]
 * @param {object} opts - {systemPrompt, maxTokens, temperature}
 * @returns {Promise<{answer:string,model:string,tokens:object,tookMs:number}|null>}
 */
async function phiChat(message, history = [], opts = {}) {
  const start = Date.now();
  try {
    const messages = [
      { role: 'system', content: opts.systemPrompt || SYSTEM_PROMPT },
      ...history.slice(-6),
      { role: 'user', content: message },
    ];

    const res = await fetch(`${PHI_HOST}/v1/chat/completions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model: 'local',
        messages,
        max_tokens: opts.maxTokens || 512,
        temperature: opts.temperature ?? 0.7,
        top_p: 0.9,
        stop: opts.stop || undefined,
      }),
      signal: AbortSignal.timeout(PHI_TIMEOUT),
    });

    if (!res.ok) {
      const err = await res.text().catch(() => '');
      console.error(`[phi] HTTP ${res.status}: ${err.substring(0, 200)}`);
      return null;
    }

    const data = await res.json();
    const choice = (data.choices || [])[0];
    if (!choice) return null;

    const tookMs = Date.now() - start;
    console.log(`[phi] ${tookMs}ms · ${data.usage?.total_tokens || '?'} tokens · model=${data.model || 'unknown'}`);

    return {
      answer: choice.message?.content || '',
      model: data.model || 'local',
      tokens: data.usage || {},
      tookMs,
    };
  } catch (err) {
    console.error(`[phi] error: ${err.message}`);
    return null;
  }
}

/**
 * Given search context + query, ask the LLM to synthesize an answer
 * @param {string} query - Original user question
 * @param {string} searchAnswer - Extractive answer from NIYAH search
 * @param {Array} citations - [{n, title, url}]
 * @returns {Promise<string|null>} - Improved answer or null
 */
async function phiImproveAnswer(query, searchAnswer, citations = []) {
  const citText = citations.map(c => `[${c.n}] ${c.title}: ${c.url}`).join('\n');
  
  const prompt = `Based on the following search results, provide a clear and accurate answer to the question.

Question: ${query}

Search Results:
${searchAnswer}

${citText ? `Sources:\n${citText}` : ''}

Instructions:
- Synthesize the information into a clear, well-structured answer
- Keep citation numbers [n] when referencing information from sources
- Answer in the same language as the question
- Be concise but thorough
- Do not add information that is not in the search results`;

  const result = await phiChat(prompt, [], {
    maxTokens: 600,
    temperature: 0.3, // Lower temp for factual synthesis
  });

  return result?.answer || null;
}

module.exports = { phiChat, phiHealthCheck, phiModelInfo, phiImproveAnswer };
