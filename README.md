# Pact

Pact is a small experiment in a language whose programs are authored by agents. The compiler is written in C++ and uses LLVM to build native executables. It accepts a compact text syntax and a JSON AST; both frontends produce the same typed program representation.

## Build and run

Requirements: C++17 compiler, LLVM 22 development libraries and `llvm-config`, and `clang++` on `PATH`. The `run` command additionally requires Linux, Bubblewrap, Python 3, `prlimit`, `systemctl`, and a user systemd manager able to create cgroup v2 scopes.

```sh
make
make check
./pact run --allow math.double examples/double.pact
./pact run --allow math.double examples/double.json
./pact run --allow math.double examples/agent_estimate.pact
./pact run examples/calc.pact
```

The examples print `42`; `agent_estimate.pact` multiplies 3 agents by 7 tasks and doubles the estimate for review. `pact check [--allow CAPABILITY] FILE` type-checks without compiling. `pact compile -o PROGRAM [--allow CAPABILITY] FILE` builds a native executable. Diagnostics are JSON by default and use stable `PACT-*` codes. The JSON AST and diagnostic envelope are versioned; see [the format contract](docs/contracts.md). `make check` checks both text and JSON examples, confirms equivalent run output, compiles and runs a native executable, and verifies JSON diagnostics for a missing host grant (`PACT-E003`) and an unknown variable (`PACT-T001`).

## Language slice

## Infographics

See the [Pact infographic guide](docs/README.md) for diagrams covering the compiler, syntax, capabilities, CLI, current tradeoffs, and proposed roadmap.

## Benchmark pilot

The preliminary 24-task generation, editing, and repair pilot is documented in [benchmarks/README.md](benchmarks/README.md). It includes a separate evaluator answer key and a trial summary CLI. The pilot measures only deterministic single-result programs; it does not establish comparative performance or broad task competence.

```text
capability math.double;
let value = 21;
return tool math.double(value);
```

The equivalent JSON AST is in `examples/double.json`. The language supports signed 64-bit integer literals, `true` and `false`, variables, `+`, `-`, `*`, `==`, `<`, `let`, one `return`, conditional expressions, and the explicitly typed `math.double(i64) -> i64` tool. Arithmetic wraps modulo 2^64. Comparisons accept two `i64` operands and produce a boolean. A conditional has the form `if condition { then_expr } else { else_expr }`: its condition must be boolean and both branches must be `i64`, so the conditional itself is an `i64` expression. Booleans cannot be used in arithmetic, comparisons, returns, or tool arguments. Tool calls inside either conditional branch are currently rejected because this slice does not lower branches to lazy control flow. Tool use requires both a capability declaration in the source and a host grant supplied with `--allow`.

```text
let count = 3;
return if count < 7 { 42 } else { 0 };
```

The strict JSON forms for these expressions are documented in [the format contract](docs/contracts.md).

## Execution boundary

`pact run` uses a fail-closed Linux launcher. It creates user, PID, IPC, UTS, and network namespaces with Bubblewrap; disables nested user namespaces, drops capabilities, exposes the generated executable and read-only system libraries, remounts the root read-only, and gives the program a private 64 MiB `/tmp`. A seccomp filter blocks namespace changes, mount operations, tracing, kernel module and BPF access, and several other privileged interfaces before Pact code runs. A user systemd scope enforces a 256 MiB memory cap, 16-process cap, and one-CPU quota. The host runner monitors cgroup v2 CPU accounting and stops the scope at 5 CPU seconds; `prlimit` adds per-process CPU and core-dump limits. The host runner also enforces a 30-second wall limit and a 1 MiB combined output cap. On interruption, it terminates the complete scope. If the required boundary cannot start, `run` reports a `PACT-S*` diagnostic and does not execute the binary directly.

This is an initial Linux boundary, not an independently reviewed security product. The seccomp policy is a short denylist, not a complete syscall allowlist. At runtime, `math.double` is served by a trusted host-side broker over a private per-run Unix socket; generated programs receive only that typed operation, and the broker rejects all other operation IDs or malformed requests. The CLI grant remains a compile-time check; the runtime broker allowlist is the authority for this mock. No credentials or real tools are exposed. Durable workflow cancellation semantics remain future work. Do not describe this prototype as suitable for untrusted production programs until the adversarial suite and independent security review pass.

The sandbox applies only to `pact run`. `pact compile` produces a native executable; launching it directly bypasses the Pact sandbox. A compiled program that calls `math.double` also requires the Pact host runner’s broker, so direct launch fails closed at that tool call.
