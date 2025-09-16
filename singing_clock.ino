#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

#include <WiFi.h>
#include <Wire.h>
#include "ds3231.h"
#include "Audio_MAX98357A.h"
#include "SD.h"
#include "driver/rtc_io.h"
#include <Update.h>
#include <Preferences.h>

#define FIRMWARE_VERSION 1.04

// Adafruit Audio BFF pinout taken from
// https://learn.adafruit.com/adafruit-audio-bff/pinouts
#define APLIFIER_BCLK A3
#define APLIFIER_LRCLK A2
#define APLIFIER_DATA A1
#define SD_CHIP_SELECT_DATA A0
#define BFF_POWER_OFF GPIO_NUM_44

#define SLEEP_LIMIT_SECONDS 300ULL
#define NEXT_ALARM_TIME_ADDR 1

#define clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))

#define PARSE_VARIABLE(name) if (strcmp(key, #name) == 0) { name = atoi(value); buffer += "Changed default value of '"#name"' to "; buffer += value; buffer += '\n';}


static unsigned DAYS_AHEAD_MIN = 1u;
static unsigned DAYS_AHEAD_MAX = 3u;
static unsigned SINCE_HOUR = 6u;
static unsigned UNTIL_HOUR = 20u;
static unsigned SINCE_MIN = 0u;
static unsigned UNTIL_MIN = 59u;
static int UTC_OFFSET = 0;
static unsigned VOLUME = 2;


Audio_MAX98357A amplifier;
String musicList[100];
unsigned numberOfTracks;
RTClib myRTC;
DS3231 Clock;
static int isSerialEnabled = (1 << 2) | (1 << 0);
Preferences prefs;

#define WAIT_FOR_SERIAL_FOR_S 2
#define WAIT_FOR_SERIAL_SINGLE_TIME() \
  do { \
    if (isSerialEnabled & (1 << 0)) { \
      for (int i = 0; i < (WAIT_FOR_SERIAL_FOR_S * 100) && !Serial; ++i) \
        delay(10); \
      if (Serial) \
      { \
        isSerialEnabled &= ~(1 << 0); \
        isSerialEnabled |= (1 << 1); \
      } \
      else \
      { \
        isSerialEnabled &= ~(1 << 0); \
        Serial.end(); \
      } \
    } \
  } while(0)

template <typename... Args>
void LOG_PRINT(Args&&... args)
{
    WAIT_FOR_SERIAL_SINGLE_TIME();
    if (isSerialEnabled & (1 << 1))
        printSerial(std::forward<Args>(args)...);
    if (isSerialEnabled & (1 << 2))
        printLog(std::forward<Args>(args)...);
}

void setup() {
  btStop();                 // BT not needed
  WiFi.mode(WIFI_OFF);      // wifi not needed
  setCpuFrequencyMhz(80);   // slow down CPU for energy efficency

  pinMode(BFF_POWER_OFF, OUTPUT);  
  digitalWrite(BFF_POWER_OFF, HIGH);    // enable RTC/amplifier and SDcard

  Serial.begin(115200);

  if (!prefs.begin("app", false))
  {
    LOG_PRINT("Failed to begin preferences");
  }

  delay(20);
  Wire.begin();  // for RTC

  if (!amplifier.initSDCard(/*csPin=*/SD_CHIP_SELECT_DATA))
  {
    Serial.println("Initialize SD card failed !"); // if SD init failed only console available
    isSerialEnabled &= ~(1 << 2);
  }
  ensureUtf8Log();

  esp_reset_reason_t rr = esp_reset_reason();
  if (rr != ESP_RST_EXT /* reset button pressed */ && rr != ESP_RST_POWERON /* battery replacement */)
  {
    // check if alarm should fire
    uint64_t nextAlarmTime = prefs.getULong64("nat", 0llu);
    uint64_t nowUTC = myRTC.now().unixtime();
    if (nowUTC < nextAlarmTime)
    {
      // alarm should not fire yet
      firmwareUpdate();
      setTime();
      justSleep(nextAlarmTime - nowUTC);
      return;
    }
    LOG_PRINT("Woke up at UTC time: ", nowUTC, "alarm at:", nextAlarmTime);
  }
  else
  {
    LOG_PRINT("Reset/battery replacement alarm fire");
  }

  turnOffTheAlarm();

  if (!amplifier.initI2S(/*_bclk=*/APLIFIER_BCLK, /*_lrclk=*/APLIFIER_LRCLK, /*_din=*/APLIFIER_DATA))
  {
    LOG_PRINT("Initialize I2S failed!");
  }

  firmwareUpdate();

  setTime();

  loadConfig();

  amplifier.scanSDMusic(musicList);
  printMusicList();
  amplifier.closeFilter();
  VOLUME = clamp(VOLUME, 1, 9);
  amplifier.setVolume(VOLUME);

  playRandom();

  end();
}

void loop() {  
}

void justSleep(uint64_t seconds)
{
  digitalWrite(BFF_POWER_OFF, LOW);
  seconds = clamp(seconds, 1ULL, SLEEP_LIMIT_SECONDS);
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();
}

void end()
{
  setNextAlarm();
  LOG_PRINT("Program finished. Version", FIRMWARE_VERSION);
  prefs.end();
  justSleep(SLEEP_LIMIT_SECONDS);
}

void turnOffTheAlarm()
{
    Clock.enable32kHz(false);

    // turn off the alarms
    Clock.turnOffAlarm(1);
    Clock.turnOffAlarm(2);
    // clear alarm flags
    Clock.checkIfAlarm(1);
    Clock.checkIfAlarm(2);
}

void trim(char* str)
{
    char* end;

    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return;

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}

void loadConfig()
{
    File file = SD.open("/config.txt", FILE_READ);
    if (!file)
    {
        LOG_PRINT("Failed to open config file, use default values");
        return;
    }

    String buffer;
    char line[512];
    unsigned count = 0;

    while ((count = file.readBytesUntil('\n', line, sizeof(line))))
    {
        line[count] = '\0';
        if (line[0] == '#' || line[0] == '\n') // skip comments and blank lines
            continue;

        char* eq = strchr(line, '='); // find first =
        if (!eq) continue; // if no =, skip line

        *eq = '\0';
        char* key = line;
        char* value = eq + 1;

        trim(key);
        trim(value);

        PARSE_VARIABLE(DAYS_AHEAD_MIN)
        else PARSE_VARIABLE(DAYS_AHEAD_MAX)
        else PARSE_VARIABLE(SINCE_HOUR)
        else PARSE_VARIABLE(UNTIL_HOUR)
        else PARSE_VARIABLE(SINCE_MIN)
        else PARSE_VARIABLE(UNTIL_MIN)
        else PARSE_VARIABLE(VOLUME)
        else PARSE_VARIABLE(UTC_OFFSET)
    }

    file.close();
    LOG_PRINT(buffer.c_str(), "\nConfig loaded from file");
}

void playRandom()
{
  if (numberOfTracks > 0)
  {
    const unsigned trackNo = esp_random() % numberOfTracks;
    LOG_PRINT("Play track : ", musicList[trackNo].c_str());
    amplifier.playSDMusic(musicList[trackNo].c_str());
    do { delay(200); } while (!amplifier.isStopped());
  }
}

inline String u64ToStr(uint64_t v)
{
  char buf[21];
  int pos = 20;
  buf[pos] = '\0';
  do {
    uint64_t digit = v % 10;
    v /= 10;
    buf[--pos] = char('0' + digit);
  } while (v);
  return String(&buf[pos]);
}

inline String i64ToStr(int64_t v)
{
  if (v < 0) {
    uint64_t u = uint64_t(-(v + 1)) + 1;
    return String('-') + u64ToStr(u);
  } else {
    return u64ToStr(uint64_t(v));
  }
}

template<typename T>
inline void singleSerialPrint(const T& x) { Serial.print(x); }
inline void singleSerialPrint(uint64_t v) { Serial.print(u64ToStr(v)); }
inline void singleSerialPrint(int64_t v)  { Serial.print(i64ToStr(v)); }

template<typename... Args>
inline void printSerial(const Args&... args) {
  const char* sep = " ";
  ( (singleSerialPrint(args), Serial.print(sep)), ... );
  Serial.println();
  Serial.flush();
}

template<typename T>
inline String singleLogPrint(T&& x) { return String(std::forward<T>(x)); }
inline String singleLogPrint(uint64_t v) { return u64ToStr(v); }
inline String singleLogPrint(int64_t v)  { return i64ToStr(v); }

void ensureUtf8Log()
{
  if (!SD.exists("/log.txt"))
  {
    File f = SD.open("/log.txt", FILE_WRITE);
    if (f) {
      const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
      f.write(bom, 3);
      f.close();
    }
  }
  else
  {
    File f = SD.open("/log.txt", FILE_READ);
    if (f) {
      uint8_t head[2] = {0,0};
      f.read(head, 2);
      f.close();
      if ((head[0]==0xFF && head[1]==0xFE) || (head[0]==0xFE && head[1]==0xFF))
      {
        SD.remove("/log.txt"); // UTF-16 -> remove and create UTF-8
        File nf = SD.open("/log.txt", FILE_WRITE);
        if (nf) nf.close();
      }
    }
  }
}

template <typename... Args>
void printLog(Args&&... args)
{
  File fp = SD.open("/log.txt", FILE_APPEND);
  if (!fp) return;

  const char* sep = "";
  ((fp.print(sep), fp.print(singleLogPrint(std::forward<Args>(args))), sep = " "), ...);
  fp.print('\n');
  fp.close();
}

void saveLog(const DateTime& now, const DateTime& nextAlarm)
{
  char buffer[128];
  const char* MONTH_ARR[] = {"<err>", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int len = snprintf(buffer, sizeof(buffer),
           "Called at %llu (%02u %s %02u:%02u), next alarm at %llu (%02u %s %02u:%02u)\n",
            (unsigned long long)now.unixtime(),
            now.day(), MONTH_ARR[now.month()],
            now.hour(), now.minute(),
            (unsigned long long)nextAlarm.unixtime(),
            nextAlarm.day(), MONTH_ARR[nextAlarm.month()],
            nextAlarm.hour(), nextAlarm.minute());

  if (len < 0) return;
  if (len >= (int)sizeof(buffer)) len = sizeof(buffer) - 1;
  buffer[len] = '\0';
  LOG_PRINT(buffer);
}

void firmwareUpdate()
{
  File firmware =  SD.open("/firmware.bin");
  if (firmware) 
  {
      Update.begin(firmware.size());
      Update.writeStream(firmware);
      if (Update.end())
      {
          firmware.close();
          SD.remove("/firmware.bin");
          LOG_PRINT("Firmware updated!");
          delay(200);
          ESP.restart();
      }
      else
      {
          firmware.close();
          LOG_PRINT("Failed to update the firmware, err = ", Update.getError());
      }
  }
}
// 0=Nd,1=Pn,...,6=So Sakamoto
static int dow_0Sun(int y, int m, int d) {
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

static int daysInMonth(int y, int m) {
  static const int dm[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  if (m != 2) return dm[m];
  bool leap = ( (y%4==0 && y%100!=0) || (y%400==0) );
  return leap ? 29 : 28;
}

static int lastSundayDay(int y, int m) {
  int last = daysInMonth(y, m);
  int w = dow_0Sun(y, m, last); // 0=Nd
  return last - w;
}

static bool isDST_Europe_UTC(uint64_t utc_epoch) {
  DateTime t(utc_epoch);
  int y = t.year();

  DateTime startUTC(y, 3, lastSundayDay(y,3), 1, 0, 0);  // Mar, 01:00 UTC
  DateTime endUTC  (y,10, lastSundayDay(y,10),1, 0, 0);  // Oct, 01:00 UTC

  return (utc_epoch >= startUTC.unixtime() && utc_epoch < endUTC.unixtime());
}

static int64_t localOffsetSeconds(uint64_t utc_epoch) {
  int64_t off = UTC_OFFSET * 3600;
  if (isDST_Europe_UTC(utc_epoch))
    off += 3600;    // +1 hour
  return off;
}

static uint64_t utcToLocal(uint64_t utc_epoch) {
  return utc_epoch + localOffsetSeconds(utc_epoch);
}

static uint64_t localToUtc(uint64_t local_epoch) {
  int64_t guess_utc = (int64_t)local_epoch - localOffsetSeconds((uint64_t)local_epoch);
  for (int i = 0; i < 2; ++i) {
    int64_t off = localOffsetSeconds((uint64_t)guess_utc);
    int64_t new_guess = (int64_t)local_epoch - off;
    if (new_guess == guess_utc) break;
    guess_utc = new_guess;
  }
  return (uint64_t)guess_utc;
}

void setTime() {
  File fp = SD.open("/timestamp.txt", FILE_READ);
  if (!fp)
    return;

  char buf[32] = {0};
  size_t n = fp.readBytesUntil('\n', buf, sizeof(buf)-1);
  fp.close();
  if (n == 0)
  {
    LOG_PRINT("Failed to read number from timestamp file");
    return;
  }

  uint64_t epoch_utc = strtoull(buf, nullptr, 10);
  LOG_PRINT("Found new timestamp");

  if (!SD.remove("/timestamp.txt"))
  {
    LOG_PRINT("Failed to remove timestamp file");
    return;
  }

  Clock.setEpoch(epoch_utc, true);
  Clock.setClockMode(false);
  Clock.clearOSF();

  LOG_PRINT("Epoch time updated : ", epoch_utc);
}

uint16_t randInRange(uint16_t lo, uint16_t hi)
{
  if (hi <= lo)
    return lo;
  uint32_t span = uint32_t(hi - lo + 1);
  return lo + (esp_random() % span);
}

uint16_t calculateNextAlarmDate()
{
  DAYS_AHEAD_MIN = clamp(DAYS_AHEAD_MIN, 0u, 365u);
  DAYS_AHEAD_MAX = clamp(DAYS_AHEAD_MAX, 0u, 365u);
  return randInRange(DAYS_AHEAD_MIN, DAYS_AHEAD_MAX);
}

byte calculateNextAlarmHour()
{
  SINCE_HOUR = clamp(SINCE_HOUR, 0u, 23u);
  UNTIL_HOUR = clamp(UNTIL_HOUR, 0u, 23u);
  if (SINCE_HOUR == UNTIL_HOUR)
    return SINCE_HOUR;
  if (SINCE_HOUR < UNTIL_HOUR)
  {
    return (byte)randInRange(SINCE_HOUR, UNTIL_HOUR);
  }
  else
  {
    // [SINCE..23] ∪ [0..UNTIL]
    uint8_t span = (24 - SINCE_HOUR) + (UNTIL_HOUR + 1);
    uint8_t r = esp_random() % span;
    return (r < (24 - SINCE_HOUR)) ? (SINCE_HOUR + r) : (r - (24 - SINCE_HOUR));
  }
}

byte calculateNextAlarmMinute()
{
  SINCE_MIN = clamp(SINCE_MIN, 0u, 59u);
  UNTIL_MIN = clamp(UNTIL_MIN, 0u, 59u);
  if (SINCE_MIN == UNTIL_MIN)
    return SINCE_MIN;
  if (SINCE_MIN < UNTIL_MIN)
  {
    return (byte)randInRange(SINCE_MIN, UNTIL_MIN);
  }
  else
  {
    uint8_t span = (60 - SINCE_MIN) + (UNTIL_MIN + 1);
    uint8_t r = esp_random() % span;
    return (r < (60 - SINCE_MIN)) ? (SINCE_MIN + r) : (r - (60 - SINCE_MIN));
  }
}

void setNextAlarm()
{
  uint64_t nowUTC = myRTC.now().unixtime();
  if (nowUTC == 0ull)
  {
    LOG_PRINT("Failed to read UTC time, cannot set the next alarm.");
    return;
  }
  uint64_t nowLocal = utcToLocal(nowUTC);

  DateTime nl(nowLocal);
  uint64_t midnightLocal = nowLocal
                         - nl.hour() * 3600llu
                         - nl.minute() * 60llu
                         - nl.second();

  uint16_t dayAdd = calculateNextAlarmDate();
  byte hour = calculateNextAlarmHour();
  byte minute = calculateNextAlarmMinute();

  uint64_t targetLocal = midnightLocal
                       + (uint64_t)dayAdd * 86400u
                       + (uint64_t)hour * 3600u
                       + (uint64_t)minute * 60u;

  if (dayAdd == 0 && targetLocal <= nowLocal + SLEEP_LIMIT_SECONDS)
  {
    targetLocal += 86400ull;
  }

  uint64_t targetUTC = localToUtc(targetLocal);
  saveLog(DateTime(nowLocal), DateTime(targetLocal));

  size_t wr = prefs.putULong64("nat", (uint64_t)targetUTC);
  if (wr != sizeof(uint64_t))
  {
    LOG_PRINT("putULong64 saved", wr, "bytes (expected 8)");
  }
}

void printMusicList(void)
{
  numberOfTracks = 0;

  if(musicList[numberOfTracks].length() == 0)
  {
    LOG_PRINT("The SD card audio file scan is empty, please check whether there are audio files in the SD card that meet the format!");
  }

  while (numberOfTracks < 100 && musicList[numberOfTracks].length())
  {
    numberOfTracks++;
  }
  LOG_PRINT("Music list found", numberOfTracks, "tracks");
}
