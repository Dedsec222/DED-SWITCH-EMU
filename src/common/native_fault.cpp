// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "common/logging.h"
#include "common/native_fault.h"
#include "common/signal_chain.h"
namespace Common {
namespace {
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
std::atomic<int> fault_fd{-1};
std::atomic<uint64_t> next_sequence{0};
std::string maps_path;
constexpr std::array FatalSignals{SIGABRT, SIGILL, SIGFPE, SIGTRAP};
std::array<struct sigaction, FatalSignals.size()> previous_fatal_actions{};
void FatalSignalHandler(int signal, siginfo_t* info, void* context) {
    const auto sequence = BeginNativeFault(signal, info, context);
    for (size_t i = 0; i < FatalSignals.size(); ++i) {
        if (FatalSignals[i] == signal) {
            ForwardSignal(signal, info, context, previous_fatal_actions[i]);
            EndNativeFault(sequence);
            return;
        }
    }
}
off_t RecordOffset(uint64_t seq) noexcept {
    return static_cast<off_t>((seq % NativeFaultRecord::Slots) * sizeof(NativeFaultRecord));
}
std::string FaultMapping(uint64_t address, const char* reg) {
    std::ifstream maps(maps_path);
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin{}, end{}, offset{};
        char perms[5]{};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx", &begin, &end, perms, &offset) == 4 &&
            address >= begin && address < end) {
            std::ostringstream out;
            out << "[PreviousNativeFaultMap] " << reg << "=0x" << std::hex << address << ' '
                << line;
            return out.str();
        }
    }
    return {};
}
void WriteAt(int fd, const void* value, size_t size, off_t offset) noexcept {
    const auto* bytes = static_cast<const char*>(value);
    while (size != 0) {
        const auto count = pwrite(fd, bytes, size, offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return;
        bytes += count;
        size -= static_cast<size_t>(count);
        offset += count;
    }
}
} // namespace
void InitializeNativeFaultCapture(const std::string& directory) {
    if (fault_fd.load(std::memory_order_relaxed) >= 0)
        return;
    maps_path = directory + "/native-fault.maps";
    const auto record_path = directory + "/native-fault.bin";
    std::ifstream previous(record_path, std::ios::binary);
    std::ofstream old_log;
    if (std::ifstream(directory + "/eden_log.txt.old.txt"))
        old_log.open(directory + "/eden_log.txt.old.txt", std::ios::app);
    const auto report = [&](const std::string& line) {
        LOG_ERROR(Frontend, "{}", line);
        if (old_log)
            old_log << line << '\n';
    };
    NativeFaultRecord record;
    for (size_t slot = 0; slot < NativeFaultRecord::Slots &&
                          previous.read(reinterpret_cast<char*>(&record), sizeof(record));
         ++slot) {
        if (record.magic != NativeFaultRecord::Magic || record.sequence == 0 ||
            record.completed_sequence == record.sequence)
            continue;
        std::ostringstream out;
        out << "[PreviousNativeFault] signal=" << record.signal << " code=" << record.code
            << " thread=" << record.thread << " timestamp_ns=" << record.timestamp_ns << " fault=0x"
            << std::hex << record.address << " pc=0x" << record.pc << " sp=0x" << record.sp
            << " lr=0x" << record.lr << std::dec << " sequence=" << record.sequence
            << " (forwarded handler did not return)";
        report(out.str());
        for (const auto& mapping : {FaultMapping(record.pc, "pc"), FaultMapping(record.lr, "lr")})
            if (!mapping.empty())
                report(mapping);
        for (size_t i = 0; i < 31; i += 4) {
            std::ostringstream regs;
            regs << "[PreviousNativeFaultRegisters]";
            for (size_t j = i; j < i + 4 && j < 31; ++j)
                regs << " x" << std::dec << j << "=0x" << std::hex << record.registers[j];
            report(regs.str());
        }
    }
    const int fd = open(record_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARNING(Frontend, "Native fault capture unavailable, errno={}", errno);
        return;
    }
    fault_fd.store(fd, std::memory_order_release);
    for (size_t i = 0; i < FatalSignals.size(); ++i) {
        struct sigaction action {};
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        action.sa_sigaction = FatalSignalHandler;
        sigemptyset(&action.sa_mask);
        if (SigAction(FatalSignals[i], &action, &previous_fatal_actions[i]) != 0) {
            LOG_WARNING(Frontend, "Native fatal handler unavailable for signal {}, errno={}",
                        FatalSignals[i], errno);
        }
    }
}
void RefreshNativeFaultMappings() {
    if (fault_fd.load(std::memory_order_acquire) < 0)
        return;
    std::ifstream source("/proc/self/maps", std::ios::binary);
    std::ofstream destination(maps_path, std::ios::binary | std::ios::trunc);
    if (source && destination)
        destination << source.rdbuf();
}
uint64_t BeginNativeFault(int signal, siginfo_t* info, void* context) noexcept {
    const int fd = fault_fd.load(std::memory_order_acquire);
    if (fd < 0)
        return 0;
    const int saved_errno = errno;
    NativeFaultRecord record{};
    record.magic = NativeFaultRecord::Magic;
    record.sequence = next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    record.signal = signal;
    record.code = info ? info->si_code : 0;
    record.address = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
    record.thread = static_cast<uint64_t>(syscall(SYS_gettid));
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0)
        record.timestamp_ns = static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL + now.tv_nsec;
    if (context) {
        const auto& machine = static_cast<ucontext_t*>(context)->uc_mcontext;
#if defined(__aarch64__)
        record.pc = machine.pc;
        record.sp = machine.sp;
        record.lr = machine.regs[30];
        for (size_t i = 0; i < 31; ++i)
            record.registers[i] = machine.regs[i];
#elif defined(__x86_64__) && defined(REG_RIP)
        record.pc = machine.gregs[REG_RIP];
        record.sp = machine.gregs[REG_RSP];
#endif
    }
    WriteAt(fd, &record, sizeof(record), RecordOffset(record.sequence));
    errno = saved_errno;
    return record.sequence;
}
void EndNativeFault(uint64_t sequence) noexcept {
    const int fd = fault_fd.load(std::memory_order_acquire);
    if (fd < 0 || sequence == 0)
        return;
    const int saved_errno = errno;
    WriteAt(fd, &sequence, sizeof(sequence),
            RecordOffset(sequence) + offsetof(NativeFaultRecord, completed_sequence));
    errno = saved_errno;
}
} // namespace Common
