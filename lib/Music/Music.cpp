// Music.cpp
#include "Music.h"
#include "AudioFileSourceFS.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorM4A.h"  // MP4 容器 + AAC
#include "AudioGeneratorOGG.h"  // Ogg Vorbis（Tremor）
#include "AudioGeneratorOpus.h" // Ogg Opus（opusfile）
#include "AudioOutputI2S.h"
#include "AudioBusLock.h"
#include "LyricEncoding.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <Preferences.h>

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

// 音量（范围 0~21；无 NVS 记录时用这个默认值）
int volume = 15;

/* ==================================================================
   音量记忆（NVS）

   需求：调好的音量要能实时保存，断电重开后恢复。

   为什么用 NVS 而不是像播放状态那样写 SD 卡上的文件：
   音量是「随时可能被调」的小状态，而 SD 卡访问必须持 SPI 总线锁、
   和刷屏/解码抢总线，写卡还会卡住 UI；更要紧的是 SD 卡不可用时
   （未插卡/初始化失败）音量就该照样记得住。NVS 正好满足：
   不经过 SPI、与 SD 无关、掉电不丢。

   为什么在 setVolume() 里直接写而不是攒起来定时写：
   用户明确要求「实时保存」。Preferences 的写入只动一小块 NVS 页，
   单次耗时在毫秒级；setVolume() 只在**数值真的变化**时才写，
   滑块拖一遍最多写 21 次（0~21 每档一次），不会造成卡顿或写坏 Flash。
   ================================================================== */
static Preferences s_volPrefs;
static bool s_volPrefsReady = false;

static bool volPrefsEnsure()
{
  if (!s_volPrefsReady)
  {
    s_volPrefsReady = s_volPrefs.begin("player", false);
    if (!s_volPrefsReady)
    {
      Serial.println("[VOL] NVS 打开失败（namespace=player），音量将不会被记忆");
    }
  }
  return s_volPrefsReady;
}

void Music_LoadVolume()
{
  if (!volPrefsEnsure())
  {
    return;
  }
  uint8_t v = s_volPrefs.getUChar("vol", (uint8_t)volume);
  if (v > 21)
  {
    v = 21;
  }
  volume = v;
  Serial.printf("[VOL] 已从 NVS 恢复音量 = %d\n", volume);
}

void Music_SaveVolume()
{
  if (!volPrefsEnsure())
  {
    return;
  }
  s_volPrefs.putUChar("vol", (uint8_t)volume);
  Serial.printf("[VOL] 音量：%d\n", volume);
}

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
   【「切歌后没声音」的兜底判据】

   现象：偶尔切一首歌之后，串口显示「开始播放」、进度条也在走，
   但喇叭完全没声，要再切一次才恢复。

   成因：audioDoLoad() 里 s_generator->begin() 返回 true 只代表
   「解码器头解析成功、可以开始喂数据了」，**并不代表真的解出了音频帧**。
   某些文件（尤其是刚切歌时 SD 读取被 TFT 刷屏抢了总线、或文件头之后的
   第一帧数据异常）会让 begin() 成功、随后每一次 loop() 都直接返回 false。
   于是就到了 Music_Loop() 里 `if (!ok)` 那条分支：把解码器 stop 掉、
   s_decRunning=false、s_decEnded=true —— 也就是「这一首结束了」。
   UI 收到 ended 就按播放模式切下一首……但如果这一首是单曲循环，
   或者切到的还是同一个坏文件，就会陷入「反复重载却始终没声」。

   与其猜是哪种文件、哪次总线冲突，不如直接观测「有没有真的出过声」：
   解码器每次成功解出一帧就会往 I2S 写数据，我们用解码帧数是否增长来判断。
   装载后连续若干轮 loop() 一帧都没解出来，就判定这次装载是哑的，
   自动重载一次（只重试一次，避免坏文件导致无限循环）。
   ================================================================== */
static uint32_t s_loadSilentFrames = 0;    // 本次装载后「连续没解出帧」的轮数
static bool s_loadRetried = false;         // 本次装载是否已经自动重试过（防止死循环）
static uint32_t s_loadAudioMs = 0;         // 本次装载累计「已成功解码」的毫秒数（>0 即真的出过声）

/* 当前正在播放的路径，以及「哑火自动重载」用到的中转变量。
   路径只在解码任务里读写，不需要加锁。 */
static char s_currentPath[256] = {0};      // 最近一次成功装载的路径
static char s_reloadPath[256] = {0};       // 待重载的路径
static volatile bool s_needReload = false; // 是否需要在下一轮重新装载

/* ==================================================================
   拖动进度条 → 跳转播放位置（seek）

   【为什么 seek 要「重开解码器」而不是调某个 seek() 接口】

   ESPAudio 的解码器**没有**对外暴露跳转能力：
     · AudioGenerator 基类里根本没有 seek() 虚函数，只有 begin/loop/stop；
     · 各解码器内部确实有 file->seek()，但那都是它自己为了回退读取位置
       用的（如 MP3 读 Xing 头后回退、FLAC 的 seek_cb 回调），
       不对调用方开放，也没法用来做「跳到第 N 秒」。
   要在帧级跳转就得改 vendored 的三个解码器（还要处理 mad_synth 内部
   状态、Xing 头已消费、FLAC 的 decoder state 等），改动面大、回归风险高。

   这里采用的做法：**按字节位置重新装载**。
     目标字节 = 文件大小 × (目标秒数 / 总秒数)
   然后把文件指针挪到那里，再重建解码器从该处继续解。
   对 CBR（固定码率）MP3、WAV、FLAC 误差很小（<1 秒）；
   VBR（可变码率）MP3 因为是按平均码率估算，可能有几秒偏差 ——
   这是这个方案的已知代价，换来的是零解码器改动。

   【为什么不用命令队列】
   和装载/停止一样属于「重活」，必须由解码任务来做（它会 new/delete
   整个解码链），所以走 s_seekReqMs 这个 volatile 意图 + 主循环落实，
   理由见上面「UI → 解码任务的命令通道」的说明。
   ================================================================== */
/* ================= 播放进度的时间基准（跨核安全） =================

   【为什么这几行必须放在临界区里读】
   进度 = (now - s_playStartMs - s_pausedMs) + s_seekBaseMs
   其中 s_playStartMs / s_pausedMs / s_seekBaseMs 全由解码任务(core0)写、
   由 UI(core1)读。它们是**一组**必须一致的数据：

   跳转时解码任务会先把 s_seekBaseMs 改成目标位置（比如 478296 ms），
   紧接着才把 s_playStartMs 置成 millis()。如果 UI 正好在这个空隙里读，
   就会拿到「新的 seekBase + 旧的 playStart」，算出来是
       (now - 旧playStart) + 478296
   当旧 playStart 比 now 还大（刚换过歌/刚跳转过），前半段按 uint32
   下溢成一个极大的值，最终显示成 **71574:49** 这种荒唐时长
   （4294489 秒 ≈ 2^32 ms / 1000，正是无符号下溢的特征）。

   所以读取时必须整组原子化。portMUX 是递归自旋锁，
   保护区间极短（只做几次减法），不影响实时性。 */
static portMUX_TYPE s_timeMux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_needSeek = false;   // 是否有待处理的跳转请求
static volatile uint32_t s_seekReqMs = 0;  // 目标位置（毫秒）
static uint32_t s_seekBaseMs = 0;          // 跳转起点在整首歌里的偏移（毫秒）

/* 原地跳转（FLAC）后的「一次性宽限」标志。
   见 Music_Loop() 里 !ok 分支的说明：
   跳转后如果很快就播完（目标靠近文件尾），不能被误判成
   「装载哑火」而自动重载回开头。 */
static bool s_seekGraceOnce = false;

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
static char s_metaComposer[256] = {0}; // 作曲家（TCOM / COMPOSER）
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
  else if (tag.equalsIgnoreCase("TCOM") || tag.equalsIgnoreCase("TCM") ||
           tag.equalsIgnoreCase("composer"))
  {
    /* 作曲家。ID3v2 里是 TCOM，v2.2 是 TCM；FLAC 的 Vorbis comment 用
       COMPOSER。以前没接这个标签，「歌曲信息」里就一直是空的。 */
    strncpy(s_metaComposer, string, sizeof(s_metaComposer) - 1);
    s_metaComposer[sizeof(s_metaComposer) - 1] = 0;
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
  /* 时间基准整组清零（原子，见 s_timeMux）。
     ⚠ s_pauseStartMs 也必须清：它若留着上一次的旧值，
       applyPauseIntent() 恢复播放时那句 `now - s_pauseStartMs`
       会算出一个巨大的差值并累加进 s_pausedMs，
       进而让进度计算下溢成 71574:49。 */
  portENTER_CRITICAL(&s_timeMux);
  s_playStartMs = 0;
  s_pausedMs = 0;
  s_pauseStartMs = 0;
  /* 一起清掉跳转基准与待处理的跳转请求。
     不清基准的话：停在中间→停止→再放另一首，进度条会带着上一首的
     偏移量继续数（比如新歌刚开始就显示 1:30）。
     不清请求的话：停止后那个还没被解码任务处理的跳转会被执行，
     而 s_currentPath 可能已经变了，就会跳到错误的曲目上。 */
  s_seekBaseMs = 0;
  portEXIT_CRITICAL(&s_timeMux);
  s_needSeek = false;
  /* 宽限标志是「针对某一次跳转」的，换歌/停止后必须清掉，
     否则下一首如果恰好一帧没解出来，会被这个残留标志当成正常结束。 */
  s_seekGraceOnce = false;
}

/* 装载并开始播放指定曲目（只能由解码任务调用）。

   startByte > 0 时表示「从文件该字节位置开始解码」—— 这是拖动进度条
   跳转的实现方式（见上面 seek 的说明）。此时还会配合 s_seekBaseMs，
   把时间基准挪到跳转点，否则 UI 上的进度会从 0 重新开始数。

   keepPicture = true 表示「这是同一首歌的跳转，不要丢掉已解析的封面」。
   ⚠ 见下面 releasePicture() 处的说明：跳转时清封面会让封面在界面上消失。 */
static void audioDoLoadFrom(const char *path, uint32_t startByte, uint32_t seekBaseMs,
                            bool keepPicture)
{
  releaseAudio();
  /* 【切歌才释放封面；跳转时保留】
     原来这里无条件调用 releasePicture()，而 releasePicture() 会把
     AlbumArtStore 里的封面数据清掉，于是 UI 那边
     Music_GetAlbumCover() 立刻返回 false → 走「显示默认封面」分支
     → **封面在拖动进度条的一瞬间就没了**。

     为什么不靠「重载时重新解析封面」来自愈：
       MP3/AAC 的封面在 ID3v2 标签里（文件最开头），WAV 在 LIST 块里。
       跳转是把文件指针挪到**中间**再重建解码器，那些头根本不会被再读到，
       所以重载后封面回不来 —— 这就是「部分情况下封面丢失」的成因
       （FLAC 走原地 seek 不重载，所以不受影响，表现成"部分情况"）。

     保留即可：同一首歌的封面本来就是同一张，跳转不改变它。
     真正切歌时（keepPicture == false）照旧释放，让新封面有机会解析。 */
  if (!keepPicture)
  {
    AudioFileSourceID3::releasePicture();
  }

  s_decPaused = false;
  s_decEnded = false;
  /* 【不要在这里清 s_wantPaused】
     用户的「暂停」意图必须跨换曲保留，否则「暂停中点下一曲」会突然出声。
     开机时的默认值是 false（即播放），由 Music_RestorePlayState() 显式保证。 */
  /* 时间基准整组原子改写（见 s_timeMux）：
     这几项一起决定 UI 算出来的播放进度，必须让 UI 要么全看到旧值、
     要么全看到新值，不能看到中间态。 */
  portENTER_CRITICAL(&s_timeMux);
  s_pausedMs = 0;
  s_pauseStartMs = 0;
  s_playStartMs = millis();
  /* 时间基准：正常从头播是 0；跳转后是目标位置。
     Music_GetCurrentPlayTime() 会在这个基准上累加实际播放时长，
     所以跳转后进度条会从目标位置继续走，而不是从 0 重新开始。 */
  s_seekBaseMs = seekBaseMs;
  portEXIT_CRITICAL(&s_timeMux);
  /* ⚠ 时长只在「换歌」时清零，跳转时**必须保留**。

     原因：时长来自文件头部的元数据块 ——
       · FLAC 的 STREAMINFO（total_samples）在文件最开头；
       · MP3 的 Xing/Info 头也在第一帧。
     跳转是把文件指针挪到中间再重建解码器，那些头根本不会被再读一次，
     所以解码器不会再发 tlen 回调。如果这里照样清零，
     Music_GetDuration() 就永远是 0，后果是一连串的连锁失效：
       进度条算不出百分比、pbPosToSeconds() 返回失败、
       再拖也跳不动 —— 表现就是「跳转一次之后进度条就废了」。
     保留旧值即可：同一首歌的时长本来就没变。 */
  const bool isSeek = (seekBaseMs > 0) || (startByte > 0);
  if (!isSeek)
  {
    Music_SetDuration(0); // 换歌先清零，等 Xing/ID3 回调送来新时长
  }
  durationPrinted = (Music_GetDuration() != 0);

  /* 【跳转时不要清标签与内嵌歌词】
     和上面「时长要保留」是同一个道理：标题/艺术家/专辑/作曲家/歌词
     全都来自文件头部的标签块（ID3v2 / Vorbis comment / WAV LIST），
     而跳转后解码器是从文件中段开始读的，那些块**再也不会被读到**。
     如果这里清掉：
       · 界面上歌手/专辑/歌词立刻变空（或退化成文件名）；
       · 内嵌歌词一旦清掉就再也补不回来（parseLrcFile 的第三级回退失效）。
     所以只有真正换歌（!isSeek）才清。 */
  if (!isSeek)
  {
    s_metaTitle[0] = 0;
    s_metaArtist[0] = 0;
    s_metaAlbum[0] = 0;
    s_metaComposer[0] = 0;
    // 清掉上一首的内嵌歌词，等新曲目的 USLT 回调重新填充
    s_id3LrcReady = false;
    s_id3Lrc[0] = 0;
  }

  String fullPath = normalizePath(path);

  /* 【重置「本首是否出过声」的判据】
     见 s_loadAudioMs / s_loadRetried 的说明。

     ⚠ s_loadRetried 不能在这里无条件清零：
       「哑火自动重载」走的也是 audioDoLoad()，如果每次进来都把它清掉，
       一个真正解不出声音的文件就会被无限重载（重载→哑火→再重载…），
       永远出不来，界面还会一直卡在「正在加载」。
       所以只有「换了一首不同的歌」才允许重新获得一次重试机会：
       路径没变（说明就是这次重载本身）时保留原来的重试标记。 */
  const bool sameTrackAsBefore = (s_currentPath[0] != 0) &&
                                 (strcmp(s_currentPath, fullPath.c_str()) == 0);
  s_loadSilentFrames = 0;
  s_loadAudioMs = 0;
  if (!sameTrackAsBefore)
  {
    s_loadRetried = false;
  }

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

  /* 跳转：把文件指针挪到目标字节位置。
     放在预读校验之后 —— 先确认这个文件真的读得出来，再去算偏移，
     否则「读不出数据」会被误报成「跳转失败」。
     ⚠ 必须夹到文件长度以内：目标时间若因四舍五入超过总长
       （比如拖到最右端），越界会导致解码器立刻 EOF，表现成「一拖就停」。 */
  if (startByte > 0)
  {
    const uint32_t fileSize = (uint32_t)s_source->getSize();
    uint32_t target = startByte;
    if (fileSize > 0 && target >= fileSize)
    {
      // 留一点余量，避免刚好落在文件末尾
      target = (fileSize > 4096) ? (fileSize - 4096) : 0;
    }
    s_source->seek(target, SEEK_SET);
    Serial.printf("[AUDIO] 跳转到字节 %u (共 %u)，时间基准 %u ms\n",
                  (unsigned)target, (unsigned)fileSize, (unsigned)seekBaseMs);
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
     注意两条规则：
       1) 只有「标签就是 ID3v2」的格式才套 AudioFileSourceID3（MP3 / AAC）。
          FLAC / WAV / M4A / OGG / OPUS 的元数据在各自的容器里
          （Vorbis comment / LIST-INFO / MP4 原子 / OpusTags），
          由解码器自己解析后经 GENINFO 回调交给我们；
          给它们套 ID3 反而会因为「流偏移 ≠ 文件偏移」把 seek/tell 搞乱。
       2) 其余情况一律按 MP3 处理。 */
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
  else if (lower.endsWith(".m4a"))
  {
    decName = "M4A";
    s_generator = new AudioGeneratorM4A(); // MP4 容器 + AAC（Helix），自带 tlen
  }
  else if (lower.endsWith(".ogg"))
  {
    decName = "OGG";
    s_generator = new AudioGeneratorOGG(); // Ogg Vorbis（Tremor 整数解码）
  }
  else if (lower.endsWith(".opus"))
  {
    decName = "OPUS";
    s_generator = new AudioGeneratorOpus(); // Ogg 封装 Opus（opusfile）
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
  /* 记下当前路径，供「哑火自动重载」使用。
     必须放在 begin() 成功之后：只有真的装载起来了才值得重载。 */
  strncpy(s_currentPath, fullPath.c_str(), sizeof(s_currentPath) - 1);
  s_currentPath[sizeof(s_currentPath) - 1] = 0;
  s_loadAudioMs = 0; // 从零开始累计这一首解出的音频时长
  Serial.printf("[AUDIO] 开始播放: %s\n", fullPath.c_str());
}

/* 从曲目开头装载（普通换歌走这里）。 */
static void audioDoLoad(const char *path)
{
  /* keepPicture = false：这是真正的换歌，
     要释放上一首的封面，好让新曲目的封面能被解析出来。 */
  audioDoLoadFrom(path, 0, 0, false);
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

  /* 恢复上次的音量。刻意放在 SD 初始化**之前**：
     音量记忆不依赖存储卡，卡没插好也应该照样生效。
     这里只改 volume 变量，真正的 SetGain 会在曲目装载（audioDoLoad）
     创建 I2S 输出对象时按 volume 应用。 */
  Music_LoadVolume();

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

/* 清空上一首的曲目信息（歌词/标题/时长等）。定义在本文件后面，
   这里提前声明，供热插拔清理提前调用。 */
static void resetCurrentMediaInfo();

/* 投递「重活」命令给解码任务（audioSendCmd）。定义在本文件后面，
   这里提前声明，供热插拔清理（拔卡时停播）提前调用。 */
static void audioSendCmd(uint8_t type, const char *path);

/* ================= SD 卡热插拔：运行时探测 / 重连 / 清理 =================

   【探测为什么用「打开根目录」而不是 SD.cardType()】
   cardType() 只是返回初始化时缓存的 type，不会重新访问卡，卡拔了它照样
   返回 CARD_SDHC —— 用它做运行时探测会误判。而 SD.open("/") 会真正走
   FATFS 的 f_opendir，落到 SD 的 SPI 读上：卡在槽里就成功、卡拔了就失败，
   是可靠的运行时判据。它和普通读文件一样必须持 SPI 总线锁（与 TFT 刷屏互斥）。 */
bool Music_ProbeStorage()
{
  if (!s_sdReady)
  {
    return false; // 还没 mount，谈不上「可读」
  }

  AudioBusLock();
  File root = SD.open("/", FILE_READ);
  bool ok = (bool)root; // 目录打开成功即代表介质可读（空槽会快速失败）
  if (root)
  {
    root.close();
  }
  AudioBusUnlock();
  return ok;
}

/* 重新初始化 SD 卡（SD.end() + SD.begin()）。
   ⚠ 用于「开机时没插卡 → 之后才插入」这条路径：SDFS::begin() 内部有
   `if (_pdrv != 0xFF) return true;` 的守卫，不先 end() 根本不会重新 mount。
   整个 end()/begin() 都在 SPI 总线锁里 —— begin() 里 ff_sd_initialize 走的是
   SPI beginTransaction（IDF 驱动），与直写寄存器的 TFT 刷屏没有仲裁，必须互斥。 */
bool Music_ReinitStorage()
{
  AudioBusLock();
  SD.end();
  delay(20); // 释放旧挂载后稍等，让卡上电稳定
  bool ok = SD.begin(SD_Pin, SPI, 10000000);
  AudioBusUnlock();

  s_sdReady = ok;
  if (ok)
  {
    Serial.println("[SD] 存储卡已重新初始化");
  }
  else
  {
    Serial.println("[SD] 重新初始化失败：卡仍未插入或不可读");
  }
  return ok;
}

/* 卡已被拔出：立即停掉播放，清空播放列表与曲目信息，并标记 SD 不可用。
   调用方（core1 主循环的 SdHotplug）在这之后应 UI_NotifyScreenRebuilt()，
   让界面把旧歌名/进度清掉。

   【为什么先停播放再清数组】
   musicFiles 数组是 UI 线程独有的，但解码任务(core0)持有的是命令消息里
   **拷贝的一份路径**，不直接引用该数组 —— 所以清数组本身是安全的。
   真正的风险是：不先停播放的话，解码任务还在对「已拔出的卡」做 open/read，
   会和下面的重新 init 抢同一张卡同一条总线。先发 STOP 并等它释放，
   保证清理彻底、界面状态干净。 */
void Music_HandleCardRemoved()
{
  if (s_loadQueue != nullptr)
  {
    audioSendCmd(ACMD_STOP, nullptr);
  }
  uint32_t t0 = millis();
  while (s_decRunning && (millis() - t0) < 500)
  {
    delay(5);
  }
  if (s_decRunning)
  {
    Serial.println("[SD] 解码任务未及时释放，仍继续清理（不阻塞主循环）");
  }

  /* 清空列表：fileCount 归 0、musicFiles 置 null。UI 那边的取歌/自动切歌
     都以 fileCount > 0 为前提，清成空之后就不会去碰已释放的指针。 */
  freeMusicArray();
  music_i = 0;
  music_prev_i = -1;
  resetCurrentMediaInfo();

  s_sdReady = false;
  Serial.println("[SD] 检测到卡被拔出：已停止播放并清空播放列表");
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
    /* 原子写：UI 可能在另一核上读这一组值算进度 */
    portENTER_CRITICAL(&s_timeMux);
    s_pauseStartMs = millis();
    portEXIT_CRITICAL(&s_timeMux);
  }
  else if (!s_wantPaused && s_decPaused)
  {
    /* 【恢复播放：累计暂停时长】
       ⚠ 这里必须防两件事，它们是 s_pausedMs 变成天文数字的根源：
         1) s_pauseStartMs == 0（从没暂停过 / 被 stop() 清过）
            → millis() - 0 是一个巨大的值，直接累加进 s_pausedMs；
         2) millis() 回绕或状态错乱导致 now < s_pauseStartMs
            → uint32 相减下溢。
       两者都会让 s_pausedMs 变成一个接近 2^32 的数，
       而 Music_GetCurrentPlayTime() 里 `elapsed = (now - start) - paused`
       立刻下溢，最终显示成 71574:49（4294489 秒 ≈ 2^32 ms）。
       所以这里用有符号差值，并且只接受合理的正值。 */
    const uint32_t nowMs = millis();
    const uint32_t pauseStart = s_pauseStartMs;
    int64_t delta = (pauseStart != 0) ? (int64_t)(int32_t)(nowMs - pauseStart) : 0;
    if (delta > 0)
    {
      portENTER_CRITICAL(&s_timeMux);
      s_pausedMs += (uint32_t)delta;
      portEXIT_CRITICAL(&s_timeMux);
    }
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

  /* 1.5 「哑火自动重载」：上一轮解码发现这首一帧都没解出来，
      在这里重新装载一次。放在 drainCommands() 之后，
      这样用户在此期间手动切了歌的话，新命令会覆盖掉这次重载意图。 */
  if (!heavyCmd && s_needReload)
  {
    s_needReload = false;
    if (s_reloadPath[0] != 0)
    {
      Serial.printf("[AUDIO] 自动重载: %s\n", s_reloadPath);
      /* keepPicture = true：这是**同一首歌**的哑火重载（重新 open 同一路径），
         不是换歌。如果在这里丢掉封面，用户会在「切歌偶尔没声音」被自动
         修复的同时发现封面也没了 —— 而且由于重载同样是读文件开头，
         封面其实能重新解析出来，但那一瞬间的闪烁完全没必要。
         直接保留更稳。 */
      audioDoLoadFrom(s_reloadPath, 0, 0, true);
      vTaskDelay(1);
      return;
    }
  }

  /* 1.6 拖动进度条跳转。

     分两条路：
       (a) FLAC：用 libFLAC 的 seek_absolute() **原地精确跳转**。
           不解码器、不重开文件、不动时长 —— 又快又准（精确到采样点）。
       (b) 其它格式（MP3/WAV/AAC）：解码器没有 seek 能力，
           退回「按字节位置重新装载」，这是估算，可能有偏差。

     为什么不再一律重开解码器（上一版的错误做法）：
       重开会走 audioDoLoadFrom()，而那里会把时长清零等元数据回调。
       但跳转是把文件指针挪到**中间**，文件头部的 Xing/STREAMINFO 再也不会
       被读到，于是时长永远是 0 → 进度条算不出百分比、也再跳不动，
       表现就是「拖过一次之后进度条彻底卡死」。
       另外对 FLAC 来说，硬跳进帧中间会找不到同步码，
       解码器直接报错退出，又被「哑火重载」逻辑拉回开头 ——
       这就是「松手后从头开始放」的第二个原因。
       现在 FLAC 走原地 seek，这两个问题都不存在了。 */
  if (!heavyCmd && s_needSeek)
  {
    s_needSeek = false;
    if (s_currentPath[0] != 0)
    {
      const uint32_t targetMs = s_seekReqMs;
      const uint32_t durMs = (uint32_t)Music_GetDuration() * 1000u;

      /* ---- (a) FLAC：精确 seek，不重开解码器 ---- */
      String lower = s_currentPath;
      lower.toLowerCase();
      if (lower.endsWith(".flac") && durMs > 0 && s_decRunning)
      {
        AudioGeneratorFLAC *flacGen = (AudioGeneratorFLAC *)s_generator;
        /* 把时间换算成「第几个采样点」：样本数 = 秒 × 采样率。
           采样率从输出对象取（解码器 begin 时写在上面）。 */
        const uint32_t rate = (s_output != nullptr) ? s_output->GetRate() : 0;
        if (rate > 0)
        {
          const uint64_t sample = ((uint64_t)targetMs * (uint64_t)rate) / 1000ULL;
          if (flacGen->seekToSample(sample))
          {
            /* 只挪时间基准，**不动**时长：
               时长是整首歌的属性，跳转不改变它。
               ⚠ 这三项必须整组原子改写：UI 线程会同时读它们算进度，
                 分步写会读到「新 seekBase + 旧 playStart」的撕裂组合，
                 算出 71574:49 那种下溢值（见 s_timeMux 的说明）。 */
            portENTER_CRITICAL(&s_timeMux);
            s_seekBaseMs = targetMs;
            s_playStartMs = millis();
            s_pausedMs = 0;
            s_pauseStartMs = 0;
            portEXIT_CRITICAL(&s_timeMux);
            s_decEnded = false;
            /* 【重置「哑火」计时，但不要动 s_loadRetried】
               s_loadAudioMs 记的是「本次装载以来累计解出多少音频」。
               跳转后要从 0 重新累计（新位置是全新的解码过程）。
               万一跳过去真的解不出声，有下面这层保护兜住：
               —— 见 s_seekGraceOnce。 */
            s_loadAudioMs = 0;
            /* 跳转后第一次「播放结束」直接当正常结束，不做哑火重载：
               目标若靠近文件尾，可能几十毫秒就播完，那时 s_loadAudioMs
               还没到 50ms，会被误判成哑火而重载回开头。 */
            s_seekGraceOnce = true;
            Serial.printf("[AUDIO] FLAC 精确跳转: %u ms -> 采样点 %llu (采样率 %u)\n",
                          (unsigned)targetMs, (unsigned long long)sample, (unsigned)rate);
            vTaskDelay(1);
            return;
          }
          Serial.println("[AUDIO] FLAC seek_absolute 失败，退回字节重载");
        }
      }

      /* ---- (b0) 容器格式：不支持按字节跳转，明确忽略 ----
         OGG / OPUS / M4A 的 begin() 必须从**文件头**开始解析
         （Ogg 的页结构、MP4 的 moov 原子都在开头）。而下面 (b) 会把文件
         指针挪到文件中间再重开解码器 —— 对这些格式结果是 begin() 直接失败、
         整首歌彻底没声。与其静默把播放搞坏，不如明确忽略这次拖动
         （进度条会弹回原位，用户能看出「这个格式不能拖」）。 */
      if (lower.endsWith(".ogg") || lower.endsWith(".opus") || lower.endsWith(".m4a"))
      {
        Serial.println("[AUDIO] 该格式是容器结构，不支持拖动跳转，已忽略本次 seek");
        vTaskDelay(1);
        return;
      }

      /* ---- (b) 其它格式：按字节位置重新装载 ---- */
      uint32_t fileSize = 0;
      {
        /* 取文件大小要读 SD，必须持 SPI 总线锁（与 TFT 刷屏互斥）。
           单独一个作用域，避免下面的 audioDoLoadFrom 还持着锁。 */
        AudioBusLock();
        File f = SD.open(s_currentPath, FILE_READ);
        if (f)
        {
          fileSize = (uint32_t)f.size();
          f.close();
        }
        AudioBusUnlock();
      }

      if (fileSize > 0 && durMs > 0)
      {
        /* 目标字节 = 文件大小 × (目标时间 / 总时长)。
           这就是「按平均码率估算位置」，CBR 文件很准，
           VBR 文件会有几秒偏差（已知代价，见前面的说明）。 */
        const uint64_t byteOff = ((uint64_t)fileSize * (uint64_t)targetMs) / (uint64_t)durMs;
        Serial.printf("[AUDIO] 跳转请求: %u ms / %u ms -> 字节 %u (共 %u)\n",
                      (unsigned)targetMs, (unsigned)durMs,
                      (unsigned)byteOff, (unsigned)fileSize);
        /* keepPicture = true：同一首歌跳转，保留已解析的封面。
           否则封面会立刻消失，而且重载读到的是文件中段、
           读不到文件头的 ID3/LIST 标签，再也恢复不了。 */
        audioDoLoadFrom(s_currentPath, (uint32_t)byteOff, targetMs, true);
        vTaskDelay(1);
        return;
      }
      else
      {
        // 时长还没解析出来（刚切歌就拖），没法换算，忽略这次跳转
        Serial.println("[AUDIO] 跳转失败：时长或文件大小未知");
      }
    }
  }

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
      // 【调试用】每秒打印一次解码心跳和剩余栈空间，方便判断解码任务是否卡死
      // Serial.printf("[AUDIO] 解码心跳: loop=%u 次/秒, 剩余栈=%u 字节\n",
      //               (unsigned)s_loopCount,
      //               (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
      /* 【区分「放完了」和「根本没出声」】

         !ok 有两种完全不同的含义，原来被当成同一种处理，
         这正是「切歌偶尔没声音」的根源：

         (a) 正常播完 —— 之前已经成功解出过音频帧，只是这次读到了文件尾。
             → 照旧：停解码器、置 ended，交给 UI 切下一首。

         (b) 一次都没解出来 —— 装载成功了，但从头到尾一帧都没有。
             这时绝不能当成「放完了」：UI 会以为这首歌正常结束而跳到下一首，
             用户看到的就是「切歌后没声音、然后又自己跳走了」。
             → 自动重载一次。重载会重新 open 文件、重新 begin()，
               能解决绝大多数「首帧读取被总线冲突打断」的情况。

         ⚠ 判据必须是「真的解出过帧」而不是「loop 调过几次」：
           一首正常的歌至少要出声几百毫秒，我们按「累计出声时长」来判断，
           比数循环次数可靠得多（循环次数受文件长度影响，几秒的歌也可能很少轮）。 */
      const bool everProducedAudio = (s_loadAudioMs >= 50);

      /* 【跳转后第一次结束不算「哑火」】
         如果跳到的位置靠近文件尾，可能几十毫秒就播完了。
         这时 s_loadAudioMs 很小、everProducedAudio 为 false，
         会被误判成「一帧都没解出来」→ 自动重载 → **跳回开头**，
         正是「拖完进度后从头发开始放」的成因之一。
         因为原地 seek 并没有重开解码器，这里用一个一次性宽限标志
         把跳转后的首次结束直接当作正常结束。 */
      if (s_seekGraceOnce)
      {
        s_seekGraceOnce = false;
        s_decEnded = true;
        Serial.println("[AUDIO] 跳转后播放结束（正常）");
        return;
      }

      if (!everProducedAudio && !s_loadRetried)
      {
        s_loadRetried = true; // 只重试一次，坏文件不会造成无限重载
        Serial.println("[AUDIO] 警告：本次装载没有解出任何音频，疑似哑火，自动重载一次");
        /* 记下要重播的路径：这里不能直接调用 Music_PlayPath()，
           因为那是往命令队列里写，而本函数正是在处理命令的调用栈里，
           会给刚刚清空的槽位塞回一条命令，语义混乱。
           改为在下一轮 Music_Loop() 里重新装载，见下面的 s_needReload。 */
        strncpy(s_reloadPath, s_currentPath, sizeof(s_reloadPath) - 1);
        s_reloadPath[sizeof(s_reloadPath) - 1] = 0;
        s_needReload = true;
        s_generator->stop();
        s_decRunning = false;
        s_decEnded = false; // 别让 UI 误判成「播放结束」而跳歌
        return;
      }

      if (!everProducedAudio)
      {
        Serial.println("[AUDIO] 警告：重载后仍然解不出音频，判定为无法解码的文件");
      }

      s_generator->stop();
      s_decRunning = false;
      s_decEnded = true;
      Serial.println("[AUDIO] 曲目播放结束");
    }
    else
    {
      /* 成功解码了一轮：累计「已经出过声」的时长，供上面区分 (a)/(b)。
         用累计耗时而不是循环次数：它直接反映「这一首到底响过没有」，
         不受歌曲长短、单轮解码量的影响。 */
      s_loadAudioMs += (cost > 0) ? cost : 1;
      s_loadSilentFrames = 0;
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

/* 阻塞式全卡递归扫描（目前没有调用者，保留作为「一次性、可阻塞」的参考实现）。
   注意：所有 SD 访问都包在 SPI 总线锁里 —— TFT 刷屏和它共用同一条总线，
   运行期（LVGL 已经在刷屏）不加锁会直接把 SPI 事务打乱。
   想要带进度的非阻塞版本请用 Music_RescanBegin/Step/End。
   内部改用 Music_OpenDir/ReadDir/CloseDir（它们自带总线锁，不能在外面再套一层，
   因为 AudioBusLock 用的是不可重入的普通互斥量）。 */
void scanAndAddMusicFiles(String dir)
{
  void *d = Music_OpenDir(dir.c_str());
  if (d == nullptr)
  {
    return;
  }

  String name;
  bool isDir = false;
  while (Music_ReadDir(d, name, isDir))
  {
    if (isDir)
    {
      scanAndAddMusicFiles(entryFullPath(dir, name)); // 递归处理子目录
    }
    else if (isMusicFile(name))
    {
      addMusicFile(entryFullPath(dir, name));
    }
  }

  Music_CloseDir(d);
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
       .mp3  -> AudioGeneratorMP3
       .flac -> AudioGeneratorFLAC
       .wav  -> AudioGeneratorWAV
       .aac  -> AudioGeneratorAAC
       .m4a  -> AudioGeneratorM4A   （MP4 容器 + AAC）
       .ogg  -> AudioGeneratorOGG   （Ogg Vorbis）
       .opus -> AudioGeneratorOpus  （Ogg Opus）

   旧实现收 .wma 却根本没有 WMA 解码分支，选中后必然打不开 ——
   表现就是「点了没反应」，正是这个项目一直在踩的那类坑，所以不再收它。
   同理：往这里加扩展名时，audioDoLoad() 里必须同时加上对应分支。

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
         base.endsWith(".wav") || base.endsWith(".aac") ||
         base.endsWith(".m4a") || base.endsWith(".ogg") ||
         base.endsWith(".opus");
}

/* ================= 供「文件浏览页」使用的路径/目录工具 =================

   这些放在 lib/Music 里而不是 UI 里，是因为它们都要碰 SD 卡，
   而 SD 访问必须走 SPI 总线锁（和 TFT 刷屏互斥）。把这件事关在一个地方，
   UI 侧就只需要关心控件，不用再去记「读卡要加锁」。 */

// 取父目录：".../a/b/c.mp3" -> ".../a/b"；根目录再往上仍然是 "/"
String Music_ParentDir(const String &path)
{
  int slash = path.lastIndexOf('/');
  if (slash <= 0)
  {
    return String("/"); // 已经是根，或者没有目录部分
  }
  return path.substring(0, slash);
}

// 取文件名（含扩展名）：".../a/b/c.mp3" -> "c.mp3"
String Music_BaseName(const String &path)
{
  int slash = path.lastIndexOf('/');
  return (slash >= 0) ? path.substring(slash + 1) : path;
}

/* 在播放列表里按路径查下标。

   ⚠ 这里**不能**对每个条目都调一次 normalizePath()：那会为 1000 首歌做
     2000 次 String 分配（一次拷贝 + 一次 trim），而内部堆本来就只有几 KB，
     点一下歌就可能因为这一下抖动把后面的 fopen 挤崩（见 lv_conf.h 里的说明）。
   播放列表里的路径本来就是 entryFullPath()/normalizePath() 规范化后写进去、
   再原样读回来的，所以直接逐条 strcmp 就是对的，全程只分配 want 这一个 String。
   另外顺手兼容一下「库里存的是不带前导 '/' 的写法」。 */
int Music_IndexOfPath(const char *path)
{
  if (path == nullptr || path[0] == 0)
  {
    return -1;
  }
  String want = normalizePath(String(path));
  const char *wantC = want.c_str();
  const char *wantNoLead = (wantC[0] == '/') ? (wantC + 1) : wantC;
  const bool checkNoLead = (wantNoLead != wantC) && (wantNoLead[0] != 0);

  for (int i = 0; i < fileCount; i++)
  {
    const char *have = musicFiles[i].c_str();
    if (strcmp(have, wantC) == 0)
    {
      return i;
    }
    if (checkNoLead && strcmp(have, wantNoLead) == 0)
    {
      return i;
    }
  }
  return -1;
}

/* ---------- 高效目录枚举 ----------
   背景说明见 Music.h：绕开 File::openNextFile() 是为了避开它对每个条目
   做一次按路径 stat() 带来的 O(n²)。 */

// 把 "/洛雪下载" 这样的相对路径拼成 VFS 绝对路径（SD 默认挂在 "/sd"）
static String musicVfsPath(const char *dir)
{
  const char *mp = SD.mountpoint();
  String base = (mp != nullptr && mp[0] != 0) ? String(mp) : String("/sd");
  if (dir == nullptr || dir[0] == 0 || strcmp(dir, "/") == 0)
  {
    return base;
  }
  if (dir[0] == '/')
  {
    return base + dir;
  }
  return base + "/" + dir;
}

void *Music_OpenDir(const char *dir)
{
  String full = musicVfsPath(dir);
  AudioBusLock();
  DIR *d = opendir(full.c_str());
  AudioBusUnlock();
  if (d == nullptr)
  {
    Serial.printf("[DIR] 打不开目录: %s\n", dir ? dir : "(null)");
  }
  return d;
}

bool Music_ReadDir(void *handle, String &nameOut, bool &isDirOut)
{
  if (handle == nullptr)
  {
    return false;
  }
  DIR *d = (DIR *)handle;

  AudioBusLock();
  struct dirent *e = readdir(d);
  String name;
  bool isDir = false;
  if (e != nullptr)
  {
    name = e->d_name;
    /* d_type 由 ESP-IDF 的 FATFS VFS 填好（框架自己的 openNextFile()
       也正是靠它区分 DT_REG / DT_DIR，不认识就跳过），所以这里直接信它，
       不再为每个条目补一次 stat() —— 那正是我们要躲开的开销。 */
    isDir = (e->d_type == DT_DIR);
  }
  AudioBusUnlock();

  if (e == nullptr)
  {
    return false;
  }
  if (name == "." || name == ".." || name.startsWith("."))
  {
    return Music_ReadDir(handle, nameOut, isDirOut); // 跳过隐藏项/自身
  }

  nameOut = name;
  isDirOut = isDir;
  return true;
}

void Music_CloseDir(void *handle)
{
  if (handle == nullptr)
  {
    return;
  }
  AudioBusLock();
  closedir((DIR *)handle);
  AudioBusUnlock();
}

/* ================= 曲目详情（歌曲信息弹层用） ================= */

/* 取任意路径对应文件的字节数；失败返回 0。
   SD 访问要和 TFT 刷屏抢同一条 SPI 总线，必须加锁 —— 调用方（UI 线程）
   直接调即可，不用自己操心加锁。 */
uint32_t Music_GetFileSize(const char *path)
{
  if (path == nullptr || path[0] == 0)
  {
    return 0;
  }
  AudioBusLock();
  File f = SD.open(path, FILE_READ);
  uint32_t size = 0;
  if (f)
  {
    size = (uint32_t)f.size();
    f.close();
  }
  AudioBusUnlock();
  return size;
}

/* 取当前曲目的完整详情。

   各字段来源：
     title/artist/album        ← s_meta*（解码任务的标签回调写的），空则文件名兜底
     composer                  ← s_metaComposer（TCOM / COMPOSER 标签）
     path                      ← musicFiles[music_i]
     format                    ← 按扩展名判断，与 audioDoLoad() 的分派保持一致
     durationSec               ← Music_GetDuration()（必须走这个跨核接口）
     fileSize                  ← 读 SD 卡
     sampleRate/bits/channels  ← I2S 输出对象（解码器会把它们写在上面）
     bitrateKbps               ← 由「文件大小 × 8 ÷ 时长」算出

   比特率为什么是算出来的：
   ESPAudio 的解码器只把比特率 log 出来，没有通过元数据回调发给我们，
   而且各格式的拿法各不相同（MP3 帧头 / FLAC STREAMINFO / WAV fmt）。
   但「平均比特率 = 字节数×8 ÷ 秒数」对三种格式都成立，也正好是用户在
   文件属性里看到的那个数（VBR 文件尤其如此），所以直接算更省事也更准。
   ⚠ 刚切歌、时长还没解析出来时算不出，返回 0，UI 显示「未知」。 */
bool Music_GetCurrentTrackInfo(struct TrackInfo *info)
{
  if (info == nullptr)
  {
    return false;
  }
  memset(info, 0, sizeof(*info));

  if (fileCount <= 0 || music_i < 0 || music_i >= fileCount)
  {
    return false;
  }

  const String &full = musicFiles[music_i];

  /* 标题/艺术家/专辑：先让 Music_info() 把标签回调攒下的 s_meta*
     落实到 currentTitle/currentArtist/currentAlbum，并做好文件名兜底。
     注意 Music_info() 是 UI 线程专用（它读跨核变量并做兜底），
     本函数也只应由 UI 线程调用。 */
  Music_info();

  strncpy(info->title, currentTitle.c_str(), sizeof(info->title) - 1);
  strncpy(info->artist, currentArtist.c_str(), sizeof(info->artist) - 1);
  strncpy(info->album, currentAlbum.c_str(), sizeof(info->album) - 1);
  strncpy(info->composer, s_metaComposer, sizeof(info->composer) - 1);
  strncpy(info->path, full.c_str(), sizeof(info->path) - 1);

  // ── 格式：与 audioDoLoad() 的分派规则严格一致 ──
  String lower = full;
  lower.toLowerCase();
  const char *fmt = "MP3";
  if (lower.endsWith(".wav"))
  {
    fmt = "WAV";
  }
  else if (lower.endsWith(".flac"))
  {
    fmt = "FLAC";
  }
  else if (lower.endsWith(".aac"))
  {
    fmt = "AAC";
  }
  else if (lower.endsWith(".m4a"))
  {
    fmt = "M4A";
  }
  else if (lower.endsWith(".ogg"))
  {
    fmt = "OGG";
  }
  else if (lower.endsWith(".opus"))
  {
    fmt = "OPUS";
  }
  strncpy(info->format, fmt, sizeof(info->format) - 1);

  // ── 时长：必须走跨核接口，直接读全局变量会拿到陈旧值 ──
  const long dur = Music_GetDuration();
  info->durationSec = (dur > 0) ? (uint32_t)dur : 0;

  // ── 文件大小：读卡（内部已加 SPI 锁）──
  info->fileSize = Music_GetFileSize(full.c_str());

  /* ── 采样率 / 位深 / 声道 ──
     解码器在 begin() 时会把这三个值写到 I2S 输出对象上
     （见 AudioGeneratorMP3/FLAC/WAV 里的 output->SetRate 等），
     但**不会**通过元数据回调发出来，所以只能从输出对象读。
     信息弹层通常在开始播放之后才被打开，那时这些值已经就绪。 */
  if (s_output != nullptr)
  {
    info->sampleRate = (uint32_t)s_output->GetRate();
    info->bitsPerSample = (uint8_t)s_output->GetBitsPerSample();
    info->channels = (uint8_t)s_output->GetChannels();
  }

  // ── 平均比特率 = 字节数 × 8 ÷ 秒数 ÷ 1000 ──
  if (info->fileSize > 0 && info->durationSec > 0)
  {
    const uint64_t bits = (uint64_t)info->fileSize * 8ULL;
    const uint64_t kbps = bits / (uint64_t)info->durationSec / 1000ULL;
    info->bitrateKbps = (kbps > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)kbps;
  }

  return true;
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

  /* 【整组原子读取】见 s_timeMux 的说明：
     s_playStartMs / s_pausedMs / s_seekBaseMs 由解码任务一起改写，
     分开读会读到「新 seekBase + 旧 playStart」这种撕裂组合，
     算出 71574:49 那种因 uint32 下溢得到的荒唐值。 */
  uint32_t startMs, pausedMs, baseMs;
  portENTER_CRITICAL(&s_timeMux);
  startMs = s_playStartMs;
  pausedMs = s_pausedMs;
  baseMs = s_seekBaseMs;
  portEXIT_CRITICAL(&s_timeMux);

  const uint32_t now = s_decPaused ? s_pauseStartMs : millis();

  /* 用有符号 32 位做减法再判断正负：
     uint32 直接相减一旦「减数 > 被减数」就会下溢成一个接近 2^32 的
     大数，这正是 71574:49 的来源。改成 int64 之后：
       · millis() 回绕（约 49.7 天）依然正确 —— 无符号差值的语义由
         int32 解释，等价于「按模 2^32 的最近距离」；
       · 任何异常情况（时钟抖动、状态撕裂）算出的负数都能被下面的
         夹取拦住，最坏也只是显示 0，不会出现天文数字。 */
  int64_t elapsed = (int64_t)(int32_t)(now - startMs) - (int64_t)pausedMs;
  if (elapsed < 0)
  {
    elapsed = 0;
  }

  /* 加上跳转基准：跳转后解码器是从中间开始解的，
     必须补上「跳过去的那一段」，否则进度条会从 0 重新开始数。 */
  int64_t total = elapsed + (int64_t)baseMs;
  if (total < 0)
  {
    total = 0;
  }

  /* 上限夹取：进度不应超过曲目总时长（时长未知时按 0 处理，
     即不做上限约束，避免刚开头就把进度压成 0）。 */
  const long durSec = Music_GetDuration();
  if (durSec > 0)
  {
    const int64_t maxMs = (int64_t)durSec * 1000;
    if (total > maxMs)
    {
      total = maxMs;
    }
  }

  return (uint32_t)(total / 1000);
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

/* ================= 拖动进度条跳转（seek） =================

   只能在 UI 线程调用。单位是毫秒（0 ~ 曲目总时长）。

   为什么单位用毫秒而不是秒：
   拖动进度条时滑块位置是连续的，LVGL 给的是 0~100 的百分比。
   用秒做单位在长曲目上（比如 10 分钟）粒度是 1/600，拖动会一跳一跳；
   用毫秒可以让换算无損耗，具体精度由下面的字节换算决定。

   这里只置一个 volatile 意图、立刻返回 —— 真正的重开解码器由解码任务
   在 Music_Loop() 里做（它会 new/delete 整条解码链，绝不能跨核操作）。 */
void Music_SeekToMs(uint32_t ms)
{
  /* 夹到有效范围：拖动松手瞬间、或时长刚好在变（切歌）时，
     传进来的值可能超过当前时长。越界会让解码器直接 EOF。 */
  const uint32_t durMs = (uint32_t)Music_GetDuration() * 1000u;
  if (durMs > 0 && ms > durMs)
  {
    /* 留 1 秒余量：拖到最右端时如果正好等于总长，
       按字节换算会落在文件末尾，解码器读到 EOF 会立刻判定播放结束。 */
    ms = (durMs > 1000u) ? (durMs - 1000u) : 0u;
  }
  s_seekReqMs = ms;
  s_needSeek = true;
}

/*播放指定路径下的音频*/
void Music_PlayPath(const char *path)
{
  if (path == nullptr)
    return;

  /* 卡被拔出/尚未就绪时拒绝点歌：此时 musicFiles 可能已被清空、甚至已释放，
     把一条指向已释放内存的命令投递出去只会让解码任务去打开一个不存在的卡。
     这个守卫同时也保护 UI 的「自动切歌」——卡不在时不会去索引空列表。 */
  if (!s_sdReady)
  {
    return;
  }

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
  /* 数值没变就直接返回：既避免重复设置增益，也避免把同一个值反复写进 NVS
     （滑块拖动时同一档可能被上报多次）。 */
  if (v == (uint8_t)volume)
  {
    return;
  }
  volume = v;
  // 真正的 SetGain 由解码任务执行（音频对象归解码任务所有）
  s_pendingGain = (int)((v * 100) / 21);
  // 实时持久化：断电重开后按这个值恢复
  Music_SaveVolume();
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
/* 目录句柄用 opendir 的 DIR*（Music_OpenDir 返回的 void*），不再用 File：
   File::openNextFile() 每取一个条目都会按完整路径 stat() 一次，而 FATFS 的
   按路径查找得从目录头扫起 —— 277 首的目录就是 3.8 万次目录项扫描，
   扫描进度条看着像卡死。readdir 自带 d_type，一次顺序扫描全拿到。
   注意：Music_OpenDir/ReadDir/CloseDir 内部已经加了 SPI 总线锁，
   而 AudioBusLock 是普通互斥量（不可重入），这里绝不能再套一层锁。 */
static void *s_scanDirHandle = nullptr;
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
    Music_CloseDir(s_scanDirHandle);
    s_scanDirHandle = nullptr;
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

        s_scanDirHandle = Music_OpenDir(s_scanCurDir.c_str());

        s_scanDirsDone++;
        if (s_scanDirHandle == nullptr)
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

      // 取一个目录项（readdir 只做一次顺序扫描，不按路径 stat）
      String name;
      bool isDir = false;
      bool valid = Music_ReadDir(s_scanDirHandle, name, isDir);

      if (!valid)
      {
        Music_CloseDir(s_scanDirHandle);
        s_scanDirHandle = nullptr;
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
    Music_CloseDir(s_scanDirHandle);
    s_scanDirHandle = nullptr;
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
    Music_CloseDir(s_scanDirHandle);
    s_scanDirHandle = nullptr;
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
