#include <CoreFoundation/CoreFoundation.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	CGLPixelFormatAttribute attrs[] = { kCGLPFARendererID, 0x21a00, 0 };
	CGLPixelFormatObj format;
	CGLContextObj context, other;
	GLint count;
	GLuint buffer;
	unsigned char expected[256], actual[256], *mapped;
	void (*storage)(GLenum, long, const void *, GLbitfield);
	void *(*map)(GLenum, long, long, GLbitfield);
	CFBundleRef bundle;
	void *library, *bundle_storage;
	void *(*fence)(GLenum, GLbitfield);
	GLboolean (*is_sync)(void *);
	void (*delete_sync)(void *);
	void *sync;
	unsigned i;
	int failed = 0;

	if (CGLChoosePixelFormat(attrs, &format, &count) ||
	    CGLCreateContext(format, NULL, &context) || CGLSetCurrentContext(context))
		return 2;
	if (CGLCreateContext(format, NULL, &other)) return 2;
	CGLDestroyPixelFormat(format);
	library = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
	storage = dlsym(library, "glBufferStorage");
	map = dlsym(library, "glMapBufferRange");
	bundle = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.opengl"));
	bundle_storage = CFBundleGetFunctionPointerForName(bundle, CFSTR("glBufferStorage"));
	if (!storage || !map || !bundle_storage ||
	    dlsym(library, "glRadeonNINonexistentFunction")) {
		printf("entrypoint lookup failed: storage=%p map=%p bundle=%p\n",
		       storage, map, bundle_storage);
		return 1;
	}
	for (i = 0; i < sizeof(expected); i++)
		expected[i] = (unsigned char)(i ^ 0x5a);
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	storage(GL_ARRAY_BUFFER, sizeof(expected), NULL, 0xc2);
	mapped = map(GL_ARRAY_BUFFER, 0, sizeof(expected), 0xc2);
	if (!mapped) {
		printf("persistent mapping failed: 0x%x\n", glGetError());
		return 1;
	}
	memcpy(mapped, expected, sizeof(expected));
	glFinish();
	glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(actual), actual);
	failed += memcmp(expected, actual, sizeof(actual)) != 0;
	failed += glGetError() != GL_NO_ERROR;
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glDeleteBuffers(1, &buffer);
	fence = dlsym(library, "glFenceSync");
	is_sync = dlsym(library, "glIsSync");
	delete_sync = dlsym(library, "glDeleteSync");
	if (!fence || !is_sync || !delete_sync) return 1;
	sync = fence(0x9117, 0);
	failed += !is_sync(sync);
	CGLSetCurrentContext(other);
	failed += is_sync(sync) != 0;
	CGLSetCurrentContext(context);
	failed += !is_sync(sync);
	delete_sync(sync);
	CGLSetCurrentContext(NULL);
	failed += dlsym(library, "glBufferStorage") != NULL;
	CGLDestroyContext(other);
	CGLDestroyContext(context);
	printf("modern entrypoints and persistent buffer: %d failed checks\n", failed);
	return failed != 0;
}
