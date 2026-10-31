/* Diagnostic legacy-context migration. Registry entries are process-local. */
struct migration_command {
    struct migration_command *next;
    int (*replay)(void *(*)(const char *), const void *);
    unsigned char arguments[];
};
struct migration_list {
    struct migration_list *next;
    void *group;
    unsigned name;
    uint64_t revision, resident[4];
    int complete;
    struct migration_command *first, *last;
};
struct migration_owner {
    struct migration_owner *next;
    void *engine;
    unsigned gpu;
    char renderer[128];
};
struct migration_registry {
    pthread_mutex_t lock;
    struct migration_list *lists;
    struct migration_owner *owners;
    uint64_t revision;
};
void *RDNGLMigrationRegistryV1(void);
__attribute__((visibility("default")))
void *RDNGLMigrationRegistryV1(void)
{
    static struct migration_registry registry = { PTHREAD_MUTEX_INITIALIZER };
    return &registry;
}
static struct migration_registry *migration_registry(void)
{
    static struct migration_registry *registry;
    if (registry) return registry;
    for (unsigned i = 0; i < _dyld_image_count(); ++i) {
        const char *path = _dyld_get_image_name(i);
        if (!path || !strstr(path, "/RadeonNIGLDriver")) continue;
        void *module = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
        void *(*get)(void) = module ? dlsym(module, "RDNGLMigrationRegistryV1") : NULL;
        if (get) { registry = get(); return registry; }
        if (module) dlclose(module);
    }
    return NULL;
}
static __thread struct migration_list *recording_list;
int rdn_list_recording(void) { return recording_list != NULL; }
void rdn_list_begin(unsigned name, unsigned mode)
{
    (void)mode;
    if (rdn_window_server || !migration_textures() || recording_list || !name) return;
    struct context *c = texture_context();
    if (!c) return;
    recording_list = calloc(1, sizeof(*recording_list));
    if (!recording_list) return;
    recording_list->group = texture_group(c);
    recording_list->name = name;
    recording_list->complete = 1;
}
void rdn_list_record(int (*replay)(void *(*)(const char *), const void *),
                     const void *arguments, unsigned size)
{
    if (!recording_list || !recording_list->complete) return;
    if (!replay) { recording_list->complete = 0; return; }
    struct migration_command *cmd = malloc(sizeof(*cmd) + size);
    if (!cmd) { recording_list->complete = 0; return; }
    cmd->next = NULL; cmd->replay = replay;
    if (size) memcpy(cmd->arguments, arguments, size);
    if (recording_list->last) recording_list->last->next = cmd;
    else recording_list->first = cmd;
    recording_list->last = cmd;
}
struct migration_bitmap {
    int width, height;
    float xorig, yorig, xmove, ymove;
    unsigned char pixels[];
};
static int replay_bitmap(void *(*lookup)(const char *), const void *data)
{
    const struct migration_bitmap *b = data;
    void (*get)(GLenum,GLint*) = lookup("glGetIntegerv");
    void (*store)(GLenum,GLint) = lookup("glPixelStorei");
    void (*bind)(GLenum,GLuint) = lookup("glBindBuffer");
    void (*bitmap)(GLsizei,GLsizei,GLfloat,GLfloat,GLfloat,GLfloat,const GLubyte*) = lookup("glBitmap");
    if (!get || !store || !bind || !bitmap) return 0;
    const GLenum keys[] = {GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_LSB_FIRST};
    GLint old[5], pbo;
    get(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo); bind(GL_PIXEL_UNPACK_BUFFER, 0);
    for (unsigned i = 0; i < 5; ++i) { get(keys[i], &old[i]); store(keys[i], i ? 0 : 1); }
    bitmap(b->width,b->height,b->xorig,b->yorig,b->xmove,b->ymove,b->pixels);
    for (unsigned i = 0; i < 5; ++i) store(keys[i], old[i]);
    bind(GL_PIXEL_UNPACK_BUFFER, pbo);
    return 1;
}
void rdn_list_bitmap(int width, int height, float xorig, float yorig,
                     float xmove, float ymove, const unsigned char *bitmap)
{
    if (!recording_list) return;
    void (*get)(GLenum,GLint*) = OSMesaGetProcAddress("glGetIntegerv");
    GLint align, row, skip_rows, skip_pixels, lsb, pbo;
    get(GL_UNPACK_ALIGNMENT,&align);get(GL_UNPACK_ROW_LENGTH,&row);
    get(GL_UNPACK_SKIP_ROWS,&skip_rows);get(GL_UNPACK_SKIP_PIXELS,&skip_pixels);
    get(GL_UNPACK_LSB_FIRST,&lsb);get(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);
    if (pbo || width < 0 || height < 0 || width > 16384 || height > 16384 ||
        (width && height && !bitmap)) { rdn_list_record(NULL,NULL,0); return; }
    size_t stride = (((size_t)(row ? row : width) + 7) / 8 + align - 1) & ~(size_t)(align - 1);
    size_t packed = ((size_t)width + 7) / 8;
    size_t size = sizeof(struct migration_bitmap) + packed * height;
    struct migration_bitmap *b = calloc(1,size);
    if (!b) { rdn_list_record(NULL,NULL,0); return; }
    b->width=width;b->height=height;b->xorig=xorig;b->yorig=yorig;b->xmove=xmove;b->ymove=ymove;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            size_t bit = (size_t)skip_pixels + x;
            unsigned value = bitmap[((size_t)skip_rows+y)*stride + bit/8];
            if (value & (1u << (lsb ? bit%8 : 7-bit%8)))
                b->pixels[y*packed+x/8] |= 1u << (7-x%8);
        }
    rdn_list_record(replay_bitmap,b,size);
    free(b);
}

static void free_migration_list(struct migration_list *list)
{
    struct migration_command *cmd = list->first;
    while (cmd) { struct migration_command *next = cmd->next; free(cmd); cmd = next; }
    free(list);
}
void rdn_lists_deleted(unsigned first, int count)
{
    if (rdn_window_server || !migration_textures() || count <= 0) return;
    struct migration_registry *r = migration_registry();
    void *group = texture_group(texture_context());
    if (!r || !group) return;
    pthread_mutex_lock(&r->lock);
    for (struct migration_list *l = r->lists; l; l = l->next) {
        if (l->group != group || l->name < first || l->name - first >= (unsigned)count) continue;
        struct migration_command *cmd = l->first;
        while (cmd) { struct migration_command *next = cmd->next; free(cmd); cmd = next; }
        l->first = l->last = NULL;
        l->complete = 2;
        l->revision = ++r->revision;
        l->resident[RDN_GPU_INDEX] = l->revision;
    }
    pthread_mutex_unlock(&r->lock);
}

void rdn_list_end(void)
{
    if (!recording_list) return;
    struct migration_registry *r = migration_registry();
    struct migration_list *list = recording_list;
    recording_list = NULL;
    if (!r) { free_migration_list(list); return; }
    pthread_mutex_lock(&r->lock);
    struct migration_list **link = &r->lists;
    while (*link && ((*link)->group != list->group || (*link)->name != list->name)) link = &(*link)->next;
    if (*link) { struct migration_list *old = *link; *link = old->next; free_migration_list(old); }
    list->revision = ++r->revision;
    list->resident[RDN_GPU_INDEX] = list->revision;
    list->next = r->lists; r->lists = list;
    pthread_mutex_unlock(&r->lock);
}
void *RDNGLMigrationContextV1(void *engine);
__attribute__((visibility("default")))
void *RDNGLMigrationContextV1(void *engine)
{
    for (unsigned i = 0; i < MAX_CONTEXTS; ++i)
        if (contexts[i].mesa && contexts[i].early_rend == engine) return contexts[i].mesa;
    return NULL;
}
static void *migration_lookup(const char *name) { return (void *)OSMesaGetProcAddress(name); }
static void sync_legacy_context(struct context *c)
{
    if (rdn_window_server || !migration_textures() || recording_list) return;
    struct migration_registry *r = migration_registry();
    void *engine = c->early_rend ? c->early_rend : c->rend;
    void *group = texture_group(c);
    if (!r || !engine || !group) return;
    pthread_mutex_lock(&r->lock);
    void (*begin)(GLuint,GLenum) = OSMesaGetProcAddress("glNewList");
    void (*end)(void) = OSMesaGetProcAddress("glEndList");
    for (struct migration_list *l = r->lists; l; l = l->next) {
        if (l->group != group || !l->complete || l->resident[RDN_GPU_INDEX] == l->revision) continue;
        if (l->complete == 2) {
            void (*destroy)(GLuint,GLsizei) = OSMesaGetProcAddress("glDeleteLists");
            destroy(l->name, 1);
            l->resident[RDN_GPU_INDEX] = l->revision;
            continue;
        }
        begin(l->name, GL_COMPILE);
        int ok = 1;
        for (struct migration_command *cmd = l->first; cmd; cmd = cmd->next)
            if (!cmd->replay(migration_lookup, cmd->arguments)) { ok = 0; break; }
        end();
        if (ok) l->resident[RDN_GPU_INDEX] = l->revision;
    }
    struct migration_owner *owner;
    for (owner = r->owners; owner && owner->engine != engine; owner = owner->next) {}
    if (!owner) {
        owner = calloc(1, sizeof(*owner));
        if (owner) { owner->engine = engine; owner->gpu = RDN_GPU_INDEX; owner->next = r->owners; r->owners = owner;
            snprintf(owner->renderer, sizeof(owner->renderer), "%s", rdn_mesa_device_model() ? rdn_mesa_device_model() : "RadeonNI"); }
    } else if (owner->gpu != RDN_GPU_INDEX) {
        void *module = foreign_module(owner->gpu);
        void *(*get)(void *) = module ? dlsym(module, "RDNGLMigrationContextV1") : NULL;
        OSMesaContext source = get ? get(engine) : NULL;
        if (source && OSMesaMigrateLegacyState(c->mesa, source)) {
            fprintf(stderr, "RadeonNI: context %p migrated GPU %u -> %u\n", engine, owner->gpu, RDN_GPU_INDEX);
            owner->gpu = RDN_GPU_INDEX;
            snprintf(owner->renderer, sizeof(owner->renderer), "%s", rdn_mesa_device_model() ? rdn_mesa_device_model() : "RadeonNI");
        }
    }
    pthread_mutex_unlock(&r->lock);
}

const char *rdn_context_renderer_name(void)
{
    const char *model = rdn_mesa_device_model();
    if (rdn_window_server || !migration_textures()) return model;
    struct context *c = texture_context();
    if (!c) return model;
    void *engine = c->early_rend ? c->early_rend : c->rend;
    struct migration_registry *r = migration_registry();
    if (!r) return model;
    pthread_mutex_lock(&r->lock);
    for (struct migration_owner *owner = r->owners; owner; owner = owner->next)
        if (owner->engine == engine) { model = owner->renderer; break; }
    pthread_mutex_unlock(&r->lock);
    return model;
}
