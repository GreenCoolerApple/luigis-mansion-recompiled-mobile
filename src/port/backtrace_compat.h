#pragma once
#include <dlfcn.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdint>

#include <cstdio>

namespace port::bt {

struct State {
    void** frames;
    int max;
    int count;
};

inline _Unwind_Reason_Code step(_Unwind_Context* ctx, void* arg) {
    auto* s = static_cast<State*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && s->count < s->max) s->frames[s->count++] = reinterpret_cast<void*>(pc);
    return s->count < s->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

}

inline int backtrace(void** frames, int max) {
    port::bt::State s{frames, max, 0};
    _Unwind_Backtrace(&port::bt::step, &s);
    return s.count;
}

inline void backtrace_symbols_fd(void* const* frames, int count, int fd) {
    for (int i = 0; i < count; i++) {
        Dl_info info{};
        char line[512];
        int n;
        if (dladdr(frames[i], &info) && info.dli_fname)
            n = std::snprintf(line, sizeof(line), "%s(%s+0x%lx) [%p]\n", info.dli_fname,
                              info.dli_sname ? info.dli_sname : "",
                              static_cast<unsigned long>(static_cast<const char*>(frames[i]) -
                                                         static_cast<const char*>(info.dli_fbase)),
                              frames[i]);
        else
            n = std::snprintf(line, sizeof(line), "[%p]\n", frames[i]);
        if (n > 0) (void)!write(fd, line, size_t(n) < sizeof(line) ? size_t(n) : sizeof(line) - 1);
    }
}
