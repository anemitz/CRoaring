# 64-bit container copy-on-write

`roaring64_bitmap_set_copy_on_write(r, true)` enables sharing on future copies.
`roaring64_bitmap_get_copy_on_write(r)` reads the setting. In C++, use
`Roaring64::setCopyOnWrite` and `getCopyOnWrite`. Copies own independent ARTs and
container-pointer arrays, but share reference-counted container payloads. Copy
cost therefore still scales with the number of containers.

## Ownership rules

A bitmap without the COW flag never owns a SHARED container. Internal validation
checks this invariant.

- Copies, overwrites, flips, and offsets inherit the source setting.
- New binary-operation results enable COW only when both inputs do.
- In-place operations retain the destination setting.
- Copy paths share only when both source and destination enable COW and the
  source is not frozen. Otherwise they clone the underlying concrete container.
- Frozen sources always deep-copy. A copy survives destruction of the view and
  its backing buffer, even if the view carries the serialized COW flag.
- Disabling COW detaches all shared containers before clearing the flag. The C
  setter returns false on allocation failure and leaves COW enabled; values
  remain unchanged, although earlier containers may have been detached. The
  C++ wrapper reports this through its existing `ROARING_TERMINATE` convention.
- Moving containers from a COW 32-bit bitmap transfers the setting as well.

Mutators detach the slot before calling consuming container operations. The
four in-place set operations retain their existing allocate-result-and-release
branches for shared inputs. Read-only iteration and serialization unwrap
without detaching. Forward iteration caches concrete array payloads as well as
bitset words, avoiding an unwrap on every array value. Portable bytes are unchanged; frozen serialization replaces
shared type tags with concrete tags in the output ART. Unaligned frozen output
uses an aligned temporary ART only when normalization needs it.

First sharing changes the source's internal metadata. Make copies before
handing them to separate threads; reference-count atomics do not permit
concurrent access to the same bitmap during copy/share or mutation. Mutations
invalidate iterators and bulk contexts, including cached bitset word pointers.
This also applies to operations preserving values: `shrink_to_fit`,
`run_optimize`, `remove_run_compression`, and disabling COW (even on failure).
Reinitialize or recreate the iterator afterward; seeking does not repair an
invalidated iterator. Mutating a different bitmap sharing the same containers
does not invalidate this bitmap's iterators.
Changing a frozen view's COW flag does not make its payload mutable.

## Verification

`roaring64_cow_unit` covers shared-reference lifetimes, every container kind,
mutations and conversions, mixed flag combinations and operand orders,
non-mutating/in-place set operations, overwrite, flips, offsets, 32-bit
ownership transfer, portable/frozen serialization, frozen buffer lifetimes,
scalar/batch/range iterators in both directions, C++ forwarding, and deterministic
random operations checked against `std::set`. Tests validate both originals
and copies. Allocation injection covers new wrapper creation and detachment;
this feature does not fix the pre-existing general ART/container-growth OOM
handling limitations.

Frozen-view coverage also exposed existing unaligned reads in validation,
iteration helpers, and the 32-bit native deserializer. This change uses
byte-addressed `memcpy` for fixed-size reads and the library's existing
`CROARING_ALLOW_UNALIGNED` convention for range iteration and array search.
These checks use valid serialized data, validating views before use.

```sh
cmake -S . -B build/cow-tests -DCMAKE_BUILD_TYPE=Debug \
  -DROARING_SANITIZE=ON -DROARING_SANITIZE_UNDEFINED=ON
cmake --build build/cow-tests -j 8
ctest --test-dir build/cow-tests --output-on-failure
```

## Benchmark protocol

The dedicated benchmark separates timing from library allocation tracking so
accounting does not distort measured times. It runs sequentially with fresh
state, one discarded warmup, and reports median/min/max nanoseconds per complete
workload. Setup, validation, and source teardown are outside timing; copies and
intermediate results are freed inside timing. Exact source-value checks and
comparison with non-COW reference checksums happen outside timing.

The separate accounting pass reports allocations during the workload, initial
live library bytes, peak live bytes, and peak bytes above the initial state.
These are requested allocation sizes, not RSS or allocator metadata. STL input
and bookkeeping allocations are excluded. `copy-shared` retains an extra copy
in setup; `copy-first` starts with concrete containers. `copies-live-8` retains
eight copies of each input simultaneously. Mutation percentages count distinct
containers touched, rounded up, not percentages of individual values.

```sh
cmake -S . -B build/cow-release -DCMAKE_BUILD_TYPE=Release \
  -DROARING_USE_CPM=OFF -DENABLE_ROARING_TESTS=OFF \
  -DENABLE_ROARING_COW_BENCHMARK=ON
cmake --build build/cow-release --target roaring64_cow_benchmark -j 8
build/cow-release/benchmarks/roaring64_cow_benchmark --cow off --iterations 31
build/cow-release/benchmarks/roaring64_cow_benchmark --cow on --iterations 31
```

Use `--filter` to select cases. Synthetic cases cover tiny arrays, larger arrays,
bitsets, and runs with narrow (`low32`) or widely distributed (`broad64`) keys.
They measure copy/free, simultaneous copies, copy-then-modify at 0/1/10/50/100%,
adjacent-pair unions, cumulative unions, intersections, iteration, membership,
and mutation with and without existing shared references.

For real data, provide `--manifest FILE`, with one integer-set file path per
line. Files accept comma-separated or whitespace-separated decimal uint64s;
values are sorted and deduplicated before building bitmaps. For example:

```sh
python3 - <<'PY'
from pathlib import Path
for dataset, directory in (('census1881', 'benchmarks/realdata/census1881'),
                           ('cow-osm-monaco', 'benchmarks/cowdata/osm-monaco')):
    files = sorted(Path(directory).glob('*.txt'))
    Path('/tmp/' + dataset + '-manifest.txt').write_text(
        ''.join(str(p.resolve()) + '\n' for p in files))
PY
build/cow-release/benchmarks/roaring64_cow_benchmark --cow on --iterations 31 \
  --manifest /tmp/census1881-manifest.txt
build/cow-release/benchmarks/roaring64_cow_benchmark --cow on --iterations 31 \
  --manifest /tmp/cow-osm-monaco-manifest.txt
```

Census1881 preserves comparison with the original 32-bit COW work (PR #30).
It contains 32-bit row identifiers; using the 64-bit API does not widen them.
The OSM sample uses natural node IDs above UINT32_MAX; see its README and
metadata for provenance, selection, and attribution.

For a baseline, build commit `7dd6eda7` in a separate directory with the same
compiler, Release flags, and static-library settings. Compile this benchmark
source against that checkout's headers/library with
`-DROARING64_COW_BENCH_BASELINE`; this excludes the new API references. For
example, with a baseline checkout at `/tmp/cow-baseline`:

```sh
cmake -S /tmp/cow-baseline -B /tmp/cow-baseline/build \
  -DCMAKE_BUILD_TYPE=Release -DROARING_USE_CPM=OFF -DENABLE_ROARING_TESTS=OFF
cmake --build /tmp/cow-baseline/build -j 8
c++ -O3 -DNDEBUG -std=c++11 -DROARING64_COW_BENCH_BASELINE \
  -I/tmp/cow-baseline/include benchmarks/roaring64_cow.cpp \
  /tmp/cow-baseline/build/src/libroaring.a -o /tmp/cow-baseline/bench
/tmp/cow-baseline/bench --cow off --label baseline --iterations 31
```

Compare baseline, patched-off, and patched-on on the same idle machine. Gains
are workload dependent: sharing tiny payloads can cost more than cloning them,
and modifying most shared containers eventually pays both sharing and copying
costs. Raw measurements and machine-specific observations are under
`results/64bit-cow/`.
