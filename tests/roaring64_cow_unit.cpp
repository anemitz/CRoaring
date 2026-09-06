#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include <roaring/containers/containers.h>
#include <roaring/roaring64.hh>

#include "test.h"

using namespace roaring::api;
namespace {
using Bitmap =
    std::unique_ptr<roaring64_bitmap_t, decltype(&roaring64_bitmap_free)>;
Bitmap own(roaring64_bitmap_t *r) {
    assert_non_null(r);
    return Bitmap(r, roaring64_bitmap_free);
}
void valid(const roaring64_bitmap_t *r) {
    const char *reason = nullptr;
    if (!roaring64_bitmap_internal_validate(r, &reason)) fail_msg("%s", reason);
}
std::vector<uint64_t> values(const roaring64_bitmap_t *r) {
    valid(r);
    std::vector<uint64_t> out(roaring64_bitmap_get_cardinality(r));
    roaring64_bitmap_to_uint64_array(r, out.data());
    return out;
}
void same(const roaring64_bitmap_t *a, const roaring64_bitmap_t *b) {
    assert_true(values(a) == values(b));
    assert_true(roaring64_bitmap_equals(a, b));
}
void cow(roaring64_bitmap_t *r, bool enabled = true) {
    assert_true(roaring64_bitmap_set_copy_on_write(r, enabled));
    assert_int_equal(roaring64_bitmap_get_copy_on_write(r), enabled);
    valid(r);
}
uint8_t type_at(const roaring64_bitmap_t *r, uint64_t value) {
    roaring64_bulk_context_t context = {};
    assert_true(roaring64_bitmap_contains_bulk(r, &context, value));
    return (uint8_t)*context.leaf;
}
// Multiple ART shapes, all container types, and the unsigned upper boundary.
Bitmap fixture(unsigned variant = 0) {
    auto r = own(roaring64_bitmap_create());
    for (uint64_t prefix :
         {UINT64_C(0), UINT64_C(1) << 40, UINT64_C(1) << 63}) {
        for (uint64_t v = variant; v < 10000; v += 2)
            roaring64_bitmap_add(r.get(), prefix + v);
        roaring64_bitmap_add_range(r.get(), prefix + 65536 + variant,
                                   prefix + 65536 + 2000);
        for (uint64_t v : {UINT64_C(3), UINT64_C(100), UINT64_C(60000)})
            roaring64_bitmap_add(r.get(), prefix + 2 * 65536 + v + variant);
    }
    roaring64_bitmap_add(r.get(), UINT64_MAX - variant);
    roaring64_bitmap_run_optimize(r.get());
    valid(r.get());
    return r;
}

DEFINE_TEST(copy_and_mutations) {
    using Mutation = void (*)(roaring64_bitmap_t *);
    const Mutation mutations[] = {
        [](roaring64_bitmap_t *r) { roaring64_bitmap_add(r, 1); },
        [](roaring64_bitmap_t *r) { roaring64_bitmap_remove(r, 2); },
        [](roaring64_bitmap_t *r) {
            assert_true(roaring64_bitmap_add_checked(r, 1));
        },
        [](roaring64_bitmap_t *r) {
            assert_true(roaring64_bitmap_remove_checked(r, 2));
        },
        [](roaring64_bitmap_t *r) {
            uint64_t v[] = {1, 3, 65536 + 3000, (UINT64_C(1) << 40) + 1};
            roaring64_bitmap_add_many(r, 4, v);
        },
        [](roaring64_bitmap_t *r) {
            uint64_t v[] = {0, 2, 65536, 65537, UINT64_MAX, UINT64_MAX};
            roaring64_bitmap_remove_many(r, 6, v);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_add_range_closed(r, 9000, 140000);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_remove_range_closed(r, 9000, 140000);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_remove_range(r, 0, 20000);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_flip_inplace(r, 0, 65536);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_flip_closed_inplace(r, 100, 140000);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_flip_closed_inplace(r, UINT64_MAX, UINT64_MAX);
        },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_remove_run_compression(r);
        },
        [](roaring64_bitmap_t *r) { roaring64_bitmap_run_optimize(r); },
        [](roaring64_bitmap_t *r) { roaring64_bitmap_shrink_to_fit(r); },
        [](roaring64_bitmap_t *r) { cow(r, false); },
        [](roaring64_bitmap_t *r) { roaring64_bitmap_clear(r); },
    };
    for (Mutation mutate : mutations) {
        for (int owners : {1, 2, 4}) {
            auto source = fixture();
            auto expected = own(roaring64_bitmap_copy(source.get()));
            auto original = values(source.get());
            cow(source.get());
            std::vector<Bitmap> copies;
            for (int i = 1; i < owners + 1; ++i)
                copies.push_back(own(roaring64_bitmap_copy(source.get())));
            assert_int_equal(type_at(source.get(), 0), SHARED_CONTAINER_TYPE);
            assert_int_equal(type_at(source.get(), 65536),
                             SHARED_CONTAINER_TYPE);
            assert_int_equal(type_at(source.get(), 131075),
                             SHARED_CONTAINER_TYPE);
            // owners == 1 exercises extracting the last reference with no
            // clone.
            copies.pop_back();
            mutate(source.get());
            mutate(expected.get());
            same(source.get(), expected.get());
            for (const auto &copy : copies)
                assert_true(values(copy.get()) == original);
        }
    }
    // Mutate the copy first, then destroy the source first.
    auto source = fixture();
    cow(source.get());
    auto copy = own(roaring64_bitmap_copy(source.get()));
    roaring64_bitmap_add(copy.get(), 1);
    assert_false(roaring64_bitmap_contains(source.get(), 1));
    source.reset();
    roaring64_bitmap_remove(copy.get(), 65536);
    valid(copy.get());
}

DEFINE_TEST(mixed_set_operations) {
    using Binary = roaring64_bitmap_t *(*)(const roaring64_bitmap_t *,
                                           const roaring64_bitmap_t *);
    using Inplace = void (*)(roaring64_bitmap_t *, const roaring64_bitmap_t *);
    Binary binary[] = {roaring64_bitmap_and, roaring64_bitmap_or,
                       roaring64_bitmap_xor, roaring64_bitmap_andnot};
    Inplace inplace[] = {
        roaring64_bitmap_and_inplace, roaring64_bitmap_or_inplace,
        roaring64_bitmap_xor_inplace, roaring64_bitmap_andnot_inplace};
    for (bool left_cow : {false, true})
        for (bool right_cow : {false, true}) {
            for (int shape = 0; shape < 3; ++shape)
                for (int op = 0; op < 4; ++op) {
                    auto a = fixture(), b = fixture(1);
                    if (shape == 1) {
                        roaring64_bitmap_clear(b.get());
                        roaring64_bitmap_add(b.get(), UINT64_C(1) << 56);
                    }
                    if (shape == 2) roaring64_bitmap_clear(a.get());
                    auto expected = own(binary[op](a.get(), b.get()));
                    auto av = values(a.get()), bv = values(b.get());
                    cow(a.get(), left_cow);
                    cow(b.get(), right_cow);
                    auto keep_a = own(roaring64_bitmap_copy(a.get()));
                    auto keep_b = own(roaring64_bitmap_copy(b.get()));
                    auto result = own(binary[op](a.get(), b.get()));
                    assert_int_equal(
                        roaring64_bitmap_get_copy_on_write(result.get()),
                        left_cow && right_cow);
                    same(result.get(), expected.get());
                    inplace[op](a.get(), b.get());
                    same(a.get(), expected.get());
                    assert_int_equal(
                        roaring64_bitmap_get_copy_on_write(a.get()), left_cow);
                    assert_true(values(keep_a.get()) == av);
                    assert_true(values(keep_b.get()) == bv);
                    assert_true(values(b.get()) == bv);
                    roaring64_bitmap_add(result.get(), 777);
                    roaring64_bitmap_add(a.get(), 779);
                    valid(result.get());
                    valid(a.get());
                    valid(b.get());
                }
        }
    // Distinct bitmaps sharing exactly the same containers (empty XOR/ANDNOT).
    for (int op = 0; op < 4; ++op) {
        auto a = fixture();
        cow(a.get());
        auto b = own(roaring64_bitmap_copy(a.get()));
        auto expected = own(binary[op](a.get(), b.get()));
        inplace[op](a.get(), b.get());
        same(a.get(), expected.get());
        valid(b.get());
    }
}

DEFINE_TEST(unary_and_transfer) {
    auto plain = fixture(), source = fixture();
    cow(source.get());
    auto keep = own(roaring64_bitmap_copy(source.get()));
    for (uint64_t offset :
         {UINT64_C(0), UINT64_C(1), UINT64_C(65536), UINT64_MAX}) {
        for (bool positive : {false, true}) {
            auto got = own(roaring64_bitmap_add_offset_signed(
                source.get(), positive, offset));
            auto expected = own(roaring64_bitmap_add_offset_signed(
                plain.get(), positive, offset));
            assert_true(roaring64_bitmap_get_copy_on_write(got.get()));
            same(got.get(), expected.get());
        }
    }
    auto flipped = own(roaring64_bitmap_flip(source.get(), 100, 140000));
    auto expected = own(roaring64_bitmap_flip(plain.get(), 100, 140000));
    assert_true(roaring64_bitmap_get_copy_on_write(flipped.get()));
    same(flipped.get(), expected.get());
    for (bool enabled : {false, true}) {
        auto dest = fixture(1);
        cow(dest.get(), enabled);
        auto old_copy = own(roaring64_bitmap_copy(dest.get()));
        roaring64_bitmap_overwrite(dest.get(), source.get());
        assert_true(roaring64_bitmap_get_copy_on_write(dest.get()));
        same(dest.get(), source.get());
        roaring64_bitmap_overwrite(dest.get(), plain.get());
        assert_false(roaring64_bitmap_get_copy_on_write(dest.get()));
        same(dest.get(), plain.get());
        valid(old_copy.get());
    }
    same(source.get(), plain.get());
    same(keep.get(), plain.get());
    roaring_bitmap_t *r32 = roaring_bitmap_from_range(0, 100, 2);
    roaring_bitmap_set_copy_on_write(r32, true);
    roaring_bitmap_t *copy32 = roaring_bitmap_copy(r32);
    auto moved = own(roaring64_bitmap_move_from_roaring32(r32));
    assert_true(roaring64_bitmap_get_copy_on_write(moved.get()));
    roaring64_bitmap_add(moved.get(), 1);
    assert_false(roaring_bitmap_contains(copy32, 1));
    valid(moved.get());
    roaring_bitmap_free(r32);
    roaring_bitmap_free(copy32);
}

DEFINE_TEST(mixed_container_operations) {
    auto make = [](int type, int variant) {
        auto r = own(roaring64_bitmap_create());
        for (uint64_t k = 0; k < 8; ++k) {
            uint64_t base = k * UINT64_C(0x4000010000);
            if (variant && k % 3 == 0) base += UINT64_C(0x80000000000000);
            if (type == 2) {
                roaring64_bitmap_add_range_closed(r.get(), base + variant * 10,
                                                  base + 7000);
            } else {
                for (uint64_t i = variant; i < (type == 0 ? 300 : 16000);
                     i += 2)
                    roaring64_bitmap_add(r.get(), base + i);
            }
        }
        if (type == 2) roaring64_bitmap_run_optimize(r.get());
        return r;
    };
    using Binary = roaring64_bitmap_t *(*)(const roaring64_bitmap_t *,
                                           const roaring64_bitmap_t *);
    using Inplace = void (*)(roaring64_bitmap_t *, const roaring64_bitmap_t *);
    Binary binary[] = {roaring64_bitmap_and, roaring64_bitmap_or,
                       roaring64_bitmap_xor, roaring64_bitmap_andnot};
    Inplace inplace[] = {
        roaring64_bitmap_and_inplace, roaring64_bitmap_or_inplace,
        roaring64_bitmap_xor_inplace, roaring64_bitmap_andnot_inplace};
    // Every concrete container pairing, flag pairing, and set operation.
    for (int left_type = 0; left_type < 3; ++left_type)
        for (int right_type = 0; right_type < 3; ++right_type)
            for (int flags = 0; flags < 4; ++flags)
                for (int op = 0; op < 4; ++op) {
                    auto a = make(left_type, 0), b = make(right_type, 1);
                    auto original_a = values(a.get()),
                         original_b = values(b.get());
                    auto expected = own(binary[op](a.get(), b.get()));
                    cow(a.get(), (flags & 1) != 0);
                    cow(b.get(), (flags & 2) != 0);
                    auto keep_a = own(roaring64_bitmap_copy(a.get()));
                    auto keep_b = own(roaring64_bitmap_copy(b.get()));
                    auto out = own(binary[op](a.get(), b.get()));
                    same(out.get(), expected.get());
                    assert_int_equal(
                        roaring64_bitmap_get_copy_on_write(out.get()),
                        flags == 3);
                    inplace[op](a.get(), b.get());
                    same(a.get(), expected.get());
                    assert_int_equal(
                        roaring64_bitmap_get_copy_on_write(a.get()),
                        (flags & 1) != 0);
                    assert_true(values(b.get()) == original_b);
                    a.reset();
                    b.reset();
                    out.reset();
                    assert_true(values(keep_a.get()) == original_a);
                    assert_true(values(keep_b.get()) == original_b);
                }
}

void check_iterator_batches(const roaring64_bitmap_t *r) {
    auto expected = values(r);
    for (unsigned chunk : {1u, 7u, 128u, 8192u}) {
        auto *it = roaring64_iterator_create(r);
        assert_non_null(it);
        std::vector<uint64_t> got, buf(chunk);
        uint64_t count;
        while ((count = roaring64_iterator_read(it, buf.data(), chunk)) != 0)
            got.insert(got.end(), buf.begin(), buf.begin() + count);
        assert_true(got == expected);
        got.clear();
        roaring64_iterator_reinit_last(r, it);
        while ((count = roaring64_iterator_read_backward(it, buf.data(),
                                                         chunk)) != 0)
            got.insert(got.end(), buf.begin(), buf.begin() + count);
        std::reverse(got.begin(), got.end());
        assert_true(got == expected);
        for (bool backward : {false, true}) {
            got.clear();
            if (backward)
                roaring64_iterator_reinit_last(r, it);
            else
                roaring64_iterator_reinit(r, it);
            std::vector<roaring64_range_closed_t> ranges(chunk);
            size_t n;
            while ((n = backward ? roaring64_iterator_read_prev_ranges(
                                       it, ranges.data(), chunk)
                                 : roaring64_iterator_read_ranges(
                                       it, ranges.data(), chunk)) != 0) {
                for (size_t i = 0; i < n; ++i) {
                    uint64_t value = backward ? ranges[i].max : ranges[i].min;
                    uint64_t end = backward ? ranges[i].min : ranges[i].max;
                    for (;;) {
                        got.push_back(value);
                        if (value == end) break;
                        if (backward)
                            --value;
                        else
                            ++value;
                    }
                }
            }
            if (backward) std::reverse(got.begin(), got.end());
            assert_true(got == expected);
        }
        roaring64_iterator_free(it);
    }
}

DEFINE_TEST(iterator_batches) {
    auto source = fixture();
    // Include runs crossing container boundaries and the unsigned endpoint.
    roaring64_bitmap_add_range(source.get(), 65530, 131090);
    roaring64_bitmap_add_range_closed(source.get(), UINT64_MAX - 100,
                                      UINT64_MAX);
    roaring64_bitmap_run_optimize(source.get());
    cow(source.get());
    auto copy = own(roaring64_bitmap_copy(source.get()));
    check_iterator_batches(source.get());
    check_iterator_batches(copy.get());
    roaring64_bitmap_remove(source.get(), 2);
    roaring64_bitmap_add(copy.get(), 1);
    check_iterator_batches(source.get());
    check_iterator_batches(copy.get());
    source.reset();
    check_iterator_batches(copy.get());
}

void check_iterator_interleavings(const roaring64_bitmap_t *r) {
    auto expected = values(r);
    const int64_t size = (int64_t)expected.size();
    assert_true(size > 0);
    int64_t position = 0;
    auto *it = roaring64_iterator_create(r);
    assert_non_null(it);
    std::mt19937_64 random(42);
    for (int step = 0; step < 3000; ++step) {
        uint64_t buffer[10];
        unsigned count = random() % 10;
        switch (random() % 8) {
            case 0:
                roaring64_iterator_advance(it);
                position = std::min(position + 1, size);
                break;
            case 1:
                roaring64_iterator_previous(it);
                position = std::max(position - 1, INT64_C(-1));
                break;
            case 2: {
                uint64_t got = roaring64_iterator_read(it, buffer, count);
                int64_t take = position >= 0 && position < size
                                   ? std::min<int64_t>(count, size - position)
                                   : 0;
                assert_int_equal(got, take);
                for (int64_t j = 0; j < take; ++j)
                    assert_int_equal(buffer[j], expected[position + j]);
                position += take;
                break;
            }
            case 3: {
                uint64_t got =
                    roaring64_iterator_read_backward(it, buffer, count);
                int64_t take = position >= 0 && position < size
                                   ? std::min<int64_t>(count, position + 1)
                                   : 0;
                assert_int_equal(got, take);
                for (int64_t j = 0; j < take; ++j)
                    assert_int_equal(buffer[j], expected[position - j]);
                position -= take;
                break;
            }
            case 4: {
                uint64_t target = expected[random() % size] + random() % 5;
                roaring64_iterator_move_equalorlarger(it, target);
                position =
                    std::lower_bound(expected.begin(), expected.end(), target) -
                    expected.begin();
                break;
            }
            case 5: {
                roaring64_range_closed_t range;
                size_t got = roaring64_iterator_read_ranges(it, &range, 1);
                if (position < 0 || position >= size) {
                    assert_int_equal(got, 0);
                    break;
                }
                assert_int_equal(got, 1);
                assert_int_equal(range.min, expected[position]);
                while (position + 1 < size &&
                       expected[position] != UINT64_MAX &&
                       expected[position + 1] == expected[position] + 1)
                    ++position;
                assert_int_equal(range.max, expected[position]);
                ++position;
                break;
            }
            case 6: {
                roaring64_range_closed_t range;
                size_t got = roaring64_iterator_read_prev_ranges(it, &range, 1);
                if (position < 0 || position >= size) {
                    assert_int_equal(got, 0);
                    break;
                }
                assert_int_equal(got, 1);
                assert_int_equal(range.max, expected[position]);
                while (position > 0 && expected[position - 1] != UINT64_MAX &&
                       expected[position] == expected[position - 1] + 1)
                    --position;
                assert_int_equal(range.min, expected[position]);
                --position;
                break;
            }
            case 7: {
                auto *copy = roaring64_iterator_copy(it);
                assert_non_null(copy);
                roaring64_iterator_free(it);
                it = copy;
                break;
            }
        }
        assert_int_equal(roaring64_iterator_has_value(it),
                         position >= 0 && position < size);
        if (position >= 0 && position < size)
            assert_int_equal(roaring64_iterator_value(it), expected[position]);
    }
    roaring64_iterator_free(it);
}

DEFINE_TEST(iterator_interleavings) {
    auto source = own(roaring64_bitmap_create());
    for (uint64_t key :
         {UINT64_C(0), UINT64_C(1), UINT64_C(8), UINT64_C(1) << 32})
        for (uint64_t value = 0; value < 100; ++value)
            if (value % 7 < 4)
                roaring64_bitmap_add(source.get(), (key << 16) + value);
    roaring64_bitmap_add_range_closed(source.get(), UINT64_MAX - 20,
                                      UINT64_MAX);
    check_iterator_interleavings(source.get());
    cow(source.get());
    auto copy = own(roaring64_bitmap_copy(source.get()));
    check_iterator_interleavings(source.get());
    check_iterator_interleavings(copy.get());
#if !CROARING_IS_BIG_ENDIAN
    // A valid portable view may have unaligned payloads. Exercise seeking,
    // direction changes, batch/range reads, saturation, and iterator copies.
    size_t size = roaring64_bitmap_portable_size_in_bytes(source.get());
    std::vector<char> storage(size + 1);
    char *buffer = storage.data() + 1;
    assert_int_equal(roaring64_bitmap_portable_serialize(source.get(), buffer),
                     size);
    auto frozen =
        own(roaring64_bitmap_portable_deserialize_frozen(buffer, size));
    check_iterator_interleavings(frozen.get());
#endif
    source.reset();
    check_iterator_interleavings(copy.get());
}

DEFINE_TEST(iterators_and_serialization) {
    auto source = fixture();
    auto original = values(source.get());
    std::vector<char> plain(
        roaring64_bitmap_portable_size_in_bytes(source.get()));
    roaring64_bitmap_portable_serialize(source.get(), plain.data());
    cow(source.get());
    auto copy = own(roaring64_bitmap_copy(source.get()));
    std::vector<char> shared(
        roaring64_bitmap_portable_size_in_bytes(source.get()));
    assert_int_equal(
        roaring64_bitmap_portable_serialize(source.get(), shared.data()),
        shared.size());
    assert_true(plain == shared);
    auto roundtrip = own(roaring64_bitmap_portable_deserialize_safe(
        shared.data(), shared.size()));
    assert_false(roaring64_bitmap_get_copy_on_write(roundtrip.get()));
    same(roundtrip.get(), source.get());
    auto *it = roaring64_iterator_create(source.get());
    for (uint64_t value : original) {
        assert_true(roaring64_iterator_has_value(it));
        assert_int_equal(roaring64_iterator_value(it), value);
        roaring64_iterator_advance(it);
    }
    assert_false(roaring64_iterator_has_value(it));
    roaring64_iterator_reinit_last(source.get(), it);
    for (auto pos = original.rbegin(); pos != original.rend(); ++pos) {
        assert_int_equal(roaring64_iterator_value(it), *pos);
        roaring64_iterator_previous(it);
    }
    for (size_t n = 0; n < original.size(); n += 997) {
        uint64_t value = original[n], selected = 0, index = 0;
        assert_true(roaring64_iterator_move_equalorlarger(it, value));
        assert_int_equal(roaring64_iterator_value(it), value);
        assert_int_equal(roaring64_bitmap_rank(source.get(), value), n + 1);
        assert_true(roaring64_bitmap_select(source.get(), n, &selected));
        assert_int_equal(selected, value);
        assert_true(roaring64_bitmap_get_index(source.get(), value, &index));
        assert_int_equal(index, n);
    }
    roaring64_iterator_free(it);
    same(source.get(), copy.get());
}

DEFINE_TEST(frozen_lifetimes) {
    for (bool portable : {false, true}) {
#if CROARING_IS_BIG_ENDIAN
        if (portable) continue;
#endif
        auto source = fixture();
        cow(source.get());
        // Shrink first, then share: exercise actual SHARED leaf tags during
        // frozen serialization, rather than detaching them by shrinking.
        roaring64_bitmap_shrink_to_fit(source.get());
        auto shared_copy = own(roaring64_bitmap_copy(source.get()));
        size_t size =
            portable ? roaring64_bitmap_portable_size_in_bytes(source.get())
                     : roaring64_bitmap_frozen_size_in_bytes(source.get());
        char *buffer = (char *)roaring_aligned_malloc(64, size);
        assert_non_null(buffer);
        size_t written =
            portable ? roaring64_bitmap_portable_serialize(source.get(), buffer)
                     : roaring64_bitmap_frozen_serialize(source.get(), buffer);
        assert_int_equal(written, size);
        if (!portable) {
            std::vector<char> unaligned(size + 8);
            char *p = unaligned.data();
            while ((uintptr_t)p % 8 == 0) ++p;
            assert_int_equal(roaring64_bitmap_frozen_serialize(source.get(), p),
                             size);
            assert_memory_equal(buffer, p, size);
        }
        auto view = own(portable ? roaring64_bitmap_portable_deserialize_frozen(
                                       buffer, size)
                                 : roaring64_bitmap_frozen_view(buffer, size));
        if (!portable)
            assert_true(roaring64_bitmap_get_copy_on_write(view.get()));
        cow(view.get());  // both frozen representations must reject sharing
        same(view.get(), source.get());
        {
            auto copy = own(roaring64_bitmap_copy(view.get()));
            same(copy.get(), source.get());
        }
        same(view.get(), source.get());
        auto survivor = own(roaring64_bitmap_copy(view.get()));
        auto overwritten = own(roaring64_bitmap_create());
        roaring64_bitmap_overwrite(overwritten.get(), view.get());
        auto empty = own(roaring64_bitmap_create());
        cow(empty.get());
        auto united = own(roaring64_bitmap_or(empty.get(), view.get()));
        roaring64_bitmap_or_inplace(empty.get(), view.get());
        auto offset = own(roaring64_bitmap_add_offset(view.get(), 65536));
        auto expected_offset =
            own(roaring64_bitmap_add_offset(source.get(), 65536));
        view.reset();
        roaring_aligned_free(buffer);
        for (auto *r :
             {survivor.get(), overwritten.get(), united.get(), empty.get()}) {
            same(r, source.get());
            roaring64_bitmap_add(r, 1);
            valid(r);
        }
        same(offset.get(), expected_offset.get());
        same(source.get(), shared_copy.get());
    }
}

DEFINE_TEST(randomized_copies) {
    std::mt19937_64 random(12345);
    std::vector<Bitmap> bitmaps;
    std::vector<std::set<uint64_t>> expected(4);
    for (int i = 0; i < 4; ++i)
        bitmaps.push_back(own(roaring64_bitmap_create()));
    for (int step = 0; step < 1200; ++step) {
        size_t a = random() % 4, b = (a + 1 + random() % 3) % 4;
        uint64_t prefix = (random() % 4) << 40;
        uint64_t value = prefix + random() % 200;
        switch (random() % 7) {
            case 0:
                cow(bitmaps[a].get(), random() % 2);
                break;
            case 1:
                bitmaps[a] = own(roaring64_bitmap_copy(bitmaps[b].get()));
                expected[a] = expected[b];
                break;
            case 2:
                roaring64_bitmap_add(bitmaps[a].get(), value);
                expected[a].insert(value);
                break;
            case 3:
                roaring64_bitmap_remove(bitmaps[a].get(), value);
                expected[a].erase(value);
                break;
            case 4:
                roaring64_bitmap_flip_inplace(bitmaps[a].get(), value,
                                              value + 20);
                for (uint64_t v = value; v < value + 20; ++v)
                    if (!expected[a].erase(v)) expected[a].insert(v);
                break;
            case 5:
                roaring64_bitmap_or_inplace(bitmaps[a].get(), bitmaps[b].get());
                expected[a].insert(expected[b].begin(), expected[b].end());
                break;
            case 6:
                roaring64_bitmap_run_optimize(bitmaps[a].get());
                roaring64_bitmap_shrink_to_fit(bitmaps[a].get());
                break;
        }
        for (size_t i = 0; i < 4; ++i)
            assert_true(
                values(bitmaps[i].get()) ==
                std::vector<uint64_t>(expected[i].begin(), expected[i].end()));
    }
}

// Inject a single failure into the new wrapper/detachment allocations.
// Hooks remain installed for this standalone test process and use the same
// allocator for every bitmap lifetime.
long fail_after = -1;
void *cow_malloc(size_t size) {
    if (fail_after == 0) {
        fail_after = -1;
        return nullptr;
    }
    if (fail_after > 0) --fail_after;
    return std::malloc(size);
}
void *test_aligned_malloc(size_t alignment, size_t size) {
#ifdef _MSC_VER
    return _aligned_malloc(size, alignment);
#else
    void *p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
}
void test_aligned_free(void *p) {
#ifdef _MSC_VER
    _aligned_free(p);
#else
    std::free(p);
#endif
}
DEFINE_TEST(allocation_failure) {
    auto source = fixture();
    cow(source.get());
    auto original = values(source.get());
    fail_after = 1;  // Bitmap allocation succeeds; first shared wrapper fails.
    assert_null(roaring64_bitmap_copy(source.get()));
    assert_true(values(source.get()) == original);
    assert_int_equal(type_at(source.get(), 0), BITSET_CONTAINER_TYPE);
    auto copy = own(roaring64_bitmap_copy(source.get()));
    fail_after = 0;  // Detachment must not release its reference on failure.
    assert_false(roaring64_bitmap_set_copy_on_write(source.get(), false));
    assert_true(roaring64_bitmap_get_copy_on_write(source.get()));
    same(source.get(), copy.get());
    cow(source.get(), false);
    copy.reset();
    assert_true(values(source.get()) == original);
}

DEFINE_TEST(iterator_reinit_after_storage_changes) {
    using ChangeStorage = void (*)(roaring64_bitmap_t *);
    const ChangeStorage changes[] = {
        [](roaring64_bitmap_t *r) { roaring64_bitmap_shrink_to_fit(r); },
        [](roaring64_bitmap_t *r) { roaring64_bitmap_run_optimize(r); },
        [](roaring64_bitmap_t *r) {
            roaring64_bitmap_remove_run_compression(r);
        },
        [](roaring64_bitmap_t *r) { cow(r, false); },
    };
    for (bool enabled : {false, true}) {
        for (auto change : changes) {
            auto source = fixture();
            cow(source.get(), enabled);
            auto sibling = own(roaring64_bitmap_copy(source.get()));
            auto expected = values(source.get());
            auto *it = roaring64_iterator_create(source.get());
            auto *sibling_it = roaring64_iterator_create(sibling.get());
            change(source.get());
            assert_true(values(source.get()) == expected);

            // Storage changes invalidate it even when every value is unchanged.
            // Reinitialization must discard both ART and payload cache state.
            roaring64_iterator_reinit(source.get(), it);
            for (uint64_t value : expected) {
                assert_true(roaring64_iterator_has_value(it));
                assert_int_equal(roaring64_iterator_value(it), value);
                roaring64_iterator_advance(it);
                // Mutation of the other owner must leave this iterator valid.
                assert_true(roaring64_iterator_has_value(sibling_it));
                assert_int_equal(roaring64_iterator_value(sibling_it), value);
                roaring64_iterator_advance(sibling_it);
            }
            assert_false(roaring64_iterator_has_value(it));
            assert_false(roaring64_iterator_has_value(sibling_it));
            roaring64_iterator_reinit_last(source.get(), it);
            for (auto value = expected.rbegin(); value != expected.rend();
                 ++value) {
                assert_true(roaring64_iterator_has_value(it));
                assert_int_equal(roaring64_iterator_value(it), *value);
                roaring64_iterator_previous(it);
            }
            assert_false(roaring64_iterator_has_value(it));
            roaring64_iterator_free(it);
            roaring64_iterator_free(sibling_it);
        }
        // Keep the ART compact, then grow only the array payload. A subsequent
        // shrink can free that payload while leaving the ART leaf unchanged.
        auto array = own(roaring64_bitmap_create());
        for (uint64_t i = 0; i < 40; ++i)
            roaring64_bitmap_add(array.get(), i * 2);
        roaring64_bitmap_shrink_to_fit(array.get());
        for (uint64_t i = 40; i < 45; ++i)
            roaring64_bitmap_add(array.get(), i * 2);
        cow(array.get(), enabled);
        auto sibling = own(roaring64_bitmap_copy(array.get()));
        auto *it =
            roaring64_iterator_create(array.get());  // Prime ARRAY cache.
        roaring64_bitmap_shrink_to_fit(array.get());
        roaring64_iterator_reinit(array.get(), it);
        for (uint64_t i = 0; i < 45; ++i) {
            assert_true(roaring64_iterator_has_value(it));
            assert_int_equal(roaring64_iterator_value(it), i * 2);
            roaring64_iterator_advance(it);
        }
        assert_false(roaring64_iterator_has_value(it));
        same(array.get(), sibling.get());
        roaring64_iterator_free(it);
    }
}

DEFINE_TEST(cpp_forwarding) {
    roaring::Roaring64 a{1, 2, UINT64_C(1) << 50};
    assert_false(a.getCopyOnWrite());
    a.setCopyOnWrite(true);
    auto b = a;
    roaring::Roaring64 c;
    c = a;
    assert_true(b.getCopyOnWrite());
    assert_true(c.getCopyOnWrite());
    b.add(3);
    c.remove(1);
    assert_false(a.contains(3));
    assert_true(a.contains(1));
    b.setCopyOnWrite(false);
    assert_false(b.getCopyOnWrite());
}
}  // namespace
int main() {
    roaring_memory_t hooks = {cow_malloc,          std::realloc,
                              std::calloc,         std::free,
                              test_aligned_malloc, test_aligned_free};
    roaring_init_memory_hook(hooks);
    const CMUnitTest tests[] = {
        cmocka_unit_test(copy_and_mutations),
        cmocka_unit_test(mixed_set_operations),
        cmocka_unit_test(mixed_container_operations),
        cmocka_unit_test(iterator_batches),
        cmocka_unit_test(iterator_interleavings),
        cmocka_unit_test(iterator_reinit_after_storage_changes),
        cmocka_unit_test(unary_and_transfer),
        cmocka_unit_test(iterators_and_serialization),
        cmocka_unit_test(frozen_lifetimes),
        cmocka_unit_test(randomized_copies),
        cmocka_unit_test(cpp_forwarding),
        cmocka_unit_test(allocation_failure),
    };
    return cmocka_run_group_tests(tests, nullptr, nullptr);
}
