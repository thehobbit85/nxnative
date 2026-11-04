// Native instruction replay; code and table contents are synthetic test data.
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint32_t (*lookup)(uint32_t, const uint8_t *);
static lookup load_code(const char *path) {
    size_t size=0x30000;
    uint8_t *code=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(code!=MAP_FAILED);
    FILE *file=fopen(path,"rb"); assert(file);
    assert(fread(code,1,size,file)==size); fclose(file);
    __builtin___clear_cache((char *)code,(char *)code+size);
    assert(!mprotect(code,size,PROT_READ|PROT_EXEC));
    return (lookup)code;
}
int main(int argc,char **argv) {
    assert(argc==3);
    struct rlimit no_core={0,0}; assert(!setrlimit(RLIMIT_CORE,&no_core));
    lookup original=load_code(argv[1]), fixed=load_code(argv[2]);
    // Reserve the invalid index's whole address range so a neighboring allocation
    // cannot accidentally make the original out-of-bounds read succeed.
    uint8_t *table=mmap(NULL,6<<20,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(table!=MAP_FAILED); assert(!mprotect(table,(size_t)getpagesize(),PROT_READ|PROT_WRITE));
    for (unsigned i=0;i<40;i++) table[88*i+0x49]=(uint8_t)(i*7+2);
    pid_t child=fork(); assert(child>=0);
    if (!child) { original(0xffff,table); _exit(1); }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && (WTERMSIG(status)==SIGSEGV || WTERMSIG(status)==SIGBUS));
    assert(fixed(0xffff,table)==3);
    for (unsigned i=0;i<40;i++) {
        assert(original(i,table)==table[88*i+0x49]);
        assert(fixed(i,table)==original(i,table));
    }
    puts("original sentinel faults; guarded sentinel = 3; 40 valid priorities unchanged");
}
