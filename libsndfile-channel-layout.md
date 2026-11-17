# WAVEX 与 CAF 多声道布局解析差异报告

- 代码基线：`task/15` 分支，commit `396a715`（`Drop pre-C99 compatibility leftovers`）
- 库版本：`libsndfile-1.2.2`（本地 CMake 构建，`-DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF`，不依赖任何外部编解码器）
- 测试基线：`ctest -j8` → **144/144 通过**（含 `command_test`、`channel_test`）

本报告只讨论容器层的**声道布局元数据**，不涉及音视频编解码实现。文中"当前行为"均来自本分支的实测输出，"建议"另行标注，二者不混写。

---

## 1. 结论摘要

| # | 结论 | 性质 |
|:--|:--|:--|
| 1 | 两种格式都归一到同一条公共接口 `SFC_GET/SET_CHANNEL_MAP_INFO` + `SF_PRIVATE.channel_map`（`int[channels]` 的 `SF_CHANNEL_MAP_*`） | 设计 |
| 2 | WAVEX 用**位掩码**表达"存在哪些声道"，位置由 `channel_mask_bits[]` 表序固定；掩码本身**不携带**样本排列顺序 | 设计 |
| 3 | CAF/AIFF 用**布局 tag** 表达"存在哪些声道**且**按什么顺序排列"；同一组扬声器可有多个 tag（如 MPEG 5.1 A/B） | 设计 |
| 4 | WAVEX 与 CAF 中"可写"的布局集合**不相等**，且同一请求在两个容器上的返回值可以不同 | 当前行为 |
| 5 | 布局**不影响**帧数；真正影响帧数的是声道数字段与数据长度。布局不一致时没有任何校验，只会导致标签错位 | 当前行为 |
| 6 | 读侧对"位数/条目数少于声道数"采取**部分成功**（`GET` 返回真，缺位填 `SF_CHANNEL_MAP_INVALID=0`）：WAVEX 打 `***` 日志，CAF **完全无告警** | 当前行为 |
| 7 | 写侧失败时 `psf->channel_map` 已被改写且**不回滚**，且容器侧会把先前成功的布局一并丢弃，造成"内存视图 ≠ 磁盘视图" | 当前行为（缺陷） |
| 8 | CAF 在 RDWR 关闭时**丢失整个 `chan` 块**；WAVEX 在 RDWR 关闭时会把 `mask==0` 覆盖成默认掩码 | 当前行为（缺陷） |

---

## 2. 统一读写接口与内部表示

公共命令定义在 `include/sndfile.h:206-207`：

```c
SFC_GET_CHANNEL_MAP_INFO = 0x1100,
SFC_SET_CHANNEL_MAP_INFO = 0x1101,
```

枚举 `include/sndfile.h:316-347`，其中 `SF_CHANNEL_MAP_INVALID = 0`、`SF_CHANNEL_MAP_MONO = 1`、`SF_CHANNEL_MAP_LEFT = 2` … `SF_CHANNEL_MAP_AMBISONIC_B_Z = 26`、`SF_CHANNEL_MAP_MAX = 27`。

存储位置是每个 `SF_PRIVATE` 上的一维数组 `int *channel_map`（`src/common.h:414`），语义为**"第 i 个交织声道 = 哪个扬声器位置"**，与 `psf->sf.channels` 同长。

公共分发在 `src/sndfile.c:1428-1474`：

- **GET**（`1428-1438`）：`channel_map == NULL` → 直接 `return SF_FALSE`（不设 `psf->error`）；否则要求 `datasize == sizeof(int) * channels`，然后 `memcpy`。
- **SET**（`1440-1474`）：`have_written` → `SFE_CMD_HAS_DATA`；长度不符或 `NULL` → `SFE_BAD_COMMAND_PARAM`；逐元素校验 `> SF_CHANNEL_MAP_INVALID && < SF_CHANNEL_MAP_MAX`（`1452-1457`）；**先** `free` 旧数组并 `memcpy` 新数组（`1460-1466`），**再**调用容器的 `psf->command`（`1472-1473`），返回值就是容器的返回值。

容器命令注册：`src/wav.c:237`（WAV/WAVEX 共用 `wav_command`）、`src/rf64.c:845`、`src/caf.c:170`、`src/aiff.c:288`；W64 没有注册 `command`。

因此"统一接口"实际是**三层**：

```
应用  ── SFC_SET/GET_CHANNEL_MAP_INFO ──▶  sndfile.c（长度/取值范围校验 + 数组缓存）
                                              │
                                              ├─ wav_command  → wavlike_gen_channel_mask → dwChannelMask
                                              ├─ caf_command  → aiff_caf_find_channel_layout_tag → layoutTag
                                              └─ aiff_command → 同一张 AIFF/CAF 表
```

关键点：**公共层只做"每个元素是否在枚举范围内"的校验，不做"这组元素能否被该容器表达"的判断**；后者完全由容器决定，而公共层在调用容器前就提交了内存状态（见 §7.3）。

---

## 3. WAVEX：位掩码 → 统一数组

### 3.1 读（`src/wavlike.c:328-383`）

`WAVE_FORMAT_EXTENSIBLE` 的 `fmt` == 40 字节，`dwChannelMask` 在偏移 +20（`wavlike.c:334-336`）。

- `mask == 0` → 只打印 `Channel Mask : 0x0 (should not be zero)`，**不分配** `channel_map`（`wavlike.c:340-341`），因此 `GET == SF_FALSE`。
- 否则按 `channel_mask_bits[]`（`wavlike.c:87-110`）从**最低位到最高位**扫描，第 k 个置位的位对应数组中第 k 项：

  | bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 |
  |:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|
  | id | LEFT | RIGHT | CENTER | LFE | REAR_LEFT | REAR_RIGHT | FRONT_LEFT_OF_CENTER | FRONT_RIGHT_OF_CENTER | REAR_CENTER | SIDE_LEFT | SIDE_RIGHT | TOP_CENTER | TOP_FRONT_LEFT | TOP_FRONT_CENTER | TOP_FRONT_RIGHT | TOP_REAR_LEFT | TOP_REAR_CENTER | TOP_REAR_RIGHT |

  注释明确写着 `WAVEFORMATEXTENSIBLE doesn't distinguish FRONT_LEFT from LEFT`（`wavlike.c:91`），所以读回来永远是 `LEFT/RIGHT`，不会是 `FRONT_LEFT/FRONT_RIGHT`。
- 置位数 < 声道数：数组按声道数分配（`calloc`，`wavlike.c:351`），尾部保持 0（即 `SF_CHANNEL_MAP_INVALID`），日志 `*** Less channel map bits than there are channels.`（`wavlike.c:377-380`）。
- 置位数 > 声道数：循环条件 `k < psf->sf.channels`（`wavlike.c:357`）会**静默丢弃**多余位；因此 `360-363` 的 `*** More channel map bits than there are channels.` 分支**不可达**（`k` 永不超过 `channels`）。
- 表中不存在的高位（如 bit 18 以上）不产生任何数组项。

### 3.2 写（`src/wav.c:1020-1140`，掩码生成 `src/wavlike.c:495-520`）

`wav_command` 的 SET 分支（`wav.c:1368-1370`）调用：

```c
wpriv->wavex_channelmask = wavlike_gen_channel_mask (psf->channel_map, psf->sf.channels) ;
return (wpriv->wavex_channelmask != 0) ;
```

`wavlike_gen_channel_mask` 的规则是：**声道必须严格按 `channel_mask_bits[]` 的下标升序出现**，否则返回 0：

- 每个 id 从上一个命中位 +1 开始查找（`wavlike.c:505`）；
- 找不到（id 不在表内，例如 `SF_CHANNEL_MAP_MONO`、`SF_CHANNEL_MAP_AMBISONIC_B_*`、重复 id）时 `bit` 保持旧值；
- `if (bit <= last_bit) return 0 ;`（`wavlike.c:512`）——把"未命中"和"逆序/重复"统一判成失败。

写 `fmt` 时（`wav.c:1055-1093`）：Ambisonic → 写 0；SET 成功过 → 写缓存掩码；否则按声道数套默认：

| 声道数 | 默认 `dwChannelMask` | 含义 |
|:--|:--|:--|
| 1 | `0x4` | **Center**（不是 L、也不是 MONO） |
| 2 | `0x3` | L, R |
| 4 | `0x33` | L, R, Ls, Rs（quad） |
| 6 | `0x3F` | L, R, C, LFE, Ls, Rs（5.1） |
| 8 | `0xFF` | 7.1 |
| 3,5,7,≥9 | `0x0` | 无映射 |

实测（不调用 `SFC_SET_CHANNEL_MAP_INFO`，直接写 1..9 声道 WAVEX 再读回）：

```
default  ch=1 GET=1 map=[4 ...]            <- CENTER
default  ch=2 GET=1 map=[2 3 ...]
default  ch=3 GET=0 map=[-1 ...]
default  ch=4 GET=1 map=[2 3 9 10 ...]
default  ch=5 GET=0 map=[-1 ...]
default  ch=6 GET=1 map=[2 3 4 11 9 10 ...]
default  ch=7 GET=0 map=[-1 ...]
default  ch=8 GET=1 map=[2 3 4 11 9 10 12 13 ...]
default  ch=9 GET=0 map=[-1 ...]
```

即"未设置"并不等于"无映射"：4/6/8 声道会被**主动伪造一个常见布局**（代码注释自认 *"some liberty is taken here"*，`wav.c:1064-1066`）。

### 3.3 可表达集合（实测）

| 请求 | WAVEX SET | 落盘后 GET |
|:--|:--|:--|
| `{LEFT, RIGHT, CENTER, REAR_RIGHT}` | **1** | `LEFT RIGHT CENTER REAR_RIGHT` |
| `{LEFT, RIGHT, REAR_LEFT, SIDE_RIGHT}` | **1** | `LEFT RIGHT REAR_LEFT SIDE_RIGHT` |
| `{SIDE_LEFT, LEFT, RIGHT, SIDE_RIGHT}` | **0** | 默认 quad `LEFT RIGHT REAR_LEFT REAR_RIGHT` |
| `{MONO}`（1 声道） | **0** | 默认 `CENTER` |
| `{LEFT}`（1 声道） | **1** | `LEFT` |
| 重复 id `{L,L,R,R}` | **0** | 默认 quad |

结论：WAVEX 能表达的集合 = "任意升序子集"，包括 WAVE 标准里没有对应命名布局的组合（如 L,R,C,Rs），只要 id 在表里且升序。

---

## 4. CAF（与 AIFF 共用）：布局 tag → 统一数组

### 4.1 tag 的位布局

布局常量集中在 `src/chanmap.c`，约定为 `(layoutTag << 16) | nChannels`，例如：

```c
{ (101 << 16) | 2, two_channel_stereo,  "stereo (L, R)" }
{ (121 << 16) | 6, six_channel_mpeg_51_a, "MPEG 5.1 A (L, R, C, LFE, Ls, Rs)" }
{ (122 << 16) | 6, six_channel_mpeg_51_b, "MPEG 5.1 B (L, R, Ls, Rs, C, LFE)" }
```

只有 0..8 声道进入查找表（`chanmap.c:213-223`，`map[]` 的下标就是声道数）。`zero_chan`（`chanmap.c:36-39`）刻意放了两项 `(0<<16)|0` 和 `(1<<16)|0`，名字分别是 *"Use channel descriptions."* 与 *"Use channel bitmap."*，`channel_map` 为 `NULL`。

`aiff_caf_of_channel_layout_tag`（`chanmap.c:245-262`）以 `tag & 0xffff` 作表下标，再精确比较整个 tag；找不到返回 `NULL`。

### 4.2 读（`src/caf.c:796-831`）

`chan` 块载荷是三个大端 `u32`：`layoutTag, channelBitmap, numberChannelDescriptions`（`caf.c:802`）。当前实现：

- **只使用 `layoutTag`**；`channelBitmap` 与 `channelDescriptions` 读取后丢弃（`802` 之后完全未引用），也不写日志。
- `map_info == NULL`（未知 tag，或 tag 0 的两种"请用 bitmap/描述"）→ **不分配** `channel_map`，`GET == SF_FALSE`；然后按 `chunk_size` 跳过剩余载荷（`caf.c:810-811`），解析器保持同步。
- `map_info->channel_map == NULL`（表里存在但无规范映射的已知 tag：0/1 号、0x6E 六声道 hexagonal、0x8C 七声道 AudioUnit 7.0、0x8F AAC 7.0、全部 8 声道项）→ 同样不分配，`GET == SF_FALSE`，但日志会打印 `Layout : ...`（`caf.c:807-808`）。
- 有映射时，分配 `psf->sf.channels` 项，只拷贝 `SF_MIN(psf->sf.channels, layout_tag & 0xff)` 项（`caf.c:819-827`）：
  - tag 声道数 **少于** 文件声道数 → 尾部填 0（`SF_CHANNEL_MAP_INVALID`），`GET == SF_TRUE`；
  - tag 声道数 **多于** 文件声道数 → **静默截断**，`GET == SF_TRUE`。
  - `& 0xff` 与 `aiff_caf_of_channel_layout_tag` 里的 `& 0xffff` 不一致；现有表项最大 8 声道，二者恰好等价，属于**潜伏不一致**（AIFF 同名代码用的是 `& 0xffff`，`aiff.c:1803`）。
- 多个 `chan` 块：每个都解析，**后一个合法映射覆盖前一个**；后一个不合法（未知 tag / 无映射）不会清掉前一个已建立的映射（实测：先 5.1 A 后未知 tag 的文件仍返回 5.1 A 的 map）。
- **没有**把 tag 声道数与 `desc.channels_per_frame` 做一致性校验。

### 4.3 写（`src/caf.c:236-238`、`736-737`）

```c
case SFC_SET_CHANNEL_MAP_INFO :
    pcaf->chanmap_tag = aiff_caf_find_channel_layout_tag (psf->channel_map, psf->sf.channels) ;
    return (pcaf->chanmap_tag != 0) ;
```

`aiff_caf_find_channel_layout_tag`（`chanmap.c:226-243`）要求对表中某项做**整数组 `memcmp` 完全相等**。因此 CAF 的可写集合 = "表里列出的、带 `channel_map` 的布局"：

| 声道数 | 可写布局（部分） |
|:--|:--|
| 1 | mono `0x640001` |
| 2 | stereo `0x650002`、headphones `0x660002` |
| 3 | MPEG 3/0 A `0x710003`、3/0 B、ITU 2.1、DVD 4 |
| 4 | ambisonic B `0x6B0004`、quad `0x6C0004`、MPEG 4.0 A/B、ITU 2.3、DVD 5、DVD 10 |
| 5 | pentagonal、MPEG 5.0 A–D、DVD 6/11/18 |
| 6 | MPEG 5.1 A–D、AudioUnit 6.0、AAC 6.0（**hexagonal 不可写**，`channel_map == NULL`） |
| 7 | MPEG 6.1 A、AAC 6.1（AudioUnit 7.0 / AAC 7.0 不可写） |
| 8 | **全部不可写**（表中 8 项 `channel_map` 均为 `NULL`） |

实测：

```
CAF 1 mono            SET=1 reGET=1: MONO
CAF 4 quad            SET=1 reGET=1: LEFT RIGHT REAR_LEFT REAR_RIGHT
CAF 6 5.1A            SET=1 reGET=1: LEFT RIGHT CENTER LFE REAR_LEFT REAR_RIGHT
CAF 8 MPEG71A         SET=0 reGET=0:
CAF {LEFT,RIGHT,CENTER,REAR_RIGHT}   SET=0 reGET=0:
```

即：**WAVEX 能写 `{L,R,C,Rs}`，CAF 不能；CAF 能写 5.1 B 的 `{L,R,Ls,Rs,C,LFE}`，WAVEX 不能**（因为 WAVEX 要求 bit 升序，C 的 bit 2 在 Ls 的 bit 4 之前）。

落盘时只有 `psf->channel_map && pcaf->chanmap_tag` 才写 `chan` 块（`caf.c:736-737`），载荷固定 12 字节、bitmap 与 descriptions 都写 0。

---

## 5. 未知 / 矛盾布局的行为矩阵（当前行为）

| 情形 | WAVEX | CAF |
|:--|:--|:--|
| 无布局字段（无 `fmt` 扩展 / 无 `chan`） | 无掩码字段、无映射；`GET=0`（plain WAV 编码路径） | `GET=0` |
| 掩码/tag 为 0 | 日志 `should not be zero`，`GET=0` | tag 0 → 日志 `Use channel descriptions.`，`GET=0`（tag `0x10000` → `Use channel bitmap.`） |
| 完全未知的 tag | 不适用（无 tag 概念） | 按 `chunk_size` 跳过，`GET=0`，日志只有 `Tag : DEAD0002`，无告警 |
| **已知但无规范映射**（0x6E、0x8C、0x8F、全部 8 声道） | 不适用 | 日志打印 `Layout : hexagonal (...)` 等，`GET=0` |
| 置位/条目数 **少于** 声道数 | `GET=1`，缺位为 `SF_CHANNEL_MAP_INVALID(0)`，日志 `*** Less channel map bits ...` | `GET=1`，缺位为 0，**无告警** |
| 置位/条目数 **多于** 声道数 | 静默丢弃多余位，`GET=1`，**无告警**（"More" 分支不可达） | 静默截断，`GET=1`，**无告警** |
| 位在表外（bit ≥ 18） | 该位不产生条目，`GET=1`，可全为 0 | 不适用 |
| 重复 `chan` 块 | 不适用 | 最后一个**合法**映射生效；后续非法块不清除先前结果 |
| 重复 `fmt` 块 | WAV/WAVEX 在 `wav.c:388-390` 直接 `break`，**未跳过第二个块的载荷** → 解析错位、打开失败（实测 `Unknown chunk marker (4FFFE) at position 68`）。RF64/W64 会正确跳过并忽略（`rf64.c:261-265`、`w64.c:280-283` 配合循环尾部跳过） | 不适用 |
| tag 声道数与文件声道数矛盾 | 掩码本无声道数，靠数组长度对齐 | 不做校验（见下） |
| 顺序不可表达（WAVEX 逆序 / CAF 非表内布局） | `SET=0`，落盘写该声道数的**默认**掩码（1/2/4/6/8 有默认值，3/5/7/≥9 写 `0x0`） | `SET=0`，且 `chanmap_tag` 被置 0，不写 `chan` 块 |

矛盾布局的两种"软失败"细节：

1. **WAVEX** 允许"位数多于声道数"是因为解析循环被 `k < channels` 截断，所以 `0x3F`（6 位）配 4 声道得到 `L,R,C,LFE`，多余的 `Ls,Rs` 消失；日志只打印 `Channel Mask : 0x3F (L, R, C, LFE)`，看起来完全正常。
2. **CAF** 的 tag 自带声道数，但查找只看 tag，不与 `desc.channels_per_frame` 比较，所以 `0x7A0006`（6 声道 MPEG 5.1 B）配 4 声道的 `desc` 会得到 `L,R,Ls,Rs` 四项，日志同时打印 `Channels / frame : 4` 与 `Layout : MPEG 5.1 B (...6 个位置...)`，二者矛盾却无告警。

**AIFF 的对照**（同为 `chan` 块）：`aiff_read_chanmap` 在未知 tag 时于 `aiff.c:1786-1787` 直接 `return 0`，**没有跳过剩余字节**。12 字节载荷时无害；若 `CHAN` 块大于 12 字节且 tag 未知，解析器会错位。实测（16 字节 `CHAN`、未知 tag `0xDEAD0004`）：

```
open failed: Unspecified internal error.
 CHAN : 16
Have 0 marker at position 66 (0x42).
Invalid SF_PRIVATE field : dataoffset == -1.
```

同一个文件把 tag 换成合法的 `0x860004` 则正常打开，并正确跳过多余 4 字节。CAF 侧没有该问题，因为它在查找前就按 `chunk_size` 跳过了（`caf.c:810-811`）。

---

## 6. 布局选择如何影响声道顺序与帧数

### 6.1 帧数：只由声道数与数据长度决定，与布局无关

WAVEX 读路径（`src/wavlike.c:481`、`src/wav.c:697-702`）：

```c
psf->blockwidth = wav_fmt->min.channels * psf->bytewidth ;      /* wavlike.c:481 */
if (psf->blockwidth)
    psf->sf.frames = psf->datalength / psf->blockwidth ;        /* wav.c:701 */
```

`dwChannelMask` 完全不参与。若 `dwChannels` 与 `blockalign` 矛盾，对 PCM/float 类编码**以 `dwChannels` 为准**（`psf->blockwidth` 由 `channels * bytewidth` 重算，`blockalign` 只用于字节率核对；ADPCM 类例外，它们直接用 `blockalign`/`samplesperblock`）。实测把 4 声道文件的 `blockalign` 从 8 改成 6，帧数仍是 100。若改的是 `dwChannels`（4→5，`blockalign` 同步 10），`data` 仍是 800 字节 → **帧数 100 → 80**。

CAF 读路径：

- `psf->sf.channels = desc.channels_per_frame ;`（`caf.c:385`）
- `psf->sf.frames = psf->datalength / psf->bytewidth ;`（`caf.c:570`，**少除了 channels**）
- 随后 PCM/float/µ-law/A-law 的 `*_init` 会用 `blockwidth = bytewidth * channels` 重算并覆盖（`pcm.c:138`、`pcm.c:285`；`float32.c:249`、`double64.c:252`、`alaw.c:73`、`ulaw.c:63`）。ALAC 走自己的 `pakt` 计算。

所以对受支持编码，`caf.c:570` 那行是**暂时错、最终被覆盖**的，但它是唯一一个"不除声道数"的容器，属于潜伏不一致。实测把 `channels_per_frame` 4→5（`pkt_bytes` 同步 10）而数据仍 800 字节 → **帧数 100 → 80**，与 WAVEX 对称。

CAF 的 `pkt_bytes` / `frames_per_packet` 与布局无关，但与**编码判定**强相关（`caf.c:289-318`）：`pkt_bytes` 必须等于 `bytewidth * channels`，否则 `decode_desc_chunk` 返回 0，打开失败并打印 `**** Unknown format identifier.`，对外是 `SF_ERR_UNSUPPORTED_ENCODING`（"Supported file format but unsupported encoding."）。实测：

| 改动 | 结果 |
|:--|:--|
| `pkt_bytes=8`（应为 8）| 正常，frames=100 |
| `pkt_bytes=16`、bits=16、4 声道 | 打开失败 `Unknown format identifier` |
| `bits_per_chan=24`、`pkt_bytes=8` | 打开失败 `Unknown format identifier` |
| `frames_per_packet=2` | **被忽略**，frames 仍 100（lpcm 不使用该字段） |
| `channels_per_frame=0` | 打开失败（`pcm_init` 内部错误 / `SFE_INTERNAL`） |
| `channels_per_frame=1025` | 打开失败，`**** Bad channels per frame value 1025.` |

WAVEX 侧对应的是 `SF_MAX_CHANNELS=1024` 上限（`wav.c:670-674`：`0` → `SFE_CHANNEL_COUNT_ZERO`，`>1024` → `SFE_CHANNEL_COUNT`）。

### 6.2 声道顺序：两种格式的语义不同，转换时不重排样本

- **CAF/AIFF**：tag 定义的是"第 i 个样本属于哪个扬声器"。MPEG 5.1 A 与 5.1 B 是**同一组** `{L,R,C,LFE,Ls,Rs}` 的**两种排列**，映射到两组不同的 `SF_CHANNEL_MAP_*` 数组（`chanmap.c:136-143`）。写 CAF 时会精确匹配到对应的 tag：实测 6 声道 `{L,R,Ls,Rs,C,LFE}` → tag `0x7A0006`，读回数组原样不变。
- **WAVEX**：掩码只表达"集合"，规范化的顺序是**位升序**（读路径 `wavlike.c:357-368` 固定如此）。请求逆序（如 `{Sl,L,R,Sr}`）直接 `SET=0`。因此 WAVEX 的隐含顺序是 MS 的 bit 序，不能表达 5.1 B 那种"Ls/Rs 先于 C"的排列。

两者通过 `channel_map` 数组对接，**样本流不做重排**。这带来一个可观察的静默错误：CAF 5.1 B → WAVEX 转换。`sndfile-convert` 只是 `GET` 后 `SET`（`programs/sndfile-convert.c:397-402`）：

```
源 CAF（tag 5.1B）  map = L, R, REAR_LEFT, REAR_RIGHT, CENTER, LFE
转换后 WAVEX       map = L, R, CENTER, LFE, REAR_LEFT, REAR_RIGHT
```

而 PCM 载荷逐样本相同 —— 实测首帧两边都是 `(1000, 2000, 3000, 4000, 5000, 6000)`，但索引 2 的样本从"左后环绕"被重新标注成"中置"。也就是说：**转换成功、没有告警、元数据却是错的**。反向（WAVEX → CAF）如果数组恰好等于表里某项（例如 5.1 A 序），会得到正确 tag；否则 `SET` 返回 0 且不写 `chan` 块，布局信息被静默丢弃。

---

## 7. 两个异常输入的实测结果

两个样本都由 libsndfile 自己写出的合法文件**逐字节改写**得到（不涉及任何外部编解码器），因此差异只来自布局字段。

### 7.1 样本 A：WAVEX，4 声道，`dwChannelMask = 0x3`

构造：以 4 声道 PCM16 WAVEX（`dwChannelMask=0x33`，`data=800` 字节，`frames=100`）为底，仅把 `fmt` 偏移 +20 的掩码改成 `0x3`：

```python
d = bytearray(open('base.wavex','rb').read())
i = d.find(b'fmt ') + 8
struct.pack_into('<I', d, i+20, 0x3)
```

可观察结果（先只读，再对副本 RDWR 打开并关闭后复读）：

```
Anomaly 1: WAVEX, dwChannelMask=0x3 on a 4-channel file
  open: ok  (channels=4, frames=100)
  GET_CHANNEL_MAP_INFO = 1, channels = LEFT RIGHT INVALID INVALID
  log: Channel Mask  : 0x3
*** Less channel map bits than there are channels.
  replay GET->SET on fresh WAVEX: 0, sf_error=30 (Bad parameter passed to function sf_command.)
  after RDWR close:
  open: ok  (channels=4, frames=100)
  GET_CHANNEL_MAP_INFO = 1, channels = LEFT RIGHT INVALID INVALID
  log: Channel Mask  : 0x3
*** Less channel map bits than there are channels.
```

对应的源码路径：`wavlike.c:340`（非 0 才分配）、`wavlike.c:351`（`calloc` 置 0）、`wavlike.c:377-380`（只告警）。

要点：

1. 打开**成功**，`SF_INFO` 正常（4 声道 / 100 帧），错误码为 `No Error`（`sf_error=0`）。
2. `GET` **返回真**，但第 3、4 项是 `SF_CHANNEL_MAP_INVALID(0)` —— 接口把"未知"编码进了合法返回值里，调用方若不逐项检查无法区分。
3. 这个 `GET` 结果**不能原样 `SET` 回去**：公共层拒绝 0 值（`sndfile.c:1453`），返回 `SF_FALSE` 且 `SFE_BAD_COMMAND_PARAM`。即 `GET` 能产出 `SET` 不接受的数据。
4. RDWR 打开→关闭后文件**逐字节不变**（`cmp` 0 处差异，880 → 880 字节），因为 `wavex_channelmask` 被原样保留并在写头时写回（`wavlike.c:346` → `wav.c:1060-1061`）。

### 7.2 样本 B：CAF，4 声道，`chan` tag = `0x790006`（MPEG 5.1 A，六声道）

构造：以 4 声道 PCM16 CAF（`chan` tag `0x6C0004` quad，`desc.channels_per_frame=4`，`data=804`）为底，只改 `chan` 载荷的 `layoutTag`：

```python
j = d.find(b'chan')          # payload 在块头 +12
struct.pack_into('>I', d, j+12, (121 << 16) | 6)
```

可观察结果：

```
Anomaly 2: CAF, 'chan' tag 0x790006 (MPEG 5.1 A, 6 channels) on a 4-channel file
  open: ok  (channels=4, frames=100)
  GET_CHANNEL_MAP_INFO = 1, channels = LEFT RIGHT CENTER LFE
  log: Layout : MPEG 5.1 A (L, R, C, LFE, Ls, Rs)
  RDWR in-memory GET=1
  after RDWR close:
  open: ok  (channels=4, frames=100)
  GET_CHANNEL_MAP_INFO = 0, channels =
```

对应的源码路径：`caf.c:802`（只读 tag）、`caf.c:804`（按 tag 查表）、`caf.c:819-827`（`SF_MIN(channels, tag & 0xff)` 截断拷贝）；RDWR 丢块来自 `caf_close → caf_write_header`（`caf.c:217-226`）在写 `chan` 块时要求 `pcaf->chanmap_tag != 0`（`caf.c:736`），而 `caf_read_chanmap` **从不**给 `pcaf->chanmap_tag` 赋值（该字段只在 `caf_command` 的 SET 分支里出现，`caf.c:237`）。

要点：

1. 打开**成功**，`SF_INFO` 正常；tag 与 `desc` 的 4 声道矛盾**没有任何告警或错误**，日志里两行直接矛盾。
2. `GET` **返回真**，`Ls/Rs` 被静默丢弃，结果看起来像一个合法的 4 声道布局 `L,R,C,LFE`——语义上等于把 5.1A 的前 4 个位置当成了完整布局。
3. **RDWR 打开→关闭后 `chan` 块完全消失**，`GET` 变为 `SF_FALSE`。音频载荷逐字节不变（`data` 段 SHA-1 前后一致，800 字节），文件长度不变（4896 → 4896，差值被 `free` 块吸收，`free` 从 3992 变 4016）。这会让"只读改写峰值/字符串"的 RDWR 操作静默摧毁声道信息。AIFF 没有该问题：RDWR 关闭走 `aiff_rewrite_header`（`aiff.c:1255-1261`），只修补长度字段，其余头字节原样保留；实测同样操作后 `CHAN` 块仍在、`GET=1`。

### 7.3 附带发现：失败的 SET 不回滚内存状态，并会丢弃先前成功的布局

公共层在调用容器前就提交了 `channel_map`（`sndfile.c:1460-1466`），容器的失败返回值不撤销这次提交；同时 `wav_command` 会**无条件覆盖** `wpriv->wavex_channelmask`（`wav.c:1369`），失败时它被写成 0。实测（先 SET 一个可表达的 `{L,R,LFE,Cs}`，再 SET 一个不可表达的 `{L,Rs,C,LFE}`，然后写入音频并关闭）：

```
good+bad.wavex
  good SET=1
  in-mem good      GET=1 [2 3 11 8]        # L,R,LFE,RearCenter，正确
  bad  SET=0  sf_error=0                   # 失败不设错误码
  in-mem bad       GET=1 [2 10 4 11]       # 内存被改成失败的请求值
  on disk          GET=1 [2 3 9 10]        # 磁盘上是默认 quad，先前成功的布局也被丢弃

good+bad.caf
  good SET=1
  in-mem good      GET=1 [2 3 11 8]
  bad  SET=0  sf_error=0
  in-mem bad       GET=1 [2 10 4 11]
  on disk          GET=0 [-1 -1 -1 -1]     # 连 chan 块都不写
```

即一次失败的 `SET` 同时造成三个后果：返回值是假但错误码为 `No Error`；`GET` 反映的是被拒绝的请求；先前**已成功**的布局也被容器侧丢弃（WAVEX 回落到默认掩码，CAF 不写 `chan` 块）。

同类问题还包括：普通 WAV（`SF_FORMAT_WAV`）也能 `SET` 成功（`wav_command` 对两种容器共用，`wav.c:237`），但 writer 根本不写掩码，实测 `SET=1` / 内存 `GET=1` / 磁盘 `GET=0`；W64 干脆没有注册 `command`，`SET=0`；而 1 声道 `{MONO}` 请求在 CAF/AIFF 上成功、在 WAVEX 上失败（MONO 不在 `channel_mask_bits` 里）。

---

## 8. 测试证据

### 8.1 现有测试覆盖了什么

`tests/command_test.c:1809-1873` 的 `channel_map_test` 是本仓库唯一直接覆盖该接口的用例，由 `:240-247` 对四种容器调用：

```
channel_map_test ("chanmap.wavex", SF_FORMAT_WAVEX | SF_FORMAT_PCM_16) ;
channel_map_test ("chanmap.rf64" , SF_FORMAT_RF64  | SF_FORMAT_PCM_16) ;
channel_map_test ("chanmap.aifc" , SF_FORMAT_AIFF  | SF_FORMAT_PCM_16) ;
channel_map_test ("chanmap.caf"  , SF_FORMAT_CAF   | SF_FORMAT_PCM_16) ;
```

用例内容是**单一规范布局** `{LEFT, RIGHT, LFE, REAR_CENTER}`（`:1813-1816`）的：
写入 → 读回 → `GET` → `memcmp` 相等（`:1848-1869`），以及对 WAVEX/RF64 额外断言"未 SET 时也存在默认映射"（`:1825-1842`）。

`tests/channel_test.c:57-122` 只验证 1..8 声道 WAV 的交织读写与帧数一致性，完全不涉及布局。

`command_test` 的 `second_fmt_chunk_test`（`:1875-1898`）只断言"第二个 `fmt` 块不改声道数"，不检查掩码。

### 8.2 现有测试没有覆盖什么（本报告涉及的路径均为空白）

- 置位/条目数 ≠ 声道数（样本 A、B 所在的路径）
- 未知 tag、已知但无映射的 tag（0x6E / 0x8C / 0x8F / 全部 8 声道）
- 掩码含表外高位、逆序、重复 id
- `SET` 失败后的内存/磁盘一致性
- CAF/AIFF 的 RDWR 往返是否保留 `chan` 块
- 跨格式转换后布局是否等价（顺序语义差异）

### 8.3 基线运行结果

```
$ ctest -j8
100% tests passed, 0 tests failed out of 144
$ ./command_test chanmap
    channel_map_test : chanmap.wavex ... ok
    channel_map_test : chanmap.rf64  ... ok
    channel_map_test : chanmap.aifc  ... ok
    channel_map_test : chanmap.caf   ... ok
```

说明上述差异都是**未被回归测试守住的既有行为**，而不是本分支引入的回归。

---

## 9. 当前行为 vs 建议

以下逐条对照，左列为本分支实测行为，右列为建议（未实现）。

| 主题 | 当前行为（实测） | 建议 |
|:--|:--|:--|
| 缺位填充 | WAVEX/CAF 都返回 `GET=TRUE`，缺位填 `SF_CHANNEL_MAP_INVALID(0)` | 要么整数组 `GET=FALSE`，要么在日志/文档中明确"0 = 未标注"；关键是让调用方能区分"完整布局"与"部分布局" |
| 多余位/条目 | WAVEX 静默丢弃（`More` 分支不可达），CAF 静默截断 | 与声道数不符时打印 `***` 级告警，并考虑直接 `GET=FALSE`；同时删除/修正 `wavlike.c:360-363` 的死分支 |
| tag 与 `desc` 声道数矛盾 | CAF 不做任何校验（样本 B） | `caf_read_chanmap` 应比较 `layout_tag & 0xffff` 与 `psf->sf.channels`，不一致时拒绝该布局（否则布局描述的是另一个声道数） |
| `& 0xff` vs `& 0xffff` | `caf.c:820` 用 `0xff`，`chanmap.c:249` 与 `aiff.c:1803` 用 `0xffff` | 统一为 `0xffff`，或直接由 `map_info` 携带条目数，去掉对 tag 编码的隐式依赖 |
| 失败 SET 的副作用 | 内存已提交、磁盘未提交，两者不一致；先前成功的布局也被丢弃，错误码仍是 `No Error`（§7.3） | 公共层先让容器确认可表达，再提交 `channel_map`；容器失败时回滚到旧数组，并设置明确的错误码 |
| 容器不支持时仍报成功 | 普通 WAV `SET` 返回真但磁盘无掩码；W64 无 handler 返回假 | 按容器能力返回一致结果（不支持即 `SF_FALSE` + `SFE_UNIMPLEMENTED` 之类的明确错误） |
| 1 声道语义 | WAVEX `{MONO}` 失败且落盘为 `CENTER`；CAF/AIFF `{MONO}` 成功 | 明确 `SF_CHANNEL_MAP_MONO` 仅 CAF/AIFF 可表达，或在 WAVEX 侧把 MONO 视作 CENTER 并返回真（行为需一致） |
| 默认掩码 | 4/6/8 声道未 SET 时**伪造**常见布局；3/5/7/≥9 写 0 | 保留现状可以，但应在文档中声明"未指定的 4/6/8 声道会被写成 quad/5.1/7.1"，因为这会改变第三方工具的解释 |
| RDWR 丢 `chan` 块 | CAF `caf_close` 重写头后 `chan` 消失（样本 B） | 读时把命中的 tag 存进 `pcaf->chanmap_tag`，或像 AIFF 一样用"原地补丁式"重写（参考 `aiff_rewrite_header`） |
| RDWR 改写 `mask==0` | 读时 `wavex_channelmask` 保持 0 → 写时落回声道数默认值，`0x0` 被改成 `0x33` | 增加"是否存在掩码字段"的独立布尔量，区分"无掩码"与"掩码为 0" |
| AIFF 未知 tag 的块长处理 | `aiff.c:1786-1787` 提前返回，`CHAN` 长于 12 字节时解析错位 | 先按 `dword` 跳过再决定是否建立映射（与 `caf.c:810-811` 对齐） |
| 跨格式转换 | `sndfile-convert` 只搬数组、不重排样本；5.1B→WAVEX 静默错标 | 在转换工具中对"目标容器无法表达该顺序"给出告警；文档明确"迁移布局 ≠ 重排样本" |
| 文档 | `docs/command.md:1427`/`:1483` 只说"AIFF or CAF Channel Layout chunk"，未提 WAVEX/RF64，也未说明失败/部分成功的返回值语义 | 补充三容器差异、可表达集合、`GET` 成功但含 0 值的情形、以及"必须在写入音频前 `SET`"（`have_written` 检查） |

---

## 10. 复现方式

```bash
# 构建（不启用任何外部编解码器）
cmake -S . -B /tmp/lsf-build \
  -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF \
  -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
  -DENABLE_CPACK=OFF -DENABLE_PACKAGE_CONFIG=OFF -DINSTALL_PKGCONFIG_MODULE=OFF
cmake --build /tmp/lsf-build -j8

# 1) 用 libsndfile 生成两个合法的 4 声道基文件
#    base.wavex : SF_FORMAT_WAVEX|PCM_16，SET quad  → dwChannelMask = 0x33
#    base.caf   : SF_FORMAT_CAF  |PCM_16，SET quad  → chan tag = 0x6C0004
#    两者各写 100 帧静音，data 段分别 800 / 804 字节（CAF 含 4 字节 edit 计数）

# 2) 只改布局字段，得到两个异常样本
#    A: base.wavex 的 'fmt ' 载荷 +20 写 0x3        → A_wavex_mask_0x3.wavex
#    B: base.caf   的 'chan' 载荷 +12 写 0x790006   → B_caf_tag_51_on_4ch.caf

# 3) SFM_READ 打开，读 SF_INFO 与 SFC_GET_CHANNEL_MAP_INFO；
#    再对副本用 SFM_RDWR 打开并立即关闭，重复第 3 步并比较文件字节
```

本次在干净目录 `/tmp/repro` 中完整复跑（`mkbase.c` → `patch.py` → `inspect.c` → `rdwr.c`）的结果：

```
WAVEX base mask = 0x33
CAF base tag    = 0x6c0004
A_wavex_mask_0x3.wavex
  channels=4 frames=100 GET=1 map= 2 3 0 0
  log: *** Less channel map bits than there are channels.
B_caf_tag_51_on_4ch.caf
  channels=4 frames=100 GET=1 map= 2 3 4 11
  log: Layout : MPEG 5.1 A (L, R, C, LFE, Ls, Rs)
--- RDWR 打开并关闭后 ---
A2.wavex: cmp 差异字节数 = 0
A2.wavex
  channels=4 frames=100 GET=1 map= 2 3 0 0
B2.caf
  channels=4 frames=100 GET=0 map= -1 -1 -1 -1
B2.caf: 4896 -> 4896 字节（chan 块被 free 块吸收）
```

> 注：本报告中的异常输入均为对 libsndfile 自身输出做字段级改写得到的合成样本，用于把"布局字段"从"编码/数据"中隔离出来；未使用任何外部编解码器或第三方文件。
