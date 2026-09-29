/*
 * niyah_core.h — NIYAH Inference Engine v3.0
 *
 * Zero external dependencies. C99 clean. C++17 compatible.
 * Targets: x86_64 (AVX2+FMA), aarch64 (NEON), scalar fallback.
 *
 * ABI version: 0x0005
 */
#ifndef NIYAH_CORE_H
#define NIYAH_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIYAH_MAGIC     UINT32_C(0x4E595148)   /* "NYQH" */
#define NIYAH_VER       UINT32_C(0x0005)
#define NIYAH_MAX_CTX   UINT32_C(8192)
#define NIYAH_MAX_VOCAB UINT32_C(131072)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t embed_dim;
    uint32_t n_heads;
    uint32_t n_kv_heads;    /* GQA: kv heads (≤ n_heads) */
    uint32_t n_layers;
    uint32_t ffn_mult;      /* ffn_hidden = embed_dim * ffn_mult */
    uint32_t vocab_size;
    uint32_t ctx_len;
    float    rope_theta;
    float    rms_eps;
    uint32_t flags;         /* reserved, zero */
    uint8_t  _pad[16];      /* pad to 64 bytes total */
} NiyahConfig;             /* sizeof must be 64 */

typedef struct {
    float *wq;          /* [embed × embed]           Q projection */
    float *wk;          /* [kv_dim × embed]          K projection */
    float *wv;          /* [kv_dim × embed]          V projection */
    float *wo;          /* [embed × embed]           O projection */
    float *w_gate;      /* [ffn_hidden × embed]      SwiGLU gate  */
    float *w_up;        /* [ffn_hidden × embed]      SwiGLU up    */
    float *w_down;      /* [embed × ffn_hidden]      FFN down     */
    float *rms_att;     /* [embed]                   pre-attn norm*/
    float *rms_ffn;     /* [embed]                   pre-ffn norm */
} NiyahLayer;

typedef struct {
    NiyahConfig  cfg;
    NiyahLayer  *layers;       /* array[n_layers] in pool */
    float *token_embed;        /* [vocab × embed]  */
    float *rms_final;          /* [embed]          */
    float *lm_head;            /* [vocab × embed]  */

    /*
     * KV cache — head-major layout:
     *   kv_k[layer][head][seq_pos][head_dim]
     *   kv_v[layer][head][seq_pos][head_dim]
     */
    float *kv_k;
    float *kv_v;
    float *scratch;
    float *_logits;
    void  *_pool;
    size_t _pool_bytes;
    uint32_t head_dim;
    uint32_t kv_dim;
    uint32_t ffn_dim;
} NiyahModel;

typedef struct {
    float   *m;
    float   *v;
    uint32_t step;
    float    lr;
    float    beta1;
    float    beta2;
    float    eps;
    float    wd;
    size_t   n_weights;
} NiyahAdam;

typedef struct {
    float temperature;
    float top_p;
    uint64_t seed;
} NiyahSampler;

NiyahModel *niyah_alloc(const NiyahConfig *cfg);
void        niyah_free (NiyahModel *m);
int  niyah_save(const NiyahModel *m, const char *path);
int  niyah_load(NiyahModel **out,    const char *path);
float *niyah_forward(NiyahModel *m, uint32_t token, uint32_t pos);
uint32_t niyah_sample(const float *logits, uint32_t vocab_size,
                      NiyahSampler *s);
float niyah_train_step(NiyahModel *m, NiyahAdam *opt,
                       const uint32_t *tokens, uint32_t n);
NiyahAdam *niyah_adam_alloc(const NiyahModel *m);
void       niyah_adam_free (NiyahAdam *opt);
const char *niyah_simd_name(void);
size_t      niyah_param_count(const NiyahModel *m);

typedef struct NiyahRuleKBTag NiyahRuleKBOpaque;

typedef struct {
    void        *rules;         /* NiyahRuleKB* or NULL for pure neural */
    uint32_t     max_retries;   /* re-sample attempts on violation (default 3) */
    bool         generate_proof;/* request an integrity receipt hash */
} NiyahHybridOpts;

/*
 * Generate text with optional deterministic text-rule verification.
 *
 * Returns malloc'd string (caller frees).
 * If proof_out is non-NULL it is always initialized to 32 zero bytes first.
 * A V2 receipt hash is emitted only when generate_proof is true and no parsed
 * rule KB is active, because this API does not carry the exact rule-file bytes
 * needed to bind a rules-backed receipt. For rules-backed receipts use the
 * CLI/audit path, which hashes the exact rule material. This fail-closed rule
 * avoids producing a receipt that appears to cover rules when it does not.
 */
char *niyah_hybrid_generate(NiyahModel *m, const char *prompt,
                            const NiyahHybridOpts *opts,
                            NiyahSampler *sampler,
                            uint8_t proof_out[32]);

#ifdef __cplusplus
}
#endif
#endif /* NIYAH_CORE_H */
