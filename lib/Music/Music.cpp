// Music.cpp
#include "Music.h"
#include <Audio.h>

// SD引脚定义
#define SD_Pin 5
// 设置I2S音频引脚
#define I2S_DOUT 25
#define I2S_BCLK 27
#define I2S_LRC 26
// 播放列表文件路径
#define PLAYLIST_FILE "/music_playlist.txt"
// 播放状态文件路径
#define PLAYSTATE_FILE "/music_state.txt"

// 音量
int volume = 15;

// 动态音乐数组
String* musicFiles = nullptr;
int fileCount = 0;           // 当前音乐文件数量
int maxAllocatedFiles = 0;    // 当前已分配的最大音乐文件数量
String folder = "/";    // 音乐文件夹路径
int music_i = 0;             // 当前播放索引
int music_prev_i = 0;        // 上一个播放索引

Audio audio;

bool durationPrinted = false; //
long duration = 0;            // 音频总时长

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

// 初始化音乐系统
void Music_Init()
{
  if (SD.begin(SD_Pin))
  {
    Serial.println("SD卡初始化成功");
  }
  else
  {
    Serial.println("SD卡初始化失败");
    ESP.restart();
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume); // 设置音量大小，范围0...21

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

// 主循环处理音频播放
void Music_Loop()
{
  audio.loop();
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
      String subDir = dir + "/" + file.name();
      scanAndAddMusicFiles(subDir); // 递归处理子目录
    }
    else if (isMusicFile(file.name()))
    {
      String fullPath = dir + "/" + file.name();
      addMusicFile(fullPath);
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

  File file = SD.open(filename);
  if (!file)
  {
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
}
/*查找歌词文件是否存在，存在解析*/
void parseLrcFile(String musicName)
{
  String music_path = musicName;                                                          // 歌曲路径
  String lrc_path = music_path.substring(0, music_path.lastIndexOf('.')) + ".lrc"; // 歌词路径
  if (SD.exists(music_path))
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

// 播放
void Music_Play()
{

  if (!audio.isRunning() && !audio.pauseResume())
  {
    String music_path = musicFiles[music_i]; // 当前播放路径
    audio.connecttoFS(SD, music_path.c_str());
    Serial.print("当前曲目: ");
    Serial.println(musicFiles[music_i]);
  }
}
// 暂停
void Music_Pause()
{

  if (audio.isRunning())
  {
    audio.pauseResume();
  }
}

// 下一曲
void Music_Next()
{
  if (fileCount == 0)
    return; // 无文件可播

  music_i = (music_i + 1) % fileCount; // 循环到下一首
  audio.stopSong();                    // 切换曲目时候，先要停止歌曲，然后加个延迟，等I2S清空下缓冲区，然后再播放，否则会有I2S缓冲区满的错误
  delay(100);
  String music_path = musicFiles[music_i]; // 当前播放路径
  audio.connecttoFS(SD, music_path.c_str());
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

  music_i = (music_i - 1 + fileCount) % fileCount;        // 循环到上一首
  String music_path = musicFiles[music_i]; // 当前播放路径
  audio.stopSong();
  delay(100);
  audio.connecttoFS(SD, music_path.c_str());
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

  music_i = 0;                                            // 播放第一首
  String music_path = musicFiles[music_i]; // 当前播放路径
  audio.stopSong();
  delay(100);
  audio.connecttoFS(SD, music_path.c_str());
  Serial.print("第一曲: ");
  Serial.println(musicFiles[music_i]);
}

// 最后一曲
void Music_Last()
{
  if (fileCount == 0)
    return; // 无文件可播

  music_i = fileCount - 1;                                // 播放最后一曲
  String music_path = musicFiles[music_i]; // 当前播放路径
  audio.stopSong();
  delay(100);
  audio.connecttoFS(SD, music_path.c_str());
  Serial.print("最后一曲: ");
  Serial.println(musicFiles[music_i]);
}

bool Music_IsPlaying()
{
  return audio.isRunning();
}

// 显示音频总时长
// 如果播放时间值跟之前找到歌词时间值不一样，确保只显示一次总时长
/*获取音频总时长，曲目名称，歌手信息*/
void Music_info()
{
  if (!durationPrinted && audio.isRunning())
  {
    duration = audio.getAudioFileDuration();
    if (duration > 0)
    {
      Serial.printf("音频总时长：%d 秒,曲目名称：%s ,歌手：%s\n", duration, currentTitle.c_str(), currentArtist.c_str());

      // 如果标题为空，则显示文件名（不含扩展名）
      if (currentTitle.length() == 0)
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

      // 如果歌手为空，则显示 "未知艺术家"
      if (currentArtist.length() == 0)
      {
        currentArtist = "未知歌手";
      }

      durationPrinted = true;
    }
    else
    {
      Serial.println("音频时长获取失败（可能为 MP3 编码或未解析完成）");
      durationPrinted = false;
    }
  }
}

/*获取当前播放时长*/
uint32_t Music_GetCurrentPlayTime()
{
  return audio.getAudioCurrentTime();
}
/*播放指定路径下的音频*/
void Music_PlayPath(const char *path)
{
  audio.stopSong();
  delay(100);
  String music_path = folder + "/" + path;   // 当前播放路径
  audio.connecttoFS(SD, music_path.c_str()); // 默认播放
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
void setVolume(uint8_t volume)
{
  if (volume < 0)
  {
    volume = 0;
  }
  else if (volume > 21)
  {
    volume = 21;
  }
  audio.setVolume(volume);
}

/*获取音量值*/
uint8_t getVolume()
{
    return audio.getVolume();
}

// 检查播放列表是否存在
bool playlistExists()
{
    return SD.exists(PLAYLIST_FILE);
}

// 保存播放列表到SD卡
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
        Serial.println("无法打开播放列表文件进行写入");
        return false;
    }

    // 第一行写入歌曲总数
    playlistFile.println(fileCount);
    
    // 逐行写入歌曲路径
    for (int i = 0; i < fileCount; i++)
    {
        playlistFile.println(musicFiles[i]);
    }

    playlistFile.close();
    Serial.printf("播放列表保存成功，共 %d 首歌曲\n", fileCount);
    return true;
}

// 从SD卡加载播放列表
bool loadPlaylist()
{
    File playlistFile = SD.open(PLAYLIST_FILE);
    if (!playlistFile)
    {
        Serial.println("无法打开播放列表文件进行读取");
        return false;
    }

    // 读取第一行的歌曲总数
    String countLine = playlistFile.readStringUntil('\n');
    countLine.trim();
    int storedCount = countLine.toInt();
    
    // 验证数量的合法性
    if (storedCount <= 0)
    {
        Serial.println("播放列表格式错误");
        playlistFile.close();
        return false;
    }

    // 分配刚好足够的内存
    if (!allocateMusicArray(storedCount))
    {
        Serial.println("无法分配播放列表内存");
        playlistFile.close();
        return false;
    }
    
    // 读取歌曲路径
    while (playlistFile.available() && fileCount < storedCount)
    {
        String line = playlistFile.readStringUntil('\n');
        line.trim(); // 去除首尾空白字符和换行符
        if (line.length() > 0)
        {
            musicFiles[fileCount++] = line;
        }
    }

    playlistFile.close();
    
    if (fileCount != storedCount)
    {
        Serial.printf("警告：期望加载 %d 首，实际加载 %d 首\n", storedCount, fileCount);
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
    File stateFile = SD.open(PLAYSTATE_FILE, FILE_WRITE);
    if (!stateFile)
    {
        Serial.println("无法打开播放状态文件进行写入");
        return false;
    }
    
    // 写入当前播放索引
    stateFile.println(music_i);
    
    stateFile.close();
    Serial.printf("播放状态已保存：当前曲目索引 = %d\n", music_i);
    return true;
}

// 从SD卡加载播放状态
bool loadPlayState()
{
    if (!SD.exists(PLAYSTATE_FILE))
    {
        Serial.println("播放状态文件不存在");
        return false;
    }
    
    File stateFile = SD.open(PLAYSTATE_FILE, FILE_READ);
    if (!stateFile)
    {
        Serial.println("无法打开播放状态文件进行读取");
        return false;
    }
    
    // 读取当前播放索引
    String line = stateFile.readStringUntil('\n');
    line.trim();
    
    int savedIndex = line.toInt();
    
    // 验证索引合法性
    if (savedIndex >= 0 && savedIndex < fileCount)
    {
        music_i = savedIndex;
        stateFile.close();
        Serial.printf("播放状态已加载：当前曲目索引 = %d\n", music_i);
        return true;
    }
    else
    {
        stateFile.close();
        Serial.printf("播放状态索引无效：%d（总曲目数：%d）\n", savedIndex, fileCount);
        return false;
    }
}

// 恢复播放状态（UI初始化后调用）
void Music_RestorePlayState()
{
    bool stateLoaded = false;
    
    // 尝试加载播放状态
    if (loadPlayState())
    {
        Serial.printf("恢复上次播放位置：第 %d 首\n", music_i + 1);
        stateLoaded = true;
    }
    else
    {
        Serial.println("没有找到播放状态记录，从第一首开始");
        music_i = 0;  // 默认从第一首开始
    }
    
    // 播放歌曲
    if (fileCount > 0 && music_i >= 0 && music_i < fileCount)
    {
        String music_path = musicFiles[music_i];
        audio.stopSong();
        delay(100);
        audio.connecttoFS(SD, music_path.c_str());
        
        // 等待歌曲信息获取完毕
        while (duration == 0)
        {
            Music_info();
        }
        
        // 解析歌词
        parseLrcFile(musicFiles[music_i]);
        
        // 设置为暂停状态
        pause_status = 1;
        audio.pauseResume();
        
        Serial.printf("已加载：%s\n", music_path.c_str());
    }
}
