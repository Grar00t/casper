'use strict';

const path = require('path');
const fs = require('fs');
const { tokenize, termFrequencies, cosineSim } = require('./relevance');

let DatabaseSync = null;
try {
  ({ DatabaseSync } = require('node:sqlite'));
} catch (_) {
  DatabaseSync = null;
}

class MemoryStore {
  constructor(dbPath = path.join(process.cwd(), 'niyah_memory.db')) {
    this.dbPath = dbPath;
    this.useSqlite = Boolean(DatabaseSync);
    if (this.useSqlite) {
      this._initSqlite();
    } else {
      this._initJsonFallback();
    }
  }

  _initSqlite() {
    this.db = new DatabaseSync(this.dbPath);
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS memories (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        query TEXT NOT NULL,
        answer TEXT NOT NULL,
        sources_json TEXT NOT NULL,
        confidence REAL NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_memories_created_at ON memories(created_at);
    `);
  }

  _initJsonFallback() {
    this.jsonPath = this.dbPath.replace(/\.db$/, '') + '.json';
    if (!fs.existsSync(this.jsonPath)) {
      fs.writeFileSync(this.jsonPath, JSON.stringify([]), 'utf-8');
    }
  }

  _readJson() {
    return JSON.parse(fs.readFileSync(this.jsonPath, 'utf-8'));
  }

  _writeJson(rows) {
    fs.writeFileSync(this.jsonPath, JSON.stringify(rows, null, 2), 'utf-8');
  }

  store(query, answer, sources, confidence) {
    const createdAt = Date.now();
    if (this.useSqlite) {
      const stmt = this.db.prepare(
        `INSERT INTO memories (query, answer, sources_json, confidence, created_at) VALUES (?, ?, ?, ?, ?)`
      );
      stmt.run(query, answer, JSON.stringify(sources), confidence, createdAt);
    } else {
      const rows = this._readJson();
      rows.push({ id: rows.length + 1, query, answer, sources_json: JSON.stringify(sources), confidence, created_at: createdAt });
      this._writeJson(rows);
    }
  }

  _allRows() {
    if (this.useSqlite) {
      return this.db.prepare('SELECT * FROM memories ORDER BY created_at DESC').all();
    }
    return this._readJson().sort((a, b) => b.created_at - a.created_at);
  }

  recall(query, similarityThreshold = 0.55, maxAgeMs = 7 * 24 * 60 * 60 * 1000) {
    const rows = this._allRows();
    const now = Date.now();
    const queryTf = termFrequencies(tokenize(query));
    let best = null, bestScore = 0;
    for (const row of rows) {
      if (now - row.created_at > maxAgeMs) continue;
      const rowTf = termFrequencies(tokenize(row.query));
      const score = cosineSim(queryTf, rowTf);
      if (score > bestScore) { bestScore = score; best = row; }
    }
    if (best && bestScore >= similarityThreshold) {
      return {
        query: best.query, answer: best.answer,
        sources: JSON.parse(best.sources_json),
        confidence: best.confidence,
        similarity: bestScore, ageMs: now - best.created_at,
      };
    }
    return null;
  }

  recentContext(n = 5) {
    return this._allRows().slice(0, n).map((r) => ({ query: r.query, answer: r.answer }));
  }

  close() {
    if (this.useSqlite && this.db) this.db.close();
  }
}

module.exports = { MemoryStore };
