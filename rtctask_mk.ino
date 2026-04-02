/* C:\Users\halin\Documents\Arduino\sketch\ESP32\getdatetimeWifi\rtctask_mk
 * 在 FreeRTOS 架構下建立一個獨立的 Task，
 * 利用 RTC 記憶體變數來記錄上次同步的時間戳記
 * 這樣即使經過深度睡眠（Deep Sleep)
 * 系統判斷是否已滿24小時自動更新
 */

//#define simp1306  //use SSD1306Ascii.h

#include <WiFi.h>
#include "time.h"
#include <Wire.h>
#ifdef simp1306
    #include "SSD1306Ascii.h"
    #include "SSD1306AsciiWire.h"
  #else
    #include <U8g2lib.h>    
#endif
#include "FS.h"
#include <SPIFFS.h>
#include <WebServer.h>
///i2s定義區/////////////////
#include <driver/i2s.h>
#include "esp_err.h"
#include "esp_sntp.h"

#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__))
// 2. 計算純檔名的長度 (找到點的位置並相減)
#define __FNAME_LEN__ (strchr(__FILENAME__, '.') ? (int)(strchr(__FILENAME__, '.') - __FILENAME__) : (int)strlen(__FILENAME__))
#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 7
#define I2S_PORT I2S_NUM_0
#define TTP223_PIN 2
#define serialDSP
#define usespiffs

File wav;
//uint8_t buffer[1024];
float Volume = 0.7; // 設定音量為 70% (0.0 ~ 1.0)
bool i2srdflag = true; //i2s旗標
////////////////////////////

#ifdef simp1306
  SSD1306AsciiWire oled;
  bool oledrdflag = false;
  #else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
#endif

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

TaskHandle_t ntpTaskHandle = NULL; //ntpSyncTask 的task handle

// 定義一個 Queue Handle
QueueHandle_t audioQueue;

#define ALARM_FILE "/alarms.bin"
#define HTML_FILE "/alarm.html"
WebServer *server = NULL;
bool webServerEnabled = false;
bool useSmartConfig = false;
typedef struct {
    int hour;
    int minute;
    bool active;
} AlarmTime;

// 設定最多 5 組鬧鐘
#define MAX_ALARMS 5
bool alarmchg =false;
bool plaied = false;
AlarmTime alarmList[MAX_ALARMS] = {
    {8, 1, true},   // 鬧鐘 1: 07:00
    {9, 2, true},   // 鬧鐘 2: 09:00
    {15, 5, true},  // 鬧鐘 3: 12:00
    {7, 0, true},  // 鬧鐘 4: 17:00
    {0, 0, false}   // 鬧鐘 5: 未啟用
};


// 使用 RTC 記憶體保存上次同步的時間戳記（深度睡眠後不會消失）
RTC_DATA_ATTR unsigned long lastSyncTimestamp = 0; //上次同步unix時間戳
RTC_DATA_ATTR int syncFailedCount = 0;
//const unsigned long SYNC_INTERVAL = 24 * 60 * 60; // 一天的秒數
unsigned long SYNC_INTERVAL =  12 * 60 * 60; // 12小時

enum MenuLayer
{
    MENU_ROOT,
    MENU_MAIN,
    MENU_ACTION,
    MENU_TIME
};
//MenuLayer menuLayer = MENU_MAIN;
MenuLayer menuLayer = MENU_ROOT;
int cursor = 0;
int actionCursor = 0;
bool editHour = true;

RTC_DATA_ATTR time_t lastNTPTime = 0;      // 上次 NTP 成功的標準時間
RTC_DATA_ATTR uint32_t lastRTCTick = 0;    // 上次 NTP 成功時的系統 Tick (millis)
RTC_DATA_ATTR float driftFactor = 1.0;     // 校正係數 (預設 1.0)

void handleRoot() {
    File htmlFile = SPIFFS.open(HTML_FILE, "r");
    if (!htmlFile) {
        server->send(404, "text/plain", "HTML file not found");
        return;
    }
    server->streamFile(htmlFile, "text/html");
    htmlFile.close();
}

void handleGetAlarms() {
    String action = server->arg("action");
    
    if (action == "load") {
        loadAlarms();
    }
    
    String json = "[";
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (i > 0) json += ",";
        json += "{\"hour\":" + String(alarmList[i].hour);
        json += ",\"minute\":" + String(alarmList[i].minute);
        json += ",\"active\":" + String(alarmList[i].active ? "true" : "false") + "}";
    }
    json += "]";
    server->send(200, "application/json", json);
}

void handleGetTime() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        server->send(200, "application/json", "{\"time\":\"" + String(timeStr) + "\"}");
    } else {
        server->send(200, "application/json", "{\"time\":\"--:--:--\"}");
    }
}

void handleGetIP() {
    String ip = WiFi.localIP().toString();
    server->send(200, "application/json", "{\"ip\":\"" + ip + "\"}");
}

void startWebServer() {
    if (server) delete server;
    server = new WebServer(80);
    
    server->on("/", HTTP_GET, []() {
        File htmlFile = SPIFFS.open(HTML_FILE, "r");
        if (!htmlFile) {
            server->send(404, "text/plain", "HTML file not found");
            return;
        }
        server->streamFile(htmlFile, "text/html");
        htmlFile.close();
    });
    
    server->on("/api/alarms", HTTP_GET, []() {
        String action = server->arg("action");
        if (action == "load") {
            loadAlarms();
        }
        String json = "[";
        for (int i = 0; i < MAX_ALARMS; i++) {
            if (i > 0) json += ",";
            json += "{\"hour\":" + String(alarmList[i].hour);
            json += ",\"minute\":" + String(alarmList[i].minute);
            json += ",\"active\":" + String(alarmList[i].active ? "true" : "false") + "}";
        }
        json += "]";
        server->send(200, "application/json", json);
    });
    
    server->on("/api/alarms", HTTP_POST, []() {
        if (!server->hasArg("plain")) {
            server->send(400, "application/json", "{\"error\":\"No data\"}");
            return;
        }
        String body = server->arg("plain");
        String action = server->arg("action");
        Serial.println("POST body: " + body);
        
        int startIdx = 0;
        for (int i = 0; i < MAX_ALARMS; i++) {
            int objStart = body.indexOf('{', startIdx);
            if (objStart < 0) break;
            
            int objEnd = body.indexOf('}', objStart);
            if (objEnd < 0) break;
            
            String obj = body.substring(objStart, objEnd + 1);
            Serial.println("Obj " + String(i) + ": " + obj);
            
            int hStart = obj.indexOf("\"hour\":") + 7;
            int hEnd = obj.indexOf(",", hStart);
            alarmList[i].hour = obj.substring(hStart, hEnd).toInt();
            
            int mStart = obj.indexOf("\"minute\":") + 9;
            int mEnd = obj.indexOf(",", mStart);
            alarmList[i].minute = obj.substring(mStart, mEnd).toInt();
            
            int aStart = obj.indexOf("\"active\":") + 9;
            int aEnd = aStart;
            while (aEnd < obj.length() && obj.charAt(aEnd) != ',' && obj.charAt(aEnd) != '}') {
                aEnd++;
            }
            String aVal = obj.substring(aStart, aEnd);
            aVal.trim();
            alarmList[i].active = (aVal == "true");
            
            Serial.printf("Parsed alarm %d: hour=%d, minute=%d, active=%d val=[%s]\n", 
                i, alarmList[i].hour, alarmList[i].minute, alarmList[i].active, aVal.c_str());
            
            startIdx = objEnd + 1;
        }
        alarmchg = true;
        if (action == "save") {
            saveAlarms();
            server->send(200, "application/json", "{\"result\":\"saved\"}");
        } else if (action == "apply") {
            server->send(200, "application/json", "{\"result\":\"applied\"}");
        } else {
            server->send(200, "application/json", "{\"result\":\"ok\"}");
        }
    });
    
    server->on("/api/time", HTTP_GET, []() {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char timeStr[20];
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
            server->send(200, "application/json", "{\"time\":\"" + String(timeStr) + "\"}");
        } else {
            server->send(200, "application/json", "{\"time\":\"--:--:--\"}");
        }
    });
    
    server->on("/api/ip", HTTP_GET, []() {
        String ip = WiFi.localIP().toString();
        server->send(200, "application/json", "{\"ip\":\"" + ip + "\"}");
    });
    
    server->begin();
    webServerEnabled = true;
    Serial.println("Web Server started");
}


/*
//喚醒後校正RTC
void adjRTC(){
    // 取得醒來後的系統時間 (此時 RTC 可能已經跑偏了)
    time_t rawNow;
    time(&rawNow);

    if (lastNTPTime != 0 && driftFactor != 1.0) {
        // 計算自上次同步後，RTC 認定經過的秒數
        double rtcDiff = difftime(rawNow, lastNTPTime);
        // 套用係數修正
        time_t correctedTime = lastNTPTime + (time_t)(rtcDiff * driftFactor);
        
        // 將修正後的時間寫回系統
        struct timeval tv = { .tv_sec = correctedTime, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        
        Serial.println("已套用軟體校正補償時間");
#ifdef simp1306      
    oled.printf("RTC * %ld",rtcDiff);
#else
      u8g2.drawStr(20, 50, "RTC adjustted"); // 繪製轉換後的字串
      u8g2.sendBuffer();
#endif
        
    }
}
*/


// 將整個alarmlist陣列儲存到 SPIFFS
void saveAlarms() {
    File file = SPIFFS.open(ALARM_FILE, FILE_WRITE);
    if (!file) {
        Serial.println("無法開啟檔案進行寫入");
        return;
    }
    
    // 使用 file.write 直接將記憶體區塊寫入檔案
    size_t written = file.write((uint8_t*)alarmList, sizeof(alarmList));
    file.close();
    
    if (written == sizeof(alarmList)) {
        Serial.println("鬧鐘設定已成功儲存到 SPIFFS");
    } else {
        Serial.println("寫入資料量異常");
    }
    alarmchg =false;
}

// 從 SPIFFS 讀取資料回填至alarmlist陣列
void loadAlarms() {
    if (!SPIFFS.exists(ALARM_FILE)) {
        Serial.println("找不到存檔，將使用程式碼內的預設值");
        return;
    }

    File file = SPIFFS.open(ALARM_FILE, FILE_READ);
    if (!file) {
        Serial.println("讀取檔案失敗");
        return;
    }

    // 檢查檔案大小是否符合預期
    if (file.size() != sizeof(alarmList)) {
        Serial.println("存檔大小不符，忽略載入");
        file.close();
        return;
    }

    // 直接讀取回記憶體
    file.read((uint8_t*)alarmList, sizeof(alarmList));
    file.close();
    Serial.println("鬧鐘設定載入完成");
}

void drawBanner()
{
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(20, 30, "ui-Alarm alpha");
  u8g2.sendBuffer();
}
/*
=============================
Draw Main Menu
=============================
*/

void drawMain()
{
    for(int i=0;i<MAX_ALARMS;i++)
    {
        int y=12+i*10;

        if(i==cursor)
            u8g2.drawStr(0,y,"> ");
        else
            u8g2.drawStr(0,y,"  ");

        char buf[16];

        sprintf(buf,"%c %02d:%02d",
                alarmList[i].active?'V':'X',
                alarmList[i].hour,
                alarmList[i].minute);

        u8g2.drawStr(12,y,buf);
    }
}

/*
=============================
Draw Action Menu
=============================
*/

void drawAction()
{
    u8g2.drawStr(0,15,"ACT");

    if(actionCursor==0)
        if (alarmList[cursor].active){
          u8g2.drawStr(0,30,"> Enable ");
          } else {
          u8g2.drawStr(0,30,"> Disable");
          }
    else
        if (alarmList[cursor].active){
          u8g2.drawStr(0,30,"  Enable ");
          } else {
          u8g2.drawStr(0,30,"  Disable");
          }

    if(actionCursor==1)
        u8g2.drawStr(0,45,"> Adjust");
    else
        u8g2.drawStr(0,45,"  Adjust");
}

/*
=============================
Draw Time Menu
=============================
*/

void drawTime()
{
    char buf[16];

    sprintf(buf,"%02d:%02d",
        alarmList[cursor].hour,
        alarmList[cursor].minute);

    u8g2.setFont(u8g2_font_logisoso24_tr);
    u8g2.drawStr(10,40,buf);

    u8g2.setFont(u8g2_font_6x10_tf);

    if(editHour)
        u8g2.drawStr(10,55,"edit hour");
    else
        u8g2.drawStr(10,55,"edit minute");
}

/*
=============================
Draw menu router
=============================
*/

void drawUI()
{
    u8g2.clearBuffer();
    if(menuLayer==MENU_ROOT)
        drawBanner();

    if(menuLayer==MENU_MAIN)
        drawMain();

    if(menuLayer==MENU_ACTION)
        drawAction();

    if(menuLayer==MENU_TIME)
        drawTime();

    u8g2.sendBuffer();
}

void listSPIFFSFiles() {
#ifdef serialDSP      
    Serial.println("目前 SPIFFS 檔案清單：");
#endif    
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while (file) {
#ifdef serialDSP      
        Serial.printf("檔案: %s, 大小: %d bytes\n", file.name(), file.size());
#endif        
        file = root.openNextFile();
    }
}


#ifdef simp1306
bool i2cDevicePresent(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void i2cRecover(int sda, int scl)
{
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, OUTPUT);

    digitalWrite(scl, HIGH);
    delay(5);

    for (int i = 0; i < 9; i++)
    {
        digitalWrite(scl, LOW);
        delayMicroseconds(5);
        digitalWrite(scl, HIGH);
        delayMicroseconds(5);
    }

    pinMode(scl, INPUT_PULLUP);
}

void initSSD1306(bool cflag){
// 定義 I2C 地址，通常是 0x3C
#define I2C_ADDRESS 0x3C
// 定義 ESP32-C3 Super Mini 的 I2C 接腳
#define SDA_PIN 8
#define SCL_PIN 9
    Serial.println("init SD1306....");


  // 手動指定接腳初始化 I2C
//  pinMode(SDA_PIN, INPUT_PULLUP);
//  pinMode(SCL_PIN, INPUT_PULLUP);
//  delay(10);  
  i2cRecover(SDA_PIN, SCL_PIN); // 釋放可能卡住的 I2C bus

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); // 設置 400kHz 快速度

  // 初始化 OLED
//  oled.begin(&Adafruit128x64, I2C_ADDRESS);  
   if(i2cDevicePresent(I2C_ADDRESS)){
      oled.begin(&Adafruit128x64, I2C_ADDRESS);
  // 設定字體 (更多字體可在程式庫的 fonts 目錄找到)
      oled.setFont(Adafruit5x7);
      oled.setScrollMode(SCROLL_MODE_AUTO);
      if(cflag){
        oled.clear();
        oled.println("ESP32-C3 Mini");
        oled.println("Alarm Sche OK!");
        oled.printf("%.*s\n", __FNAME_LEN__, __FILENAME__);
        }
      Serial.println("SD1306 ready.");      
      oledrdflag =true;
      } else {
      Serial.println("OLED not detected");
      } 
  
}
#endif

void setupI2S()
{
   esp_err_t err;
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 8000,
//        .sample_rate = 12000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
//        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
//        .channel_format = I2S_CHANNEL_FMT_ALL_LEFT,    
            
//        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
//        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),

        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

//    i2s_driver_install(I2S_PORT, &config, 0, NULL);
//    i2s_set_pin(I2S_PORT, &pins);
    err = i2s_driver_install(I2S_PORT, &config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("I2S 驅動安裝失敗: 0x%X\n", err);
        i2srdflag = false;
        return; // 或進行其他錯誤處理
    }
    // 設定接腳
    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("I2S 接腳設定失敗: 0x%X\n", err);
        i2srdflag = false;
        return;
    }
/* 這段檢查會影響輸出
// 檢查i2s傳送正常
  size_t bytes_read;
  int32_t buffer[64];
  // 3. 讀取數據判斷是否有訊號
  i2s_read(I2S_NUM_0, &buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
  if (bytes_read > 0 && buffer[0] != 0) {
    Serial.println("偵測到 I2S 訊號");
  } else {
        Serial.println("I2S 資料傳送失敗!");
        i2srdflag = false;
        return;    
  }
*/  

    // 3. 強制設定取樣率，這會重新觸發硬體時脈計算
//    i2s_set_sample_rates(I2S_PORT, 16000); 
    
    Serial.println("I2S 設定完成");
}

//單聲道播放8k bit檔
void playWav(const char* fileName , int ch) { // 單聲道通常不需要傳 ch 參數了
    if (!i2srdflag) return;//沒有i2s
    
    wav = SPIFFS.open(fileName, "r");
    if (!wav) {
        Serial.printf("無法開啟檔案: %s\n", fileName);
        return;
    }

    wav.seek(44); // 跳過 WAV Header

    // 1. 修正取樣率為 8000
//    i2s_set_sample_rates(I2S_PORT, 8000); 

static uint8_t read_buf[512];
static int16_t write_buf[256];

    int fade_count = 0;
    const int FADE_IN_STEPS = 300; 
    const int FADE_OUT_STEPS = 300; 
    // 先取得檔案總大小來計算淡出時機
    size_t total_samples = (wav.size() - 44) / 2 ; 
    int current_sample_idx = 0;
    
    while (wav.available()) {
        int bytes_read = wav.read(read_buf, sizeof(read_buf));
        int samples_count = bytes_read / 2; // 每個樣本 2 bytes (16-bit)
        int16_t* ptr = (int16_t*)read_buf;
        for (int i = 0; i < samples_count; i++) {
            // 2. 直接讀取每一個樣本，不跳聲道
            int32_t raw_sample = (int32_t)ptr[i] << 1; 

            float current_vol = Volume;
            
            // 淡入邏輯
            if (fade_count < FADE_IN_STEPS) {
                current_vol *= ((float)fade_count / FADE_IN_STEPS);
                fade_count++;
            }
            // --- 淡出邏輯 (選配) ---
//            int remaining = total_samples - current_sample_idx;
//            if (remaining < FADE_OUT_STEPS) {
//               current_vol *= ((float)remaining / FADE_OUT_STEPS);
//            }

            // 套用音量並存入寫入緩衝區
            write_buf[i] = (int16_t)(raw_sample * current_vol);
            current_sample_idx++;
        }

/*
        for (int i = 0; i < samples_count; i++) {
            // 2. 直接讀取每一個樣本，不跳聲道
            int16_t raw_sample = ptr[i]; 

            float current_vol = Volume;
            
            // 淡入邏輯
            if (fade_count < FADE_IN_STEPS) {
                current_vol *= ((float)fade_count / FADE_IN_STEPS);
                fade_count++;
            }
            // --- 淡出邏輯 (選配) ---
            int remaining = total_samples - current_sample_idx;
            if (remaining < FADE_OUT_STEPS) {
               current_vol *= ((float)remaining / FADE_OUT_STEPS);
            }

            // 套用音量並存入寫入緩衝區
            write_buf[i] = (int16_t)(raw_sample * current_vol);
            current_sample_idx++;
        }
*/
        size_t written;
        // 3. 寫入 I2S (samples_count * 2 是因為每個樣本是 2 bytes)
        i2s_write(I2S_PORT, write_buf, samples_count * 2, &written, portMAX_DELAY);
    }

    i2s_zero_dma_buffer(I2S_PORT);
    vTaskDelay(pdMS_TO_TICKS(10)); 
    wav.close();
}


/*
//雙聲道播放16k bit檔
void playWav(const char* fileName, int ch) {

    wav = SPIFFS.open(fileName, "r");
    if (!wav) {
        Serial.printf("無法開啟檔案: %s\n", fileName);
        return;
    }

    // --- 解決啟動爆音：先送入 128 個採樣點的靜音 ---
//    int16_t pre_silence[128] = {0};
//    size_t written;
//    i2s_write(I2S_PORT, pre_silence, sizeof(pre_silence), &written, portMAX_DELAY);
    // ------------------------------------------
    
    wav.seek(44); // 跳過標頭
    
    uint8_t read_buf[512];
    int16_t write_buf[256];

    int fade_count = 0;
    const int FADE_IN_STEPS = 200;  // 淡入樣本數
    const int FADE_OUT_STEPS = 200; // 淡出樣本數
    // 先取得檔案總大小來計算淡出時機
    size_t total_samples = (wav.size() - 44) / 2 / (ch == 0 || ch == 1 ? 2 : 1); 
    int current_sample_idx = 0;
    
    // 確保取樣率與音檔一致 (假設你的音檔都是 16k 或 8k)
    i2s_set_sample_rates(I2S_PORT, 128000); 

    while (wav.available()) {
        int bytes_read = wav.read(read_buf, sizeof(read_buf));
        int samples_count = bytes_read / 2;
        int mono_samples = 0;
        int16_t* ptr = (int16_t*)read_buf;

        for (int i = 0; i < samples_count; i += 2) {
//            write_buf[mono_samples++] = ptr[i + ch];
              int16_t raw_sample = ptr[i + ch]; 

        float current_vol = Volume;
        // 執行淡入邏輯
        if (fade_count < FADE_IN_STEPS) {
            current_vol *= ((float)fade_count / FADE_IN_STEPS);
            fade_count++;
        }
            // --- 淡出邏輯 (選配) ---
//            int remaining = total_samples - current_sample_idx;
//            if (remaining < FADE_OUT_STEPS) {
//               current_vol *= ((float)remaining / FADE_OUT_STEPS);
//            }

                 
              // 將音量降低到 25% (向右位移 2 位)
            write_buf[mono_samples++] = (int16_t)(raw_sample * current_vol);
            current_sample_idx++;

        }
        size_t written;
        i2s_write(I2S_PORT, write_buf, mono_samples * 2, &written, portMAX_DELAY);
    }
    // 播放結束前，送入一小段「靜音」資料（全為 0）
//    int16_t silent_buf[64] = {0}; 
//    size_t written;
//    i2s_write(I2S_PORT, silent_buf, sizeof(silent_buf), &written, portMAX_DELAY);

    i2s_zero_dma_buffer(I2S_PORT);
    // 給予硬體一點點時間讓電位穩定
    vTaskDelay(pdMS_TO_TICKS(10)); 
    wav.close();
}
*/
void audioTask(void *pvParameters) {
    struct tm timeinfo;
    char fileName[20];
    Serial.println("audiotask ready..");

    while (1) {
        // 等待 Queue 傳入時間資料 (阻塞模式，不消耗 CPU)
        if (xQueueReceive(audioQueue, &timeinfo, portMAX_DELAY)) {
            Serial.println("[AudioTask] 開始播報...");

            // 1. 現在時間
            playWav("/head.wav", 0);

            // 2. 小時邏輯 (簡化示範)
            if (timeinfo.tm_hour >= 10) {
                if (timeinfo.tm_hour / 10 > 1) {
                    sprintf(fileName, "/%d.wav", timeinfo.tm_hour / 10);
                    playWav(fileName, 0);
                }
                playWav("/10.wav", 0);
            }
            if (timeinfo.tm_hour % 10 != 0 || timeinfo.tm_hour == 0) {
                sprintf(fileName, "/%d.wav", timeinfo.tm_hour % 10);
                playWav(fileName, 0);
            }
            playWav("/dot.wav", 0);

            // 3. 分鐘邏輯
            int m = timeinfo.tm_min;
            if (m >= 10) {
                sprintf(fileName, "/%d.wav", m / 10);
                if((m / 10) > 1){ playWav(fileName, 0);}
                playWav("/10.wav", 0);
            }
            if (m % 10 != 0 || m == 0) {
                sprintf(fileName, "/%d.wav", m % 10);
                playWav(fileName, 0);
            }
            playWav("/min.wav", 0);
/*
            // 4. 秒數邏輯
            
            sprintf(fileName, "/%d.wav", timeinfo.tm_sec % 10);
            playWav(fileName, 0);
            playWav("/sec.wav", 0);
*/
            Serial.println("[AudioTask] 播報完畢");
            
        }
    }
}

/**
 * 計算距離最近的一個鬧鐘還有多少秒
 * @param now 目前時間的指標
 * @return uint64_t 距離下次鬧鐘的秒數（微秒轉換用）
 */
uint64_t getSecondsToNextAlarm(struct tm *now) {
    String str;  
    long minDiff = 86400; // 初始化為一天的秒數 (最大的可能差距)
    long currentSeconds = now->tm_hour * 3600 + now->tm_min * 60 + now->tm_sec;

    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!alarmList[i].active) continue;

        long alarmSeconds = alarmList[i].hour * 3600 + alarmList[i].minute * 60;
        long diff = alarmSeconds - currentSeconds;

        // 如果鬧鐘時間小於等於目前時間，代表是明天的這個時間
        if (diff <= 0) {
            diff += 86400; // 加一天的秒數
        }

        if (diff < minDiff) {
            str =String(alarmList[i].hour) +":" + String(alarmList[i].minute);
            minDiff = diff;
        }
    }
    
#ifdef simp1306      
    if(oledrdflag){
    oled.printf("Sleep Timer %ld\n", minDiff);
    }
#else
    u8g2.drawStr(48, 54, str.c_str()); // 繪製轉換後的字串
    u8g2.sendBuffer();
#endif                 
    Serial.printf("[Sleep] 下次鬧鐘時間 %s \n", str);
    Serial.printf("[Sleep] 距離下次鬧鐘還有 %ld 秒\n", minDiff);
    Serial.flush(); // 確保序列埠傳完
    return (uint64_t)minDiff;
}

void alarmCheckTask(void *pvParameters) {
    struct tm timeinfo;
    int lastCheckedMinute = -1; // 避免同一分鐘內重複觸發

    while (1) {
        // 1. 取得目前的本地 RTC 時間
        if (getLocalTime(&timeinfo)) {
            
            // 2. 只有在分鐘改變時才檢查鬧鐘，節省 CPU
            if (timeinfo.tm_min != lastCheckedMinute) {
                plaied =false;
                for (int i = 0; i < MAX_ALARMS; i++) {
                    if (alarmList[i].active &&
                        alarmList[i].hour == timeinfo.tm_hour && 
                        alarmList[i].minute == timeinfo.tm_min) {
                        
                        Serial.printf("[Alarm] 鬧鐘觸發！目前時間 %02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min);
                        
                        // 3. 傳送目前時間給語音播報 Task
                        if(!plaied){
                          plaied =true;
                          xQueueSend(audioQueue, &timeinfo, 0);
                          }
                        
                        // 記錄已檢查過這一分，防止一秒內檢查多次
                        lastCheckedMinute = timeinfo.tm_min;
                        break; 
                    }
                }
            }
        }
        
        // 每 30 秒檢查一次即可，不需要太頻繁
        vTaskDelay(pdMS_TO_TICKS(30000)); 
    }
}


// 取得目前日期字串 (YYYYMMDD)
String getTodayDate() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
#ifdef serialDSP      
        Serial.println("無法取得網路時間");
#endif        
        return "00000000";
    }
    char dateStr[9];
    strftime(dateStr, 9, "%Y%m%d", &timeinfo);
    return String(dateStr);
}

//****************************************************
// 開始更改部分
//****************************************************
// 1. TTP223 觸摸任務
void ttp223Task(void *pv) {
  pinMode(TTP223_PIN, INPUT);
/*
    UBaseType_t uxHighWaterMark;      //monitor stack
// 初始量測 
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("TTP223Task Start - Free Stack: %ld words\n", uxHighWaterMark);
*/
    bool lastStable = LOW;
    bool lastRead   = LOW;

    TickType_t debounceTick = 0;
    TickType_t pressTick = 0;
    TickType_t releaseTick = 0;

    int clickCount = 0;

    while (1)
    {
        bool state = digitalRead(TTP223_PIN);
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
                        xQueueSend(buttonEventQueue, &evt, 0);
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

            xQueueSend(buttonEventQueue, &evt, 0);
            clickCount = 0;
        }

/////////////////////////////
/*
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);        
        // 如果剩餘空間過小（例如少於 20 words），發出警告
        if(uxHighWaterMark < 20) {
            Serial.printf("WARNING: Stack running low! Only %ld words left.\n", uxHighWaterMark);
        } else {
            Serial.printf("Current ButtonTask: %ld words\n", uxHighWaterMark);
        }
*/        
/////////////////////////////

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

void buttonEventTask(void *pvParameters)
{
/*
    UBaseType_t uxHighWaterMark;      //monitor stack
    // 初始量測 
    uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("ButtonEventTask Start - Free Stack: %ld words\n", uxHighWaterMark);
*/

    ButtonEvent evt;
    while (1)
    {
        if (xQueueReceive(buttonEventQueue, &evt, portMAX_DELAY))
        {
            switch (evt)
            {
                case EVENT_SINGLE:
#ifdef serialDSP      
                Serial.println("單擊");
#endif                

//================================
                    if (menuLayer==MENU_MAIN) {
                        cursor++;
                        if(cursor>=MAX_ALARMS) cursor=0;
#ifdef serialDSP      
                      Serial.print("Menu: ");
#endif                      
                    } else if (menuLayer==MENU_ACTION){
                      actionCursor++;
                      if(actionCursor>1) actionCursor=0;
                    
                    } else { //menuLayer==MENU_TIME
                      if(editHour){
                        alarmList[cursor].hour++;
                        if(alarmList[cursor].hour>23) alarmList[cursor].hour=0;
                      } else {
                        alarmList[cursor].minute++;
                        if(alarmList[cursor].minute>59) alarmList[cursor].minute=0;
                      }
                      alarmchg = true;
                     }
                    drawUI();
//================================                                                       

                    break;

                case EVENT_DOUBLE:
#ifdef serialDSP      
                    Serial.println("雙擊");
#endif
                if (menuLayer==MENU_ROOT) {

                  Volume = 0.3;//放低音量
                  struct tm ti;
                  if (getLocalTime(&ti)) {
                    xQueueSend(audioQueue, &ti, 0);
                  }
                }

//================================
                    if (menuLayer==MENU_MAIN) {
                        menuLayer=MENU_ACTION;
                        actionCursor=0;
                      
#ifdef serialDSP      
                      Serial.print("Menu: ");
#endif                      
                    } else if (menuLayer==MENU_ACTION){
                      if(actionCursor==0){
                        alarmList[cursor].active=!alarmList[cursor].active;
                      } else {
                        alarmList[cursor].active= false;
                        menuLayer=MENU_TIME;
                      } 
                      alarmchg = true;
                    } else { //menuLayer==MENU_TIME
                       editHour=!editHour;
                   }
                    drawUI();
//================================                                                       


                    break;

                case EVENT_TRIPLE:
#ifdef serialDSP      
                        Serial.println("三擊");
#endif

//================================
                    if (menuLayer==MENU_MAIN) {
//                        cursor++;
//                        if(cursor>=MAX_ALARMS) cursor=0;
//                      updatemenu();
                      
#ifdef serialDSP      
                      Serial.print("save: ");
#endif                      
                    } else if (menuLayer==MENU_ACTION){
                    
                    } else { //menuLayer==MENU_TIME
/*                      
                      if(editHour){
                        alarmList[cursor].hour--;
                        if(alarmList[cursor].hour<0) alarmList[cursor].hour=23;
                      } else {
                        alarmList[cursor].minute--;
                        if(alarmList[cursor].minute<0) alarmList[cursor].minute=59;
                      }
                      alarmchg = true;
*/                      
                    }
                    drawUI();
//================================                                                       

                    break;

                case EVENT_LONG:
#ifdef serialDSP      
                    Serial.println("Entering Deep Sleep");
#endif                    
//================================
                    if (menuLayer==MENU_ROOT) {
                      menuLayer=MENU_MAIN;
                    } else if (menuLayer==MENU_MAIN) {
                      if(alarmchg)saveAlarms();
                      menuLayer=MENU_ROOT;
                    } else if (menuLayer==MENU_ACTION){
                      menuLayer=MENU_MAIN;
                    
                    } else { //menuLayer==MENU_TIME
                      menuLayer=MENU_ACTION;
                    }
                    drawUI();
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
/*
        // 2. 定期檢查高水位線 
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);        
        // 如果剩餘空間過小（例如少於 20 words），發出警告
        if(uxHighWaterMark < 20) {
            Serial.printf("WARNING: Stack running low! Only %ld words left.\n", uxHighWaterMark);
        } else {
            Serial.printf("Current ButtonEventTask: %ld words\n", uxHighWaterMark);
        }
*/        
///////////////////////////////////        
    }
}


void ntpSyncTask(void *pvParameters) {
    struct tm timeinfo;
    char timeStr[10]; // 準備一個空間存放 "HH:MM:SS" (至少 9 位元組)
    bool sync_ok = false;
    time_t now;
    time(&now);

      
    if(lastSyncTimestamp != 0){
      getLocalTime(&timeinfo);
#ifdef simp1306      
    if(oledrdflag){
      oled.println("Timestamp!=0");
      oled.println("Get RTC time:");
      oled.println(&timeinfo, "%H:%M:%S");
      }
  #else
      // 使用 strftime 將 timeinfo 轉換為指定格式字串
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);       
      u8g2.drawStr(40, 18, timeStr); // 繪製轉換後的字串
      u8g2.sendBuffer();
#endif     

      Serial.printf("Get RTC time:");      
      Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");      
      }

 while (1) {
     

    // 判斷是否需要同步 (24小時到了，或是之前失敗過且已經過了一段時間)
    if (lastSyncTimestamp == 0 || (unsigned long)(now - lastSyncTimestamp) >= SYNC_INTERVAL) {
        
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        
#ifdef simp1306      
    if(oledrdflag){
      oled.println("[Task] Wait Wifi");
      }
#endif     
        Serial.println("[Task] 嘗試連接 WiFi...");

        // 嘗試自動連線（使用已儲存的 WiFi 帳密）
        int connectAttempts = 0;
        WiFi.begin();
        while (WiFi.status() != WL_CONNECTED && connectAttempts < 30) {
            vTaskDelay(pdMS_TO_TICKS(500));
            connectAttempts++;
            Serial.print(".");
        }
        
        // 如果連線失敗，進入 SmartConfig 模式
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\n[Task] WiFi 連線失敗，進入 SmartConfig 配網模式...");
            WiFi.mode(WIFI_AP_STA);
            WiFi.beginSmartConfig();
            
            int scAttempts = 0;
#ifdef simp1306      
            if(oledrdflag){
              oled.println("SmartConfig...");
              }
#endif              
            while (!WiFi.smartConfigDone() && scAttempts < 120) {
                vTaskDelay(pdMS_TO_TICKS(500));
                scAttempts++;
                Serial.print("+");
            }
            
            if (WiFi.smartConfigDone()) {
                Serial.println("\n[Task] SmartConfig 完成，等待 WiFi 連線...");
                connectAttempts = 0;
                while (WiFi.status() != WL_CONNECTED && connectAttempts < 30) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    connectAttempts++;
                }
            } else {
                Serial.println("\n[Task] SmartConfig 超時");
            }
            
            WiFi.mode(WIFI_STA);
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            // --- 連線成功處理 ---
#ifdef simp1306      
    if(oledrdflag){
       oled.println("[Task] Sync NTP..");
      }
#endif     
            Serial.println("[Task] WiFi 已連線，同步 NTP...");
            configTime(28800, 0, "pool.ntp.org","time.nist.gov"); // 設定台灣時區

            int retry = 0;
//            while(!getLocalTime(&timeinfo) && retry < 20) {
//                vTaskDelay(pdMS_TO_TICKS(500));
//                retry++;
//            }
while (retry < 30) {
    // 1. 檢查 SNTP 狀態是否真的變為「已完成」
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        if (getLocalTime(&timeinfo) && timeinfo.tm_year > (2020 - 1900)) {
            sync_ok = true;
            break; 
        }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    retry++;
}

//            if(retry < 20) {
            if (sync_ok) {
                time(&now); //獲取的是 Unix 時間戳記（秒）
                
/////////設定校準係數/////////
    uint32_t currentTick = millis();

    if (lastNTPTime != 0) {
        // 計算實際經過的秒數 (NTP) 與 RTC 認為經過的秒數
        double actualElapsed = difftime(now, lastNTPTime);
        double rtcElapsed = (currentTick - lastRTCTick) / 1000.0;

        if (rtcElapsed > 60) { // 確保時間夠長，計算才精準 (例如超過一分鐘)
            driftFactor = (float)(actualElapsed / rtcElapsed);
            Serial.printf("[Calibrate] 新校正係數: %.6f\n", driftFactor);
        }
    }

    // 更新基準點
    lastNTPTime = now;
    lastRTCTick = currentTick;
//    lastSyncTimestamp = now; 

////////////////////////////                

                lastSyncTimestamp = now;
                syncFailedCount = 0; // 重置失敗計數
                Serial.println("[Task] NTP 同步成功");
                Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
#ifdef simp1306      
                if(oledrdflag){
                  oled.println("[Task] NTP update OK!");
                  oled.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
                  }
#else
                strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);       
                u8g2.drawStr(40, 42, timeStr); // 繪製轉換後的字串
                u8g2.sendBuffer();
                  
#endif     
              SYNC_INTERVAL =  12 * 60 * 60;   
              
              startWebServer();
              delay(100);
              String ip = WiFi.localIP().toString();
              Serial.println("Web UI: http://" + ip);
#ifndef simp1306              
              u8g2.clearBuffer();
              u8g2.setFont(u8g2_font_ncenB08_tr);
              u8g2.drawStr(0, 20, "Web Server ON");
              u8g2.drawStr(0, 40, ip.c_str());
              u8g2.sendBuffer();
#endif              

            } else {
              SYNC_INTERVAL =  60 * 60;              
            }  
            
            while (WiFi.status() == WL_CONNECTED) {
                if (server) server->handleClient();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // --- 強制關閉無線電 (不論成功與否都關閉以省電) ---
        if (server) {
            server->stop();
            delete server;
            server = NULL;
        }
//        wm.disconnect();          // 讓 WiFiManager 釋放資源
        WiFi.disconnect(true);    // 斷開連接並清除設定
        WiFi.mode(WIFI_OFF);      // 關閉 WiFi 無線電模組
    }


    
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYNC_INTERVAL * 1000)); 
    }
 //   vTaskDelete(NULL);
}


void setup() {
    delay(500);
    Serial.begin(115200);
//////////////////////////////////////////////////////////////    
    Serial.printf("File: %.*s\n", __FNAME_LEN__, __FILENAME__);

#ifdef simp1306      
    if(lastSyncTimestamp == 0){
      initSSD1306(true);
      } else {
      initSSD1306(false);
      }
#else
   u8g2.begin();
   delay(50);
 // 這裡展示正常畫面
  drawUI();      
#endif
      
      
    
    if (!SPIFFS.begin(false))
    {
        Serial.println("SPIFFS mount failed");
        return;
    } else {
        listSPIFFSFiles();  //列出SPIFFS裡的檔案有哪一些
        loadAlarms();
    }


    setupI2S();

    // 建立語音播報隊列 Queue (長度 1，因為報時通常不需要排隊)
    audioQueue = xQueueCreate(3, sizeof(struct tm));
    
    // 建立語音播放 Task (給予足夠的 Stack 空間)
    xTaskCreate(audioTask, "Audio_Task", 4096, NULL, 1, NULL);
    
    // 啟動鬧鐘監控 Task
    xTaskCreate(alarmCheckTask, "Alarm_Task", 2048, NULL, 1, NULL);

//////////////////////////////////////////////////////////////
// 設定環境變數為台灣時區 (CST-8 代表 GMT+8),也可以使用 configTime(28800, 0, "pool.ntp.org");//不聯網也可以設定
    setenv("TZ", "CST-8", 1);
    tzset();
    

//按鍵通知訊息需要是跨任務通知,所以在setup中建立
  buttonEventQueue = xQueueCreate(5, sizeof(ButtonEvent));                    //建立按鍵通知訊息
// 按鍵之後的處理任務
  xTaskCreate(buttonEventTask, "ButtonEventTask", 2048, NULL, 2, NULL);       //
  xTaskCreate(ttp223Task, "TTP223_Touch", 3072, NULL, 1, NULL);
  
// 啟動 NTP 同步 Task (分配較大的堆疊空間給 WiFiManager)
    xTaskCreate(
        ntpSyncTask,    // Task 函數
        "NTP_Task",     // 名稱
        8192,           // WiFiManager 執行時會消耗較多記憶體，因此 xTaskCreate 的 Stack 指定為 8192 以上較為穩定
        NULL,           // 參數
        1,              // 優先度
        &ntpTaskHandle  // Handle
    );



    // 檢查喚醒原因
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    switch(wakeup_reason){
      case ESP_SLEEP_WAKEUP_TIMER://timer wake up
        {
            if(!plaied){
                  plaied =true;
                  Volume = 0.3;//放低音量
                  struct tm ti;
                  if (getLocalTime(&ti)) {
                    xQueueSend(audioQueue, &ti, 0);
                  }
                } 
      
            if (ntpTaskHandle != NULL) {
                Serial.println("強制 NTP 同步！");
                SYNC_INTERVAL =  3;
                xTaskNotifyGive(ntpTaskHandle); // 直接喚醒 ntpSyncTask
            }
            
            WiFi.mode(WIFI_STA);
            WiFi.setTxPower(WIFI_POWER_8_5dBm);
            WiFi.begin();
            int waitCount = 0;
            while (WiFi.status() != WL_CONNECTED && waitCount < 30) {
                vTaskDelay(pdMS_TO_TICKS(500));
                waitCount++;
                Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                configTime(28800, 0, "pool.ntp.org","time.nist.gov");
                startWebServer();
                webServerEnabled = true;
                String ip = WiFi.localIP().toString();
                Serial.println("Web UI: http://" + ip);
#ifndef simp1306              
                u8g2.clearBuffer();
                u8g2.setFont(u8g2_font_ncenB08_tr);
                u8g2.drawStr(0, 20, "Timer Wake + Web");
                u8g2.drawStr(0, 40, ip.c_str());
                u8g2.sendBuffer();
#endif              
            }
      
#ifdef simp1306      
            if(oledrdflag){
              oled.println("wakeup by timer");
              }
#endif            
    Serial.println("wakeup by timer");
        }
        break;
        case ESP_SLEEP_WAKEUP_GPIO: 
        {
            Serial.println("GPIO 喚醒報時");
#ifdef simp1306      
            if(oledrdflag){
              oled.println("wakeup by GPIO");
              }
#endif            
            Serial.println("wakeup by GPIO, connecting WiFi...");
            
            WiFi.mode(WIFI_STA);
            WiFi.setTxPower(WIFI_POWER_8_5dBm);
            WiFi.begin();
            int waitCount = 0;
            while (WiFi.status() != WL_CONNECTED && waitCount < 30) {
                vTaskDelay(pdMS_TO_TICKS(500));
                waitCount++;
                Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                configTime(28800, 0, "pool.ntp.org","time.nist.gov");
                startWebServer();
                webServerEnabled = true;
                String ip = WiFi.localIP().toString();
                Serial.println("Web UI: http://" + ip);
#ifndef simp1306              
                u8g2.clearBuffer();
                u8g2.setFont(u8g2_font_ncenB08_tr);
                u8g2.drawStr(0, 20, "GPIO Wake + Web");
                u8g2.drawStr(0, 40, ip.c_str());
                u8g2.sendBuffer();
#endif              
            }
        }
        break;  
    default: Serial.printf("非睡眠喚醒 (原因代碼: %d)\n", wakeup_reason);
        break;
        
    }

////////////////////////////////////////////////////////////////
/*
//************** 以 下 為 測 試 碼 ***************
#ifdef simp1306      
     oled.println("play test..");
#endif     

// 獲取時間並丟入 Queue 觸發播放
    Volume = 0.7;//放低音量
    struct tm ti;
    if (getLocalTime(&ti)) {
        xQueueSend(audioQueue, &ti, 0);
    }


*/    
////////////////////////////////////////////////////////////////
    
    struct tm ti;
//    if (getLocalTime(&ti)) {
//        if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
//           Serial.println("手動開機模式：120 秒後自動計算下次鬧鐘並進入睡眠...");
            
            for(int i = 180; i > 0; i--) {
                while (menuLayer != MENU_ROOT) {
                    // 如果正在設定，就停在這裡，每 100ms 檢查一次是否結束
                    vTaskDelay(pdMS_TO_TICKS(100)); 
                }
                
                if (webServerEnabled && server) {
                    server->handleClient();
                }
                
                if(i % 10 == 0){
                  Serial.printf("倒數 %d 秒...\n", i);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            
/*            
            //如果超過1小時就改為1小時
            if (sleepSeconds > 3600) {
              sleepSeconds = 3600; 
#ifdef simp1306      
              if(oledrdflag){
                oled.printf("force %ld s\n",sleepSeconds);
                }
#endif                 
              Serial.printf("強制醒來計時");
                        
              } else {
#ifdef simp1306      
              if(oledrdflag){
                oled.printf("wakeup at %ld s\n", sleepSeconds);
                }
#endif                 
                  Serial.printf("預計醒來計時");
                
              }
*/              
            // 倒數結束，手動觸發一次「計算睡眠」
            getLocalTime(&ti);
            uint64_t sleepSeconds = getSecondsToNextAlarm(&ti);
            Serial.println((unsigned long long)sleepSeconds);
            Serial.flush(); // 確保序列埠傳完

while (1){  
    if(menuLayer == MENU_ROOT){                
#ifdef simp1306      
              if(oledrdflag) oled.ssd1306WriteCmd(SSD1306_DISPLAYOFF);
#else
  // 畫面全黑
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  // 進入休眠模式
  u8g2.setPowerSave(1); 
              
#endif                

// 假設補償係數為 1.0002 (依據你的實測誤差調整)
float compensation = 1.002; //太早醒就增加,如果是慢醒就減少
//uint64_t adjustedSleep = (uint64_t)((sleepSeconds-20) * compensation);
uint64_t adjustedSleep = (uint64_t)(sleepSeconds * compensation);
esp_sleep_enable_timer_wakeup(adjustedSleep * 1000000ULL);            
//            esp_sleep_enable_timer_wakeup((sleepSeconds-10) * 1000000ULL);

            esp_deep_sleep_enable_gpio_wakeup(1 << 2, ESP_GPIO_WAKEUP_GPIO_HIGH);                      
            i2s_zero_dma_buffer(I2S_PORT);
            i2s_driver_uninstall(I2S_PORT);
            
            if (webServerEnabled && server) {
                server->stop();
                webServerEnabled = false;
            }
             esp_deep_sleep_start();
    }        
    vTaskDelay(pdMS_TO_TICKS(50)); 

} //while(1) end            
//        }
//    }


/*
    // 等待 Task 完成（或是您可以在 Task 內執行深度睡眠）
    // 這裡示範簡單等待後進入睡眠
//    vTaskDelay(pdMS_TO_TICKS(5000)); 
    
//************** 以 下 為 測 試 碼 ***************
#ifdef simp1306      
     oled.println("play test..");
#endif     

// 獲取時間並丟入 Queue 觸發播放
    Volume = 0.3;//放低音量
    struct tm ti;
    if (getLocalTime(&ti)) {
        xQueueSend(audioQueue, &ti, 0);
    }
*/
    
/*    
////////////////////////////////////////////////
  for(int i = 0; i < 120; i++) {
    Serial.printf("距離睡眠還有 %d 秒...\n", 120 - i);
#ifdef simp1306      
//    oled.printf("%3.3d...", 120 - i);
//    oled.printf("\b\b\b\b\b\b");
#endif     
    delay(1000); 
  }
    Serial.println("進入深度睡眠...");
#ifdef simp1306      
    oled.printf("Enter Deep sleep...");
#endif     
    esp_sleep_enable_timer_wakeup(30 * 1000000); // 30 秒後喚醒測試
    i2s_zero_dma_buffer(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);    
    esp_deep_sleep_start();
*/    
}

void loop(){vTaskDelay(pdMS_TO_TICKS(10));}
