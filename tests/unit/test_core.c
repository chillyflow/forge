#include "internal.h"
#include <assert.h>
/* Do not compile out assertions in Release builds. */
#ifdef NDEBUG
#undef NDEBUG
#include <assert.h>
#endif
static size_t count(const char *s,void *u){(void)u;return strlen(s);}
int main(void){
    fg_buf b={0};assert(fg_buf_puts(&b,"abc"));assert(fg_buf_printf(&b,"%d",123));char *s=fg_buf_take(&b);assert(!strcmp(s,"abc123"));free(s);
    forge_error e={0};char path[FG_PATH_MAX];assert(!fg_safe_path(".","../secret",true,path,&e));assert(e.code==FORGE_ERR_POLICY);assert(!fg_safe_path(".","C:/secret",true,path,&e));assert(!fg_safe_path(".",".git/config",true,path,&e));
    forge_context *c=forge_context_create(300,50,count,NULL);assert(c);uint64_t a=forge_context_add(c,FORGE_SEG_SYSTEM,"stable system",100,true,0,0);assert(a);
    uint64_t action=forge_context_add(c,FORGE_SEG_ACTION,"read_file",1,false,0,0);forge_context_add(c,FORGE_SEG_RESULT,"important result",90,false,action,0);
    size_t tokens=0,evicted=0;s=forge_context_plan(c,&tokens,&evicted,&e);assert(s && tokens<=250);assert(strstr(s,"read_file")<strstr(s,"important result"));free(s);
    assert(forge_context_update(c,a,"new system",1)==FORGE_OK);s=forge_context_plan(c,&tokens,&evicted,&e);assert(s && strstr(s,"new system"));free(s);forge_context_destroy(c);
    c=forge_context_create(30,10,count,NULL);forge_context_add(c,FORGE_SEG_SYSTEM,"This pinned text cannot possibly fit",100,true,0,0);assert(!forge_context_plan(c,NULL,NULL,&e));assert(e.code==FORGE_ERR_LIMIT);forge_context_destroy(c);
    const char *j="{\"path\":\"x\",\"start\":1,\"end\":2}";yyjson_doc *d=yyjson_read(j,strlen(j),0);assert(d && fg_tool_validate("read_file",yyjson_doc_get_root(d),&e));yyjson_doc_free(d);
    j="{\"path\":\"x\",\"start\":-1,\"end\":2}";d=yyjson_read(j,strlen(j),0);assert(!fg_tool_validate("read_file",yyjson_doc_get_root(d),&e));yyjson_doc_free(d);
    s=fg_tool_grammar();assert(s && strstr(s,"call0") && strstr(s,"root ::="));free(s);
    const char *raw="{\"Action\":\"pass\",\"Test\":\"TestOK\"}\n{\"Action\":\"fail\",\"Package\":\"example/math\",\"Test\":\"TestAdd\"}\n{\"Action\":\"output\",\"Output\":\"math_test.go:9: expected 4 got 3\\n\"}\n";
    s=fg_compress_output(raw,1024,NULL,&e);assert(s && strstr(s,"TestAdd") && strstr(s,"expected 4") && strstr(s,"pass=1"));free(s);
    puts("core tests passed");return 0;
}
