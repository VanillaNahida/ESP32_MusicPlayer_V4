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
#include "esp_heap_caps.h"
#include "freertos/semphr.h"

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

// ====== 音频命令队列：UI 只投递命令，绝不直接触碰音频对象 ======
enum AudioCmdType : uint8_t
{
  ACMD_LOAD = 0, // 装载并播放指定曲目
  ACMD_PLAY,     // 继续播放
  ACMD_PAUSE,    // 暂停
  ACMD_STOP      // 停止并释放
};

struct AudioCmdMsg
{
  uint8_t type;
  char path[300];
};

static QueueHandle_t s_cmdQueue = nullptr;

// 元数据缓存（解码器回调运行在音频任务核心，先写入缓冲区，再由 Music_info() 拷贝给 UI）
static char s_metaTitle[256] = {0};
static char s_metaArtist[256] = {0};
static char s_metaAlbum[256] = {0};
static volatile bool s_metaDirty = false;

bool durationPrinted = false; //
long duration = 0;            // 音频总时长（秒）

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
      duration = ms / 1000; // 回调单位是毫秒，转换为秒
    }
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
  s_pausedMs = 0;
  s_pauseStartMs = 0;
  s_playStartMs = millis();
  duration = 0;
  durationPrinted = false;
  s_metaTitle[0] = 0;
  s_metaArtist[0] = 0;
  s_metaAlbum[0] = 0;

  String fullPath = normalizePath(path);

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

  // DMA 缓冲加大到 16 个（默认 8）：TFT 刷屏与 SD 读卡共用同一条 SPI 总线，
  // 缓冲更充裕可以吸收总线争用带来的填充延迟，减少 I2S 欠载（爆音）
  s_output = new AudioOutputI2S(0, 16);
  s_output->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  s_output->SetMclk(false);
  s_output->SetGain(volume / 21.0f); // 音量 0~21 映射为增益 0~1.0

  Serial.printf("[AUDIO] I2S 引脚 BCLK=%d LRC=%d DOUT=%d, 增益=%.2f\n",
                I2S_BCLK, I2S_LRC, I2S_DOUT, (double)(volume / 21.0f));

  String lower = fullPath;
  lower.toLowerCase();
  AudioFileSource *srcForGen = s_source;

  if (lower.endsWith(".wav"))
  {
    s_generator = new AudioGeneratorWAV();
  }
  else if (lower.endsWith(".flac"))
  {
    s_generator = new AudioGeneratorFLAC();
  }
  else if (lower.endsWith(".aac"))
  {
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

// 初始化音乐系统
void Music_Init()
{
  Serial.printf("PSRAM 总容量: %u KB, 空闲: %u KB\n",
                (unsigned)(ESP.getPsramSize() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  Serial.printf("内部堆 空闲: %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));

  // 创建音频命令队列（UI 与解码任务之间的唯一通信通道）
  if (s_cmdQueue == nullptr)
  {
    s_cmdQueue = xQueueCreate(8, sizeof(AudioCmdMsg));
  }

  // 复用 TFT_eSPI 已初始化的 SPI 总线，避免两处各自初始化 SPI2 主机导致 SD 读失效
  if (!SD.begin(SD_Pin, SPI, 10000000))
  {
    Serial.printf("SD卡初始化失败（CS = GPIO%d），请检查 SD 卡与接线\n", SD_Pin);
    Serial.flush();
    return;
  }
  Serial.println("SD卡初始化成功");

  // 优先尝试加载播放列表，如果不存在则扫描SD卡并创建播放列表
  if (playlistExists())
  {
    Serial.println("发现播放列表，正在加载...");
    if (loadPlaylist())
    {
      Serial.printf("成功加载 %d 首歌曲\n", fileCount);
    }
    else
    {
      Serial.println("加载播放列表失败，重新扫描...");
      listMusicFiles(folder); // 列出音乐文件
      savePlaylist();          // 保存播放列表
    }
  }
  else
  {
    Serial.println("首次使用，正在扫描SD卡并创建播放列表...");
    listMusicFiles(folder); // 列出音乐文件
    if (fileCount > 0)
    {
      savePlaylist(); // 保存播放列表
      Serial.printf("播放列表创建成功，共 %d 首歌曲\n", fileCount);
    }
  }

  // 注意：不在这里播放歌曲，等待UI初始化后由 Music_RestorePlayState() 恢复播放状态
  Serial.printf("播放列表加载完成，共 %d 首歌曲\n", fileCount);
}

// 主循环处理音频播放（在独立解码任务中运行，独占全部音频对象）
void Music_Loop()
{
  // 1. 处理 UI 投递的命令（非阻塞，一次清空队列）
  if (s_cmdQueue != nullptr)
  {
    AudioCmdMsg msg;
    while (xQueueReceive(s_cmdQueue, &msg, 0) == pdTRUE)
    {
      switch (msg.type)
      {
      case ACMD_LOAD:
        audioDoLoad(msg.path);
        break;

      case ACMD_STOP:
        audioDoStop();
        break;

      case ACMD_PLAY:
        if (s_decRunning && s_decPaused)
        {
          s_pausedMs += millis() - s_pauseStartMs;
          s_decPaused = false;
        }
        break;

      case ACMD_PAUSE:
        if (s_decRunning && !s_decPaused)
        {
          s_decPaused = true;
          s_pauseStartMs = millis();
        }
        break;

      default:
        break;
      }
    }
  }

  // 2. 应用待生效的音量
  if (s_pendingGain >= 0)
  {
    if (s_output != nullptr)
    {
      s_output->SetGain(s_pendingGain / 100.0f);
    }
    s_pendingGain = -1;
  }

  // 3. 解码推进（暂停时完全不喂数据，I2S 由 auto_clear 输出静音）
  // 注意：这里不能持 SPI 总线锁 —— 一次解码会填满 I2S 缓冲(~90ms)再阻塞等待(~100ms)，
  // 持锁近 200ms 会把 core1 的刷屏彻底饿死。SPI 互斥已下移到实际的 SD 读取处
  // （见 AudioFileSourceFS::read / AudioBusLock.h）。
  if (s_decRunning && !s_decPaused && s_generator != nullptr && s_generator->isRunning())
  {
    if (!s_generator->loop())
    {
      s_generator->stop();
      s_decRunning = false;
      s_decEnded = true;
      Serial.println("[AUDIO] 曲目播放结束");
    }
  }

  delay(1);
}

// 辅助函数：实际扫描并添加音乐文件
void scanAndAddMusicFiles(String dir)
{
  File root = SD.open(dir);
  if (!root)
  {
    return;
  }

  while (true)
  {
    File file = root.openNextFile();
    if (!file)
      break;

    if (file.isDirectory())
    {
      String subDir = normalizePath(dir + "/" + file.name());
      scanAndAddMusicFiles(subDir); // 递归处理子目录
    }
    else if (isMusicFile(file.name()))
    {
      // 统一路径格式，避免出现 "//目录/文件" 这种双斜杠导致后续打开失败
      addMusicFile(normalizePath(dir + "/" + file.name()));
    }

    file.close();
  }

  root.close();
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

// 检查是否是音乐文件
bool isMusicFile(String name)
{
  return !name.startsWith(".") &&
         (name.endsWith(".mp3") || name.endsWith(".MP3") ||
          name.endsWith(".wma") || name.endsWith(".WMA") ||
          name.endsWith(".wav") || name.endsWith(".WAV"));
}

//  解析歌词文件
void parseLRC(const char *filename, LyricEntry *lyrics, int &lyricCount)
{
  lyricCount = 0;

  if (filename == nullptr || strlen(filename) == 0)
  {
    Serial.println("LRC 文件路径为空");
    return;
  }

  AudioBusLock();
  File file = SD.open(filename);
  if (!file)
  {
    AudioBusUnlock();
    Serial.printf("无法打开 LRC 文件: %s\n", filename);
    return;
  }

  while (file.available() && lyricCount < maxLyrics)
  {
    String line = file.readStringUntil('\n');
    line.trim();

    // 跳过空行和无时间标签行
    if (line.length() == 0 || !line.startsWith("["))
    {
      continue;
    }

    int startTag = 0;
    int endTag = line.indexOf("]");

    if (endTag == -1 || endTag < 3)
    {
      // 没有找到结束括号或格式不对
      continue;
    }

    String timestampStr = line.substring(startTag + 1, endTag);
    String lyricText = line.substring(endTag + 1);

    // 验证时间格式 mm:ss.xx 或 mm:ss.xx
    if (timestampStr.length() < 5 ||
        timestampStr[2] != ':' ||
        (timestampStr[5] != '.' && timestampStr[5] != ':'))
    {
      continue;
    }

    int timestamp = 0;
    uint8_t m1 = chartonumber(timestampStr[0]);
    uint8_t m2 = chartonumber(timestampStr[1]);
    uint8_t s1 = chartonumber(timestampStr[3]);
    uint8_t s2 = chartonumber(timestampStr[4]);

    timestamp = (m1 * 10 + m2) * 60 + s1 * 10 + s2;

    lyrics[lyricCount].timestamp = timestamp;
    lyrics[lyricCount].lyric = lyricText;
    lyricCount++;
  }

  file.close();
  AudioBusUnlock();
}
/*查找歌词文件是否存在，存在解析*/
void parseLrcFile(String musicName)
{
  String music_path = normalizePath(musicName);                                    // 统一路径格式
  String lrc_path = music_path.substring(0, music_path.lastIndexOf('.')) + ".lrc"; // 歌词路径
  // 注意：这里应判断歌词文件是否存在（原实现误判为歌曲文件）
  AudioBusLock();
  bool lrcExist = SD.exists(lrc_path);
  AudioBusUnlock();
  if (lrcExist)
  {
    lrc_flag = 1;
    Serial.println("找到歌词文件");
    parseLRC(lrc_path.c_str(), lyrics, lyricCount);
  }
  else
  {
    lrc_flag = 0;
    Serial.println("没有找到歌词文件");
  }
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

/* 投递命令给解码任务（由 UI 侧调用，非阻塞） */
static void audioSendCmd(uint8_t type, const char *path)
{
  if (s_cmdQueue == nullptr)
  {
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
  // 命令队列很短，正常不会满；即使满也绝不阻塞 UI
  if (xQueueSend(s_cmdQueue, &msg, 0) != pdTRUE)
  {
    Serial.println("[AUDIO] 命令队列已满，命令被丢弃");
  }
}

// 播放（无曲目时装载当前曲目，暂停状态则继续）
void Music_Play()
{
  if (!s_decRunning)
  {
    if (fileCount > 0 && music_i >= 0 && music_i < fileCount)
    {
      Music_PlayPath(musicFiles[music_i].c_str());
    }
    return;
  }
  audioSendCmd(ACMD_PLAY, nullptr);
}

// 暂停
void Music_Pause()
{
  audioSendCmd(ACMD_PAUSE, nullptr);
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

  durationPrinted = (duration != 0);
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
        Music_PlayPath(musicFiles[music_i].c_str());

        // 等待曲目信息（标题/时长）到达，最多等待 5 秒，避免死循环
        uint32_t waitStart = millis();
        while (duration == 0 && millis() - waitStart < 5000)
        {
            Music_info();
            delay(5);
        }
        Music_info();
        Serial.printf("[AUDIO] 时长获取%s: %ld 秒\n", (duration != 0) ? "成功" : "失败", duration);

        // 解析歌词
        parseLrcFile(musicFiles[music_i]);

        // 设置为暂停状态
        pause_status = 1;
        Music_Pause();

        Serial.printf("已加载：%s\n", musicFiles[music_i].c_str());
    }
}
