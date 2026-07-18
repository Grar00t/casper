/* casper_rag.c - RAG: DDG/Bing/SearXNG->context->JSON. WinHTTP. C11. */
#include "casper_rag.h"
#include "proof_generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib,"winhttp.lib")
#endif
static uint32_t ms_now(void){
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint32_t)(ts.tv_sec*1000+ts.tv_nsec/1000000);
#endif
}
#define HBUF_MAX (256*1024)
typedef struct{char *buf;size_t len,cap;}HBuf;
static HBuf *hbuf_new(void){HBuf *b=calloc(1,sizeof*b);if(!b)return NULL;
    b->cap=8192;b->buf=malloc(b->cap);
    if(!b->buf){free(b);return NULL;}b->buf[0]=0;return b;}
static void hbuf_free(HBuf *b){if(b){free(b->buf);free(b);}}
static int hbuf_app(HBuf *b,const char *d,size_t n){
    if(b->len+n+1>b->cap){size_t nc=(b->len+n+1)*2;
        if(nc>HBUF_MAX)nc=HBUF_MAX;if(b->len+n+1>nc)return -1;
        char *p=realloc(b->buf,nc);if(!p)return -1;b->buf=p;b->cap=nc;}
    memcpy(b->buf+b->len,d,n);b->len+=n;b->buf[b->len]=0;return 0;}
static void url_enc(const char *in,char *out,size_t max){size_t o=0;
    for(const unsigned char *p=(const unsigned char*)in;*p&&o+4<max;p++){
        unsigned char c=*p;
        if(isalnum(c)||c==45||c==95||c==46||c==126)out[o++]=(char)c;
        else o+=(size_t)snprintf(out+o,max-o,"%%%02X",c);}out[o]=0;}
