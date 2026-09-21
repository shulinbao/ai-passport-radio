// tests/test_radio_favorites.c —— 收藏集合与持久化编码的主机测试。
//
// 收藏是要写进 NVS、跨固件版本保留的数据,所以解析必须"宁可整份丢弃,也不接受
// 半截或来路不明的字节"。这里的重点是用例覆盖各种坏数据。
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "radio_favorites.h"

static void test_add_keeps_insertion_order(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);

    assert(radio_favorites_add(&fav, 7));
    assert(radio_favorites_add(&fav, 3));
    assert(radio_favorites_add(&fav, 11));
    assert(radio_favorites_count(&fav) == 3);
    assert(radio_favorites_at(&fav, 0) == 7);
    assert(radio_favorites_at(&fav, 1) == 3);
    assert(radio_favorites_at(&fav, 2) == 11);
}

static void test_add_is_idempotent_and_rejects_invalid(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);

    assert(radio_favorites_add(&fav, 5));
    // 重复添加返回 false,但不应产生第二条记录。
    assert(!radio_favorites_add(&fav, 5));
    assert(radio_favorites_count(&fav) == 1);

    // id 0 是"无效"哨兵值,不接受。
    assert(!radio_favorites_add(&fav, 0));
    assert(radio_favorites_count(&fav) == 1);
    assert(!radio_favorites_contains(&fav, 0));
}

static void test_capacity_is_enforced(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);

    for (uint16_t i = 1; i <= RADIO_FAVORITES_MAX; i++) {
        assert(radio_favorites_add(&fav, i));
    }
    assert(radio_favorites_count(&fav) == RADIO_FAVORITES_MAX);
    // 满了以后不能越界写。
    assert(!radio_favorites_add(&fav, 1000));
    assert(radio_favorites_count(&fav) == RADIO_FAVORITES_MAX);
}

static void test_remove_compacts_the_list(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);
    radio_favorites_add(&fav, 1);
    radio_favorites_add(&fav, 2);
    radio_favorites_add(&fav, 3);

    assert(radio_favorites_remove(&fav, 2));
    assert(radio_favorites_count(&fav) == 2);
    assert(radio_favorites_at(&fav, 0) == 1);
    assert(radio_favorites_at(&fav, 1) == 3);
    // 删除不存在的项返回 false。
    assert(!radio_favorites_remove(&fav, 99));
    assert(radio_favorites_count(&fav) == 2);
}

static void test_toggle(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);

    assert(radio_favorites_toggle(&fav, 42));
    assert(radio_favorites_contains(&fav, 42));
    assert(radio_favorites_toggle(&fav, 42));
    assert(!radio_favorites_contains(&fav, 42));
    assert(radio_favorites_count(&fav) == 0);
    assert(!radio_favorites_toggle(&fav, 0));
}

static void test_round_trip(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);
    radio_favorites_add(&fav, 1);
    radio_favorites_add(&fav, 513);
    radio_favorites_add(&fav, 65535);

    uint8_t blob[64];
    const size_t n = radio_favorites_serialize(&fav, blob, sizeof(blob));
    assert(n == 3 + 3 * 2);

    radio_favorites_t back;
    assert(radio_favorites_deserialize(&back, blob, n));
    assert(radio_favorites_count(&back) == 3);
    assert(radio_favorites_at(&back, 0) == 1);
    // 513 = 0x0201,专门用来验证小端字节序没写反。
    assert(radio_favorites_at(&back, 1) == 513);
    assert(radio_favorites_at(&back, 2) == 65535);
}

static void test_empty_round_trip(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);

    uint8_t blob[8];
    const size_t n = radio_favorites_serialize(&fav, blob, sizeof(blob));
    assert(n == 3);

    radio_favorites_t back;
    assert(radio_favorites_deserialize(&back, blob, n));
    assert(radio_favorites_count(&back) == 0);
}

static void test_serialize_rejects_small_buffers(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);
    radio_favorites_add(&fav, 1);

    uint8_t blob[8];
    assert(radio_favorites_serialize(&fav, blob, 4) == 0);   // 需要 5 字节
    assert(radio_favorites_serialize(&fav, NULL, sizeof(blob)) == 0);
    assert(radio_favorites_serialize(NULL, blob, sizeof(blob)) == 0);
}

static void test_deserialize_rejects_bad_data(void) {
    radio_favorites_t fav;
    radio_favorites_t out;

    // 先放一份有效数据,验证失败时会被清空而不是保留旧内容。
    radio_favorites_clear(&fav);
    radio_favorites_add(&fav, 9);

    // 太短
    assert(!radio_favorites_deserialize(&out, NULL, 0));
    const uint8_t shortbuf[2] = {0x52, 0x46};
    assert(!radio_favorites_deserialize(&out, shortbuf, sizeof(shortbuf)));

    // 魔数错
    const uint8_t badmagic[5] = {0x00, 0x46, 1, 1, 0};
    assert(!radio_favorites_deserialize(&out, badmagic, sizeof(badmagic)));

    // 版本错
    const uint8_t badver[5] = {0x52, 0x46, 99, 1, 0};
    assert(!radio_favorites_deserialize(&out, badver, sizeof(badver)));

    // 奇数长度(半个 id)
    const uint8_t odd[6] = {0x52, 0x46, 1, 1, 0, 0};
    assert(!radio_favorites_deserialize(&out, odd, sizeof(odd)));

    // id 为 0
    const uint8_t zeroid[5] = {0x52, 0x46, 1, 0, 0};
    assert(!radio_favorites_deserialize(&out, zeroid, sizeof(zeroid)));

    // 重复 id
    const uint8_t dup[7] = {0x52, 0x46, 1, 2, 0, 2, 0};
    assert(!radio_favorites_deserialize(&out, dup, sizeof(dup)));

    // 任何一次失败后 out 都必须是空集合,不能残留上一次的内容。
    assert(radio_favorites_count(&out) == 0);
    assert(!radio_favorites_contains(&out, 9));
}

static void test_deserialize_rejects_too_many_entries(void) {
    // 声明 21 条(超过上限),即使长度自洽也必须拒绝。
    uint8_t blob[3 + 21 * 2];
    blob[0] = 0x52;
    blob[1] = 0x46;
    blob[2] = 1;
    for (int i = 0; i < 21; i++) {
        const uint16_t id = (uint16_t)(i + 1);
        blob[3 + i * 2] = (uint8_t)(id & 0xFF);
        blob[3 + i * 2 + 1] = (uint8_t)(id >> 8);
    }
    radio_favorites_t out;
    assert(!radio_favorites_deserialize(&out, blob, sizeof(blob)));
    assert(radio_favorites_count(&out) == 0);

    // 恰好 20 条要能通过。
    for (int i = 0; i < RADIO_FAVORITES_MAX; i++) {
        const uint16_t id = (uint16_t)(i + 1);
        blob[3 + i * 2] = (uint8_t)(id & 0xFF);
        blob[3 + i * 2 + 1] = (uint8_t)(id >> 8);
    }
    radio_favorites_t full;
    assert(radio_favorites_deserialize(&full, blob, 3 + RADIO_FAVORITES_MAX * 2));
    assert(radio_favorites_count(&full) == RADIO_FAVORITES_MAX);
}

static void test_at_is_bounds_checked(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);
    radio_favorites_add(&fav, 4);

    assert(radio_favorites_at(&fav, 0) == 4);
    assert(radio_favorites_at(&fav, -1) == 0);
    assert(radio_favorites_at(&fav, 1) == 0);
    assert(radio_favorites_at(NULL, 0) == 0);
}

static void test_null_safety(void) {
    radio_favorites_t fav;
    radio_favorites_clear(&fav);

    radio_favorites_clear(NULL);
    assert(!radio_favorites_add(NULL, 1));
    assert(!radio_favorites_remove(NULL, 1));
    assert(!radio_favorites_toggle(NULL, 1));
    assert(!radio_favorites_contains(NULL, 1));
    assert(radio_favorites_count(NULL) == 0);
    assert(!radio_favorites_deserialize(NULL, NULL, 0));
}

int main(void) {
    test_add_keeps_insertion_order();
    test_add_is_idempotent_and_rejects_invalid();
    test_capacity_is_enforced();
    test_remove_compacts_the_list();
    test_toggle();
    test_round_trip();
    test_empty_round_trip();
    test_serialize_rejects_small_buffers();
    test_deserialize_rejects_bad_data();
    test_deserialize_rejects_too_many_entries();
    test_at_is_bounds_checked();
    test_null_safety();
    return 0;
}
