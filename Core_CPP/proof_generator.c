/* proof_generator.c — SHA-256 integrity receipts. C11. */
#include "proof_generator.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static const uint32_t K[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

typedef struct { uint32_t state[8]; uint64_t bitcount; uint8_t buffer[64]; uint32_t buflen; } SHA256_CTX;

static void sha256_init(SHA256_CTX *c){
    c->state[0]=0x6a09e667;c->state[1]=0xbb67ae85;c->state[2]=0x3c6ef372;c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f;c->state[5]=0x9b05688c;c->state[6]=0x1f83d9ab;c->state[7]=0x5be0cd19;c->bitcount=0;c->buflen=0;}
static void sha256_transform(SHA256_CTX *c,const uint8_t b[64]){
    uint32_t W[64],a,bv,d,e,f,g,h,t1,t2,cc;
    for(int i=0;i<16;i++)W[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for(int i=16;i<64;i++)W[i]=SIG1(W[i-2])+W[i-7]+SIG0(W[i-15])+W[i-16];
    a=c->state[0];bv=c->state[1];cc=c->state[2];d=c->state[3];e=c->state[4];f=c->state[5];g=c->state[6];h=c->state[7];
    for(int i=0;i<64;i++){t1=h+EP1(e)+CH(e,f,g)+K[i]+W[i];t2=EP0(a)+MAJ(a,bv,cc);h=g;g=f;f=e;e=d+t1;d=cc;cc=bv;bv=a;a=t1+t2;}
    c->state[0]+=a;c->state[1]+=bv;c->state[2]+=cc;c->state[3]+=d;c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;}
static void sha256_update(SHA256_CTX *c,const uint8_t *data,size_t len){
    if(!data&&len)return;
    while(len){size_t n=64u-c->buflen;if(n>len)n=len;memcpy(c->buffer+c->buflen,data,n);c->buflen+=(uint32_t)n;data+=n;len-=n;if(c->buflen==64u){sha256_transform(c,c->buffer);c->bitcount+=512u;c->buflen=0;}}}
static void sha256_final(SHA256_CTX *c,uint8_t out[32]){
    c->bitcount+=(uint64_t)c->buflen*8u;c->buffer[c->buflen++]=0x80u;
    if(c->buflen>56u){while(c->buflen<64u)c->buffer[c->buflen++]=0;sha256_transform(c,c->buffer);c->buflen=0;}
    while(c->buflen<56u)c->buffer[c->buflen++]=0;
    for(int i=7;i>=0;i--)c->buffer[c->buflen++]=(uint8_t)(c->bitcount>>(i*8));
    sha256_transform(c,c->buffer);
    for(int i=0;i<8;i++){out[i*4]=(uint8_t)(c->state[i]>>24);out[i*4+1]=(uint8_t)(c->state[i]>>16);out[i*4+2]=(uint8_t)(c->state[i]>>8);out[i*4+3]=(uint8_t)c->state[i];}}

void niyah_sha256(const uint8_t *data,size_t len,uint8_t out[32]){SHA256_CTX c;sha256_init(&c);sha256_update(&c,data,len);sha256_final(&c,out);}
void niyah_hash_to_hex(const uint8_t h[32],char hex[65]){static const char x[]="0123456789abcdef";for(int i=0;i<32;i++){hex[i*2]=x[h[i]>>4];hex[i*2+1]=x[h[i]&15u];}hex[64]='\0';}
static bool hex_to_hash(const char *s,uint8_t h[32]){if(!s||strlen(s)!=64u)return false;for(int i=0;i<32;i++){char a=s[i*2],b=s[i*2+1];int hi=(a>='0'&&a<='9')?a-'0':(a>='a'&&a<='f')?a-'a'+10:(a>='A'&&a<='F')?a-'A'+10:-1;int lo=(b>='0'&&b<='9')?b-'0':(b>='a'&&b<='f')?b-'a'+10:(b>='A'&&b<='F')?b-'A'+10:-1;if(hi<0||lo<0)return false;h[i]=(uint8_t)((hi<<4)|lo);}return true;}

static void hash_text(const char *s,uint8_t out[32]){
    if(s)niyah_sha256((const uint8_t*)s,strlen(s),out);else niyah_sha256((const uint8_t*)"",0u,out);
}

bool niyah_sha256_file(const char *path,uint8_t out[32]){
    FILE *f;
    SHA256_CTX c;
    uint8_t buf[8192];
    size_t n;
    if(!path||!out)return false;
    f=fopen(path,"rb");
    if(!f)return false;
    sha256_init(&c);
    while((n=fread(buf,1u,sizeof(buf),f))>0u)sha256_update(&c,buf,n);
    if(ferror(f)){fclose(f);return false;}
    if(fclose(f)!=0)return false;
    sha256_final(&c,out);
    return true;
}

void niyah_proof_generate_hashed(const char *prompt,const char *output,const uint8_t rules_hash[32],uint8_t proof[32]){
    static const uint8_t domain[]="NIYAH-PROOF-V2";
    uint8_t prompt_hash[32],output_hash[32],empty_rules_hash[32];
    const uint8_t *rh=rules_hash;
    SHA256_CTX c;
    uint8_t sep=0u;
    hash_text(prompt,prompt_hash);
    hash_text(output,output_hash);
    if(!rh){hash_text(NULL,empty_rules_hash);rh=empty_rules_hash;}
    sha256_init(&c);
    sha256_update(&c,domain,sizeof(domain)-1u);
    sha256_update(&c,&sep,1u);
    sha256_update(&c,prompt_hash,sizeof(prompt_hash));
    sha256_update(&c,output_hash,sizeof(output_hash));
    sha256_update(&c,rh,32u);
    sha256_final(&c,proof);
}

void niyah_proof_generate(const char *prompt,const char *output,const char *rule_material,uint8_t proof[32]){
    uint8_t rules_hash[32];
    hash_text(rule_material,rules_hash);
    niyah_proof_generate_hashed(prompt,output,rules_hash,proof);
}

int niyah_proof_save_hashed(const char *path,const uint8_t proof[32],const char *prompt,const char *output,const uint8_t rules_hash[32]){
    FILE *f;
    char hex[65];
    uint8_t h[32],empty_rules_hash[32];
    const uint8_t *rh=rules_hash;
    int rc;
    if(!path||!proof)return -1;
    if(!rh){hash_text(NULL,empty_rules_hash);rh=empty_rules_hash;}
    f=fopen(path,"w");
    if(!f)return -1;
    fputs("NIYAH-PROOF-V2\n",f);
    niyah_hash_to_hex(proof,hex);fprintf(f,"hash: %s\n",hex);
    hash_text(prompt,h);niyah_hash_to_hex(h,hex);fprintf(f,"prompt_hash: %s\n",hex);
    hash_text(output,h);niyah_hash_to_hex(h,hex);fprintf(f,"output_hash: %s\n",hex);
    niyah_hash_to_hex(rh,hex);fprintf(f,"rules_hash: %s\n",hex);
    if(prompt)fprintf(f,"prompt: %s\n",prompt);
    if(output)fprintf(f,"output: %s\n",output);
    rc=ferror(f)?-1:0;
    if(fclose(f)!=0)rc=-1;
    return rc;
}

int niyah_proof_save(const char *path,const uint8_t proof[32],const char *prompt,const char *output,const char *rule_material){
    uint8_t rules_hash[32];
    hash_text(rule_material,rules_hash);
    return niyah_proof_save_hashed(path,proof,prompt,output,rules_hash);
}

bool niyah_proof_verify(const char *proof_path,const char *prompt,const char *output,const char *rule_material){
    FILE *f;
    char line[4096],header[32]={0},stored_hex[65]={0},prompt_hex[65]={0},output_hex[65]={0},rules_hex[65]={0};
    uint8_t stored[32],actual[32],actual_prompt[32],actual_output[32],actual_rules[32],meta[32];
    if(!proof_path)return false;
    f=fopen(proof_path,"r");
    if(!f)return false;
    if(fgets(header,sizeof(header),f))header[strcspn(header,"\r\n")]='\0';
    while(fgets(line,sizeof(line),f)){
        char *value=NULL;char *dst=NULL;
        if(!strncmp(line,"hash: ",6)){value=line+6;dst=stored_hex;}
        else if(!strncmp(line,"prompt_hash: ",13)){value=line+13;dst=prompt_hex;}
        else if(!strncmp(line,"output_hash: ",13)){value=line+13;dst=output_hex;}
        else if(!strncmp(line,"rules_hash: ",12)){value=line+12;dst=rules_hex;}
        if(value&&dst){size_t l=strcspn(value,"\r\n");if(l==64u){memcpy(dst,value,64u);dst[64]='\0';}}
    }
    fclose(f);
    if(strcmp(header,"NIYAH-PROOF-V2")!=0)return false;
    if(!hex_to_hash(stored_hex,stored)||!hex_to_hash(prompt_hex,meta))return false;
    hash_text(prompt,actual_prompt);if(memcmp(meta,actual_prompt,32u)!=0)return false;
    if(!hex_to_hash(output_hex,meta))return false;
    hash_text(output,actual_output);if(memcmp(meta,actual_output,32u)!=0)return false;
    if(!hex_to_hash(rules_hex,meta))return false;
    hash_text(rule_material,actual_rules);if(memcmp(meta,actual_rules,32u)!=0)return false;
    niyah_proof_generate_hashed(prompt,output,actual_rules,actual);
    return memcmp(stored,actual,32u)==0;
}

int niyah_proof_smoke(void){
    int fail=0;
    uint8_t h[32],h2[32],file_hash[32];
    char hex[65];
    const char *tmp="niyah_test.proof";
    const char *rules="rule: \"IF output CONTAINS 'bad' THEN output = REJECTED\"";
    niyah_sha256((const uint8_t*)"",0,h);niyah_hash_to_hex(h,hex);if(strncmp(hex,"e3b0c44298fc1c14",16))++fail;
    niyah_sha256((const uint8_t*)"abc",3,h);niyah_hash_to_hex(h,hex);if(strncmp(hex,"ba7816bf8f01cfea",16))++fail;
    niyah_proof_generate("hello","world",rules,h);niyah_hash_to_hex(h,hex);if(strlen(hex)!=64u)++fail;
    niyah_proof_generate("hello","world","different rules",h2);if(memcmp(h,h2,32u)==0)++fail;
    if(niyah_proof_save(tmp,h,"hello","world",rules)!=0)++fail;
    else{
        if(!niyah_proof_verify(tmp,"hello","world",rules))++fail;
        if(niyah_proof_verify(tmp,"hello","tampered",rules))++fail;
        if(niyah_proof_verify(tmp,"hello","world","different rules"))++fail;
        if(!niyah_sha256_file(tmp,file_hash))++fail;
        remove(tmp);
    }
    return fail;
}
