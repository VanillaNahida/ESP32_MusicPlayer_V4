#ifndef MUSIC_H
#define MUSIC_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

enum PlayMode
{
    SINGLE_LOOP, // 单曲循环
    LIST_LOOP,   // 列表循环
    RANDOM_PLAY  // 随机播放
};

// 全局变量声明
extern int volume;          // 音量
extern int fileCount;           // 当前音乐文件总数量
extern int maxAllocatedFiles;   // 当前已分配的最大音乐文件数量
extern String folder;
extern String currentTitle;
extern String currentArtist;
extern String currentAlbum;
/* 总时长不再用裸全局变量跨核共享。
   它是解码任务(core0)写、UI(core1)读的数据，必须经由下面两个函数访问，
   否则会因为缺 volatile 而读到陈旧值（表现为进度条不动、时长恒为 00:00）。
   写入方：解码任务的元数据回调；读取方：UI。 */
long Music_GetDuration();
void Music_SetDuration(long sec);
extern bool durationPrinted;
extern int music_i;             // 当前播放文件索引
extern int music_prev_i;        // 上一个播放文件索引
extern int lyricCount;          // 歌词数量
extern boolean lrc_flag;        // 是否存在歌词标志位
extern uint8_t lrc_m;           // 歌词分钟
extern uint8_t lrc_s;           // 歌词秒
extern boolean lrc_flag;        // 是否存在歌词标志位
extern struct LyricEntry lyrics[]; // 结构体数组
extern uint16_t temp_AudioCurrentTime;
extern String* musicFiles;      // 音乐文件数组指针（动态分配）
extern PlayMode currentPlayMode;
extern uint8_t pause_status;    // 暂停状态标志位

void Music_Init();
void Music_Loop();

/* ============ 播放列表：加载 / 首次扫描 ============
   Music_Init() 只负责起 SD 卡和请求通道，**不**碰播放列表 ——
   因为首次使用要全卡扫描、而扫描需要显示进度界面，进度界面又依赖 LVGL，
   而 Music_Init() 是在 lv_init() 之前调用的，那时还没有任何显示能力。

   所以 setup() 的顺序是：
       Music_Init();
       ... lv_init() / ui_init() / PlaylistTool_Init() ...
       if (!Music_LoadPlaylistCache())      // 有缓存 → 秒加载
           PlaylistTool_RunFirstScan();     // 没缓存 → 带进度界面扫描
   这样「首次使用扫描」和「按 BOOT 重建」走的是同一套代码和同一个界面。
   =================================================== */

// 存储卡是否已就绪
bool Music_IsStorageReady();
// 尝试直接载入播放列表缓存；返回 false 表示需要走一次全卡扫描
bool Music_LoadPlaylistCache();
// void Music_PlayPause();
// void Music_Next();
// void Music_Prev();

// 函数声明
bool isMusicFile(String name);
void listMusicFiles(String dir);
void parseLRC(const char *filename, struct LyricEntry *lyrics, size_t &lyricCount);
void printLyrics(struct LyricEntry *lyrics, size_t lyricCount);
uint8_t chartonumber(char charnumber);
bool Music_IsPlaying();

/*
  「曲目自然播放结束」事件（消费一次后自动清零）。

  为什么需要它：
  UI 以前用 !Music_IsPlaying() 来判断「这首歌放完了，该切下一首」，
  但这个条件在下面三种情况下**同样成立**：
      1) 曲目真的播完了            <- 只有这种才该自动切歌
      2) 正在装载下一首（解码器还没跑起来）
      3) 用户手动暂停了
  于是 UI 会在 2)、3) 时误判成「播完了」而自动跳到下一首，
  表现为「UI 显示和实际播放对不上、莫名跳歌」。

  正确做法是让音频侧明确区分「结束了」和「没在播」，由 UI 询问这个
  专门的标志位，而不是从「是否正在播放」反推。
*/
bool Music_ConsumeEnded();
void Music_info();
void parseLrcFile(String MusicName);
/* 换曲目时清掉上一首的曲目信息（歌名/歌手/专辑/歌词/时长标志）。
   必须由 UI 线程在切歌的那一刻调用：否则新曲目若没有标签，
   界面会一直显示上一首的歌名 —— 也就是「列表里选歌后文字没更新」。
   清掉之后 Music_info() 会用新曲目的文件名兜底。 */
void Music_ResetTrackInfo();
/* 解码任务是否已解析出**指定曲目**的内嵌歌词（ID3 USLT / FLAC 的 LYRICS）。
   用途：换歌瞬间就去 parseLrcFile() 是拿不到内嵌歌词的（标签还没解析完），
   UI 一旦发现这个函数变 true，就需要为当前曲目补查一次歌词。 */
bool Music_EmbeddedLyricsReady(const char *path);
uint32_t Music_GetCurrentPlayTime();
// 获取当前曲目内嵌的专辑封面（原始 JPEG 数据）；无封面时返回 false
// revision 每换一张封面会变化，供 UI 判断是否需要重新解码显示
bool Music_GetAlbumCover(const uint8_t **data, size_t *size, uint32_t *revision);
void Music_PlayPath(const char *path);

// 播放列表相关函数
bool playlistExists();
bool savePlaylist();
bool loadPlaylist();
void refreshPlaylist();

/* ============ 供「文件浏览页」使用的路径/目录工具 ============
   放在这里而不是 UI 里：它们都要读 SD 卡，而 SD 访问必须走 SPI 总线锁
   （与 TFT 刷屏互斥）。关在一个地方，UI 侧就不用操心加锁的事。 */

// 取父目录：".../a/b/c.mp3" -> ".../a/b"；根目录再往上仍然是 "/"
String Music_ParentDir(const String &path);
// 取文件名（含扩展名）
String Music_BaseName(const String &path);

/* ---------- 高效目录枚举（句柄式，支持增量/跨帧进行）----------

   为什么不用 File::openNextFile()：
   它内部会 new 一个 VFSFileImpl，而那个构造函数会对**每一个条目**做一次
   按完整路径的 stat()。FATFS 的按路径查找是从目录开头逐条扫的，
   于是遍历 n 个条目变成 O(n²) —— 实测一个 277 首的目录要做约 3.8 万次
   目录项扫描，界面会在这一步冻住好几秒。
   这里直接用 opendir/readdir：readdir 返回的 struct dirent 自带 d_type，
   一次顺序扫描就同时拿到「名字」和「是不是目录」，复杂度回到 O(n)。

   做成句柄式是为了让调用方能**分帧**枚举：每帧只看几十个条目，
   再大的目录也不会把主循环卡住。 */
void *Music_OpenDir(const char *dir);                                  // 失败返回 nullptr
bool Music_ReadDir(void *handle, String &nameOut, bool &isDirOut);     // 没有更多条目返回 false
void Music_CloseDir(void *handle);
// 动态数组管理函数
bool allocateMusicArray(int size);
void freeMusicArray();
bool addMusicFile(String filePath);
// 播放状态记忆函数
bool savePlayState();
bool loadPlayState();
void Music_RestorePlayState();  // 恢复播放状态（UI初始化后调用）

/* ================= 播放列表重建（增量扫描，带进度） =================

   为什么要做成「增量」而不是一次性扫完：
   全卡递归可能耗时几秒。如果写成一次阻塞调用，主循环就没法跑
   lv_timer_handler()，进度条根本画不出来，界面看起来是死的。
   拆成每轮只干一小段时间（budgetMs），主循环就能在两次 Music_RescanStep()
   之间刷新进度条和文字，同时保持触摸响应。

   ⚠ 重建**只做目录枚举 + 文件登记**，不解析任何 ID3、不生成歌词。
     歌词（含 /lrc/<歌名>.lrc 的生成）全部推迟到真正播放某首歌的时候，
     由 parseLrcFile() 顺带完成 —— 否则为了几十首可能永远不播的歌，
     要把每首几百 KB 的标签都读一遍，重建会慢几十倍。

   典型用法（见 src/PlaylistTool.cpp）：
       Music_RescanBegin();
       while (!Music_RescanStep(30)) { lv_timer_handler(); 更新进度条; }
       Music_RescanEnd();                       // 落盘 + 收尾
   中途放弃调用 Music_RescanAbort()（会重新载入 SD 上原有的列表）。
   ==================================================================== */

// 开始重建：停播放、清空当前列表、准备扫描队列
void Music_RescanBegin();
// 推进扫描最多 budgetMs 毫秒；返回 true 表示已扫完（此时列表已在内存里）
bool Music_RescanStep(uint32_t budgetMs);
// 扫描完成后的收尾：写二进制播放列表、复位索引与内嵌歌词缓存
// 返回 true 表示列表已成功保存
bool Music_RescanEnd();
// 放弃扫描：丢弃半成品，重新载入 SD 上原有的播放列表
void Music_RescanAbort();

int Music_RescanPercent();              // 0..100
const char *Music_RescanCurrentPath();  // 当前正在处理的条目（供 UI 显示）
int Music_RescanFoundCount();           // 已发现的歌曲数
bool Music_RescanSaveOk();              // 重建后的列表是否成功落盘

// 结构体定义
struct LyricEntry
{
    uint16_t timestamp;
    String lyric;
};

#ifdef __cplusplus
extern "C"
{
#endif

    void Music_Play();
    void Music_Pause();
    /* 设置「用户期望的暂停状态」（true=暂停，false=播放）。
       与 Music_Pause()/Music_Play() 的区别：它只表达意图，不会去装载曲目。
       这个意图走的是 volatile 变量而不是命令队列，因此**永远不会丢**：
       解码任务每轮都会把它落实到解码器上。*/
    void Music_WantPaused(bool paused);
    /* 「解码任务存活计数」。每次 Music_Loop() 自增一次。
       UI 侧可用它判断解码任务是否卡死（长时间不变 = 卡死），
       此时界面上任何音频操作都不会有反应，问题在解码任务侧。*/
    uint32_t Music_GetLoopSeq();
    void Music_Next();
    void Music_Prev();
    void Music_First();
    void Music_Last();
    void setVolume(uint8_t volume);
    uint8_t getVolume();
    void switchPlayMode();

#ifdef __cplusplus
}
#endif

#endif // MUSIC_H