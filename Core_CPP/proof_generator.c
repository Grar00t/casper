/* proof_generator.c — SHA-256 integrity receipts. C11. */
#include "proof_generator.h"
#include "utf8_file.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define NIYAH_PROOF_MAX_FILE_BYTES (2u * 1024u * 1024u)
#define NIYAH_PROOF_MAX_LINE_BYTES (1024u * 1024u)
#define NIYAH_PROOF_MAX_HEX_BYTES  (1024u * 1024u)

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
    f=niyah_fopen_utf8(path,"rb");
    if(!f)return false;
    sha256_init(&c);
    for(;;){
        n=fread(buf,1u,sizeof(buf),f);
        if(n>0u)sha256_update(&c,buf,n);
        if(ferror(f)){(void)fclose(f);return false;}
        if(n<sizeof(buf)){
            if(!feof(f)){(void)fclose(f);return false;}
            break;
        }
    }
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

static int write_hex_text(FILE *f,const char *label,const char *text){
    static const char x[]="0123456789abcdef";
    const unsigned char *p=(const unsigned char *)(text?text:"");
    if(fputs(label,f)==EOF)return -1;
    while(*p){
        if(fputc(x[*p>>4],f)==EOF||fputc(x[*p&15u],f)==EOF)return -1;
        ++p;
    }
    return fputc('\n',f)==EOF?-1:0;
}

int niyah_proof_save_hashed(const char *path,const uint8_t proof[32],const char *prompt,const char *output,const uint8_t rules_hash[32]){
    FILE *f;
    char hex[65];
    uint8_t h[32],empty_rules_hash[32];
    const uint8_t *rh=rules_hash;
    int rc=0;
    if(!path||!proof)return -1;
    if(!rh){hash_text(NULL,empty_rules_hash);rh=empty_rules_hash;}
    f=fopen(path,"w");
    if(!f)return -1;
    if(fputs("NIYAH-PROOF-V2\n",f)==EOF)rc=-1;
    niyah_hash_to_hex(proof,hex);if(fprintf(f,"hash: %s\n",hex)<0)rc=-1;
    hash_text(prompt,h);niyah_hash_to_hex(h,hex);if(fprintf(f,"prompt_hash: %s\n",hex)<0)rc=-1;
    hash_text(output,h);niyah_hash_to_hex(h,hex);if(fprintf(f,"output_hash: %s\n",hex)<0)rc=-1;
    niyah_hash_to_hex(rh,hex);if(fprintf(f,"rules_hash: %s\n",hex)<0)rc=-1;
    if(write_hex_text(f,"prompt_hex: ",prompt)!=0)rc=-1;
    if(write_hex_text(f,"output_hex: ",output)!=0)rc=-1;
    if(ferror(f))rc=-1;
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
    if(!fgets(header,sizeof(header),f)){(void)fclose(f);return false;}
    header[strcspn(header,"\r\n")]='\0';
    while(!feof(f)&&!ferror(f)&&fgets(line,sizeof(line),f)){
        char *value=NULL;char *dst=NULL;
        if(!strncmp(line,"hash: ",6)){value=line+6;dst=stored_hex;}
        else if(!strncmp(line,"prompt_hash: ",13)){value=line+13;dst=prompt_hex;}
        else if(!strncmp(line,"output_hash: ",13)){value=line+13;dst=output_hex;}
        else if(!strncmp(line,"rules_hash: ",12)){value=line+12;dst=rules_hex;}
        if(value&&dst){size_t l=strcspn(value,"\r\n");if(l==64u){memcpy(dst,value,64u);dst[64]='\0';}}
    }
    if(ferror(f)){(void)fclose(f);return false;}
    if(fclose(f)!=0)return false;
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

static char *read_line_alloc(FILE *f,size_t max_len){
    size_t cap=256u,len=0u;
    char *buf;
    int ch=EOF;
    if(!f||max_len==0u)return NULL;
    if(cap>max_len+1u)cap=max_len+1u;
    buf=(char*)malloc(cap);
    if(!buf)return NULL;
    while(!feof(f)&&!ferror(f)){
        ch=fgetc(f);
        if(ch==EOF)break;
        if(ch=='\n')break;
        if(ch=='\r')continue;
        if(len>=max_len){free(buf);return NULL;}
        if(len+1u>=cap){
            size_t next=cap*2u;
            char *grown;
            if(next>max_len+1u)next=max_len+1u;
            if(next<=cap){free(buf);return NULL;}
            grown=(char*)realloc(buf,next);
            if(!grown){free(buf);return NULL;}
            buf=grown;cap=next;
        }
        buf[len++]=(char)ch;
    }
    if(ferror(f)){free(buf);return NULL;}
    if(ch==EOF&&len==0u){free(buf);return NULL;}
    buf[len]='\0';
    return buf;
}

static char *dup_text(const char *s){size_t n;char *p;if(!s)return NULL;n=strlen(s)+1u;p=(char*)malloc(n);if(p)memcpy(p,s,n);return p;}

static char *decode_hex_text(const char *hex){
    size_t n,i;
    char *out;
    if(!hex)return NULL;
    n=strlen(hex);
    if(n>NIYAH_PROOF_MAX_HEX_BYTES||(n&1u)!=0u)return NULL;
    out=(char*)malloc(n/2u+1u);
    if(!out)return NULL;
    for(i=0u;i<n;i+=2u){
        int hi=(hex[i]>='0'&&hex[i]<='9')?hex[i]-'0':(hex[i]>='a'&&hex[i]<='f')?hex[i]-'a'+10:(hex[i]>='A'&&hex[i]<='F')?hex[i]-'A'+10:-1;
        int lo=(hex[i+1u]>='0'&&hex[i+1u]<='9')?hex[i+1u]-'0':(hex[i+1u]>='a'&&hex[i+1u]<='f')?hex[i+1u]-'a'+10:(hex[i+1u]>='A'&&hex[i+1u]<='F')?hex[i+1u]-'A'+10:-1;
        unsigned char byte;
        if(hi<0||lo<0){free(out);return NULL;}
        byte=(unsigned char)((hi<<4)|lo);
        if(byte==0u){free(out);return NULL;}
        out[i/2u]=(char)byte;
    }
    out[n/2u]='\0';
    return out;
}

static bool proof_file_size_ok(FILE *f){
    long size;
    if(!f)return false;
    if(fseek(f,0,SEEK_END)!=0)return false;
    size=ftell(f);
    if(size<0||(unsigned long)size>(unsigned long)NIYAH_PROOF_MAX_FILE_BYTES)return false;
    return fseek(f,0,SEEK_SET)==0;
}

bool niyah_proof_verify_saved(const char *proof_path,const char *rule_file_path,bool *rules_bound,bool *rules_verified){
    FILE *f;
    char *line;
    char header[32]={0},stored_hex[65]={0},prompt_hash_hex[65]={0},output_hash_hex[65]={0},rules_hash_hex[65]={0};
    char *prompt_data_hex=NULL,*output_data_hex=NULL,*prompt=NULL,*output=NULL;
    uint8_t stored[32],prompt_meta[32],output_meta[32],rules_meta[32],actual_prompt[32],actual_output[32],actual[32],empty_hash[32],file_hash[32];
    bool ok=false,bound=false,rule_ok=false;
    if(rules_bound)*rules_bound=false;
    if(rules_verified)*rules_verified=false;
    if(!proof_path)return false;
    f=fopen(proof_path,"rb");
    if(!f)return false;
    if(!proof_file_size_ok(f)){fclose(f);return false;}
    line=read_line_alloc(f,NIYAH_PROOF_MAX_LINE_BYTES);
    if(!line){fclose(f);return false;}
    (void)snprintf(header,sizeof(header),"%s",line);
    free(line);
    while(!feof(f)&&!ferror(f)&&
          (line=read_line_alloc(f,NIYAH_PROOF_MAX_LINE_BYTES))!=NULL){
        const char *value=NULL;
        if(!strncmp(line,"hash: ",6)){value=line+6;if(strlen(value)==64u)memcpy(stored_hex,value,65u);}
        else if(!strncmp(line,"prompt_hash: ",13)){value=line+13;if(strlen(value)==64u)memcpy(prompt_hash_hex,value,65u);}
        else if(!strncmp(line,"output_hash: ",13)){value=line+13;if(strlen(value)==64u)memcpy(output_hash_hex,value,65u);}
        else if(!strncmp(line,"rules_hash: ",12)){value=line+12;if(strlen(value)==64u)memcpy(rules_hash_hex,value,65u);}
        else if(!strncmp(line,"prompt_hex: ",12)){free(prompt_data_hex);prompt_data_hex=dup_text(line+12);}
        else if(!strncmp(line,"output_hex: ",12)){free(output_data_hex);output_data_hex=dup_text(line+12);}
        free(line);
    }
    if(ferror(f)||!feof(f)){(void)fclose(f);goto done;}
    if(fclose(f)!=0)goto done;
    if(strcmp(header,"NIYAH-PROOF-V2")!=0)goto done;
    if(!prompt_data_hex||!output_data_hex)goto done;
    if(strlen(prompt_data_hex)>NIYAH_PROOF_MAX_HEX_BYTES||strlen(output_data_hex)>NIYAH_PROOF_MAX_HEX_BYTES)goto done;
    prompt=decode_hex_text(prompt_data_hex);output=decode_hex_text(output_data_hex);
    if(!prompt||!output)goto done;
    if(!hex_to_hash(stored_hex,stored)||!hex_to_hash(prompt_hash_hex,prompt_meta)||!hex_to_hash(output_hash_hex,output_meta)||!hex_to_hash(rules_hash_hex,rules_meta))goto done;
    hash_text(prompt,actual_prompt);hash_text(output,actual_output);hash_text(NULL,empty_hash);
    if(memcmp(prompt_meta,actual_prompt,32u)!=0||memcmp(output_meta,actual_output,32u)!=0)goto done;
    niyah_proof_generate_hashed(prompt,output,rules_meta,actual);
    if(memcmp(stored,actual,32u)!=0)goto done;
    bound=memcmp(rules_meta,empty_hash,32u)!=0;
    if(!bound)rule_ok=true;
    else if(rule_file_path&&niyah_sha256_file(rule_file_path,file_hash)&&memcmp(file_hash,rules_meta,32u)==0)rule_ok=true;
    ok=true;
done:
    free(prompt_data_hex);free(output_data_hex);free(prompt);free(output);
    if(rules_bound)*rules_bound=bound;
    if(rules_verified)*rules_verified=rule_ok;
    return ok;
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
