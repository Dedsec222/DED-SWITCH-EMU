// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <dlfcn.h>
#include <unistd.h>

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/scope_exit.h"
#include "common/signal_chain.h"

namespace Common {

template <typename T>
T* LookupLibcSymbol(const char* name) {
#if defined(__BIONIC__)
    Common::DynamicLibrary provider("libc.so");
    if (!provider.IsOpen()) {
        UNREACHABLE_MSG("Failed to open libc!");
    }
#else
    // For other operating environments, we assume the symbol is not overridden.
    const char* base = nullptr;
    Common::DynamicLibrary provider(base);
#endif

    void* sym = provider.GetSymbolAddress(name);
    if (sym == nullptr) {
        sym = dlsym(RTLD_DEFAULT, name);
    }
    if (sym == nullptr) {
        UNREACHABLE_MSG("Unable to find symbol {}!", name);
    }

    return reinterpret_cast<T*>(sym);
}

int SigAction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    static auto libc_sigaction = LookupLibcSymbol<decltype(sigaction)>("sigaction");
    return libc_sigaction(signum, act, oldact);
}

void ForwardSignal(int signum, siginfo_t* info, void* context, const struct sigaction& previous) {
    if (previous.sa_handler == SIG_IGN)
        return;
    if (previous.sa_handler == SIG_DFL) {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (SigAction(signum, &action, nullptr) != 0)
            _exit(128 + signum);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, signum);
        sigprocmask(SIG_UNBLOCK, &mask, nullptr);
        raise(signum);
        _exit(128 + signum);
    }
    if ((previous.sa_flags & SA_RESETHAND) != 0) {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        SigAction(signum, &action, nullptr);
    }
    if ((previous.sa_flags & SA_SIGINFO) != 0)
        previous.sa_sigaction(signum, info, context);
    else
        previous.sa_handler(signum);
}

} // namespace Common
