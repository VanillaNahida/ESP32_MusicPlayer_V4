// Music.cpp
#include "Music.h"
#include "AudioFileSourceFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorAAC.h"
#include "AudioOutputI2S.h"
#include "AudioBusLock.h"
#include "LyricEncoding.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <vector>

// SD引脚定义
#define SD_Pin 38
// ==================================================================
// I2S 音频引脚（连接 PCM5102A 模块）
//
// 【可用的引脚白名单】
// 这块模组是 ESP32-S3-WROOM-1-N16R8。扣掉已被占用的和被芯片保留的，
// 真正可自由使用的 GPIO 只有下面这 13 个：
//
//     1, 2, 8, 16, 17, 18, 21, 39, 40, 41, 42, 47, 48
//
// 其中 40 虽被 User_Setup.h 声明为 TOUCH_IRQ，但代码从不使用触摸中断
// （见 src/Touch.h 第 (1) 条），所以可以挪用；不放心就避开它。
//
// 【被占用的】：TFT 9~14、TOUCH_CS 15、TFT_BL 4、SD_CS 38、BOOT 0
// 【芯片保留，绝对不能用】：
//     GPIO 26~37 : SPI0/1，接内部 Flash 与 Octal PSRAM
//     GPIO 19/20 : USB-JTAG
//     GPIO 3/45/46 : strapping
//     GPIO 43/44 : UART0（串口日志）
//     （ESP32-S3 根本没有 GPIO22~25）
//
// 【推荐的组合，优先相邻以便接线】
//     BCLK=16 LRC=17 DOUT=18   ← 连续三针，最省事
//     BCLK=40 LRC=41 DOUT=42
//     BCLK=41 LRC=42 DOUT=47
//     BCLK=1  LRC=2  DOUT=8
//     BCLK=5  LRC=6  DOUT=7    ← 当前使用
//
// 【血泪教训】
//   · 曾设 LRC=35 / BCLK=27：驱动了 PSRAM 数据线，内存错乱，
//     看门狗反复复位（rst:0x8 TG1WDT_SYS_RST）。
//     判断依据：日志打印 "PSRAM 总容量: 8192 KB" 即说明 Octal PSRAM
//     已启用，26~37 一定不可用。
//   · 曾设 BCLK=15，而 TOUCH_CS 也是 15：同一引脚被两个外设占用，
//     触摸永远读到 z=0，但屏幕/SD 都正常，极难反推。
//
// 改这里时必须同步更新 src/Touch.cpp 顶部的冲突检查常量。
// ==================================================================
#define I2S_BCLK 1
#define I2S_LRC 2
#define I2S_DOUT 8

/* ---------------- 编译期引脚校验 ---------------- */
/* 1) 禁止落在芯片保留区 */
#if (I2S_BCLK >= 26 && I2S_BCLK <= 37) || (I2S_LRC >= 26 && I2S_LRC <= 37) || \
    (I2S_DOUT >= 26 && I2S_DOUT <= 37)
#error "I2S 引脚落在 SPI0/1 保留区(GPIO26~37)！N16R8 会因驱动 Flash/PSRAM 不断复位，请换用白名单里的引脚。"
#endif
#if (I2S_BCLK == 19) || (I2S_BCLK == 20) || (I2S_LRC == 19) || (I2S_LRC == 20) || \
    (I2S_DOUT == 19) || (I2S_DOUT == 20)
#error "I2S 引脚不能使用 GPIO19/20（USB-JTAG）！"
#endif
#if (I2S_BCLK == 3) || (I2S_LRC == 3) || (I2S_DOUT == 3) ||     \
    (I2S_BCLK == 45) || (I2S_LRC == 45) || (I2S_DOUT == 45) || \
    (I2S_BCLK == 46) || (I2S_LRC == 46) || (I2S_DOUT == 46)
#error "I2S 引脚不能使用 strapping 引脚(GPIO3/45/46)！"
#endif
#if (I2S_BCLK == 43) || (I2S_BCLK == 44) || (I2S_LRC == 43) || (I2S_LRC == 44) || \
    (I2S_DOUT == 43) || (I2S_DOUT == 44)
#error "I2S 引脚不能使用 GPIO43/44（UART0 串口日志）！"
#endif
/* 2) 禁止与已占用的外设引脚撞车 */
#if (I2S_BCLK == 4) || (I2S_LRC == 4) || (I2S_DOUT == 4) ||   \
    (I2S_BCLK == 38) || (I2S_LRC == 38) || (I2S_DOUT == 38) || \
    (I2S_BCLK == 0) || (I2S_LRC == 0) || (I2S_DOUT == 0) ||    \
    (I2S_BCLK == 15) || (I2S_LRC == 15) || (I2S_DOUT == 15)
#error "I2S 引脚与 TFT_BL(4)/SD_CS(38)/BOOT(0)/TOUCH_CS(15) 冲突！"
#endif
#if (I2S_BCLK >= 9 && I2S_BCLK <= 14) || (I2S_LRC >= 9 && I2S_LRC <= 14) || \
    (I2S_DOUT >= 9 && I2S_DOUT <= 14)
#error "I2S 引脚与 SPI 屏/触摸占用的 GPIO9~14 冲突！"
#endif
/* 3) 三根线之间也不能重复 */
#if (I2S_BCLK == I2S_LRC) || (I2S_BCLK == I2S_DOUT) || (I2S_LRC == I2S_DOUT)
#error "I2S 的三根线不能使用同一个 GPIO！"
#endif

// 播放列表缓存文件（二进制，便于整块顺序读写）
#define PLAYLIST_FILE "/music_playlist.bin"
#define PLAYLIST_MAGIC 0x314C504DUL // 'M''P''L''1'
#define PLAYLIST_VERSION 1
#define PLAYLIST_PATH_MAX 256 // 每条记录的固定长度（含结束符）
#define PLAYLIST_MAX_COUNT 20000
// 播放状态文件路径
#define PLAYSTATE_FILE "/music_state.txt"

/* 从 ID3 内嵌歌词导出的 .lrc 统一放这里（独立目录，不污染音乐文件夹）。
   播放时歌词查找顺序：
       1) 歌曲所在目录的同名 .lrc   （用户自己放的，优先级最高）
       2) /lrc/<歌曲文件名>.lrc     （重建播放列表时从 ID3 导出）
       3) 曲目内嵌的 USLT 歌词      （内存里，无需任何文件） */
#define LRC_DIR "/lrc"
// ID3 内嵌歌词的最大保留长度（与 AudioFileSourceID3 的 10KB 上限对齐）
#define LRC_TEXT_MAX 10240

// 播放列表缓存头
struct PlaylistHeader
{
  uint32_t magic;    // 魔数，用于校验文件有效性
  uint32_t version;  // 版本号
  uint32_t count;    // 歌曲数量
  uint32_t reserved; // 预留
};

// 音量
int volume = 15;

// 动态音乐数组
String* musicFiles = nullptr;
int fileCount = 0;           // 当前音乐文件数量
int maxAllocatedFiles = 0;    // 当前已分配的最大音乐文件数量
String folder = "/";    // 音乐文件夹路径
int music_i = 0;             // 当前播放索引
int music_prev_i = 0;        // 上一个播放索引

// ====== 音频播放对象：只允许解码任务（Music_Loop，core0）创建/销毁 ======
static AudioOutputI2S *s_output = nullptr;    // I2S 输出
static AudioFileSource *s_source = nullptr;   // 音频文件源
static AudioFileSourceID3 *s_id3 = nullptr;   // ID3 元数据解析
static AudioGenerator *s_generator = nullptr; // 解码器

// ====== 跨核共享状态（UI 只读，解码任务写） ======
static volatile bool s_decRunning = false; // 解码器是否已装载且在运行
static volatile bool s_decPaused = false;  // 是否暂停
static volatile bool s_decEnded = false;   // 曲目是否自然播放结束
static uint32_t s_playStartMs = 0;         // 播放开始时刻
static uint32_t s_pausedMs = 0;            // 累计暂停时长
static uint32_t s_pauseStartMs = 0;        // 本次暂停开始时刻
static volatile int s_pendingGain = -1;    // 待应用的增益百分比(0~100)，-1 表示无变化

/* ==================================================================
   UI → 解码任务的命令通道

   【为什么不再用「8 深的命令队列」】

   旧实现把 LOAD / PLAY / PAUSE 全塞进一个 8 深的队列，队列满就**直接丢弃**。
   实测串口日志（用户在界面上连点播放/暂停）：

       [AUDIO] 投递命令: PAUSE / PLAY / PAUSE / PLAY        ← 恰好 8 条被接收
       [AUDIO] 命令队列已满，命令被丢弃                      ← 之后每一条都被丢掉
       （整个过程中解码任务一条「收到命令」都没打印，心跳也停在开机那一行）

   解码任务单次 s_generator->loop() 最长可以阻塞上百毫秒，生产端（UI 5ms 一轮）
   瞬间就能把 8 个槽位灌满，而消费端一旦追不上就**再也追不上** ——
   表现就是「播放/暂停、切歌、音量、列表选曲全部没反应」，而且不会自愈。

   现在按语义拆成两条互不相同的通道，两者都**不可能丢掉用户操作**：

     1) 「重活」通道（装载 / 停止）：只有一个槽位，用 xQueueOverwrite 覆盖写。
        连点「下一曲」10 次，解码任务只会看到**最后一个**目标 ——
        这正是播放器应有的「最新意图优先」语义，而且写入永不失败。
     2) 「播放 / 暂停」意图：一个 volatile 变量，最新值即真相，不存在溢出。
        （音量本来就是这种模式：见 s_pendingGain。）
   ================================================================== */

/* 期望的暂停状态（UI 写、解码任务读）。
   它**不是**「解码器当前是否暂停」(s_decPaused)，而是「用户希望它暂停」。
   与旧实现的区别：这个意图在换曲时**不会**被清掉，所以
   「暂停状态下点下一曲」会保持暂停，UI 与实际播放状态始终一致。 */
static volatile bool s_wantPaused = false;

/* 解码任务存活计数：每跑一轮 Music_Loop() 自增一次。
   UI 侧用它判断「解码任务是不是卡死了」——如果它长时间不涨，
   那么 UI 再怎么点都不会有反应，问题在解码任务侧而不是事件绑定侧。 */
static volatile uint32_t s_loopSeq = 0;

enum AudioCmdType : uint8_t
{
  ACMD_LOAD = 0, // 装载并播放指定曲目（最新优先）
  ACMD_STOP      // 停止并释放
};

struct AudioCmdMsg
{
  uint8_t type;
  char path[300];
};

// 单槽「重活」请求队列：永远只保留最新的一条，写入永不失败
static QueueHandle_t s_loadQueue = nullptr;

// 元数据缓存（解码器回调运行在音频任务核心，先写入缓冲区，再由 Music_info() 拷贝给 UI）
static char s_metaTitle[256] = {0};
static char s_metaArtist[256] = {0};
static char s_metaAlbum[256] = {0};
static volatile bool s_metaDirty = false;

/* 本曲内嵌歌词（ID3 的 USLT / v2.2 的 ULT 帧），UTF-8。
   解码器回调（core0）写、UI（core1）读，因此用**静态缓冲区 + 就绪标志**
   而不是堆指针：跨核 malloc/free 一旦时序错开就是 use-after-free。
   播放新曲目时由 audioDoLoad() 清空。 */
static char s_id3Lrc[LRC_TEXT_MAX + 1] = {0};
static volatile bool s_id3LrcReady = false;
/* 这份内嵌歌词属于哪首曲目。
   必须带上它：UI 在「换歌的那一刻」就会去找歌词，而 ID3 要等解码任务
   真正开始读文件才解析完 —— 中间有个几百毫秒的窗口。
   没有这个归属信息的话，UI 可能把**上一首**残留的歌词当成新曲目的歌词。 */
static char s_id3LrcPath[300] = {0};

/* 拷贝一份内嵌歌词到静态缓冲（只保留前 LRC_TEXT_MAX 字节）。
   先写正文、最后才置就绪标志，读侧看到标志为真时内容一定已经写完。

   【这里要做两级编码适配】
   上游 AudioFileSourceID3 / libFLAC 交过来的字节有两类坑：
     1) FLAC 的 Vorbis comment 规范要求 UTF-8，但中文标注工具经常塞 GBK 进去；
     2) MP3 的 USLT 帧有些把编码标志写成 0（ISO-8859-1）而正文是 GBK，
        上游会老老实实按 Latin-1 把每个字节展开成 U+00XX —— 得到的是
        **完全合法的 UTF-8 乱码**，光靠编码校验发现不了。
   LyricEncoding_ToUtf8 解决第 1 类，RepairLatin1Mojibake 解决第 2 类。 */
static void storeId3Lyrics(const char *text)
{
  if (text == nullptr || text[0] == 0)
  {
    return;
  }
  s_id3LrcReady = false;

  const char *enc = "?";
  size_t n = LyricEncoding_ToUtf8((const uint8_t *)text, strlen(text),
                                  s_id3Lrc, sizeof(s_id3Lrc), &enc);
  if (n > 0 && LyricEncoding_RepairLatin1Mojibake(s_id3Lrc, sizeof(s_id3Lrc)))
  {
    n = strlen(s_id3Lrc);
    enc = "GBK(标签误标为Latin-1)";
  }
  if (n > LRC_TEXT_MAX)
  {
    n = LRC_TEXT_MAX;
  }
  s_id3Lrc[n] = 0;
  s_id3LrcReady = true;

  /* 只在真的做了转码时打一行，方便判断「内嵌歌词是不是编码不对」。
     UTF-8 是常态，打出来只会刷屏。 */
  if (strcmp(enc, "UTF-8") != 0 && strcmp(enc, "ASCII") != 0)
  {
    Serial.printf("[LRC] 内嵌歌词编码: %s，已转成 UTF-8（%u 字节）\n",
                  enc, (unsigned)n);
  }
}

bool durationPrinted = false; //
/* 【跨核可见性】duration 由解码任务（core0）在元数据回调里写，
   由 UI（core1）在 UI_update() 里读。不加 volatile 时编译器可以把
   读取提到循环外或缓存进寄存器，表现为「时长永远是 0、进度条不动」。
   UI 侧请统一用 Music_GetDuration() 读取。 */
static volatile long s_duration = 0;

/* 取当前曲目总时长（秒）。0 表示尚未解析出来。 */
long Music_GetDuration()
{
  return s_duration;
}

/* 设置总时长（秒）。只允许解码任务的元数据回调调用。 */
void Music_SetDuration(long sec)
{
  if (sec < 0)
  {
    sec = 0;
  }
  s_duration = sec;
}

// 当前音频元数据
String currentTitle = "";  // 曲目名
String currentArtist = ""; // 歌手名
String currentAlbum = "";  // 专辑名

// LRC歌词相关变量
uint8_t lrc_m = 0;            // 分钟
uint8_t lrc_s = 0;            // 秒
String lrc_time;              // 时间字符串
boolean lrc_flag = 0;         // 是否存在歌词标志位
const size_t maxLyrics = 200; // 最大歌词行数
LyricEntry lyrics[maxLyrics]; // 歌词数组
int lyricCount = 0;           // 实际歌词行数
uint16_t temp_AudioCurrentTime = 1;

PlayMode currentPlayMode = LIST_LOOP; // 默认列表循环
uint8_t pause_status = 0;             // 暂停状态//

File file;

// ============== 音频播放内部辅助函数 ==============

/* 元数据/时长回调（由解码器在音频任务中触发） */
static void audioMetadataCB(void *cbData, const char *type, bool isUnicode, const char *string)
{
  (void)cbData;
  (void)isUnicode;
  if (type == nullptr || string == nullptr)
    return;

  String tag = type;
  if (tag.equalsIgnoreCase("TIT2") || tag.equalsIgnoreCase("TT2") || tag.equalsIgnoreCase("title"))
  {
    strncpy(s_metaTitle, string, sizeof(s_metaTitle) - 1);
    s_metaTitle[sizeof(s_metaTitle) - 1] = 0;
    s_metaDirty = true;
  }
  else if (tag.equalsIgnoreCase("TPE1") || tag.equalsIgnoreCase("TP1") ||
           tag.equalsIgnoreCase("artist") || tag.equalsIgnoreCase("performer"))
  {
    strncpy(s_metaArtist, string, sizeof(s_metaArtist) - 1);
    s_metaArtist[sizeof(s_metaArtist) - 1] = 0;
    s_metaDirty = true;
  }
  else if (tag.equalsIgnoreCase("TALB") || tag.equalsIgnoreCase("TAL") || tag.equalsIgnoreCase("album"))
  {
    strncpy(s_metaAlbum, string, sizeof(s_metaAlbum) - 1);
    s_metaAlbum[sizeof(s_metaAlbum) - 1] = 0;
    s_metaDirty = true;
  }
  else if (tag.equalsIgnoreCase("tlen") || tag.equalsIgnoreCase("TLEN"))
  {
    long ms = atol(string);
    if (ms > 0)
    {
      Music_SetDuration(ms / 1000); // 回调单位是毫秒，转换为秒
    }
  }
  else if (tag.equalsIgnoreCase("USLT") || tag.equalsIgnoreCase("ULT") ||
           tag.equalsIgnoreCase("LYRICS") || tag.equalsIgnoreCase("UNSYNCEDLYRICS"))
  {
    /* 内嵌歌词。这样即使卡里既没有同名 .lrc、也没有 /lrc/ 下的导出文件，
       也能直接显示歌词（parseLrcFile 的第三级回退）。 */
    storeId3Lyrics(string);
  }
  // APIC（专辑封面）暂不处理
}

/* 规范化文件路径：统一以单个 '/' 开头 */
static String normalizePath(const String &path)
{
  String p = path;
  p.trim();
  if (p.length() == 0)
    return p;
  if (!p.startsWith("/"))
  {
    p = folder + "/" + p;
  }
  while (p.startsWith("//"))
  {
    p = p.substring(1);
  }
  return p;
}

/* 优先从 PSRAM 分配内存，不可用时回退到内部堆 */
static void *psramAlloc(size_t size)
{
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (p == nullptr)
  {
    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return p;
}

/* 释放当前解码器、文件源与输出（只能由解码任务调用） */
static void releaseAudio()
{
  if (s_generator != nullptr)
  {
    if (s_generator->isRunning())
    {
      s_generator->stop();
    }
    delete s_generator;
    s_generator = nullptr;
  }
  if (s_id3 != nullptr)
  {
    delete s_id3;
    s_id3 = nullptr;
  }
  if (s_source != nullptr)
  {
    delete s_source;
    s_source = nullptr;
  }
  if (s_output != nullptr)
  {
    s_output->stop();
    delete s_output;
    s_output = nullptr;
  }
  s_decRunning = false;
}

/* 停止播放（只能由解码任务调用） */
static void audioDoStop()
{
  releaseAudio();
  s_decPaused = false;
  s_decEnded = false;
  s_playStartMs = 0;
  s_pausedMs = 0;
}

/* 装载并开始播放指定曲目（只能由解码任务调用） */
static void audioDoLoad(const char *path)
{
  releaseAudio();
  // 切歌：释放上一首的专辑封面缓冲，新封面解析到后 UI 会自动替换
  AudioFileSourceID3::releasePicture();

  s_decPaused = false;
  s_decEnded = false;
  /* 【不要在这里清 s_wantPaused】
     用户的「暂停」意图必须跨换曲保留，否则「暂停中点下一曲」会突然出声。
     开机时的默认值是 false（即播放），由 Music_RestorePlayState() 显式保证。 */
  s_pausedMs = 0;
  s_pauseStartMs = 0;
  s_playStartMs = millis();
  Music_SetDuration(0); // 切歌先清零，等 Xing/ID3 回调送来新时长
  durationPrinted = false;
  s_metaTitle[0] = 0;
  s_metaArtist[0] = 0;
  s_metaAlbum[0] = 0;
  // 清掉上一首的内嵌歌词，等新曲目的 USLT 回调重新填充
  s_id3LrcReady = false;
  s_id3Lrc[0] = 0;

  String fullPath = normalizePath(path);

  /* 记录「接下来这份内嵌歌词属于哪首曲目」，并清掉上一首的残留。
     放在这里（而不是前面清 s_metaTitle 的地方）是因为要用到 fullPath。
     UI 侧靠 s_id3LrcPath 判断歌词是否已经属于当前曲目，见 parseLrcFile()。 */
  s_id3LrcReady = false;
  s_id3Lrc[0] = 0;
  strncpy(s_id3LrcPath, fullPath.c_str(), sizeof(s_id3LrcPath) - 1);
  s_id3LrcPath[sizeof(s_id3LrcPath) - 1] = 0;

  Serial.printf("[AUDIO] 装载: %s\n", fullPath.c_str());
  s_source = new AudioFileSourceFS(SD, fullPath.c_str());
  if (!s_source->isOpen())
  {
    Serial.printf("[AUDIO] 打开失败: %s\n", fullPath.c_str());
    releaseAudio();
    return;
  }
  // 校验能否真正读到数据（SPI 总线被 TFT 抢占时这里会露出来）
  uint8_t probe[64];
  uint32_t got = s_source->read(probe, sizeof(probe));
  Serial.printf("[AUDIO] 预读 %u 字节, 首字节 0x%02X%02X\n", (unsigned)got,
                got > 1 ? probe[0] : 0, got > 1 ? probe[1] : 0);
  s_source->seek(0, SEEK_SET);
  if (got == 0)
  {
    Serial.println("[AUDIO] SD 读取返回 0，无法解码");
    releaseAudio();
    return;
  }

  /* DMA 缓冲 = dma_buf_count × 256 帧。
     8  → 2048 帧 ≈ 46ms 音频
     16 → 4096 帧 ≈ 93ms 音频（当前使用）

     这个值决定「DMA 里随时存着多少音频」，也就是对 SD 读卡 / TFT 刷屏
     抢占 SPI 总线的容忍度 —— 一次 SD 读加上总线锁竞争偶尔会到十几毫秒，
     46ms 的余量偏紧，容易在刷屏与读卡撞车时听到爆音。
     放大到 93ms 后余量充足。

     那它会不会让 UI 变迟钝？不会了：所有 generator 的 loop() 都已经有
     单次调用的时间/样本上限（见 AudioLoopBudget.h），
     所以「缓冲越大 → 一轮喂得越久 → 命令等得越久」这条老因果已经不成立，
     缓冲大小现在只影响抗欠载能力。 */
  s_output = new AudioOutputI2S(0, 16);
  s_output->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  s_output->SetMclk(false);
  s_output->SetGain(volume / 21.0f); // 音量 0~21 映射为增益 0~1.0
  /* 单次 i2s_channel_write 的阻塞上限从默认 100ms 收到 30ms。
     它决定的是「解码任务一轮最多被卡多久」，也就是 UI 命令的最坏响应延迟。
     30ms 完全够用：DMA 每 23µs 就腾出一个帧位，真正需要的等待通常只有几毫秒；
     会等满超时的只有「I2S 通道卡住不消耗」这种异常情况 —— 那种时候早点返回
     反而更好，让 Music_Loop() 有机会去收 UI 命令。 */
  s_output->SetTimeout(30);

  Serial.printf("[AUDIO] I2S 引脚 BCLK=%d LRC=%d DOUT=%d, 增益=%.2f\n",
                I2S_BCLK, I2S_LRC, I2S_DOUT, (double)(volume / 21.0f));

  String lower = fullPath;
  lower.toLowerCase();
  AudioFileSource *srcForGen = s_source;

  /* 解码器分派：扩展名必须和 isMusicFile() 的收录范围严格对应。
     注意两点：
       1) .flac / .wav 用**原始文件源**，不套 AudioFileSourceID3 ——
          FLAC 的元数据是 Vorbis comment、WAV 的是 LIST/INFO，
          各自由解码器自己解析并通过 GENINFO 回调交给我们；
          套 ID3 反而会因为「流偏移 ≠ 文件偏移」把 FLAC 的 seek/tell 搞乱。
       2) 其它一律按 MP3 处理并套 ID3（MP3/AAC 的标签就是 ID3v2）。 */
  const char *decName = "MP3";
  if (lower.endsWith(".wav"))
  {
    decName = "WAV";
    s_generator = new AudioGeneratorWAV();
  }
  else if (lower.endsWith(".flac"))
  {
    decName = "FLAC";
    s_generator = new AudioGeneratorFLAC();
  }
  else if (lower.endsWith(".aac"))
  {
    decName = "AAC";
    s_id3 = new AudioFileSourceID3(s_source);
    s_id3->RegisterMetadataCB(audioMetadataCB, (void *)"ID3TAG");
    srcForGen = s_id3;
    s_generator = new AudioGeneratorAAC();
  }
  else
  {
    // 默认按 MP3 处理
    s_id3 = new AudioFileSourceID3(s_source);
    s_id3->RegisterMetadataCB(audioMetadataCB, (void *)"ID3TAG");
    srcForGen = s_id3;
    s_generator = new AudioGeneratorMP3();
  }
  Serial.printf("[AUDIO] 解码器: %s (源=%s)\n", decName,
                (srcForGen == s_id3) ? "ID3包装" : "原始流");

  s_generator->RegisterMetadataCB(audioMetadataCB, (void *)"GENINFO");

  if (!s_generator->begin(srcForGen, s_output))
  {
    Serial.printf("[AUDIO] 解码器初始化失败: %s\n", fullPath.c_str());
    releaseAudio();
    return;
  }

  s_decRunning = true;
  Serial.printf("[AUDIO] 开始播放: %s\n", fullPath.c_str());
}

// ============== 音乐系统初始化 ==============

// 存储卡是否已就绪（SD.begin 成功）
static bool s_sdReady = false;

/* 初始化音乐系统 —— **只做存储与请求通道**，不加载也不扫描播放列表。

   为什么把「加载/扫描播放列表」从这里挪走：
   首次使用（卡里没有 music_playlist.bin）需要全卡扫描，而扫描是要给用户
   看进度条的。进度界面依赖 LVGL，可 Music_Init() 是在 lv_init()/ui_init()
   **之前**调用的 —— 那时候还没有任何显示能力，只能干等一大片黑屏。
   所以现在拆成两步：
       setup():  Music_Init();          // 起 SD、建请求通道
                 ... lv_init / ui_init ...
                 Music_LoadPlaylistCache()   // 有缓存就秒加载
                 PlaylistTool_RunFirstScan() // 没缓存才带进度界面扫描
   这样做的好处是：无论哪条路径，第一次使用和按 BOOT 重建走的是**同一套**
   扫描代码和同一个进度界面，不会再出现两套逻辑各修一遍的情况。 */
void Music_Init()
{
  Serial.printf("PSRAM 总容量: %u KB, 空闲: %u KB\n",
                (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  Serial.printf("内部堆 空闲: %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));

  // 创建「重活」请求通道（单槽，最新优先，写入永不失败）
  if (s_loadQueue == nullptr)
  {
    s_loadQueue = xQueueCreate(1, sizeof(AudioCmdMsg));
  }

  // 复用 TFT_eSPI 已初始化的 SPI 总线，避免两处各自初始化 SPI2 主机导致 SD 读失效
  s_sdReady = SD.begin(SD_Pin, SPI, 10000000);
  if (!s_sdReady)
  {
    Serial.printf("SD卡初始化失败（CS = GPIO%d），请检查 SD 卡与接线\n", SD_Pin);
    Serial.flush();
    return;
  }
  Serial.println("SD卡初始化成功");
}

bool Music_IsStorageReady()
{
  return s_sdReady;
}

/* 尝试直接载入 SD 上的播放列表缓存。
   返回 true  = 缓存有效且已载入，不需要扫描；
   返回 false = 卡没就绪 / 没有缓存 / 缓存无效 —— 调用方应当走一次全卡扫描
                （用 Music_RescanBegin/Step/End，可以配进度界面）。 */
bool Music_LoadPlaylistCache()
{
  if (!s_sdReady)
  {
    Serial.println("SD 卡未就绪，无法加载播放列表");
    return false;
  }

  if (!playlistExists())
  {
    Serial.println("首次使用：卡里没有播放列表缓存，需要扫描存储卡");
    return false;
  }

  Serial.println("发现播放列表，正在加载...");
  if (!loadPlaylist())
  {
    Serial.println("播放列表缓存无效，需要重新扫描存储卡");
    return false;
  }

  Serial.printf("成功加载 %d 首歌曲\n", fileCount);
  return true;
}

/* 处理一条命令。返回 true 表示这条命令会「重活」（装载/停止），
   调用方据此决定是否要跳过本轮的解码，避免紧接着的 loop() 又长时间阻塞。 */
static bool handleCmd(const AudioCmdMsg &msg)
{
  Serial.printf("[AUDIO] 收到命令: type=%u (解码任务核心=%d)\n",
                (unsigned)msg.type, xPortGetCoreID());
  switch (msg.type)
  {
  case ACMD_LOAD:
    audioDoLoad(msg.path);
    return true;

  case ACMD_STOP:
    audioDoStop();
    return true;

  default:
    return false;
  }
}

/* 取走「重活」槽位里的最新请求（没有就返回 false）。
   返回 true 表示本轮处理过 LOAD/STOP 这类重活。 */
static bool drainCommands()
{
  if (s_loadQueue == nullptr)
  {
    return false;
  }
  AudioCmdMsg msg;
  if (xQueueReceive(s_loadQueue, &msg, 0) != pdTRUE)
  {
    return false;
  }
  return handleCmd(msg);
}

/* 把「用户期望的暂停状态」落实到解码器上。
   每一轮 Music_Loop() 都执行，因此这个意图永远不会丢：
   命令到达时解码器无论处于「正在装载」还是「已装载」，
   只要 s_decRunning 变 true，下一次进来就会生效。 */
static void applyPauseIntent()
{
  if (!s_decRunning)
  {
    return;
  }
  if (s_wantPaused && !s_decPaused)
  {
    s_decPaused = true;
    s_pauseStartMs = millis();
  }
  else if (!s_wantPaused && s_decPaused)
  {
    s_pausedMs += millis() - s_pauseStartMs;
    s_decPaused = false;
  }
}

// 主循环处理音频播放（在独立解码任务中运行，独占全部音频对象）
void Music_Loop()
{
  /* 存活计数放在最前面（任何 return 之前都要走到），
     这样 UI 侧看到的「解码任务还活着」是可信的。
     写成 read-modify-write 而不是 s_loopSeq++：C++20 起对 volatile
     用 ++ 会报 [-Wvolatile] 弃用告警。 */
  s_loopSeq = s_loopSeq + 1;

  /* 1. 处理 UI 投递的「重活」请求（单槽覆盖写，非阻塞）

     【为什么要「先处理命令、最后再解码」】
     s_generator->loop() 一次会把 I2S 缓冲填满，内部 i2s_channel_write
     的超时是 100ms，加上 MP3 解码本身，单次 Music_Loop() 可以阻塞
     100~200ms。而命令处理原本排在它**后面**就会白等一轮。
     所以这里的策略是：
       · 每轮开头先把命令取走（保证「按下去就有反应」）；
       · 若本轮处理过 LOAD/STOP 这种重活，就跳过本轮解码，直接进入
         下一轮 —— 让紧接着的命令有机会被立刻处理，不至于又等 200ms；
       · 解码放在最末尾，且只在解码器确实在跑时才做。 */
  bool heavyCmd = drainCommands();

  /* 2. 应用「播放/暂停」意图。
     这一条不走队列，因此不存在「队列满 → 用户按了没反应」的可能。 */
  applyPauseIntent();

  // 3. 应用待生效的音量
  //    注意：只有真正写进 I2S 输出后才清标志。若此刻还没有输出对象
  //    （尚未装载任何曲目），就把它留着，等装载完成后立刻生效 ——
  //    否则「开机先拖音量条」会看起来毫无反应。
  if (s_pendingGain >= 0 && s_output != nullptr)
  {
    s_output->SetGain(s_pendingGain / 100.0f);
    s_pendingGain = -1;
  }

  // 4. 刚处理过重活：本轮不再解码，优先让下一批命令得到响应
  if (heavyCmd)
  {
    /* 这条提前返回的路径也必须让出 CPU：本任务优先级最高，
       连续多轮不让出会把 core0 的 IDLE 饿死 → 看门狗复位整机。 */
    vTaskDelay(1);
    return;
  }

  // 5. 解码推进（暂停时完全不喂数据，I2S 由 auto_clear 输出静音）
  // 注意：这里不能持 SPI 总线锁 —— 一次解码会填满 I2S 缓冲(~90ms)再阻塞等待(~100ms)，
  // 持锁近 200ms 会把 core1 的刷屏彻底饿死。SPI 互斥已下移到实际的 SD 读取处
  // （见 AudioFileSourceFS::read / AudioBusLock.h）。
  if (s_decRunning && !s_decPaused && s_generator != nullptr && s_generator->isRunning())
  {
    /* 心跳：每秒打一次，用来判断解码任务是否还活着。
       如果「投递命令」有、但连心跳都停了，说明任务卡死或在某处永久阻塞，
       而不是命令队列的问题。同时打印栈水位，排除栈溢出。 */
    static uint32_t s_lastBeatMs = 0;
    static uint32_t s_loopCount = 0;
    s_loopCount++;
    uint32_t nowMs = millis();
    if (nowMs - s_lastBeatMs >= 1000)
    {
      Serial.printf("[AUDIO] 解码心跳: loop=%u 次/秒, 剩余栈=%u 字节\n",
                    (unsigned)s_loopCount,
                    (unsigned)uxTaskGetStackHighWaterMark(NULL));
      s_loopCount = 0;
      s_lastBeatMs = nowMs;
    }

    /* 【关键】给单次解码计时。
       本函数是 UI 命令的**唯一**消费点：只要这里的某一次调用不返回，
       界面上所有按钮就都会「没反应」。所有 generator 的 loop()
       都已做有界化处理（见 AudioLoopBudget.h），正常情况下单次远小于 50ms；
       一旦这里打印出警告，就说明解码器又出现了长时间不返回的路径，
       请优先排查 ESPAudio 库而不是 UI 侧。 */
    uint32_t t0 = millis();
    bool ok = s_generator->loop();
    uint32_t cost = millis() - t0;

    /* 【必须真正让出 CPU】
       本任务跑在 core0、优先级 configMAX_PRIORITIES-1（最高）。
       ⚠ vTaskDelay(0) 在 FreeRTOS 里是**空操作**（内部 `if (xTicksToDelay > 0)`
       才真正挂起），写它等于什么都没做。必须给至少 1 个 tick。
       不让出的话 core0 上的 IDLE 任务永远拿不到 CPU，
       ESP-IDF 的 Task Watchdog 会判定「IDLE0 没喂狗」并直接复位整机。 */
    vTaskDelay(1);

    if (cost >= 150)
    {
      Serial.printf("[AUDIO] 警告：单次解码阻塞 %u ms，期间 UI 命令无法处理\n",
                    (unsigned)cost);
    }

    if (!ok)
    {
      s_generator->stop();
      s_decRunning = false;
      s_decEnded = true;
      Serial.println("[AUDIO] 曲目播放结束");
    }
  }

  delay(1);
}

/* 把目录项的名字补成绝对路径。
   为什么要判断 name 是否已带 '/'：Arduino-ESP32 不同核心版本里
   File::name() 有时返回纯文件名、有时返回完整路径。
   直接无脑拼 dir + "/" + name 会得到 "/a/b/a/b/c.mp3" 这种坏路径，
   表现为「列表里有歌，但一播就打开失败」。 */
static String entryFullPath(const String &dir, const String &name)
{
  if (name.startsWith("/"))
  {
    return normalizePath(name);
  }
  if (dir.length() == 0)
  {
    return normalizePath(String("/") + name);
  }
  if (dir.endsWith("/"))
  {
    return normalizePath(dir + name);
  }
  return normalizePath(dir + "/" + name);
}

/* 阻塞式全卡递归扫描（开机自动建表、以及放弃重建后的兜底用）。
   注意：所有 SD 访问都包在 SPI 总线锁里 —— TFT 刷屏和它共用同一条总线，
   运行期（LVGL 已经在刷屏）不加锁会直接把 SPI 事务打乱。
   想要带进度的非阻塞版本请用 Music_RescanBegin/Step/End。 */
void scanAndAddMusicFiles(String dir)
{
  AudioBusLock();
  File root = SD.open(dir);
  AudioBusUnlock();
  if (!root)
  {
    return;
  }

  while (true)
  {
    AudioBusLock();
    File file = root.openNextFile();
    String name = file ? file.name() : String("");
    bool isDir = file ? file.isDirectory() : false;
    AudioBusUnlock();
    if (!file)
      break;

    if (isDir)
    {
      scanAndAddMusicFiles(entryFullPath(dir, name)); // 递归处理子目录
    }
    else if (isMusicFile(name))
    {
      addMusicFile(entryFullPath(dir, name));
    }

    AudioBusLock();
    file.close();
    AudioBusUnlock();
  }

  AudioBusLock();
  root.close();
  AudioBusUnlock();
}

// 重写 listMusicFiles 为完整实现
void listMusicFiles(String dir)
{
  // 先释放之前的数组
  freeMusicArray();
  
  // 初始分配 50 个位置
  if (!allocateMusicArray(50))
  {
    Serial.println("无法分配初始音乐数组");
    return;
  }
  
  // 扫描并添加所有文件
  scanAndAddMusicFiles(dir);
  
  Serial.printf("扫描完成，共发现 %d 首歌曲\n", fileCount);
}

/* 是否是「能被本机解码器播放」的音乐文件。

   ⚠ 这里的扩展名必须和 audioDoLoad() 的解码器分派严格一致：
       .wav  -> AudioGeneratorWAV
       .flac -> AudioGeneratorFLAC
       .aac  -> AudioGeneratorAAC
       其它  -> AudioGeneratorMP3（含 .mp3）

   旧实现收 .wma 却根本没有 WMA 解码分支，选中后必然打不开 ——
   表现就是「点了没反应」，正是这个项目一直在踩的那类坑，所以不再收它。

   另外：扫描器传进来的是完整路径（ESP32 core 3.x 的 File::name() 就是全路径），
   所以要先剥掉目录再判断扩展名，免得目录名里恰好含 ".mp3" 就误判。 */
bool isMusicFile(String name)
{
  int slash = name.lastIndexOf('/');
  String base = (slash >= 0) ? name.substring(slash + 1) : name;
  if (base.length() == 0 || base.startsWith("."))
  {
    return false; // 空名字或隐藏文件（如 ._xxx.mp3 这种 macOS 垃圾）
  }
  base.toLowerCase();
  return base.endsWith(".mp3") || base.endsWith(".flac") ||
         base.endsWith(".wav") || base.endsWith(".aac");
}

/* 解析一段 LRC 文本到 lyrics[]。
   文本来源可能是 SD 上的 .lrc 文件，也可能是曲目内嵌的 USLT 歌词，
   因此这里只认「字符串」，不关心它是从哪来的。 */
static void parseLrcText(const char *text, int &lyricCount)
{
  lyricCount = 0;
  if (text == nullptr)
  {
    return;
  }

  const char *p = text;
  // 跳过 UTF-8 BOM（某些编辑器/导出工具会加，加了会让第一行的 '[' 判断失败）
  if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
  {
    p += 3;
  }

  while (*p != 0 && lyricCount < (int)maxLyrics)
  {
    // 取一行
    const char *eol = strchr(p, '\n');
    size_t lineLen = (eol != nullptr) ? (size_t)(eol - p) : strlen(p);

    String line;
    line.reserve(lineLen + 1);
    for (size_t i = 0; i < lineLen; i++)
    {
      char c = p[i];
      if (c != '\r') // 统一换行符，免得 trim() 之外还要处理 CRLF
      {
        line += c;
      }
    }
    line.trim();

    p = (eol != nullptr) ? (eol + 1) : (p + lineLen);

    // 跳过空行和无时间标签行
    if (line.length() == 0 || !line.startsWith("["))
    {
      continue;
    }

    int endTag = line.indexOf("]");
    if (endTag == -1 || endTag < 3)
    {
      continue;
    }

    String timestampStr = line.substring(1, endTag);
    String lyricText = line.substring(endTag + 1);

    // 验证时间格式 mm:ss.xx
    if (timestampStr.length() < 5 ||
        timestampStr[2] != ':' ||
        (timestampStr[5] != '.' && timestampStr[5] != ':'))
    {
      continue;
    }

    uint8_t m1 = chartonumber(timestampStr[0]);
    uint8_t m2 = chartonumber(timestampStr[1]);
    uint8_t s1 = chartonumber(timestampStr[3]);
    uint8_t s2 = chartonumber(timestampStr[4]);

    lyrics[lyricCount].timestamp = (uint16_t)((m1 * 10 + m2) * 60 + s1 * 10 + s2);
    lyrics[lyricCount].lyric = lyricText;
    lyricCount++;
  }
}

/* 读取整个 .lrc 文件到内存，再交给 parseLrcText() 解析。
   为什么先把整个文件读出来：原实现在**持着 SPI 总线锁**的情况下逐行解析，
   总线被占用的时间随文件大小线性增长，会饿死 core1 的刷屏。
   现在只在读取的那一小段持锁（且用的是 psramAlloc），解析完全在锁外做。 */
static bool loadLrcFile(const char *filename, int &lyricCount)
{
  lyricCount = 0;
  if (filename == nullptr || filename[0] == 0)
  {
    return false;
  }

  char *buf = nullptr;
  size_t total = 0;
  const size_t cap = 32 * 1024; // LRC 文本远小于此，够用且不会吃光内存

  AudioBusLock();
  File file = SD.open(filename);
  if (file)
  {
    buf = (char *)psramAlloc(cap + 1);
    if (buf != nullptr)
    {
      while (total < cap)
      {
        int n = file.read((uint8_t *)buf + total, cap - total);
        if (n <= 0)
        {
          break;
        }
        total += n;
      }
      buf[total] = 0;
    }
    file.close();
  }
  AudioBusUnlock();

  if (buf == nullptr)
  {
    Serial.printf("无法打开 LRC 文件: %s\n", filename);
    return false;
  }

  /* 【编码适配】卡上的 .lrc 可能是 UTF-8、GBK/GB2312 或 UTF-16，
     直接按 UTF-8 解会让整篇歌词变成乱码（而且乱码码点在字库里没有字形，
     屏幕上一片空白，比乱码更难看出原因）。
     这里统一先转成 UTF-8，再交给解析器 —— 解析器只需要认一种编码。
     GBK→UTF-8 最多膨胀 1.5 倍，所以输出缓冲按 1.5 倍 + 余量给。 */
  char *utf8 = (char *)psramAlloc(cap * 3 / 2 + 4);
  if (utf8 == nullptr)
  {
    heap_caps_free(buf);
    Serial.println("LRC 转码缓冲分配失败");
    return false;
  }
  const char *enc = "?";
  size_t outLen = LyricEncoding_ToUtf8((const uint8_t *)buf, total, utf8, cap * 3 / 2 + 4, &enc);
  heap_caps_free(buf);

  Serial.printf("歌词文件编码: %s (%u → %u 字节) %s\n",
                enc, (unsigned)total, (unsigned)outLen, filename);

  parseLrcText(utf8, lyricCount);
  heap_caps_free(utf8);
  return true;
}

/* 换曲目时清掉上一首的曲目信息（只能由 UI 线程 core1 调用）。

   为什么需要：界面上的歌名/歌手都来自 currentTitle/currentArtist，
   而它们**只在新曲目的标签解析出来时**才会被覆盖。如果新曲目没有标签、
   或者标签解析得慢，界面就会一直显示上一首的信息 ——
   用户看到的就是「在列表里选了一首歌，文字却没跟着变」。
   清掉之后 Music_info() 会立刻用新曲目的文件名兜底，界面马上就有正确文字。

   currentTitle/Artist/Album 本来就只由 core1 的 Music_info() 写，
   所以在这里清它们是安全的，不会和 core0 的元数据回调抢内存。 */
void Music_ResetTrackInfo()
{
  currentTitle = "";
  currentArtist = "";
  currentAlbum = "";
  lrc_flag = 0;
  lyricCount = 0;
  durationPrinted = false;
}

/* 解码任务是否已经把指定曲目的内嵌歌词解析出来了。
   UI 侧用它决定「要不要为这首歌再找一次歌词」：
   换歌的瞬间就去 parseLrcFile() 是拿不到内嵌歌词的（ID3 还没解析完），
   所以一旦这个函数变 true，就需要补查一次。 */
bool Music_EmbeddedLyricsReady(const char *path)
{
  if (!s_id3LrcReady || path == nullptr)
  {
    return false;
  }
  String p = normalizePath(path);
  return strcmp(p.c_str(), s_id3LrcPath) == 0;
}

/* 把一段 UTF-8 歌词落盘成 /lrc/<歌名>.lrc。

   【只在真正要播放这首歌、并且确实解析出了带时间标签的歌词时才调用】
   重建播放列表时**不再**逐首解析 ID3 去导歌词 —— 那要把每首歌的整个
   标签（内嵌封面动辄几百 KB）读一遍，几百首就是几十秒到几分钟，
   而绝大多数歌其实你根本不会去播。歌词一律等到播放时才读取/生成：
   第一次播这首时从内嵌标签里取出来并存成 .lrc，以后就直接读文件。 */
static bool exportLyricsToLrcFile(const String &musicPath, const char *utf8)
{
  if (utf8 == nullptr || utf8[0] == 0)
  {
    return false;
  }

  int slash = musicPath.lastIndexOf('/');
  String file = (slash >= 0) ? musicPath.substring(slash + 1) : musicPath;
  int dot = file.lastIndexOf('.');
  String stem = (dot > 0) ? file.substring(0, dot) : file;
  if (stem.length() == 0)
  {
    return false;
  }

  // 统一成不带 BOM 的纯 UTF-8：带 BOM 会让第一行的 '[' 判断失败
  const uint8_t *body = (const uint8_t *)utf8;
  if (body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF)
  {
    body += 3;
  }
  size_t n = strlen((const char *)body);
  if (n == 0)
  {
    return false;
  }

  String lrcPath = String(LRC_DIR) + "/" + stem + ".lrc";

  AudioBusLock();
  if (!SD.exists(LRC_DIR))
  {
    SD.mkdir(LRC_DIR);
  }
  File f = SD.open(lrcPath, FILE_WRITE);
  bool ok = false;
  if (f)
  {
    f.write(body, n);
    if (body[n - 1] != '\n')
    {
      f.write((const uint8_t *)"\n", 1); // 保证以换行结尾，便于逐行读取
    }
    f.close();
    ok = true;
  }
  AudioBusUnlock();

  if (ok)
  {
    Serial.printf("[LRC] 已由内嵌歌词生成 %s (%u 字节)\n", lrcPath.c_str(), (unsigned)n);
  }
  else
  {
    Serial.printf("[LRC] 生成失败: %s\n", lrcPath.c_str());
  }
  return ok;
}

/* 查找并解析歌词。查找顺序：
     1) 歌曲所在目录的同名 .lrc      —— 用户自己放的，优先级最高
     2) /lrc/<歌曲文件名>.lrc        —— 之前播放时由内嵌歌词生成的
     3) 曲目内嵌的歌词               —— 内存里的，不需要任何文件；
                                        命中后会顺手落盘成 2)，下次直接读文件
   任何一级命中就停止，因此「同名文件」永远能覆盖生成/内嵌的结果。 */
void parseLrcFile(String musicName)
{
  String music_path = normalizePath(musicName);

  int slash = music_path.lastIndexOf('/');
  String dir = (slash >= 0) ? music_path.substring(0, slash) : String("");
  String file = (slash >= 0) ? music_path.substring(slash + 1) : music_path;
  int nameDot = file.lastIndexOf('.');
  String stem = (nameDot >= 0) ? file.substring(0, nameDot) : file;

  String candidates[2];
  candidates[0] = dir + "/" + stem + ".lrc";              // 歌曲旁边
  candidates[1] = String(LRC_DIR) + "/" + stem + ".lrc";  // /lrc/ 生成目录

  for (int i = 0; i < 2; i++)
  {
    AudioBusLock();
    bool exist = SD.exists(candidates[i]);
    AudioBusUnlock();
    if (!exist)
    {
      continue;
    }

    if (loadLrcFile(candidates[i].c_str(), lyricCount) && lyricCount > 0)
    {
      lrc_flag = 1;
      Serial.printf("找到歌词文件(%s): %s（%d 行）\n",
                    (i == 0) ? "歌曲同名" : "/lrc", candidates[i].c_str(), lyricCount);
      return;
    }
    /* 文件存在，但一行时间标签都解析不出来（纯文本歌词 / 空文件）。
       不要就此打住 —— 继续试下一个候选，最后回退到内嵌歌词。 */
    Serial.printf("歌词文件无可用的时间标签，继续查找: %s\n", candidates[i].c_str());
  }

  // 3) 回退到曲目内嵌歌词。
  //    必须校验归属：换歌瞬间 ID3 还没解析完，这里读到的可能仍是上一首的歌词。
  if (s_id3LrcReady && strcmp(music_path.c_str(), s_id3LrcPath) == 0)
  {
    parseLrcText(s_id3Lrc, lyricCount);
    if (lyricCount > 0)
    {
      lrc_flag = 1;
      Serial.printf("使用曲目内嵌歌词: %d 行\n", lyricCount);
      /* 顺手落盘成 /lrc/<歌名>.lrc —— 这就是「歌词只在要播放时才读取生成」：
         生成动作挂在「确实要播这首、并且确实取到了带时间标签的歌词」上，
         而不是在重建列表时对几百首根本不会播的歌做无用功。
         下次再播这首就直接读文件，连 ID3 都不用解析。 */
      exportLyricsToLrcFile(music_path, s_id3Lrc);
      return;
    }
    Serial.println("曲目内嵌歌词没有时间标签，无法按时显示");
  }

  lrc_flag = 0;
  lyricCount = 0;
  Serial.println("没有找到歌词文件");
}
// 打印歌词内容
void printLyrics(LyricEntry *lyrics, size_t lyricCount)
{
  for (size_t i = 0; i < lyricCount; i++)
  {
    Serial.printf("%d\n%s\n", lyrics[i].timestamp, lyrics[i].lyric.c_str());
  }
}

// 字符转数字
uint8_t chartonumber(char charnumber)
{
  if (charnumber >= '0' && charnumber <= '9')
  {
    return charnumber - '0';
  }
  return 0;
}

// ========== 播放控制模块 ==========

/* 投递「重活」请求给解码任务（由 UI 侧调用，非阻塞、永不失败）

   用 xQueueOverwrite 往单槽队列里写：写入永远成功，且只保留**最新**的一条。
   连点「下一曲」10 次，解码任务只会去装最后一首 —— 这既避免了旧实现
   「队列满就丢命令 → UI 按了没反应」，也省掉了一堆无意义的中间装载。 */
static void audioSendCmd(uint8_t type, const char *path)
{
  if (s_loadQueue == nullptr)
  {
    Serial.println("[AUDIO] 请求通道未就绪，命令被忽略");
    return;
  }
  AudioCmdMsg msg;
  msg.type = type;
  msg.path[0] = 0;
  if (path != nullptr)
  {
    strncpy(msg.path, path, sizeof(msg.path) - 1);
    msg.path[sizeof(msg.path) - 1] = 0;
  }

  // 单槽队列：覆盖写，永不阻塞、永不失败
  if (xQueueOverwrite(s_loadQueue, &msg) != pdTRUE)
  {
    Serial.println("[AUDIO] 覆盖写请求槽失败（不应发生）");
    return;
  }

  /* 排查「UI 操作没反应」用：确认请求确实投递出去了。
     如果这条打印了、但 Music_Loop 那边的「收到命令」没打印，
     说明解码任务没在跑（看 [AUDIO] 解码心跳 是否还在涨），问题在任务侧；
     反之则说明 UI 根本没调到这些函数，问题在事件绑定侧。 */
  static const char *kCmdName[] = {"LOAD", "STOP"};
  Serial.printf("[AUDIO] 投递命令: %s (ui核心=%d)\n",
                (type < 2) ? kCmdName[type] : "?", xPortGetCoreID());
}

/* 设置「用户期望的暂停状态」。
   UI 只改这个 volatile 变量，不碰任何音频对象；
   解码任务每轮 Music_Loop() 都会把它落实到解码器上，
   所以无论按多快、无论解码器当时在装载还是在播放，这个意图都不会丢。 */
void Music_WantPaused(bool paused)
{
  s_wantPaused = paused;
}

/* 解码任务存活计数。UI 侧拿它来判断「解码任务是否卡死」：
   若这个值长时间不变化，则所有 UI 音频操作都不会生效，
   问题在解码任务侧，而不是事件绑定侧。 */
uint32_t Music_GetLoopSeq()
{
  return s_loopSeq;
}

// 播放（无曲目时装载当前曲目，暂停状态则继续）
void Music_Play()
{
  Music_WantPaused(false);
  if (!s_decRunning)
  {
    if (fileCount > 0 && music_i >= 0 && music_i < fileCount)
    {
      Music_PlayPath(musicFiles[music_i].c_str());
    }
  }
}

// 暂停
void Music_Pause()
{
  Music_WantPaused(true);
}

// 下一曲
void Music_Next()
{
  if (fileCount == 0)
    return; // 无文件可播

  music_i = (music_i + 1) % fileCount; // 循环到下一首
  Music_PlayPath(musicFiles[music_i].c_str());
  Serial.print("下一曲: ");
  Serial.println(musicFiles[music_i]);

  // 保存播放状态
  savePlayState();
}

// 上一曲
void Music_Prev()
{
  if (fileCount == 0)
    return; // 无文件可播

  music_i = (music_i - 1 + fileCount) % fileCount; // 循环到上一首
  Music_PlayPath(musicFiles[music_i].c_str());
  Serial.print("上一曲: ");
  Serial.println(musicFiles[music_i]);

  // 保存播放状态
  savePlayState();
}

// 第一曲
void Music_First()
{
  if (fileCount == 0)
    return; // 无文件可播

  music_i = 0; // 播放第一首
  Music_PlayPath(musicFiles[music_i].c_str());
  Serial.print("第一曲: ");
  Serial.println(musicFiles[music_i]);
}

// 最后一曲
void Music_Last()
{
  if (fileCount == 0)
    return; // 无文件可播

  music_i = fileCount - 1; // 播放最后一曲
  Music_PlayPath(musicFiles[music_i].c_str());
  Serial.print("最后一曲: ");
  Serial.println(musicFiles[music_i]);
}

bool Music_IsPlaying()
{
  return s_decRunning && !s_decPaused && !s_decEnded;
}

/* 「曲目自然播放结束」事件，消费一次后清零。
   详见 Music.h 中的说明：UI 必须用它来判断「是否该自动切下一首」，
   而不能用 !Music_IsPlaying()（那个条件在装载中 / 暂停时也成立）。 */
bool Music_ConsumeEnded()
{
  if (s_decEnded)
  {
    s_decEnded = false;
    return true;
  }
  return false;
}

// 显示音频总时长
// 如果播放时间值跟之前找到歌词时间值不一样，确保只显示一次总时长
/*获取音频总时长，曲目名称，歌手信息*/
void Music_info()
{
  // 1. 应用解码器/ID3 回调缓存的最新元数据
  if (s_metaDirty)
  {
    if (s_metaTitle[0] != 0)
      currentTitle = s_metaTitle;
    if (s_metaArtist[0] != 0)
      currentArtist = s_metaArtist;
    if (s_metaAlbum[0] != 0)
      currentAlbum = s_metaAlbum;
    s_metaDirty = false;
  }

  // 2. 标题兜底：没有 ID3 标题时使用文件名（不含扩展名）
  if (currentTitle.length() == 0 && fileCount > 0 && music_i >= 0 && music_i < fileCount)
  {
    String fileName = musicFiles[music_i];
    // 去掉路径，只保留文件名
    int lastSlash = fileName.lastIndexOf("/");
    if (lastSlash != -1)
    {
      fileName = fileName.substring(lastSlash + 1);
    }
    // 去掉扩展名
    int dotIndex = fileName.lastIndexOf(".");
    if (dotIndex != -1)
    {
      fileName = fileName.substring(0, dotIndex);
    }
    currentTitle = fileName;
  }

  // 3. 歌手兜底
  if (currentArtist.length() == 0)
  {
    currentArtist = "未知歌手";
  }

  durationPrinted = (Music_GetDuration() != 0);
}

/*获取当前播放时长（秒）*/
uint32_t Music_GetCurrentPlayTime()
{
  if (!s_decRunning || s_playStartMs == 0)
  {
    return 0;
  }
  uint32_t now = s_decPaused ? s_pauseStartMs : millis();
  uint32_t elapsed = now - s_playStartMs - s_pausedMs;
  return elapsed / 1000;
}

/* 获取当前曲目内嵌的专辑封面（原始图片数据） */
bool Music_GetAlbumCover(const uint8_t **data, size_t *size, uint32_t *revision)
{
  const uint8_t *pic = AudioFileSourceID3::pictureData();
  size_t len = AudioFileSourceID3::pictureSize();
  if (data)
    *data = pic;
  if (size)
    *size = len;
  if (revision)
    *revision = AudioFileSourceID3::pictureRevision();
  return (pic != nullptr) && (len > 0);
}

/*播放指定路径下的音频*/
void Music_PlayPath(const char *path)
{
  if (path == nullptr)
    return;

  // 只投递命令，实际的对象创建/销毁全部由解码任务完成（跨核安全）
  audioSendCmd(ACMD_LOAD, path);
}

/*模式切换函数（可绑定到UI按钮）*/
void switchPlayMode()
{
  currentPlayMode = static_cast<PlayMode>(
      (currentPlayMode + 1) % 3); // 循环切换三种模式

  // 更新UI显示模式状态
  const char *modeText[] = {"单曲循环", "列表循环", "随机播放"};
  // lv_label_set_text(ui_modeLabel, modeText[currentPlayMode]);
}

/*设置音量*/
void setVolume(uint8_t v)
{
  if (v > 21)
  {
    v = 21;
  }
  volume = v;
  // 真正的 SetGain 由解码任务执行（音频对象归解码任务所有）
  s_pendingGain = (int)((v * 100) / 21);
}

/*获取音量值*/
uint8_t getVolume()
{
  return (uint8_t)volume;
}

// 检查播放列表是否存在
bool playlistExists()
{
    return SD.exists(PLAYLIST_FILE);
}

// 保存播放列表到SD卡（二进制格式）
bool savePlaylist()
{
    if (fileCount == 0)
    {
        Serial.println("没有歌曲可以保存到播放列表");
        return false;
    }

    File playlistFile = SD.open(PLAYLIST_FILE, FILE_WRITE);
    if (!playlistFile)
    {
        Serial.println("无法创建播放列表缓存文件");
        return false;
    }

    // 写入文件头
    PlaylistHeader hdr;
    hdr.magic = PLAYLIST_MAGIC;
    hdr.version = PLAYLIST_VERSION;
    hdr.count = (uint32_t)fileCount;
    hdr.reserved = 0;
    if (playlistFile.write((const uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr))
    {
        Serial.println("写入播放列表头失败");
        playlistFile.close();
        return false;
    }

    // 每条记录定长写入，便于整块顺序读取
    char record[PLAYLIST_PATH_MAX];
    for (int i = 0; i < fileCount; i++)
    {
        memset(record, 0, sizeof(record));
        strncpy(record, musicFiles[i].c_str(), PLAYLIST_PATH_MAX - 1);
        if (playlistFile.write((const uint8_t *)record, sizeof(record)) != sizeof(record))
        {
            Serial.println("写入播放列表记录失败");
            playlistFile.close();
            return false;
        }
    }

    playlistFile.close();
    Serial.printf("播放列表缓存已保存，共 %d 首歌曲\n", fileCount);
    return true;
}

// 从SD卡加载播放列表（二进制格式）
bool loadPlaylist()
{
    File playlistFile = SD.open(PLAYLIST_FILE, FILE_READ);
    if (!playlistFile)
    {
        Serial.println("无法打开播放列表缓存文件");
        return false;
    }

    // 读取并校验文件头
    PlaylistHeader hdr;
    if (playlistFile.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic != PLAYLIST_MAGIC ||
        hdr.version != PLAYLIST_VERSION ||
        hdr.count == 0 || hdr.count > PLAYLIST_MAX_COUNT)
    {
        Serial.println("播放列表缓存无效，将重新扫描SD卡");
        playlistFile.close();
        return false;
    }

    // 分配刚好足够的内存
    if (!allocateMusicArray((int)hdr.count))
    {
        Serial.println("无法分配播放列表内存");
        playlistFile.close();
        return false;
    }

    // 分块读取（缓冲区优先放在 PSRAM，减少内部 RAM 占用与碎片）
    size_t chunkRecords = 128;
    if (chunkRecords > hdr.count)
    {
        chunkRecords = hdr.count;
    }
    size_t chunkBytes = chunkRecords * PLAYLIST_PATH_MAX;
    char *buf = (char *)psramAlloc(chunkBytes);
    if (buf == nullptr)
    {
        Serial.println("播放列表缓冲区分配失败");
        playlistFile.close();
        return false;
    }

    uint32_t loaded = 0;
    while (loaded < hdr.count)
    {
        size_t remain = (size_t)(hdr.count - loaded);
        size_t want = (remain < chunkRecords) ? remain : chunkRecords;
        size_t got = playlistFile.read((uint8_t *)buf, want * PLAYLIST_PATH_MAX);
        size_t n = got / PLAYLIST_PATH_MAX;
        if (n == 0)
        {
            break;
        }
        for (size_t i = 0; i < n; i++)
        {
            char *record = buf + i * PLAYLIST_PATH_MAX;
            record[PLAYLIST_PATH_MAX - 1] = 0; // 强制结束符，避免越界
            if (record[0] != 0)
            {
                musicFiles[fileCount++] = String(record);
            }
        }
        loaded += n;
    }
    heap_caps_free(buf);
    playlistFile.close();

    if (fileCount != (int)hdr.count)
    {
        Serial.printf("警告：期望加载 %u 首，实际加载 %d 首\n", hdr.count, fileCount);
    }

    return fileCount > 0;
}

// 刷新播放列表（重新扫描SD卡）
void refreshPlaylist()
{
    fileCount = 0;
    listMusicFiles(folder);
    if (fileCount > 0)
    {
        savePlaylist();
        Serial.printf("播放列表已刷新，共 %d 首歌曲\n", fileCount);
    }
    else
    {
        Serial.println("刷新失败：未找到任何歌曲");
    }
}

// ============== 动态数组管理函数 ==============

// 分配音乐数组
bool allocateMusicArray(int size)
{
    // 先释放之前的内存
    freeMusicArray();
    
    if (size <= 0)
    {
        Serial.println("数组大小必须大于0");
        return false;
    }
    
    musicFiles = new (std::nothrow) String[size];
    if (musicFiles == nullptr)
    {
        Serial.println("内存分配失败");
        return false;
    }
    
    maxAllocatedFiles = size;
    fileCount = 0;
    Serial.printf("已分配内存，可存储 %d 首歌曲\n", size);
    return true;
}

// 释放音乐数组
void freeMusicArray()
{
    if (musicFiles != nullptr)
    {
        delete[] musicFiles;
        musicFiles = nullptr;
    }
    maxAllocatedFiles = 0;
    fileCount = 0;
}

// 添加音乐文件到数组
bool addMusicFile(String filePath)
{
    if (musicFiles == nullptr)
    {
        // 如果还没有分配数组，先分配一个初始大小
        if (!allocateMusicArray(50))
        {
            return false;
        }
    }
    
    // 检查是否需要扩容
    if (fileCount >= maxAllocatedFiles)
    {
        // 扩容：每次增加 50 个位置
        int newSize = maxAllocatedFiles + 50;
        String* newArray = new (std::nothrow) String[newSize];
        if (newArray == nullptr)
        {
            Serial.println("扩容失败：内存不足");
            return false;
        }
        
        // 复制旧数据到新数组
        for (int i = 0; i < fileCount; i++)
        {
            newArray[i] = musicFiles[i];
        }
        
        // 释放旧数组
        delete[] musicFiles;
        musicFiles = newArray;
        maxAllocatedFiles = newSize;
        Serial.printf("数组已扩容到 %d 首\n", newSize);
    }
    
    // 添加新文件
    musicFiles[fileCount] = filePath;
    fileCount++;
    return true;
}

// 保存播放状态到SD卡
bool savePlayState()
{
    AudioBusLock();
    File stateFile = SD.open(PLAYSTATE_FILE, FILE_WRITE);
    if (!stateFile)
    {
        AudioBusUnlock();
        Serial.println("无法打开播放状态文件进行写入");
        return false;
    }
    
    // 写入当前播放索引
    stateFile.println(music_i);
    
    stateFile.close();
    AudioBusUnlock();
    Serial.printf("播放状态已保存：当前曲目索引 = %d\n", music_i);
    return true;
}

// 从SD卡加载播放状态
bool loadPlayState()
{
    AudioBusLock();
    bool stateExist = SD.exists(PLAYSTATE_FILE);
    if (!stateExist)
    {
        AudioBusUnlock();
        Serial.println("播放状态文件不存在");
        return false;
    }
    
    File stateFile = SD.open(PLAYSTATE_FILE, FILE_READ);
    if (!stateFile)
    {
        AudioBusUnlock();
        Serial.println("无法打开播放状态文件进行读取");
        return false;
    }
    
    // 读取当前播放索引
    String line = stateFile.readStringUntil('\n');
    line.trim();
    stateFile.close();
    AudioBusUnlock();
    
    int savedIndex = line.toInt();
    
    // 验证索引合法性
    if (savedIndex >= 0 && savedIndex < fileCount)
    {
        music_i = savedIndex;
        Serial.printf("播放状态已加载：当前曲目索引 = %d\n", music_i);
        return true;
    }
    else
    {
        Serial.printf("播放状态索引无效：%d（总曲目数：%d）\n", savedIndex, fileCount);
        return false;
    }
}

// 恢复播放状态（UI初始化后调用）
void Music_RestorePlayState()
{
    // 尝试加载播放状态
    if (loadPlayState())
    {
        Serial.printf("恢复上次播放位置：第 %d 首\n", music_i + 1);
    }
    else
    {
        Serial.println("没有找到播放状态记录，从第一首开始");
        music_i = 0;  // 默认从第一首开始
    }

    // 播放歌曲
    if (fileCount > 0 && music_i >= 0 && music_i < fileCount)
    {
        /* 【关键】这里不再「先播再暂停」。

           原来的写法是 Music_PlayPath() 之后紧跟 pause_status = 1 + Music_Pause()，
           本意是「装载好但不自动播放」。但它和解码任务存在竞态：
           暂停请求有可能在 audioDoLoad() 还没跑完时就被处理掉，
           此时 s_decRunning 仍为 false，「暂停」会被静默丢弃。
           结果是最糟糕的一种状态：
               · 解码器实际上在播放
               · 而 pause_status = 1（UI 认为已暂停）
           之后所有操作都会对不上号：
               · 上一首/下一首：if (pause_status) 成立，只改标签并再发一次暂停，
                 **根本不会切歌**
               · 播放键：pause_status 清 0 后发出「播放」，但解码器本来就没在暂停，
                 于是什么都不发生 —— 表现为「点了没反应」

           现在的机制已经从根上解决了这个竞态：
           暂停/播放只是把「用户期望的状态」写进 volatile 变量 s_wantPaused，
           由 Music_Loop() 每一轮无条件落实 —— 装载期间写进去的意图会在
           装载完成后立刻生效，不存在被丢弃的窗口。
           所以这里可以放心地只装载、不暂停（保持「恢复上次播放位置」的原意：
           开机即播），pause_status 与解码器状态因此始终一致。 */
        Music_PlayPath(musicFiles[music_i].c_str());

        // 等待曲目信息（标题/时长）到达。
        // 上限 1.5 秒：这段是在 setup() 里同步等的，等太久就等于开机卡住；
        // 而且这只是「让界面先把时长填上」，等不到也不影响正常播放。
        uint32_t waitStart = millis();
        while (Music_GetDuration() == 0 && millis() - waitStart < 1500)
        {
            Music_info();
            delay(5);
        }
        Music_info();
        Serial.printf("[AUDIO] 时长获取%s: %ld 秒\n",
                      (Music_GetDuration() != 0) ? "成功" : "失败", Music_GetDuration());

        // 解析歌词
        parseLrcFile(musicFiles[music_i]);

        // 开机即播：显式把「期望的暂停状态」清成 false，
        // 不依赖任何别处的隐含假设（audioDoLoad 已不再清这个意图）。
        Music_WantPaused(false);
        pause_status = 0;

        Serial.printf("已加载：%s\n", musicFiles[music_i].c_str());
    }
}

/* ==================================================================
   播放列表重建：全卡递归扫描 + ID3 歌词导出（增量、带进度）

   只有**一个阶段**：递归走目录、把音乐文件登记进列表。
   这里刻意不做任何 ID3 解析 —— 歌词（以及 /lrc/*.lrc 的生成）全部推迟到
   真正播放某首歌的时候才做（见 parseLrcFile / exportLyricsToLrcFile）。
   这样重建只是一次纯 SD 目录枚举，几百首也就一两秒，
   而不是「为了几十首你可能永远不播的歌，把每首几百 KB 的标签都读一遍」。

   进度 = 已访问目录数 / (已访问 + 待访问)，受 budgetMs 约束，
   主循环可以在两次 Step 之间刷新进度条。
   ================================================================== */

#define SCAN_MAX_DIRS 512 // 待扫描目录队列上限，防止异常卡结构吃光内存

enum ScanPhase : uint8_t
{
  SCAN_IDLE = 0,
  SCAN_WALK, // 递归枚举目录
  SCAN_DONE
};

static ScanPhase s_scanPhase = SCAN_IDLE;
static bool s_scanSaveOk = false;
static bool s_scanTruncated = false;
static std::vector<String> *s_scanPending = nullptr;
static File s_scanDirHandle;
static bool s_scanDirOpen = false;
static String s_scanCurDir;
static int s_scanDirsDone = 0;
static int s_scanFound = 0;
static int s_scanPct = 0;
static String s_scanCurrent;

/* 进度只增不减：往队列里压新目录时分母会变大，
   如果不做单调保护，进度条会来回倒退，看起来像卡住了。 */
static void scanUpdatePct(int pct)
{
  if (pct < 0)
  {
    pct = 0;
  }
  if (pct > 99)
  {
    pct = 99;
  }
  if (pct > s_scanPct)
  {
    s_scanPct = pct;
  }
}

void Music_RescanBegin()
{
  // 0) 防御：万一上一次扫描是被异常路径打断的，这里先把目录句柄收干净
  if (s_scanDirOpen)
  {
    AudioBusLock();
    s_scanDirHandle.close();
    AudioBusUnlock();
    s_scanDirOpen = false;
  }

  /* 1) 先停掉播放，并等解码任务真正把音频对象释放掉。
        解码任务也在读同一张 SD 卡，不停掉的话两边会互相抢 SPI 总线，
        扫描会变得极慢，声音也会卡成一片。

        【为什么只等 600ms 就往下走】
        这个函数是在 UI 线程（core1）里被调用的，等太久就等于把界面卡住 ——
        而「UI 不能被后台阻塞」是硬要求。解码循环现在已经做了有界化，
        正常情况下 100ms 内就会响应 STOP；万一没等到也照样继续，
        扫描器自己对每次 SD 访问都加了 SPI 总线锁，最坏只是慢一点，不会出错。 */
  if (s_loadQueue != nullptr)
  {
    audioSendCmd(ACMD_STOP, nullptr);
  }
  uint32_t t0 = millis();
  while (s_decRunning && (millis() - t0) < 600)
  {
    delay(5);
  }
  if (s_decRunning)
  {
    Serial.println("[SCAN] 解码任务未及时释放，仍继续扫描（不阻塞界面）");
  }
  Music_WantPaused(false);
  s_decEnded = false;

  // 2) 清空当前列表，重新分配
  freeMusicArray();
  allocateMusicArray(50);

  // 3) 扫描状态复位，从根目录开始
  if (s_scanPending == nullptr)
  {
    s_scanPending = new (std::nothrow) std::vector<String>();
  }
  if (s_scanPending == nullptr)
  {
    Serial.println("[SCAN] 无法分配扫描队列，重建取消");
    s_scanPhase = SCAN_DONE;
    return;
  }
  s_scanPending->clear();
  s_scanPending->push_back((folder.length() > 0) ? folder : String("/"));

  s_scanDirOpen = false;
  s_scanCurDir = "";
  s_scanDirsDone = 0;
  s_scanFound = 0;
  s_scanPct = 0;
  s_scanTruncated = false;
  s_scanSaveOk = false;
  s_scanCurrent = (folder.length() > 0) ? folder : String("/");
  s_scanPhase = SCAN_WALK;

  Serial.println("[SCAN] 开始重建播放列表（只做目录枚举，歌词留到播放时再生成）...");
}

bool Music_RescanStep(uint32_t budgetMs)
{
  if (s_scanPhase == SCAN_IDLE || s_scanPhase == SCAN_DONE)
  {
    return true;
  }
  if (s_scanPending == nullptr)
  {
    s_scanPhase = SCAN_DONE;
    return true;
  }

  uint32_t start = millis();

  while ((millis() - start) < budgetMs)
  {
    if (s_scanPhase == SCAN_WALK)
    {
      if (!s_scanDirOpen)
      {
        if (s_scanPending->empty())
        {
          Serial.printf("[SCAN] 目录枚举完成：%d 个目录，%d 首歌曲\n",
                        s_scanDirsDone, s_scanFound);
          s_scanPhase = SCAN_DONE;
          return true;
        }

        s_scanCurDir = s_scanPending->back();
        s_scanPending->pop_back();

        AudioBusLock();
        s_scanDirHandle = SD.open(s_scanCurDir);
        AudioBusUnlock();

        s_scanDirsDone++;
        if (!s_scanDirHandle)
        {
          scanUpdatePct((int)((int64_t)s_scanDirsDone * 100 /
                              (s_scanDirsDone + (int)s_scanPending->size() + 1)));
          continue;
        }
        s_scanDirOpen = true;
        s_scanCurrent = s_scanCurDir;
        scanUpdatePct((int)((int64_t)s_scanDirsDone * 100 /
                            (s_scanDirsDone + (int)s_scanPending->size())));
        continue;
      }

      // 取一个目录项（openNextFile / isDirectory 都会真正读卡，必须持锁）
      AudioBusLock();
      File entry = s_scanDirHandle.openNextFile();
      String name;
      bool isDir = false;
      bool valid = (bool)entry;
      if (valid)
      {
        name = entry.name();
        isDir = entry.isDirectory();
      }
      AudioBusUnlock();

      if (!valid)
      {
        AudioBusLock();
        s_scanDirHandle.close();
        AudioBusUnlock();
        s_scanDirOpen = false;
        continue;
      }

      String full = entryFullPath(s_scanCurDir, name);

      if (isDir)
      {
        if ((int)s_scanPending->size() < SCAN_MAX_DIRS)
        {
          s_scanPending->push_back(full);
        }
        else
        {
          s_scanTruncated = true;
        }
      }
      else if (isMusicFile(name))
      {
        s_scanCurrent = full;
        if (addMusicFile(full))
        {
          s_scanFound++;
        }
      }

      AudioBusLock();
      entry.close();
      AudioBusUnlock();
      continue;
    }

    break;
  }

  return (s_scanPhase == SCAN_DONE);
}

/* 重建/放弃重建之后，原来那首「当前曲目」在列表里已经不存在了。
   必须把上一首的元数据清干净，否则界面会继续显示旧歌名、旧时长，
   而实际什么都没在播 —— 也就是经典的「UI 和实际状态对不上」。
   清掉之后，Music_info() 会用新列表里第一首的文件名兜底。 */
static void resetCurrentMediaInfo()
{
  currentTitle = "";
  currentArtist = "";
  currentAlbum = "";
  s_metaTitle[0] = 0;
  s_metaArtist[0] = 0;
  s_metaAlbum[0] = 0;
  s_metaDirty = false;
  s_id3LrcReady = false;
  s_id3Lrc[0] = 0;
  Music_SetDuration(0);
  durationPrinted = false;
  lrc_flag = 0;
  lyricCount = 0;
}

bool Music_RescanEnd()
{
  AudioFileSourceID3::clearLyrics();
  AudioFileSourceID3::releasePicture();
  if (s_scanPending != nullptr)
  {
    s_scanPending->clear();
  }
  if (s_scanDirOpen)
  {
    AudioBusLock();
    s_scanDirHandle.close();
    AudioBusUnlock();
    s_scanDirOpen = false;
  }

  s_scanSaveOk = false;
  if (fileCount > 0)
  {
    s_scanSaveOk = savePlaylist();
  }

  music_i = 0;
  // 让 UI 认为「换歌了」，下一次刷新会重新拉取曲目信息与歌词
  music_prev_i = -1;
  resetCurrentMediaInfo();
  s_scanPhase = SCAN_DONE;
  s_scanPct = 100;
  s_scanCurrent = "";

  Serial.printf("[SCAN] 重建完成：%d 首歌曲，列表保存%s\n",
                fileCount, s_scanSaveOk ? "成功" : "失败");
  /* 扫描跑在 core1 的主循环任务里，顺手报一下它的栈余量，
     方便判断要不要继续加大 ARDUINO_LOOP_STACK_SIZE。 */
  Serial.printf("[SCAN] 主循环任务剩余栈=%u 字节, 空闲堆=%u 字节\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL),
                (unsigned)ESP.getFreeHeap());
  if (s_scanTruncated)
  {
    Serial.printf("[SCAN] 警告：子目录数超过上限 %d，部分目录被跳过\n", SCAN_MAX_DIRS);
  }
  return s_scanSaveOk;
}

void Music_RescanAbort()
{
  if (s_scanDirOpen)
  {
    AudioBusLock();
    s_scanDirHandle.close();
    AudioBusUnlock();
    s_scanDirOpen = false;
  }
  if (s_scanPending != nullptr)
  {
    s_scanPending->clear();
  }
  AudioFileSourceID3::clearLyrics();
  AudioFileSourceID3::releasePicture();
  s_scanPhase = SCAN_DONE;
  s_scanPct = 0;
  s_scanCurrent = "";

  // 扫描期间从没写过 SD 上的列表文件，所以原来的 music_playlist.bin 还是完好的
  freeMusicArray();
  if (!loadPlaylist())
  {
    listMusicFiles(folder);
  }
  music_i = 0;
  music_prev_i = -1;
  resetCurrentMediaInfo();
  Serial.printf("[SCAN] 已放弃重建，恢复原有播放列表（%d 首）\n", fileCount);
}

int Music_RescanPercent() { return s_scanPct; }
const char *Music_RescanCurrentPath() { return s_scanCurrent.c_str(); }
int Music_RescanFoundCount() { return s_scanFound; }
bool Music_RescanSaveOk() { return s_scanSaveOk; }
