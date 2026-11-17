# WAVEX 与 CAF 多声道布局解析差异报告

仓库：`0015-libsndfile-3-fd4d0639`（分支 `task/15`，HEAD `396a715`）
范围：`src/wavlike.c`、`src/wav.c`、`src/caf.c`、`src/chanmap.c`、`src/sndfile.c`、`src/aiff.c`、`include/sndfile.h`、`tests/command_test.c`、`tests/channel_test.c`。
不涉及外部编解码器；所有实验均用本仓库构建的 `libsndfile.so.1.0.37` 完成。

---

## 1. 统一读写接口

两种格式都收敛到同一对命令（`src/sndfile.c:1428-1474`）：

- `SFC_GET_CHANNEL_MAP_INFO`：若 `psf->channel_map == NULL` 返回 `SF_FALSE`；否则把 `psf->sf.channels` 个 `SF_CHANNEL_MAP_*` 枚举值（`include/sndfile.h:317-346`）拷贝给调用者。
- `SFC_SET_CHANNEL_MAP_INFO`：校验每个枚举值在 `(SF_CHANNEL_MAP_INVALID, SF_CHANNEL_MAP_MAX)` 区间内，**先把 map 存进 `psf->channel_map`（sndfile.c:1460-1466），再转发给容器的 command 回调**（sndfile.c:1472-1473），容器的返回值决定整个命令的成败。

关键点：map 只是给交错样本的每个声道位置贴语义标签，libsndfile **从不重排样本**；声道顺序始终由文件中的交错顺序决定。

## 2. WAVEX：位掩码模型

### 读取（`src/wavlike.c:328-383`）

- `fmt ` chunk 为 `WAVE_FORMAT_EXTENSIBLE` 时读出 `dwChannelMask`。
- `channel_mask_bits[]`（wavlike.c:87-110）把 18 个掩码位映射到 `SF_CHANNEL_MAP_*`；**第 k 个置位位（按位序号升序）标注第 k 个声道**（wavlike.c:357-368）。
- `channelmask == 0`：只记日志 `Channel Mask : 0x0 (should not be zero)`，`psf->channel_map` 保持 NULL → GET 失败。
- 置位数 < 声道数：分配完整数组（calloc，余项为 0 = `SF_CHANNEL_MAP_INVALID`），拷贝前 k 项，记日志 `*** Less channel map bits than there are channels.` → GET 成功但 map 部分无效。
- 置位数 > 声道数：循环条件 `k < psf->sf.channels` 使多余位被**静默截断**。wavlike.c:360-363 的 `*** More channel map bits ...` 警告是死代码（循环已保证 `k < channels`，`k > channels` 永不成立）。

### 写入（`src/wav.c:1368-1370`、`src/wavlike.c:496-520`、`src/wav.c:1055-1097`）

- `SFC_SET_CHANNEL_MAP_INFO` → `wavlike_gen_channel_mask()`：要求 map 对应的位序号**严格递增**，否则视为 "bad sequence" 返回 0 → 命令返回 `SF_FALSE`。
- 但文件仍会被写出：掩码为 0 时回退到按声道数的默认掩码（wav.c:1068-1093：1ch→0x4(C)，2ch→0x3，4ch→0x33 quad，6ch→0x3F 5.1，8ch→0xFF 7.1，其余→0 表示无映射；Ambisonic 强制写 0，wav.c:1058-1059）。

## 3. CAF：布局标签模型

### 读取（`src/caf.c:443-454, 796-831`、`src/chanmap.c`）

- `chan` chunk 含 3 个 u32：layout tag、bitmap、descriptions 数。**只使用 layout tag**；bitmap 字段被读出后直接丢弃——CAF 规范中 "use channel bitmap"（tag = `1<<16`）的布局实际不被支持。
- tag 经 `aiff_caf_of_channel_layout_tag()`（chanmap.c:245-262）在静态表 `chanmap.c` 中查找：高 16 位是布局编号，低 16 位是声道数（`tag & 0xffff`），声道数 >8 直接拒绝。
- 找到且 `channel_map != NULL`：按 `min(sf.channels, tag & 0xff)` 拷贝到按 `sf.channels` 分配的数组（caf.c:813-828）。caf.c:814-818 的注释说明这是修复后的行为：旧代码按 `sf.channels` 整体 `memcpy`，当 tag 描述的声道数少于文件声明声道数时会越界读静态表。
- tag 为 0（"Use channel descriptions"）或 `1<<16`（"Use channel bitmap"）：表项存在但 `channel_map == NULL`（chanmap.c:36-39）→ 不建 map → GET 失败。
- 未知 tag：`map_info == NULL`，静默忽略，GET 失败。

### 写入（`src/caf.c:236-238, 736-737`）

- `SFC_SET_CHANNEL_MAP_INFO` → `aiff_caf_find_channel_layout_tag()`（chanmap.c:226-243）：map 必须与表中某个布局**逐项完全相等**才返回非 0 tag；否则命令返回 `SF_FALSE`，且 `caf_write_header` 因 `chanmap_tag == 0` **不写 `chan` chunk**（caf.c:736）——读回时 GET 失败。
- AIFF 复用同一张表（`src/aiff.c:1605, 1453, 1779`），行为与 CAF 一致。

## 4. 两种模型的差异对照

| 方面 | WAVEX | CAF |
|---|---|---|
| 布局载体 | `dwChannelMask` 位掩码（18 位） | 4 字节 layout tag（查表） |
| 表达能力 | 任意置位子集（读）；写要求位序递增 | 仅表中预定义布局（chanmap.c，≤8 声道） |
| 声道数来源 | `fmt ` chunk 的 `channels` | `desc` chunk 的 `channels_per_frame` |
| 声道数与布局矛盾 | 部分填充 + 日志警告，GET 成功 | 截断拷贝（余项为 INVALID），GET 成功 |
| 未知/零布局 | mask=0 → 无 map，GET 失败 | 未知 tag / tag=0 / bitmap tag → 无 map，GET 失败 |
| bitmap 布局 | 支持（本身就是位图） | **不支持**（bitmap 字段被丢弃） |
| SET 失败后的文件 | 仍写默认掩码（可能语义不符） | 不写 `chan` chunk（无布局信息） |
| 默认布局（未 SET） | 按声道数写默认掩码 | 不写 `chan` chunk |

## 5. 对声道顺序与帧数的影响

- **声道顺序**：两种格式都只"贴标签"。WAVEX 按置位位升序标注声道 0..k；CAF 按表中数组顺序标注。样本在文件中的交错顺序永远不变，读取端不做任何重排。因此矛盾的布局只会**错标**声道（例如把实际是 C 的第 3 声道标成 INVALID），不会移动数据。
- **帧数**：完全不受布局影响。`frames = datalength / (channels × bytewidth)`，声道数来自 `fmt `/`desc` chunk 而非布局块。实验中所有异常文件的 `frames` 均与正常文件一致（见 §6）。

## 6. 异常输入的可观察结果（实验证据）

方法：用本仓库构建的 libsndfile 生成合法文件，再按字节修补制造矛盾，用 `sf_open` + `SFC_GET_CHANNEL_MAP_INFO` + `SFC_GET_LOG_INFO` 观察。

### 异常 1：WAVEX，6 声道，`dwChannelMask = 0x33`（仅 4 个置位位）

```
format=0x130002 channels=6 samplerate=44100 frames=4096
SFC_GET_CHANNEL_MAP_INFO -> 1
map: LEFT RIGHT REAR_LEFT REAR_RIGHT INVALID INVALID
log:  Channel Mask  : 0x33
      *** Less channel map bits than there are channels.
```

- 当前行为：打开成功、帧数不变；GET 成功但后 2 个声道为 `SF_CHANNEL_MAP_INVALID`（calloc 的 0 值，wavlike.c:351）。矛盾只出现在日志里，调用者不查日志就无法察觉。
- 相关边界：mask=0 时 GET 直接返回 `SF_FALSE`（日志 `should not be zero`）；mask=0xFF（位多于声道数）时多余位被静默截断为 `LEFT RIGHT`，wavlike.c:360 的警告是死代码。
- 建议：置位数与声道数不一致时，应让 `SFC_GET_CHANNEL_MAP_INFO` 返回 `SF_FALSE`（或至少提供 `SFC_` 级别的告警），并把 wavlike.c:360 的死代码警告移到循环之后使其真正生效。

### 异常 2：CAF，`desc` 声明 6 声道，`chan` 布局 tag = `0x650002`（立体声 (101<<16)|2）

```
format=0x180002 channels=6 samplerate=44100 frames=4096
SFC_GET_CHANNEL_MAP_INFO -> 1
map: LEFT RIGHT INVALID INVALID INVALID INVALID
log:  Tag    : 650002
      Layout : stereo (L, R)
```

- 当前行为：按 `min(6, 2)` 截断拷贝（caf.c:820），余项为 INVALID，GET 成功，无任何矛盾警告。这是 caf.c:814-818 注释所述越界读修复后的安全行为，但语义矛盾被静默吞掉。
- 相关边界：tag=0（"use channel descriptions"）时 GET 返回 `SF_FALSE`；bitmap 布局（tag=`1<<16`）的位图被读出后丢弃。
- 建议：当 `tag & 0xffff != sf.channels` 时记一条 `***` 日志（与 WAVEX 的 "Less channel map bits" 对齐）；长期应实现 CAF bitmap/描述符布局，或至少在遇到 bitmap tag 时显式声明不支持。

### 附带发现：SET 失败后的会话内不一致

对 WAVEX 写入 `SFC_SET_CHANNEL_MAP_INFO {RIGHT, LEFT}`（位序递减，`wavlike_gen_channel_mask` 判为 bad sequence）：

```
SET -> 0                       （命令报告失败）
GET（同一会话）-> 1 : 3 2       （内存中已是 RIGHT LEFT）
重新打开文件    -> LEFT RIGHT   （磁盘上写的是默认掩码 0x3）
```

原因：sndfile.c:1460-1466 先存 map，wav.c:1369 才生成掩码失败；写头时 `wavex_channelmask == 0` 回退默认掩码（wav.c:1068+）。CAF 同类失败则不写 `chan` chunk，读回 GET 失败。建议：容器回调失败时回滚 `psf->channel_map`，避免内存状态与磁盘状态分叉。

## 7. 测试证据

- `tests/command_test.c:1810-1873`（`channel_map_test`，覆盖 WAVEX/RF64/AIFF/CAF）：
  - 验证 WAVEX/RF64 即使不 SET 也有默认 map 可读（command_test.c:1826-1842，对应 wav.c:1068 的默认掩码表）；
  - 验证 `{LEFT, RIGHT, LFE, REAR_CENTER}`（即 DVD 5，tag `(134<<16)|4`）在四种容器上的 SET→写→GET 往返一致。
  - 缺口：没有覆盖矛盾布局（位数/声道数不符、tag 与声道数不符）、SET 失败路径、CAF bitmap tag。
- `tests/channel_test.c:56-122`：1–8 声道 WAV 文件的写入/读回帧数一致性，佐证帧数只由声道数决定、与布局无关。

## 8. 结论

WAVEX 与 CAF 在统一接口下采用了两种不对称的布局模型：位掩码（读时宽容、部分填充；写时要求位序递增、失败回退默认掩码）与布局标签（读时查表、截断拷贝；写时要求精确匹配、失败则省略 `chan` chunk）。未知布局在两者中都表现为 GET 返回 `SF_FALSE`；矛盾布局在两者中都表现为"部分无效 map + 至多一条日志"，且都不影响声道交错顺序与帧数。主要改进方向：矛盾时让失败可编程可见（GET 返回 FALSE 或显式告警）、修复 wavlike.c:360 死代码、SET 失败时回滚内存 map、补齐 CAF bitmap 布局支持或显式拒绝。
