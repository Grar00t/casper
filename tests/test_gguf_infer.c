/* Standard C11 GGUF forward test with independent small-model golden vectors.
 * No external models, Python packages, downloads or GPU required. */
#include "niyah_core.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    const char *name;
    uint32_t nd, cols, rows, kind;
    uint64_t offset;
} Tensor;

enum { EMBED, HEAD, NORM, QMAT, KMAT, VMAT, OMAT, ZERO };
static Tensor fields[] = {
    {"token_embd.weight", 2, 4, 4, EMBED, 0},
    {"output.weight", 2, 4, 4, HEAD, 0},
    {"output_norm.weight", 1, 4, 1, NORM, 0},
    {"blk.0.attn_norm.weight", 1, 4, 1, NORM, 0},
    {"blk.0.ffn_norm.weight", 1, 4, 1, NORM, 0},
    {"blk.0.attn_q.weight", 2, 4, 4, QMAT, 0},
    {"blk.0.attn_k.weight", 2, 4, 2, KMAT, 0},
    {"blk.0.attn_v.weight", 2, 4, 2, VMAT, 0},
    {"blk.0.attn_output.weight", 2, 4, 4, OMAT, 0},
    {"blk.0.ffn_gate.weight", 2, 4, 8, ZERO, 0},
    {"blk.0.ffn_up.weight", 2, 4, 8, ZERO, 0},
    {"blk.0.ffn_down.weight", 2, 8, 4, ZERO, 0}
};
static int write8(FILE *f, uint8_t b) { return fputc((int)b, f) != EOF; }
static int write16(FILE *f, uint16_t v) {
    return write8(f, (uint8_t)v) && write8(f, (uint8_t)(v >> 8));
}
static int write32(FILE *f, uint32_t v) {
    return write8(f, (uint8_t)v) && write8(f,(uint8_t)(v>>8)) &&
           write8(f,(uint8_t)(v>>16)) && write8(f,(uint8_t)(v>>24));
}
static int write64(FILE *f, uint64_t v) {
    return write32(f,(uint32_t)v) && write32(f,(uint32_t)(v >> 32));
}
static int writestr(FILE *f, const char *s) {
    size_t n = strlen(s);
    return write64(f,(uint64_t)n) && fwrite(s,1u,n,f)==n;
}
static int meta_u32(FILE *f,const char *key,uint32_t v) {
    return writestr(f,key) && write32(f,4u) && write32(f,v);
}
static int meta_f32(FILE *f,const char *key,float v) {
    uint32_t bits;
    memcpy(&bits,&v,sizeof bits);
    return writestr(f,key) && write32(f,6u) && write32(f,bits);
}
static uint64_t align32(uint64_t n) { return (n+31u)&~UINT64_C(31); }
static float element(const Tensor *t, uint32_t row, uint32_t col) {
    if (t->kind == NORM) return 1.0f;
    if (t->kind == EMBED || t->kind == HEAD ||
        t->kind == QMAT || t->kind == OMAT)
        return row == col ? 1.0f : 0.0f;
    if (t->kind == KMAT || t->kind == VMAT)
        return row == col ? 1.0f : 0.0f;
    return 0.0f;
}
static int fixture(const char *path, int is_f16) {
    FILE *f = fopen(path,"wb");
    const uint64_t nt = sizeof fields / sizeof fields[0];
    uint64_t next_offset = 0u, base;
    int ok = 1;
    long at;
    if (!f) return 0;
    ok &= fwrite("GGUF",1u,4u,f)==4u;
    ok &= write32(f,3u) && write64(f,nt) && write64(f,11u);
    ok &= writestr(f,"general.architecture") && write32(f,8u) && writestr(f,"llama");
    ok &= meta_u32(f,"general.alignment",32u);
    ok &= meta_u32(f,"llama.block_count",1u);
    ok &= meta_u32(f,"llama.embedding_length",4u);
    ok &= meta_u32(f,"llama.attention.head_count",2u);
    ok &= meta_u32(f,"llama.attention.head_count_kv",1u);
    ok &= meta_u32(f,"llama.feed_forward_length",8u);
    ok &= meta_u32(f,"llama.context_length",4u);
    ok &= meta_u32(f,"llama.rope.dimension_count",2u);
    ok &= meta_f32(f,"llama.rope.freq_base",10000.0f);
    ok &= meta_f32(f,"llama.attention.layer_norm_rms_epsilon",1e-5f);
    for (size_t i=0u; i<nt; ++i) {
        fields[i].offset = next_offset;
        next_offset = align32(next_offset+
            (uint64_t)fields[i].cols*fields[i].rows*(is_f16?2u:4u));
        ok &= writestr(f,fields[i].name) && write32(f,fields[i].nd) &&
              write64(f,fields[i].cols);
        if (fields[i].nd==2u) ok &= write64(f,fields[i].rows);
        ok &= write32(f,is_f16?1u:0u) && write64(f,fields[i].offset);
    }
    at=ftell(f);
    if (at < 0) ok=0;
    base=align32((uint64_t)(at < 0 ? 0 : at));
    while (ok && (uint64_t)ftell(f)<base) ok &= write8(f,0u);
    for (size_t i=0u; i<nt && ok; ++i) {
        const Tensor *t=&fields[i];
        uint64_t target=base+t->offset;
        while (ok && (uint64_t)ftell(f)<target) ok &= write8(f,0u);
        if ((uint64_t)ftell(f)!=target) { ok=0; break; }
        for (uint32_t row=0; row<t->rows; ++row) {
            for (uint32_t col=0; col<t->cols; ++col) {
                float v=element(t,row,col);
                uint32_t bits;
                if (is_f16) ok &= write16(f,(uint16_t)(v==1.0f?0x3c00u:0u));
                else { memcpy(&bits,&v,sizeof bits); ok &= write32(f,bits); }
            }
        }
    }
    if (fclose(f)!=0) ok=0;
    return ok;
}
static int test_path(const char *path, const char *label) {
    NiyahGGUFRunner *m=NULL;
    float *out;
    int fail=0;
    float a=1.0f / sqrtf(13.0f/4.0f+1e-5f);
    if (niyah_gguf_runner_open(&m,path,4u)!=0 || !m) {
        (void)printf("%s=FAIL(load)\n",label);
        return 1;
    }
    if (niyah_gguf_runner_vocab_size(m)!=4u) ++fail;
    if (niyah_gguf_runner_step(m,1u,1u)!=NULL) ++fail;
    out=niyah_gguf_runner_step(m,0u,0u);
    if (!out || fabsf(out[0]-3.0f*a)>2e-4f ||
        fabsf(out[2]-2.0f*a)>2e-4f ||
        fabsf(out[1])>2e-4f || fabsf(out[3])>2e-4f) ++fail;
    out=niyah_gguf_runner_step(m,1u,1u);
    if (!out) ++fail;
    else {
        const float inv=sqrtf(2.0f);
        float s0=-4.0f*sinf(1.0f)/inv;
        float s1=4.0f/inv;
        float w0=expf(s0-s1)/(expf(s0-s1)+1.0f);
        float w1=1.0f-w0;
        float x0=2.0f*w0, x1=1.0f+2.0f*w1, x2=1.0f, x3=1.0f;
        float scale=1.0f / sqrtf((x0*x0+x1*x1+x2*x2+x3*x3)/4.0f+1e-5f);
        if (fabsf(out[0]-x0*scale)>4e-4f ||
            fabsf(out[1]-x1*scale)>4e-4f ||
            fabsf(out[2]-x2*scale)>4e-4f ||
            fabsf(out[3]-x3*scale)>4e-4f) ++fail;
    }
    if (niyah_gguf_runner_step(m,2u,3u)!=NULL) ++fail;
    niyah_gguf_runner_reset(m);
    out=niyah_gguf_runner_step(m,0u,0u);
    if (!out || fabsf(out[0]-3.0f*a)>2e-4f) ++fail;
    niyah_gguf_runner_close(m);
    (void)printf("%s=%s\n",label,fail?"FAIL":"PASS");
    return fail;
}
int main(void) {
    int fail=0;
    if (!fixture("build/release/gguf_f32_fixture.gguf",0)) return 2;
    if (!fixture("build/release/gguf_f16_fixture.gguf",1)) return 2;
    fail+=test_path("build/release/gguf_f32_fixture.gguf","GGUF_DENSE_F32");
    fail+=test_path("build/release/gguf_f16_fixture.gguf","GGUF_DENSE_F16");
    if (niyah_gguf_runner_open(NULL,"missing.gguf",1u)==0) ++fail;
    return fail?1:0;
}
