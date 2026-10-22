/*
 * Hooks on CGLSetCurrentContext and CFBundleGetFunctionPointerForName for
 * every image in the process.
 *
 * OpenGL fills a new context's dispatch table itself when the context is
 * complete, with no call to the driver, and the driver is only asked to
 * set its entries up once a drawable is attached. Programs make GL calls
 * before that (textures, display lists), so the bundle has to give the
 * table to Mesa earlier, and the one thing every program does between the
 * two is to make the context current.
 *
 * The hook replaces the pointers through which other images call
 * CGLSetCurrentContext (their lazy and non-lazy symbol pointers), in the
 * images loaded now and in every one loaded later. No code is changed.
 *
 * The second hook is for OpenGL functions that Tiger's framework does not
 * have and a program looks up by name, as World of Warcraft does for
 * GL_APPLE_flush_buffer_range's two: it asks the framework's bundle
 * (CFBundleGetFunctionPointerForName on "com.apple.opengl"), which will
 * never have them. The names the bundle answers for are its own to give
 * (rdn_hook_function_lookup); every other question goes on to Core
 * Foundation.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <dlfcn.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "rdn_glue.h"

static void branch_absolute(uint32_t *code, const void *target)
{
	uintptr_t address = (uintptr_t)target;
	code[0] = 0x3d800000 | (uint32_t)(address >> 16);
	code[1] = 0x618c0000 | (uint32_t)(address & 0xffff);
	code[2] = 0x7d8903a6;
	code[3] = 0x4e800420;
}

static void sync_code(uint32_t *code, unsigned words)
{
	unsigned i;
	for (i = 0; i < words; ++i)
		__asm__ volatile("dcbst 0,%0; sync; icbi 0,%0; sync; isync"
			: : "r"(code + i) : "memory");
}

/* These verified prologues contain no PC-relative instructions. */
static int hook_checked(void **original, void *hook, const uint32_t *expected,
			unsigned words)
{
#ifdef __LP64__
	return 0;
#else
	uint32_t *code = *original, *trampoline;
	vm_address_t memory = 0, page;

	if (!code || memcmp(code, expected, words * 4))
		return 0;
	if (vm_allocate(mach_task_self(), &memory, vm_page_size, TRUE))
		return 0;
	trampoline = (uint32_t *)memory;
	memcpy(trampoline, code, 16);
	branch_absolute(trampoline + 4, code + 4);
	sync_code(trampoline, 8);
	if (vm_protect(mach_task_self(), memory, vm_page_size, FALSE,
		VM_PROT_READ | VM_PROT_EXECUTE)) {
		vm_deallocate(mach_task_self(), memory, vm_page_size);
		return 0;
	}
	page = (vm_address_t)code & ~(vm_page_size - 1);
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
		VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY)) {
		vm_deallocate(mach_task_self(), memory, vm_page_size);
		return 0;
	}
	*original = trampoline;
	branch_absolute(code, hook);
	sync_code(code, 4);
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	return 1;
#endif
}

#define HOOKED "_CGLSetCurrentContext"
#define HOOKED_LOOKUP "_CFBundleGetFunctionPointerForName"
#define HOOKED_TEX "_CGLTexImagePBuffer"
#define ASCII 0x0600	/* kCFStringEncodingASCII */

static int (*real_set_current)(void *ctx);
static void (*after_set_current)(void *ctx);

static int hooked_set_current(void *ctx)
{
	int err = real_set_current(ctx);

	if (rdn_trace)
		rdn_log("CGLSetCurrentContext(%p) -> %d", ctx, err);
	if (!err)
		after_set_current(ctx);
	return err;
}

static void *(*real_lookup)(void *bundle, void *name);
static unsigned char (*string_to_c)(void *string, char *buffer, long size, unsigned encoding);
static void *(*own_function)(const char *name);
static void *(*real_dlsym)(void *handle, const char *name);
static void *(*bundle_identifier)(void *bundle);
static void *(*get_current_context)(void);

static void *missing_function(const char *name)
{
	return get_current_context ? rdn_mesa_lookup_missing(name, get_current_context()) : NULL;
}

static void *hooked_dlsym(void *handle, const char *name)
{
	void *result = real_dlsym(handle, name);

	if (!result && (result = missing_function(name)) != NULL)
		rdn_log("Mesa supplies missing symbol %s", name);
	return result;
}

static void *hooked_lookup(void *bundle, void *name)
{
	char text[64], identifier[64];
	void *own, *bundle_id;
	int named = name && string_to_c(name, text, sizeof(text), ASCII);

	if (named &&
	    (own = own_function(text)) != NULL)
		return own;
	own = real_lookup(bundle, name);
	if (!own && named && bundle && bundle_identifier &&
	    (bundle_id = bundle_identifier(bundle)) != NULL &&
	    string_to_c(bundle_id, identifier, sizeof(identifier), ASCII) &&
	    !strcmp(identifier, "com.apple.opengl")) {
		own = missing_function(text);
		if (own)
			rdn_log("Mesa supplies missing bundle function %s", text);
	}
	return own;
}

/*
 * CGLTexImagePBuffer goes to the engine, which wants the texture bound in
 * its own state (Mesa's glBindTexture never reaches it) and refuses the
 * rest with kCGLBadState. A pbuffer of ours is Mesa's to show.
 */
static int (*real_tex_image)(void *ctx, void *pbuffer, int source);
static int (*tex_image_handler)(void *ctx, void *pbuffer, int source, int *result);

static int hooked_tex_image(void *ctx, void *pbuffer, int source)
{
	int result;

	if (tex_image_handler(ctx, pbuffer, source, &result))
		return result;
	return real_tex_image(ctx, pbuffer, source);
}

/* CoreAnimation imports a surface ID through CGLSetParameter(999). */
static int (*real_set_parameter)(void *, int, const int *);
static int (*set_parameter_handler)(void *, int, int *, int *);

static int hooked_set_parameter(void *ctx, int pname, const int *values)
{
	int result;
	if (pname == 999 && values &&
	    set_parameter_handler(ctx, pname, (int *)values, &result))
		return result;
	return real_set_parameter(ctx, pname, values);
}

/*
 * Calls of CGL that the log (RDN_GLD_LOG) shows, with what they were given
 * and what they returned: pbuffers and pixel formats are decided before
 * any driver function is called.
 */
static int (*real_create_pbuffer)(int w, int h, int target, int format,
				   int levels, void **out);
static int (*real_set_pbuffer)(void *ctx, void *pbuffer, int face, int level,
				int screen);
static int (*real_choose)(const int *attrs, void **pix, int *npix);
static void (*prepare_pbuffer)(void *ctx, void *pbuffer);
static int (*real_cgls_create_pbuffer)(void **out, int w, int h,
	int target, int format, int levels);

static int hooked_cgls_create_pbuffer(void **out, int w, int h,
	int target, int format, int levels)
{
	long err = real_cgls_create_pbuffer(out, w, h, target, format, levels);
	if (!err && out && *out)
		rdn_mesa_prepare_pbuffer(NULL, *out);
	return err;
}

static int hooked_create_pbuffer(int w, int h, int target, int format,
				  int levels, void **out)
{
	long err = real_create_pbuffer(w, h, target, format, levels, out);

	rdn_log("CGLCreatePBuffer(%ld, %ld, target 0x%lx, format 0x%lx, levels %ld) -> %ld, %p",
		w, h, target, format, levels, err, out ? *out : NULL);
	return err;
}

static int hooked_set_pbuffer(void *ctx, void *pbuffer, int face, int level,
			       int screen)
{
	long err;
	if (prepare_pbuffer)
		prepare_pbuffer(ctx, pbuffer);
	err = real_set_pbuffer(ctx, pbuffer, face, level, screen);

	rdn_log("CGLSetPBuffer(%p, %p, face %ld, level %ld, screen %ld) -> %ld",
		ctx, pbuffer, face, level, screen, err);
	return err;
}

static int hooked_choose(const int *attrs, void **pix, int *npix)
{
	long err = real_choose(attrs, pix, npix);
	char text[400];
	int n = 0, i;

	for (i = 0; attrs && attrs[i] && i < 24 && n < (int)sizeof(text) - 12; i++)
		n += snprintf(text + n, sizeof(text) - n, " %d", attrs[i]);
	text[n] = 0;
	rdn_log("CGLChoosePixelFormat(%s) -> %ld, %ld formats", text, err,
		npix ? *npix : -1L);
	return err;
}

/* The window server's cglsTexImagePBuffer (CoreGraphics), the same way. */
static int (*real_cgls_tex_image)(void *ctx, void *pbuffer, int source);
static int (*cgls_tex_image_handler)(void *ctx, void *pbuffer, int source, int *result);

static int hooked_cgls_tex_image(void *ctx, void *pbuffer, int source)
{
	int result;

	if (cgls_tex_image_handler(ctx, pbuffer, source, &result))
		return result;
	return real_cgls_tex_image(ctx, pbuffer, source);
}

/* CGLDestroyPBuffer and the window server's cglsDestroyPBuffer. */
static int (*real_destroy_pbuffer)(void *pbuffer);
static int (*real_cgls_destroy_pbuffer)(void *pbuffer);
static void (*destroy_pbuffer_handler)(void *pbuffer);

static int hooked_destroy_pbuffer(void *pbuffer)
{
	destroy_pbuffer_handler(pbuffer);
	return real_destroy_pbuffer(pbuffer);
}

static int hooked_cgls_destroy_pbuffer(void *pbuffer)
{
	destroy_pbuffer_handler(pbuffer);
	return real_cgls_destroy_pbuffer(pbuffer);
}

/*
 * mach_msg: a thread that is about to wait for a message with no time
 * limit may be asked to wait only so long first (rdn_hook_mach_msg): `idle`
 * returns how many milliseconds, 0 for the wait to go on as it was; if
 * the time runs out `timeout` is called and then the original wait begins.
 * Any message that comes first is handed on as usual.
 */
static mach_msg_return_t (*real_mach_msg)(mach_msg_header_t *, mach_msg_option_t, mach_msg_size_t,
					  mach_msg_size_t, mach_port_t, mach_msg_timeout_t, mach_port_t);
static unsigned (*idle_handler)(void);
static pthread_t mach_thread;
static void (*idle_timeout)(void);

static mach_msg_return_t hooked_mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
					 mach_msg_size_t send_size, mach_msg_size_t rcv_size,
					 mach_port_t rcv_name, mach_msg_timeout_t timeout,
					 mach_port_t notify)
{
	if (rdn_logging && pthread_equal(pthread_self(), mach_thread)) {
		/* Debug: which kinds of wait the thread makes. */
		static unsigned seen[16][2];
		unsigned key = option & 0x3f, i;

		for (i = 0; i < 16 && seen[i][0]; i++)
			if (seen[i][0] == key + 1)
				break;
		if (i < 16 && seen[i][0] == 0) {
			seen[i][0] = key + 1;
			rdn_log("mach_msg option 0x%x timeout %u (first of its kind)", (unsigned)option, (unsigned)timeout);
		}
	}
	if ((option & MACH_RCV_MSG) && !(option & MACH_SEND_MSG)) {
		unsigned ms = idle_handler();

		if (ms && (!(option & MACH_RCV_TIMEOUT) || ms < timeout)) {
			mach_msg_return_t r = real_mach_msg(msg, option | MACH_RCV_TIMEOUT, send_size,
							    rcv_size, rcv_name, ms, notify);

			if (r != MACH_RCV_TIMED_OUT)
				return r;
			idle_timeout();
			if (option & MACH_RCV_TIMEOUT)
				timeout -= ms;
		}
	}
	return real_mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
}

#ifdef __LP64__
#define RDN_MACH_HEADER mach_header_64
#define RDN_SEGMENT segment_command_64
#define RDN_SECTION section_64
#define RDN_NLIST nlist_64
#define RDN_MAGIC MH_MAGIC_64
#define RDN_LC_SEGMENT LC_SEGMENT_64
#else
#define RDN_MACH_HEADER mach_header
#define RDN_SEGMENT segment_command
#define RDN_SECTION section
#define RDN_NLIST nlist
#define RDN_MAGIC MH_MAGIC
#define RDN_LC_SEGMENT LC_SEGMENT
#endif

static void rebind(const struct mach_header *mh, intptr_t slide)
{
	const struct RDN_MACH_HEADER *header = (const void *)mh;
	const struct load_command *lc = (const void *)(header + 1);
	const struct RDN_SEGMENT *linkedit = NULL;
	const struct symtab_command *symtab = NULL;
	const struct dysymtab_command *dysymtab = NULL;
	const struct RDN_NLIST *symbols;
	const uint32_t *indirect;
	const char *strings;
	uint32_t i, j, k;

	if (mh->magic != RDN_MAGIC)
		return;
	for (i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
		if (lc->cmd == RDN_LC_SEGMENT &&
		    !strcmp(((const struct RDN_SEGMENT *)lc)->segname, SEG_LINKEDIT))
			linkedit = (const struct RDN_SEGMENT *)lc;
		else if (lc->cmd == LC_SYMTAB)
			symtab = (const struct symtab_command *)lc;
		else if (lc->cmd == LC_DYSYMTAB)
			dysymtab = (const struct dysymtab_command *)lc;
	}
	if (!linkedit || !symtab || !dysymtab || !dysymtab->nindirectsyms)
		return;
	/* The tables are in __LINKEDIT, given by file offset. */
	symbols = (const void *)(slide + linkedit->vmaddr - linkedit->fileoff + symtab->symoff);
	strings = (const char *)(slide + linkedit->vmaddr - linkedit->fileoff + symtab->stroff);
	indirect = (const void *)(slide + linkedit->vmaddr - linkedit->fileoff +
				  dysymtab->indirectsymoff);

	lc = (const struct load_command *)(header + 1);
	for (i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
		const struct RDN_SEGMENT *seg = (const struct RDN_SEGMENT *)lc;
		const struct RDN_SECTION *sect = (const struct RDN_SECTION *)(seg + 1);

		if (lc->cmd != RDN_LC_SEGMENT)
			continue;
		for (j = 0; j < seg->nsects; j++, sect++) {
			uint32_t type = sect->flags & SECTION_TYPE;
			void **pointers = (void **)(slide + sect->addr);

			if (type != S_LAZY_SYMBOL_POINTERS &&
			    type != S_NON_LAZY_SYMBOL_POINTERS)
				continue;
			for (k = 0; k < sect->size / sizeof(void *); k++) {
				uint32_t sym = indirect[sect->reserved1 + k];
#ifdef __LP64__
				/* Cache indirect indices address a coalesced symbol table. */
				if (sym >= symtab->nsyms &&
					!(sym & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))) {
#define BIND_RESOLVED(real, hook) \
					if ((real) && pointers[k] == (void *)(real)) \
						pointers[k] = (void *)(hook)
					BIND_RESOLVED(real_set_current, hooked_set_current);
					BIND_RESOLVED(real_create_pbuffer, hooked_create_pbuffer);
					BIND_RESOLVED(real_cgls_create_pbuffer, hooked_cgls_create_pbuffer);
					BIND_RESOLVED(real_set_pbuffer, hooked_set_pbuffer);
					BIND_RESOLVED(real_choose, hooked_choose);
					BIND_RESOLVED(real_mach_msg, hooked_mach_msg);
					BIND_RESOLVED(real_destroy_pbuffer, hooked_destroy_pbuffer);
					BIND_RESOLVED(real_cgls_destroy_pbuffer, hooked_cgls_destroy_pbuffer);
					BIND_RESOLVED(real_cgls_tex_image, hooked_cgls_tex_image);
					BIND_RESOLVED(real_tex_image, hooked_tex_image);
					BIND_RESOLVED(real_set_parameter, hooked_set_parameter);
					BIND_RESOLVED(real_lookup, hooked_lookup);
					BIND_RESOLVED(real_dlsym, hooked_dlsym);
#undef BIND_RESOLVED
					continue;
				}
#endif

				if (sym & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) ||
				    sym >= symtab->nsyms)
					continue;
				if (real_set_current &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, HOOKED) &&
				    pointers[k] != (void *)hooked_set_current)
					pointers[k] = (void *)hooked_set_current;
				if (real_create_pbuffer &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_CGLCreatePBuffer") &&
				    pointers[k] != (void *)hooked_create_pbuffer)
					pointers[k] = (void *)hooked_create_pbuffer;
				if (real_cgls_create_pbuffer &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_cglsCreatePBuffer"))
					pointers[k] = (void *)hooked_cgls_create_pbuffer;
				if (real_set_pbuffer &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_CGLSetPBuffer") &&
				    pointers[k] != (void *)hooked_set_pbuffer)
					pointers[k] = (void *)hooked_set_pbuffer;
				if (real_choose &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_CGLChoosePixelFormat") &&
				    pointers[k] != (void *)hooked_choose)
					pointers[k] = (void *)hooked_choose;
				if (real_mach_msg &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_mach_msg") &&
				    pointers[k] != (void *)hooked_mach_msg)
					pointers[k] = (void *)hooked_mach_msg;
				if (real_destroy_pbuffer &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_CGLDestroyPBuffer") &&
				    pointers[k] != (void *)hooked_destroy_pbuffer)
					pointers[k] = (void *)hooked_destroy_pbuffer;
				if (real_cgls_destroy_pbuffer &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_cglsDestroyPBuffer") &&
				    pointers[k] != (void *)hooked_cgls_destroy_pbuffer)
					pointers[k] = (void *)hooked_cgls_destroy_pbuffer;
				if (real_cgls_tex_image &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_cglsTexImagePBuffer") &&
				    pointers[k] != (void *)hooked_cgls_tex_image)
					pointers[k] = (void *)hooked_cgls_tex_image;
				if (real_set_parameter &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_CGLSetParameter"))
					pointers[k] = (void *)hooked_set_parameter;
				if (real_tex_image &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, HOOKED_TEX) &&
				    pointers[k] != (void *)hooked_tex_image)
					pointers[k] = (void *)hooked_tex_image;
				if (real_lookup &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, HOOKED_LOOKUP) &&
				    pointers[k] != (void *)hooked_lookup)
					pointers[k] = (void *)hooked_lookup;
				if (real_dlsym &&
				    !strcmp(strings + symbols[sym].n_un.n_strx, "_dlsym"))
					pointers[k] = (void *)hooked_dlsym;
			}
		}
	}
}

/* dyld calls back for every image there is, and then for each new one. */
static void watch_images(void)
{
	static int watching;

	if (!watching) {
		watching = 1;
		_dyld_register_func_for_add_image(rebind);
	} else {
		uint32_t i;

		/* A hook added later: the images there are, again. */
		for (i = 0; i < _dyld_image_count(); i++)
			rebind(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
	}
}

int rdn_hook_function_lookup(void *(*own)(const char *name))
{
	if (real_lookup)
		return 1;
	string_to_c = (unsigned char (*)(void *, char *, long, unsigned))
		dlsym(RTLD_DEFAULT, "CFStringGetCString");
	own_function = own;
	if (string_to_c)
		real_lookup = (void *(*)(void *, void *))
			dlsym(RTLD_DEFAULT, "CFBundleGetFunctionPointerForName");
	if (!real_lookup)
		return 0;	/* a program without Core Foundation */
	bundle_identifier = (void *(*)(void *))dlsym(RTLD_DEFAULT, "CFBundleGetIdentifier");
	get_current_context = (void *(*)(void))dlsym(RTLD_DEFAULT, "CGLGetCurrentContext");
	real_dlsym = dlsym;
	watch_images();
	return 1;
}

void rdn_hook_set_current(void (*after)(void *cgl_ctx))
{
	if (real_set_current)
		return;
	real_set_current = (int (*)(void *))dlsym(RTLD_DEFAULT, "CGLSetCurrentContext");
	if (!real_set_current) {
		rdn_log("no CGLSetCurrentContext to hook");
		return;
	}
	after_set_current = after;
	watch_images();
}

void rdn_hook_set_parameter(int (*handler)(void *, int, int *, int *))
{
	if (real_set_parameter)
		return;
	real_set_parameter = (int (*)(void *, int, const int *))
		dlsym(RTLD_DEFAULT, "CGLSetParameter");
	if (!real_set_parameter)
		return;
	set_parameter_handler = handler;
	watch_images();
}

void rdn_hook_tex_image_pbuffer(int (*handler)(void *cgl_ctx, void *pbuffer,
					       int source, int *result))
{
	if (real_tex_image)
		return;
	real_tex_image = (int (*)(void *, void *, int))
		dlsym(RTLD_DEFAULT, "CGLTexImagePBuffer");
	if (!real_tex_image) {
		rdn_log("no CGLTexImagePBuffer to hook");
		return;
	}
	tex_image_handler = handler;
	watch_images();
}

void rdn_hook_prepare_pbuffer(void (*prepare)(void *ctx, void *pbuffer))
{
	prepare_pbuffer = prepare;
	real_set_pbuffer = (int (*)(void *, void *, int, int, int))
		dlsym(RTLD_DEFAULT, "CGLSetPBuffer");
	watch_images();
}

/* The CGL calls above go to the log. */
void rdn_hook_cgl_log(void)
{
	if (real_create_pbuffer)
		return;
	real_create_pbuffer = (int (*)(int, int, int, int, int, void **))
		dlsym(RTLD_DEFAULT, "CGLCreatePBuffer");
	real_set_pbuffer = (int (*)(void *, void *, int, int, int))
		dlsym(RTLD_DEFAULT, "CGLSetPBuffer");
	real_choose = (int (*)(const int *, void **, int *))
		dlsym(RTLD_DEFAULT, "CGLChoosePixelFormat");
	if (!real_create_pbuffer || !real_set_pbuffer || !real_choose) {
		real_create_pbuffer = NULL;
		return;
	}
	watch_images();
}

void rdn_hook_cgls_tex_image(int (*handler)(void *ctx, void *pbuffer, int source,
					    int *result))
{
	if (real_cgls_tex_image)
		return;
	real_cgls_tex_image = (int (*)(void *, void *, int))
		dlsym(RTLD_DEFAULT, "cglsTexImagePBuffer");
	if (!real_cgls_tex_image) {
		rdn_log("no cglsTexImagePBuffer to hook");
		return;
	}
	cgls_tex_image_handler = handler;
	real_cgls_create_pbuffer = (int (*)(void **, int, int, int, int, int))
		dlsym(RTLD_DEFAULT, "cglsCreatePBuffer");
	{
		static const uint32_t prologue[] = {
			0x7c0802a6, 0xbf01ffe0, 0x7c781b79, 0x7c9a2378,
			0x7cb92b78, 0x7cdb3378, 0x90010008, 0x7cfd3b78,
			0x9421ff90, 0x7d1c4378, 0x3800271e, 0x418200e4
		};
		rdn_log("Leopard cglsCreatePBuffer hook: %d", hook_checked(
			(void **)&real_cgls_create_pbuffer, hooked_cgls_create_pbuffer,
			prologue, sizeof(prologue) / sizeof(prologue[0])));
	}
	watch_images();
}

/*
 * The window server's cglsSetInteger. CoreGraphics calls it by a plain
 * branch inside itself (a filter's backdrop is a texture of the screen's
 * surface, set with parameter 0x3e6), so no symbol pointer can be
 * replaced. The function is twelve instructions long in 10.4.11:
 *
 *   mfspr r0,lr; bcl 20,31,+4; mfspr r10,lr; mtspr lr,r0      (position)
 *   addis r2,r10,0xff0; lwz r2,-0x2d28(r2); add r2,r3,r2      (offset)
 *   lwz r3,0(r3); lwz r2,0xc(r2); lwz r12,0x30(r2)
 *   mtctr r12; bctr                                            (the engine's)
 *
 * Its words are checked, and the four from "addis" on are replaced by a
 * jump to rdn_hook_cgls_set_integer's own function, which does what the
 * rest did unless the handler takes the call. A function that is not
 * these twelve words is left alone.
 */
static int (*cgls_set_handler)(void *ctx, int pname, int *vals, int *result);
static const void *cgls_set_fn;
static const int *cgls_set_offset;
static unsigned cgls_set_table_offset, cgls_set_slot;

static int hooked_cgls_set_integer(void *ctx, int pname, int *vals)
{
	int result;
	/* The offset the original reads, from where the original reads it. */
	long off = *cgls_set_offset;
	void **table;
	int (*engine)(void *rend, int pname, int *vals);

	if (!ctx || !*(void **)ctx)
		return 0x2714;
	table = *(void ***)((char *)ctx + off + cgls_set_table_offset);
	if (!table)
		return 0x2714;
	engine = (int (*)(void *, int, int *))table[cgls_set_slot];

	if (cgls_set_handler(ctx, pname, vals, &result))
		return result;
	return engine(*(void **)ctx, pname, vals);
}

int rdn_hook_cgls_set_integer(int (*handler)(void *ctx, int pname, int *vals, int *result))
{
	static const uint32_t original[12] = {
		0x7c0802a6, 0x429f0005, 0x7d4802a6, 0x7c0803a6,
		0x3c4a0ff0, 0x8042d2d8, 0x7c431214, 0x80630000,
		0x8042000c, 0x81820030, 0x7d8903a6, 0x4e800420
	};
	static const uint32_t leopard[24] = {
		0x7c0802a6, 0x2c030000, 0x429f0005, 0x7d4802a6,
		0x7c0803a6, 0x41820044, 0x81230000, 0x2f890000,
		0x419e0038, 0x3c4a004f, 0x80422354, 0x7c631214,
		0x2f83fffc, 0x419e0024, 0x80630004, 0x2f830000,
		0x419e0018, 0x8003002c, 0x7d234b78, 0x7c0903a6,
		0x7c0c0378, 0x4e800420, 0x38602714, 0x4e800020
	};
	volatile uint32_t *code;
	uintptr_t target = (uintptr_t)hooked_cgls_set_integer;
	uintptr_t page;
	kern_return_t kr;
	unsigned i;
	int is_leopard = 1, is_tiger = 1;
	unsigned high_word, low_word, pc_offset;

	if (cgls_set_fn)
		return 1;
	code = (volatile uint32_t *)dlsym(RTLD_DEFAULT, "cglsSetInteger");
	if (!code) {
		rdn_log("no cglsSetInteger to hook");
		return 0;
	}
	for (i = 0; i < 24; ++i) {
		uint32_t mask = (i == 9 || i == 10) ? 0xffff0000 : 0xffffffff;
		if ((code[i] & mask) != (leopard[i] & mask))
			is_leopard = 0;
	}
	for (i = 0; i < 12; ++i) {
		uint32_t mask = (i == 4 || i == 5) ? 0xffff0000 : 0xffffffff;
		if ((code[i] & mask) != (original[i] & mask))
			is_tiger = 0;
	}
	if (!is_leopard && !is_tiger) {
		rdn_log("cglsSetInteger does not match a supported implementation");
		return 0;
	}
	page = (uintptr_t)code & ~(uintptr_t)(vm_page_size - 1);
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
			VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS)
		/* A mapping whose maximum protection lacks writing: copy the page. */
		kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
				VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		rdn_log("cglsSetInteger's code cannot be written (vm_protect %d at 0x%lx)",
			(int)kr, (unsigned long)page);
		return 0;
	}
	cgls_set_handler = handler;
	cgls_set_fn = (const void *)code;
	high_word = is_leopard ? 9 : 4;
	low_word = is_leopard ? 10 : 5;
	pc_offset = is_leopard ? 12 : 8;
	/* The shared cache relocates these two immediate operands. */
	cgls_set_offset = (const int *)((const char *)code + pc_offset +
		(int32_t)(code[high_word] << 16) + (int16_t)code[low_word]);
	cgls_set_table_offset = is_leopard ? 4 : 12;
	cgls_set_slot = is_leopard ? 11 : 12;
	code[0] = 0x3d800000 | (uint32_t)(target >> 16);		/* lis r12,hi */
	code[1] = 0x618c0000 | (uint32_t)(target & 0xffff);		/* ori r12,r12,lo */
	code[2] = 0x7d8903a6;					/* mtctr r12 */
	code[3] = 0x4e800420;					/* bctr */
	for (i = 0; i < 4; i++) {
		__asm__ volatile("dcbst 0,%0; sync; icbi 0,%0; sync; isync" : : "r"(code + i) : "memory");
	}
	vm_protect(mach_task_self(), page, vm_page_size, FALSE,
		   VM_PROT_READ | VM_PROT_EXECUTE);
	rdn_log("cglsSetInteger is ours (jump at %p)", (const void *)code);
	return 1;
}

void rdn_hook_destroy_pbuffer(void (*handler)(void *pbuffer))
{
	if (destroy_pbuffer_handler)
		return;
	real_destroy_pbuffer = (int (*)(void *))dlsym(RTLD_DEFAULT, "CGLDestroyPBuffer");
	real_cgls_destroy_pbuffer = (int (*)(void *))dlsym(RTLD_DEFAULT, "cglsDestroyPBuffer");
	if (!strcmp(getprogname(), "WindowServer")) {
		static const uint32_t prologue[] = {
			0x7c0802a6, 0xbfc1fff8, 0x7c7e1b79, 0x3860271e,
			0x90010008, 0x9421ffb0, 0x41820040, 0x809e0008
		};
		rdn_log("Leopard cglsDestroyPBuffer hook: %d", hook_checked(
			(void **)&real_cgls_destroy_pbuffer, hooked_cgls_destroy_pbuffer,
			prologue, sizeof(prologue) / sizeof(prologue[0])));
	}
	if (!real_destroy_pbuffer && !real_cgls_destroy_pbuffer)
		return;
	destroy_pbuffer_handler = handler;
	watch_images();
}

void rdn_hook_mach_msg(unsigned (*idle)(void), void (*timeout)(void))
{
	if (real_mach_msg)
		return;
	real_mach_msg = (mach_msg_return_t (*)(mach_msg_header_t *, mach_msg_option_t, mach_msg_size_t,
					       mach_msg_size_t, mach_port_t, mach_msg_timeout_t,
					       mach_port_t))dlsym(RTLD_DEFAULT, "mach_msg");
	if (!real_mach_msg)
		return;
	idle_handler = idle;
	idle_timeout = timeout;
	mach_thread = pthread_self();
	watch_images();
}
