# 有损音频定位后的有效样本区间

本文分析 libsndfile 中 **有损/压缩音频在 `sf_seek()` 之后实际返回的样本区间**：请求的帧位置、压缩流边界（Ogg 页 / MPEG 帧 / FLAC 帧）与解码器延迟三者如何共同决定调用者真正拿到的样本，以及越界、短读时的结果契约。

分析对象为本仓库当前提交 `396a715`。涉及的有损路径为 Ogg/Vorbis (`src/ogg_vorbis.c`)、Ogg/Opus (`src/ogg_opus.c`) 与 MPEG 音频 (`src/mpeg_decode.c`)；FLAC 作为无损对照。所有数值均在本机以 Debug 构建实测（见第 9 节）。

---

## 1. 结论摘要

| 编解码 | 定位后返回的样本区间 | 与"同句柄顺序解码"的一致性 |
|---|---|---|
| Opus | `[t, t+n)`，`t` 即请求帧，`n = min(请求, frames−t)` | **逐样本相同**（本测试 lag=0，差值 0.0） |
| MP3 (mpg123) | `[t, t+n)` | **逐样本相同**（浮点舍入级，最大 2.2e-8） |
| Vorbis，页搜索落在**倒数第二页及更早** | `[t, t+n)` | **逐样本相同** |
| Vorbis，页搜索落在**最后一页** | 书签位置仍报 `t`，但内容为 `[t+δ, …)`；最后 `δ` 帧不存在，返回越界内容 | 相差固定偏移 `δ` 帧 |
| FLAC（无损对照） | `[t, t+n)` | 逐样本相同 |

`δ` 是文件最后一页"块实际解码跨度"超出"granule 声明跨度"的量，由末页精确截断引起。本测试中 `δ` = 125 帧（48 kHz，2.60 ms）、957 帧（19.94 ms）、48 帧（44.1 kHz，1.09 ms），见 §5.2。

注意触发条件是**页搜索是否落到末页**，不是"目标是否位于末页的输出区间"。判据是 `target_gp − 1024 ≥ 末页 granule 起点`（`1024 = blocksize1/2`，`src/ogg_vorbis.c:921-922`）。本测试文件中末页 granule 起点为 96960，因此 `t ≥ 97985` 才出现偏移；`t = 96960…97984` 仍走倒数第二页并顺序解码到目标，结果精确。

调用者可以依赖的硬契约（`src/sndfile.c`）：

- 成功时 `sf_seek()` 返回**从数据区首样本计的多声道帧号**，即随后第一次读到的样本位置（`src/sndfile.c:1593-1615`；文档 `docs/api.md:349-352`）。
- 读模式下 `t < 0` 或 `t > psf->sf.frames` 在分派给编解码器之前就被拒绝：返回 `-1`，不改变读位置（`src/sndfile.c:1588-1591`）。写/RDWR 模式只拒绝 `t < 0`（`src/sndfile.c:1582-1587`）。
- `t == psf->sf.frames` 合法：返回 `frames`，随后读取返回 0（`src/sndfile.c:2044-2047`）。
- 每次读取最多返回 `frames − 当前位置` 帧（`src/sndfile.c:2060-2067`）。

---

## 2. 三个坐标：请求帧、流边界、解码延迟

### 2.1 请求帧 → 容器内绝对位置

编解码器的 `seek` 入口只接收**绝对帧号**：`sf_seek()` 先把 `SEEK_CUR`/`SEEK_END` 统一折算成绝对位置（`src/sndfile.c:1519-1591`，`SEEK_END` 用 `psf->sf.frames + offset`），然后调用 `psf->seek`：

- Vorbis：`psf->seek = vorbis_seek`（`src/ogg_vorbis.c:501`）
- Opus：`psf->seek = ogg_opus_seek`（`src/ogg_opus.c:373`）
- MPEG：`psf->seek = mpeg_dec_seek`（`src/mpeg_decode.c:616`）
- FLAC：`psf->seek = flac_seek`（`src/flac.c:776`）

因此 `sf_seek(t, SEEK_SET)` 的语义基准是 **`SF_INFO.frames` 定义的样本坐标系**，而不是字节或页。所有有损实现随后把这个帧号换算成容器的 granule position：

```c
/* src/ogg_vorbis.c:955 */
target_gp = (uint64_t) offset + vdata->pcm_start ;
```
```c
/* src/ogg_opus.c:1624-1626 */
target_gp  = offset * oopus->sr_factor ;   /* sr_factor = 48000 / 实际采样率 */
target_gp += oopus->u.decode.gp_start ;    /* 文件级 granule 偏移 */
target_gp += oopus->header.preskip ;       /* 解码器延迟 */
```

MP3 则直接把帧号交给 mpg123：`mpg123_seek (pmp3d->pmh, count, SEEK_SET)`（`src/mpeg_decode.c:203`）。

### 2.2 压缩流边界：页 / 帧 / 块

**Ogg（Vorbis、Opus）**的定位不可能落到任意字节，只能落到"带 granule position 的页"：

- 打开时先定位末页并取出末页 granule position：`ogg_sync_last_page_before()`（`src/ogg.c:412-461`），从文件尾按 `OGG_CHUNK_SIZE = 65536`（`src/ogg.c:82`）向前翻块，块内用 `ogg_sync_next_page()` 前扫（`src/ogg.c:261-317`），页长上限 `OGG_PAGE_SIZE_MAX = 65307`（`src/ogg.c:81`）。
- 定位时做二分/预测搜索：`ogg_stream_seek_page_search()`（`src/ogg.c:464-717`），区间小于 `OGG_CHUNK_SIZE` 时退化为顺序扫描（`src/ogg.c:499-500`），最终停在"granule position ≤ 目标"的最后一页，必要时回退并重新读入含续包的页（`src/ogg.c:693-716`）。

**MPEG 音频**的最小单元是 MPEG 帧（48 kHz、Layer III 为 1152 样本/帧，24 ms）；**FLAC** 是编码帧（默认 4096 样本）。这两者由 libmpg123 / libFLAC 在内部按帧对齐并向前解码，libsndfile 只转发帧号。

### 2.3 解码延迟：为什么定位后必须先"多解一段"

三个有损编解码器都有需要丢弃或预滚的样本：

| 量 | 值 | 出处 |
|---|---|---|
| Opus preskip（编码器延迟） | 312 样本 @48 kHz（本测试文件；6.50 ms） | 头部字段 `src/ogg_opus.c:202`，解析 `:477`；回放时丢弃 `:1092-1095` |
| Opus 预滚 `OGG_OPUS_PREROLL` | `660*48 = 31680` 样本 = 660 ms | `src/ogg_opus.c:193` |
| Opus 帧/包 | 960 样本 @48 kHz（20 ms），每页 50 包 | `src/ogg_opus.c:185` 的 20 ms 约定；实测页结构 |
| Vorbis 大块 / 小块 | 2048 / 256 样本（本测试流） | `vorbis_info_blocksize(...,1)`，用于 `src/ogg_vorbis.c:921` |
| Vorbis 预取半块 | `blocksize1/2 = 1024` 样本（21.33 ms） | `src/ogg_vorbis.c:921-922` |
| Vorbis 免搜索阈值 | 2 秒 = 2×采样率 | `VORBIS_SEEK_THRESHOLD`，`src/ogg_vorbis.c:83`、`:905-907` |
| Vorbis 末页定位偏移 | `δ`（本文件 125 样本 = 2.60 ms；文件相关） | §5.2 |
| MPEG 帧 | 1152 样本（本项目 48 kHz） | 由 mpg123 内部处理；`MPG123_GAPLESS` 见 `src/mpeg_decode.c:555` |

**Opus** 的定位因此是"向目标前 660 ms 的那一页跳，然后解到目标"（`src/ogg_opus.c:1634-1639`，实测 `preroll_gp = target_gp − 31680`），这段预滚保证 MDCT 重叠区收敛；`ogg_opus_null_read()` 负责把预滚段的样本"读掉但不返回"，并在扣除 preskip 与文件 granule 偏移后返回位置（`src/ogg_opus.c:1503-1526`、`:1691-1692`）。源码注释解释了 660 ms 的来历：解码器在 80 ms 内即收敛到 -40 dB，但为"高概率 32-bit 精确"取保守值 660 ms（33 个 20 ms 包，`src/ogg_opus.c:187-193`）。这是一个**质量**选择而非正确性下界：实测 660 ms 预滚下的定位结果与顺序解码逐样本相同（§4.1）。

**Vorbis** 是重叠变换，目标样本由相邻两块各贡献一半，所以它搜索的是"目标前 `blocksize/2`"的页（`src/ogg_vorbis.c:909-925`），再逐包 `vorbis_synthesis_trackonly/blockin` 把解码器状态推进到目标块，最后用 `vorbis_read_sample(..., vorbis_rnull)` 精确丢掉到目标的样本（`src/ogg_vorbis.c:820-890`，关键判定 `:874-876`）。

### 2.4 `SF_INFO.frames` 也是"解码延迟"的函数

`sf_seek` 的合法上界与所有短读截断都用 `psf->sf.frames`，而 `frames` 本身已扣除延迟：

```c
/* src/ogg_opus.c:1481-1482 */
psf->sf.frames = (oopus->u.decode.gp_end - oopus->u.decode.gp_start
                  - oopus->header.preskip) / oopus->sr_factor ;
```
```c
/* src/ogg_vorbis.c:311 */
psf->sf.frames = (vdata->pcm_end != (uint64_t) -1) ? vdata->pcm_end - vdata->pcm_start : SF_COUNT_MAX ;
```

因此"请求帧位置 + 延迟"共同决定了 granule 目标；"流边界"决定可解码的最远点。三者一旦不一致（末页截断、文件被截短），就会出现第 5 节的偏差。

---

## 3. 起始定位（t = 0 与极小 t）

起始定位必须同时：

1. 丢弃编码器延迟（Opus preskip / MP3 encoder delay）；
2. 让重叠区"从零状态"收敛（Vorbis 无前一块时第一块输出被截断）；
3. 保证返回 0 时第一个样本就是文件第 0 个样本。

**Opus**：`seek(0)` 换算 `target_gp = 0 + gp_start + 312`，`ogg_opus_null_read()` 读到 granule 312 后返回 `0`（实测跟踪：`null_read -> 312, returns 0 ... loc=312`）。`pkt_pos < preskip` 时 `read_refill` 还会额外把包内前若干样本跳过（`src/ogg_opus.c:1092-1095`）。结果与顺序解码逐样本一致。

**Vorbis**：`seek(0)` 命中 2 秒免搜索短路（`src/ogg_vorbis.c:905-907`），`vorbis_skip(target_gp = pcm_start = 0)` 不消费任何样本即返回 0（`src/ogg_vorbis.c:820-890`：`pcmout` 为 0、`gp` 已达 target，直接跳过循环）。结果逐样本一致。

**MP3**：`mpg123_seek(..., 0, SEEK_SET)`；`MPG123_GAPLESS`（`src/mpeg_decode.c:555`）让 mpg123 按 LAME 标签丢弃首帧编码延迟。本测试流首帧 `Skip Samples = 1105`。

**FLAC**：`FLAC__stream_decoder_seek_absolute(0)`（`src/flac.c:1568`）。

实测（3.0000625 s、48 kHz 单声道；MP3 为 144047 帧）：

| 请求 t | Opus 返回帧数 | Vorbis 返回帧数 | MP3 返回帧数 |
|---|---|---|---|
| 0 | 4096 | 4096 | 4096 |
| 1 | 4096 | 4096 | 4096 |
| 1000 | 4096 | 4096 | 4096 |

`seek(0)` 与"顺序解码"的 4096 帧逐样本相同（差值 0.0），三者均可作为"起始定位不产生偏移"的证据。

---

## 4. 中间定位

### 4.1 Opus

两条路径，都精确：

- **前向且距离 ≤ 660 ms**：不重新跳页，直接在当前解码器状态上解码到目标（`src/ogg_opus.c:1629` 的条件不成立）。实测在前向 1、100、31679、31680、31681 帧各点均与顺序解码逐样本相同，说明该路径不引入偏差。
- **后向，或前向 > 660 ms**：`ogg_opus_page_seek_search()` 跳到预滚页（`src/ogg_opus.c:1535-1570`），把该页包入流、令 `pkt_pos = best_gp`（`:1562-1564`）、复位解码器（`:1567`），再按包逐个跳过预滚段中位于目标之前的包（`:1670-1684`），最后 null-read 到目标（`:1691`）。

实测 3 s Opus（`frames = 144003`，`gp_end = 144315`，`gp_start = 0`，`preskip = 312`）在 t = 1、1000、5000、20000、31679、60000、100000、120000、130000 上，与同句柄顺序解码的样本**完全相同**（lag = 0，RMS 差 0.0），与 ffmpeg 独立解码的参考也有约 9e-6 的 RMS 差（仅编解码差异）。跟踪示例：`seek(100000)` → `preroll_gp = 68632`、`null_read -> 100312, returns 100000`。

### 4.2 Vorbis

- 目标在当前位置之后 2 秒内：短路跳过，不跳页（`src/ogg_vorbis.c:905-907`）。
- 否则：搜索 `target_gp − 1024` 的页（`src/ogg_vorbis.c:921-925`），`ogg_stream_unpack_page` 后 `vorbis_synthesis_restart` 并用 `vorbis_calculate_granulepos()` 反推当前位置（`:929-934`）。
- 搜索失败（`ret < 0 || vdata->gp > target_gp`）时回退到流首顺序解码（`src/ogg_vorbis.c:959-968`）——最坏是线性扫描，位置语义不变。

目标落在**倒数第二页及更早**时，结果与顺序解码逐样本相同。实测 3 s Vorbis 在 t = 90000、95000、97000、97984 上 `exact@0 = 1`；20 s、44.1 kHz 立体声文件在 t = 1000…856768 全程 lag = 0。

### 4.3 MP3

`mpg123_seek` 自行完成帧对齐与帧内丢弃；实测 3 s MP3（`frames = 144047`）在 t = 0…143000 的全部采样点上与顺序解码一致（最大差 2.2e-8，仅是 float 往返误差）。观察到的解码长度为

```
127 个 MPEG 帧 × 1152 = 146304
146304 − 1105（首帧 Skip Samples）− 1152（末帧裁剪）= 144047
```

即 `frames` 已剔除大部分编码填充，但相对 144003 帧的源 PCM 仍多出 **44 帧**（约 0.92 ms）的编码器 padding。这 44 帧是"合法的可定位区"——`seek(144000)` 返回 47 帧，`seek(144002)` 返回 45 帧，全部可读出。

---

## 5. 接近结尾的定位

### 5.1 索引上界：`frames` 是硬墙，不是"最后能解码的位置"

- `t == frames`：合法，返回 `frames`，读取返回 0（`src/sndfile.c:1588`、`:2044-2047`）。
- `t > frames` 或 `t < 0`：在分派前拒绝，返回 `-1`，错误串 `"Internal psf_fseek() failed."`（`SFE_BAD_SEEK`，`src/sndfile.c:121`），读位置不变（实测 `SEEK_CUR(0)` 仍为旧值）。
- 对管道/不可 seek 输入：`SFE_NOT_SEEKABLE`（`src/sndfile.c:1514`，串见 `:122`），且 Opus 此时 `frames = SF_COUNT_MAX`、`seekable = 0`（`src/ogg_opus.c:1462-1463`）。

结尾的"短读"是正常返回值，不是错误：

| 请求 t | MP3（144047） | Opus（144003） | Vorbis（144003） |
|---|---|---|---|
| 143000 | 1047 | 1003 | 1003 |
| 143990 | 57 | 13 | 13 |
| 143999 | 48 | 4 | 4 |
| 144000 | 47 | 3 | 3 |
| 144002 | 45 | 1 | 1 |
| 144003 / 144047 | 0 | 0 | 0 |

### 5.2 Vorbis 末页：位置对、内容偏移 `δ`

Vorbis 的重叠结构使"页的 granule position"与"该页块能解码出的样本数"在**最后一页**上不相等（规范允许末页 gp 小于块可解码总量，用于精确截断，见 `src/ogg_vorbis.c:1007-1010` 的注释）。libsndfile 用

```c
/* src/ogg_vorbis.c:1018-1025 —— 用相邻块对累加页时长 */
duration += (lastblock + thisblock) >> 2 ;
/* src/ogg_vorbis.c:1064 —— 页起始 granule = 末包 gp − 块对和 */
*gp_out = last_gp - duration ;
```

反推页起始位置。末页的 `duration` 会把被 gp 截掉的尾部样本也算进去，于是**页起点被低估 `δ` 帧**（等价地说：该页冷启动解码出的样本跨度比 granule 声明的跨度长 `δ`）。

以 3 s Vorbis（48 kHz，`pcm_start = 0`，`pcm_end = 144003`）为例：

| 量 | 值 |
|---|---|
| 末页 granule `last_gp` | 144003 |
| 块对和 `duration` | 46144 |
| 公式页起点 `last_gp − duration` | **97859** |
| 用 libvorbis 直接冷启动解码该页，实测对齐点 | **97984** |
| 偏差 `δ` | **125 帧（2.60 ms）** |

同一实测在其它文件上：`δ = 957`（19.94 ms，正弦测试流）、`δ = 48`（1.09 ms，20 s/44.1 kHz 流）。`δ` 有一个可核算的恒等式：

```
δ = (冷启动对齐点 + 该页冷启动解码长度) − frames
```

四个文件的实测值（冷启动对齐点由 libvorbis 独立测得，恒为页顺序起点 + 1024）：

| 文件 | 页顺序起点 | 冷启动对齐点 | 冷启动解码长度 | 两者之和 | `frames` | `δ` |
|---|---|---|---|---|---|---|
| noise.ogg | 96960 | 97984 | 46144 | 144128 | 144003 | **125** |
| wn.ogg | 96960 | 97984 | 46144 | 144128 | 144003 | **125** |
| vorbis_q4.ogg | 96832 | 97856 | 47104 | 144960 | 144003 | **957** |
| vorbis20.ogg | 855744 | 856768 | 25280 | 882048 | 882000 | **48** |

即 `δ` 就是"该页块实际解码出的样本跨度"超出"granule 声明的跨度"的那部分。

**可观察的诊断信号**：只要定位路径落入末页并触发 `vorbis_calculate_granulepos()` 的歧义分支（`src/ogg_vorbis.c:1035-1056`），日志就会追加一行

```
Vorbis: Cannot calculate ambiguous last page duration. Sample count may be wrong.
```

实测该行**只在**定位进入末页时出现：`seek(97000)`、`seek(97984)` 时为 0 次；`seek(97985)`、`seek(100000)` 时为 1 次（阈值正是 §4.2 的 `target − 1024` 越过末页起点 96960 的位置）。调用者可用 `SFC_GET_LOG_INFO` 主动检测这一情形。

**后果**：当页搜索落到最后一页（判据 `target_gp − 1024 ≥ 末页 granule 起点`，即本文件 `t ≥ 97985`）时，`vorbis_seek()` 仍然返回请求的 `t`（`src/ogg_vorbis.c:972`），但解码器真实首样本在 `t + δ`，于是返回的第 `i` 个样本等于顺序解码的第 `t + δ + i` 个样本；当 `t + δ + i ≥ frames` 时该位置不存在，读到的是越界内容。

该模型被精确验证（64 帧窗口内的重叠长度与预测逐点吻合）：

| 请求 t | 返回帧数 | 匹配到的位移 | 可验证重叠帧数 | 预测重叠 = frames − (t+δ) |
|---|---|---|---|---|
| 143200 | 256 | +125 | 256 | 678（窗口内即 256） |
| 143700 | 256 | +125 | 178 | 144003−143825 = 178 |
| 143750 | 253 | +125 | 128 | 128 |
| 143800 | 203 | +125 | 78 | 78 |
| 143820 | 183 | +125 | 58 | 58 |
| 143900 | 103 | 无固定位移 | 0 | 0（t+125 > frames） |

即：**越接近文件末尾，可用的真实样本越少**；最后 `δ` 帧（本文件中 t ≥ 143878）返回的内容不再对应任何顺序解码结果。用逐点重叠校验可确认这一边界是精确的（预测重叠帧数 = `min(返回帧数, frames − (t+δ))`）：

| 请求 t | 返回帧数 | 预测重叠 | 重叠区内是否逐样本精确 | 重叠区 RMS 差 |
|---|---|---|---|---|
| 143800 | 64 | 64 | 是 | 0 |
| 143820 | 64 | 58 | 是 | 0 |
| 143840 | 64 | 38 | 是 | 0 |
| 143860 | 64 | 18 | 是 | 0 |
| 143880 | 64 | 0 | —（无有效样本） | — |

也就是说：`t + δ < frames` 的部分（`[0, frames−(t+δ))`）与顺序解码逐样本相同，其余为越界内容。本文件越界区失配 RMS 约为信号 RMS 的 47%–50%，属于解码器越界输出而非静音。20 s、44.1 kHz 文件的 `δ = 48`，其失配起点相应为 t ≥ 881955。

**最后 `δ` 帧的行为**：`sf_seek(t)` 对 `t ∈ [frames−δ, frames]` 仍返回 `t`，`sf_readf_*` 仍按 `frames − t` 给出帧数；但这些帧全部来自越界区，没有有效样本。这是本报告唯一"返回值与内容语义完全脱钩"的区间。

注意这**不是** `frames` 计算错误：`sf_seek` 的上界、返回帧数、短读截断都正确；出错的只是"末页内的样本起点"。对调用者的含义是：**页搜索落入 Vorbis 末页后，`t` 与样本内容的对应关系不再是恒等映射**。

### 5.3 Opus / MP3 的结尾

两者没有上述偏差：Opus 的 granule 单位是 48 kHz 样本数，末页的尾部裁剪由 `ogg_opus_read_refill()` 显式处理（`src/ogg_opus.c:1044-1077`：当包内样本使 `pkt_pos` 超过页 gp 且页面带 EOS 时，`len` 被截到 `pg_pos − pkt_pos`）；MP3 的尾部裁剪由 mpg123 的 gapless 逻辑完成。实测 Opus 在 t = 143990/144000/144002 返回 13/3/1 帧且与顺序解码逐样本一致；MP3 同理（含多出的 44 帧 padding）。

---

## 6. 定位后的样本区间：可观察量汇总

以下数值均可通过 `SF_INFO.frames`、`sf_seek()` 返回值与 `sf_readf_*()` 返回值直接观察（3.0000625 s / 48 kHz 单声道，Opus、Vorbis 同为 144003 帧）。

| 观察量 | Opus | Vorbis | MP3 |
|---|---|---|---|
| `SF_INFO.frames` | 144003 | 144003 | 144047 |
| 时长（frames / rate） | 3.0000625 s | 3.0000625 s | 3.000979 s |
| 流内 granule 端点 | gp_start 0 / gp_end 144315 | pcm_start 0 / pcm_end 144003 | 127×1152 − 1105 − 1152 |
| 隐含延迟 | preskip 312；预滚 31680 | 预取半块 1024 | 首帧 1105；末帧 1152 |
| `seek(t)` 返回值 | 恒等于 `t`（`0 ≤ t ≤ frames`） | 恒等于 `t` | 恒等于 `t` |
| `seek(t)` 后读取的样本映射 | `out[i] = seq[t+i]` | 页搜索未落末页：`out[i] = seq[t+i]`；落末页：`out[i] = seq[t+δ+i]` | `out[i] = seq[t+i]` |
| 末页/末帧偏移 `δ` | 0 | 125（本文件；另测 957、48） | 0 |
| 结尾额外帧 | 0 | 0 | 44（相对源 PCM） |

结论：**只有 Vorbis 的末页页搜索**会在"返回的帧区间"与"对应的时间区间"之间引入固定偏移；其余情况（Opus 任意位置、Vorbis 非末页、MP3 任意位置）都是样本坐标恒等映射。

---

## 7. 短读 / 越界的调用者可依赖契约

### 7.1 返回值与缓冲区

文档 `docs/api.md:452-460` 声明"未填满的缓冲用零填充"。实际实现分两种情形：

**情形 A：读取在入口处已在 EOF 之后** —— 整块清零并返回 0：
```c
/* src/sndfile.c:2044-2047 */
if (psf->read_current >= psf->sf.frames)
{   psf_memset (ptr, 0, frames * psf->sf.channels * sizeof (float)) ;
    return 0 ; }
```
实测：`seek(frames)` 后 `readf_float(100)` 返回 0，100 帧全部为 0。

**情形 B：本次读取越过 `frames` 才被截断** —— 仅把"多出的部分"清零：
```c
/* src/sndfile.c:2060-2067 */
if (psf->read_current + count / psf->sf.channels <= psf->sf.frames)
    psf->read_current += count / psf->sf.channels ;
else
{   count = (psf->sf.frames - psf->read_current) * psf->sf.channels ;
    extra = frames * psf->sf.channels - count ;
    psf_memset (ptr + count, 0, extra * sizeof (float)) ;
    psf->read_current = psf->sf.frames ; }
```

**要害在于 `count` 的值**：`count` 是编解码器实际返回的样本数。若编解码器返回的样本数本身就没超过 `frames`，则走 `if` 分支、**`extra` 不会被清零**，缓冲区尾部保留调用者原有内容。实测：

| 场景 | 返回帧数 | `[返回, 请求)` 区间内容 |
|---|---|---|
| `seek(frames−5)` + `read(100)` | 5 | **未被触碰**（95 帧保留原值） |
| `seek(frames)` + `read(100)` | 0 | 全部清零 |
| 声明 `frames` 大于实际可解码量（本测试用改 gp 的 Opus 构造） | 恰好 `frames` | 多出部分**全部清零** |

因此可依赖的契约是：

1. `[0, 返回值)` 一定是本次读取写入的有效样本；
2. `[返回值, 请求)` 的内容**未定义**——可能是零（情形 A/B 的截断分支），也可能是调用者原有数据（情形 B 的非截断分支）。**不能依赖"未用部分被清零"**，这与 `docs/api.md:459-460` 的措辞不一致，且与无损格式（PCM/WAV）表现相同（同样不清零）。
3. 返回值永不超过 `frames − 读取前位置`。

### 7.2 定位失败时的读位置

`sf_seek()` 对失败有两条不同处理路径：

- **范围检查失败**（读模式下 `t < 0 || t > frames`）：在调用编解码器之前返回，`read_current` **不变**（`src/sndfile.c:1588-1591` 位于 `:1593` 的分派之前）。实测：`seek(frames+1)` → `-1`，`SEEK_CUR(0)` 仍是旧位置；随后 `read` 从旧位置继续。
- **编解码器内部失败**：`retval = psf->seek(...)` 之后 **无条件** `psf->read_current = retval`（`src/sndfile.c:1596-1601`），因此 `psf->seek` 返回 `-1` 时 `read_current` 被置为 `-1`。实测（截断的 FLAC 文件）：`seek(60000)` → `-1`，`SEEK_CUR(0)` → `-1`；再 `read()` 返回 0；随后一次成功 `seek(1000)` 可恢复正常（`SEEK_CUR(0)` → 1000）。

可依赖的契约：**`sf_seek()` 返回负值时，读位置应视为未知**。不要假设它保持不变；若需继续，应显式重新 `sf_seek()`。这也解释了为何在截断文件上 `SEEK_CUR(0)` 可能返回 `-1`——它直接返回 `psf->read_current`（`src/sndfile.c:1543-1548`），不调用编解码器。

### 7.3 不可 seek 的输入

管道输入时 `psf->is_pipe` 为真，`sf.seekable = 0`（`src/sndfile.c:3069-3072`），Opus 的 `analyze_file` 也据此跳过末页扫描并令 `frames = SF_COUNT_MAX`（`src/ogg_opus.c:1412-1415`、`:1462-1463`）。实测从 stdin 读取 Opus：`frames = 9223372036854775807`、`seekable = 0`，任何 `sf_seek` 均返回 `-1` 且错误串为 `"Seek attempted on unseekable file type."`。此时若必须定位，唯一办法是顺序读取并丢弃。

### 7.4 声明长度与实际可解码长度不一致

文件被截断或 granule 元数据不可信时，`sf_seek` 的上界仍是**声明**的 `frames`，而不是实际能解出的长度：

- 截断的 MP3（声明 144047，实际可解 56495）：`seek(100000)` 仍返回 100000，随后 `read` 返回 0，无错误；`SEEK_CUR(0)` 保持 100000。
- 截断的 Opus（声明并重算为 47688）：`seek(47680)` 返回 47680，`read(16)` 只返回 8 帧（短读），位置推进到 47688。
- 截断的 FLAC（声明 144003，实际 59904）：`seek(60000)` 触发编解码器失败 → `-1`，`read_current` 变 `-1`。

即：**"定位成功"不保证"其后能读到数据"**；调用者必须以读取返回值为准，并把 0 视为该坐标处流已结束。

---

## 8. 一个会改变定位后样本内容的状态相关缺陷（Vorbis 同句柄前跳）

在 Vorbis 上，若同一 `SNDFILE*` 之前已经读过/定位过（解码器处于流中段），随后向**未来 2 秒内**定位，`vorbis_seek_trysearch()` 会短路（`src/ogg_vorbis.c:905-907`），由 `vorbis_skip()` 直接在既有解码器状态上推进。此时 `vorbis_skip` 的"块包含目标"判定使用了**大块长度**而非实际包块：

```c
/* src/ogg_vorbis.c:874-876 */
if (vdata->gp + ((thisblock + blocksize) / 4) >= target)
    break ;
```

被跳过的包只做了 `vorbis_synthesis_trackonly/blockin`（`:878-882`），而先前已被输出并丢弃的块左半部无法再参与重叠，于是**定位后的前若干样本不属于任何真正的解码结果**，直到第一个"完全由定位点之后数据构成"的块才恢复精确。

实测（3 s Vorbis，大块 2048、小块 256）：

| 前置操作 | `seek(10000)` 后 2048 帧中的错误帧数 | 错误区间 |
|---|---|---|
| 新开句柄，直接 `seek(10000)` | 0 | — |
| 先 `read(1)`，再 `seek(10000)` | 2048（整块） | `[0, 2048)` |
| 先 `read(512)`，再 `seek(10000)` | 941 | `[0, 941)` |
| 先 `read(8192)`，再 `seek(10000)` | 941 | `[0, 941)` |
| 先 `seek(5000)+read(512)`，再 `seek(31679)` | 768 | `[0, 767)` |

错误只出现在**前缀**，且长度受一个 Vorbis 大块（2048 帧，42.7 ms）约束；之后的内容恢复为逐样本一致。这解释了"整块损坏"与"部分损坏"的分界：

| 前置操作 | `seek(10000)` 后损坏帧数 | 说明 |
|---|---|---|
| `read(1)` / `read(64)` | 2048 | 此前解码的块几乎未输出，重叠历史缺失一整块 |
| `read(256)` 及以上 | 约 941 | 已输出超过半块，仅首半块缺失 |

同样的前置操作在 Opus、MP3、FLAC 上均不产生任何错误帧（在 660 ms 预滚边界两侧、以及远距前跳上实测最大差为 0，MP3 为浮点舍入级的 3e-8）。

规避方式（对调用者有实际意义）：

1. 一次定位后重新打开文件，或
2. 使目标落在"当前位置之后超过 2 秒"或"当前位置之前"以强制走页搜索路径（`src/ogg_vorbis.c:905-907` 条件不成立即为页搜索），或
3. 定位后丢弃首个大块长度的样本再使用。

需要强调：该缺陷只影响 **Vorbis + 同一句柄 + 前向且 < 2 s** 的定位；它不影响 §7 的返回值/长度契约，只影响前缀样本的**内容**。

---

## 9. 验证方法与复现

- 构建：本仓库 `396a715`，CMake Debug，`ENABLE_EXTERNAL_LIBS=ON`、`ENABLE_MPEG=ON`；链接 libvorbis 1.3.7、libopus 1.4、mpg123 1.32.5、libFLAC 1.4.3。
- 测试信号：48 kHz 单声道 3.0 s（正弦、粉噪、白噪）与 44.1 kHz 立体声 20 s（粉噪），用 ffmpeg 编码为 Ogg/Vorbis、Ogg/Opus、MP3(LAME)、FLAC。
- 测量手段：
  1. `SF_INFO`、`sf_seek`/`sf_readf_float` 返回值与 `SFC_GET_LOG_INFO` 日志（含 `PCM offset`/`PCM end`/`Granule pos offset`/`Last Granule pos`/`Preskip`）。
  2. "同句柄顺序解码"作为基准：先完整解码，再逐点 `sf_seek(t)` 读取并与基准逐样本比对，同时在全范围内搜索最佳偏移（lag）以区分"位置错误"与"编解码差异"。
  3. 直接解析 Ogg 页（granule position、段表、包数），以及用 libvorbis/libogg 绕过 libsndfile 冷启动解码末页，独立测出 §5.2 的 `δ`。
  4. 用 ffmpeg 独立解码作为第二参考，确认 Vorbis 顺序解码本身与 ffmpeg 一致（RMS 8.8e-6），从而排除基准自身偏差。
  5. 构造性测试：修改末页 granule position（重算 Ogg CRC）以夸大/缩小 `frames`，以及截断文件，用于验证 §7.4 与 §7.1 两种清零分支。

---

## 10. 对调用者的建议

1. **坐标系**：把 `sf_seek()` 的返回值当作唯一权威位置；`SEEK_END` 的基准是 `SF_INFO.frames`（已扣除延迟），不是文件长度。
2. **缓冲区**：只信任 `[0, 返回值)`；`[返回值, 请求)` 不要假设为零（情形 B 的非截断分支会把调用者原有数据留在原地），也不要跨调用复用未初始化数据来判断 EOF。
3. **失败处理**：`sf_seek() < 0` 后不要依赖 `SEEK_CUR(0)`，显式重新定位；对可 seek 但有截断的文件，用"读到 0"而不是 `frames` 判断实际结尾。
4. **Vorbis 末页**：若应用依赖"定位后样本与顺序解码逐样本一致"，避免定位到最后 `δ` 帧（`δ` 是文件相关量，需实测或用 §5.2 的恒等式算）。检测手段：定位后用 `SFC_GET_LOG_INFO` 检查是否出现 `Cannot calculate ambiguous last page duration`；出现即说明落入末页。
5. **Vorbis 同句柄前跳**（§8）：在需要严格样本一致性的场景，定位后丢弃首个 ~2048 帧，或强制走页搜索（后向定位 / 跨 2 s 前跳）。
6. **Opus / MP3**：可安全依赖样本坐标恒等映射；Opus 额外提供 660 ms 预滚（`src/ogg_opus.c:193`）保证 MDCT 收敛，代价是每次远距定位多解约 31680 样本。
