/* re4_host_macos: macOS loader of the whole-program port (x86_64 under Rosetta).
 *
 * The game tells its own data from offsets by bit 31 of an address and keeps addresses in 32
 * bits, so everything it reaches (the game libraries, its stacks, memory the platform hands it)
 * has to lie in 0x83000000..0xFFFFFFFF (runtime/game_window.cpp). The Linux loader (loader.c)
 * gets that by reserving every free range below 0x83000000 and above 4 GB. macOS hands out
 * address space from the bottom up when the executable is linked with a small __PAGEZERO
 * (-pagezero_size 0x1000; only x86_64, the arm64 kernel insists on 4 GB), so reserving what is
 * free below 0x83000000 is enough: the libraries, and the stacks of re4_window_map, then land
 * in the window. The MEM1 part (0x80000000..0x83000000) is taken over by re4_mem1_init
 * (RE4_MEM1_PLACEHOLDER). */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOW_LIMIT 0x83000000UL

static unsigned long reserveLow(void)
{
    unsigned long taken = 0;
    mach_vm_address_t addr = 0x1000;
    while (addr < LOW_LIMIT) {
        mach_vm_address_t at = addr;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object;
        kern_return_t kr = mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t) &info, &count, &object);
        unsigned long end = (kr == KERN_SUCCESS && at < LOW_LIMIT) ? at : LOW_LIMIT;
        if (end > addr) {
            void* p = mmap((void*) addr, end - addr, PROT_NONE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED) {
                fprintf(stderr, "re4_host_macos: cannot reserve %#lx..%#lx\n", (unsigned long) addr, end);
            } else {
                taken += end - addr;
            }
        }
        if (kr != KERN_SUCCESS) {
            break;
        }
        addr = at + size;
    }
    return taken;
}

typedef int (*ReserveFn)(const char*, const char*);
typedef const char* (*MessageFn)(void);

int main(int argc, char** argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s <libre4_main.dylib> <module dir> <data dir>\n", argv[0]);
        return 2;
    }
    setenv("RE4_MEM1_PLACEHOLDER", "1", 1);
    printf("reserved %lu MB below %#lx\n", reserveLow() >> 20, LOW_LIMIT);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    printf("libre4_main at %p (window is 0x83000000..0xFFFFFFFF)\n", dlsym(lib, "re4_core_reserve"));
    ReserveFn reserve = (ReserveFn) dlsym(lib, "re4_core_reserve");
    MessageFn message = (MessageFn) dlsym(lib, "re4_core_message");
    if (!reserve || !message) {
        fprintf(stderr, "re4_core_reserve / re4_core_message not exported\n");
        return 1;
    }
    int ok = reserve(argv[3], argv[2]);
    printf("re4_core_reserve -> %d: %s\n", ok, message());
    if (!ok) {
        return 3;
    }
    int (*start)(void) = (int (*)(void)) dlsym(lib, "re4_core_start");
    int (*state)(void) = (int (*)(void)) dlsym(lib, "re4_core_state");
    const char* (*problem)(void) = (const char* (*)(void)) dlsym(lib, "re4_core_disc_problem");
    int (*tail)(char*, int) = (int (*)(char*, int)) dlsym(lib, "re4_core_log_tail");
    if (problem && problem() && problem()[0]) {
        printf("disc problem: %s\n", problem());
    }
    int started = start();
    printf("re4_core_start -> %d\n", started);
    const char* secs = getenv("RE4_RUN_SECONDS");
    int ticks = (secs ? atoi(secs) : 2) * 10;
    unsigned (*frames)(void) = (unsigned (*)(void)) dlsym(lib, "re4_core_frame_count");
    for (int i = 0; i < ticks && state() == 1; i++) {
        usleep(100000);
        if (frames && i % 10 == 9) {
            printf("[host] t=%ds frames=%u\n", (i + 1) / 10, frames());
        }
    }
    printf("state %d: %s\n", state(), message());
    char buf[4096];
    if (tail && tail(buf, sizeof buf) > 0) {
        printf("--- log tail ---\n%s\n", buf);
    }
    fflush(stdout);
    _exit(started ? 0 : 4); /* the game threads are still running: no orderly teardown */
}
