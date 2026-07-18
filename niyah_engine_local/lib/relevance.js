'use strict';

const STOPWORDS = new Set([
  'the', 'a', 'an', 'is', 'are', 'was', 'were', 'of', 'to', 'in', 'on', 'for',
  'and', 'or', 'but', 'with', 'this', 'that', 'it', 'as', 'be', 'by', 'at',
  'من', 'الى', 'إلى', 'على', 'في', 'عن', 'مع', 'هذا', 'هذه', 'ذلك', 'التي',
  'الذي', 'كان', 'هو', 'هي', 'ما', 'لا', 'ان', 'إن', 'او', 'أو', 'ثم', 'قد',
]);

function tokenize(text) {
  if (!text) return [];
  return text
    .toLowerCase()
    .normalize('NFKC')
    .replace(/[\u064B-\u065F\u0670]/g, '')
    .match(/[\p{L}\p{N}]+/gu) || [];
}

function termFrequencies(tokens) {
  const tf = new Map();
  for (const raw of tokens) {
    if (STOPWORDS.has(raw) || raw.length < 2) continue;
    tf.set(raw, (tf.get(raw) || 0) + 1);
  }
  return tf;
}

function cosineSim(tfA, tfB) {
  let dot = 0, magA = 0, magB = 0;
  for (const [term, freqA] of tfA) {
    magA += freqA * freqA;
    if (tfB.has(term)) dot += freqA * tfB.get(term);
  }
  for (const freqB of tfB.values()) magB += freqB * freqB;
  if (magA === 0 || magB === 0) return 0;
  return dot / (Math.sqrt(magA) * Math.sqrt(magB));
}

function rankByRelevance(query, items, textExtractor) {
  const queryTf = termFrequencies(tokenize(query));
  return items
    .map((item) => {
      const itemTf = termFrequencies(tokenize(textExtractor(item)));
      const score = cosineSim(queryTf, itemTf);
      return { ...item, relevanceScore: score };
    })
    .sort((a, b) => b.relevanceScore - a.relevanceScore);
}

function splitSentences(text) {
  if (!text) return [];
  return text
    .split(/(?<=[.!?؟。])\s+|\n+/)
    .map((s) => s.trim())
    .filter((s) => s.length > 15);
}

function extractTopSentences(query, longText, n = 3) {
  const sentences = splitSentences(longText);
  if (sentences.length === 0) return [];
  const ranked = rankByRelevance(query, sentences.map((s) => ({ text: s })), (i) => i.text);
  return ranked.slice(0, n).filter((r) => r.relevanceScore > 0);
}

module.exports = { tokenize, termFrequencies, cosineSim, rankByRelevance, splitSentences, extractTopSentences };
