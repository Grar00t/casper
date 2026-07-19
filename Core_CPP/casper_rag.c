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
#ifdef _WIN32
static HBuf *http_get(const char *host,const char *path){
    HBuf *out=hbuf_new();if(!out)return NULL;
    int whl=MultiByteToWideChar(CP_UTF8,0,host,-1,NULL,0);
    int wpl=MultiByteToWideChar(CP_UTF8,0,path,-1,NULL,0);
    wchar_t *wh=malloc((size_t)whl*sizeof(wchar_t));
    wchar_t *wp=malloc((size_t)wpl*sizeof(wchar_t));
    if(!wh||!wp){free(wh);free(wp);hbuf_free(out);return NULL;}
    MultiByteToWideChar(CP_UTF8,0,host,-1,wh,whl);
    MultiByteToWideChar(CP_UTF8,0,path,-1,wp,wpl);
    HINTERNET hs=WinHttpOpen(L"Casper/1.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    free(wh);free(wp);
    if(!hs){hbuf_free(out);return NULL;}
    HINTERNET hc=WinHttpConnect(hs,wh,INTERNET_DEFAULT_HTTPS_PORT,0);
    if(!hc){WinHttpCloseHandle(hs);hbuf_free(out);return NULL;}
    HINTERNET hr=WinHttpOpenRequest(hc,L"GET",wp,NULL,WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if(!hr){WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);hbuf_free(out);return NULL;}
    WinHttpAddRequestHeaders(hr,L"User-Agent: Mozilla/5.0",(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);
    DWORD to=RAG_TIMEOUT_MS;
    WinHttpSetOption(hr,WINHTTP_OPTION_RECEIVE_TIMEOUT,&to,sizeof(to));
    if(!WinHttpSendRequest(hr,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(hr,NULL)){
        WinHttpCloseHandle(hr);WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);
        hbuf_free(out);return NULL;}
    char tmp[8192];DWORD tmplen=8192,av=0,rd=0;
    while(WinHttpQueryDataAvailable(hr,&av) && av>0){
        DWORD ch=av<tmplen?av:tmplen;
        if(!WinHttpReadData(hr,tmp,ch,&rd))break;
        if(hbuf_app(out,tmp,rd)!=0)break;}
    WinHttpCloseHandle(hr);WinHttpCloseHandle(hc);WinHttpCloseHandle(hs);
    return out;
}
#endif

static void strip_tags(const char *h,char *o,size_t max){
    size_t n=0;int t=0;
    for(const char *p=h;*p&&n+2<max;p++){
	if(*p==60){t=1;continue;}
	if(*p==62){t=0;if(n>0&&o[n-1]!=32)o[n++]=32;continue;}
	if(t)continue;
	if(*p==38){
	    if(!strncmp(p,"&amp;",5)){o[n++]=38;p+=4;}
	    else if(!strncmp(p,"&lt;",4)){o[n++]=60;p+=3;}
	    else if(!strncmp(p,"&gt;",4)){o[n++]=62;p+=3;}
	    else if(!strncmp(p,"&quot;",6)){o[n++]=34;p+=5;}
	    else if(!strncmp(p,"&nbsp;",6)){o[n++]=32;p+=5;}
	    else o[n++]=38;continue;}
	if(*p==13||*p==10||*p==9){if(n>0&&o[n-1]!=32)o[n++]=32;}
	else o[n++]=*p;}
    while(n>0&&o[n-1]==32)n--;o[n]=0;}

static int parse_ddg(const char *html,RagResult *res,int max){
    int n=0;const char *p=html;
    while(n<max){
        /* Try multiple patterns — DDG changes HTML structure often */
        const char *h=NULL;
        /* Pattern 1: class="result__a" href="URL" */
        h=strstr(p,"result__a\" href=\"");
        if(h){h+=17;}
        else{
            /* Pattern 2: href="/l/?uddg=ENCODED_URL" (redirect links) */
            h=strstr(p,"/l/?uddg=");
            if(h){
                h+=9; /* skip /l/?uddg= */
                /* URL-decode the target URL */
                const char *he2=strstr(h,"&");
                if(!he2) he2=strchr(h,34); /* quote */
                if(!he2){p=h+1;continue;}
                size_t el=(size_t)(he2-h);
                if(el>=RAG_URL_MAX)el=RAG_URL_MAX-1;
                /* Simple URL decode */
                char enc[RAG_URL_MAX];memcpy(enc,h,el);enc[el]=0;
                char *dst=res[n].url;size_t di=0;
                for(size_t i=0;i<el&&di<RAG_URL_MAX-1;i++){
                    if(enc[i]=='%'&&i+2<el){
                        char hx[3]={enc[i+1],enc[i+2],0};
                        dst[di++]=(char)strtol(hx,NULL,16);i+=2;
                    }else{dst[di++]=enc[i];}}
                dst[di]=0;
                /* Find the link text for title */
                const char *ts2=strchr(he2,62);
                if(ts2){ts2++;
                    const char *te2=strstr(ts2,"</a>");
                    if(te2){char raw2[RAG_TITLE_MAX*2];
                        size_t tl2=(size_t)(te2-ts2);
                        if(tl2>=sizeof(raw2))tl2=sizeof(raw2)-1;
                        memcpy(raw2,ts2,tl2);raw2[tl2]=0;
                        strip_tags(raw2,res[n].title,RAG_TITLE_MAX);
                        p=te2+4;
                    }else{res[n].title[0]=0;p=he2+1;}}
                else{res[n].title[0]=0;p=he2+1;}
                /* Find snippet */
                const char *sn2=strstr(p,"result__snippet");
                if(sn2){const char *s0=strchr(sn2,62);
                    if(s0){s0++;const char *s1=strstr(s0,"</");
                        if(s1){char rs2[RAG_SNIPPET_MAX*2];
                            size_t sl2=(size_t)(s1-s0);
                            if(sl2>=sizeof(rs2))sl2=sizeof(rs2)-1;
                            memcpy(rs2,s0,sl2);rs2[sl2]=0;
                            strip_tags(rs2,res[n].snippet,RAG_SNIPPET_MAX);
                            p=s1;}}}
                else{res[n].snippet[0]=0;}
                /* Only count if URL is real */
                if(res[n].url[0]&&strncmp(res[n].url,"http",4)==0)n++;
                continue;
            }
            /* Pattern 3: no more results */
            break;
        }
        /* Pattern 1 continues here */
        const char *he=strchr(h,34);if(!he)break;
        size_t ul=(size_t)(he-h);if(ul>=RAG_URL_MAX)ul=RAG_URL_MAX-1;
        memcpy(res[n].url,h,ul);res[n].url[ul]=0;
        const char *ts=strchr(he,62);if(!ts)break;ts++;
        const char *te=strstr(ts,"</a>");if(!te)break;
        char raw[RAG_TITLE_MAX*2];size_t tl=(size_t)(te-ts);
        if(tl>=sizeof(raw))tl=sizeof(raw)-1;memcpy(raw,ts,tl);raw[tl]=0;
        strip_tags(raw,res[n].title,RAG_TITLE_MAX);
        const char *sn=strstr(te,"result__snippet");
        if(sn){const char *s0=strchr(sn,62);
            if(s0){s0++;const char *s1=strstr(s0,"</a>");
                if(!s1)s1=strstr(s0,"</");
                if(s1){char rs[RAG_SNIPPET_MAX*2];size_t sl=(size_t)(s1-s0);
                    if(sl>=sizeof(rs))sl=sizeof(rs)-1;
                    memcpy(rs,s0,sl);rs[sl]=0;
                    strip_tags(rs,res[n].snippet,RAG_SNIPPET_MAX);p=s1;}}}
        else{res[n].snippet[0]=0;p=te;}
        if(res[n].url[0]&&res[n].title[0])n++;
        p=(p>te+4)?p:te+4;}
    return n;}

static float score_rel(const char *q,const RagResult *r){
    char qc[512];strncpy(qc,q,511);qc[511]=0;
    for(char *c=qc;*c;c++)*c=(char)tolower((unsigned char)*c);
    int hits=0,total=0;char *tok=strtok(qc," \t\r\n");
    while(tok){if(strlen(tok)>=3){total++;
        char tl[RAG_TITLE_MAX],sl[RAG_SNIPPET_MAX];
        strncpy(tl,r->title,RAG_TITLE_MAX-1);tl[RAG_TITLE_MAX-1]=0;
        strncpy(sl,r->snippet,RAG_SNIPPET_MAX-1);sl[RAG_SNIPPET_MAX-1]=0;
        for(char *c=tl;*c;c++)*c=(char)tolower((unsigned char)*c);
        for(char *c=sl;*c;c++)*c=(char)tolower((unsigned char)*c);
        if(strstr(tl,tok)||strstr(sl,tok))hits++;}
        tok=strtok(NULL," \t\r\n");}
    return total?((float)hits/(float)total):0.5f;}

static void tr_add(RagCtx *ctx,TraceKind k,float conf,uint32_t t0,const char *fmt,...){
    if(ctx->n_steps>=RAG_TRACE_MAX)return;
    TraceStep *st=&ctx->trace[ctx->n_steps++];
    st->kind=k;st->elapsed_ms=ms_now()-t0;st->confidence=conf;
    va_list ap;va_start(ap,fmt);vsnprintf(st->detail,sizeof(st->detail),fmt,ap);va_end(ap);}

bool casper_rag_online(RagBackend backend){
    (void)backend;
#ifdef _WIN32
    HINTERNET h=WinHttpOpen(L"Casper/1.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!h)return false;
    HINTERNET hc=WinHttpConnect(h,L"html.duckduckgo.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    bool ok=(hc!=NULL);if(hc)WinHttpCloseHandle(hc);WinHttpCloseHandle(h);return ok;
#else
    return true;
#endif
}

RagCtx *casper_rag_query(const char *q,RagBackend b,const char *r){(void)r;RagCtx *ctx=calloc(1,sizeof*ctx);if(!ctx)return NULL;uint32_t t0=ms_now();strncpy(ctx->query,q,sizeof(ctx->query)-1);tr_add(ctx,TRACE_PARSE,0.0f,t0,"parsed %zu",strlen(q));char enc[512];url_enc(q,enc,sizeof(enc));char host[128],path[640];switch(b){case RAG_BACKEND_BING:strncpy(host,"www.bing.com",127);snprintf(path,sizeof(path),"/search?q=%s&setlang=en",enc);break;default:strncpy(host,"html.duckduckgo.com",127);snprintf(path,sizeof(path),"/html/?q=%s",enc);break;}tr_add(ctx,TRACE_SEARCH,0.0f,t0,"GET https://%s%s",host,path);
#ifdef _WIN32
    HBuf *resp=http_get(host,path);
#else
    HBuf *resp=NULL;
#endif
    if(!resp||resp->len==0){tr_add(ctx,TRACE_WARN,0.0f,t0,"offline");hbuf_free(resp);ctx->elapsed_ms=ms_now()-t0;return ctx;}tr_add(ctx,TRACE_FETCH,0.9f,t0,"received %zu",resp->len);ctx->n_results=parse_ddg(resp->buf,ctx->results,RAG_MAX_RESULTS);hbuf_free(resp);tr_add(ctx,TRACE_RANK,0.9f,t0,"parsed %d",ctx->n_results);float tot=0.0f;for(int i=0;i<ctx->n_results;i++){ctx->results[i].score=score_rel(q,&ctx->results[i]);tot+=ctx->results[i].score;char anc[RAG_URL_MAX+RAG_SNIPPET_MAX+2];size_t ul=strlen(ctx->results[i].url);size_t sl=strlen(ctx->results[i].snippet);memcpy(anc,ctx->results[i].url,ul);anc[ul]=0;memcpy(anc+ul+1,ctx->results[i].snippet,sl);niyah_sha256((const uint8_t*)anc,ul+1+sl,ctx->results[i].sha256);}ctx->confidence=ctx->n_results?tot/(float)ctx->n_results:0.0f;size_t cp=0;for(int i=0;i<ctx->n_results&&cp+64<RAG_CONTEXT_MAX;i++)cp+=(size_t)snprintf(ctx->context+cp,RAG_CONTEXT_MAX-cp,"[%d] %s\n%s\n\n",i+1,ctx->results[i].title,ctx->results[i].snippet);tr_add(ctx,TRACE_CONTEXT,ctx->confidence,t0,"context %zu",cp);niyah_sha256((const uint8_t*)ctx->context,cp,ctx->chain_hash);tr_add(ctx,TRACE_COMPOSE,ctx->confidence,t0,"conf=%.2f",(double)ctx->confidence);ctx->elapsed_ms=ms_now()-t0;return ctx;}
void casper_rag_free(RagCtx *ctx){free(ctx);}


char *casper_rag_to_json(const RagCtx *ctx){
    if(!ctx)return NULL;
    size_t cap=16384;char *out=malloc(cap);if(!out)return NULL;
    char hx[65];niyah_hash_to_hex(ctx->chain_hash,hx);
    int n=(int)snprintf(out,cap,
        "{\"query\":\"%s\",\"confidence\":%.3f,\"elapsed_ms\":%u,"
        "\"chain_hash\":\"%s\",\"n_sources\":%d,\"n_steps\":%d}",
        ctx->query,(double)ctx->confidence,ctx->elapsed_ms,
        hx,ctx->n_results,ctx->n_steps);
    if(n<0||n>=(int)cap){free(out);return NULL;}
    return out;
}
