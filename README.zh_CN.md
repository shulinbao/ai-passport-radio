<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI Passport 网络收音机

给 [FoloToy AI Passport](docs/README.md)(ESP32-C3、240x320 屏、三颗按键、8 MB Flash)写的网络收音机,面向**香港、新加坡、马来西亚**:精选 49 个电台,直接从广播方自己的流地址播放,不经过手机、也不经过浏览器。

![FoloToy AI Passport](docs/brand/ai-passport-front.png)

## 功能

- **三个地区、49 个电台** —— 香港(RTHK)、新加坡(Mediacorp)、马来西亚(Astro Radio 与 RTM)。在电台列表页长按上/下即可切换地区。
- **MP3 与 AAC 两种解码** —— 两种解码器都编进固件,解码成 16 位 PCM 后下混到单声道喇叭通路。
- **实时曲目文本** —— 从字节流里解复用 ICY 元数据,显示在播放页。
- **收藏** —— 在电台列表上双击确定即可收藏/取消;重启后仍然保留。
- **熄屏但不断音乐** —— 双击上/下只关背光,播放和 Wi-Fi 全部继续;熄屏后按任意键**只唤醒**,不会顺手触发别的功能。自动熄屏按计时做同样的事:关 / 15 秒 / 30 秒 / 1 / 2 / 5 / 10 分钟。
- **睡眠定时** —— 15 / 30 / 60 / 90 分钟。到点停止播放、按顺序关闭外设并进入深度睡眠;倒计时期间屏幕上能看到剩余时间。
- **记住上次听的台** —— 开机恢复上次的电台;如果关机时正在播放,连上 Wi-Fi 后会自动续播。
- **设备端配网** —— 扫描、网络列表、软键盘输入密码全在固件里。不需要 SoftAP 网页,也不需要手机 App。
- **右上角电量** —— CW2017 电量计;没装电量计时安静地留空,不会显示假数字。
- **中文不会变方块** —— 界面是英文,但网络名与曲目名不一定是;内置生成的中文字体作为兜底。

## 电台

| 地区 | 数量 | 电台 |
| --- | --- | --- |
| 香港 | 6 | RTHK 第一台、第二台、第三台、第四台、第五台、普通话台(MP3) |
| 新加坡 | 17 | Mediacorp:Class 95、Gold 905、YES 933、Capital 958、987FM、CNA 938、Symphony 924、Warna 942、Oli 968、LOVE 972、Kiss92、88.3JIA、UFM 100.3、Money FM 89.3、RIA 897、ONE FM 91.3、Hao 96.3(MP3) |
| 马来西亚 | 26 | Astro Radio:Mix FM、Lite FM、Hitz FM、MY FM、Melody FM、ERA FM、Sinar FM、THR Raaga、Fly FM、Hot FM、Zayan、GoXuan、Eight FM、BFM 89.9、CITYPlus FM、Cats FM、THR Gegar(AAC-ADTS);RTM:988 FM、Traxx FM、Ai FM、Nasional FM、Radio Klasik、Asyik FM、Minnal FM、Langkawi FM、Suria FM(MP3) |

目录里每一条地址都**先实测再写进固件**:HTTP 200,并且按 ICY 解复用之后能连续找到至少 5 个合法帧头。这条标准用来排除"返回 200、其实给的是 HTML 或 m3u8 播放列表"的地址 —— 那种地址在设备上的表现是"显示连上了但没声音"。

## 按键

| 按键 | 短按 | 双击 | 长按 |
| --- | --- | --- | --- |
| 上 / 下 | 移动光标;播放页调音量;设置页改数值 | **熄屏**,播放继续 | 电台列表页切换地区 |
| 确定 | 播放选中电台、进入子页、暂停/继续、进入或退出调节模式 | 列表页收藏该台;播放页切下一个台 | 返回上一层;电台列表页打开菜单 |

熄屏手势在"上下键需要连续按"的地方**故意不生效** —— 电台/收藏/Wi-Fi 列表、设置页调数值时、以及软键盘打开时:否则双击判定窗口会把"连续快按"误判成双击,变成滚着滚着屏幕黑了。

## 编译与刷机

需要 ESP-IDF 5.5.3,见[环境搭建](docs/development/engineering/environment-setup.md)与[编译与测试](docs/development/engineering/build-and-test.md)。

```bash
idf.py build                 # 应用镜像,烧到 0x10000
idf.py -p <PORT> flash monitor
idf.py merge-bin             # 可从 0x0 整片写入的合并镜像
```

8 MB 分区表在 `partitions.csv`:`nvs` 在 `0x9000`、`phy_init` 在 `0xF000`,其余全是 factory 应用。因此**只写应用分区不会丢 Wi-Fi 凭据和设置**,而全新设备用合并镜像从 `0x0` 写。

```bash
./tools/validate.sh --static     # 仓库检查 + 主机测试
./tools/validate.sh --firmware   # ESP-IDF 编译 + 合并镜像校验
```

## 代码在哪

| 路径 | 内容 |
| --- | --- |
| `main/radio_*.c` | 应用本体:目录、播放器、Wi-Fi、界面、键盘、状态机、持久化 |
| `main/main.c` | 接线层:按键分发、动作执行、周期刷新、电源与睡眠 |
| `components/bsp/` | 复用的板级支持:显示、音频、电量、按键 |
| `assets/fonts/` | 中文字体资源、生成脚本与覆盖清单 |
| `tests/` | 状态机、目录、收藏、帧同步、ICY 解复用的主机测试 |

上游的 demo 页(如 `main/demo_*.c`)保留在仓库里供参考,但**不参与本应用的编译**。平台文档(硬件、引脚、BSP 约定、Wi-Fi、校验流程)仍在 [`docs/`](docs/README.md) 下,改动 `main/` 以下的东西之前值得先读。

## 字体

网络名与曲目名不一定是 ASCII。`assets/fonts/radio_font_cjk_16.c` 是 16 px 的 Noto Sans SC 子集,覆盖 CJK 统一表意文字区、兼容表意文字、假名、中日韩标点,外加 GB2312、Big5 一级字和应用里用到的全部码位。**故意没有收录**的字符记录在 `assets/fonts/inventory.json`;从仓库内的源字体重新生成的方法见 [assets/fonts/README.zh_CN.md](assets/fonts/README.zh_CN.md)。

## 状态

- **已在真机确认**:三个地区的播放、Wi-Fi 扫描与软键盘输密码、收藏、自动熄屏、设置页与播放页。
- **尚未在真机确认**:睡眠定时到点后的深睡与之后的开机、以及双击上/下熄屏这个手势本身。

## 致谢与许可

基于 [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport) 开发基线构建,MIT 许可(见 [LICENSE](LICENSE))。音频流归各广播方所有;本项目只链接它们已公开的地址。
