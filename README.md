<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport Radio

An internet radio for the [FoloToy AI Passport](docs/README.md) - an ESP32-C3 wearable with a 240x320 screen, three buttons and 8 MB of flash - tuned for **Hong Kong, Singapore and Malaysia**: 49 curated stations, played straight from the broadcaster's own stream over Wi-Fi, with no phone and no browser in the loop.

![FoloToy AI Passport](docs/brand/ai-passport-front.png)

## What it does

- **49 stations in three regions** - Hong Kong (RTHK), Singapore (Mediacorp) and Malaysia (Astro Radio and RTM). A long press of up/down switches region from the station list.
- **MP3 and AAC** - both decoders are linked into the firmware. Streams are decoded to 16-bit PCM and downmixed to the single speaker path.
- **Live now-playing text** - ICY metadata is de-muxed out of the byte stream and shown on the playing page.
- **Favorites** - double press OK on a station to keep or drop it; favorites survive a reboot.
- **Screen off without stopping the music** - a double press of up/down blanks the backlight while playback and Wi-Fi keep running, and the next key press only wakes the screen instead of acting on the page. Auto screen-off does the same on a timer: off, 15 s, 30 s, 1, 2, 5 or 10 minutes.
- **Sleep timer** - 15, 30, 60 or 90 minutes. When it expires the radio stops, the peripherals are shut down in order and the device enters deep sleep; the screen shows the remaining time while it counts down.
- **Remembers where you were** - the last station is restored on boot, and it resumes playing by itself once Wi-Fi is up if it was playing when you switched off.
- **Wi-Fi setup on the device** - scanning, the network list and an on-screen keyboard are all in the firmware. No SoftAP page, no companion app.
- **Battery in the corner** - CW2017 state of charge, which quietly degrades to empty when no gauge is fitted.
- **Chinese-safe text** - the interface is English, with a generated CJK font as the fallback for network names and track titles that are not.

## Stations

| Region | Count | Stations |
| --- | --- | --- |
| Hong Kong | 6 | RTHK Radio 1, 2, 3, 4, 5 and RTHK PTH (MP3) |
| Singapore | 17 | Mediacorp: Class 95, Gold 905, YES 933, Capital 958, 987FM, CNA 938, Symphony 924, Warna 942, Oli 968, LOVE 972, Kiss92, 88.3JIA, UFM 100.3, Money FM 89.3, RIA 897, ONE FM 91.3, Hao 96.3 (MP3) |
| Malaysia | 26 | Astro Radio: Mix FM, Lite FM, Hitz FM, MY FM, Melody FM, ERA FM, Sinar FM, THR Raaga, Fly FM, Hot FM, Zayan, GoXuan, Eight FM, BFM 89.9, CITYPlus FM, Cats FM and THR Gegar (AAC-ADTS); RTM: 988 FM, Traxx FM, Ai FM, Nasional FM, Radio Klasik, Asyik FM, Minnal FM, Langkawi FM and Suria FM (MP3) |

Every address in the catalog was measured before it was committed: HTTP 200 plus at least five valid frame headers after ICY de-mux. That is what keeps out the URLs that answer 200 with an HTML page or an m3u8 playlist, which on a device look like "connected but silent".

## Controls

| Button | Click | Double click | Long press |
| --- | --- | --- | --- |
| Up / Down | move the cursor; volume on the playing page; change a value in settings | **screen off**, playback continues | switch region on the station list |
| OK | play the selected station, open a page, pause or resume, enter and leave adjust mode | favorite a station; next station while playing | back one page, or open the menu from the station list |

The screen-off gesture is intentionally inert where up/down is used for continuous movement - the station, favorites and Wi-Fi lists, settings adjustment, and while the soft keyboard is open - because the double-click window would otherwise read fast scrolling as a double click.

## Build and flash

ESP-IDF 5.5.3 is required; see [environment setup](docs/development/engineering/environment-setup.md) and [build and test](docs/development/engineering/build-and-test.md).

```bash
idf.py build                 # application image at 0x10000
idf.py -p <PORT> flash monitor
idf.py merge-bin             # single image that can be written at 0x0
```

The 8 MB layout lives in `partitions.csv`: `nvs` at `0x9000`, `phy_init` at `0xF000` and one factory application spanning the rest. Writing only the application region therefore keeps the stored Wi-Fi credentials and settings, while the merged image is what a blank device needs.

```bash
./tools/validate.sh --static     # repository checks and host tests
./tools/validate.sh --firmware   # ESP-IDF build plus merged-image verification
```

## Where the code lives

| Path | Content |
| --- | --- |
| `main/radio_*.c` | the application: catalog, player, Wi-Fi, interface, keyboard, state machine, storage |
| `main/main.c` | wiring: key dispatch, actions, periodic refresh, power and sleep |
| `components/bsp/` | board support reused from the platform: display, audio, battery, buttons |
| `assets/fonts/` | the CJK font asset, its generator and its coverage record |
| `tests/` | host tests for the state machine, catalog, favorites, frame sync and ICY de-mux |

The upstream demo pages remain in `main/demo_*.c` for reference, but they are not compiled into this application. Platform documentation - hardware, pins, BSP contracts, Wi-Fi and the validation gate - stays under [`docs/`](docs/README.md) and is worth reading before changing anything below `main/`.

## Fonts

Network names and track titles are not necessarily ASCII. `assets/fonts/radio_font_cjk_16.c` is a 16 px Noto Sans SC subset covering the CJK Unified Ideographs block, compatibility ideographs, kana and CJK punctuation, plus GB2312, Big5 level 1 and every code point used by the application. What is deliberately not covered is recorded in `assets/fonts/inventory.json`; [assets/fonts/README.md](assets/fonts/README.md) explains how to regenerate the asset from the checked-in source font.

## Status

- Verified on the board: playback from all three regions, Wi-Fi scanning and password entry through the on-screen keyboard, favorites, auto screen-off, the settings page and the playing page.
- Not yet verified on the board: the sleep timer's deep-sleep hand-off and waking afterwards, and the double-press screen-off gesture itself.

## Credits and license

Built on the [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) development baseline, MIT licensed (see [LICENSE](LICENSE)). Streams belong to their broadcasters; this project only links to the public endpoints they already publish.
