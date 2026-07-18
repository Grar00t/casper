import os
os.chdir(r'C:/Users/sulaimanalshammari/Casper_Engine')
# Writes the COMPLETE casper_rag.c in one atomic open('wb') - no duplication.
# Usage: python scripts/gen_rag.py
out = open('Core_CPP/casper_rag.c', 'wb')
L=[]
L+=[b'/* casper_rag.c - RAG: DDG/Bing/SearXNG->context->JSON. WinHTTP. C11. */']
L+=[b'#include "casper_rag.h"',b'#include "proof_generator.h"']
L+=[b'#include <stdio.h>',b'#include <stdlib.h>',b'#include <string.h>']
L+=[b'#include <ctype.h>',b'#include <stdarg.h>',b'#include <time.h>']
L+=[b'#ifdef _WIN32',b'#  define WIN32_LEAN_AND_MEAN',b'#  include <windows.h>']
L+=[b'#  include <winhttp.h>',b'#  pragma comment(lib,"winhttp.lib")',b'#endif']
L+=[b'static uint32_t ms_now(void){']
L+=[b'#ifdef _WIN32',b'    return (uint32_t)GetTickCount();',b'#else']
L+=[b'    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);']
L+=[b'    return (uint32_t)(ts.tv_sec*1000+ts.tv_nsec/1000000);',b'#endif',b'}']
L+=[b'#define HBUF_MAX (256*1024)']
L+=[b'typedef struct{char *buf;size_t len,cap;}HBuf;']
L+=[b'static HBuf *hbuf_new(void){HBuf *b=calloc(1,sizeof*b);if(!b)return NULL;']
L+=[b'    b->cap=8192;b->buf=malloc(b->cap);']
L+=[b'    if(!b->buf){free(b);return NULL;}b->buf[0]=0;return b;}']
L+=[b'static void hbuf_free(HBuf *b){if(b){free(b->buf);free(b);}}']
L+=[b'static int hbuf_app(HBuf *b,const char *d,size_t n){']
L+=[b'    if(b->len+n+1>b->cap){size_t nc=(b->len+n+1)*2;']
L+=[b'        if(nc>HBUF_MAX)nc=HBUF_MAX;if(b->len+n+1>nc)return -1;']
L+=[b'        char *p=realloc(b->buf,nc);if(!p)return -1;b->buf=p;b->cap=nc;}']
L+=[b'    memcpy(b->buf+b->len,d,n);b->len+=n;b->buf[b->len]=0;return 0;}']
L+=[b'static void url_enc(const char *in,char *out,size_t max){size_t o=0;']
L+=[b'    for(const unsigned char *p=(const unsigned char*)in;*p&&o+4<max;p++){']
L+=[b'        unsigned char c=*p;']
L+=[b'        if(isalnum(c)||c==45||c==95||c==46||c==126)out[o++]=(char)c;']
L+=[b'        else o+=(size_t)snprintf(out+o,max-o,"%%%02X",c);}out[o]=0;}']
out.write(b'\n'.join(L)+b'\n')
print('hdr done bytes='+str(out.tell()))
out.close()
