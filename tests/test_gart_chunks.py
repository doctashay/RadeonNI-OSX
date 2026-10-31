"""Exercise the Darwin GART chunk allocator with bounded VM/bind stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'mesa/target/rdn_device_darwin.c').read_text()
start = source.index('static struct gart_chunk *gart_new_chunk(')
end = source.index('\n/*', start)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define GART_CHUNK_BYTES (16u<<20)
#define GART_MOST_BYTES (512u<<20)
#define GART_CHUNKS 8
#define VM_FLAGS_ANYWHERE 1
#define RDN_UC_GART_UNBIND 0
#define mach_task_self() 0
typedef uintptr_t vm_address_t;
typedef uint32_t vm_size_t;
struct mem { uint64_t bytes; };
struct gart_chunk { uint8_t *cpu; uint32_t size,offset; struct mem mem; };
struct darwin_device { struct gart_chunk gart[GART_CHUNKS]; uint32_t gart_bytes; int conn,mem_os; };
static unsigned allocs;
static int vm_allocate(int task,vm_address_t *a,vm_size_t b,int flags) { *a=0x10000+4096*++allocs;return 0; }
static int bind_address(struct darwin_device *d,uintptr_t a,uint32_t bytes,int *offset) { *offset=4096;return 0; }
static int rdn_mem_init(struct mem *m,int *os,uint32_t o,uint32_t b) { m->bytes=b;return 0; }
static uint64_t rdn_mem_available(struct mem *m) { return m->bytes; }
static int IOConnectMethodScalarIScalarO(int c,int s,int i,int o,int offset) { return 0; }
static void vm_deallocate(int task,vm_address_t a,vm_size_t b) {}
'''
cases = r'''
int main(void) {
 struct darwin_device d={0};struct gart_chunk *c;
 unsetenv("RDN_STATS");
 c=gart_new_chunk(&d,256u<<20,4096);assert(c&&c->size==(256u<<20));
 c=gart_new_chunk(&d,256u<<20,4096);assert(c&&c->size==(256u<<20));
 assert(d.gart_bytes==GART_MOST_BYTES&&allocs==2);
 assert(!gart_new_chunk(&d,4096,4096)&&allocs==2);
 d=(struct darwin_device){0};
 c=gart_new_chunk(&d,256u<<20,65536);assert(c&&c->size==(256u<<20)+61440);
 d=(struct darwin_device){0};
 c=gart_new_chunk(&d,16u<<20,32u<<20);assert(c&&c->size==(48u<<20)-4096);
 d=(struct darwin_device){0};
 c=gart_new_chunk(&d,4097,256);assert(c&&c->size==GART_CHUNK_BYTES);
 assert(!gart_new_chunk(&d,UINT64_MAX-1024,8192));
 assert(!gart_new_chunk(&d,(uint64_t)GART_MOST_BYTES+1,4096));
 puts("PASS exact GART budget, page/subpage/large alignment and overflow rejection");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-gart-chunks-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness + source[start:end] + cases)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=gnu99', '-Wall', '-Werror'] + shlex.split(os.environ.get('CFLAGS', '')) + ['-o', str(path / 'test'), str(path / 'test.c')], check=True)
    subprocess.run(shlex.split(os.environ.get('TEST_RUNNER', '')) + [str(path / 'test')], check=True)
