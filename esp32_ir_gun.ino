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
    ESP32-Lite:    M5Stack-Core-ESP32
    M5Stack Core2: M5Stack-Core2

  Sounds: 
    MP3, Mono, 16kHz
    .mp3-> .h file, use: Bin2C.exe
  Sound Lib fix: See buttom of lightsaber_V3 file.
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

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// color definitions  xxxx.x xxx.xxx x.xxxx
#define TFT_COLOR_black      0x0000
#define TFT_COLOR_gray       0xC618   //1100.0 110.000 1.1000
#define TFT_COLOR_darkgray   0x4208   //0100.0 010.000 0.1000
#define TFT_COLOR_red        0x001F
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
#define STATUS_LED_hit      0xff8080
#define STATUS_LED_charge1  0x808000  // Yellow
#define STATUS_LED_charge2  0x008000  // Green

// Globals
#define GUN_ID_CNT      4  // Max 4 guns

#define GUN_MODE_tag    0
#define GUN_MODE_target 1
uint8_t gunMode;           // Tag or target mode
const char *gunMode2Txt[] = {"TAG", "TARGET"};

uint8_t gunTagId;        // Gun TX tag (3 bits)
const char *gunTagId2Txt[] = {"0", "1", "2", "3"};
uint8_t gunTagGroup;     // Gun TX group (1 bit)
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


int tftLine[4] = {16, 35, 54, 73};

/* WARNING
  Arduino can't handle typedef below this line if used in functions!!!!!
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

#include "skud2.h"
#include "hit1.h"
#include "lowbatt1.h"

typedef struct {
  const unsigned char *data;
  uint32_t size;  
} soundInfo_t; 

soundInfo_t soundInfo[] = {
  {skud2Sound, sizeof(skud2Sound)},
  {hit1Sound, sizeof(hit1Sound)},
  {lowBatt1Sound, sizeof(lowBatt1Sound)},
};

#define SOUND_shot      0
#define SOUND_hit       1
#define SOUND_lowBatt   2

void SoundInit(void)
{
  audioOut = new AudioOutputI2S(0, 1); // 2nd parameter: 0: External DAC, 1: INTERNAL_DAC
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

  audiofileProg = new AudioFileSourcePROGMEM(soundInfo[index].data, soundInfo[index].size);
  audioMp3->begin(audiofileProg, audioOut);
}


#include "driver/i2s.h"

void PollSound(void)
{
  if (audioMp3->isRunning()) {
    if (!audioMp3->loop()) {
      printf("MP3 done\n");
      audioMp3->stop();
      delete audiofileProg;
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

void setup()
{
  int i;

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

  //delay(2000);
  printf("\nIR GUN\n");
  printf("Software date: %s, %s\n", __DATE__, __TIME__);

  // Port setup
  pinMode(GPI_swShootN, INPUT_PULLUP);
  pinMode(GPI_swSetupN, INPUT_PULLUP);
  digitalWrite(GPO_irShoot, 0);
  pinMode(GPO_irShoot, OUTPUT);
  pinMode(GPI_irRx0, INPUT);
  pinMode(GPI_irRx1, INPUT);
  digitalWrite(GPO_haptic, 0);
  pinMode(GPO_haptic, OUTPUT);
  pinMode(GPO_pwmDac, OUTPUT);

  // Eeprom restore
  EEPROM.begin(5);
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

  irLedTxLevel = 2;  // TBD

  SoundInit();
  TagSetup(); // PWM setup
  IrLedPowerSet(irLedTxLevel);
  configTimeSOffset = millis()/1000;
  esp_task_wdt_init(30, false);  // Disable watchdog

  //hitsIdCnt[3]=23;  
}


#define LOOP_minMs  10
void MainLoop()
{
  uint32_t timerMs=0;
  for (;;) {
    //printf("LoopTime: %i\n", millis()-(timerMs-LOOP_minMs));
    while (millis() < timerMs)
      PollSound();
    timerMs = millis() + LOOP_minMs;
  }
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
  ledcWrite(TAG_channel, 128);
  delayMicroseconds(2*TAG_pulsMs-40);
  ledcWrite(TAG_channel, 0);
  delayMicroseconds(TAG_pulsMs);
  // Group
  IrSendValue(gunTagGroup, 1);  // If None just send A (0)
  // Gun
  IrSendValue(gunTagId, 3);
  // Post-ample
  ledcWrite(TAG_channel, 0);
  delayMicroseconds(TAG_pulsMs);
  ledcWrite(TAG_channel, 128);
  delayMicroseconds(2*TAG_pulsMs-40);
  ledcWrite(TAG_channel, 0);
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

#define IRX_cnt 2           // Number of IR receivers
int irxGpi[IRX_cnt] = {GPI_irRx0, GPI_irRx1};

int IrxDecodeBit(int gpiNo)
{
  int a, b;
  delayMicroseconds(TAG_pulsMs/2);
  a = digitalRead(gpiNo);
  delayMicroseconds(TAG_pulsMs);
  b = digitalRead(gpiNo);
  delayMicroseconds(TAG_pulsMs/2);

  if (a == b) {
    printf("IRX bit error\n");
    return -1;
  }
  return a;
}

void IrxPoller()
{
  int groupId, gunId, irxNo, v, i;
  uint32_t t, t0;
  int error;
  uint32_t preAmpMs, postAmpMs;

  for (;;) {
    error = 0;
    groupId = gunId = 0;
    // Look for low signal on all ports
    for (irxNo= 0; irxNo<IRX_cnt; irxNo++) {
      if (digitalRead(irxGpi[irxNo]) == 0)
        break;
    }
    if (irxNo == IRX_cnt)
      continue;  // No pulse found

    // Check pre-ample MMS (LLH)
    t0 = micros();
    for (;;) {
      if (digitalRead(irxGpi[irxNo]) == 1)
        break;
    }
    t = micros() - t0;
    preAmpMs = t;
    if ((t < 950) || (t > 1050)) {  // Must be app. 1ms
      error++;
      //continue;  // Wrong pulse
    }
    delayMicroseconds(500);
    // End of pre-ample

    // Decode group id
    if ((groupId = IrxDecodeBit(irxGpi[irxNo])) == -1) {
      error++;
      //continue;
    }
    // Decode gun id
    for (i=0; i<3; i++) {
      gunId <<= 1;
      if ((v = IrxDecodeBit(irxGpi[irxNo])) == -1) {
        error++;
        //continue;
      }        
      gunId |= v;
    }
    // Check post-ample SMM (HLL)
    delayMicroseconds(420);
    for (i=0; i<100; i++) {
      if (digitalRead(irxGpi[irxNo]) == 0)
        break;
      delayMicroseconds(1);
    }
    if (i == 100) {
      printf("No post-ample\n");
      postAmpMs = 0;
      //continue;  // No post ample found
    }
    else {
      t0 = micros();
      for (;;) {
        if (digitalRead(irxGpi[irxNo]) == 1)
          break;
      }
      t = micros() - t0;
      postAmpMs = t;
      if ((t < 950) || (t > 1050)) {  // Must be app. 1ms
        printf("t= %i\n", t);
        error++;
        //continue;  // Wrong pulse
      }
    }
    // Update global hit values
    if ((gunId != gunTagId) && (groupId != gunTagGroup)) {
      // No suicide, no group hit
      if ((error == 0) && (gunId < GUN_ID_CNT)) {
        hitsIdCnt[gunId]++;
        gunIsHit = true;
        delay(500);  // Hit hold down!
      }
    }
    if (error > 0) { 
      // error, preAmpMs, groupId, gunId, postAmpMs
      float p1, p2;
      p1 = preAmpMs;
      p1 /=1000;
      p2 = postAmpMs;
      p2 /= 1000;
      sprintf(logList[logIndex++], "%1.2f %i:%i %1.2f", p1, groupId, gunId, p2);
      if (logIndex == LOG_ENTRY_CNT)
        logIndex = 0;
      logCnt++;
    }
    /*printf("HIT: Pre= %i, Group= %i, gun= %i, post= %i\n", 
        preAmpMs, groupId, gunId, postAmpMs);*/
#if 0    
    DisplayClr();
    tft.setTextColor(TFT_COLOR_yellow);
    tft.setCursor(0, tftLine[0]);
    tft.print("Error: "); tft.print(error);
    tft.setCursor(0, tftLine[1]);
    tft.print("Pre:     "); tft.print(preAmpMs);
    tft.setCursor(0, tftLine[2]);
    tft.print("Gr:Id: "); tft.print(groupId); tft.print(", "); tft.print(gunId);
    tft.setCursor(0, tftLine[3]);
    tft.print("Post:   "); tft.print(postAmpMs);
#endif    
  }
}


// *** Poll hit **********************************************

// This is used so only 1 core is accessing the sound, LEDs etc.
void PollHit()
{
  if (gunIsHit) {
    gunShootHoldoffMs = millis() + 1000;
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
  static float vbatLast=0, vbatRealLast=0;
  static uint16_t skudLast;
  static uint8_t hitsLast[GUN_ID_CNT];
  static uint16_t lastConfigTimeS;
  uint16_t timeS;
  float temp, vbat, vbatReal;
  char s[20];
  bool update = forceUpdate;

  if (chargeMode == 1) {
    DisplayCharge();
    return;
  }    

  if ((millis() > updateMs) || forceUpdate) {
    updateMs = millis() + 300; // Next poll
    // Intelligent update
    tft.setTextSize(1);
    tft.setFont(&FreeSansBold9pt7b);
    vbatReal = LiPoVoltage();
    vbat = FloatRound1(vbatReal);

    if ((vbatLast > 0) && (vbat > (vbatRealLast + 0.15))) {
      // Auto char mode
      chargeMode = 1;
      vbatRealLast = vbatReal;
      DisplayCharge(true);
      return;
    }
    vbatRealLast = vbatReal;

    if (forceUpdate) {
      DisplayClr();
      tft.fillRect(0, 0, 159, 18, TFT_COLOR_top);
      tft.setTextColor(TFT_COLOR_yellow);
      tft.setCursor(0, tftLine[0]);
      tft.print(gunMode2Txt[gunMode]);
#if 0
      tft.setTextColor(TFT_COLOR_blue);
      tft.setCursor(85, tftLine[0]);
      tft.print(gunShootMode2Ch[gunShootMode]);
#endif
      tft.setTextColor(TFT_COLOR_red);
      tft.setCursor(105, tftLine[0]);
      tft.print(irLedTxLevel2Txt[irLedTxLevel]);
    }

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
      if (forceUpdate) {
        tft.setTextColor(TFT_COLOR_white);
        tft.setCursor(50, tftLine[0]);
        sprintf(s, "%s:%s", gunTagId2Txt[gunTagId], gunTagGroup2Txt[gunTagGroup]); 
        tft.print(s);
        tft.setTextColor(TFT_COLOR_yellow);
        tft.setCursor(0, tftLine[1]);
        tft.print("Hits:");
      }
      for (int i=0; i<GUN_ID_CNT; i++) {
        if (hitsIdCnt[i] != hitsLast[i]) {
          update = true;
          hitsLast[i] = hitsIdCnt[i];
        }
      }
      if (update) {
        tft.fillRect(60, 18, 159, 20, TFT_COLOR_erase);  
        tft.setCursor(50, tftLine[1]);
        for (int i=0; i<GUN_ID_CNT; i++) {
          if (hitsIdCnt[i] > 0) {
            tft.setTextColor(TFT_COLOR_gray);
            sprintf(s, "  %i:", i);
            tft.print(s);
            tft.setTextColor(TFT_COLOR_red);
            tft.print(hitsIdCnt[i]);
          }
        }
      }
      if (forceUpdate) {
        tft.setCursor(0, tftLine[2]);
        tft.setTextColor(TFT_COLOR_yellow);
        tft.print("Skud: ");
      }
      if ((gunShotsUsed != skudLast) || forceUpdate) {
        tft.setTextColor(TFT_COLOR_erase);
        tft.setCursor(60, tftLine[2]);
        tft.print(skudLast);
        tft.setTextColor(TFT_COLOR_green);
        tft.setCursor(60, tftLine[2]);
        tft.print(gunShotsUsed);
        skudLast = gunShotsUsed;
      }
      timeS = millis()/1000 - configTimeSOffset;
      if ((timeS != lastConfigTimeS) || forceUpdate) {
        tft.setTextColor(TFT_COLOR_erase);
        tft.setCursor(112, tftLine[2]);
        sprintf(s, "%02i:%02i", lastConfigTimeS/60, lastConfigTimeS%60);
        tft.print(s);
        tft.setTextColor(TFT_COLOR_gray);
        tft.setCursor(112, tftLine[2]);
        sprintf(s, "%02i:%02i", timeS/60, timeS%60);
        tft.print(s);
        lastConfigTimeS = timeS;
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
menuItem_t menuId = {&gunTagId, 3, gunTagId2Txt};
menuItem_t menuGroup = {&gunTagGroup, 2, gunTagGroup2Txt};
menuItem_t menuShootMode = {&gunShootMode, 2, gunShootMode2Txt};
menuItem_t menuSoundLevel = {&soundLevel, 3, soundLevel2Txt};
menuItem_t menuCharge = {&chargeMode, 1, chargeMode2Txt};
menuItem_t menuLog = {&logMode, 1, logMode2Txt};

menuLine_t menuLines[] = {{"Mode:", &menuMode}, 
                          {"Power:", &menuIrPower},
                          {"Id:", &menuId},
                          {"Group:", &menuGroup}, 
                          // {"Shoot:", &menuShootMode},
                          {"Sound:", &menuSoundLevel},
                          {"Charge:", &menuCharge},
                          {"Log:", &menuLog},
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

  btnAction = ButtonAction();
  if (btnAction == BTN_shortPressed) {
    if (chargeMode != 0) {
      // If button pressed, exit chargemode
      chargeMode = 0;
      PollDisplay(true);
      return;
    }
    // Re-load TBD
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
    EEPROM.commit();
#endif
    SoundInit();
    LedColorSet(0);
    IrLedPowerSet(irLedTxLevel);
    if (chargeMode)
      DisplayCharge(true);
    else
      PollDisplay(true);
    configTimeSOffset = millis()/1000;
    if (logMode)
      LogPrint();
    logMode = 0;
  }
}


// *** Core/thread support **********************************************

TaskHandle_t Task0;

void CreateMainTask()
{
  // Create a task that will be executed in the Task0code() function, 
  // with priority 1 and executed on core 0
  xTaskCreatePinnedToCore(
                    MainTaskCode,   /* Task function. */
                    "Task0",     /* name of task. */
                    10000,       /* Stack size of task */
                    NULL,        /* parameter of the task */
                    1,           /* priority of the task */
                    &Task0,      /* Task handle to keep track of created task */
                    0);          /* pin task to core 0 */                  
  delay(500); 
}

#define LOOP_minMs  10

void MainTaskCode(void * pvParameters)
{
  uint32_t timerMs=0, tt;

  printf("Main task: core %i\n", xPortGetCoreID());
  LedStatusSet(STATUS_LED_green, 1000, 100);
  PollDisplay(true);
  //Mp3Play(SOUND_lowBatt);
  for (;;) {
    PollDisplay();
    PollTriggerShoot();
    PollConfigMenu();
    PollHaptic();
    PollSound();
    PollLed();
    PollHealth();
    PollHit();
    delay(1);  // Avoid watchdog prints
    tt = millis()-(timerMs-LOOP_minMs);
    if (tt > 100)
      printf("LoopTime: %i\n", tt);
    while (millis() < timerMs)
      PollSound();
    timerMs = millis() + LOOP_minMs;
  }
}

void IrxPollerTask() 
{
  printf("IRx poller: core: %i\n", xPortGetCoreID());
  
  IrxPoller();
  for (;;);
}


void loop()
{
  //dacWrite(26, 200);
  CreateMainTask();
  IrxPollerTask();
}


