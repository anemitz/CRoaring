// Reproducible COW timing and allocation benchmark. No hardware counters or
// third-party dependencies. Define ROARING64_COW_BENCH_BASELINE to compile
// this same source against the pre-COW library (only --cow off is available).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <roaring/roaring64.h>
using namespace roaring::api;

namespace {
using Inputs = std::vector<std::vector<uint64_t>>;
struct State {
    std::vector<roaring64_bitmap_t *> inputs;
    std::vector<roaring64_bitmap_t *> keep;
    ~State() {
        for (auto *r : keep) roaring64_bitmap_free(r);
        for (auto *r : inputs) roaring64_bitmap_free(r);
    }
};
void set_cow(roaring64_bitmap_t *r, bool on) {
#ifdef ROARING64_COW_BENCH_BASELINE
    (void)r;
    (void)on;
#else
    if (!roaring64_bitmap_set_copy_on_write(r, on)) std::abort();
#endif
}
void validate(const roaring64_bitmap_t *r) {
    const char *reason = nullptr;
    if (r == nullptr || !roaring64_bitmap_internal_validate(r, &reason)) {
        std::fprintf(stderr, "invalid result: %s\n", reason ? reason : "NULL");
        std::abort();
    }
}
State *build(const Inputs &inputs, bool cow, bool shared) {
    auto *state = new State;
    for (const auto &input : inputs) {
        auto *r = roaring64_bitmap_of_ptr(input.size(), input.data());
        roaring64_bitmap_run_optimize(r);
        roaring64_bitmap_shrink_to_fit(r);
        set_cow(r, cow);
        state->inputs.push_back(r);
        if (shared) state->keep.push_back(roaring64_bitmap_copy(r));
    }
    return state;
}
// Library allocation accounting runs separately from timing. std::map and
// input-vector allocations are deliberately outside these library hooks.
std::map<void *, size_t> allocations;
size_t live_bytes = 0, peak_bytes = 0, allocation_count = 0;
void record(void *p, size_t size) {
    if (!p) std::abort();
    allocations[p] = size;
    live_bytes += size;
    peak_bytes = std::max(peak_bytes, live_bytes);
    ++allocation_count;
}
void *tracked_malloc(size_t size) {
    void *p = std::malloc(size ? size : 1);
    record(p, size);
    return p;
}
void tracked_free(void *p) {
    if (!p) return;
    auto it = allocations.find(p);
    if (it == allocations.end()) std::abort();
    live_bytes -= it->second;
    allocations.erase(it);
    std::free(p);
}
void *tracked_calloc(size_t n, size_t size) {
    void *p = std::calloc(n ? n : 1, size ? size : 1);
    record(p, n * size);
    return p;
}
void *tracked_realloc(void *p, size_t size) {
    if (p) {
        live_bytes -= allocations.at(p);
        allocations.erase(p);
    }
    void *result = std::realloc(p, size ? size : 1);
    record(result, size);
    return result;
}
void *plain_aligned(size_t alignment, size_t size) {
#ifdef _MSC_VER
    return _aligned_malloc(size, alignment);
#else
    void *p = nullptr;
    return posix_memalign(&p, alignment, size ? size : alignment) == 0
               ? p
               : nullptr;
#endif
}
void plain_aligned_free(void *p) {
#ifdef _MSC_VER
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void *tracked_aligned(size_t alignment, size_t size) {
    void *p = plain_aligned(alignment, size);
    record(p, size);
    return p;
}
void tracked_aligned_free(void *p) {
    if (!p) return;
    live_bytes -= allocations.at(p);
    allocations.erase(p);
    plain_aligned_free(p);
}
void hooks(bool tracking) {
    roaring_memory_t h =
        tracking
            ? roaring_memory_t{tracked_malloc,  tracked_realloc,
                               tracked_calloc,  tracked_free,
                               tracked_aligned, tracked_aligned_free}
            : roaring_memory_t{std::malloc, std::realloc,  std::calloc,
                               std::free,   plain_aligned, plain_aligned_free};
    roaring_init_memory_hook(h);
}
void validate_sources(const State *state, const Inputs &inputs) {
    for (size_t i = 0; i < state->inputs.size(); ++i) {
        validate(state->inputs[i]);
        std::vector<uint64_t> actual(
            roaring64_bitmap_get_cardinality(state->inputs[i]));
        roaring64_bitmap_to_uint64_array(state->inputs[i], actual.data());
        if (actual != inputs[i]) std::abort();
        if (!state->keep.empty()) {
            validate(state->keep[i]);
            if (!roaring64_bitmap_equals(state->inputs[i], state->keep[i]))
                std::abort();
        }
    }
}
struct Case {
    std::string name;
    bool shared;
    std::function<uint64_t(State *)> run;
};
volatile uint64_t sink = 0;
void measure(const Inputs &inputs, const Case &c, bool cow, int iterations,
             const std::string &label) {
    std::vector<double> times;
    uint64_t expected = 0;
    // Validate reference results and source immutability outside timing.
    State *reference = build(inputs, false, c.shared);
    expected = c.run(reference);
    validate_sources(reference, inputs);
    delete reference;
    for (int i = -1; i < iterations; ++i) {
        State *s = build(inputs, cow, c.shared);
        auto start = std::chrono::steady_clock::now();
        uint64_t answer = c.run(s);
        auto stop = std::chrono::steady_clock::now();
        sink = answer;
        if (answer != expected) std::abort();
        validate_sources(s, inputs);
        delete s;
        if (i >= 0)
            times.push_back(
                std::chrono::duration<double, std::nano>(stop - start).count());
    }
    std::sort(times.begin(), times.end());
    hooks(true);
    State *s = build(inputs, cow, c.shared);
    size_t initial = live_bytes;
    allocation_count = 0;
    peak_bytes = initial;
    if (c.run(s) != expected) std::abort();
    size_t count = allocation_count, peak = peak_bytes;
    delete s;
    if (live_bytes != 0 || !allocations.empty()) std::abort();
    hooks(false);
    std::printf("%s,%s,%s,%.0f,%.0f,%.0f,%zu,%zu,%zu,%zu,%llu\n", label.c_str(),
                cow ? "on" : "off", c.name.c_str(), times[times.size() / 2],
                times.front(), times.back(), count, initial, peak,
                peak - initial, (unsigned long long)expected);
    std::fflush(stdout);
}
Inputs synthetic(const std::string &kind, bool broad) {
    Inputs result(2);
    int count = kind == "bitset" ? 32 : 128;
    int values = kind == "bitset"  ? 8192
                 : kind == "array" ? 128
                 : kind == "run"   ? 4096
                                   : 2;
    for (int i = 0; i < count; ++i) {
        uint64_t key = broad ? (uint64_t(i) * UINT64_C(0x9e3779b97f4b)) &
                                   ((UINT64_C(1) << 48) - 1)
                             : uint64_t(i);
        uint64_t base = key << 16;
        for (int v = 0; v < values; ++v) {
            uint64_t value = base + uint64_t(v) * (kind == "run" ? 1 : 2);
            result[0].push_back(value);
            // Half the containers overlap; the rest occur only in one input.
            if (i % 2 == 0) result[1].push_back(value);
        }
        if (i % 2 != 0)
            result[1].push_back(base ^ (UINT64_C(1) << (broad ? 55 : 31)));
    }
    for (auto &r : result) std::sort(r.begin(), r.end());
    return result;
}
std::vector<Case> cases(const Inputs &inputs) {
    std::vector<Case> result;
    for (bool shared : {false, true}) {
        result.push_back(
            {shared ? "copy-shared" : "copy-first", shared, [](State *s) {
                 uint64_t sum = 0;
                 for (auto *r : s->inputs) {
                     auto *copy = roaring64_bitmap_copy(r);
                     sum += roaring64_bitmap_get_cardinality(copy);
                     roaring64_bitmap_free(copy);
                 }
                 return sum;
             }});
    }
    result.push_back({"copies-live-8", false, [](State *s) {
                          std::vector<roaring64_bitmap_t *> copies;
                          uint64_t sum = 0;
                          for (int n = 0; n < 8; ++n)
                              for (auto *r : s->inputs) {
                                  auto *copy = roaring64_bitmap_copy(r);
                                  copies.push_back(copy);
                                  sum += roaring64_bitmap_get_cardinality(copy);
                              }
                          for (auto *r : copies) roaring64_bitmap_free(r);
                          return sum;
                      }});
    for (int percent : {0, 1, 10, 50, 100}) {
        std::vector<std::vector<uint64_t>> edits;
        for (const auto &input : inputs) {
            std::vector<uint64_t> keys;
            for (uint64_t value : input)
                if (keys.empty() || (value >> 16) != (keys.back() >> 16))
                    keys.push_back(value);
            // Touch ceil(percent * containers / 100) different containers.
            keys.resize((keys.size() * percent + 99) / 100);
            edits.push_back(keys);
        }
        result.push_back({"copy-modify-" + std::to_string(percent), false,
                          [edits](State *s) {
                              uint64_t sum = 0;
                              for (size_t i = 0; i < s->inputs.size(); ++i) {
                                  auto *copy =
                                      roaring64_bitmap_copy(s->inputs[i]);
                                  for (uint64_t v : edits[i])
                                      roaring64_bitmap_remove(copy, v);
                                  sum += roaring64_bitmap_get_cardinality(copy);
                                  roaring64_bitmap_free(copy);
                              }
                              return sum;
                          }});
    }
    result.push_back({"successive-union", false, [](State *s) {
                          uint64_t sum = 0;
                          for (size_t i = 1; i < s->inputs.size(); ++i) {
                              auto *r = roaring64_bitmap_or(s->inputs[i - 1],
                                                            s->inputs[i]);
                              sum += roaring64_bitmap_get_cardinality(r);
                              roaring64_bitmap_free(r);
                          }
                          return sum;
                      }});
    result.push_back({"aggregate-union", false, [](State *s) {
                          auto *acc = roaring64_bitmap_copy(s->inputs.front());
                          for (size_t i = 1; i < s->inputs.size(); ++i) {
                              auto *next =
                                  roaring64_bitmap_or(acc, s->inputs[i]);
                              roaring64_bitmap_free(acc);
                              acc = next;
                          }
                          uint64_t answer =
                              roaring64_bitmap_get_cardinality(acc);
                          roaring64_bitmap_free(acc);
                          return answer;
                      }});
    result.push_back({"intersection", true, [](State *s) {
                          uint64_t sum = 0;
                          for (size_t i = 1; i < s->inputs.size(); ++i) {
                              auto *r = roaring64_bitmap_and(s->inputs[i - 1],
                                                             s->inputs[i]);
                              sum += roaring64_bitmap_get_cardinality(r);
                              roaring64_bitmap_free(r);
                          }
                          return sum;
                      }});
    result.push_back({"iterate-shared", true, [](State *s) {
                          uint64_t sum = 0;
                          for (auto *r : s->inputs) {
                              auto *it = roaring64_iterator_create(r);
                              while (roaring64_iterator_has_value(it)) {
                                  sum += roaring64_iterator_value(it);
                                  roaring64_iterator_advance(it);
                              }
                              roaring64_iterator_free(it);
                          }
                          return sum;
                      }});
    result.push_back({"contains-shared", true, [inputs](State *s) {
                          uint64_t sum = 0;
                          for (size_t i = 0; i < s->inputs.size(); ++i)
                              for (size_t j = 0; j < inputs[i].size(); j += 7)
                                  sum += roaring64_bitmap_contains(
                                      s->inputs[i], inputs[i][j]);
                          return sum;
                      }});
    for (bool shared : {false, true})
        result.push_back(
            {shared ? "mutate-shared" : "mutate-unshared", shared,
             [inputs](State *s) {
                 uint64_t sum = 0;
                 for (size_t i = 0; i < s->inputs.size(); ++i) {
                     for (size_t j = 0; j < inputs[i].size(); j += 101) {
                         uint64_t v = inputs[i][j];
                         roaring64_bitmap_remove(s->inputs[i], v);
                         roaring64_bitmap_add(s->inputs[i], v);
                     }
                     sum += roaring64_bitmap_get_cardinality(s->inputs[i]);
                 }
                 return sum;
             }});
    return result;
}
}  // namespace
int main(int argc, char **argv) {
    bool cow = false;
    int iterations = 15;
    std::string label = "patched", filter, manifest;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (i + 1 < argc && arg == "--cow")
            cow = std::string(argv[++i]) == "on";
        else if (i + 1 < argc && arg == "--iterations")
            iterations = std::atoi(argv[++i]);
        else if (i + 1 < argc && arg == "--label")
            label = argv[++i];
        else if (i + 1 < argc && arg == "--filter")
            filter = argv[++i];
        else if (i + 1 < argc && arg == "--manifest")
            manifest = argv[++i];
        else {
            std::fprintf(
                stderr,
                "usage: %s [--cow on|off] [--iterations N] [--label NAME] "
                "[--filter SUBSTRING] [--manifest FILE]\nManifest: one "
                "integer-set file path per line; each set contains decimal "
                "uint64 values separated by commas or whitespace.\n",
                argv[0]);
            return 1;
        }
    }
    if (iterations < 1) return 1;
#ifdef ROARING64_COW_BENCH_BASELINE
    if (cow) {
        std::fprintf(stderr, "baseline has no COW support\n");
        return 1;
    }
#endif
    hooks(false);
    std::vector<std::pair<std::string, Inputs>> datasets;
    if (manifest.empty()) {
        for (const char *kind : {"tiny", "array", "bitset", "run"})
            for (bool broad : {false, true})
                datasets.push_back(
                    {std::string(kind) + (broad ? "-broad64" : "-low32"),
                     synthetic(kind, broad)});
    } else {
        std::ifstream paths(manifest);
        std::string path;
        Inputs inputs;
        if (!paths) {
            std::fprintf(stderr, "cannot read manifest\n");
            return 1;
        }
        while (std::getline(paths, path)) {
            if (path.empty()) continue;
            std::ifstream file(path);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", path.c_str());
                return 1;
            }
            std::ostringstream contents;
            contents << file.rdbuf();
            std::string text = contents.str();
            std::replace(text.begin(), text.end(), ',', ' ');
            std::istringstream numbers(text);
            uint64_t v;
            std::vector<uint64_t> values;
            while (numbers >> v) values.push_back(v);
            if (!numbers.eof()) {
                std::fprintf(stderr, "invalid integer file %s\n", path.c_str());
                return 1;
            }
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()),
                         values.end());
            inputs.push_back(std::move(values));
        }
        if (inputs.empty()) return 1;
        datasets.push_back({"dataset", std::move(inputs)});
    }
    std::puts(
        "label,cow,case,median_ns,min_ns,max_ns,allocations,initial_live_bytes,"
        "peak_live_bytes,extra_peak_bytes,checksum");
    for (const auto &dataset : datasets)
        for (auto c : cases(dataset.second)) {
            c.name = dataset.first + "/" + c.name;
            if (c.name.find(filter) != std::string::npos)
                measure(dataset.second, c, cow, iterations, label);
        }
}
