#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <amdgpu_drm.h>

static int (*next_ioctl)(int, unsigned long, ...);
static int (*next_drm_ioctl)(int, unsigned long, void *);
static FILE *output;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned depth;
static uint64_t submits;
static uint64_t limit = 256;

__attribute__((constructor)) static void initialize(void)
{
    next_ioctl = dlsym(RTLD_NEXT, "ioctl");
    next_drm_ioctl = dlsym(RTLD_NEXT, "drmIoctl");
    const char *path = getenv("WSI_REPRO_BO_TRACE");
    if (path) {
        output = fopen(path, "ae");
        if (output) setvbuf(output, NULL, _IOLBF, 0);
    }
    const char *configured = getenv("WSI_REPRO_BO_TRACE_SUBMITS");
    if (configured) limit = strtoull(configured, NULL, 10);
}

static uint64_t clock_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static int is_command(unsigned long request, unsigned number)
{
    return _IOC_TYPE(request) == DRM_IOCTL_BASE && _IOC_NR(request) == DRM_COMMAND_BASE + number;
}

static void log_submit(int fd, const struct drm_amdgpu_cs_in *input)
{
    pthread_mutex_lock(&lock);
    uint64_t index = submits++;
    if (index >= limit) {
        if (index == limit) fprintf(output, "{\"kind\":\"submit_limit\",\"limit\":%"PRIu64"}\n", limit);
        pthread_mutex_unlock(&lock);
        return;
    }
    fprintf(output, "{\"kind\":\"submit\",\"ns\":%"PRIu64",\"index\":%"PRIu64",\"pid\":%d,\"fd\":%d,\"context\":%u,\"legacy_bo_list\":%u,\"bo_handles\":[",
            clock_ns(), index, getpid(), fd, input->ctx_id, input->bo_list_handle);
    const uint64_t *addresses = (const void *)(uintptr_t)input->chunks;
    int comma = 0;
    for (uint32_t i = 0; i < input->num_chunks; ++i) {
        const struct drm_amdgpu_cs_chunk *chunk = (const void *)(uintptr_t)addresses[i];
        if (chunk->chunk_id != AMDGPU_CHUNK_ID_BO_HANDLES || chunk->length_dw * 4 < sizeof(struct drm_amdgpu_bo_list_in)) continue;
        const struct drm_amdgpu_bo_list_in *list = (const void *)(uintptr_t)chunk->chunk_data;
        if (list->bo_info_size < sizeof(struct drm_amdgpu_bo_list_entry)) continue;
        for (uint32_t j = 0; j < list->bo_number; ++j) {
            const struct drm_amdgpu_bo_list_entry *entry = (const void *)((uintptr_t)list->bo_info_ptr + (uintptr_t)j * list->bo_info_size);
            fprintf(output, "%s%u", comma ? "," : "", entry->bo_handle);
            comma = 1;
        }
    }
    fprintf(output, "]}\n");
    pthread_mutex_unlock(&lock);
}

static int invoke(int fd, unsigned long request, void *argument, int drm)
{
    int outer = depth++ == 0;
    if (drm && !next_drm_ioctl) next_drm_ioctl = dlsym(RTLD_NEXT, "drmIoctl");
    struct drm_amdgpu_gem_va mapping = {0};
    int map = outer && output && is_command(request, DRM_AMDGPU_GEM_VA) && _IOC_SIZE(request) >= 40;
    if (map) memcpy(&mapping, argument, _IOC_SIZE(request) < sizeof(mapping) ? _IOC_SIZE(request) : sizeof(mapping));
    if (outer && output && is_command(request, DRM_AMDGPU_CS) && _IOC_SIZE(request) >= sizeof(struct drm_amdgpu_cs_in)) log_submit(fd, argument);
    int result = drm ? next_drm_ioctl(fd, request, argument) : next_ioctl(fd, request, argument);
    int saved_errno = errno;
    if (map) {
        pthread_mutex_lock(&lock);
        fprintf(output, "{\"kind\":\"va\",\"ns\":%"PRIu64",\"pid\":%d,\"fd\":%d,\"handle\":%u,\"operation\":%u,\"va\":\"0x%"PRIx64"\",\"size\":%"PRIu64",\"offset\":%"PRIu64",\"flags\":%u,\"result\":%d,\"errno\":%d}\n",
                clock_ns(), getpid(), fd, mapping.handle, mapping.operation, (uint64_t)mapping.va_address, (uint64_t)mapping.map_size, (uint64_t)mapping.offset_in_bo, mapping.flags, result, result ? saved_errno : 0);
        pthread_mutex_unlock(&lock);
    }
    --depth;
    errno = saved_errno;
    return result;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);
    return invoke(fd, request, argument, 0);
}

int drmIoctl(int fd, unsigned long request, void *argument)
{
    return invoke(fd, request, argument, 1);
}
