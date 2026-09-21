<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 应用字库

港／新／马网络电台应用使用的 LVGL 字库子集，由
[`tools/gen_fonts.py`](../../tools/gen_fonts.py) 生成。

## 文件说明

| 文件 | 用途 |
| --- | --- |
| `NotoSansSC-Regular.otf` | 源字库。Noto Sans SC（SIL Open Font License 1.1），7.9 MiB，SHA-256 `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`。 |
| `radio_font_cjk_16.c` | 生成的 16 px、4 bpp、未压缩子集。**唯一**一套生成字体。 |
| `inventory.json` | 完整的码点清单、源字库哈希、转换器版本，以及**明确不覆盖**的文字种类清单。 |

## 为什么只留一套，而且覆盖这么宽

界面文案是英文，因此固定文案使用 LVGL 自带 Montserrat（14 px 与 20 px），行高分别只有
16 px 与 22 px，排版紧凑。生成的中文字体**只服务于动态文本**：Wi-Fi SSID、ICY 曲目名、
导入的台名。只要字符串里出现非 ASCII 字节，`radio_font_for_text()` 就会自动改用它。

正因为它是给"用户环境里可能出现什么就是什么"兜底的，所以它覆盖**整块 CJK 统一汉字区**，
外加兼容汉字、假名、希腊字母、西里尔字母、Latin-1 与 Latin Ext-A，以及常用标点与符号区。
对源字库实测：CJK 基本区 21,087 个码点里有 21,072 个确实有字形。

早先的版本生成的是 GB2312 + Big5 一级子集，另外还有一套 20 px 繁体／标题字体。两者都已删除：
窄子集会让任何生僻字变成占位方框（邻居家 SSID 里的字并不限于常用字），而那套 20 px 字体在
界面英文化之后已经没有任何代码路径会选中它。

| 字符清单 | 字形数 | 位图负载 |
| --- | ---: | ---: |
| GB2312 + Big5 一级（旧） | 12,953 | 1,455 KiB |
| **整块 CJK 基本区等（现）** | **22,439** | **约 2.4 MiB** |

## 不覆盖的部分

源字库里没有，生成的字体自然也没有。这份清单同时记录在 `inventory.json` 里，让限制是明确的
而不是意外：

韩文（Hangul）、泰文、阿拉伯文、希伯来文、天城文、CJK 扩展 B 区及以上。

要支持这些需要再合并别的字族，那是另一个 Flash 体积决策。

## 集成方式

`main/CMakeLists.txt` 通过 `target_sources` 把生成文件编进 `main` 组件；
`main/radio_fonts.c` 持有可写描述符并挂上 Montserrat 兜底。只把生成文件复制到本目录，
并不会让它出现在屏幕上。

**行高对排版很关键**：这套 16 px 字体的 `line_height` 是 31 px（Noto Sans SC 的
ascent/descent 本身很宽）。**不要用标称字号当标签高度**，请用
`lv_font_get_line_height()` —— 界面代码就是这么做的。

## 复现方式

```bash
python tools/gen_fonts.py            # 重新生成子集
python tools/gen_fonts.py --check    # 只报告字符清单规模
python tests/test_radio_font_coverage.py
python .idf/measure_font_reach.py    # 源字库到底能覆盖多远（本地工具）
```

转换参数：`lv_font_conv 1.5.3`、`--bpp 4`、`--format lvgl`、`--no-compress`、
`--lv-include lvgl.h`。字符清单是通过 `lib/cli` 在进程内传给转换器的，因为约 24,000 个字符
拼成单个 `--symbols` 参数会远远超出 Windows 命令行 32,767 字符的上限。完整命令由
`tools/gen_fonts.py` 构造，不要手工重写一遍。

生成的 C 文件约有 18 MiB 源码文本，因此**改动到字库的完整重建需要几分钟**；只改
`main/*.c` 的增量构建很快，因为字体目标文件已经编译好了。

## 覆盖验收

`tests/test_radio_font_coverage.py` 会解析生成的字体表，在下列任一情况报错：`main/` 字符串
字面量里有码点缺失、CJK 基线覆盖率低于下限、或抽查的（刻意选择生僻但有分配的）汉字消失。
它同时断言一个"确定不存在"的码点被判定为缺失，避免检查在解析失败时变成永远通过。真机渲染
是另一件事：没有实物设备时，非拉丁文字显示无论主机测试怎么报，都仍然算未验证。
