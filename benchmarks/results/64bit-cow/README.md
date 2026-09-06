# 64-bit COW benchmark results

Recorded 2026-09-06 UTC on an Apple M5 Max (18 cores), macOS 26.6.2,
arm64, Apple Clang 21.0.0 (clang-2100.1.1.101). All library builds use
`-O3 -DNDEBUG -std=gnu11`, static linking, no sanitizers, and no LTO;
the benchmark uses `-O3 -DNDEBUG -std=gnu++11`.

Baseline is commit `7dd6eda7`. The same benchmark source is compiled against
baseline with `ROARING64_COW_BENCH_BASELINE`; off/on use commit `655516a4`. The main
CSVs contain 31 timed iterations per case after one discarded warmup.
Runs are sequential, with no concurrent builds/tests. CPU affinity/frequency
are not pinned; min/max columns expose scheduling noise. Treat these as local
measurements, not cross-platform guarantees.

See [the protocol and reproduction commands](../../64bit-cow.md) for exact
timing boundaries, dataset manifests, and allocation accounting. The census
input is all 200 existing census1881 sets. The OSM input is the 46 checked-in
Monaco sets with original node IDs, including values above UINT32_MAX.
`broad64` synthetic cases distribute containers over much wider upper bits.

## Selected timings

Times below are median microseconds for each complete workload. Speedup is
patched-off / patched-on; a value below 1 means COW is slower.

| Dataset / workload | Baseline µs | Off µs | On µs | COW speedup |
|---|---:|---:|---:|---:|
| synthetic / bitset-broad64/copy-first | 11.21 | 11.25 | 3.83 | 2.93× |
| synthetic / bitset-broad64/copy-shared | 11.83 | 12.88 | 3.25 | 3.96× |
| synthetic / bitset-broad64/copies-live-8 | 94.38 | 105.58 | 23.79 | 4.44× |
| synthetic / bitset-broad64/copy-modify-100 | 13.25 | 12.08 | 23.08 | 0.52× |
| census1881 / copy-first | 167.71 | 176.50 | 79.50 | 2.22× |
| census1881 / copies-live-8 | 1,498.50 | 1,507.38 | 580.88 | 2.60× |
| census1881 / successive-union | 357.96 | 428.42 | 212.58 | 2.02× |
| census1881 / aggregate-union | 4,227.54 | 4,745.00 | 2,597.12 | 1.83× |
| osm-monaco / copy-first | 138.46 | 143.21 | 106.29 | 1.35× |
| osm-monaco / copies-live-8 | 1,148.96 | 1,023.21 | 670.42 | 1.53× |
| osm-monaco / aggregate-union | 864.29 | 828.67 | 673.42 | 1.23× |
| osm-monaco / copy-modify-100 | 187.21 | 169.67 | 195.54 | 0.87× |

## Library allocation peaks

These are extra requested live bytes above setup, not RSS. Allocation tracking
runs separately from timing. Copies in `copies-live-8` coexist; ordinary copy
cases free each copy before proceeding to the next input bitmap.

| Dataset / workload | Off extra bytes | On extra bytes | Reduction |
|---|---:|---:|---:|
| synthetic / bitset-broad64/copies-live-8 | 3,197,440 | 44,288 | 98.6% |
| census1881 / copies-live-8 | 16,914,848 | 1,719,920 | 89.8% |
| osm-monaco / copies-live-8 | 1,297,104 | 1,037,664 | 20.0% |
| osm-monaco / copy-first | 18,938 | 33,152 | -75.1% |

Negative reduction means sharing uses more memory. The sparse OSM sample has
many tiny containers: wrappers can outweigh the payload saved on a single
copy. Multiple simultaneous copies amortize that cost. Touching most shared
containers also pays both sharing and detachment costs. COW should remain an
explicit workload choice.

## COW-disabled overhead

The main CSVs compare patched-off with the baseline across every workload.
Focused cases have three reruns of 101 iterations each, alternating baseline/off process
order between rounds. The table reports the median of the three per-run
medians; individual repeat CSVs retain the full observed ranges.

| Workload | Baseline µs | Off µs | Off change |
|---|---:|---:|---:|
| run-mutation / run-broad64/mutate-unshared | 162.75 | 158.50 | -2.6% |
| run-mutation / run-broad64/mutate-shared | 236.50 | 164.62 | -30.4% |
| bitset-mutation / bitset-low32/mutate-unshared | 66.42 | 66.58 | +0.3% |
| osm-iteration / iterate-shared | 30.58 | 31.50 | +3.0% |
| census-modify50 / copy-modify-50 | 189.83 | 190.33 | +0.3% |
| census-modify100 / copy-modify-100 | 216.21 | 220.25 | +1.9% |
| census-union / successive-union | 340.38 | 339.71 | -0.2% |
| bitset-shared-mutation / bitset-low32/mutate-shared | 59.00 | 62.79 | +6.4% |

The focused repeats reduce several large deltas seen in the main CSVs, but
some single-run regressions remain unresolved (including bitset-broad64 scalar
iteration and run-broad64 copy workloads). These measurements do not establish
a broad "no material COW-off regression" result. Disabled COW is not free;
repeatable overhead and scheduling noise both need consideration. Array payload
caching was added after early runs showed per-value shared-wrapper checks slowing scalar iteration. The published
CSVs record that implementation; the later transfer correction below has not
been rebenchmarked.

## Correctness checks

Every timed case compares its result checksum with a COW-disabled reference
outside timing. Original values and retained copies are checked exactly,
internal validation runs, and the accounting pass verifies that all tracked library allocations
are released. The final Debug build passes all 28 CTest executables under ASan
and UBSan, including 11 new COW test groups. Independent subagent review also
passed a 150,000-operation iterator harness including validated, unaligned
portable frozen views.

The modified `roaring64.c` also compiles as C++11, and the C++64 wrapper tests
pass. An optional whole-library C-as-C++ build is blocked by pre-existing
namespace errors in `src/bitset.c`, reproduced on the baseline; it is not
counted as a passing check.

Follow-up review reproduced an ASan use-after-free when advancing an array
iterator after `shrink_to_fit` reallocated its payload. The passing runs above
did not cover that lifecycle. The public C/C++ contract now explicitly makes
storage-changing operations iterator-invalidating, even when values are
unchanged. Callers must reinitialize or recreate iterators afterward. Added
coverage checks reinitialization after shrinking, run conversion, and disabling
COW, as well as continued validity of an iterator on a separate sharing owner.
This documents the lifetime requirement; it does not make using an invalidated
iterator safe. The benchmarked runtime code is unchanged by this follow-up.
After this update and worktree relocation, all 28 test executables passed
again under ASan and UBSan, including the now 12 COW test groups.

A further review found that mixed 32-bit operations can produce SHARED
containers with the COW flag clear (the 32-bit validator rejects that mismatch).
Transfer now enables 64-bit COW based on actual SHARED tags as well as the
source flag. This fixes the reproduced double release on subsequent mutation.
Regression coverage includes all three container types, one/two owners,
add/remove/disable-COW, donor preservation, and both destruction orders.
The timed workloads above do not exercise this 32-bit transfer path.
After this correction, all 28 test executables passed under ASan and UBSan,
including 13 COW test groups; the original transfer reproducer also passes.
