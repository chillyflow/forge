#include "internal.h"
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#endif

void fg_process_free(fg_process_result *r) { free(r->out); free(r->err); memset(r,0,sizeof(*r)); }
static void capture(fg_buf *b,const char *p,size_t n,size_t cap,bool *truncated) {
    size_t take=FG_MIN(n,cap-b->len); if(take<n) *truncated=true; fg_buf_add(b,p,take);
}
static bool executable_path(const char *root,const char *name,char path[FG_PATH_MAX]){
    if(strchr(name,'/') || strchr(name,'\\')){if(strlen(name)>=FG_PATH_MAX)return false;strcpy(path,name);return true;}
    const char *env=getenv("PATH");if(!env)return false;
#ifdef _WIN32
    const char separator=';';
#else
    const char separator=':';
#endif
    const char *p=env;
    while(*p){const char *z=strchr(p,separator);size_t n=z?(size_t)(z-p):strlen(p);char dir[FG_PATH_MAX],canonical[FG_PATH_MAX];
        if(n && n<sizeof(dir)){memcpy(dir,p,n);dir[n]=0;
#ifdef _WIN32
            if(dir[0]=='"' && n>1 && dir[n-1]=='"'){memmove(dir,dir+1,n-2);dir[n-2]=0;}
            bool absolute=strlen(dir)>2 && dir[1]==':';
#else
            bool absolute=dir[0]=='/';
#endif
            if(absolute && fg_workspace(dir,canonical,NULL)){
                size_t root_len=strlen(root);
#ifdef _WIN32
                bool in_root=!_strnicmp(canonical,root,root_len) && (!canonical[root_len] || canonical[root_len]=='/' || canonical[root_len]=='\\');
#else
                bool in_root=!strncmp(canonical,root,root_len) && (!canonical[root_len] || canonical[root_len]=='/');
#endif
                if(!in_root && fg_path_join(path,canonical,name)){
#ifdef _WIN32
                    if(!strchr(name,'.') && strlen(path)+4<FG_PATH_MAX)strcat(path,".exe");DWORD attr=GetFileAttributesA(path);if(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_DIRECTORY))return true;
#else
                    struct stat st;if(stat(path,&st)==0 && S_ISREG(st.st_mode) && access(path,X_OK)==0)return true;
#endif
                }
            }
        }if(!z)break;p=z+1;
    }return false;
}
#ifdef _WIN32
static void quote_arg(fg_buf *b,const char *s) {
    fg_buf_puts(b,"\""); size_t slashes=0;
    for(;*s;s++) {
        if(*s=='\\') { slashes++; continue; }
        if(*s=='"') { for(size_t j=0;j<slashes*2+1;j++) fg_buf_puts(b,"\\"); }
        else for(size_t j=0;j<slashes;j++) fg_buf_puts(b,"\\");
        fg_buf_add(b,s,1); slashes=0;
    }
    for(size_t j=0;j<slashes*2;j++) fg_buf_puts(b,"\\"); fg_buf_puts(b,"\"");
}
static void drain(HANDLE h,fg_buf *b,size_t cap,bool *trunc) {
    DWORD avail=0,n=0; char block[4096];
    size_t reads=0;
    while(reads++<64 && PeekNamedPipe(h,NULL,0,NULL,&avail,NULL) && avail) {
        if(!ReadFile(h,block,(DWORD)FG_MIN(sizeof(block),avail),&n,NULL) || !n) break;
        capture(b,block,n,cap,trunc);
    }
}
#endif
forge_status fg_process(const char *root,const char *const *argv,uint64_t timeout,size_t max_bytes,
    forge_cancel_fn cancel,void *user,fg_process_result *r,forge_error *e) {
    if(!root || !argv || !argv[0] || !*argv[0] || !max_bytes || !timeout) return fg_error(e,FORGE_ERR_ARGUMENT,"Invalid process request");
    char executable[FG_PATH_MAX],canonical_root[FG_PATH_MAX];
    if(!fg_workspace(root,canonical_root,e))return FORGE_ERR_IO;
    if(!executable_path(canonical_root,argv[0],executable))return fg_error(e,FORGE_ERR_NOT_FOUND,"Executable not found outside workspace PATH entries: %s",argv[0]);
    memset(r,0,sizeof(*r)); r->exit_code=-1; uint64_t start=fg_now_ms(); fg_buf out={0},err={0};
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE out_r=NULL,out_w=NULL,err_r=NULL,err_w=NULL,job=NULL,null_in=NULL;
    PROCESS_INFORMATION pi={0}; fg_buf command={0},env={0};
    if(!CreatePipe(&out_r,&out_w,&sa,0) || !CreatePipe(&err_r,&err_w,&sa,0)) goto win_fail;
    SetHandleInformation(out_r,HANDLE_FLAG_INHERIT,0); SetHandleInformation(err_r,HANDLE_FLAG_INHERIT,0);
    null_in=CreateFileA("NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if(null_in==INVALID_HANDLE_VALUE) { null_in=NULL; goto win_fail; }
    for(size_t i=0;argv[i];i++) { if(i) fg_buf_puts(&command," "); quote_arg(&command,argv[i]); }
    /* Child processes receive a small allowlist, not API keys or the parent's full environment. */
    const char *keys[]={"PATH","SystemRoot","TEMP","TMP","USERPROFILE","LOCALAPPDATA","APPDATA",NULL};
    for(size_t i=0;keys[i];i++) { const char *v=getenv(keys[i]); if(v) { fg_buf_printf(&env,"%s=%s",keys[i],v); fg_buf_add(&env,"",1); } }
    fg_buf_add(&env,"",1);
    job=CreateJobObjectA(NULL,NULL); if(!job) goto win_fail;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) goto win_fail;
    STARTUPINFOA si={0}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES; si.hStdOutput=out_w; si.hStdError=err_w; si.hStdInput=null_in;
    if(command.failed || env.failed || !CreateProcessA(executable,command.data,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,env.data,root,&si,&pi)) goto win_fail;
    if(!AssignProcessToJobObject(job,pi.hProcess)) { TerminateProcess(pi.hProcess,125); goto win_fail; }
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); pi.hThread=NULL;
    CloseHandle(out_w); out_w=NULL; CloseHandle(err_w); err_w=NULL; CloseHandle(null_in); null_in=NULL;
    for(;;) {
        drain(out_r,&out,max_bytes,&r->truncated); drain(err_r,&err,max_bytes,&r->truncated);
        if(WaitForSingleObject(pi.hProcess,10)==WAIT_OBJECT_0) break;
        if(cancel && cancel(user)) { r->cancelled=true; TerminateJobObject(job,130); break; }
        if(fg_now_ms()-start>=timeout) { r->timed_out=true; TerminateJobObject(job,124); break; }
    }
    WaitForSingleObject(pi.hProcess,2000); DWORD code=0; GetExitCodeProcess(pi.hProcess,&code); r->exit_code=(int)code;
    /* Closing the job also stops orphaned descendants before draining their pipes. */
    CloseHandle(job); job=NULL; drain(out_r,&out,max_bytes,&r->truncated); drain(err_r,&err,max_bytes,&r->truncated);
    CloseHandle(pi.hProcess); CloseHandle(out_r); CloseHandle(err_r); fg_buf_clear(&command); fg_buf_clear(&env);
    goto collected;
win_fail:
    if(pi.hThread) CloseHandle(pi.hThread); if(pi.hProcess) CloseHandle(pi.hProcess);
    if(out_r) CloseHandle(out_r); if(out_w) CloseHandle(out_w); if(err_r) CloseHandle(err_r); if(err_w) CloseHandle(err_w);
    if(job) CloseHandle(job); if(null_in) CloseHandle(null_in); fg_buf_clear(&command); fg_buf_clear(&env);
    fg_buf_clear(&out); fg_buf_clear(&err); return fg_error(e,FORGE_ERR_IO,"Unable to start command (Windows error %lu)",(unsigned long)GetLastError());
collected:
#else
    int op[2],ep[2]; if(pipe(op)!=0) return fg_error(e,FORGE_ERR_IO,"pipe failed");
    if(pipe(ep)!=0) { close(op[0]);close(op[1]);return fg_error(e,FORGE_ERR_IO,"pipe failed"); }
    pid_t pid=fork(); if(pid<0) { close(op[0]);close(op[1]);close(ep[0]);close(ep[1]);return fg_error(e,FORGE_ERR_IO,"fork failed"); }
    if(pid==0) {
        setpgid(0,0); close(op[0]);close(ep[0]); dup2(op[1],STDOUT_FILENO);dup2(ep[1],STDERR_FILENO);close(op[1]);close(ep[1]);
        int input=open("/dev/null",O_RDONLY); if(input>=0) { dup2(input,STDIN_FILENO); close(input); }
        if(chdir(root)!=0) _exit(126);
        const char *path=getenv("PATH"),*home=getenv("HOME"),*tmp=getenv("TMPDIR");
        char *path_copy=fg_strdup(path?path:"/usr/bin:/bin"),*home_copy=fg_strdup(home?home:""),*tmp_copy=fg_strdup(tmp?tmp:"/tmp");
        extern char **environ; char *empty[]={NULL}; environ=empty;
        setenv("PATH",path_copy?path_copy:"/usr/bin:/bin",1);setenv("HOME",home_copy?home_copy:"",1);setenv("TMPDIR",tmp_copy?tmp_copy:"/tmp",1);
        setenv("LANG","C.UTF-8",1); execv(executable,(char *const *)argv); _exit(127);
    }
    setpgid(pid,pid); close(op[1]);close(ep[1]);fcntl(op[0],F_SETFL,O_NONBLOCK);fcntl(ep[0],F_SETFL,O_NONBLOCK);
    int status=0; bool done=false; char block[4096];
    while(!done) {
        struct pollfd fds[2]={{op[0],POLLIN,0},{ep[0],POLLIN,0}}; poll(fds,2,10);
        ssize_t n;size_t reads=0;while(reads++<64 && (n=read(op[0],block,sizeof(block)))>0) capture(&out,block,(size_t)n,max_bytes,&r->truncated);
        reads=0;while(reads++<64 && (n=read(ep[0],block,sizeof(block)))>0) capture(&err,block,(size_t)n,max_bytes,&r->truncated);
        pid_t got=waitpid(pid,&status,WNOHANG); if(got==pid) { done=true; break; }
        if(cancel && cancel(user)) { r->cancelled=true; break; }
        if(fg_now_ms()-start>=timeout) { r->timed_out=true; break; }
        if(got<0 && errno!=EINTR) break;
    }
    kill(-pid,SIGKILL); if(!done) while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
    ssize_t n; while((n=read(op[0],block,sizeof(block)))>0) capture(&out,block,(size_t)n,max_bytes,&r->truncated);
    while((n=read(ep[0],block,sizeof(block)))>0) capture(&err,block,(size_t)n,max_bytes,&r->truncated);
    close(op[0]);close(ep[0]); r->exit_code=WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);
#endif
    r->out_len=out.len;r->err_len=err.len;r->out=fg_buf_take(&out);r->err=fg_buf_take(&err);r->duration_ms=(double)(fg_now_ms()-start);
    if(!r->out || !r->err) { fg_process_free(r); return fg_error(e,FORGE_ERR_MEMORY,"Process output allocation failed"); }
    return FORGE_OK;
}
