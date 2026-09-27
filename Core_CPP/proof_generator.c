/* proof_generator.c — SHA-256 proof generation and verification. C11. */
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
    int i;
    for(i=0;i<16;i++)W[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for(i=16;i<64;i++)W[i]=SIG1(W[i-2])+W[i-7]+SIG0(W[i-15])+W[i-16];
    a=c->state[0];bv=c->state[1];cc=c->state[2];d=c->state[3];e=c->state[4];f=c->state[5];g=c->state[6];h=c->state[7];
    for(i=0;i<64;i++){t1=h+EP1(e)+CH(e,f,g)+K[i]+W[i];t2=EP0(a)+MAJ(a,bv,cc);h=g;g=f;f=e;e=d+t1;d=cc;cc=bv;bv=a;a=t1+t2;}
    c->state[0]+=a;c->state[1]+=bv;c->state[2]+=cc;c->state[3]+=d;c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;}
static void sha256_update(SHA256_CTX *c,const uint8_t *data,size_t len){
    if(!data&&len)return;
    while(len){size_t n=64u-c->buflen;if(n>len)n=len;memcpy(c->buffer+c->buflen,data,n);c->buflen+=(uint32_t)n;data+=n;len-=n;if(c->buflen==64u){sha256_transform(c,c->buffer);c->bitcount+=512u;c->buflen=0;}}}
static void sha256_final(SHA256_CTX *c,uint8_t out[32]){
    int i;
    c->bitcount+=(uint64_t)c->buflen*8u;c->buffer[c->buflen++]=0x80u;
    if(c->buflen>56u){while(c->buflen<64u)c->buffer[c->buflen++]=0;sha256_transform(c,c->buffer);c->buflen=0;}
    while(c->buflen<56u)c->buffer[c->buflen++]=0;
    for(i=7;i>=0;i--)c->buffer[c->buflen++]=(uint8_t)(c->bitcount>>(i*8));
    sha256_transform(c,c->buffer);
    for(i=0;i<8;i++){out[i*4]=(uint8_t)(c->state[i]>>24);out[i*4+1]=(uint8_t)(c->state[i]>>16);out[i*4+2]=(uint8_t)(c->state[i]>>8);out[i*4+3]=(uint8_t)c->state[i];}}

void niyah_sha256(const uint8_t *data,size_t len,uint8_t out[32]){SHA256_CTX c;sha256_init(&c);sha256_update(&c,data,len);sha256_final(&c,out);}
void niyah_hash_to_hex(const uint8_t h[32],char hex[65]){static const char x[]="0123456789abcdef";int i;for(i=0;i<32;i++){hex[i*2]=x[h[i]>>4];hex[i*2+1]=x[h[i]&15u];}hex[64]='\0';}

static bool hex_to_hash(const char *s,uint8_t h[32]){
    int i;
    if(!s||strlen(s)!=64u)return false;
    for(i=0;i<32;i++){
        char a=s[i*2],b=s[i*2+1];
        int hi=(a>='0'&&a<='9')?a-'0':(a>='a'&&a<='f')?a-'a'+10:(a>='A'&&a<='F')?a-'A'+10:-1;
        int lo=(b>='0'&&b<='9')?b-'0':(b>='a'&&b<='f')?b-'a'+10:(b>='A'&&b<='F')?b-'A'+10:-1;
        if(hi<0||lo<0)return false;
        h[i]=(uint8_t)((hi<<4)|lo);
    }
    return true;
}

static bool sha256_file(const char *path,uint8_t out[32]){
    FILE *f;
    SHA256_CTX c;
    uint8_t buf[4096];
    size_t n;
    if(!path||!path[0]){niyah_sha256(NULL,0u,out);return true;}
    f=fopen(path,"rb");
    if(!f)return false;
    sha256_init(&c);
    while((n=fread(buf,1u,sizeof(buf),f))>0u)sha256_update(&c,buf,n);
    if(ferror(f)){fclose(f);return false;}
    if(fclose(f)!=0)return false;
    sha256_final(&c,out);
    return true;
}

static bool proof_generate_v2(const char *prompt,const char *output,const char *rule_file,
                              uint8_t proof[32],uint8_t rules_hash[32]){
    SHA256_CTX c;
    uint8_t sep=0u;
    if(!proof||!rules_hash)return false;
    if(!sha256_file(rule_file,rules_hash)){memset(proof,0,32u);memset(rules_hash,0,32u);return false;}
    sha256_init(&c);
    if(prompt)sha256_update(&c,(const uint8_t*)prompt,strlen(prompt));
    sha256_update(&c,&sep,1u);
    if(output)sha256_update(&c,(const uint8_t*)output,strlen(output));
    sha256_update(&c,&sep,1u);
    sha256_update(&c,rules_hash,32u);
    sha256_final(&c,proof);
    return true;
}

static void proof_generate_v1_raw(const char *prompt,const char *output,const char *rule_text,uint8_t proof[32]){
    SHA256_CTX c;uint8_t sep=0u;
    sha256_init(&c);
    if(prompt)sha256_update(&c,(const uint8_t*)prompt,strlen(prompt));
    sha256_update(&c,&sep,1u);
    if(output)sha256_update(&c,(const uint8_t*)output,strlen(output));
    sha256_update(&c,&sep,1u);
    if(rule_text)sha256_update(&c,(const uint8_t*)rule_text,strlen(rule_text));
    sha256_final(&c,proof);
}

void niyah_proof_generate(const char *prompt,const char *output,const char *rule_file,uint8_t proof[32]){
    uint8_t rules_hash[32];
    if(!proof)return;
    (void)proof_generate_v2(prompt,output,rule_file,proof,rules_hash);
}

static int write_escaped(FILE *f,const char *label,const char *s){
    const unsigned char *p=(const unsigned char *)(s?s:"");
    static const char hex[]="0123456789abcdef";
    if(fprintf(f,"%s",label)<0)return -1;
    while(*p){
        unsigned char c=*p++;
        if(c=='\\'){if(fputs("\\\\",f)==EOF)return -1;}
        else if(c=='\n'){if(fputs("\\n",f)==EOF)return -1;}
        else if(c=='\r'){if(fputs("\\r",f)==EOF)return -1;}
        else if(c=='\t'){if(fputs("\\t",f)==EOF)return -1;}
        else if(c<0x20u){if(fputc('\\',f)==EOF||fputc('x',f)==EOF||fputc(hex[c>>4],f)==EOF||fputc(hex[c&15u],f)==EOF)return -1;}
        else if(fputc((int)c,f)==EOF)return -1;
    }
    return fputc('\n',f)==EOF?-1:0;
}

static int hexval(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}

static bool unescape_value(const char *src,char *dst,size_t cap){
    size_t o=0u;
    if(!src||!dst||cap==0u)return false;
    while(*src&&*src!='\r'&&*src!='\n'){
        unsigned char c=(unsigned char)*src++;
        if(c=='\\'){
            int hi,lo;
            c=(unsigned char)*src++;
            if(c=='\0')return false;
            if(c=='n')c='\n';
            else if(c=='r')c='\r';
            else if(c=='t')c='\t';
            else if(c=='\\')c='\\';
            else if(c=='x'){
                hi=hexval(src[0]);lo=hexval(src[1]);
                if(hi<0||lo<0)return false;
                c=(unsigned char)((hi<<4)|lo);src+=2;
            }
        }
        if(o+1u>=cap)return false;
        dst[o++]=(char)c;
    }
    dst[o]='\0';
    return true;
}

int niyah_proof_save(const char *path,const uint8_t proof[32],const char *prompt,const char *output,const char *rule_file){
    FILE *f;
    char hex[65];
    uint8_t expected[32],rules_hash[32],h[32];
    if(!path||!proof)return -1;
    if(!proof_generate_v2(prompt,output,rule_file,expected,rules_hash))return -1;
    if(memcmp(expected,proof,32u)!=0)return -1;
    f=fopen(path,"w");
    if(!f)return -1;
    if(fputs("NIYAH-PROOF-V2\n",f)==EOF){fclose(f);return -1;}
    niyah_hash_to_hex(proof,hex);if(fprintf(f,"hash: %s\n",hex)<0){fclose(f);return -1;}
    niyah_sha256((const uint8_t*)(prompt?prompt:""),prompt?strlen(prompt):0u,h);niyah_hash_to_hex(h,hex);if(fprintf(f,"prompt_hash: %s\n",hex)<0){fclose(f);return -1;}
    niyah_sha256((const uint8_t*)(output?output:""),output?strlen(output):0u,h);niyah_hash_to_hex(h,hex);if(fprintf(f,"output_hash: %s\n",hex)<0){fclose(f);return -1;}
    niyah_hash_to_hex(rules_hash,hex);if(fprintf(f,"rules_hash: %s\n",hex)<0){fclose(f);return -1;}
    if(write_escaped(f,"rules_path: ",rule_file?rule_file:"")!=0||
       write_escaped(f,"prompt: ",prompt?prompt:"")!=0||
       write_escaped(f,"output: ",output?output:"")!=0){fclose(f);return -1;}
    if(ferror(f)){fclose(f);return -1;}
    return fclose(f)==0?0:-1;
}

static bool verify_v2(FILE *f,const char *proof_path){
    char line[32768];
    char stored_hex[65]={0},prompt_hex[65]={0},output_hex[65]={0},rules_hex[65]={0};
    char prompt[8192]={0},output[16384]={0},rules_path[4096]={0};
    uint8_t stored[32],actual[32],actual_rules[32],recorded_rules[32],h[32];
    (void)proof_path;
    while(fgets(line,sizeof(line),f)){
        if(!strncmp(line,"hash: ",6)){size_t l=strcspn(line+6,"\r\n");if(l==64u){memcpy(stored_hex,line+6,64u);stored_hex[64]='\0';}}
        else if(!strncmp(line,"prompt_hash: ",13)){size_t l=strcspn(line+13,"\r\n");if(l==64u){memcpy(prompt_hex,line+13,64u);prompt_hex[64]='\0';}}
        else if(!strncmp(line,"output_hash: ",13)){size_t l=strcspn(line+13,"\r\n");if(l==64u){memcpy(output_hex,line+13,64u);output_hex[64]='\0';}}
        else if(!strncmp(line,"rules_hash: ",12)){size_t l=strcspn(line+12,"\r\n");if(l==64u){memcpy(rules_hex,line+12,64u);rules_hex[64]='\0';}}
        else if(!strncmp(line,"rules_path: ",12)){if(!unescape_value(line+12,rules_path,sizeof(rules_path)))return false;}
        else if(!strncmp(line,"prompt: ",8)){if(!unescape_value(line+8,prompt,sizeof(prompt)))return false;}
        else if(!strncmp(line,"output: ",8)){if(!unescape_value(line+8,output,sizeof(output)))return false;}
    }
    if(!hex_to_hash(stored_hex,stored)||!hex_to_hash(rules_hex,recorded_rules))return false;
    niyah_sha256((const uint8_t*)prompt,strlen(prompt),h);niyah_hash_to_hex(h,line);if(strcmp(line,prompt_hex)!=0)return false;
    niyah_sha256((const uint8_t*)output,strlen(output),h);niyah_hash_to_hex(h,line);if(strcmp(line,output_hex)!=0)return false;
    if(!proof_generate_v2(prompt,output,rules_path[0]?rules_path:NULL,actual,actual_rules))return false;
    if(memcmp(actual_rules,recorded_rules,32u)!=0)return false;
    return memcmp(stored,actual,32u)==0;
}

bool niyah_proof_verify(const char *proof_path,const char *prompt,const char *output,const char *rule_file){
    FILE *f;
    char first[64];
    if(!proof_path)return false;
    f=fopen(proof_path,"r");
    if(!f)return false;
    if(!fgets(first,sizeof(first),f)){fclose(f);return false;}
    if(!strcmp(first,"NIYAH-PROOF-V2\n")||!strcmp(first,"NIYAH-PROOF-V2\r\n")){
        bool ok=verify_v2(f,proof_path);fclose(f);return ok;
    }
    if(!strcmp(first,"NIYAH-PROOF-V1\n")||!strcmp(first,"NIYAH-PROOF-V1\r\n")){
        char line[4096];char stored_hex[65]={0};uint8_t expected[32],actual[32];
        while(fgets(line,sizeof(line),f)){
            if(!strncmp(line,"hash: ",6)){size_t l=strcspn(line+6,"\r\n");if(l==64u){memcpy(stored_hex,line+6,64u);stored_hex[64]='\0';}break;}
        }
        fclose(f);
        if(!hex_to_hash(stored_hex,expected))return false;
        proof_generate_v1_raw(prompt,output,rule_file,actual);
        return memcmp(expected,actual,32u)==0;
    }
    fclose(f);
    return false;
}

int niyah_proof_smoke(void){
    int fail=0;
    uint8_t h[32];char hex[65];
    const char *rules="niyah_test_rules.nrule";
    const char *proof="niyah_test.proof";
    FILE *f;
    niyah_sha256((const uint8_t*)"",0u,h);niyah_hash_to_hex(h,hex);if(strncmp(hex,"e3b0c44298fc1c14",16u))++fail;
    niyah_sha256((const uint8_t*)"abc",3u,h);niyah_hash_to_hex(h,hex);if(strncmp(hex,"ba7816bf8f01cfea",16u))++fail;
    f=fopen(rules,"wb");if(!f)return fail+1;
    if(fputs("rule: \"IF output CONTAINS 'bad' THEN output = REJECTED\"\n",f)==EOF||fclose(f)!=0){remove(rules);return fail+1;}
    niyah_proof_generate("hello\nuser","world\nanswer",rules,h);
    if(niyah_proof_save(proof,h,"hello\nuser","world\nanswer",rules)!=0)++fail;
    else if(!niyah_proof_verify(proof,NULL,NULL,NULL))++fail;
    f=fopen(rules,"ab");if(!f)++fail;else{if(fputs("// tampered\n",f)==EOF)++fail;if(fclose(f)!=0)++fail;}
    if(niyah_proof_verify(proof,NULL,NULL,NULL))++fail;
    remove(proof);remove(rules);
    return fail;
}
