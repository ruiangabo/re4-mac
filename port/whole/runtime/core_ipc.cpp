// The core in a process of its own: what the app and the core process share
// (re4_core_ipc.h). Compiled into the app (core mode), the core process
// (host/coreproc.cpp) and the Linux check re4_core_remote_test.
#include "re4_core_ipc.h"

#include <errno.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/futex.h>
#endif
#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#include <stdio.h>
#endif
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

#ifndef SYS_memfd_create
#if defined(__arm__)
#define SYS_memfd_create 385
#elif defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__aarch64__)
#define SYS_memfd_create 279
#endif
#endif

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

namespace re4ipc {

namespace {

size_t align64(size_t n)
{
    return (n + 63) & ~(size_t) 63;
}

size_t snapshotOffset()
{
    return align64(sizeof(Control));
}

size_t audioOffset()
{
    return align64(snapshotOffset() + 3 * sizeof(Re4CoreSnapshot));
}

size_t depthOffset()
{
    return align64(audioOffset() + kAudioFrames * kAudioMaxChannels * sizeof(int16_t));
}

// FNV-1a, 64 bit
struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void add(const void* data, size_t n)
    {
        const unsigned char* p = (const unsigned char*) data;
        for (size_t i = 0; i < n; i++) {
            h = (h ^ p[i]) * 0x100000001b3ull;
        }
    }
};

}  // namespace

size_t controlBytes()
{
    return depthOffset() + align64(320 * 264 * sizeof(uint32_t));  // RE4_GX_DEPTH_W x RE4_GX_DEPTH_H
}

void controlInit(Control* c, uint32_t pid)
{
    memset((void*) c, 0, sizeof(Control));
    c->magic = kMagic;
    c->version = kVersion;
    c->size = (uint32_t) controlBytes();
    c->pid = pid;
    c->snapshotOffset = (uint32_t) snapshotOffset();
    c->audioOffset = (uint32_t) audioOffset();
    c->depthOffset = (uint32_t) depthOffset();
    c->audioChannels = 6;
    c->audioRate = 32000;
    c->snapshotMiddle.store(1);
    c->freeAt.store(-1);
}

int shmCreate(const char* name, size_t size)
{
#if defined(__APPLE__)
    // POSIX shared memory, unlinked at once: only the descriptor (passed over the socket) keeps it
    static int serial = 0;
    char unique[31];
    snprintf(unique, sizeof(unique), "/re4.%d.%d", (int) getpid(), serial++);
    int fd = shm_open(unique, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(unique);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (ftruncate(fd, (off_t) size) == 0) {
            return fd;
        }
        close(fd);
    }
    (void) name;
    return -1;
#else
#if defined(SYS_memfd_create)
    int fd = (int) syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t) size) == 0) {
            return fd;
        }
        close(fd);
    }
#endif
#if defined(__ANDROID__)
    // kernels or policies without memfd: the NDK's shared memory (Android 8.0 and later)
    static int (*create)(const char*, size_t) = [] {
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        return lib ? (int (*)(const char*, size_t)) dlsym(lib, "ASharedMemory_create") : nullptr;
    }();
    if (create) {
        int fd = create(name, size);
        if (fd >= 0) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            return fd;
        }
    }
#endif
    (void) name;
    (void) size;
    return -1;
#endif
}

void* shmMap(int fd, size_t size)
{
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void ring(Bell& bell)
{
    bell.seq.fetch_add(1, std::memory_order_release);
#if defined(__APPLE__)
    // the kernel's futex of macOS 14.4+, across processes (x86_64 under Rosetta and arm64 share the page)
    os_sync_wake_by_address_all(reinterpret_cast<void*>(&bell.seq), sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#else
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&bell.seq), FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
}

void waitBell(Bell& bell, uint32_t seen, int timeoutMs)
{
#if defined(__APPLE__)
    // a short spin first (the other side often answers within microseconds), then sleep in the kernel
    for (int spin = 0; spin < 200; spin++) {
        if (bell.seq.load(std::memory_order_acquire) != seen) {
            return;
        }
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (bell.seq.load(std::memory_order_acquire) == seen) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t left = (int64_t) timeoutMs * 1000000 -
                             ((int64_t) (now.tv_sec - start.tv_sec) * 1000000000 + (now.tv_nsec - start.tv_nsec));
        if (left <= 0) {
            return;
        }
        int r = os_sync_wait_on_address_with_timeout(reinterpret_cast<void*>(&bell.seq), seen, sizeof(uint32_t),
                                                     OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                     (uint64_t) left);
        if (r < 0 && errno == ETIMEDOUT) {
            return;
        }
    }
#else
    // the kernel's timespec of this program width (time32 on 32 bit, SYS_futex)
    struct {
        long sec, nsec;
    } t = {timeoutMs / 1000, (long) (timeoutMs % 1000) * 1000000L};
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&bell.seq), FUTEX_WAIT, seen, &t, nullptr, 0);
#endif
}

bool sendMsg(int sock, const MsgHeader& header, const void* payload, int fd)
{
    const char* p = (const char*) &header;
    size_t left = sizeof(header);
    bool first = true;
    while (left) {
        struct iovec iov = {(void*) p, left};
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(sizeof(int))];
        } control;
        if (first && fd >= 0) {
            memset(&control, 0, sizeof(control));
            m.msg_control = control.bytes;
            m.msg_controllen = sizeof(control.bytes);
            struct cmsghdr* c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &fd, sizeof(int));
        }
        ssize_t n = sendmsg(sock, &m, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        first = false;
        p += n;
        left -= (size_t) n;
    }
    p = (const char*) payload;
    left = header.length;
    while (left) {
        ssize_t n = ::send(sock, p, left, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        left -= (size_t) n;
    }
    return true;
}

bool receiveMsg(int sock, MsgHeader& header, std::vector<char>& payload, int* fd)
{
    *fd = -1;
    char* p = (char*) &header;
    size_t left = sizeof(header);
    while (left) {
        struct iovec iov = {p, left};
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(sizeof(int))];
        } control;
        m.msg_control = control.bytes;
        m.msg_controllen = sizeof(control.bytes);
        ssize_t n = recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int got;
                memcpy(&got, CMSG_DATA(c), sizeof(int));
                if (*fd >= 0) {
                    close(*fd);
                }
                *fd = got;
            }
        }
        p += n;
        left -= (size_t) n;
    }
    payload.resize(header.length);
    p = payload.data();
    left = header.length;
    while (left) {
        ssize_t n = recv(sock, p, left, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        left -= (size_t) n;
    }
    return true;
}

uint64_t hashChunk(const GxInfo& info, const void* ops, const void* vertices, const void* indices, const void* blob)
{
    Hash h;
    h.add(&info, sizeof(info));
    h.add(ops, info.opWords * 4u);
    h.add(vertices, info.vertexBytes);
    h.add(indices, info.indexCount * 2u);
    h.add(blob, info.blobBytes);
    return h.h;
}

namespace {

unsigned clampCount(unsigned n, unsigned max)
{
    return n < max ? n : max;
}

}  // namespace

uint64_t hashSnapshot(const Re4CoreSnapshot& s)
{
    Hash h;
    h.add(&s, offsetof(Re4CoreSnapshot, models));
    h.add(s.models, clampCount(s.nModels, RE4_CORE_MAX_MODELS) * sizeof(s.models[0]));
    h.add(s.infos, clampCount(s.nInfos, RE4_CORE_MAX_INFOS) * sizeof(s.infos[0]));
    h.add(s.parts, clampCount(s.nParts, RE4_CORE_MAX_PARTS) * sizeof(s.parts[0]));
    h.add(s.morph, clampCount(s.nMorph, RE4_CORE_MAX_MORPH) * sizeof(s.morph[0]));
    return h.h;
}

void copySnapshot(Re4CoreSnapshot& to, const Re4CoreSnapshot& from)
{
    memcpy(&to, &from, offsetof(Re4CoreSnapshot, models));
    memcpy(to.models, from.models, clampCount(from.nModels, RE4_CORE_MAX_MODELS) * sizeof(from.models[0]));
    memcpy(to.infos, from.infos, clampCount(from.nInfos, RE4_CORE_MAX_INFOS) * sizeof(from.infos[0]));
    memcpy(to.parts, from.parts, clampCount(from.nParts, RE4_CORE_MAX_PARTS) * sizeof(from.parts[0]));
    memcpy(to.morph, from.morph, clampCount(from.nMorph, RE4_CORE_MAX_MORPH) * sizeof(from.morph[0]));
}

void fillInfo(GxInfo& info, const Re4GxChunk& c)
{
    info.version = c.version;
    info.sequence = c.sequence;
    info.flags = c.flags;
    info.opWords = c.opWords;
    info.vertexBytes = c.vertexBytes;
    info.indexCount = c.indexCount;
    info.blobBytes = c.blobBytes;
    info.xfbAddress = c.xfbAddress;
    info.xfbWidth = c.xfbWidth;
    info.xfbHeight = c.xfbHeight;
    info.draws = c.draws;
    info.vertexCount = c.vertexCount;
    info.textureBytes = c.textureBytes;
}

uint32_t nowMs()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t) ((uint64_t) t.tv_sec * 1000u + (uint64_t) t.tv_nsec / 1000000u);
}

}  // namespace re4ipc
