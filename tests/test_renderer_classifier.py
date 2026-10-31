"""Keep public model names while satisfying Leopard Core Image classification."""
from pathlib import Path
import re, subprocess, tempfile
source=(Path(__file__).resolve().parents[1]/'gld/gen_dispatch.py').read_text()
function=re.search(r'static const char \*renderer_name\(void\)\n\{.*?\n\}',source,re.S).group()
harness=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static int rdn_window_server, disabled;
static const char *model;
static int rdn_ws_no_core_image(void) { return disabled; }
static const char *rdn_context_renderer_name(void) { return model; }
'''
cases=r'''
int main(void) {
 unsetenv("RDN_RENDERER"); model="ATI FirePro V7900";
 assert(!strcmp(renderer_name(),model));
 rdn_window_server=1;
 assert(!strcmp(renderer_name(),"ATI Radeon (FirePro V7900)"));
 model="ATI Radeon HD 7570";assert(!strcmp(renderer_name(),model));
 disabled=1;assert(!renderer_name());
 setenv("RDN_RENDERER","Explicit",1);assert(!strcmp(renderer_name(),"Explicit"));
 puts("PASS model identity and Leopard Core Image family classification");
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-quartz-name-') as d:
 p=Path(d);(p/'test.c').write_text(harness+function+cases)
 subprocess.run(['cc','-std=gnu99','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
