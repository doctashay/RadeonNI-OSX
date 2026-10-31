import pathlib
import subprocess
import tempfile
import unittest


class ModernBindingTest(unittest.TestCase):
    def test_context_lifetime_and_repeated_binding(self):
        source = (pathlib.Path(__file__).resolve().parents[1] /
                  "gld/rdn_mesa.c").read_text()
        start = source.index("static int modern_make_current(void *cgl_context)")
        end = source.index("static void *modern_lookup", start)
        implementation = source[start:end]
        harness = r"""
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef void *OSMesaContext;
typedef int GLint;
typedef unsigned GLenum;
#define GL_MAJOR_VERSION 1
#define GL_MINOR_VERSION 2
#define MAX_CONTEXTS 2
struct context { void *gld_ctx, *mesa, *rend, *early_rend; int modern_version; };
static struct context contexts[MAX_CONTEXTS];
static void *current, *requested;
static int owned = 1, enabled = 1, queries, major = 4, minor = 6;
static int lookup_missing, restores;
static int rdn_hook_modern_enabled(void) { return enabled; }
static int rdn_mesa_owns_current(void) { return owned; }
static void rdn_mesa_early_all(void *ctx) { ++restores; current = ctx ? requested : NULL; }
static void *OSMesaGetCurrentContext(void) { return current; }
static void get_integer(GLenum key, GLint *value) {
    ++queries; *value = key == GL_MAJOR_VERSION ? major : minor;
}
static void *OSMesaGetProcAddress(const char *name) {
    assert(!strcmp(name, "glGetIntegerv"));
    return lookup_missing ? NULL : (void *)get_integer;
}
""" + implementation + r"""
int main(void) {
    void *engine = (void *)11, *other_engine = (void *)12;
    int i;
    contexts[0] = (struct context){(void *)1, (void *)2, engine, NULL, 0};
    contexts[1] = (struct context){(void *)3, (void *)4, other_engine, NULL, 0};
    requested = contexts[0].mesa;
    assert(modern_make_current(&engine) && queries == 2);
    for (i = 0; i < 10000; ++i) assert(modern_make_current(&engine));
    assert(queries == 2 && restores == 10001);
    current = (void *)99;
    assert(modern_make_current(&engine) && current == requested && queries == 2);
    requested = contexts[1].mesa; major = 2; minor = 1;
    assert(!modern_make_current(&other_engine) && queries == 4);
    assert(!modern_make_current(&other_engine) && queries == 4);
    memset(&contexts[1], 0, sizeof(contexts[1]));
    contexts[1] = (struct context){(void *)3, (void *)4, other_engine, NULL, 0};
    major = 4; minor = 6; lookup_missing = 1;
    assert(!modern_make_current(&other_engine) && queries == 4);
    lookup_missing = 0;
    assert(modern_make_current(&other_engine) && queries == 6);
    owned = 0;
    assert(!modern_make_current(&other_engine));
    owned = 1; requested = (void *)99;
    assert(!modern_make_current(&other_engine) && queries == 6);
    assert(modern_make_current(NULL) && current == NULL);
    enabled = 0;
    assert(!modern_make_current(&engine));
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            src = pathlib.Path(temp) / "binding.c"
            exe = pathlib.Path(temp) / "binding"
            src.write_text(harness)
            subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
