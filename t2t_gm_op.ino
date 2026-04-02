/*
 * 2026.03.27 開始 
 * 
 * 2026.04.01 
 *          備份版本t2t_gm_op0327.bak 使用wifi 必須設定ssid及PASSWORD
 *          增加wifi smartcnf設定,手機上下載ESPtouch連線wifi後scan esp device傳帳密給esp設定使用
 *          備份版本t2t_gm_op0402.bak 不需要給ssid及PASSWORD,STACK精簡版
 *   
 * 程式名稱: AI聊天小物(包含Google語音輸出) 已完結
 * 1.程式碼需要使用huge mode編譯
 * 2.連接本地伺服器,包含VOSK STT(語音轉文字)+Ollama安裝QWEN2.51模型
 *   電腦伺服器若是192.168.0.6
 *   需要設定
    #define VOSK_SERVER   "http://192.168.0.6:5000/speech"
    #define TTS_SERVER   "http://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=zh-TW&q="

 * 
// 3.VOSK 安裝參考https://www.cnblogs.com/meetrice/p/15718111.html
//   啟動程序:1.進到cd C:\Users\halin\Desktop\vosk
             2.開啟批次檔server_ot.bat 
           
//
//    u8g2_font_5x7_tf     //5*7 1612bytes (20-FF) 192chars
//    u8g2_font_5x8_tr     //5*8 844bytes (20-7F) 96chars
//    u8g2_font_7x13_tr     //6*13 1111bytes (20-7F) 96chars
//    u8g2_font_7x14_tr     //7*14 1154bytes (20-7F) 96chars
//    u8g2_font_8x13_tr     //8*13 1127bytes (20-7F) 96chars
//    u8g2_font_9x15_tr     //9*15 1227bytes (20-7F) 96chars
//    u8g2_font_helvB08_tf //11*11 1028bytes (20-7F) 96chars
//    u8g2_font_helvB10_tf //14*17 2872bytes (20-FF) 192chars
* 觸摸按鍵說明
* 長按切換wifi開關
* 1下觸發錄音,若有wifi以及server運作下會進行即時交談,模式(聊天,問答,翻譯,找碴,寶貝)
* 2下播放記憶體內的語音資料以及切換交談模式
* 3下儲存記憶體內的語音資料成wav檔並播放rec000.wav-rec002.wav
* 開機初始將檢查是否有錄音檔在裡面,有的話將選最高編號播放(當留言機)
*/

#define USE_I2S
#define USE_I2S_V2
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
//使用i2s 驅動inmp441 + MAX98357A
#include <driver/i2s.h>
#include <SPIFFS.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include "time.h"
#include "esp_sntp.h"
#include <HTTPClient.h>
#include <vector>
//使用AudioTools 驅動inmp441 + MAX98357A
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h" // 需要安裝 Helix 解碼函式庫
#include "AudioTools/Communication/HTTP/URLStream.h"
//#include <esp_private/periph_ctrl.h>
AudioInfo info(8000, 1, 16); 
URLStream urlStream;               // 負責 HTTP 連線
MP3DecoderHelix mp3Decoder;         // 負責解碼 MP3 (Google TTS 回傳的是 MP3)
I2SStream i2s; 
EncodedAudioStream dec(&i2s, &mp3Decoder); // 串接：解碼 -> I2S
StreamCopy copier(dec, urlStream); 

//#define forceEraseNVS //強制清除wifi帳密資料,開機後將清除並停止執行
#define ShowTime
#define ShowRespCN  //中文回應後顯示在OLED上
//#define serialDSP //除錯顯示
//#define ChkHeap   //開啟顯示task堆疊使用狀況
// ============================================================
// 組態配置 (config.h)
// ============================================================
#define I2S_BCLK      5
#define I2S_LRC       6
#define I2S_DOUT      7
#define I2S_DIN       4
#define I2S_PORT      I2S_NUM_0
#define SAMPLE_RATE   8000
#define BLOCK_SIZE    256
#define resonsetimeout 40000  //VOSK STT + OLLAMA 等待回應時間40秒

typedef int16_t sample_t;
#define BYTES_PER_SAMPLE  2

#define I2C_SDA       8
#define I2C_SCL       9
#define OLED_ADDR     0x3C
#define TTP223_PIN     2
//伺服器URL API
#define VOSK_SERVER   "http://192.168.0.6:5000/speech"
#define TTS_SERVER   "http://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=zh-TW&q="
#define IDLE_TIME_MIN    10 //系統進入休眠前等待時間目前設定10分鐘
#define MAX_RECORD_SEC   4  //每次錄音的秒數
#define MAX_FILES        3  //錄音留言檔的數量,存在SPIFFS中
#define MAX_RECORD_SAMPLES (SAMPLE_RATE * MAX_RECORD_SEC)
#define RECORD_FILENAME  "/rec%03d.wav"
const uint32_t max_rec_ms = MAX_RECORD_SEC *1000;
const uint32_t recordCapacity = SAMPLE_RATE * MAX_RECORD_SEC * BYTES_PER_SAMPLE;
uint8_t recordBuffer[recordCapacity];
const uint32_t WIFI_TIMEOUT = IDLE_TIME_MIN *60000; // 10 分鐘沒使用就關 WiFi
uint32_t lastActivityMs = 0;          // 最後一次連線的時間
uint32_t lastRecordtime = 0;          // 最後一次錄音時間
static uint32_t resultStartTime = 0;  // STT的回應文字每頁顯示開始時間

static size_t recordSize = 0;
volatile bool isTTSplay = false; // 新增全域旗標TTS處理播放中
volatile bool stopTTS = false; // 全域變數，用來標記TTS是否要中斷

const int LineDelay = 3000; // 回應內容每行停頓時間2.5秒

RTC_DATA_ATTR int nextFileIndex = 0;
RTC_DATA_ATTR int chatmode = 0;             // 根據server端的mode選擇回應模式
                                            // 0:輕鬆愉快1:AI問答2:中翻英3:壞脾氣4:寶貝模式

//#define DEBOUNCE_MS      50
//#define LONG_PRESS_MS    1000
//#define DOUBLE_CLICK_MS  300
//#define TRIPLE_CLICK_MS  500
//========按鍵參數區===============
#define SCAN_INTERVAL_MS 5
#define DEBOUNCE_MS 50
#define LONG_PRESS_MS 2000
#define CLICK_TIMEOUT_MS 300

enum ButtonEvent {
    EVENT_SINGLE,
    EVENT_DOUBLE,
    EVENT_TRIPLE,
    EVENT_LONG
};
QueueHandle_t buttonEventQueue;


#define PRIORITY_BUTTON    1
#define PRIORITY_WM        1
#define PRIORITY_TTP223    1
#define PRIORITY_AUDIO     4
#define PRIORITY_STT       4
#define PRIORITY_DISPLAY   3
#define PRIORITY_PLAYBACK  4

//目前stack都在安全範圍
#define STACK_TTP223     2048 //ttp223Task 484
#define STACK_BUTTON     2048 //buttonEventTask min 372
#define STACK_AUDIO      2048 //audioTask 456
#define STACK_STT        3072 //sttTask 668
#define STACK_DISPLAY    2048 //displayTask 448
#define STACK_PLAYBACK   2048 //wavPlaybackTask 540
#define STACK_WM         2048 //WiFiManagerTask 536

//============================================================
// 系統狀態定義
//============================================================
enum SystemState {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_SENDING,
    STATE_RESULT,
    STATE_TTS_PLAYING,
    STATE_PLAYBACK,
    STATE_SAVING
};

static SystemState currentState = STATE_IDLE;

// ============================================================
// STT模組參數
// ============================================================
bool sttInitWiFi();
bool sttIsWiFiConnected();
String sttRecognize(uint8_t* audioData, size_t audioSize);
static bool wifiConnected = false;
static bool voskConnected = false;

// ============================================================
// 播放模組 (playback.h/cpp)
// ============================================================
File audioFile;               // 全域檔案物件
bool isStreamingFile = false; // 標記目前是否從檔案讀取
bool isBufferPlaying = false; // 標記目前是否從記憶體播放
uint8_t* playBuffer = nullptr;
size_t playSize = 0;
size_t playPos = 0;
bool serialplay = false;
bool dispRESULT = false;

// ============================================================
// 顯示模組 (display.h/cpp)
// ============================================================
enum DisplayState {
    DISP_IDLE,
    DISP_RECORDING,
    DISP_SENDING,
    DISP_RESULT,
    DISP_PLAYBACK,
    DISP_SAVING
};

void displayInit();
void displaySetState(DisplayState state);
void displayUpdate(const char* l1, const char* l2);
void displayRefresh();

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);
static DisplayState dispState = DISP_IDLE;
static char dispLine1[32] = "";
static char dispLine2[32] = "";
bool c_font = false;

std::vector<String> displayLines;
int currentDisplayLine = 0;
uint32_t lastUpdateMs = 0;
const int MAX_WIDTH = 120; // 留點邊距，設為 120 像素

//更新主畫面上兩列狀態訊息
void displaySetState(DisplayState state) {
#ifdef serialDSP  
    Serial.println("displaySetState");
#endif    
    dispState = state;
    switch (state) {
        case DISP_IDLE:      strcpy(dispLine1, "Voice STT");
                             switch(chatmode){ 
                              case 0:strcpy(dispLine2, "happy chat");break;
                              case 1:strcpy(dispLine2, "AI-ans....");break;
                              case 2:strcpy(dispLine2, "tran mode");break;
                              case 3:strcpy(dispLine2, "story chat");break;
                              case 4:strcpy(dispLine2, "bad chat");break;
                              case 5:strcpy(dispLine2, "lover chat");break;
                              case 6:strcpy(dispLine2, "joke mode");break;
                              case 7:strcpy(dispLine2, "cat mode");break;
                              case 8:strcpy(dispLine2, "command");break;
                             
                             }
                             break;
        case DISP_RECORDING: strcpy(dispLine1, "Recording"); strcpy(dispLine2, ""); break;
        case DISP_SENDING:   strcpy(dispLine1, "Recognizing"); strcpy(dispLine2, "..."); break;
        case DISP_RESULT:    strcpy(dispLine1, "Result:"); strcpy(dispLine2, ""); break;
        case DISP_PLAYBACK:  strcpy(dispLine1, "Playing..."); strcpy(dispLine2, ""); break;
        case DISP_SAVING:    strcpy(dispLine1, "Saved:"); strcpy(dispLine2, ""); break;
    }
    displayRefresh(); //執行刷新
}

void displayInit() {
    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.begin();
    u8g2.clearBuffer();
    displaySetState(DISP_IDLE);
}

//強制顯示訊息
void displayUpdate(const char* l1, const char* l2) {
#ifdef serialDSP  
    Serial.println("displayUpdate");
#endif    
    strncpy(dispLine1, l1, sizeof(dispLine1) - 1);
    strncpy(dispLine2, l2, sizeof(dispLine2) - 1);
    displayRefresh();
    vTaskDelay(pdMS_TO_TICKS(1500));
}
//顯示底部狀態列
void drawstatusbar(){
//show status bar
    u8g2.setFont(u8g2_font_iconquadpix_m_all);    
    if(wifiConnected){u8g2.drawGlyph(2, 63, 0x0061);} else {u8g2.drawGlyph(2, 63, 0x004D);}
    if(voskConnected){u8g2.drawGlyph(16, 63, 0x0076);} else {u8g2.drawGlyph(16, 63, 0x004F);}  
    switch(currentState){
     case STATE_IDLE:u8g2.drawGlyph(30, 63, 0x0062);
          break;  
     case STATE_RECORDING:u8g2.drawGlyph(30, 63, 0x004A);
          break;  
     case STATE_SENDING:u8g2.drawGlyph(30, 63, 0x006C);
          break;  
     case STATE_RESULT:u8g2.drawGlyph(30, 63, 0x0059);
          break;  
     case STATE_PLAYBACK:u8g2.drawGlyph(30, 63, 0x0054);
          break;  
     case STATE_SAVING:u8g2.drawGlyph(30, 63, 0x0042);
          break;  
    }
    u8g2.drawGlyph(44, 63, 0x0031+chatmode);
#ifdef ShowTime
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
#ifdef serialDSP      
        Serial.println("無法取得系統時間");
#endif        
    } else {
//    u8g2.setFont(u8g2_font_8x13_tr);
//    u8g2.setFont(u8g2_font_guildenstern_nbp_tn);//9x13 size 207
    u8g2.setFont(u8g2_font_7x14_mn);//7x14 size 292
    char timeStr[6];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    
    u8g2.drawStr(110-u8g2.getStrWidth(timeStr),63 , timeStr); // 繪製轉換後的字串
    }
#endif    
}

void displayRefresh() {  
//    Serial.println("displayRefresh");
    u8g2.clearBuffer();
    
    u8g2.setFont(u8g2_font_9x15_tr);
    u8g2.drawStr(0, 20, dispLine1);
    u8g2.drawStr(0, 38, dispLine2);
    if(!c_font){    
//    u8g2.setFont(u8g2_font_5x8_tr);
    } else {
#ifdef ShowRespCN      
    u8g2.setFont(u8g2_font_unifont_t_chinese3); // 務必先設定字型才能計算寬度    
//    u8g2.setFont(u8g2_font_wqy12_t_chinese2);      
//    u8g2.drawStr(0, 63, "你好世界");
#endif
    }
    
    drawstatusbar();
    u8g2.sendBuffer();
    c_font =false;
}

// 報時函式
void clockplay(struct tm &timeinfo) {
//    struct tm timeinfo;
    char fileName[20];
#ifdef serialDSP  
            Serial.println("[AudioTask] 開始播報...");
#endif
            // 1. 現在時間
            
            audioLoadAndPlayFromSPIFFS("/head.wav",true);
            serialplay =true;//第一個檔播完後設立連續旗標,不再重設I2S
            // 2. 小時邏輯 (簡化示範)
            if (timeinfo.tm_hour >= 10) {
                if (timeinfo.tm_hour / 10 > 1) {
                    sprintf(fileName, "/%d.wav", timeinfo.tm_hour / 10);
                    audioLoadAndPlayFromSPIFFS(fileName,true);
                }
                audioLoadAndPlayFromSPIFFS("/10.wav",true);
            }
            if (timeinfo.tm_hour % 10 != 0 || timeinfo.tm_hour == 0) {
                sprintf(fileName, "/%d.wav", timeinfo.tm_hour % 10);
                audioLoadAndPlayFromSPIFFS(fileName,true);
            }
            audioLoadAndPlayFromSPIFFS("/dot.wav",true);

            // 3. 分鐘邏輯
            int m = timeinfo.tm_min;
            if (m >= 10) {
                sprintf(fileName, "/%d.wav", m / 10);
                if((m / 10) > 1){ audioLoadAndPlayFromSPIFFS(fileName,true);}
                audioLoadAndPlayFromSPIFFS("/10.wav",true);
            }
            if (m % 10 != 0 || m == 0) {
                sprintf(fileName, "/%d.wav", m % 10);
                audioLoadAndPlayFromSPIFFS(fileName,true);
            }
            audioLoadAndPlayFromSPIFFS("/min.wav",true);
/*
            // 4. 秒數邏輯
            
            sprintf(fileName, "/%d.wav", timeinfo.tm_sec % 10);
            playWav(fileName, 0);
            playWav("/sec.wav", 0);
*/
            serialplay =false;          
#ifdef serialDSP  
            Serial.println("[AudioTask] 播報完畢");  
#endif            
}

//TTP223 觸摸模組task
void ttp223Task(void *pv) {
#ifdef serialDSP  
    Serial.println("[TTP223Task] Started");
#endif  
///////////////////////////////////
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    // 初始量測 
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("ttp223Task Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif    
///////////////////////////////////

    pinMode(TTP223_PIN, INPUT);

    bool lastStable = LOW;
    bool lastRead   = LOW;

    TickType_t debounceTick = 0;
    TickType_t pressTick = 0;
    TickType_t releaseTick = 0;

    int clickCount = 0;

    while (1)
    {
        int state = digitalRead(TTP223_PIN);
        TickType_t now = xTaskGetTickCount();

        if (state != lastRead)
        {
            debounceTick = now;
            lastRead = state;
        }

        if ((now - debounceTick) >= pdMS_TO_TICKS(DEBOUNCE_MS))
        {
            if (state != lastStable)
            {
                lastStable = state;

                if (state == HIGH) // 按下
                {
                    pressTick = now;
                }
                else // 放開
                {
                    TickType_t duration = now - pressTick;

                    if (duration >= pdMS_TO_TICKS(LONG_PRESS_MS))
                    {
                        ButtonEvent evt = EVENT_LONG;
                        if (buttonEventQueue) xQueueSend(buttonEventQueue, &evt, 0);
                        clickCount = 0;
                    }
                    else
                    {
                        clickCount++;
                        releaseTick = now;
                    }
                }
            }
        }

        if (clickCount > 0 &&
            (now - releaseTick >= pdMS_TO_TICKS(CLICK_TIMEOUT_MS)))
        {
            ButtonEvent evt;

            if (clickCount == 1) evt = EVENT_SINGLE;
            else if (clickCount == 2) evt = EVENT_DOUBLE;
            else evt = EVENT_TRIPLE;

            if (buttonEventQueue) xQueueSend(buttonEventQueue, &evt, 0);
            clickCount = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
        
///////////////////////////////////
#ifdef ChkHeap          
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current ttp223Task: %d words\n", minval);
        }       
#endif         
///////////////////////////////////                
    }
}

//使用audio tools的模組
static bool audiotask_rest = true;

void setupI2S(int sample_rate, RxTxMode mode) {
#ifdef serialDSP  
  Serial.println("--- 初始化 I2S 雙工模式 (RXTX) ---");
#endif  

    while(!audiotask_rest)vTaskDelay(pdMS_TO_TICKS(10));//等待audiotask休息

  // 使用 RXTX_MODE 同時啟動輸入與輸出
  auto cfg = i2s.defaultConfig(RXTX_MODE);
  cfg.copyFrom(info);
  
  // 腳位設定 (ESP32-C3 Super Mini)
  cfg.pin_bck = I2S_BCLK; 
  cfg.pin_ws = I2S_LRC;
  cfg.pin_data = I2S_DOUT;
  cfg.pin_data_rx = I2S_DIN;;  // DIN  (接 INMP441 SD)
  
  // C3 穩定性優化
  cfg.buffer_size = 256;
  cfg.buffer_count = 16;
  cfg.use_apll = false; // C3 不支援 APLL
  cfg.is_master = true;

  if (!i2s.begin(cfg)) {
#ifdef serialDSP  
    Serial.println("I2S 啟動失敗！");
#endif    
  } else {
#ifdef serialDSP  
    Serial.println("I2S 雙工模式已就緒 (8k/Mono)");
#endif    
  }
        
}

String urlEncode(String str) {
    String encodedString = "";
    char c, code0, code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encodedString += c;
        } else {
            code1 = (c & 0xf) + ((c & 0xf) < 10 ? '0' : 'A' - 10);
            code0 = ((c >> 4) & 0xf) + (((c >> 4) & 0xf) < 10 ? '0' : 'A' - 10);
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

//斷句處理,以免一次性餵給TTS太多失敗
int findNextPunctuation(String &text, int start) {
    int len = text.length();
    if (start >= len) return len;

    // 1. 先處理特殊換行符號，這通常是長文本最優先的斷點
    // 尋找 \n (換行) 或 \r (回車)
    int newlinePos = text.indexOf('\n', start);
    int returnPos = text.indexOf('\r', start);
    
    // 取最先出現的換行符
    int firstLF = -1;
    if (newlinePos != -1 && returnPos != -1) firstLF = min(newlinePos, returnPos);
    else if (newlinePos != -1) firstLF = newlinePos;
    else if (returnPos != -1) firstLF = returnPos;

    // 2. 定義其他標點符號
    const char* delimiters[] = {
        "。", "，", "！", "？", "；", "：", // 中文
        ".", ",", "!", "?", ";"           // 英文
    };
    int delimCount = sizeof(delimiters) / sizeof(delimiters[0]);

    int closestPos = -1;

    // 尋找最近的標點
    for (int i = 0; i < delimCount; i++) {
        int pos = text.indexOf(delimiters[i], start);
        if (pos != -1) {
            if (closestPos == -1 || pos < closestPos) {
                closestPos = pos + strlen(delimiters[i]);
            }
        }
    }

    // 如果換行符比標點更早出現，優先用換行符斷句
    if (firstLF != -1 && (closestPos == -1 || firstLF < closestPos)) {
        closestPos = firstLF + 1; // 包含換行符本身
    }

    // 3. 安全機制：長度限制 (避免 URL 爆掉)
    int maxChunk = 100; 
    if (closestPos == -1 || (closestPos - start) > maxChunk) {
        closestPos = start + maxChunk;
        if (closestPos > len) closestPos = len;
        
        // 確保不切斷 UTF-8 中文字
        while (closestPos < len && (text[closestPos] & 0xC0) == 0x80) {
            closestPos++;
        }
    }

    // 4. 極端情況防止死循環：確保每次至少前進 1 byte
    if (closestPos <= start) {
        closestPos = start + 1;
    }

    return closestPos;
}

// 封裝成獨立函式
void streamChunk(String chunk) {
    String encodedText = urlEncode(chunk);
    String url = String(TTS_SERVER) + encodedText;

    if (urlStream.begin(url.c_str(), "audio/mpeg")) {
        // 每次連線成功都要重新 begin
        dec.begin(AudioInfo(24000, 1, 16)); 
        
        #ifdef serialDSP
        Serial.println("Streaming TTS...");
        #endif

        while (urlStream.available() > 0) {
            if (stopTTS) break;
            copier.copy();
            yield();
            if (!(bool)urlStream) break;
        }
        urlStream.end();
        delay(100); 
    } else {
        #ifdef serialDSP
        Serial.println("URL 連線失敗");
        #endif
    }
}


void playTTS(String text) {
///////////////////////////////////////////////////////////////////////////    
 // 1. 先關閉 I2S 以徹底釋放 8000Hz 的時鐘鎖定
 i2s.end();     
 // 2. 以 TTS 規格 (通常是 24000Hz, 1ch) 重新啟動 I2S
 auto cfg = i2s.defaultConfig(RXTX_MODE);
 cfg.sample_rate = 24000; // 直接強制 24k
 cfg.channels = 1;
/*
     switch(chatmode){
      case 3://bad
            cfg.channels = 1;
            break;
      case 4://lover
            cfg.channels = 2;
            break;
    }
*/
 cfg.bits_per_sample = 16;
 cfg.pin_bck = I2S_BCLK; 
 cfg.pin_ws = I2S_LRC;
 cfg.pin_data = I2S_DOUT;
 cfg.pin_data_rx = I2S_DIN;;  // DIN  (接 INMP441 SD)
    
 if (!i2s.begin(cfg)) {
#ifdef serialDSP  
     Serial.println("TTS 模式啟動失敗");
#endif     
     return;
 }
        isTTSplay = true;
//    currentState = STATE_TTS_PLAYING;
 
///////////////長文字分段處理開始///////////////////////////////////////    
  int start = 0;
  String pendingChunk = ""; // 用來存儲過短的片段（如 "1."）
  
  while (start < text.length() && !stopTTS) {
    int end = findNextPunctuation(text, start);
    String chunk = text.substring(start, end);
    start = end;
    chunk.trim(); // 去除前後空白或換行
    if(chunk.length() == 0) continue;

        // 如果這一段太短（例如只有 "1." 或 "第1條："），先存起來跟下一句合併
        // 中文字 UTF-8 佔 3 bytes，所以 5 bytes 約為 1-2 個中文字或 5 個英文字
        if (chunk.length() < 5) {
            pendingChunk += chunk + " "; 
            continue; 
        }
        // 組合先前的片段與當前片段
        chunk = pendingChunk + chunk;
        pendingChunk = ""; // 清空緩衝
    
#ifdef serialDSP  
Serial.printf("斷句(L)%d %s\n", chunk.length(),chunk.c_str());
#endif    
    if (chunk.length() > 0) {
      splitPayloadByWidth(chunk);
      lastUpdateMs = millis()- LineDelay;             // 強制 displayTask 立刻顯示第一行
      currentState = STATE_RESULT;
      dispRESULT =true;   
      streamChunk(chunk);  
    }
//    while(dispRESULT)yield();
if(dispRESULT){Serial.printf("disRESULT_1=true\n");}
  }
//    while(dispRESULT)yield();
if(dispRESULT){Serial.printf("disRESULT_2=true\n");}
    
    // 如果最後還有剩下的 pendingChunk 沒播完，補播一下
    if (!stopTTS && pendingChunk.length() > 0) {
      splitPayloadByWidth(pendingChunk);
      lastUpdateMs = millis()- LineDelay;             // 強制 displayTask 立刻顯示第一行
      currentState = STATE_RESULT;
      dispRESULT =true;   
      streamChunk(pendingChunk);        
    }
if(dispRESULT){Serial.printf("disRESULT_3=true\n");}
   
///////////////長文字分段處理結束///////////////////////////////////////    

///////////////////////////////////////////////////////////////////////////        
 i2s.end(); // 播完再次關閉
 // 4. 恢復錄音用的 8k 模式
 auto recCfg = i2s.defaultConfig(RXTX_MODE);
 recCfg.sample_rate = 8000;
 recCfg.channels = 1;
 recCfg.bits_per_sample = 16;
 recCfg.pin_bck = I2S_BCLK; 
 recCfg.pin_ws = I2S_LRC;
 recCfg.pin_data = I2S_DOUT;
 recCfg.pin_data_rx = I2S_DIN;;  // DIN  (接 INMP441 SD)
 i2s.begin(recCfg);
 ///////////////////////////////////////////////////////////////////////////    
#ifdef serialDSP  
  Serial.println("TTS 播放結束");
#endif        

 isTTSplay = false;
 stopTTS = false; // 恢復狀態
   
// 4. 強制恢復錄音用的 8k (這一步非常重要)
//    i2s.setAudioInfo(AudioInfo(8000, 1, 16)); 
//    currentState = STATE_IDLE;
}
/*
void playTTS(String text) {
    AudioInfo ttsInfo(16000, 1, 16); 
    i2s.setAudioInfo(ttsInfo); 
    
    String encodedText = urlEncode(text); 
    String url = "http://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=zh-TW&q=" + encodedText;

    Serial.println("Streaming TTS: " + url);

    if (!urlStream.begin(url.c_str(), "audio/mpeg")) {
        Serial.println("URL 連線失敗");
//        setupI2S(SAMPLE_RATE, RX_MODE);
        i2s.setAudioInfo(AudioInfo(8000, 1, 16)); // 失敗也要切換回來
        return;
    }

    dec.begin(info);
    currentState = STATE_TTS_PLAYING;
    
 //   uint32_t start = millis();
 //   while (urlStream && millis() - start < 30000) {
     while (urlStream.available() > 0) {
       copier.copy();
         yield(); // 餵看門狗
   }
    
    urlStream.end();
    i2s.setAudioInfo(AudioInfo(8000, 1, 16)); // 強制恢復錄音用的 8k
    currentState = STATE_IDLE;
//    setupI2S(SAMPLE_RATE, RX_MODE);
    Serial.println("TTS playback done");
}
*/

// ============================================================
// 音訊模組 (audio.h/cpp)
// ============================================================
enum AudioState {
    AUDIO_IDLE,
    AUDIO_RECORDING,
    AUDIO_READY,
    AUDIO_PLAYING
};

void audioInit();
bool audioSaveToSPIFFS(const char* filename, uint8_t* data, size_t size);
const char* audioGetNextFile();

static AudioState audioState = AUDIO_IDLE;
static int currentFileIndex = 0;
static char latestFile[32] = "";



void audioInit() {
//    setupI2S(SAMPLE_RATE, RX_MODE);  // 使用 AudioTools 初始化 8kHz 錄音模式
    if (!SPIFFS.begin(true)) {
#ifdef serialDSP  
        Serial.println("SPIFFS mount failed");
#endif        
    } else {
  unsigned int totalBytes = SPIFFS.totalBytes();
  unsigned int usedBytes = SPIFFS.usedBytes();
  unsigned int freeBytes = totalBytes - usedBytes;
#ifdef serialDSP    
  Serial.println("--SPIFFS info--");
  Serial.printf("total:%u used:%u free:%u\n",totalBytes,usedBytes,freeBytes);    
#endif  
    }
}

bool audioSaveToSPIFFS(const char* filename, uint8_t* data, size_t size) {
    // 参数验证
    if (!filename || filename[0] == '\0') {
#ifdef serialDSP  
        Serial.println("Invalid filename");
#endif        
        return false;
    }
    if (!data || size == 0) {
#ifdef serialDSP  
        Serial.println("Invalid data or size");
#endif        
        return false;
    }
#ifdef serialDSP  
    Serial.printf("Filename:%s length=%d\n",filename,size);
#endif    
    
    // 删除可能存在的同名文件，避免文件系统问题
    SPIFFS.remove(filename);
    
    File file = SPIFFS.open(filename, FILE_WRITE);
    if (!file) {
#ifdef serialDSP  
        Serial.println("Failed to open file for writing");
#endif        
        return false;
    }

    // 1. 直接產生並寫入 Header (不需 malloc)
    uint32_t sampleRate = 8000; // 根據你的設定
    uint16_t channels = 1;
    uint16_t bitsPerSample = 16; 
    uint32_t fileSize = size + 36; // 如果是 16bit 直接存
    
    // 寫入簡單的 WAV Header (44 bytes)
    file.write((const uint8_t*)"RIFF", 4);
    file.write((const uint8_t*)&fileSize, 4);
    file.write((const uint8_t*)"WAVEfmt ", 8);
    uint32_t fmtLen = 16; file.write((const uint8_t*)&fmtLen, 4);
    uint16_t fmtTag = 1; file.write((const uint8_t*)&fmtTag, 2);
    file.write((const uint8_t*)&channels, 2);
    file.write((const uint8_t*)&sampleRate, 4);
    uint32_t byteRate = sampleRate * channels * (bitsPerSample/8);
    file.write((const uint8_t*)&byteRate, 4);
    uint16_t blockAlign = channels * (bitsPerSample/8);
    file.write((const uint8_t*)&blockAlign, 2);
    file.write((const uint8_t*)&bitsPerSample, 2);
    file.write((const uint8_t*)"data", 4);
    file.write((const uint8_t*)&size, 4);
    
    size_t written = 0;
    // 分块写入以减少峰值内存使用
    const size_t chunkSize = 1024;
    while (written < size) {
        size_t toWrite = min(chunkSize, size - written);
        file.write(data + written, toWrite);
        written += toWrite;
    }    
    
    file.close();
#ifdef serialDSP  
    Serial.printf("Saved %d bytes to %s\n", written, filename);
#endif    
    strcpy(latestFile, filename);
    return true;
}

bool audioLoadAndPlayFromSPIFFS(const char* filename,bool prate) {
    currentState = STATE_PLAYBACK;
    if(!serialplay){    
    setupI2S(8000, TX_MODE);  // 切換到 8kHz 播放模式
      }
//    memset(recordBuffer, 0, recordCapacity); 

    audioFile = SPIFFS.open(filename, FILE_READ);
    size_t fileSize = audioFile.size();    
#ifdef serialDSP  
        Serial.printf("File:%s (%d)\n",filename,fileSize);
#endif        

    if (!audioFile || fileSize <= 44) {
#ifdef serialDSP  
        Serial.println("Failed to open file or file empty");
#endif        
        return false;
    }
    fileSize -=44;
    audioFile.seek(44); // 跳過 WAV 頭部
    // 2. 檢查資料是否會超出緩衝區
    if (fileSize > recordCapacity) {
#ifdef serialDSP  
        Serial.println("警告：檔案音訊超出緩衝區，將進行截斷讀取");
#endif        
        fileSize = recordCapacity;
    }
    // 3. 一次性讀入 recordBuffer 的開頭
    recordSize = audioFile.read(recordBuffer, fileSize);
#ifdef serialDSP  
        Serial.printf("loadFileSize(%d)\n",recordSize);
#endif        
    audioFile.close();


//////////////////////////////////////

    int32_t minval =0;
    int32_t maxval =0;
    int32_t raw_sample;
    int fade_count = 0;
    float Volume = 1.0f; // 設定音量為 70% (0.0 ~ 1.0)
    const int FADE_IN_STEPS = 300; 
    const int FADE_OUT_STEPS = 300; 
    int current_sample_idx = 0;
    int16_t* samples = (int16_t*)recordBuffer;
    int samples_count = recordSize / BYTES_PER_SAMPLE;
        for (int i = 0; i < samples_count; i++) {
            // 2. 直接讀取每一個樣本，不跳聲道
            if(prate){
            raw_sample = (int32_t)samples[i] << 1; 
            } else {raw_sample = (int32_t)samples[i];}
#ifdef serialDSP  
      minval = min(minval, raw_sample);
      maxval = max(maxval, raw_sample);
#endif      
            float current_vol = Volume;            
            // 淡入邏輯
            if (fade_count < FADE_IN_STEPS) {
                current_vol *= ((float)fade_count / FADE_IN_STEPS);
                fade_count++;
            }
            // --- 淡出邏輯 (選配) ---
            int remaining = samples_count - current_sample_idx;
            if (remaining < FADE_OUT_STEPS) {
               current_vol *= ((float)remaining / FADE_OUT_STEPS);
            }
            
        raw_sample *= current_vol;
        // 限制幅度防止爆音
        if (raw_sample > 32767) raw_sample = 32767;
        else if (raw_sample < -32768) raw_sample = -32768;        

            // 套用音量並存入寫入緩衝區
            samples[i] = (int16_t)raw_sample;
            current_sample_idx++;
#ifdef serialDSP  
//      minval = min(minval, (int32_t)samples[i]);
//      maxval = max(maxval, (int32_t)samples[i]);
#endif
        }
#ifdef serialDSP  
      Serial.printf("maxval:%ld , minval:%ld\n",maxval,minval);  
#endif

/////使用錄音的那一段放大/////////////////////
/*
    int32_t minval =0;
    int32_t maxval =0;
    
    int16_t* samples = (int16_t*)recordBuffer;
    int sampleCount = recordSize / BYTES_PER_SAMPLE;
    static int32_t last_val = 0;

    for (int i = 0; i < sampleCount; i++) {              
        // 使用你覺得自然的 4 倍增益 (左移 2 位)
        int32_t val = (int32_t)samples[i]; 
      minval = min(minval, val);
      maxval = max(maxval, val);
        // 簡易低通濾波 (減少高頻沙沙聲)
        val = (val + last_val) / 2; 
        last_val = val;
        // 限制幅度防止爆音
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;        
        samples[i] = (int16_t)val;      //回存              
       }
      Serial.printf("maxval:%ld , minval:%ld\n",maxval,minval);  
*/   
//////高通濾波 + 雙端淡入淡出////////////////
/*
    int16_t* ptr = (int16_t*)recordBuffer;
    int samples = recordSize / 2;

    // --- 1. 一階高通濾波 (移除所有看不見的直流與次低頻) ---
    float alpha = 0.99f; // 濾波係數
    float prev_x = 0, prev_y = 0;
    for (int i = 0; i < samples; i++) {
        float x = (float)ptr[i];
        float y = alpha * (prev_y + x - prev_x);
        prev_x = x;
        prev_y = y;
        ptr[i] = (int16_t)y;
    }

    // --- 2. 雙端淡入淡出 (消除開頭與結尾的報音/直流衝擊) ---
    const int FADE_STEPS = 500; // 約 62.5ms (8kHz 下)
    for (int i = 0; i < FADE_STEPS && i < samples; i++) {
        // 開頭淡入
        float fade_in = (float)i / FADE_STEPS;
        ptr[i] = (int16_t)(ptr[i] * fade_in);
        
        // 結尾淡出 (同步處理)
        int tail_idx = samples - 1 - i;
        if (tail_idx >= 0) {
            ptr[tail_idx] = (int16_t)(ptr[tail_idx] * fade_in);
        }
    }
*/        
////撐得稍微久一點了///////中心化 + 淡入///////////////
/*
    int16_t* ptr = (int16_t*)recordBuffer;
    int samples = recordSize / 2;

    // A. 移除直流偏移 (防止擴大機判斷為直流短路)
    int32_t sum = 0;
    for (int i = 0; i < samples; i++) sum += ptr[i];
    int16_t avg = (int16_t)(sum / samples);
    for (int i = 0; i < samples; i++) ptr[i] -= avg;

    // B. 淡入處理 (徹底消除開頭「報音」)
    const int FADE_STEPS = 400; // 約 50ms
    for (int i = 0; i < FADE_STEPS && i < samples; i++) {
        ptr[i] = (int16_t)(ptr[i] * ((float)i / FADE_STEPS));
    }
*/    
//////////////////////////////////////
/*
        float Volume = 0.7f; // 設定音量為 70% (0.0 ~ 1.0)
        int fade_count = 0;
        const int FADE_IN_STEPS = 300; 
        int bytes_read = recordSize;
        int samples_count = bytes_read / 2; // 每個樣本 2 bytes (16-bit)
        int16_t* ptr = (int16_t*)recordBuffer;

        for (int i = 0; i < samples_count; i++) {
            // 2. 直接讀取每一個樣本，不跳聲道
            int16_t raw_sample = ptr[i]; 
            float current_vol = Volume;
            
            // 淡入淡出邏輯
            if (i < FADE_IN_STEPS) {
              current_vol *= ((float)i / FADE_IN_STEPS);
            } 
//            else if (i > (samples_count - FADE_IN_STEPS)) {
//              current_vol *= ((float)(samples_count - i) / FADE_IN_STEPS);
//             }
            // 套用音量並存入寫入緩衝區
            ptr[i] = (int16_t)(raw_sample * current_vol);
        }
*/
//////////////////////////////////////    
    // 4. 重置播放位置並啟動播放標誌
    playPos = 0;        
    currentState = STATE_PLAYBACK;
    displaySetState(DISP_PLAYBACK);
    isBufferPlaying = true;//啟動wavPlaybackTask
    
#ifdef serialDSP  
    Serial.printf("WAV 載入成功: 跳過 44B, 實際載入 %d bytes\n", recordSize); 
#endif
            //等待播放完成才離開
            while (isBufferPlaying) {
              yield();
            }    
            vTaskDelay(pdMS_TO_TICKS(100)); 
    return true;
}

void wavPlaybackTask(void* pvParameters) {
#ifdef serialDSP  
    Serial.println("[WAVPlaybackTask] Started");
#endif    


///////////////////////////////////
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    // 初始量測 
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("wavPlaybackTask Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif
///////////////////////////////////


    while (true) {
        if (!isBufferPlaying) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
//       if (recordSize > 0 && playPos == 0) {
//          int16_t silence[128] = {0}; 
//          i2s.write((uint8_t*)silence, sizeof(silence)); // 寫入一段靜音預熱       
//       }

       if (isBufferPlaying && playPos < recordSize) {
            size_t bytesToRead = min((size_t)512, (size_t)(recordSize - playPos));
//            bytesToRead &= ~1;
#ifdef serialDSP  
            Serial.printf("Playing: playPos=%d, playSize=%d\n", playPos, bytesToRead);
#endif
//            int16_t* samples = (int16_t*)(recordBuffer + playPos);
//            size_t written = i2sOut.write((uint8_t*)samples, bytesToRead);
            size_t written = i2s.write(recordBuffer + playPos, bytesToRead);
            if(written > 0){playPos += written;} else {yield();}
        }
        else {
          // 播放結束前，補一段靜音
       
//        int16_t silence = 0;
//        for(int i=0; i<800; i++) i2s.write((uint8_t*)&silence, 2); 

            vTaskDelay(pdMS_TO_TICKS(100)); // 確保硬體緩衝播放完畢

/*
///////////////////////////////////////////////////////////////////////////        
 i2s.end(); // 播完再次關閉
    periph_module_reset(PERIPH_I2S1_MODULE); 
    vTaskDelay(pdMS_TO_TICKS(20));
 // 4. 恢復錄音用的 8k 模式
 auto recCfg = i2s.defaultConfig(RXTX_MODE);
 recCfg.sample_rate = 8000;
 recCfg.channels = 1;
 recCfg.bits_per_sample = 16;
 recCfg.pin_bck = I2S_BCLK; 
 recCfg.pin_ws = I2S_LRC;
 recCfg.pin_data = I2S_DOUT;
 recCfg.pin_data_rx = I2S_DIN;;  // DIN  (接 INMP441 SD)
 i2s.begin(recCfg);
 ///////////////////////////////////////////////////////////////////////////    
*/
            
            // 播放結束
#ifdef serialDSP  
            Serial.printf("Playback end: playPos=%d, playSize=%d\n", playPos, recordSize);
#endif
            playPos = 0;
            isBufferPlaying = false;
//            i2s.flush();
            currentState = STATE_IDLE; // 恢復狀態，audioTask 會自動重新開始工作              
            displaySetState(DISP_IDLE);
        }

///////////////////////////////////
#ifdef ChkHeap          
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current wavPlaybackTask: %d words\n", minval);
        }       
#endif         
///////////////////////////////////        
        
    }
}
/*
void bufferPlaybackStart(uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return;
    
//    setupI2S(SAMPLE_RATE, TX_MODE);
    
#ifdef serialDSP  
    Serial.printf("Playback: sample_rate=%d, size=%d, isWAV=%d\n", SAMPLE_RATE, size, memcmp(data, "RIFF", 4) == 0);
#endif
    
    // 檢查是否為 WAV 格式
//    if (memcmp(data, "RIFF", 4) == 0) {
//        playPos = 44; // 跳過 WAV header
//    } else {
        playPos = 0;
//   }
    
    playBuffer = data;
    playSize = size;
    isBufferPlaying = true;
    currentState = STATE_PLAYBACK;
    
#ifdef serialDSP  
    Serial.printf("BufferPlayBack started: %d bytes, playSize=%d, playPos=%d\n", size, playSize, playPos);
#endif
}
*/
const char* audioGetNextFile() {
    static char filename[32];
    snprintf(filename, sizeof(filename), RECORD_FILENAME, currentFileIndex);
    // 确保文件名以斜杠开头（SPIFFS 要求）
    if (filename[0] != '/') {
        // 向后移动字符串为斜杠腾出空间
        memmove(filename + 1, filename, strlen(filename) + 1);
        filename[0] = '/';
    }
    // 更新索引供下一次使用，並循環覆蓋
    nextFileIndex = (nextFileIndex + 1) % MAX_FILES; 
    currentFileIndex = (currentFileIndex + 1) % MAX_FILES;
    return filename;
}


bool sttInitWiFi() {
    WiFi.mode(WIFI_STA);
//    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.begin();
    WiFi.setTxPower(WIFI_POWER_8_5dBm);     
#ifdef serialDSP  
    Serial.print("Connecting to WiFi");
#endif    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
#ifdef serialDSP  
        Serial.print(".");
#endif        
        attempts++;
    }
#ifdef serialDSP  
    Serial.println();
#endif    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
#ifdef serialDSP  
        Serial.println("WiFi connected");
        Serial.println(WiFi.localIP());
#endif        
        return true;
    } else {
        // 連不上，開啟手機配網監聽，但不阻塞，直接回傳 false
        WiFi.beginSmartConfig(); //呼叫smartcnf啟動task等待配網
#ifdef serialDSP  
        Serial.println("WiFi connection failed");
#endif          
        return false;
    }
}

bool checkVoskConnection() {
    if (!sttIsWiFiConnected()) return false;
    
    HTTPClient http;
    http.begin(VOSK_SERVER);
    http.addHeader("Content-Type", "audio/wav");
    
    int httpCode = http.POST((uint8_t*)"", 0);
    bool connected = (httpCode != 0 && httpCode != -1);
    http.end();
    
    return connected;
}

void WiFiManagerTask(void *pvParameters) {
///////////////////////////////////
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    // 初始量測 
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("WiFiManagerTask Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif
///////////////////////////////////


    for (;;) {
        // 如果正在 SmartConfig，檢查是否成功
        if (WiFi.smartConfigDone()) {
            Serial.println("配網成功！");
            displayUpdate("WiFi", "CFG OK.");
            vTaskDelay(pdMS_TO_TICKS(2000));
            WiFi.stopSmartConfig();
            WiFi.mode(WIFI_STA);
            currentState = STATE_IDLE; // 恢復狀態，audioTask 會自動重新開始工作              
            displaySetState(DISP_IDLE);
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // 每半秒檢查一次就好，完全不卡
///////////////////////////////////
#ifdef ChkHeap  
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current WiFiManagerTask: %d words\n", minval);
        }
#endif        
///////////////////////////////////        
        
    }
}


void toggleNetworkConnection(bool isAsync) {
    // 如果已連線 -> 斷開
    if (wifiConnected && WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true, false);
        wifiConnected = false;
        voskConnected = false;
        displayUpdate("WiFi", "OFF");
        vTaskDelay(pdMS_TO_TICKS(100));
        displaySetState(DISP_IDLE);
        return;
    }

    // 如果未連線 -> 開始連線程序
    if(WiFi.status() != WL_CONNECTED){
      displayUpdate("WiFi", "STARTING");
      WiFi.mode(WIFI_AP_STA); // 混合模式才能配網
      WiFi.setTxPower(WIFI_POWER_8_5dBm);     
      WiFi.begin();           // 嘗試用 Flash 裡的舊帳密連線
      }
    // 等待 5 秒確認是否能自動連上
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 10) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        displayUpdate("WiFi", "ON");
        wifiConnected = true;
            lastActivityMs = millis();
            voskConnected = checkVoskConnection();
            if (voskConnected) {
#ifdef serialDSP  
                Serial.println("VOSK OK");
#endif                
                displayUpdate("VOSK", "OK");
        vTaskDelay(pdMS_TO_TICKS(100));
        displaySetState(DISP_IDLE);
            } else {
#ifdef serialDSP  
                Serial.println("VOSK FAIL");
#endif                
                displayUpdate("VOSK", "FAIL");
        vTaskDelay(pdMS_TO_TICKS(100));
        displaySetState(DISP_IDLE);
            }


        
    } else {
        // 如果 5 秒連不上，開啟手機配網模式 (SmartConfig)
        displayUpdate("WiFi", "SC READY"); // 螢幕顯示進入配網模式
        WiFi.beginSmartConfig(); // 這行是非同步的，不會卡死
        Serial.println("等待手機 App (EspTouch) 發送帳密...");
    }
}

/*
void toggleNetworkConnection(bool isAsync) {
  
    if (wifiConnected) {
        WiFi.disconnect();
        wifiConnected = false;
        voskConnected = false;
#ifdef serialDSP  
        Serial.println("WiFi disconnected");
#endif        
        displayUpdate("WiFi", "OFF");
    } else {
        if (sttInitWiFi()) {
            wifiConnected = true;
#ifdef serialDSP  
            Serial.println("WiFi ON");
#endif            
            displayUpdate("WiFi", "ON");
            delay(1000);
            lastActivityMs = millis();
            voskConnected = checkVoskConnection();
            if (voskConnected) {
#ifdef serialDSP  
                Serial.println("VOSK OK");
#endif                
                displayUpdate("VOSK", "OK");
            } else {
#ifdef serialDSP  
                Serial.println("VOSK FAIL");
#endif                
                displayUpdate("VOSK", "FAIL");
            }
            
        } else {
#ifdef serialDSP  
            Serial.println("WiFi FAIL");
#endif            
            displayUpdate("WiFi", "FAIL");
        }
    }
    delay(1500);
    displaySetState(DISP_IDLE);
}
*/
bool sttIsWiFiConnected() {
    return wifiConnected && (WiFi.status() == WL_CONNECTED);
}

String sttRecognize(uint8_t* audioData, size_t audioSize) {
    if (!sttIsWiFiConnected()) {
#ifdef serialDSP  
        Serial.println("WiFi not connected");
#endif        
        return "";
    }
//  Serial.printf("Free Heap before: %d\n", ESP.getFreeHeap());
    HTTPClient http;
    // 2. 拼接網址，例如 http://192.168.1.100
    
    String fullUrl = String(VOSK_SERVER);
    switch(chatmode){
      case 1://chat
            fullUrl +="?mode=ans";
            break;
      case 2://chat
            fullUrl +="?mode=tran";
            break;
      case 3://chat
            fullUrl +="?mode=story";
            break;
      case 4://chat
            fullUrl +="?mode=bad";
            break;
      case 5://chat
            fullUrl +="?mode=lover";
            break;
      case 6://chat
            fullUrl +="?mode=joke";
            break;
      case 7://chat
            fullUrl +="?mode=cat";
            break;
      case 8://chat
            fullUrl +="?mode=command";
            break;
    }
#ifdef serialDSP  
    Serial.println(fullUrl);
#endif    
    http.begin(fullUrl); // 使用拼接後的網址    
//    http.begin(VOSK_SERVER);
    http.setTimeout(resonsetimeout); // 增加到 15 秒
    http.addHeader("Content-Type", "audio/l16; rate=8000; channels=1");


    int httpCode = http.POST(audioData, audioSize);

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();
#ifdef serialDSP  
        Serial.println("VOSK Response: " + payload);
#endif        

        int textStart = payload.indexOf("\"text\":\"");
        if (textStart == -1) textStart = payload.indexOf("\"text\": \"");
        if (textStart != -1) {
            int textEnd = payload.indexOf("\"", textStart + 8);
            if (textEnd != -1) {
                return payload.substring(textStart + 8, textEnd);
            }
        }

        int resultStart = payload.indexOf("\"result\":[");
        if (resultStart != -1) {
            int resultEnd = payload.indexOf("]", resultStart);
            if (resultEnd != -1) {
                return payload.substring(resultStart + 10, resultEnd);
            }
        }
// Serial.printf("Free Heap after: %d\n", ESP.getFreeHeap());        
        return payload;
    } else {
#ifdef serialDSP  
        Serial.printf("HTTP error: %d\n", httpCode);
#endif        
        http.end();
// Serial.printf("Free Heap after: %d\n", ESP.getFreeHeap());
        return "";
    }
}

void buttonEventTask(void *pvParameters)
{
#ifdef serialDSP    
    Serial.println("[ButtonEventTask] started");
#endif    
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    // 初始量測 
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("ButtonEventTask Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif
    
    ButtonEvent evt;
    while (1)
    {
        if (buttonEventQueue && xQueueReceive(buttonEventQueue, &evt, pdMS_TO_TICKS(100)))
        {
            switch (evt)
            {
                case EVENT_SINGLE:
#ifdef serialDSP      
                Serial.println("單擊");
#endif                
//================================
                    if (isTTSplay)stopTTS = true;
                                                                
                    if (currentState == STATE_IDLE) {
                        setupI2S(8000, RX_MODE);                        
                        recordSize = 0;
                        currentState = STATE_RECORDING;
                        displaySetState(DISP_RECORDING);
#ifdef serialDSP  
                        Serial.println("Recording started");
#endif                        
                    }
                    
//================================                                                       
                    break;

                case EVENT_DOUBLE:
#ifdef serialDSP     
                    Serial.println("雙擊");
#endif

//================================
//切換chat 模式 chatmode
                    if (currentState == STATE_IDLE && (millis() - lastRecordtime) >= 30000){
                      chatmode++;
                      if(chatmode >= 9)chatmode=0;
                      displaySetState(DISP_IDLE);
                    } else {
//================================
                      if (currentState == STATE_IDLE && recordSize > 44) {
                        setupI2S(8000, TX_MODE);
                        playPos = 0;    
                        isBufferPlaying = true;
                        currentState = STATE_PLAYBACK;
                        displaySetState(DISP_PLAYBACK);
                      } else {
                        displaySetState(DISP_IDLE);                      
                      }
                    }
//================================                                                       

                    break;
                    
                case EVENT_TRIPLE:
#ifdef serialDSP      
                        Serial.println("三擊");
#endif

//================================
                    
//存檔並播放

                    if (currentState == STATE_IDLE && recordSize > 44) {
                            const char* filename = audioGetNextFile();
                            if (audioSaveToSPIFFS(filename, recordBuffer, recordSize)) {
                                char info[32];
                                snprintf(info, sizeof(info), "%s", filename);
                                displayUpdate("Saved:", info);
                                delay(500);
                                currentState = STATE_PLAYBACK;
                                displaySetState(DISP_PLAYBACK);
                                audioLoadAndPlayFromSPIFFS(filename,false);
                            }
                    }
//================================                                                       

                    break;

                case EVENT_LONG:
#ifdef serialDSP      
                    Serial.println("長按");
#endif                    

//================================
                    if (currentState == STATE_IDLE) {
                        toggleNetworkConnection(true);
                    }
//================================                                                       


/*
                    powerOffAnimation();
                    esp_deep_sleep_enable_gpio_wakeup(1 << 2, ESP_GPIO_WAKEUP_GPIO_HIGH);                      
                    esp_deep_sleep_start();
*/                    
                    break;
            }
        }
///////////////////////////////////
#ifdef ChkHeap  
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current ButtonEventTask: %d words\n", minval);
        }
#endif       
///////////////////////////////////        
    }
}

/*
void audioTask(void* parameter) {
#ifdef serialDSP    
    Serial.println("[AudioTask] started");
#endif    
    sample_t sampleBuffer[BLOCK_SIZE];
    size_t bytesRead = 0;
    uint32_t recordStartTime = 0;
    uint32_t lastDebugTime = 0;

    while (true) {
        // 使用 I2SStream 讀取麥克風資料
        bytesRead = i2sIn.readBytes((uint8_t*)sampleBuffer, sizeof(sampleBuffer));
        
       if (bytesRead == 0) {
            yield(); // 沒讀到資料時，讓出 CPU 給系統處理 WiFi 或其他背景任務
            continue;
        }
       if (bytesRead > 0) {
            // 計算音量 (取最大值)
//            int32_t maxVal = 0;
            size_t sampleCount = bytesRead / BYTES_PER_SAMPLE;
            for (size_t i = 0; i < sampleCount; i++) {
              
        int32_t amplified = sampleBuffer[i] << 4; 
        if (amplified > 32767) amplified = 32767;
        else if (amplified < -32768) amplified = -32768;
        sampleBuffer[i] = (int16_t)amplified;
              
//                int32_t val = sampleBuffer[i];
//                if (val < 0) val = -val;
//                if (val > maxVal) maxVal = val;
            }

//            static int plotCounter = 0;
//            if (++plotCounter >= 32) { 
                // Serial Plotter 格式要求：直接輸出數值並換行
                // 如果要畫多條線，用逗號隔開：Serial.printf("%d,%d\n", val1, val2);
//                Serial.printf("%d\n",sampleBuffer[0]); 
//                plotCounter = 0;
//           }

            
            
            // 每500ms輸出一次
//            if (millis() - lastDebugTime >= 500) {
//                lastDebugTime = millis();
//                Serial.printf("[%lu] INMP441: max=%ld, bytes=%d\n", millis(), maxVal, bytesRead);
//            }

            // 如果正在錄音，儲存資料
            if (currentState == STATE_RECORDING) {
                if (recordStartTime == 0) {
                    recordStartTime = millis();
                    recordSize = 0;
#ifdef serialDSP  
                    Serial.printf("Recording started: sample_rate=%d, capacity=%d\n", SAMPLE_RATE, recordCapacity);
#endif
                }

                float duration = (millis() - recordStartTime) / 1000.0;
                if (duration >= MAX_RECORD_SEC) {
#ifdef serialDSP  
                    Serial.printf("Max recording time reached, size: %d bytes\n", recordSize);
#endif                    
                    currentState = STATE_SENDING;
                    displaySetState(DISP_SENDING);
                    recordStartTime = 0;
                } else {
                    if (recordSize + bytesRead <= recordCapacity) {
                        memcpy(recordBuffer + recordSize, sampleBuffer, bytesRead);
                        recordSize += bytesRead;
                    }
                }
            }
        } else {
            if (currentState == STATE_RECORDING) {
                recordStartTime = 0;
            }
        }
*/
void audioTask(void* parameter) {
#ifdef serialDSP    
    Serial.println("[AudioTask] started");
#endif    
    
///////////////////////////////////
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("audioTask Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif    
///////////////////////////////////

    sample_t sampleBuffer[BLOCK_SIZE];
    size_t bytesRead = 0;
    uint32_t recordStartTime = 0;
    uint32_t lastDebugTime = 0;
    static int32_t last_val = 0;
    while (true) {
/*      
            // 每500ms輸出一次
            if (millis() - lastDebugTime >= 500) {
               lastDebugTime = millis();
               Serial.print(".");
            }
*/
        
        if (!(currentState == STATE_RECORDING)) {
           audiotask_rest =true;
           recordStartTime = 0; // 重置錄音計時
            vTaskDelay(pdMS_TO_TICKS(50)); // 進入播放模式時，讓出 CPU 時間
            continue; 
        }                 
//只有在錄音狀態才會進入以下
        audiotask_rest =false;    
        
        if (recordStartTime == 0) { //開始錄音
            last_val = 0;
            recordStartTime = millis();
            recordSize = 0;
#ifdef serialDSP  
            Serial.printf("Recording started: sample_rate=%d, capacity=%d\n", SAMPLE_RATE, recordCapacity);
#endif
            }

        if (millis() - recordStartTime < max_rec_ms && recordSize < recordCapacity) {
          uint32_t to_read = min((uint32_t)256, recordCapacity - recordSize);
                  
        
        // 使用 I2SStream 讀取麥克風資料
           bytesRead = i2s.readBytes(recordBuffer + recordSize, to_read);
        
           if (bytesRead > 0) {            
            int16_t* samples = (int16_t*)(recordBuffer + recordSize);
            int sampleCount = bytesRead / BYTES_PER_SAMPLE;

     // --- 診斷印出：只在錄音剛開始時印出前幾個樣本 ---
//    if (recordSize < 512) { 
//        Serial.printf("Raw Sample Data: %d, %d, %d\n", samples[0], samples[1], samples[2]);
//    }
            
            int32_t minval =0;
            int32_t maxval =0;
            for (int i = 0; i < sampleCount; i++) {
              
//              samples[i] =  samples[i] << 4; 

#ifdef serialDSP  
//      minval = min(minval, (int32_t)samples[i]);
//      maxval = max(maxval, (int32_t)samples[i]);
#endif      

        // 使用你覺得自然的 4 倍增益 (左移 2 位)
        int32_t val = (int32_t)samples[i] * 91; //經過實際準確的測量值需要放大到91倍
        // 簡易低通濾波 (減少高頻沙沙聲)
 //       val = (val + last_val) / 2; 
 //       last_val = val;
        // 限制幅度防止爆音
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;        
        samples[i] = (int16_t)val;      //回存
#ifdef serialDSP  
      minval = min(minval, (int32_t)samples[i]);
      maxval = max(maxval, (int32_t)samples[i]);
#endif      
              
              }
              
            recordSize += bytesRead;
#ifdef serialDSP  
//    Serial.printf("recording data recordSize: %d + %d bytes\n", recordSize,bytesRead); 
      Serial.printf("maxval:%ld , minval:%ld\n",maxval,minval);  
#endif
                   
            }            
           } else {
#ifdef serialDSP  
                    Serial.printf("Max recording time reached, size: %d bytes\n", recordSize);
#endif                    
                    lastRecordtime = millis();
                    currentState = STATE_SENDING;
                    displaySetState(DISP_SENDING);
                    recordStartTime = 0;

            
           }
       yield();

///////////////////////////////////
#ifdef ChkHeap  
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current audioTask: %d words\n", minval);
        }
#endif        
///////////////////////////////////

     }
}
            
void splitPayloadByWidth(String text) {
    displayLines.clear();    
    #ifdef ShowRespCN    
        u8g2.setFont(u8g2_font_unifont_t_chinese3); 
    #endif    

    String currentLine = "";
    
    for (int i = 0; i < text.length(); ) {
        int len = 1;
        // 1. 判斷 UTF-8 位元組長度
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        
        String word = text.substring(i, i + len);

        // === 關鍵修正：處理強制換行符號 ===
        if (word == "\n" || word == "\r") {
            displayLines.push_back(currentLine); // 遇到換行，直接存入目前行
            currentLine = "";                   // 清空，準備下一行
            
            // 處理 Windows 的 \r\n 情況
            if (word == "\r" && (i + 1) < text.length() && text[i + 1] == '\n') {
                i++; 
            }
        } else {
            // === 原本的寬度檢查邏輯 ===
            String tempLine = currentLine + word;
            if (u8g2.getUTF8Width(tempLine.c_str()) > MAX_WIDTH) {
                displayLines.push_back(currentLine); 
                currentLine = word; 
            } else {
                currentLine = tempLine;
            }
        }
        i += len;
    }
    
    // 存入最後殘留的一行
    if (currentLine.length() > 0) {
        displayLines.push_back(currentLine);
    }
    
    currentDisplayLine = 0;
}

void sttTask(void* parameter) {
#ifdef serialDSP    
    Serial.println("[STTTask] started");
#endif    
///////////////////////////////////
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("sttTask Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif    
///////////////////////////////////

    while (true) {
        if (currentState == STATE_SENDING) {
#ifdef serialDSP  
            Serial.println("Sending to VOSK...");
#endif            

            if (recordSize > 44) {
              
                if (!sttIsWiFiConnected()) {
                    displayUpdate("No WiFi", "Skip STT");
#ifdef serialDSP  
                    Serial.println("No WiFi, skipping STT");
#endif                    
                    currentState = STATE_IDLE;
                    displaySetState(DISP_IDLE);
                    resultStartTime = millis();
                    continue;
                }

                    String result = sttRecognize(recordBuffer, recordSize);
                    c_font =true; //中文字型顯示
                    if (result.length() > 0) {

#ifdef serialDSP  
                        Serial.printf("VOSK Result:(L)%d %s\n", result.length(),result.c_str());   
#endif                        
//                        splitPayloadByWidth(result);         // 依據寬度自動切成好幾行                       
//                        lastUpdateMs = millis()- LineDelay;             // 強制 displayTask 立刻顯示第一行
//                        currentState = STATE_RESULT;
                        
//                        if(chatmode != 1 || result.length() > 0)
                        // 自動播放 TTS
                        result.trim();
                        if(result.length() > 0)playTTS(result);
                        
                        lastRecordtime = millis();//重置
                        currentState = STATE_IDLE;
                        displaySetState(DISP_IDLE);
                       
                    } else {
                        displayUpdate("Result:", "No text");
                        currentState = STATE_IDLE;
                        displaySetState(DISP_IDLE);
                    }
                
            } else {
                displayUpdate("Result:", "No audio");
                currentState = STATE_IDLE;
                displaySetState(DISP_IDLE);
            }

 //           currentState = STATE_IDLE;
 //           displaySetState(DISP_IDLE);
            resultStartTime = millis();
            
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 使用 FreeRTOS 的 Delay
///////////////////////////////////
#ifdef ChkHeap  
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current sttTask: %d words\n", minval);
        }
#endif        
///////////////////////////////////

    }
}

void displayTask(void* parameter) {
#ifdef serialDSP    
    Serial.println("[DisplayTask] started");
#endif    
///////////////////////////////////
#ifdef ChkHeap  
    UBaseType_t uxHighWaterMark;      //monitor stack
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    int minval =(int)uxHighWaterMark;
    Serial.printf("displayTask Start - Free Stack: %ld words\n", uxHighWaterMark);
#endif    
///////////////////////////////////

    uint32_t lastDispRefresh = 0;
//    uint32_t lastDebug = 0;

    while (true) {

/*        
        if (millis() - lastDebug >= 2000) {
#ifdef serialDSP  
            Serial.printf("DisplayTask running, state=%d\n", currentState);
#endif
            lastDebug = millis();
        }
*/

        if (currentState == STATE_RESULT) {            
            if (currentDisplayLine < displayLines.size()) {
                // 模擬逐行顯示邏輯 (這裡需要根據你的螢幕 Library 實作)
                // 範例：如果文字很長，我們分段顯示
               if (millis() -  lastUpdateMs >= LineDelay) {
                  
                    u8g2.clearBuffer();
#ifdef ShowRespCN                    
                    u8g2.setFont(u8g2_font_unifont_t_chinese3); // 務必先設定字型才能計算寬度
//                    u8g2.setFont(u8g2_font_wqy12_t_chinese2);      
#endif
                    // 置中顯示或靠左
                    int y_pos = 35; // 垂直位置大約在中間
                    u8g2.drawUTF8(0, y_pos, displayLines[currentDisplayLine].c_str());
                    drawstatusbar();                    
                    u8g2.sendBuffer();
                    currentDisplayLine++;
                    lastUpdateMs = millis();
                    
                    // 假設顯示完了（這裡需要判斷是否還有下一頁/行）
                    // 如果顯示到最後一段：
                    if (currentDisplayLine >= displayLines.size()) {
                        dispRESULT =false;
                        resultStartTime = millis(); // 顯示完後才開始計算 3 秒倒數
                    }
                }
            } else {
#ifdef serialDSP  
//                    Serial.println("Result list end");
#endif
                // 所有行都顯示完了，等待 3 秒後回到 IDLE
                if (!isTTSplay && (millis() - resultStartTime) >= 2000) {
                    currentDisplayLine = 0;   // 重置行索引
                    displayLines.clear();     // 清空文字內容 (防止重複觸發)
//                    currentState = STATE_IDLE;
//                    displaySetState(DISP_IDLE);
                }
            }
        }

        if (currentState == STATE_IDLE) {

        // 定期刷新顯示
        if (millis() - lastDispRefresh >= 1000) {
            displayRefresh();
            lastDispRefresh = millis();
        }
            
            // 檢查是否 idle 超過 60 秒，且 WiFi 目前是連線狀態
            if (wifiConnected && (millis() - lastActivityMs >= WIFI_TIMEOUT)) {
#ifdef serialDSP  
                Serial.println("自動省電：關閉網路連線");
#endif                
                toggleNetworkConnection(true); // 內部會把 wifiConnected 設為 false
//////////////////////////////////////////////////////////////////////////////////
             // 畫面全黑
            u8g2.clearBuffer();
            u8g2.sendBuffer();
            // 進入休眠模式
            u8g2.setPowerSave(1);                 
            //進入深度休眠由觸摸按鈕喚醒            
            esp_deep_sleep_enable_gpio_wakeup(1 << 2, ESP_GPIO_WAKEUP_GPIO_HIGH);                      
            esp_deep_sleep_start();
//////////////////////////////////////////////////////////////////////////////////                
            }
            
        } else {
            // 只要在錄音、AI 運算或播放中，就持續重置計時器
            lastActivityMs = millis();
        }

         vTaskDelay(pdMS_TO_TICKS(10));
///////////////////////////////////
#ifdef ChkHeap  
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL); 
        if((int)uxHighWaterMark < minval){       
        minval = min(minval,(int)uxHighWaterMark);
        Serial.printf("Current displayTask: %d words\n", minval);
        }
#endif        
///////////////////////////////////
         
    }
}

#ifdef forceEraseNVS
#include <nvs_flash.h>
#endif        

void setup() {
    delay(500);
    Serial.begin(115200);
#ifdef serialDSP    
    Serial.println("\n=== ESP32 VOSK STT ===\n");
#endif
#ifdef forceEraseNVS
    // 強制擦除整個 NVS WiFi 分區
    WiFi.disconnect(true, true); //刪除SSID及密碼
    nvs_flash_erase();    // 擦除所有 NVS 儲存 (包含 WiFiManager 留下的殘餘)
    nvs_flash_init();     // 重新初始化
    Serial.println("WiFi 帳密與 NVS 已徹底清除！");
    Serial.println("現在請註解掉這幾行，重新燒錄您的主程式。");
    while(1); // 停機在這裡
#endif        

    displayInit();
    displayUpdate("Init...", "Wait...");
    delay(100);
    
    // 初始化 I2S
  // 1. 初始化 I2S 雙工模式
  auto cfg = i2s.defaultConfig(RXTX_MODE);
  cfg.sample_rate = 8000;
  cfg.channels = 1;
  cfg.bits_per_sample = 16;
  
  // 腳位設定 (ESP32-C3 Super Mini)
  cfg.pin_bck = I2S_BCLK; 
  cfg.pin_ws = I2S_LRC;
  cfg.pin_data = I2S_DOUT;
  cfg.pin_data_rx = I2S_DIN;;  // DIN  (接 INMP441 SD)
  
  cfg.buffer_size = 256;
  cfg.buffer_count = 16;

  if (!i2s.begin(cfg)) {
#ifdef serialDSP  
    Serial.println("I2S 啟動失敗！");
#endif    
  } else {
#ifdef serialDSP  
    Serial.println("I2S 雙工模式 (8k/Mono) 已在 setup 啟動");
#endif    
  }    
//    setupI2S(SAMPLE_RATE, RX_MODE);
//    delay(50);
    
    // 初始化 SPIFFS
    if (!SPIFFS.begin(true)) {
        displayUpdate("SPIFFS", "FAIL");
    } else {
        displayUpdate("SPIFFS", "OK");
    }
    delay(100);

    toggleNetworkConnection(true);    //開啟WiFi
//    WiFi.mode(WIFI_OFF);
//    displayUpdate("Long press", "for WiFi");
//    delay(100);

//按鍵通知訊息需要是跨任務通知,所以在setup中建立
    buttonEventQueue = xQueueCreate(5, sizeof(ButtonEvent));                    //建立按鍵通知訊息
// 按鍵之後的處理任務
    xTaskCreate(buttonEventTask, "ButtonEventTask", STACK_BUTTON, NULL, PRIORITY_BUTTON, NULL);       //
    xTaskCreate(ttp223Task, "TTP223_Touch", STACK_TTP223, NULL, PRIORITY_TTP223, NULL);
    xTaskCreatePinnedToCore(audioTask, "AudioTask", STACK_AUDIO, NULL, PRIORITY_AUDIO, NULL, 0);
    xTaskCreatePinnedToCore(sttTask, "STTTask", STACK_STT, NULL, PRIORITY_STT, NULL, 0);
    xTaskCreatePinnedToCore(displayTask, "DisplayTask", STACK_DISPLAY, NULL, PRIORITY_DISPLAY, NULL, 0);
    xTaskCreate(wavPlaybackTask, "WavPlaybackTask", STACK_PLAYBACK, NULL, PRIORITY_PLAYBACK, NULL);
    xTaskCreate(WiFiManagerTask, "WiFiManagerTask", STACK_WM, NULL, PRIORITY_WM, NULL);       //
    
    Serial.println("Setup complete");
    


                vTaskDelay(pdMS_TO_TICKS(1000)); 
//    delay(1000); // 等待各個 Task 初始化穩定
// 播放留言錄音
#ifdef serialDSP  
    Serial.println("Checking for previous recordings in SPIFFS...");
#endif    

////////////////////////////////////////////////////////////////////////
//播放所有錄音留言檔
// FIFO 播放邏輯：從「下一個要寫入的位置」開始播放，那就是最舊的
    for (int i = 0; i < MAX_FILES; i++) {
        int targetIdx = (nextFileIndex + i) % MAX_FILES;
        char filename[32];
        snprintf(filename, sizeof(filename), RECORD_FILENAME, targetIdx);

        if (SPIFFS.exists(filename)) {
#ifdef serialDSP  
            Serial.printf("FIFO Playing [%d/%d]: %s\n", i + 1, MAX_FILES, filename);
#endif            
            
            currentState = STATE_PLAYBACK;
            displaySetState(DISP_PLAYBACK);
            
            audioLoadAndPlayFromSPIFFS(filename,false);

            // 必須等待播放完成，才播下一個
//            while (isBufferPlaying) {
//                vTaskDelay(pdMS_TO_TICKS(100)); 
//            }
        }
    }
//       currentState = STATE_IDLE;
//       displaySetState(DISP_IDLE);
///////////////////////////////////////////////////////////////////////// 

/////////////////////////////////////////////////////////////////////////
    struct tm ti;
    displayUpdate("SNTP", "Connect.");
    configTime(28800, 0, "pool.ntp.org","time.nist.gov"); // 設定台灣時區
    int retry = 0;
    bool sync_ok = false;

    setenv("TZ", "CST-8", 1);
    tzset();
    // 2. 嘗試直接從內部 RTC 取得時間 (Deep Sleep 期間 RTC 不會停)
    if (getLocalTime(&ti) && ti.tm_year > (2020 - 1900)) {
        Serial.println("使用內部 RTC 時間報時...");
    } 
    else {
        // 如果 RTC 時間失效（例如第一次開機），才執行連網對時
        Serial.println("RTC 時間無效，嘗試連網對時...");
        while (retry < 30) {
        // 1. 檢查 SNTP 狀態是否真的變為「已完成」
          if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            if (getLocalTime(&ti) && ti.tm_year > (2020 - 1900)) {
              break; 
            }
          }
          vTaskDelay(pdMS_TO_TICKS(500));
          retry++;
        }
    }
       currentState = STATE_IDLE;
       displaySetState(DISP_IDLE);

            
    // 檢查喚醒原因
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
//    struct tm ti;
if (getLocalTime(&ti)) {
    switch(wakeup_reason) {
        // 對應 esp_deep_sleep_enable_gpio_wakeup 產生的原因
        case ESP_SLEEP_WAKEUP_GPIO: 
            Serial.println("GPIO 喚醒報時");
            clockplay(ti);
            break;

        // 如果是傳統的 ext0 方式 (esp_sleep_enable_ext0_wakeup)
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.println("EXT0 喚醒報時");
            clockplay(ti);
            break;

        default:
            Serial.printf("其他原因喚醒 (%d)，執行預設報時\n", wakeup_reason);
            clockplay(ti);
            break;
    }
} else {
    // 剛喚醒時若 NTP 還沒同步，getLocalTime 可能會失敗
    Serial.println("尚未取得系統時間，無法報時");
} 
              
/////////////////////////////////////////////////////////////////////////
   
/*    
//播放最高編號的wav檔     
    bool foundAndPlayed = false;
    char checkFilename[32];

    // 從最新的編號往回找 (從 002 找回 000)
    for (int i = MAX_FILES - 1; i >= 0; i--) {
        snprintf(checkFilename, sizeof(checkFilename), RECORD_FILENAME, i);
        
        if (SPIFFS.exists(checkFilename)) {
            Serial.printf("Found existing record: %s, playing now...\n", checkFilename);
            
            // 1. 設定播放狀態，讓 displayTask 顯示播放畫面
            currentState = STATE_PLAYBACK;
            displaySetState(DISP_PLAYBACK);
            
            // 2. 執行播放 (這會觸發你的 I2S 播放邏輯)
            audioLoadAndPlayFromSPIFFS(checkFilename);            
            foundAndPlayed = true;
            break; // 找到一個並播放後就跳出迴圈
        }
    }

    if (!foundAndPlayed) {
        Serial.println("No previous recordings found. Entering IDLE.");
        currentState = STATE_IDLE;
        displaySetState(DISP_IDLE);
    }
*/    
/////////////////////////////////////////////////////////////////////////
}

void loop() {
     vTaskDelay(pdMS_TO_TICKS(10));
}
