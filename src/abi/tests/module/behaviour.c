/* A game module, written in C, for the reload suite. Tasks 2.4, 2.6, 2.7.
 *
 * WHY THIS FILE IS C AND NOT C++. Two reasons, and the second is the one that matters.
 *
 *   1. It is what a module is. `native-abi` requires that the engine be buildable with one compiler
 *      and a module with another, and the only way to keep that honest is for the thing on the
 *      other side of the boundary not to be C++.
 *   2. IT COMPILES cy/abi/cy_abi.h AS C, ON EVERY CONFIGURE. "The ABI SHALL consist only of C
 *      constructs" is otherwise a claim nothing checks: the header is included from C++ everywhere
 *      else in the tree, and C++ accepts a great deal of C that a C compiler does not — a `bool`
 *      without <stdbool.h>, a `static_assert` without <assert.h>, an empty parameter list meaning
 *      something different. This file is the check, and it is a build failure rather than a test.
 *
 * WHAT IT MODELS. One behaviour type, `Counter`, holding live state, serialized by name so that a
 * reload can migrate it — the blob format is the spike's: a magic word, the writer's schema, a
 * count, then (key, value) pairs. Migration is by name and never by offset, which is the whole
 * finding: v2 code reading a v1 object at fixed offsets reported health = 17 and mana = 3.5e18 with
 * no trap and no diagnostic.
 *
 * FOUR BUILDS OF THIS ONE FILE, chosen by the defines below, are declared in
 * src/abi/tests/CMakeLists.txt:
 *
 *   schema 1              `health`, `ammo`, `ticks`
 *   schema 2              `health`, `mana` (migrated from ammo / 2), `shield` (new, defaults),
 * `ticks` refusing              `cy_module_entry` returns false, which the loader must report and
 * survive renamed               schema 2 under a different type name, so a live instance has
 * nowhere to go
 */

#include <cy/abi/cy_abi.h>

#include <stdlib.h>
#include <string.h>

/*
 * Two checks, both wrong about this file rather than about the code.
 *
 *   * The first proposes C11 Annex K (`memcpy_s`, `memset_s`), which is optional, is not
 *     implemented by glibc, and would make this module fail to build on the platform it is built
 *     for. Every copy below is bounded by a length the same function computed.
 *   * The second sees `memcpy` writing a key into the blob and infers a string that is not
 *     terminated. It is not a string: the blob is length-prefixed on purpose, which is what lets a
 *     key contain any byte and what makes the format readable without a scan. The one place a
 *     C string IS built — the key buffer in `counter_deserialize` — writes its own NUL on the very
 *     next line, after a bounds check the analyser does not follow.
 *
 * Listed one per line because clang-tidy reads a NOLINT directive one line at a time: a check list
 * wrapped across two lines is not parsed at all.
 */
/* NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
/* NOLINTBEGIN(bugprone-not-null-terminated-result) */

#ifndef CY_TEST_MODULE_SCHEMA
#    define CY_TEST_MODULE_SCHEMA 1
#endif

#ifndef CY_TEST_MODULE_REFUSE
#    define CY_TEST_MODULE_REFUSE 0
#endif

#ifndef CY_TEST_MODULE_RENAMED
#    define CY_TEST_MODULE_RENAMED 0
#endif

#if CY_TEST_MODULE_RENAMED
#    define CY_TEST_MODULE_TYPE_NAME "CounterRenamed"
#else
#    define CY_TEST_MODULE_TYPE_NAME "Counter"
#endif

/* The instance. Its LAYOUT CHANGES between the two schemas on purpose: that is the case in-place
 * preservation gets wrong silently, and the case this module exists to make the loader survive. */
typedef struct Counter {
    int64_t health;
#if CY_TEST_MODULE_SCHEMA == 1
    int64_t ammo;
#else
    int64_t mana;
    int64_t shield;
#endif
    int64_t ticks;
    CyEntity entity;
} Counter;

/* The interface table this image was handed. A module reaches the engine only through it. */
static const CyInterface* g_interface = NULL;

/* The refusing build stops at `cy_module_entry` and never registers anything, so none of what
 * follows exists there. That is not an accident of the warning settings: a module that cannot
 * initialise has no behaviour to offer, and compiling one that is unreachable would only make the
 * refusing build a worse model of the case it exists to be.
 */
#if !CY_TEST_MODULE_REFUSE

/* --- The blob: magic, schema, count, then (key, value) pairs ------------------------------------
 *
 * Fixed-width and little-endian-by-memcpy, which is enough for a same-process reload and is the
 * format the generated Swift overlay will emit mechanically. Restoring reads BY NAME: a key the
 * reader does not know is skipped, and a field the blob does not carry keeps its default. That is
 * what makes adding and removing a field a migration rather than a corruption.
 */

#    define CY_TEST_BLOB_MAGIC 0x54535943u /* 'CYST' */

static uint32_t write_u32(uint8_t* buffer, uint32_t offset, uint32_t value) {
    memcpy(buffer + offset, &value, sizeof(value));
    return offset + (uint32_t)sizeof(value);
}

static uint32_t write_i64(uint8_t* buffer, uint32_t offset, int64_t value) {
    memcpy(buffer + offset, &value, sizeof(value));
    return offset + (uint32_t)sizeof(value);
}

static uint32_t read_u32(const uint8_t* buffer, uint32_t offset, uint32_t* out) {
    memcpy(out, buffer + offset, sizeof(*out));
    return offset + (uint32_t)sizeof(*out);
}

static uint32_t read_i64(const uint8_t* buffer, uint32_t offset, int64_t* out) {
    memcpy(out, buffer + offset, sizeof(*out));
    return offset + (uint32_t)sizeof(*out);
}

/* One entry costs a length, the key bytes, and the value. */
static uint32_t entry_size(const char* key) {
    return (uint32_t)sizeof(uint32_t) + (uint32_t)strlen(key) + (uint32_t)sizeof(int64_t);
}

static uint32_t write_entry(uint8_t* buffer, uint32_t offset, const char* key, int64_t value) {
    const uint32_t length = (uint32_t)strlen(key);
    offset = write_u32(buffer, offset, length);
    memcpy(buffer + offset, key, length);
    offset += length;
    return write_i64(buffer, offset, value);
}

/* --- The behaviour ------------------------------------------------------------------------------
 */

static CyInstance counter_create(CyEngine engine, CyEntity entity, void* user_data) {
    Counter* self = (Counter*)calloc(1, sizeof(Counter));
    (void)engine;
    (void)user_data;
    if (self == NULL) {
        return NULL;
    }
    self->entity = entity;
    self->health = 95;
#    if CY_TEST_MODULE_SCHEMA == 1
    self->ammo = 17;
#    else
    /* A field that did not exist in schema 1. A restored instance keeps this default, which is the
     * "new field defaults" half of a migration. */
    self->shield = 10;
    self->mana = 0;
#    endif
    return self;
}

static void counter_destroy(CyInstance instance, void* user_data) {
    (void)user_data;
    free(instance);
}

static void counter_fixed_update(CyInstance instance, float dt, void* user_data) {
    Counter* self = (Counter*)instance;
    (void)dt;
    (void)user_data;
    self->ticks += 1;
    self->health -= 1;
}

static uint32_t counter_serialize(CyInstance instance, uint8_t* buffer, uint32_t capacity,
                                  void* user_data) {
    Counter* self = (Counter*)instance;
    uint32_t required = 3u * (uint32_t)sizeof(uint32_t);
    uint32_t offset = 0;
    (void)user_data;

    required += entry_size("health");
    required += entry_size("ticks");
#    if CY_TEST_MODULE_SCHEMA == 1
    required += entry_size("ammo");
#    else
    required += entry_size("mana");
    required += entry_size("shield");
#    endif

    /* The size query: a null buffer or too small a one writes nothing and reports the requirement,
     * which is what lets the host size the blob in one extra call rather than guessing. */
    if (buffer == NULL || capacity < required) {
        return required;
    }

    offset = write_u32(buffer, offset, CY_TEST_BLOB_MAGIC);
    offset = write_u32(buffer, offset, (uint32_t)CY_TEST_MODULE_SCHEMA);
#    if CY_TEST_MODULE_SCHEMA == 1
    offset = write_u32(buffer, offset, 3u);
    offset = write_entry(buffer, offset, "health", self->health);
    offset = write_entry(buffer, offset, "ammo", self->ammo);
#    else
    offset = write_u32(buffer, offset, 4u);
    offset = write_entry(buffer, offset, "health", self->health);
    offset = write_entry(buffer, offset, "mana", self->mana);
    offset = write_entry(buffer, offset, "shield", self->shield);
#    endif
    offset = write_entry(buffer, offset, "ticks", self->ticks);
    return offset;
}

static int32_t counter_deserialize(CyInstance instance, const uint8_t* buffer, uint32_t size,
                                   uint32_t from_schema, void* user_data) {
    Counter* self = (Counter*)instance;
    uint32_t offset = 0;
    uint32_t magic = 0;
    uint32_t schema = 0;
    uint32_t count = 0;
    uint32_t index = 0;
    (void)user_data;

    if (size < 3u * (uint32_t)sizeof(uint32_t)) {
        return CY_RESULT_BUFFER_TOO_SMALL;
    }
    offset = read_u32(buffer, offset, &magic);
    offset = read_u32(buffer, offset, &schema);
    offset = read_u32(buffer, offset, &count);
    if (magic != CY_TEST_BLOB_MAGIC || schema != from_schema) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    /* THE ONE CHECK THE LOADER RELIES ON. A blob written by a schema newer than this code is not a
     * migration this code can perform, and saying so is what makes the reload be rejected with the
     * previous generation kept live rather than restored into a shape nobody wrote. */
    if (from_schema > (uint32_t)CY_TEST_MODULE_SCHEMA) {
        return CY_RESULT_SCHEMA_TOO_NEW;
    }

    for (index = 0; index < count; ++index) {
        uint32_t length = 0;
        char key[32];
        int64_t value = 0;
        offset = read_u32(buffer, offset, &length);
        if (length >= sizeof(key) || offset + length + sizeof(int64_t) > size) {
            return CY_RESULT_INVALID_ARGUMENT;
        }
        memcpy(key, buffer + offset, length);
        key[length] = '\0';
        offset += length;
        offset = read_i64(buffer, offset, &value);

        /* BY NAME. An unknown key is skipped rather than being applied to whatever field happens to
         * be next, which is what makes a removed field harmless. */
        if (strcmp(key, "health") == 0) {
            self->health = value;
        } else if (strcmp(key, "ticks") == 0) {
            self->ticks = value;
#    if CY_TEST_MODULE_SCHEMA == 1
        } else if (strcmp(key, "ammo") == 0) {
            self->ammo = value;
#    else
        } else if (strcmp(key, "mana") == 0) {
            self->mana = value;
        } else if (strcmp(key, "shield") == 0) {
            self->shield = value;
        } else if (strcmp(key, "ammo") == 0) {
            /* The migration itself: schema 1's `ammo` becomes schema 2's `mana`, halved. It is
             * expressed here, in the code that knows both shapes, and nowhere else. */
            self->mana = value / 2;
#    endif
        }
    }
    return CY_RESULT_OK;
}

/* --- Entry points -------------------------------------------------------------------------------
 */

static void module_initialize(CyEngine engine, CyInitLevel level, void* user_data) {
    CyBehaviourVTable vtable;
    (void)user_data;
    /* Types are registered at the Scene level, which is `native-abi`'s "Module registers types". */
    if (level != CY_INIT_LEVEL_SCENE || g_interface == NULL) {
        return;
    }
    memset(&vtable, 0, sizeof(vtable));
    vtable.struct_size = (uint32_t)sizeof(vtable);
    vtable.schema_version = (uint32_t)CY_TEST_MODULE_SCHEMA;
    vtable.create = &counter_create;
    vtable.destroy = &counter_destroy;
    vtable.fixed_update = &counter_fixed_update;
    vtable.serialize = &counter_serialize;
    vtable.deserialize = &counter_deserialize;
    vtable.user_data = NULL;
    (void)g_interface->register_behaviour(engine, CY_TEST_MODULE_TYPE_NAME, &vtable);
}

static void module_shutdown(CyEngine engine, CyInitLevel level, void* user_data) {
    (void)engine;
    (void)level;
    (void)user_data;
}

#endif /* !CY_TEST_MODULE_REFUSE */

bool cy_module_entry(const CyInterface* iface, CyEngine engine, CyModuleInit* out_init);

bool cy_module_entry(const CyInterface* iface, CyEngine engine, CyModuleInit* out_init) {
    (void)engine;
    if (iface == NULL || out_init == NULL) {
        return false;
    }
    /* THE GUARD, FROM THE MODULE'S SIDE. `native-abi`'s "Older engine, newer module": if the engine
     * exports a shorter table than this module was compiled against, entries this module intends to
     * call do not exist. Refusing here is how that is reported without aborting engine startup. */
    if (iface->header.abi_major != CY_ABI_MAJOR || iface->header.table_size < sizeof(CyInterface)) {
        iface->set_last_error(CY_RESULT_VERSION_MISMATCH,
                              "the engine's interface table is older than this module's");
        return false;
    }
    g_interface = iface;

#if CY_TEST_MODULE_REFUSE
    /* The refusing build. A module that cannot initialise says so and the loader reports it. */
    iface->set_last_error(CY_RESULT_UNAVAILABLE, "this module always refuses, on purpose");
    return false;
#else
    memset(out_init, 0, sizeof(*out_init));
    out_init->struct_size = (uint32_t)sizeof(*out_init);
    out_init->abi_major = CY_ABI_MAJOR;
    out_init->abi_minor = CY_ABI_MINOR;
    out_init->initialize = &module_initialize;
    out_init->shutdown = &module_shutdown;
    out_init->user_data = NULL;
    return true;
#endif
}

void cy_module_shutdown(void);

void cy_module_shutdown(void) {
    g_interface = NULL;
}

/* NOLINTEND(bugprone-not-null-terminated-result) */
/* NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
