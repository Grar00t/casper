/* Unit regression for SearXNG field-order and object-boundary parsing. */
#include "../Core_CPP/casper_rag.c"
#include <stdio.h>
#include <string.h>
static int failed = 0;
static void check(int ok, const char *label) {
    if (!ok) { fprintf(stderr,"FAIL %s\n",label); ++failed; }
}
int main(void) {
    RagResult r[3] = {{0}};
    const char *first = "{\"results\":[{\"title\":\"Title A\",\"url\":\"https://example.test/a\",\"content\":\"Snippet A\"},"
                        "{\"url\":\"https://example.test/b\",\"nested\":{\"title\":\"Wrong\",\"url\":\"https://bad.test/\"},"
                        "\"content\":\"Snippet B\",\"title\":\"Title B\"}]}";
    int n = parse_searxng(first,r,3);
    check(n==2,"result count independent of key order");
    check(strcmp(r[0].title,"Title A")==0,"title before URL");
    check(strcmp(r[0].snippet,"Snippet A")==0,"first snippet");
    check(strcmp(r[1].title,"Title B")==0,"title after URL and nested object");
    check(strcmp(r[1].url,"https://example.test/b")==0,"top-level URL only");
    memset(r,0,sizeof(r));
    const char *missing = "{\"results\":[{\"url\":\"https://example.test/c\",\"content\":\"No title\"},"
                          "{\"title\":\"Second title\",\"url\":\"https://example.test/d\",\"content\":\"Second\"}]}";
    n = parse_searxng(missing,r,3);
    check(n==2,"missing title does not consume next result");
    check(r[0].title[0]=='\0',"missing title stays blank");
    check(strcmp(r[1].title,"Second title")==0,"second title retained");
    memset(r,0,sizeof(r));
    const char *escaped = "{\"results\":[{\"content\":\"Quoted \\\"brace}\\\" snippet\","
                          "\"url\":\"https://example.test/e\",\"title\":\"Escaped\"}]}";
    n = parse_searxng(escaped,r,3);
    check(n==1,"escaped quote and brace in JSON string");
    check(strcmp(r[0].snippet,"Quoted \"brace}\" snippet")==0,"decode escaped string");
    memset(r,0,sizeof(r));
    n = parse_searxng("{\"results\":[{\"url\":\"file:///etc/passwd\",\"title\":\"Bad\"}]}",r,3);
    check(n==0,"reject unsupported URL scheme");
    if(!failed) puts("RAG_JSON_REGRESSION=PASS");
    return failed ? 1 : 0;
}
