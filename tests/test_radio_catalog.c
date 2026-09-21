// tests/test_radio_catalog.c —— 内置电台目录的一致性检查。
//
// 目录是手写的数据表,最容易出的错不是逻辑而是数据:id 重复、地区字段写错、
// 空 URL、频率字段超出 FM 频段。这些错误在设备上要么表现为"某个台永远选不到",
// 要么表现为"界面显示一个收不到的频率",靠肉眼逐行看 49 条记录很不现实。
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "radio_catalog.h"
#include "radio_favorites.h"

static void test_ids_are_unique_and_nonzero(void) {
    const int total = radio_catalog_total();
    assert(total > 0);
    for (int i = 0; i < total; i++) {
        const radio_station_t *a = radio_catalog_all_at(i);
        assert(a != NULL);
        assert(a->id != 0);   // 0 是"无效 id"哨兵,收藏功能依赖它
        for (int j = i + 1; j < total; j++) {
            const radio_station_t *b = radio_catalog_all_at(j);
            assert(b != NULL);
            assert(a->id != b->id);
        }
    }
    assert(radio_catalog_all_at(-1) == NULL);
    assert(radio_catalog_all_at(total) == NULL);
}

static void test_required_fields_are_present(void) {
    const int total = radio_catalog_total();
    for (int i = 0; i < total; i++) {
        const radio_station_t *s = radio_catalog_all_at(i);
        assert(s != NULL);
        assert(s->name != NULL && s->name[0] != '\0');
        assert(s->tagline != NULL && s->tagline[0] != '\0');
        assert(s->url != NULL && s->url[0] != '\0');
        // 流地址必须是 http(s);没有协议的地址在 esp_http_client 里会直接失败。
        assert(strncmp(s->url, "http://", 7) == 0 || strncmp(s->url, "https://", 8) == 0);
        assert(s->region < RADIO_REGION_COUNT);
        assert(s->codec == RADIO_CODEC_MP3 || s->codec == RADIO_CODEC_AAC);
    }
}

static void test_region_partition_matches_total(void) {
    int sum = 0;
    for (uint8_t region = 0; region < RADIO_REGION_COUNT; region++) {
        const int count = radio_catalog_region_count(region);
        sum += count;
        for (int i = 0; i < count; i++) {
            const radio_station_t *s = radio_catalog_region_at(region, i);
            assert(s != NULL);
            assert(s->region == region);
        }
        assert(radio_catalog_region_at(region, count) == NULL);
        assert(radio_catalog_region_at(region, -1) == NULL);
    }
    assert(sum == radio_catalog_total());
}

static void test_every_region_has_stations(void) {
    // 三个地区都必须有内容:空地区会让"切换地区"看起来像坏了。
    for (uint8_t region = 0; region < RADIO_REGION_COUNT; region++) {
        assert(radio_catalog_region_count(region) >= 5);
    }
}

static void test_frequency_is_within_the_fm_band_or_absent(void) {
    const int total = radio_catalog_total();
    for (int i = 0; i < total; i++) {
        const radio_station_t *s = radio_catalog_all_at(i);
        if (s->freq_deci == 0) {
            // 0 表示"没有可信频率",界面应留空而不是编一个。
            assert(!radio_catalog_has_frequency(s));
            continue;
        }
        // 87.0 - 108.0 MHz
        assert(s->freq_deci >= 870);
        assert(s->freq_deci <= 1080);
        assert(radio_catalog_has_frequency(s));
    }
    assert(!radio_catalog_has_frequency(NULL));
}

static void test_lookup_by_id_and_url(void) {
    const int total = radio_catalog_total();
    for (int i = 0; i < total; i++) {
        const radio_station_t *s = radio_catalog_all_at(i);
        assert(radio_catalog_by_id(s->id) == s);
        assert(radio_catalog_by_url(s->url) == s);
    }
    assert(radio_catalog_by_id(0) == NULL);
    assert(radio_catalog_by_id(60000) == NULL);
    assert(radio_catalog_by_url("https://example.invalid/nope") == NULL);
    assert(radio_catalog_by_url(NULL) == NULL);
    assert(radio_catalog_by_url("") == NULL);
}

static void test_urls_are_unique(void) {
    // 两条记录共用同一个流地址通常是复制粘贴写错,会让"收藏了两个台但听起来一样"。
    const int total = radio_catalog_total();
    for (int i = 0; i < total; i++) {
        const radio_station_t *a = radio_catalog_all_at(i);
        for (int j = i + 1; j < total; j++) {
            const radio_station_t *b = radio_catalog_all_at(j);
            assert(strcmp(a->url, b->url) != 0);
        }
    }
}

static void test_resolve_ids_skips_unknown_and_keeps_order(void) {
    const uint16_t ids[] = {radio_catalog_all_at(4)->id, 59999, radio_catalog_all_at(0)->id};
    const radio_station_t *out[4] = {NULL, NULL, NULL, NULL};
    const int n = radio_catalog_resolve_ids(ids, 3, out, 4);
    assert(n == 2);
    assert(out[0] == radio_catalog_all_at(4));
    assert(out[1] == radio_catalog_all_at(0));

    // 容量不足时只写能放下的部分,不越界。
    const radio_station_t *small[1] = {NULL};
    assert(radio_catalog_resolve_ids(ids, 3, small, 1) == 1);
    assert(small[0] == radio_catalog_all_at(4));

    assert(radio_catalog_resolve_ids(NULL, 3, out, 4) == 0);
    assert(radio_catalog_resolve_ids(ids, 0, out, 4) == 0);
    assert(radio_catalog_resolve_ids(ids, 3, NULL, 4) == 0);
}

static void test_favorites_round_trip_through_the_catalog(void) {
    // 收藏存的是 id;固件升级后目录变化时,无法解析的 id 必须被安静跳过。
    radio_favorites_t fav;
    radio_favorites_clear(&fav);
    assert(radio_favorites_add(&fav, radio_catalog_all_at(1)->id));
    assert(radio_favorites_add(&fav, 54321));   // 不存在的 id
    assert(radio_favorites_add(&fav, radio_catalog_all_at(9)->id));

    const radio_station_t *out[RADIO_FAVORITES_MAX] = {0};
    int n = 0;
    for (int i = 0; i < radio_favorites_count(&fav); i++) {
        const radio_station_t *s = radio_catalog_by_id(radio_favorites_at(&fav, i));
        if (s != NULL) out[n++] = s;
    }
    assert(n == 2);
    assert(out[0] == radio_catalog_all_at(1));
    assert(out[1] == radio_catalog_all_at(9));
}

static void test_wanted_stations_are_present(void) {
    // 用户明确点名要有 Mix FM 与 Lite FM;回归时先盯住这两条。
    bool mix = false;
    bool lite = false;
    const int total = radio_catalog_total();
    for (int i = 0; i < total; i++) {
        const radio_station_t *s = radio_catalog_all_at(i);
        if (strstr(s->name, "Mix FM") != NULL && s->region == RADIO_REGION_MY) mix = true;
        if (strstr(s->name, "Lite FM") != NULL && s->region == RADIO_REGION_MY) lite = true;
    }
    assert(mix);
    assert(lite);
}

int main(void) {
    test_ids_are_unique_and_nonzero();
    test_required_fields_are_present();
    test_region_partition_matches_total();
    test_every_region_has_stations();
    test_frequency_is_within_the_fm_band_or_absent();
    test_lookup_by_id_and_url();
    test_urls_are_unique();
    test_resolve_ids_skips_unknown_and_keeps_order();
    test_favorites_round_trip_through_the_catalog();
    test_wanted_stations_are_present();
    return 0;
}
