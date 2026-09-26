#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <array>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
extern "C" std::int64_t pact_entry();
namespace {
constexpr int kBrokerFailureStatus = 125;
constexpr int kBrokerTimeoutMs = 500;

[[noreturn]] void brokerFailure() { _exit(kBrokerFailureStatus); }

bool waitSocket(int fd, short events, int timeoutMs) {
    pollfd descriptor{fd, events, 0};
    int result;
    do { result = poll(&descriptor, 1, timeoutMs); } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & (events | POLLHUP)) && !(descriptor.revents & (POLLERR | POLLNVAL));
}

bool sendAll(int fd, const unsigned char *data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        if (!waitSocket(fd, POLLOUT, kBrokerTimeoutMs)) return false;
        ssize_t count = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool receiveAll(int fd, unsigned char *data, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        if (!waitSocket(fd, POLLIN, kBrokerTimeoutMs)) return false;
        ssize_t count = recv(fd, data + received, size - received, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}
}

extern "C" std::int64_t pact_math_double(std::int64_t x) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) brokerFailure();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    constexpr char path[] = "/run/pact-broker.sock";
    static_assert(sizeof(path) <= sizeof(address.sun_path), "broker path too long");
    std::memcpy(address.sun_path, path, sizeof(path));
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); brokerFailure(); }
    int connected = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connected < 0 && errno != EINPROGRESS) { close(fd); brokerFailure(); }
    if (connected < 0) {
        if (!waitSocket(fd, POLLOUT, kBrokerTimeoutMs)) { close(fd); brokerFailure(); }
        int error = 0;
        socklen_t errorSize = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorSize) < 0 || error != 0) { close(fd); brokerFailure(); }
    }
    std::array<unsigned char, 16> request{};
    request[0] = 'P'; request[1] = 'D'; request[2] = 'M'; request[3] = '1';
    request[4] = 1; // math.double
    request[5] = 1; // one argument
    std::uint64_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    for (unsigned i = 0; i < 8; ++i) request[8 + i] = static_cast<unsigned char>(bits >> (8 * i));
    if (!sendAll(fd, request.data(), request.size())) { close(fd); brokerFailure(); }
    if (shutdown(fd, SHUT_WR) < 0) { close(fd); brokerFailure(); }
    std::array<unsigned char, 16> response{};
    if (!receiveAll(fd, response.data(), response.size())) { close(fd); brokerFailure(); }
    unsigned char extra;
    if (response[0] != 'P' || response[1] != 'D' || response[2] != 'R' || response[3] != '1' ||
        response[4] != 0 || response[5] != 0 || response[6] != 0 || response[7] != 0) { close(fd); brokerFailure(); }
    if (!waitSocket(fd, POLLIN, kBrokerTimeoutMs) || recv(fd, &extra, 1, 0) != 0) { close(fd); brokerFailure(); }
    close(fd);
    bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(response[8 + i]) << (8 * i);
    std::int64_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
static bool installSyscallFilter() {
#if defined(__x86_64__)
    constexpr std::uint32_t expectedArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr std::uint32_t expectedArch = AUDIT_ARCH_AARCH64;
#else
    return false;
#endif
    std::vector<sock_filter> filter;
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_LD | BPF_W | BPF_ABS), 0, 0,
                                 static_cast<std::uint32_t>(offsetof(struct seccomp_data, arch))});
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_JMP | BPF_JEQ | BPF_K), 1, 0, expectedArch});
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_KILL_PROCESS});
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_LD | BPF_W | BPF_ABS), 0, 0,
                                 static_cast<std::uint32_t>(offsetof(struct seccomp_data, nr))});
#if defined(__x86_64__)
    // The x32 ABI uses the x86-64 audit architecture tag but sets bit 30 in
    // the syscall number. Reject it so alternate syscall numbers cannot skip
    // this architecture-specific denylist.
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_JMP | BPF_JSET | BPF_K), 0, 1, 0x40000000U});
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_KILL_PROCESS});
#endif
#define PACT_DENY_SYSCALL(number) \
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_JMP | BPF_JEQ | BPF_K), 0, 1, number}); \
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_ERRNO | EPERM})
#ifdef __NR_mount
    PACT_DENY_SYSCALL(__NR_mount);
#endif
#ifdef __NR_umount2
    PACT_DENY_SYSCALL(__NR_umount2);
#endif
#ifdef __NR_pivot_root
    PACT_DENY_SYSCALL(__NR_pivot_root);
#endif
#ifdef __NR_setns
    PACT_DENY_SYSCALL(__NR_setns);
#endif
#ifdef __NR_unshare
    PACT_DENY_SYSCALL(__NR_unshare);
#endif
#ifdef __NR_ptrace
    PACT_DENY_SYSCALL(__NR_ptrace);
#endif
#ifdef __NR_process_vm_readv
    PACT_DENY_SYSCALL(__NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    PACT_DENY_SYSCALL(__NR_process_vm_writev);
#endif
#ifdef __NR_kexec_load
    PACT_DENY_SYSCALL(__NR_kexec_load);
#endif
#ifdef __NR_kexec_file_load
    PACT_DENY_SYSCALL(__NR_kexec_file_load);
#endif
#ifdef __NR_init_module
    PACT_DENY_SYSCALL(__NR_init_module);
#endif
#ifdef __NR_finit_module
    PACT_DENY_SYSCALL(__NR_finit_module);
#endif
#ifdef __NR_delete_module
    PACT_DENY_SYSCALL(__NR_delete_module);
#endif
#ifdef __NR_bpf
    PACT_DENY_SYSCALL(__NR_bpf);
#endif
#ifdef __NR_keyctl
    PACT_DENY_SYSCALL(__NR_keyctl);
#endif
#ifdef __NR_add_key
    PACT_DENY_SYSCALL(__NR_add_key);
#endif
#ifdef __NR_request_key
    PACT_DENY_SYSCALL(__NR_request_key);
#endif
#ifdef __NR_perf_event_open
    PACT_DENY_SYSCALL(__NR_perf_event_open);
#endif
#ifdef __NR_userfaultfd
    PACT_DENY_SYSCALL(__NR_userfaultfd);
#endif
#ifdef __NR_io_uring_setup
    PACT_DENY_SYSCALL(__NR_io_uring_setup);
#endif
#ifdef __NR_open_by_handle_at
    PACT_DENY_SYSCALL(__NR_open_by_handle_at);
#endif
#ifdef __NR_name_to_handle_at
    PACT_DENY_SYSCALL(__NR_name_to_handle_at);
#endif
#ifdef __NR_reboot
    PACT_DENY_SYSCALL(__NR_reboot);
#endif
#ifdef __NR_swapon
    PACT_DENY_SYSCALL(__NR_swapon);
#endif
#ifdef __NR_swapoff
    PACT_DENY_SYSCALL(__NR_swapoff);
#endif
#undef PACT_DENY_SYSCALL
    filter.push_back(sock_filter{static_cast<unsigned short>(BPF_RET | BPF_K), 0, 0, SECCOMP_RET_ALLOW});
    sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return false;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

int main() {
    if (!installSyscallFilter()) return 126;
    std::printf("%" PRId64 "\n", pact_entry());
}
