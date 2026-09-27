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
extern long duration;
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
// 动态数组管理函数
bool allocateMusicArray(int size);
void freeMusicArray();
bool addMusicFile(String filePath);
// 播放状态记忆函数
bool savePlayState();
bool loadPlayState();
void Music_RestorePlayState();  // 恢复播放状态（UI初始化后调用）

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