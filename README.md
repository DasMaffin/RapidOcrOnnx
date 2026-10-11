# ocr — RapidOcrOnnx as an MCCP native module

This fork of [RapidAI/RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx) builds **ocr.dll**, a native module for
MCCP (Moddable Capture Card Provider): Lua mods `require("ocr")` and read text off captured frames. It is one Windows x64
DLL with the PP-OCRv4 models embedded. It writes no files, opens no network connections and needs no other DLL, not even
the VC++ runtime.

What this fork adds:

| File | What it is |
|---|---|
| [mccp/module.cpp](mccp/module.cpp) | The module: the Lua API, the job queue, the worker thread |
| [mccp/mccp.cmake](mccp/mccp.cmake) | The `-DOCR_OUTPUT=MCCP` build: pinned downloads, embedded models, the targets |
| [mccp/mccp_module.h](mccp/mccp_module.h) | MCCP's module interface, copied unchanged from MCCP's `include/mccp_module.h` (development branch) |
| [tools/host.cpp](tools/host.cpp) | A test program that loads ocr.dll the way MCCP does |
| [mod.txt](mod.txt) | `kind = native`, `version = 1.0.0` |
| `// MCCP:` blocks in 7 upstream files | Load models from memory, single-line recognition, no angle classifier ([below](#the--mccp-patch)) |

## Lua API

```lua
local ocr = require("ocr")                       -- bin/ocr.dll

-- One dialogue box: two text lines and the speaker's name plate.
local REGIONS = {
    { x = 400, y = 889, w = 1092, h = 67, mode = "line", key = 1 },
    { x = 400, y = 967, w = 1092, h = 67, mode = "line", key = 2 },
    { x = 345, y = 792, w = 383,  h = 67, mode = "line", key = "name" },
}

local waiting = false
hook.Add("OnFrameReceived", "reader", function(frame)
    if not waiting then
        waiting = ocr.read(frame, REGIONS, { lang = "en" }) ~= nil   -- copies the pixels, returns at once
    end
    local done = ocr.poll()                                          -- nil until a job has finished
    if not done then return end
    waiting = false
    if done.error then print("ocr failed: " .. done.error) return end
    for _, region in ipairs(done.regions) do
        for _, line in ipairs(region.lines) do
            print(region.key, line.text, string.format("%.3f", line.score))
        end
    end
end)
```

### `ocr.read(frame, regions [, opts])` → `id`, or `nil, "busy"`

Copies the regions' pixels out of the frame, queues one job and returns its id (1, 2, 3, …) without doing any OCR. If a
job is already waiting to start, nothing is queued and it returns `nil, "busy"`. One job can be running while another
waits.

- **frame**: the frame `OnFrameReceived` hands Lua. NV12 is what MCCP delivers. NV21, IYUV and YV12 work too: the
  module reads only the luma plane (`f.pitch` bytes per row), as greyscale.
- **regions**: an array of `{ x=, y=, w=, h=, mode=, key= }` in frame pixels. Each is clamped to the frame. A region
  entirely outside the frame comes back with no lines. At most 64 regions per call.
  - `mode = "line"` (the default): the region holds exactly one line of text. Recognition only, which is fast. The crop
    is narrowed to the columns that contain text, padded with its median value (8 px above and below, 16 px left and
    right) and recognised.
  - `mode = "auto"`: text detection, then recognition of every text block found. Use it when the lines' positions
    vary. It is much slower: about 370–570 ms per 1080p frame here, against 21–41 ms per line.
  - `key`: any Lua value. It comes back untouched in the result.
- **opts**: optional, `{ lang = "en" }`. The language is one of `ocr.languages()`; the default is `"en"`.

Bad arguments raise a Lua error, e.g. `bad argument #2 (regions[1]: needs numbers x, y, w and h)`.

### `ocr.poll()` → `nil`, or one finished job

```lua
{ id = 7, ms = 61.2, regions = {
    { key = 1, lines = { { text = "Scorbunny has been added to", score = 0.975,
                           x0 = 530, y0 = 888, x1 = 1376, y1 = 939 }, ... } },
    ... } }
-- or, if the job failed:
{ id = 7, error = "..." }
```

- `regions` are in the order they were given. `lines` are sorted top to bottom, then left to right.
- `text` is UTF-8. `score` is the mean of the per-character scores (0–1).
- Coordinates are frame pixels, and `x1`/`y1` lie one past the edge (`w = x1 - x0`). In `line` mode the box is the
  clamped region; in `auto` mode it is the detected text block.
- `ms` is the time the worker spent on the job.
- There is one slot for a finished job. If Lua doesn't poll before the next job finishes, only the newest result is
  kept.

### `ocr.languages()` → `{ "en", "ch" }` · `ocr.version()` → string

`"en"` is PP-OCRv4 English, which knows only ASCII. `"ch"` is PP-OCRv4 Chinese, which also reads English and has ’ — é
among its 6,623 characters. `ocr.version()` returns e.g.
`ocr 1.0.0 (RapidOcrOnnx 1.2.3, ONNX Runtime 1.15.1, OpenCV 4.8.1, PP-OCRv4)`.

### How it keeps MCCP's rules

- `read` only copies pixels; all OCR runs on **one worker thread**, started on the first `require` (`MCCP_MODULE_OPEN`)
  and joined in `MCCP_MODULE_CLOSE`.
- `LUA` is used only inside the call it came with. Region keys are Lua references, created and freed only on Lua's
  thread, in `read`, `poll` and close.
- Frame pointers are not kept: each region's luma is copied during `read`.
- Each job runs inside `try`/`catch (...)` and a failure becomes `{ id=, error= }`; the worker loop is wrapped too, so no
  exception leaves the thread. A hardware fault (an access violation) on that thread cannot be caught by C++ and would
  still take MCCP down, as MCCP's docs say.
- Each language's sessions (detection + recognition) are created on first use, used and destroyed only on the worker.
  English is loaded as soon as the worker starts. ONNX Runtime uses at most 4 threads (half the cores, at least 1).
- Models are loaded from the DLL's own resources. Upstream's file-writing debug paths (result images, result text) are
  off by default and never switched on.

## Build

Windows x64, Visual Studio 2022 with "Desktop development with C++" (CMake 3.18 or newer is included). From a command
prompt in the repository root:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOCR_OUTPUT=MCCP && cmake --build build --config Release
```

This gives `build\Release\ocr.dll` (about 40 MB, 23 MB of it models) and `build\Release\ocr_host.exe`. The configure step
downloads ONNX Runtime, OpenCV and the models (about 75 MB) into `build\mccp-downloads`, checks each SHA-256 and stops on
a mismatch. Nothing is downloaded at run time and no binary is committed. Release only, since the prebuilt libraries are
Release builds with the static CRT (`/MT`).

Options: `-DMCCP_OCR_CH=OFF` leaves out the Chinese model (−11 MB); `-DMCCP_OCR_HOST=OFF` skips the test program.
Without `-DOCR_OUTPUT=MCCP`, the upstream build (BIN/JNI/CLIB) is unchanged.

The build was verified with Visual Studio 2026 (MSVC 19.50, CMake 4.1), using `-G "Visual Studio 18 2026"` in the same
command. The prebuilt libraries are VS 2022 builds, and VS 2026 links them as-is.

## Test

`ocr_host` implements a minimal fake `mccp::Lua` (a value stack, tables, references), loads ocr.dll, calls
`mccp_module_open`, and runs `read`/`poll` on 1920×1080 PNGs converted to NV12. Luma is converted as BT.709 limited range
(16–235), like a capture card; `--full-range` gives 0–255. Rows are 64 bytes longer than the picture, so a pitch bug
would show.

```bat
build\Release\ocr_host.exe build\Release\ocr.dll path\to\samples
```

The samples are `leon_grookey_woods.png`, `system_scorbunny.png` and `subtitle_leon_1.png` from
[DasMaffin/Sw-Sh-Help-Overlay](https://github.com/DasMaffin/Sw-Sh-Help-Overlay) `dev/samples/`. It also checks argument
errors, the busy slot, newest-result-wins, key round-trips, clamping, and that no Lua reference is leaked.

### Test results

On an AMD Threadripper 2950X (ONNX Runtime on 4 threads), limited-range NV12. Line regions are the ones in
[tools/host.cpp](tools/host.cpp). Line time is the job time divided by its lines, as the mean of 5 jobs after a warm-up;
auto time is one whole 1920×1080 frame as a single `auto` region, the mean of 3.

| Line | Expected | `en` (line mode) | score | `ch` (line mode) | score |
|---|---|---|---|---|---|
| dialogue 1 | The Grass-type Pokémon Grookey, eh? | The Grass-type Pok**e**mon Grookey, eh? | 0.970 | The Grass-type Pok**e**mon Grookey, eh? | 0.977 |
| dialogue 2 | It’s soothing—like a nice, long walk in the woods. | It**'**s soothinglike a nice, long walk in the woods. | 0.976 | It**'**s soothing**--**like a nice, long walk in the woods. | 0.963 |
| name plate | Leon | Leon ✓ | 0.998 | Leon ✓ | 0.999 |
| system 1 | Scorbunny has been added to | ✓ | 0.975 | ✓ | 0.976 |
| system 2 | your party. | ✓ | 0.989 | ✓ | 0.961 |
| subtitle 1 | All right! | ✓ | 0.976 | ✓ | 0.871 |
| subtitle 2 | Line up, everyone! | ✓ | 0.950 | ✓ | 0.938 |

| Time | `en` | `ch` |
|---|---|---|
| line mode, per line | 21–32 ms | 25–41 ms |
| auto mode, per 1080p frame | 390–570 ms | 370–540 ms |

In auto mode both languages find exactly the expected lines on all three screenshots (3, 2 and 2). The texts match
line mode, except that `ch` reads "Pok**é**mon" correctly and `en` gives "soothing--like".

**The special characters:**

- **’ (curly apostrophe)**: never read. `en` cannot produce it at all; its dictionary
  ([en_dict.txt](https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/paddle/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile/en_dict.txt))
  is the 95 printable ASCII characters. `ch` has ’ but reads it as `'`.
- **— (long dash)**: `en` cannot produce it, and drops it or writes `--`. `ch` writes `--`; in a full-range test it
  once read `—` correctly in auto mode.
- **é**: `en` cannot produce it and writes `e`. `ch` reads it correctly in auto mode, but not in line mode.
- **Spacing** ("All right!", "Line up, everyone!"): correct in both. Spaces at the ends of a line are trimmed, since
  the recogniser sometimes adds them.

To get ’ — é reliably, a mod can map `'`→’ and `--`→— itself, or a Latin model can be added (see below).

**Line-mode preprocessing, measured.** Median padding alone made no consistent difference on these regions: padding
0/0, 4/8, 8/16 and 16/32 px each got 1–2 of the 7 lines exact, with different misreads each time. What hurt was regions
much wider than their text: "your party." fills under a quarter of its 846 px box, and the subtitles run over busy
scenery. Trimming the crop to the columns that contain text (a pixel more than 80 levels from the median), then padding,
took `en` from 1 to 5 exact lines. That is every line it can spell. It also raised the mean score from 0.88 to 0.98 and
made lines faster, since there are fewer pixels. A threshold of 48 left the subtitle clutter in. Full-range luma made no
consistent difference either, so the module uses luma as it comes.

## Where everything comes from

All downloaded by [mccp/mccp.cmake](mccp/mccp.cmake), pinned by SHA-256. The model hashes are the ones RapidOCR publishes
in [default_models.yaml](https://github.com/RapidAI/RapidOCR/blob/main/python/rapidocr/default_models.yaml); the key
files and libraries were hashed here.

| What | Source | SHA-256 | Licence |
|---|---|---|---|
| ONNX Runtime 1.15.1, static, /MT | [RapidAI/OnnxruntimeBuilder 1.15.1](https://github.com/RapidAI/OnnxruntimeBuilder/releases/download/1.15.1/onnxruntime-v1.15.1-windows-vs2022-static-mt.7z) | `3b02dc5ee8f1e38c8f16c4170e8ec3101a1a33c3fe08c56687c253f7de4febff` | MIT |
| OpenCV 4.8.1, static, /MT | [RapidAI/OpenCVBuilder 4.8.1](https://github.com/RapidAI/OpenCVBuilder/releases/download/4.8.1/opencv-4.8.1-windows-vs2022-mt.7z) | `9b647b479aad111f685efc0612a533c86c3ed1dc145c6d3c9408430fb9c0a019` | Apache-2.0 |
| Detection: PP-OCRv4 det (mobile) | [ch_PP-OCRv4_det_mobile.onnx](https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv4/det/ch_PP-OCRv4_det_mobile.onnx) | `d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9` | Apache-2.0 |
| `en`: PP-OCRv4 en rec (mobile) | [en_PP-OCRv4_rec_mobile.onnx](https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile.onnx) | `e8770c967605983d1570cdf5352041dfb68fa0c21664f49f47b155abd3e0e318` | Apache-2.0 |
| `en` keys | [en_dict.txt](https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/paddle/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile/en_dict.txt) | `5662df9d2d03f0e8ca0d3b0649d6acbab904b6a14b3d3521463c71c37c668ce3` | Apache-2.0 |
| `ch`: PP-OCRv4 ch rec (mobile) | [ch_PP-OCRv4_rec_mobile.onnx](https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile.onnx) | `48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b` | Apache-2.0 |
| `ch` keys | [ppocr_keys_v1.txt](https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/paddle/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile/ppocr_keys_v1.txt) | `28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7` | Apache-2.0 |

The models are [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)'s PP-OCRv4 models (Apache-2.0), as converted to
ONNX and published by [RapidOCR](https://github.com/RapidAI/RapidOCR) (Apache-2.0) on ModelScope. The angle classifier
is not used, since game text is never rotated.

### Licences

- RapidOcrOnnx, and so this fork: Apache-2.0 ([LICENSE](LICENSE)). Its `clipper.cpp`/`clipper.hpp` (Clipper 6.4.2) are
  under the Boost Software License 1.0.
- PP-OCR models and key files: Apache-2.0 (PaddleOCR).
- ONNX Runtime: MIT. Its static library also contains the third-party code listed in ONNX Runtime's
  [ThirdPartyNotices.txt](https://github.com/microsoft/onnxruntime/blob/v1.15.1/ThirdPartyNotices.txt) (protobuf, abseil
  and others, all permissive).
- OpenCV: Apache-2.0. The static build includes zlib, libpng, libjpeg-turbo, libtiff, libwebp, OpenJPEG, OpenEXR and
  flatbuffers, whose licences ship in the package's `etc/licenses`.

## The `// MCCP:` patch

Every change to upstream files is marked `// MCCP:` (`# MCCP:` in CMake); `git diff upstream/main -- CMakeLists.txt include src` shows all of it
(63 lines added, 2 removed).

| File | Change |
|---|---|
| `CMakeLists.txt` | With `-DOCR_OUTPUT=MCCP`, include `mccp/mccp.cmake` and return; other modes are untouched |
| `include/DbNet.h`, `src/DbNet.cpp` | `initModel(data, len)`: the detection session from memory, `Ort::Session(env, data, len, options)` |
| `include/CrnnNet.h`, `src/CrnnNet.cpp` | `initModel(model, len, keys, len)`: the recognition session and its key file from memory (lines split as `getline` would, `\r` dropped). `getTextLine(mat)` made public, so one line can be recognised on its own: text plus per-character scores, with no detection |
| `include/OcrLiteImpl.h`, `src/OcrLiteImpl.cpp` | `initModels(det, rec, keys)` from memory without the angle classifier (`detect(..., doAngle = false)` never touches it); `recognizer()` exposes the `CrnnNet` |

One more difference stays outside the upstream sources: upstream includes `<onnxruntime/core/session/onnxruntime_cxx_api.h>`,
but ONNX Runtime 1.15.1 ships that header flat. `mccp.cmake` writes a one-line forwarding header into the build folder
instead of editing five upstream headers.

## Adding a language

A language is one recognition model plus its key file. Add one `mccp_language(...)` call to
[mccp/mccp.cmake](mccp/mccp.cmake):

```cmake
mccp_language(latin
    ${MODELS}/onnx/PP-OCRv4/rec/latin_PP-OCRv3_rec_mobile.onnx
    e9d7a33667e8aaa702862975186adf2012e3f390cc0f9422865957125f8071cf
    ${MODELS}/paddle/PP-OCRv4/rec/latin_PP-OCRv3_rec_mobile/latin_dict.txt
    <sha256 of latin_dict.txt>)
```

It embeds `REC_LATIN` and `KEYS_LATIN`, lists `"latin"` in `ocr.languages()` and accepts `{ lang = "latin" }`. No C++
changes. RapidOCR publishes `japan_PP-OCRv4_rec_mobile` (`e1075a67…`) and `korean_PP-OCRv4_rec_mobile` (`ab151ba9…`)
the same way. The detection model is shared, but each language loads its own copy of the detection session on first
use.

---

*Below: upstream RapidOcrOnnx's README, unchanged.*

# RapidOcrOnnx

### 联系方式

[QQ群](https://rapidai.github.io/RapidOCRDocs/main/communicate/#qq)


### Project下载

* 整合好源码和依赖库的完整工程项目，可以在Release中下载(github)
* 可到Q群共享内下载，以Project开头的压缩包文件为源码工程，例：Project_RapidOcrOnnx-版本号.7z
* 如果想自己折腾，则请继续阅读本说明

### Demo下载(win、mac、linux)

* 编译好的demo，可以在release中下载，或者Q群共享内下载
* 各平台可执行文件：linux-bin.7z、macos-bin.7z、windows-bin.7z
* 用于java的jni库：linux-jni.7z、macos-jni.7z、windows-jni.7z
* 用于C的动态库：linux-clib.7z、macos-clib.7z、windows-clib.7z
* C动态库调用范例:[RapidOcrOnnxLibTest](https://github.com/RapidAI/RapidOcrOnnxLibTest)
* 注意：linux编译平台为ubuntu18.04，如果你的linux版本无法运行demo，请自行从源码编译依赖库和完整项目。

### 介绍

请查看项目主仓库：https://github.com/RapidAI/RapidOCR

这个项目使用onnxruntime框架进行推理

采用onnxruntime框架[https://github.com/microsoft/onnxruntime](https://github.com/microsoft/onnxruntime)

### 更新说明

#### 代码贡献者
* configuration parameters for the number of IntraOp threads by https://github.com/Gmgge
* 支持传入bitmap和image的二进制数据 by https://github.com/MyMonsterCat
* Enhance ONNX Runtime GPU inference performance by https://github.com/Tryanks
* Added DirectML GPU support by https://github.com/airchaoz

#### 2022-10-15 update v1.1.1

* opencv 4.6.0
* onnxruntime 1.12.1
* windows支持mt版引用库
* rec模型输入图片高度改为48

#### 2022-10-16 update v1.1.2

* 修复：字典添加空格

#### 2022-10-17 update v1.1.3

* 修复：scoreToTextLine方法索引越界问题
* Windows控制台编码修改为UTF8

#### 2022-10-20 update v1.2.0

* 再次修复空格问题
* 增加GPU(cuda)支持，需要自行下载整合依赖库
* windows下的free()方法更焕为_aligned_free()
* 修改默认输入参数
* 修改benchmark输出样式

#### 2022-10-28 update v1.2.1

* 适配onnxruntime 1.13.1
* 修了些warning

#### 2023-02-13 update v1.2.2

* 适配onnxruntime 1.14.0

#### 2024-01-07 update v1.2.3

* onnxruntime 1.15.1
* opencv 4.8.1

### 模型下载

整合好的范例工程自带了模型，在models文件夹中

```
RapidOcrOnnx/models
    ├── ch_PP-OCRv3_det_infer.onnx
    ├── ch_PP-OCRv3_rec_infer.onnx
    ├── ch_ppocr_mobile_v2.0_cls_infer.onnx
    └── ppocr_keys_v1.txt
```

### [编译说明](./BUILD.md)

### [GPU版附加说明](./onnxruntime-gpu/README.md)

### 测试说明

1. 根据系统下载对应的程序包linux-bin.7z、macos-bin.7z、windows-bin.7z，并解压.
2. 把上面的模型下载，解压到第一步解压的文件夹里.
3. 终端运行run-test.sh或命令行运行run-test.bat，查看识别结果.
4. 终端运行run-benchmark.sh或命令行运行run-benchmark.bat，查看识别过程平均耗时.

### FAQ

#### windows10下bat执行错误

- 在win、linux、mac系统下，文本文件使用不同的换行符格式，win是CRLF，linux是LF，mac是CR
- github的"Download ZIP"按钮下载的整个仓库代码，默认格式是UNIX换行符，此时bat脚本在windows平台执行会出错
- 解决方法1：从Release中下载完整工程压缩包
- 解决方法2：使用git clone命令同步仓库代码，git可以自动转换文件格式
- 解决方法3：使用UltraEdit等文本编辑器，把bat文件的换行符转成win格式

#### windows静态链接msvc

- 作用:静态链接CRT(mt)可以让编译出来的包，部署时不需要安装c++运行时，但会增大包体积；
- 需要mt版的引用库，参考编译说明，下载mt版的库；

#### windows提示缺少"VCRUNTIME140_1.dll"

下载安装适用于 Visual Studio 2015、2017 和 2019 的 Microsoft Visual C++ 可再发行软件包
[下载地址](https://support.microsoft.com/zh-cn/help/2977003/the-latest-supported-visual-c-downloads)

#### Windows7执行错误|中文乱码

1. cmd窗口左上角-属性
2. 字体选项卡-选择除了“点阵字体”以外的TrueType字体,例如:Lucida Console、宋体
3. 重新执行bat

### Windows调试运行

* 下载范例项目工程自带的引用库是Release版，不能用于调试运行
* debug版的引用库未压缩时容量超过1GB，极限压缩后也超过了100MB，请自行编译或到群共享里寻找
* debug版的引用库必须是md版
* 把debug版的引用库替换到范例工程的对应文件夹
* 双击generate-vs-project.bat，选择2)Debug，生成对应的build-win-vsxxx-xx文件夹
* 进入生成的文件夹，打开RapidOcrOnnx.sln
* 右边解决方案管理器，选中RapidOcrOnnx，右键->设为启动项目，并生成(查看输出log，确保生成成功)
* 如果引用库是dll，需要把对应的dll文件，例onnxruntime.dll复制到build-win-vsxxx-xx文件夹\Debug，跟上一步生成的RapidOcrOnnx.exe放在一起
* 右边解决方案管理器，选中RapidOcrOnnx，右键->属性->调试->
  命令参数->```--models ../models --det ch_PP-OCRv3_det_infer.onnx --cls ch_ppocr_mobile_v2.0_cls_infer.onnx --rec ch_PP-OCRv3_rec_infer.onnx --keys ppocr_keys_v1.txt --image ../images/1.jpg```
* 工具栏，点击绿色三角号启动"本地Windows调试器"
* 第一次运行的话，查看左下角，等待加载各dll符号，网络不好的话，要等挺久的

### 输入参数说明

* 请参考main.h中的命令行参数说明。
* 每个参数有一个短参数名和一个长参数名，用短的或长的均可。

1. ```-d或--models```：模型所在文件夹路径，可以相对路径也可以绝对路径。
2. ```-1或--det```:det模型文件名(含扩展名)
3. ```-2或--cls```:cls模型文件名(含扩展名)
4. ```-3或--rec```:rec模型文件名(含扩展名)
5. ```-4或--keys```:keys.txt文件名(含扩展名)
6. ```-i或--image```：目标图片路径，可以相对路径也可以绝对路径。
7. ```-t或--numThread```：线程数量。
8. ```-p或--padding```：图像预处理，在图片外周添加白边，用于提升识别率，文字框没有正确框住所有文字时，增加此值。
9. ```-s或--maxSideLen```
   ：按图片最长边的长度，此值为0代表不缩放，例：1024，如果图片长边大于1024则把图像整体缩小到1024再进行图像分割计算，如果图片长边小于1024则不缩放，如果图片长边小于32，则缩放到32。
10. ```-b或--boxScoreThresh```：文字框置信度门限，文字框没有正确框住所有文字时，减小此值。
11. ```-o或--boxThresh```：请自行试验。
12. ```-u或--unClipRatio```：单个文字框大小倍率，越大时单个文字框越大。此项与图片的大小相关，越大的图片此值应该越大。
13. ```-a或--doAngle```：启用(1)/禁用(0) 文字方向检测，只有图片倒置的情况下(旋转90~270度的图片)，才需要启用文字方向检测。
14. ```-A或--mostAngle```：启用(1)/禁用(0) 角度投票(整张图片以最大可能文字方向来识别)，当禁用文字方向检测时，此项也不起作用。
15. ```-h或--help```：打印命令行帮助。

### 关于内存泄漏与valgrind

* 项目根目录的valgrind-memcheck.sh用来检查内存泄漏(需要debug编译)。
* 常见的并行库有tbb，hpx，openmp，gcd，concurrency，pthread
* 并行库的种类可以看：https://docs.opencv.org/4.x/db/d05/tutorial_config_reference.html
* 测试了openmp和pthread，目前已知这类并行库会导致检查报告中出现"possibly lost"
* opencv只做简单的图像预处理，可以完全不使用任何并行库，但需要定制编译
* onnxruntime1.6.0或之前，默认引用openmp，从1.7.0开始默认关闭openmp并使用自带的ThreadPool代码
* 阅读报告可以看出"possibly lost"发生位置均在引用的第三方库(如果使用了并行库的话)，如opencv或onnxruntime
* "possibly lost"不一定是内存泄露
* 如果opencv想定制编译不使用任何并行库，可以使用以下参数进行编译

```
-DWITH_TBB=OFF
-DWITH_HPX=OFF
-DWITH_OPENMP=OFF
-DWITH_GCD=OFF
-DWITH_CONCURRENCY=OFF
-DWITH_PTHREADS_PF=OFF
```
