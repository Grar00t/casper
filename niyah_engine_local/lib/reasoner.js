'use strict';

/* ══════════════════════════════════════════════════════════════════
   reasoner.js — Extractive synthesis with citation anchors
   Algorithm: score all candidate sentences → dedup → pick top N
              → group by source → attach [n] citation numbers
   No generation. No hallucination. Text comes from real web pages.
   ══════════════════════════════════════════════════════════════════ */

const { extractTopSentences, rankByRelevance, detectLang } = require('./relevance');

function synthesize(query, sourcesWithText, opts = {}) {
  const sentencesPerSource = opts.sentencesPerSource || 2;
  const maxSentencesTotal  = opts.maxSentencesTotal  || 5;
  const minScore           = opts.minScore           || 0.12; // higher = less noise

  const candidates = [];

  sourcesWithText.forEach((src, idx) => {
    const top = extractTopSentences(query, src.text, sentencesPerSource);
    for (const t of top) {
      if (t.relevanceScore >= minScore) {
        candidates.push({
          text: t.text, score: t.relevanceScore,
          sourceIndex: idx, sourceUrl: src.url, sourceTitle: src.title,
        });
      }
    }
  });

  if (candidates.length === 0) {
    const lang = detectLang(query);
    const noResult = lang === 'ar'
      ? 'لم يُعثر على نص ذي صلة كافية بالسؤال في المصادر المسترجعة. أعد صياغة السؤال.'
      : lang === 'zh'
      ? '在检索到的来源中未找到足够相关的文本。请尝试重新表述您的问题。'
      : 'No sufficiently relevant text found in fetched sources. Try rephrasing.';
    return { answer: noResult, citations: [], confidence: 0 };
  }

  /* Sort by score, pick top N ensuring source diversity */
  candidates.sort((a, b) => b.score - a.score);
  const selected = [];
  const perSource = new Map();

  for (const c of candidates) {
    if (selected.length >= maxSentencesTotal) break;
    const n = perSource.get(c.sourceIndex) || 0;
    if (n >= sentencesPerSource) continue;
    selected.push(c);
    perSource.set(c.sourceIndex, n + 1);
  }

  /* Build citation map */
  const citationMap = new Map();
  const citations   = [];
  for (const c of selected) {
    if (!citationMap.has(c.sourceIndex)) {
      citationMap.set(c.sourceIndex, citations.length + 1);
      citations.push({ n: citations.length + 1, title: c.sourceTitle, url: c.sourceUrl });
    }
  }

  const answerLines = selected.map(c => {
    const n = citationMap.get(c.sourceIndex);
    return `${c.text} [${n}]`;
  });

  /* Confidence = weighted avg score + diversity bonus */
  const avgScore   = selected.reduce((s, c) => s + c.score, 0) / selected.length;
  const diversity  = Math.min(citationMap.size / 3, 1); // saturates at 3 independent sources
  const confidence = Math.round(Math.min(avgScore * 0.7 + diversity * 0.3, 1) * 100) / 100;

  return { answer: answerLines.join('\n'), citations, confidence };
}

module.exports = { synthesize };
