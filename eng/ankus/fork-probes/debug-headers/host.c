// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* These layouts read version 5 of the public Native AOT debugger contract. */
typedef struct
{
    const char *type_name;
    const char *field_name;
    uint32_t field_offset;
    uint32_t padding;
} debug_type_entry;

typedef struct
{
    const char *name;
    const void *address;
} global_entry;

typedef struct
{
    uint8_t cookie[4];
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t flags;
    uint32_t reserved;
    const debug_type_entry (*debug_types)[100];
    const global_entry (*globals)[8];
} debug_header;

typedef int (*initialize_fn)(void);

typedef struct
{
    void *handle;
    const debug_header *header;
    debug_header saved_header;
    debug_type_entry saved_types[100];
    global_entry saved_globals[8];
    const void *module_base;
    const void *runtime;
    const void *gc;
    initialize_fn initialize;
} image_state;

/* Fail immediately so an uninitialized or corrupted descriptor is never followed. */
static void require(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

/* Resolve through the individual module handle, as a module-aware debugger does. */
static void *symbol(void *handle, const char *name)
{
    dlerror();
    void *address = dlsym(handle, name);
    const char *error = dlerror();
    if (error != NULL)
    {
        fprintf(stderr, "dlsym(%s): %s\n", name, error);
    }

    require(address != NULL && error == NULL, "public symbol lookup failed");
    return address;
}

/* Array storage and literal names must belong to this image, not its neighbour. */
static void require_owner(const void *address, const void *expected_base)
{
    Dl_info info;
    require(dladdr(address, &info) != 0, "descriptor address has no owning module");
    require(info.dli_fbase == expected_base, "descriptor address belongs to another image");
}

/* Check a full exported descriptor and retain its exact bytes before the next load. */
static image_state load_image(const char *path)
{
    image_state image = {0};
    image.handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (image.handle == NULL)
    {
        fprintf(stderr, "dlopen(%s): %s\n", path, dlerror());
    }

    require(image.handle != NULL, "image load failed");
    image.initialize = (initialize_fn)symbol(image.handle, "debug_header_probe_initialize");
    require(image.initialize() == getpid(), "managed initialization or collection failed");
    image.header = symbol(image.handle, "DotNetRuntimeDebugHeader");
    Dl_info info;
    require(dladdr((void *)image.initialize, &info) != 0, "managed export has no module");
    image.module_base = info.dli_fbase;
    require_owner(image.header, image.module_base);
    require(memcmp(image.header->cookie, "DNDH", 4) == 0, "debugger cookie is invalid");
    require(image.header->major_version == 5 && image.header->minor_version == 0,
            "debugger contract version changed; update the reader");
    require(image.header->flags == 1 && sizeof(void *) == 8, "expected the 64-bit little-endian contract");
    require(image.header->reserved == 0, "reserved header bytes changed");
    require(image.header->debug_types != NULL && image.header->globals != NULL,
            "this image's debugger descriptor was not initialized");
    require_owner(image.header->debug_types, image.module_base);
    require_owner(image.header->globals, image.module_base);
    size_t types = 0;
    for (; types < 100 && (*image.header->debug_types)[types].type_name != NULL; ++types)
    {
        const debug_type_entry *entry = &(*image.header->debug_types)[types];
        require(entry->field_name != NULL, "type entry lacks a field name");
        require_owner(entry->type_name, image.module_base);
        require_owner(entry->field_name, image.module_base);
    }

    require(types > 0 && types < 100, "debug type entries lack contents or a terminator");
    size_t globals = 0;
    int found_base = 0;
    for (; globals < 8 && (*image.header->globals)[globals].name != NULL; ++globals)
    {
        const global_entry *entry = &(*image.header->globals)[globals];
        require_owner(entry->name, image.module_base);
        require(entry->address != NULL, "runtime global has a null address");
        if (strcmp(entry->name, "moduleBaseAddress") == 0)
        {
            require(entry->address == image.module_base, "debugger sees the wrong module base");
            found_base = 1;
        }
        else if (strcmp(entry->name, "g_pTheRuntimeInstance") == 0)
        {
            image.runtime = entry->address;
        }
        else if (strcmp(entry->name, "g_gcDacGlobals") == 0)
        {
            image.gc = entry->address;
            require_owner(image.gc, image.module_base);
        }
    }

    require(globals == 6 && found_base && image.runtime != NULL && image.gc != NULL,
            "debugger global table is incomplete");
    image.saved_header = *image.header;
    memcpy(image.saved_types, image.header->debug_types, sizeof(image.saved_types));
    memcpy(image.saved_globals, image.header->globals, sizeof(image.saved_globals));
    printf("IMAGE path=%s base=%p header=%p runtime=%p gc=%p types=%zu globals=%zu\n",
           path, image.module_base, (const void *)image.header, image.runtime, image.gc, types, globals);
    return image;
}

/* Loading and executing a neighbour must preserve the first descriptor exactly. */
static void require_unchanged(const image_state *image)
{
    require(memcmp(image->header, &image->saved_header, sizeof(image->saved_header)) == 0,
            "another image overwrote this debugger header");
    require(memcmp(image->header->debug_types, image->saved_types, sizeof(image->saved_types)) == 0,
            "another image overwrote this debugger type table");
    require(memcmp(image->header->globals, image->saved_globals, sizeof(image->saved_globals)) == 0,
            "another image overwrote this debugger global table");
}

/* Run each order in a fresh process; Native AOT libraries remain loaded for its life. */
int main(int argc, char **argv)
{
    require(argc == 3, "usage: host first-image second-image");
    alarm(30);
    image_state first = load_image(argv[1]);
    image_state second = load_image(argv[2]);
    require(first.module_base != second.module_base && first.header != second.header,
            "the loader reused the same image");
    require(first.runtime != second.runtime && first.gc != second.gc,
            "the images share runtime or GC descriptors");
    require_unchanged(&first);
    require(first.initialize() == getpid() && second.initialize() == getpid(),
            "managed reentry or collection failed");
    require_unchanged(&first);
    require_unchanged(&second);
    puts("PASS independent-debug-headers");
    return 0;
}
