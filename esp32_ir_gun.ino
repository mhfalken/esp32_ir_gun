/*
  Michael Hansen 10-2023
  IR gun (tx + rx)

  Board (meta PCB): 
    ESP32 Lolin32 (lite), w. charger app. 380mA
    TFT Display 0.96" 80x160 color, 3V3, GND, IO4 (RES), IO5 (CS), IO18 (CLK), IO23 (SDA), IO2 (DC)
    Forstærker, IO25
    4x NEO pixels, IO14
    IR TX nFET, IO19
    Motor nFET, IO13
    VBat måler, IO34 (R/R)
    IR RX IO32, IO33, IO35

  Arduino board setup: 
    ESP32-Lite:    WEMOS LONIN32 Lite

  Sounds: 
    MP3, Mono, 44100Hz, 32kb/s
    .mp3-> .h file, use: Bin2C.exe
*/

#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>    // Core graphics library
#include <Adafruit_ST7735.h> // Hardware-specific library for ST7735
#include <SPI.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h> 
#include <Fonts/FreeMonoBold9pt7b.h> 
#include <Fonts/FreeMono9pt7b.h> 

#include <esp_task_wdt.h>

// If set enable communication between guns (work in progress)
#define COMM

// GPIO
#define GPO_haptic         13  // Vibrations motor (active high)
#define GPI_vbat           34  // LiPo voltage (4.7k/4.7k)
#define GPI_irRx0          33  // IR Rx LED
#define GPI_irRx1          32  // IR Rx LED
#define GPI_irRx2          35  // IR Rx LED (AUX)
#define GPO_irShoot        19  // IR Tx LED (active high)
#define GPI_swShootN       16  // Switch shoot (active low)
#define GPI_swSetupN       27  // Switch setup/config (active low)
#define GPO_neo            14  // 4x NEO pixels
#define GPO_pwmDac         17  // DAC for LED TX power
#define GPO_amplifierOn    12  // Used to disable the amplifier (avoid WiFi noise)
// GPIO TFT Display
#define TFT_DC              2
#define TFT_RST             4  // Or set to -1 and connect to Arduino RESET pin
#define TFT_CS              5
#define TFT_SCLK           18  // SCL
#define TFT_MOSI           23  // SDA

/* Indirect GPIO
  Sound out:    25
  Build-in LED  22
*/

//Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST); // HW SPI

// color definitions  BGR: xxxx.x xxx.xxx x.xxxx
#define TFT_COLOR_black      0x0000
#define TFT_COLOR_gray       0xC618   //1100.0 110.000 1.1000
#define TFT_COLOR_darkgray   0x4208   //0100.0 010.000 0.1000
#define TFT_COLOR_red        0x001F
#define TFT_COLOR_orange     0x03F8   //0000.0 011.111 1.1000
#define TFT_COLOR_blue       0xF800
#define TFT_COLOR_green      0x07E0
#define TFT_COLOR_white      0xFFFF
#define TFT_COLOR_yellow     0x07FF
#define TFT_COLOR_top        0x1863
#define TFT_COLOR_erase      0x0000
#define TFT_COLOR_select     0xffff

#define LED_cnt   5  // NEO pixels
Adafruit_NeoPixel neos(LED_cnt, GPO_neo, NEO_GRB + NEO_KHZ800);

// LED colors
#define STATUS_LED_green    0x008000
#define STATUS_LED_menu     0x000080
#define STATUS_LED_hit      0xff8080  // Enemy has hit your gun
#define STATUS_LED_kill     0x00ff00  // You have hit another gun
#define STATUS_LED_charge1  0x808000  // Yellow
#define STATUS_LED_charge2  0x008000  // Green

// Globals
#define GUN_ID_CNT      16  // Max 16 guns

#define GUN_MODE_tag    0
#define GUN_MODE_target 1
uint8_t gunMode;           // Tag or target mode
const char *gunMode2Txt[] = {"TAG", "TARGET"};

uint8_t gunTagId;        // [0-15] Gun TX tag (IR 4 bits) 
const char *gunTagId2Txt[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"};
uint8_t gunTagGroup;     // [0-2] Gun TX group (IR 1 bit)
const char *gunTagGroup2Txt[] = {"A", "B", "N"};

uint16_t gunShotsUsed;
#define GUN_SHOOT_MODE_semi      0
#define GUN_SHOOT_MODE_burst     1
#define GUN_SHOOT_MODE_automatic 2
// gunShootMode is disabled !!
uint8_t gunShootMode;
const char *gunShootMode2Txt[] = {"Semi", "Burst", "Auto"};
const char *gunShootMode2Ch[] = {"S", "B", "A"};
uint8_t soundLevel;    // 0-3
#define SOUND_LEVEL_off 3
const char *soundLevel2Txt[] = {"Low", "Mid", "High", "Off"};
uint8_t chargeMode;
const char *chargeMode2Txt[] = {"Off", "On"};
uint8_t irLedTxLevel;   // 0-3
const char *irLedTxLevel2Txt[] = {"1", "2", "3", "4"};
uint16_t irLedTxLevel2Pwm[] = {60, 80, 130, 150};  // 80, 260, 760, 1000 mA
uint8_t logMode;
const char *logMode2Txt[] = {"Off", "On"};
#define WIFI_MODE_off    0
#define WIFI_MODE_server 1
#define WIFI_MODE_client 2
uint8_t wifiMode;
const char *wifiMode2Txt[] = {"Off", "Server", "Client"};

uint8_t hitsIdCnt[GUN_ID_CNT];  // Number of hits by enemy
volatile bool gunIsHit;   // Set in the RX poller when a hit is detected
                          // Cleared in main loop
uint32_t gunShootHoldoffMs;   // Used to block gun from shoot after it is hit
uint16_t configTimeSOffset;

// IR LED puls
#define FREQ_40kHz  40000   // TAG modulation freq. Must match IR RX
#define TAG_pulsMs    500   // TAG pulse length (Mark/space)
#define TAG_channel     0   // LED modulation
#define DAC_channel     1   // LED power PWM->DAC

// EEPROM
#define EEPROM_INDEX_gunMode       0
#define EEPROM_INDEX_gunTagId      1
#define EEPROM_INDEX_gunTagGroup   2
#define EEPROM_INDEX_soudLevel     3
#define EEPROM_INDEX_irLedTxLevel  4
#define EEPROM_INDEX_wifiMode      5

// MENU structures
typedef struct {
  uint8_t *value;
  uint8_t maxValue;
  const char **txt;
} menuItem_t;

typedef struct {
  const char *name;
  menuItem_t *item;
} menuLine_t;

// Log system

#define LOG_ENTRY_CNT       4  // Max number of entries
#define LOG_ENTRY_lineSize 20  // Max line size 
char logList[LOG_ENTRY_CNT][LOG_ENTRY_lineSize];
uint8_t logCnt;    // Total number of log entries
uint8_t logIndex;  // Current vacant entry

uint8_t irRxErrors;

int tftLine[4] = {16, 35, 54, 73};

// RMT IR RX
#define RMT_MEM_RX RMT_MEM_192
#define RMT_TICK_ns 1000
rmt_data_t irRx0Data[64];
rmt_data_t irRx1Data[64];
rmt_obj_t *irRx0Recv = NULL;
rmt_obj_t *irRx1Recv = NULL;
// TX not used, but needed for RX to work !!
rmt_data_t irTxData[64];
rmt_obj_t *irTxSend = NULL;

static EventGroupHandle_t irRx0Events, irRx1Events;

#ifdef COMM
// Gun info and statistics
typedef struct {
  IPAddress ipAddr;           // GUN IP address
  uint8_t connCnt;            // Number of connects (must be 1, guard against reset)
  uint8_t groupId;            // Team id
  uint8_t shots;              // Number of shots used
  uint8_t kills[GUN_ID_CNT];  // Kills for this gun
} gunInfo_t;

gunInfo_t gunInfo[GUN_ID_CNT];

IPAddress hostIP(192, 168, 4, 1);  // Server IP address
bool wifiStatus;  // Server started OR client connected
#endif

/* WARNING
  Arduino can't handle typedef below this line if used in functionheaders!!!!!
*/

// Round float to 1 decimal
float FloatRound1(float f)
{
  return roundf(f*10+0.5)/10;
}

// *** Sound **********************************************

#include "AudioFileSourceSD.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#define CONFIG_I2S_BCK_PIN 12
#define CONFIG_I2S_LRCK_PIN 0
#define CONFIG_I2S_DATA_PIN 2
#define Speak_I2S_NUMBER I2S_NUM_0

void PollDisplay(bool forceUpdate);

void LogPrint()
{
  int i, btnAction;

  DisplayClr();
  for (i=0; i<logCnt; i++) {
    if (i >= LOG_ENTRY_CNT)
      break;
    printf("%s\n", logList[i]);
    tft.setCursor(0, tftLine[i]);
    tft.print(logList[i]);
  }
  while ((btnAction = ButtonAction()) == 0)
    DelayMsPoll(0);
  PollDisplay(true);
}

// Global audio variables
AudioGeneratorMP3 *audioMp3;
AudioFileSourcePROGMEM *audiofileProg;
AudioOutputI2S *audioOut;

#include "shot.h"
#include "hit.h"
#include "lowbatt.h"

typedef struct {
  const unsigned char *data;
  uint32_t size;  
} soundInfo_t; 

soundInfo_t soundInfo[] = {
  {shotSound, sizeof(shotSound)},
  {hitSound, sizeof(hitSound)},
  {lowbattSound, sizeof(lowbattSound)},
};

#define SOUND_shot      0
#define SOUND_hit       1
#define SOUND_lowBatt   2

bool soundPlaying;  // True while sound is playing

void SoundInit(void)
{
  audioOut = new AudioOutputI2S(0, 1, 64); // 2nd parameter: 0: External DAC, 1: INTERNAL_DAC
                                           // 64: buffers for app. 160 ms of delay between polls
  audioOut->SetOutputModeMono(true);
  if (soundLevel == SOUND_LEVEL_off)
    audioOut->SetGain(1);  // Low. (Off, but some sounds are allowed)
  else
    audioOut->SetGain(soundLevel+1);  // 1-3
  audioMp3 = new AudioGeneratorMP3();
}

void Mp3Play(int index)
{
  if (audioMp3->isRunning()) {
    audioMp3->stop();
    delete audiofileProg;
  }
  printf("Play MP3: %i : %i\n", index, soundInfo[index].size);
  digitalWrite(GPO_amplifierOn, 1);
  audiofileProg = new AudioFileSourcePROGMEM(soundInfo[index].data, soundInfo[index].size);
  audioMp3->begin(audiofileProg, audioOut);
  soundPlaying = true;
}


#include "driver/i2s.h"

void PollSound(void)
{
  if (audioMp3->isRunning()) {
    if (!audioMp3->loop()) {
      printf("MP3 done\n");
      audioMp3->stop();
      delete audiofileProg;
      soundPlaying = false;
      digitalWrite(GPO_amplifierOn, 0);
    }
  }
}

void DelayMsPoll(uint32_t delayMs)
{
  uint32_t waitMs = millis()+delayMs;
  delay(1);
  do {
    PollSound();
  } while (millis() < waitMs);
}

float LiPoVoltage()
{
  static float vbat = -1;
  float vin;
  vin = analogRead(GPI_vbat);
  //printf("Vin= %5.2f\n", vbat);
  vin = (vin / 4096) * 3.48;  // Manual tested
  vin *= 2;  // Resisters on board

  if (vbat > 0)
    vbat = vbat + 0.3*(vin - vbat);  // Calman filter
  else
    vbat = vin;

  //printf("VBat= %5.2f\n", vbat);
  return vbat;
}

// *** Haptic control **********************************************

uint32_t hapticOffMs;

void HapticSetMs(int delayMs, bool forceWait = false)
{
  digitalWrite(GPO_haptic, 1);
  hapticOffMs = millis() + delayMs;
  if (forceWait) 
    while (PollHaptic());
}

bool PollHaptic()
{
  if (millis() > hapticOffMs) {
    digitalWrite(GPO_haptic, 0);
    return false;
  }
  return true;
}

// *** LED control **********************************************

uint32_t ledColor;
uint32_t ledOffMs;
uint32_t ledNextMs;
uint32_t ledFlashMs;

void LedColorSet(uint32_t color)
{
  uint8_t r, g, b;

  b = color & 0xff;
  color >>= 8;
  g = color & 0xff;
  color >>= 8;
  r = color & 0xff;
  for (int i=0; i<LED_cnt; i++)
    neos.setPixelColor(i, r, g, b);
  neos.show();
}

void LedStatusSet(uint32_t color, uint32_t timeoutMs, uint32_t flashMs=0)
{
  LedColorSet(color);
  ledColor = color;
  ledOffMs = millis() + timeoutMs;
  ledFlashMs = flashMs;
  if (ledFlashMs > 0)
    ledNextMs = ledFlashMs + millis();
  else
    ledNextMs = 0xffffffff;    
}

void PollLed()
{
  static bool colorOn;

  if (millis() < ledOffMs) {
    if (millis() > ledNextMs) {
      if (colorOn == false) 
        LedColorSet(0);
      else        
        LedColorSet(ledColor);
      colorOn = !colorOn;        
      ledNextMs = ledFlashMs + millis();
    }
  }
  else {
    LedColorSet(0);  // Off
    colorOn = false;
  }
}

// IR LED power control

void IrLedPowerSet(uint8_t level)
{
  ledcWrite(DAC_channel, irLedTxLevel2Pwm[level]);  // DAC TX power
}

// *** Setup **********************************************

void RmtSetup()
{
  irRx0Events = xEventGroupCreate();
  irRx1Events = xEventGroupCreate();
  if ((irTxSend = rmtInit(GPO_irShoot, RMT_TX_MODE, RMT_MEM_64)) == NULL)
  {  // This is needed in order for RX to work !!!!!
    Serial.println("init sender failed\n");
  }

  if ((irRx0Recv = rmtInit(GPI_irRx0, RMT_RX_MODE, RMT_MEM_RX)) == NULL)
  {
    Serial.println("IR RX0 failed\n");
  }
  if ((irRx1Recv = rmtInit(GPI_irRx1, RMT_RX_MODE, RMT_MEM_RX)) == NULL)
  {
    Serial.println("IR RX1 failed\n");
  }
  rmtSetTick(irRx0Recv, RMT_TICK_ns);
  rmtSetTick(irRx1Recv, RMT_TICK_ns);
  rmtSetRxThreshold(irRx0Recv, 1200);
  rmtSetRxThreshold(irRx1Recv, 1200);
  rmtReadAsync(irRx0Recv, irRx0Data, 12, irRx0Events, false, 0);
  rmtReadAsync(irRx1Recv, irRx1Data, 12, irRx1Events, false, 0);
}


void setup()
{
  int i;
  char s[50];

  Serial.begin(115200);
  tft.initR(INITR_MINI160x80); // Init ST7735S chip, black tab
  tft.invertDisplay(true);
  // Clear screen - hack
  tft.setRotation(0);
  tft.fillScreen(TFT_COLOR_black);
  tft.setRotation(1);
  tft.fillScreen(TFT_COLOR_black);
  tft.setRotation(2);
  tft.fillScreen(TFT_COLOR_black);
  tft.setRotation(3);
  tft.fillScreen(TFT_COLOR_black);
  tft.setRotation(3); 

  tft.setTextSize(1);
  tft.setFont(&FreeSansBold9pt7b);
  tft.setCursor(65, tftLine[0]);
  tft.setTextColor(TFT_COLOR_white);
  tft.print("SW");
  tft.setCursor(20, tftLine[1]);
  tft.setTextColor(TFT_COLOR_yellow);
  tft.print(__DATE__);
  tft.setCursor(40, tftLine[2]);
  tft.setTextColor(TFT_COLOR_gray);
  tft.print(__TIME__);

  printf("\nIR GUN\n");
  printf("Software date: %s, %s\n", __DATE__, __TIME__);
  delay(2000);

  RmtSetup();
  // Port setup
  pinMode(GPI_swShootN, INPUT_PULLUP);
  pinMode(GPI_swSetupN, INPUT_PULLUP);
  digitalWrite(GPO_irShoot, 0);
  pinMode(GPO_irShoot, OUTPUT);
  digitalWrite(GPO_haptic, 0);
  pinMode(GPO_haptic, OUTPUT);
  pinMode(GPO_pwmDac, OUTPUT);
  pinMode(GPO_amplifierOn, OUTPUT);

  // Eeprom restore
  EEPROM.begin(6);
  // Gun mode
  gunMode = EEPROM.read(EEPROM_INDEX_gunMode);
  if (gunMode == 0xff)
    gunMode = GUN_MODE_tag;
  // Gun id
  gunTagId = EEPROM.read(EEPROM_INDEX_gunTagId);
  if (gunTagId == 0xff)
    gunTagId = 0;
  // Gun group
  gunTagGroup = EEPROM.read(EEPROM_INDEX_gunTagGroup);
  if (gunTagGroup == 0xff)
    gunTagGroup = 0;
  // Sound level
  soundLevel = EEPROM.read(EEPROM_INDEX_soudLevel);
  if (soundLevel == 0xff)
    soundLevel = 1;
  // IR LED TX power
  irLedTxLevel = EEPROM.read(EEPROM_INDEX_irLedTxLevel);
  if (irLedTxLevel == 0xff)
    irLedTxLevel = 0;
  // WiFi mode
  wifiMode = EEPROM.read(EEPROM_INDEX_wifiMode);
  if (wifiMode == 0xff)
    wifiMode = WIFI_MODE_off;
  SoundInit();
  TagSetup(); // PWM setup
  IrLedPowerSet(irLedTxLevel);
  configTimeSOffset = millis()/1000;
  esp_task_wdt_init(30, false);  // Disable watchdog
#if 0
  hitsIdCnt[1]=122;  
  hitsIdCnt[2]=244;  
  hitsIdCnt[3]=166;  
  hitsIdCnt[4]=188;  
  hitsIdCnt[5]=199;  
#endif  
#ifdef COMM
  CommSetup();
#endif
}


// *** IR Disc **********************************************

void IrShootTarget()
{
  //printf("IR shoot\n");
  delay(1);  // Fix unknown delay error!
  ledcWrite(TAG_channel, 255);
  delay(1);
  ledcWrite(TAG_channel, 0);
  delay(1);
  ledcWrite(TAG_channel, 255);
  delay(1);
  ledcWrite(TAG_channel, 0);
}

// *** IR TX TAG **********************************************

void TagSetup()
{
  ledcSetup(TAG_channel, FREQ_40kHz, 8);
  ledcAttachPin(GPO_irShoot, TAG_channel);

  ledcSetup(DAC_channel, FREQ_40kHz, 8);
  ledcAttachPin(GPO_pwmDac, DAC_channel);
}

// L-H
void TagMark()
{
  ledcWrite(TAG_channel, 0);
  delayMicroseconds(TAG_pulsMs);
  ledcWrite(TAG_channel, 128);
  delayMicroseconds(TAG_pulsMs);
}

// H-L
void TagSpace()
{
  ledcWrite(TAG_channel, 128);
  delayMicroseconds(TAG_pulsMs);
  ledcWrite(TAG_channel, 0);
  delayMicroseconds(TAG_pulsMs);
}

void IrSendValue(uint8_t value, uint8_t bitLength)
{
  int i;
  uint8_t bit;

  bit = 1<<(bitLength-1);
  for (i=0; i<bitLength; i++) {
    if (value & bit)
      TagMark();
    else
      TagSpace();
    value <<= 1;
  }
}

void IrShootTag()
{
  // Pre-ample
  TagMark();
  // Group
  IrSendValue(gunTagGroup, 1);  // If None just send A (0)
  // Gun
  IrSendValue(gunTagId, 4);
  // Post-ample
  TagSpace();
}


void PollTriggerShoot()
{
  static uint32_t nextShotMs;
  static int shots;

  if (gunShootHoldoffMs > millis()) 
    return;

  if ((digitalRead(GPI_swShootN) == 0) && (nextShotMs < millis()) && (shots > 0)) {
    if ((gunShootMode == GUN_SHOOT_MODE_semi) && (shots < 3))
      return;

    nextShotMs = millis() + 200;
    if (gunMode == GUN_MODE_tag)
      IrShootTag();
    else      
      IrShootTarget();
    gunShotsUsed++;
    if (soundLevel != SOUND_LEVEL_off)
      Mp3Play(SOUND_shot);

    if (gunShootMode != GUN_SHOOT_MODE_automatic)
      shots--;
  }
  if (digitalRead(GPI_swShootN) == 1) 
    shots = 3;
}


// *** IR RX TAG **********************************************

// Receive MSB first
int TagDecode(rmt_data_t *rxData)
{
  uint32_t bits;
  uint32_t v, val;
  int i, cnt;
  uint32_t hl;

  bits = 0;
  cnt = 0;
  for (i = 0; i < 20; i++)
  {
    v = (rxData[i / 2].val >> ((i % 2) * 16)) & 0xffff;
    val = v & 0x7fff;
    hl = (v >> 15)?0:1; // High or Low (RX is inverted)
    if (val == 0)
      break;
    if ((val < 400) || (val > 1200) ||    // Legal [400-600] [800-1200]
        ((val > 600) && (val < 800)))
      return -1;

    bits <<= 1;
    bits |= hl;
    cnt++;
    if (val > 700)
    {
      bits <<= 1;
      bits |= hl;
      cnt++;
    }
    //printf("V= %4x, bits= %3x\n", v, bits);
  }
  if (cnt != 12)
    return -1;
#if 0
  printf("cnt= %2i,  %5x: ", cnt, bits);
  for (int j = cnt - 1; j >= 0; j--)
  {
    if (bits & (1 << j))
      printf("1");
    else
      printf("0");
  }
  printf("\n");
#endif  
  v = 0;
  cnt -= 1; // Remove PREAMPLE
  for (int j = cnt - 2; j > 0; j -= 2)  // Skip END
  { 
    v <<= 1;
    if (bits & (1 << j))
      v |= 1;
  }
  return v;
}


void IrxDecode(rmt_data_t *rxData, int rxNo)
{
  static uint32_t lastHitMs;
  int groupId, gunId;
  int v;
#if 0
  printf("#%i: ", rxNo);
  for (int i = 0; i < 12; i++)
  {
    printf("%4i, ", rxData[i].val & 0x7fff);
    printf("%4i, ", (rxData[i].val >> 16) & 0x7fff);
  }
  printf("\n");
#endif  
  v = TagDecode(rxData);
  if (v >= 0) {
    groupId = (v >> 4) & 1;
    gunId = v & 0xf;
    //printf("#%i: Gr= %i, Gun= %i\n", rxNo, groupId, gunId);

    if ((gunId != gunTagId) && (groupId != gunTagGroup)) {
      // No suicide, no group hit
      if (gunId < GUN_ID_CNT) {
        if (millis() > (lastHitMs + 500)) {  // Hold down
          hitsIdCnt[gunId]++;
          gunIsHit = true;
          lastHitMs = millis();
#ifdef COMM
          char packetBuffer[25];
          if (wifiMode == WIFI_MODE_client) {
            if (WifiClientConnect()) {
              sprintf(packetBuffer, "H:%i:%i:%i:h", gunTagId, gunId, hitsIdCnt[gunId]);
              UdpTx(hostIP, packetBuffer, strlen(packetBuffer)+1);
            }
          }
          if (wifiMode == WIFI_MODE_server) {
            gunInfo[gunId].kills[gunTagId] = hitsIdCnt[gunId];
            sprintf(packetBuffer, "K:%i:k", gunTagId);
            UdpTx(gunInfo[gunId].ipAddr, packetBuffer, strlen(packetBuffer)+1);
          }
#endif
        }
      }
    }
  }
  else {
    // Error
    irRxErrors++;
  }
}

void IrxPoller()
{
  if (xEventGroupWaitBits(irRx0Events, RMT_FLAG_RX_DONE, 1, 1, 0) == RMT_FLAG_RX_DONE)
  {
    IrxDecode(irRx0Data, 0);
    rmtReadAsync(irRx0Recv, irRx0Data, 12, irRx0Events, false, 0);
  }
  if (xEventGroupWaitBits(irRx1Events, RMT_FLAG_RX_DONE, 1, 1, 0) == RMT_FLAG_RX_DONE)
  {
    IrxDecode(irRx1Data, 1);
    rmtReadAsync(irRx1Recv, irRx1Data, 12, irRx1Events, false, 0);
  }
}


// *** Poll hit **********************************************

// This is used so only ONE core is accessing the sound, LEDs etc.
// Currently only ONE core is used!
void PollHit()
{
  if (gunIsHit) {
    gunShootHoldoffMs = millis() + 500;
    gunIsHit = false;
    LedStatusSet(STATUS_LED_hit, 500, 50);
    HapticSetMs(300);  // Hit
    if (soundLevel != SOUND_LEVEL_off)
      Mp3Play(SOUND_hit);
  }
}

// *** Display control **********************************************


// 0-120 minuter = 120 steps [36-156]
// 3.2V-4.2V = 50 steps [20-70]
#define PLOT_CH_xOff  36
#define PLOT_CH_yOff  20

int plotChPrevX;
int plotChPrevY;

int PlotChCalcv2y(float v)
{
  if (v < 3.2)  
    v = 3.2;
  return (v-3.2)*50;
}

void PlotChargeGrid(int x, float v)
{
  tft.fillRect(PLOT_CH_xOff, 80-PLOT_CH_yOff, 120, -50, TFT_COLOR_darkgray);
  tft.setTextColor(TFT_COLOR_gray);
  // Y
  tft.setCursor(0, 16);
  tft.print("4.2");
  tft.setCursor(0, 65);
  tft.print("3.2");
  // X
  tft.setCursor(30, 79);
  tft.print("0");
  tft.setCursor(86, 79);
  tft.print("60");
  tft.setCursor(130, 79);
  tft.print("120");

  tft.drawLine(PLOT_CH_xOff, 80-(PLOT_CH_yOff+25), PLOT_CH_xOff+120, 80-(PLOT_CH_yOff+25), TFT_COLOR_black);
  tft.drawLine(PLOT_CH_xOff+60, 80-PLOT_CH_yOff, PLOT_CH_xOff+60, 80-(PLOT_CH_yOff+50), TFT_COLOR_black);
  plotChPrevX = x;
  plotChPrevY = PlotChCalcv2y(v);
}

void PlotChargeLineXY(int x, float v)
{
  int y;
  if (x > 120)
    return;
  y = PlotChCalcv2y(v);
  tft.drawLine(plotChPrevX+PLOT_CH_xOff, 80-(plotChPrevY+PLOT_CH_yOff), 
        x+PLOT_CH_xOff, 80-(y+PLOT_CH_yOff), TFT_COLOR_yellow);
  plotChPrevX = x;
  plotChPrevY = y;
}

void DisplayClr()
{
  tft.fillScreen(TFT_COLOR_black);
}

void DisplayCharge(bool forceUpdate=false)
{
  static uint32_t updateMs=0, update60s;
  static float vbatLast=0;
  static float posX;
  float vbat;
  char s[20];

  if ((millis() > updateMs) || forceUpdate) {
    updateMs = millis() + 3000; // Next poll
    vbat = LiPoVoltage();
    vbat = FloatRound1(vbat*10)/10;
    if (forceUpdate) {
      DisplayClr();
      posX = 0;
      update60s = 0;
      PlotChargeGrid(posX, vbat);
    }
    if ((vbat != vbatLast) || forceUpdate) {
      tft.setTextColor(TFT_COLOR_darkgray);
      tft.setCursor(110, 58);
      sprintf(s, "%2.2fV", vbatLast);
      tft.print(s);
      if (vbat > 3.7)
        tft.setTextColor(TFT_COLOR_green);
      else if (vbat > 3.3)
        tft.setTextColor(TFT_COLOR_yellow);
      else        
        tft.setTextColor(TFT_COLOR_red);
      tft.setCursor(110, 58);
      sprintf(s, "%2.2fV", vbat);
      tft.print(s);
      vbatLast = vbat;
    }
    if (millis() > update60s) {
      update60s = millis() + 60000; // Next poll
      PlotChargeLineXY(posX++, vbat);
    }
  }
}

  //tft.drawLine(0, 59, 159, 59, TFT_COLOR_white);
  //tft.fillRect(0, 60, 159, 79, TFT_COLOR_bottom);  
  
// Show 'normal' status (4 x 16)
void PollDisplay(bool forceUpdate=false)
{
  // Hits IDs, cnt
  static uint32_t updateMs=0;
  static float vbatMin=-1, vbatLast=0;
  static int autoChargeCnt=0;
  static uint16_t skudLast;
  static uint8_t hitsLast[GUN_ID_CNT];
  static uint16_t lastConfigTimeS;
  static uint32_t lastRxErrorMs;
  static bool lastWifiStatus;
  uint16_t timeS;
  float temp, vbat, vbatReal;
  char s[20];
  bool update = forceUpdate;

  if (chargeMode == 1) {
    DisplayCharge();
    return;
  }    

  if (!forceUpdate && soundPlaying)
    return;  // Avoid updating the LCD while sound is playing - takes too long time bwteen polls

  if ((millis() > updateMs) || forceUpdate) {
    updateMs = millis() + 300; // Next poll
    // Intelligent update
    tft.setTextSize(1);
    tft.setFont(&FreeSansBold9pt7b);
    vbatReal = LiPoVoltage();
    vbat = FloatRound1(vbatReal);
    // Auto detect charge mode
    if (vbatMin < 0)
      vbatMin = vbatReal;  // Init
    else {
      if (vbatReal < vbatMin)
        vbatMin -= 0.01;
      else
        vbatMin += 0.01;
    }
    if (vbatReal > (vbatMin + 0.1)) {
      if (autoChargeCnt++ > 4) {
        chargeMode = 1;
        vbatMin = -1;
        DisplayCharge(true);
        return;
      }
    }
    else
      autoChargeCnt = 0;

    if (forceUpdate) {
      DisplayClr();
      tft.fillRect(0, 0, 159, 18, TFT_COLOR_top);
      tft.setCursor(0, tftLine[0]);
      if (gunMode == GUN_MODE_target) 
        tft.print(gunMode2Txt[gunMode]);
#if 0
      tft.setTextColor(TFT_COLOR_blue);
      tft.setCursor(85, tftLine[0]);
      tft.print(gunShootMode2Ch[gunShootMode]);
#endif
      tft.setTextColor(TFT_COLOR_yellow);
      tft.setCursor(85, tftLine[0]);
      tft.print(irLedTxLevel2Txt[irLedTxLevel]);
    }
    // WiFi
    if (forceUpdate || (lastWifiStatus != wifiStatus)) {
      lastWifiStatus = wifiStatus;
      if (gunMode == GUN_MODE_tag) {
        tft.setCursor(0, tftLine[0]);
        switch (wifiMode) {
#ifdef COMM
        case WIFI_MODE_server:
          if (wifiStatus)
            tft.setTextColor(TFT_COLOR_green);
          else
            tft.setTextColor(TFT_COLOR_red);
          tft.print("SR");
          break;
        case WIFI_MODE_client:
          if (wifiStatus)
            tft.setTextColor(TFT_COLOR_green);
          else
            tft.setTextColor(TFT_COLOR_red);
          tft.print("CL");
          break;
#endif          
        case WIFI_MODE_off:
        default:
          tft.setTextColor(TFT_COLOR_gray);
          tft.print("---");
          break;
        }
      }
    }

    // VBAT
    if ((vbat != vbatLast) || forceUpdate) {
      tft.setTextColor(TFT_COLOR_top);
      tft.setCursor(120, tftLine[0]);
      sprintf(s, "%2.1fV", vbatLast);
      tft.print(s);
      if (vbat > 3.7)
        tft.setTextColor(TFT_COLOR_green);
      else if (vbat > 3.3)
        tft.setTextColor(TFT_COLOR_yellow);
      else        
        tft.setTextColor(TFT_COLOR_red);
      tft.setCursor(120, tftLine[0]);
      sprintf(s, "%2.1fV", vbat);
      tft.print(s);
      vbatLast = vbat;
    }
    if (gunMode == GUN_MODE_tag) {
      // Status line
      if (forceUpdate) {
        // ID:GR
        tft.setTextColor(TFT_COLOR_white);
        tft.setCursor(40, tftLine[0]);
        sprintf(s, "%s:%s", gunTagId2Txt[gunTagId], gunTagGroup2Txt[gunTagGroup]); 
        tft.print(s);
      }
#if 0
      // IR RX error level
      if ((millis() > lastRxErrorMs) || forceUpdate) {
        uint32_t level;
        tft.fillRect(102, 0, 15, 18, TFT_COLOR_top);  
        tft.setTextColor(TFT_COLOR_orange);
        tft.setCursor(102, tftLine[0]);
        level = irRxErrors/2;
        if (level > 9) {
          level = 9;
        }
        irRxErrors /= 10;
        tft.print(level);
        lastRxErrorMs = millis() + 10000;
      }
#endif        
      // SKUD
      if (forceUpdate) {
        tft.setCursor(0, tftLine[1]);
        tft.setTextColor(TFT_COLOR_yellow);
        tft.print("Skud: ");
      }
      if ((gunShotsUsed != skudLast) || forceUpdate) {
        tft.setTextColor(TFT_COLOR_erase);
        tft.setCursor(60, tftLine[1]);
        tft.print(skudLast);
        tft.setTextColor(TFT_COLOR_green);
        tft.setCursor(60, tftLine[1]);
        tft.print(gunShotsUsed);
        skudLast = gunShotsUsed;
      }
#if 0
      // TIME since last config
      timeS = millis()/1000 - configTimeSOffset;
      if ((timeS > lastConfigTimeS) || forceUpdate) {
        tft.setTextColor(TFT_COLOR_erase);
        tft.setCursor(112, tftLine[1]);
        sprintf(s, "%02i:%02i", lastConfigTimeS/60, lastConfigTimeS%60);
        tft.print(s);
        tft.setTextColor(TFT_COLOR_gray);
        tft.setCursor(112, tftLine[1]);
        sprintf(s, "%02i:%02i", timeS/60, timeS%60);
        tft.print(s);
        lastConfigTimeS = timeS;
      }
#endif      
      // HITS
      if (forceUpdate) {
        tft.setTextColor(TFT_COLOR_yellow);
        tft.setCursor(0, tftLine[2]);
        tft.print("Hits:  ");
      }
      for (int i=0; i<GUN_ID_CNT; i++) {
        if (hitsIdCnt[i] != hitsLast[i]) {
          update = true;
          hitsLast[i] = hitsIdCnt[i];
        }
      }
      if (update) {
        int itemCnt=0;
        tft.fillRect(50, 37, 100, 20, TFT_COLOR_erase);  
        tft.fillRect(0, 56, 159, 20, TFT_COLOR_erase);  
        PollSound();
        tft.setCursor(50, tftLine[2]);
        for (int i=0; i<GUN_ID_CNT; i++) {
          if (hitsIdCnt[i] > 0) {
            if (++itemCnt == 3)
              tft.setCursor(0, tftLine[3]);
            tft.setTextColor(TFT_COLOR_gray);
            sprintf(s, "%i:", i+1);
            tft.print(s);
            tft.setTextColor(TFT_COLOR_red);
            tft.print(hitsIdCnt[i]);
            tft.print("  ");
          }
        }
      }
    }
    forceUpdate = false;
  }
}

// *** Button control **********************************************

bool ButtonPressed()
{
  static uint32_t lastPollMs;
  static bool lastState = false;

  if (millis() < lastPollMs + 50)
    return lastState;  // Noise canceling
  lastPollMs = millis();    
  lastState = digitalRead(GPI_swSetupN) == 0;
  return lastState;
}

#define BTN_shortPressed      1
#define BTN_longPressed       2
int ButtonAction()
{
#define BTN_shortPressMs     50
#define BTN_longPressMs     300
  static uint32_t btnTimerMs;
  static bool btnLastState, btnActive;
  bool btnState;
  int btnAction;

  btnAction = 0;  
  btnState = ButtonPressed();
  if (!btnActive) {
    if (btnState) {
      // Pressed
      if (btnState != btnLastState) {
        // First press
        btnTimerMs = millis();
      }
      else {
        if ((millis() > btnTimerMs + BTN_longPressMs) ) {
          // Long press
          btnActive = true;
          btnAction = BTN_longPressed;
          //printf("Long press\n");
        }
      }
    }
    else {
      // Not pressed
      if (btnState != btnLastState) {
        // Released
        if ((millis() > btnTimerMs + BTN_shortPressMs) ) {
          // Remove noise
          //printf("Pressed\n");
          btnActive = true;
          btnAction = BTN_shortPressed;
        }
      }
    }
  }
  if (btnState == false) 
    btnActive = false;

  btnLastState = btnState;
  return btnAction;
}

// *** Menu control **********************************************

void MenuChoice(menuItem_t *menuItem)
{
  int btnAction;
  int x, y;
  
  x = tft.getCursorX(); 
  y = tft.getCursorY(); 
  printf("x=%i, y=%i\n", x, y);
  tft.setTextColor(TFT_COLOR_select);
  tft.print(menuItem->txt[*(menuItem->value)]);
  for (;;) {
    if ((btnAction = ButtonAction()) == 0) {
      DelayMsPoll(1);
      continue;
    }
    if (btnAction == BTN_longPressed) {
      return;
    }
    if (btnAction == BTN_shortPressed) {
      tft.setCursor(x, y);
      tft.setTextColor(TFT_COLOR_erase);
      tft.print(menuItem->txt[*(menuItem->value)]);
      (*(menuItem->value))++;
      if (*(menuItem->value) > menuItem->maxValue)
        *(menuItem->value) = 0;
      tft.setCursor(x, y);
      tft.setTextColor(TFT_COLOR_select);
      tft.print(menuItem->txt[*(menuItem->value)]);
    }
  }
}

//                     uint8_t  max   txt-array
menuItem_t menuMode = {&gunMode, 1, gunMode2Txt};
menuItem_t menuIrPower = {&irLedTxLevel, 3, irLedTxLevel2Txt};
menuItem_t menuId = {&gunTagId, 15, gunTagId2Txt};
menuItem_t menuGroup = {&gunTagGroup, 2, gunTagGroup2Txt};
menuItem_t menuShootMode = {&gunShootMode, 2, gunShootMode2Txt};
menuItem_t menuSoundLevel = {&soundLevel, 3, soundLevel2Txt};
menuItem_t menuCharge = {&chargeMode, 1, chargeMode2Txt};
menuItem_t menuLog = {&logMode, 1, logMode2Txt};
menuItem_t menuWifi = {&wifiMode, 2, wifiMode2Txt};

menuLine_t menuLines[] = {{"Mode:", &menuMode}, 
                          {"Power:", &menuIrPower},
                          {"Id:", &menuId},
                          {"Team:", &menuGroup}, 
                          // {"Shoot:", &menuShootMode},
                          {"Sound:", &menuSoundLevel},
                          {"WiFi:", &menuWifi},
                          {"Charge:", &menuCharge},
                          // {"Log:", &menuLog},
                          {"Exit", NULL}};

void MenuSelect(menuLine_t *mLines, int cnt)
{
  int btnAction;
  int index, sel, lineNo, lineNoMax, prevIndex;

  sel = 0;
  index = 0;
  prevIndex = -1;
  lineNoMax = cnt>4?4:cnt;
  for (;;) {
    index = sel>3?sel-3:0;
    if (index != prevIndex)
      DisplayClr();
    prevIndex = index;      
    for (lineNo=0; lineNo<lineNoMax; lineNo++) {
      if (index == sel)
        tft.setTextColor(TFT_COLOR_select);
      else      
        tft.setTextColor(TFT_COLOR_yellow);
      tft.setCursor(0, tftLine[lineNo]);
      tft.print(mLines[index].name);
      tft.setTextColor(TFT_COLOR_green);
      tft.setCursor(75, tft.getCursorY());
      if (mLines[index].item)
        tft.print(mLines[index].item->txt[*(mLines[index].item->value)]);
      index++;
    }
    // Wait for button
    while ((btnAction = ButtonAction()) == 0)
      PollSound();

    if (btnAction == BTN_longPressed) {
      if (mLines[sel].item == NULL)
        return; // Exit
      tft.setTextColor(TFT_COLOR_yellow);
      if (sel > 3)
        tft.setCursor(0, tftLine[3]);
      else
        tft.setCursor(0, tftLine[sel]);
      tft.print(mLines[sel].name);
      tft.setCursor(75, tft.getCursorY());
      MenuChoice(mLines[sel].item);
    }
    if (btnAction == BTN_shortPressed) {
      sel++;
      if (sel >= cnt)
        sel = 0;
    }
  }
        }

void PollHealth()
{
  static uint32_t updateMs;
  static float lipoLimitV = 3.2;
  float vbat;

  if (millis() > updateMs) {
    updateMs = millis() + 60*1000; // Next poll

    vbat = LiPoVoltage();
    if (vbat < lipoLimitV) {
      if (lipoLimitV > 3.1)
        lipoLimitV -= 0.05;
      Mp3Play(SOUND_lowBatt);
   }
  }
}

// The gun will not work, while in MENU mode
void PollConfigMenu()
{
  int btnAction;
  uint8_t lastWifiMode;

  lastWifiMode = wifiMode;
  btnAction = ButtonAction();
  if (btnAction == BTN_shortPressed) {
    if (chargeMode != 0) {
      // If button pressed, exit chargemode
      chargeMode = 0;
      PollDisplay(true);
      return;
    }
    // Force resend hit info from all clients
    if (wifiMode == WIFI_MODE_server) {
      DisplayClr();
      tft.setCursor(10, tftLine[1]);
      tft.setTextColor(TFT_COLOR_yellow);
      tft.print("Update hit info");
      UdpSlaveResend();
      DelayMsPoll(500);
      PollDisplay(true);
      return;
    }
  }
  else if (btnAction == BTN_longPressed) {
    // Enter MENU by longpress config button

    LedColorSet(STATUS_LED_menu);
    HapticSetMs(100, true);
    DisplayClr();
    MenuSelect(menuLines, sizeof(menuLines)/sizeof(menuLine_t));
#if 1
    // Save changes to eeprom
    EEPROM.write(EEPROM_INDEX_gunMode, gunMode);
    EEPROM.write(EEPROM_INDEX_gunTagId, gunTagId);
    EEPROM.write(EEPROM_INDEX_gunTagGroup, gunTagGroup);
    EEPROM.write(EEPROM_INDEX_soudLevel, soundLevel);
    EEPROM.write(EEPROM_INDEX_irLedTxLevel, irLedTxLevel);
    EEPROM.write(EEPROM_INDEX_wifiMode, wifiMode);
    EEPROM.commit();
#endif

    if (wifiMode != lastWifiMode) {
      // Force restart if WiFi mode has changed
      DelayMsPoll(1000);
      ESP.restart();
    }
    SoundInit();
    LedColorSet(0);
    IrLedPowerSet(irLedTxLevel);
    configTimeSOffset = millis()/1000;
    if (chargeMode)
      DisplayCharge(true);
    else
      PollDisplay(true);
    if (logMode)
      LogPrint();
    logMode = 0;
  }
}


// *** Core/thread support **********************************************
#if 0
TaskHandle_t Task0;

void CreateTask0()
{
  // Create a task that will be executed in the Task0code() function, 
  // with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
                    Task0Code,   /* Task function. */
                    "Task0",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &Task0,      /* Task handle to keep track of created task */
                    0);          /* pin task to core 0 */                  
  delay(500); 
}

// NOT USED!
void Task0Code(void * pvParameters) 
{
  printf("Task0: core: %i\n", xPortGetCoreID());
  
  for (;;);
}
#endif

#define LOOP_minMs  10

void MainTaskCode()
{
  uint32_t timerMs=0, tt;

  printf("Main task: core %i\n", xPortGetCoreID());
  LedStatusSet(STATUS_LED_green, 1000, 100);
  PollDisplay(true);
  for (;;) {
    PollDisplay();
    PollTriggerShoot();
    PollConfigMenu();
    PollHaptic();
    PollSound();
    PollLed();
    PollHealth();
    PollHit();
#ifdef COMM
    PollComm();
#endif    
    IrxPoller();
    tt = millis()-(timerMs-LOOP_minMs);
    if (tt > 100)
      printf("LoopTime: %i\n", tt);
    while (millis() < timerMs)
      PollSound();
    timerMs = millis() + LOOP_minMs;
  }
}

void loop()
{
#ifdef COMM
  //PollComm();
#endif  
  //not used CreateTask0();
  MainTaskCode();
}


#ifdef COMM

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <WiFiUdp.h>

#define HTTP_PORT 80

#define UDP_PORT 8888
#define UDP_PKT_SIZE_MAX  25
WiFiUDP Udp;

uint8_t gunIdMax = 2;  // Used to limit HTML table

// *********************************************************************
// WiFi Server
// *********************************************************************

// Set these to your desired credentials.
const char *ssid = "MH-GUNS";
// NO password: const char *password = "1234";

WiFiServer server(HTTP_PORT);

void WiFiServerSetup()
{
  printf("Access point setup: %s\n", ssid);
  if (!WiFi.softAP(ssid)) {
    printf("ERROR: Soft AP creation failed.\n");
    return;
  }
  IPAddress myIP = WiFi.softAPIP();
  printf("Access Point: IP address: %s\n", myIP.toString());
  server.begin();
  wifiStatus = true;
  gunInfo[gunTagId].ipAddr = hostIP;
  gunInfo[gunTagId].groupId = gunTagGroup;
  printf("Server started\n");
}

void WiFiServerLoop()
{
  uint16_t totalKills[GUN_ID_CNT];
  uint16_t totalHits[GUN_ID_CNT];
  WiFiClient client = server.available();   // listen for incoming clients

  if (client) {                             // if you get a client,
    printf("\nNew Client: %s.\n", client.remoteIP().toString()); 
    String currentLine = "";                // make a String to hold incoming data from the client
    while (client.connected()) {            // loop while the client's connected
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        //Serial.write(c);                    // print it out the serial monitor
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0) {
            // Calc total kills
            for (int g=0; g<=gunIdMax; g++) {
              totalKills[g] = 0;
              totalHits[g] = 0;
            }
            for (int g=0; g<=gunIdMax; g++) {
              for (int h=0; h<=gunIdMax; h++) {
                totalKills[g] += gunInfo[g].kills[h];
                totalHits[h] += gunInfo[g].kills[h];
              }
            }
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println();
            // Actual contents
            // Table setup
            client.println("<style>");
            client.println("table, th, td {border:1px solid black;border-collapse: collapse;}");
            client.println("th, td {padding-left: 20px; padding-right: 20px;}");
            client.println("</style>");

            client.print("<body>");
            client.print("<h1>LaserTag status<br></h1>");
            client.println("<table><colgroup><col span='1' style='background-color: #D6EEEE'></colgroup>");
            for (int r=0; r<=gunIdMax+1; r++) {
              if (r == 0)
                client.println("<tr style='background-color: #D6EEEE'>");
              else {
                if ((gunInfo[r-1].connCnt == 0) && (r-1 != gunTagId))
                  continue; // Skip row
                client.println("<tr>");
              }
              for (int c=0; c<=gunIdMax+1; c++) {
                // Kills total (ekstra column)
                if (c == 1) {
                  if (r == 0) 
                    client.println("<th>Kills</th>");
                  else
                  {
                    client.println("<th>");
                    client.println(totalKills[r-1]);
                    client.println("</th>");
                  }
                }
                if ((c == 0) || (gunInfo[c-1].connCnt > 0) || (c-1 == gunTagId)) {
                  // Kills pr. gunId
                  client.println("<th>");
                  if (r == 0) {
                    if (c != 0)
                      client.println(gunTagId2Txt[c-1]);
                  }
                  else
                    if (c == 0)
                      client.println(gunTagId2Txt[r-1]);
                    else {
                      if (r == c)
                        client.println("-");
                      else
                        client.println(gunInfo[r-1].kills[c-1]);
                    }
                  client.println("</th>");
                }
              }
              // Extra column team id
              if (r == 0) 
                client.println("<th>Team</th>");
              else
              {
                client.println("<th>");
                client.println(gunTagGroup2Txt[gunInfo[r-1].groupId]);
                client.println("</th>");
              }
              // Extra column IP address
              if (r == 0) 
                client.println("<th>IP</th>");
              else
              {
                client.println("<th>");
                client.println(gunInfo[r-1].ipAddr.toString());
                client.println("</th>");
              }
              // Extra column connCnt
              if (r == 0) 
                client.println("<th>CC</th>");
              else
              {
                if ((gunInfo[r-1].connCnt != 1) && (r-1 != gunTagId))
                  client.println("<th style='background-color: #ff4040'>");
                else
                  client.println("<th>");
                if (r-1 == gunTagId)
                  client.println("-");
                else                  
                  client.println(gunInfo[r-1].connCnt);
                client.println("</th>");
              }
              client.println("</tr>");
            }
            // Extra row: Hits 
            client.println("<tr>");
            client.println("<th>Hits</th>");
            client.println("<th>-</th>");
            for (int c=0; c<=gunIdMax; c++) {
              if ((gunInfo[c].connCnt > 0) || (c == gunTagId)) {
                client.println("<th>");
                client.println(totalHits[c]);
                client.println("</th>");
              }
            }
            client.println("<th>-</th>");
            client.println("<th>-</th>");
            client.println("<th>-</th>");
            client.println("</tr>");
            client.println("</table>");
  
            if (gunTagGroup != 2) {
              totalKills[0] = 0;
              totalKills[1] = 0;
              for (int g=0; g<=gunIdMax; g++) {
                for (int h=0; h<=gunIdMax; h++) {
                  totalKills[gunInfo[g].groupId] += gunInfo[g].kills[h];
                }
              }
              client.print("<h2>Team statistic</h2>");
              client.println("<table><colgroup><col span='1' style='background-color: #D6EEEE'></colgroup>");

              client.println("<tr style='background-color: #D6EEEE'>");
              client.println("<th></th>");
              client.println("<th>Kills</th>");
              client.println("</tr>");

              client.println("<tr>");
              client.println("<th>A</th>");
              client.println("<th>");
              client.println(totalKills[0]);
              client.println("</th>");
              client.println("</tr>");

              client.println("<tr>");
              client.println("<th>B</th>");
              client.println("<th>");
              client.println(totalKills[1]);
              client.println("</th>");
              client.println("</tr>");
              client.println("</table>");
            }
            client.println("<p>Made by: Michael Hansen 2024</p>");
            client.print("</body>");
          // The HTTP response ends with another blank line:
            client.println();
            // break out of the while loop:
            break;
          } else {    // if you got a newline, then clear currentLine:
            currentLine = "";
          }
        } else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
    // close the connection:
    client.stop();
    printf("Client Disconnected.\n");
  }
}

// *********************************************************************
// WiFi Client
// *********************************************************************

bool WifiClientConnect()
{
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiStatus == false) {
      printf("IP address: %s\n", WiFi.localIP().toString());
      wifiStatus = true;
    }            
    return true;
  }
  wifiStatus = false;

  // Connecting to a WiFi network
  printf("Client setup: %s\n", ssid);
  WiFi.begin(ssid);
  return false;
}


// *********************************************************************
// UDP
// *********************************************************************
/** Protocol:
 * C: Client connect with gunId, IP is taken from UDP message
 * H: Client send information when it is hit
 * K: Server send hit info back to shooter 
 * R: Server request all clients to send all hit info at end of game
*/


// Decode messages
bool forceResendMode;

int MsgNextNum(char *message, int msgSize, int *pos)
{
  int i, num;

  for (i=*pos; i<msgSize; i++) {
    if (message[i] == ':') {
      num = strtol(&message[*pos], NULL, 10);
      *pos = i+1;
      return num;
    }
  }
  return 0;
}

void MsgDecode(char *message, int msgSize)
{
  int pos, num;
  uint8_t gunId, aGunId, hits, teamId;
  char packetBuffer[UDP_PKT_SIZE_MAX];

  message[msgSize] = 0;  // Force end of line
  if (msgSize < 5) {
    printf("MSG: Too short <%s>\n", message);
    return;
  }
  if (message[1] != ':' || message[msgSize-3] != ':') {
    printf("MSG: Missing : <%s>\n", message);
    return;
  }
  if ((message[0] | 0x20) != message[msgSize-2]) {
    printf("MSG: Start/end mismatch <%s>\n", message);
    return;
  }
  printf("MSG: ");
  pos = 2;
  switch (message[0]) {
  case 'H':  // Hits H:<source gunId>:<attack gunId>:<total hits>:h
    printf("H ");
    gunId = MsgNextNum(message, msgSize, &pos);
    aGunId = MsgNextNum(message, msgSize, &pos);
    hits = MsgNextNum(message, msgSize, &pos);
    printf("%i<-%i: %i\n", gunId, aGunId, hits);
    gunInfo[aGunId].kills[gunId] = hits;
    if (!forceResendMode) {
      // Send confirmed kill
      sprintf(packetBuffer, "K:%i:k", gunId);
      UdpTx(gunInfo[aGunId].ipAddr, packetBuffer, strlen(packetBuffer)+1);
    }
    break;
  case 'C':  // Connect C:<source gunId>:c
    printf("C ");
    gunId = MsgNextNum(message, msgSize, &pos);
    teamId = MsgNextNum(message, msgSize, &pos);
    printf("%i:%s\n", gunId, gunTagGroup2Txt[teamId]);
    gunInfo[gunId].ipAddr = Udp.remoteIP();
    gunInfo[gunId].groupId = teamId;
    gunInfo[gunId].connCnt++;
    if (gunId > gunIdMax)
      gunIdMax = gunId;
    break;
  case 'K':  // Confirmed kill, K:<gunId>:k
    printf("K %i\n", MsgNextNum(message, msgSize, &pos));
    LedStatusSet(STATUS_LED_kill, 1000, 0);
    break;
  case 'R':  // Force resend, R:0:r
    printf("R %i\n", MsgNextNum(message, msgSize, &pos));
    for (gunId=0; gunId<GUN_ID_CNT; gunId++) {
      if (hitsIdCnt[gunId] > 0) {
        sprintf(packetBuffer, "H:%i:%i:%i:h", gunTagId, gunId, hitsIdCnt[gunId]);
        UdpTx(hostIP, packetBuffer, strlen(packetBuffer)+1);
      }
    }
    break;
  default:
    printf("Unknown: <%s>\n", message);
    break;
  }
}

void UdpSlaveResend()
{
  forceResendMode = true;
  for (int gunId=0; gunId<=gunIdMax; gunId++) {
    if ((gunId != gunTagId) && 
        (gunInfo[gunId].ipAddr != IPAddress(0, 0, 0, 0))) {
      UdpTx(gunInfo[gunId].ipAddr, "R:0:r", 6);
      while (UdpRxPoll()) ;  // Empty RX
    }
  }
  DelayMsPoll(500);
  while (UdpRxPoll()) ;  // Empty RX
  forceResendMode = false;
}

void UpdSetup()
{
  Udp.begin(UDP_PORT);
}

int UdpRx(char *packetBuffer)
{
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    Udp.read(packetBuffer, packetSize);
    //printf("UDP RX: [%i] <%s>\n", packetSize, packetBuffer);
    return packetSize;
  }
  return 0;
}

bool UdpRxPoll()
{
  char packetBuffer[UDP_PKT_SIZE_MAX];
  int packetSize;

  packetSize = UdpRx(packetBuffer);
  if (packetSize > 0) {
    MsgDecode(packetBuffer, packetSize);
  }
  return packetSize > 0;
}

int UdpTx(IPAddress destIP, const char *packetBuffer, int packetSize)
{
  if ((wifiMode == WIFI_MODE_client) && !wifiStatus)
    return 0;
  Udp.beginPacket(destIP, UDP_PORT);
  Udp.write((const uint8_t*)packetBuffer, packetSize);
  Udp.endPacket();
  return packetSize;
}

void UdpPoll()
{
  static uint32_t nextTxMs;
  static bool firstTime=true;
  char packetBuffer[UDP_PKT_SIZE_MAX];
  int packetSize;

  UdpRxPoll();
  // TX
  if (wifiMode == WIFI_MODE_client) {
    // Client
    if (millis() > nextTxMs) {
      nextTxMs = millis() + 1000;
      if (WifiClientConnect() && firstTime) {
        firstTime = false;
        sprintf(packetBuffer, "C:%i:%i:c", gunTagId, gunTagGroup);
        UdpTx(hostIP, packetBuffer, strlen(packetBuffer)+1);
      }
    }
  }
}


// TEST ONLY
void UdpTest()
{
  static int hits;  
  static uint32_t nextTxMs;
  char packetBuffer[UDP_PKT_SIZE_MAX];

  if (wifiMode == WIFI_MODE_client) {
    // Client
    if (millis() > nextTxMs) {
      nextTxMs = millis() + 2000;
      if (WifiClientConnect()) {
        sprintf(packetBuffer, "H:%i:%i:%i:h", gunTagId, 2, hits++);
        UdpTx(hostIP, packetBuffer, strlen(packetBuffer)+1);
      }
    }
  }
}


// *********************************************************************
// Communication common
// *********************************************************************

void CommSetup() 
{
  //wifiMode = WIFI_MODE_server;  
  if (wifiMode == WIFI_MODE_off)
    return;

  if (wifiMode == WIFI_MODE_server) {
    WiFiServerSetup();
  }
  if (wifiMode == WIFI_MODE_client) {
    WifiClientConnect();
  }
  UpdSetup();
}

void PollComm() 
{
  static uint32_t nextPollMs;
  static bool toggle;
  if (wifiMode == WIFI_MODE_off)
    return;

  if (wifiMode == WIFI_MODE_server) {
    WiFiServerLoop();
  }
  UdpPoll();
  //UdpTest();

}
#endif  // COMM