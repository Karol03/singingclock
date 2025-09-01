#include <stdio.h>
#include <stdint.h>

#include <WiFi.h>
#include <Wire.h>
#include "ds3231.h"
#include "Audio_MAX98357A.h"
#include "SD.h"
#include "driver/rtc_io.h"
#include <Update.h>
#include <Preferences.h>

#define FIRMWARE_VERSION 1.02

// Adafruit Audio BFF pinout taken from
// https://learn.adafruit.com/adafruit-audio-bff/pinouts
#define APLIFIER_BCLK A3
#define APLIFIER_LRCLK A2
#define APLIFIER_DATA A1
#define SD_CHIP_SELECT_DATA A0
#define BFF_POWER_OFF GPIO_NUM_44

#define SLEEP_LIMIT_SECONDS 300ULL
#define DST_FLAG_ADDR 0
#define NEXT_ALARM_TIME_ADDR 1

#define clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))

#define PARSE_VARIABLE(name) if (strcmp(key, #name) == 0) { name = atoi(value); buffor += "Changed default value of '"#name"' to "; buffor += value; buffor += '\n';}


static unsigned DAYS_AHEAD_MIN = 1u;
static unsigned DAYS_AHEAD_MAX = 3u;
static unsigned SINCE_HOUR = 6u;
static unsigned UNTIL_HOUR = 20u;
static unsigned SINCE_MIN = 0u;
static unsigned UNTIL_MIN = 59u;
static int UTC_OFFSET = 0;
static unsigned ENABLE_DAYLIGHT_SAVING_TIME = 0;
static int DAYLIGHT_SAVING_TIME_OFFSET = 0;
static unsigned SUMMER_TIME_START_MONTH = 0u;
static unsigned SUMMER_TIME_END_MONTH = 0u;
static unsigned VOLUME = 4;


Audio_MAX98357A amplifier;
String musicList[100];
unsigned numberOfTracks;
RTClib myRTC;
DS3231 Clock;
static int isSerialEnabled = -1;
Preferences prefs;

#define WAIT_FOR_SERIAL_FOR_S 2
#define WAIT_FOR_SERIAL_SINGLE_TIME() do { if (isSerialEnabled == -1) { for (int i = 0; i < (WAIT_FOR_SERIAL_FOR_S * 100) && !Serial; ++i) delay(20); if (!(isSerialEnabled = !!Serial)) Serial.end(); } } while(0)

void setup() {
  btStop();                 // BT not needed
  WiFi.mode(WIFI_OFF);      // wifi not needed
  setCpuFrequencyMhz(80);   // slow down CPU for energy efficency

  Wire.begin();
  Serial.begin(115200);
  
  if (!prefs.begin("app", false))
  {
    LOG_PRINT("Failed to begin preferences");
  }

  uint64_t nextAlarmTime = prefs.getULong64("nat", UINT64_MAX);
  uint64_t now = myRTC.now().unixtime();
  if (now < nextAlarmTime)
  {
    firmwareUpdate();
    setTime();
    justSleep(nextAlarmTime - now);
    return;
  }  // no need to sleep for less than 10 seconds, just do all the stuff

  LOG_PRINT("Woke-up at", now, "loaded alarm timestamp is", nextAlarmTime);
  pinMode(BFF_POWER_OFF, OUTPUT);
  digitalWrite(BFF_POWER_OFF, HIGH);

  turnOffTheAlarm();

  int i = 0;
  while (!amplifier.initI2S(/*_bclk=*/APLIFIER_BCLK, /*_lrclk=*/APLIFIER_LRCLK, /*_din=*/APLIFIER_DATA))
  {
    Serial.println("Initialize I2S failed !"); // only console, as sd card not available
    if (i > 10)
    {
      end();
    }
    ++i;
  }
  
  i = 0;
  while (!amplifier.initSDCard(/*csPin=*/SD_CHIP_SELECT_DATA))
  {
    LOG_PRINT("Initialize SD card failed !");
    if (i > 10)
    {
      end();
    }
    ++i;
  }

  firmwareUpdate();

  setTime();

  loadConfig();

  updateForDaylightTime();  // must be called after loadConfig to first load required variables from file

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

bool isSummerTimeNow()
{
  bool century;
  byte month = Clock.getMonth(century);
  if (SUMMER_TIME_START_MONTH < SUMMER_TIME_END_MONTH)
  {
    return (SUMMER_TIME_START_MONTH <= month && month <= SUMMER_TIME_END_MONTH);
  }
  return (month <= SUMMER_TIME_START_MONTH || SUMMER_TIME_END_MONTH <= month);
}

void updateForDaylightTime()
{
  if (!ENABLE_DAYLIGHT_SAVING_TIME || DAYLIGHT_SAVING_TIME_OFFSET == 0)
  {
    return;
  }

  bool isSummerTime = isSummerTimeNow();
  uint8_t dstFlag = prefs.getUChar("dst", 0);
  const uint8_t SUMMER_TIME_FLAG = 0xAB;
  const uint8_t WINTER_TIME_FLAG = 0xBC;
  if (dstFlag != SUMMER_TIME_FLAG && isSummerTime)
  {
    // set summer time
    uint64_t epoch_time = myRTC.now().unixtime() + int64_t(DAYLIGHT_SAVING_TIME_OFFSET) * 3600ull;
    Clock.setEpoch(epoch_time, false);
    Clock.setClockMode(false);
    prefs.putUChar("dst", SUMMER_TIME_FLAG);
    LOG_PRINT("Update to daylight summer time");
  }
  else if (dstFlag != WINTER_TIME_FLAG && !isSummerTime)
  {
    // set winter time
    uint64_t epoch_time = myRTC.now().unixtime() - int64_t(DAYLIGHT_SAVING_TIME_OFFSET) * 3600ull;
    Clock.setEpoch(epoch_time, false);
    Clock.setClockMode(false);
    prefs.putUChar("dst", WINTER_TIME_FLAG);
    LOG_PRINT("Update to winter time");
  }
}

void loadConfig()
{
    FILE* file = fopen("/sd/config.txt", "r");
    if (!file)
    {
        LOG_PRINT("Failed to open config file, use default values");
        return;
    }

    String buffor;
    char line[512];
    unsigned count = 0;

    while (fgets(line, sizeof(line), file))
    {
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
        else PARSE_VARIABLE(ENABLE_DAYLIGHT_SAVING_TIME)
        else PARSE_VARIABLE(DAYLIGHT_SAVING_TIME_OFFSET)
        else PARSE_VARIABLE(SUMMER_TIME_START_MONTH)
        else PARSE_VARIABLE(SUMMER_TIME_END_MONTH)
    }

    fclose(file);
    LOG_PRINT(buffor.c_str(), "\nConfig loaded from file");
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

template <typename... Args>
void printLog(Args&&... args)
{
  FILE* fp = fopen("/sd/log.txt", "a");
  if (!fp) return;

  const char* sep = "";
  ( (fprintf(fp, "%s", sep),
     fprintf(fp, "%s", singleLogPrint(std::forward<Args>(args)).c_str()),
     sep = " "), ... );
  fprintf(fp, "\n");
  fclose(fp);
}

template <typename... Args>
void LOG_PRINT(Args&&... args)
{
    WAIT_FOR_SERIAL_SINGLE_TIME();
    if (Serial)
        printSerial(std::forward<Args>(args)...);
    else
        printLog(std::forward<Args>(args)...);
}

void saveLog(DateTime now, DateTime nextAlarm)
{
  const char* MONTH_ARR[] = {"<err>", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  FILE* fp = fopen("/sd/log.txt", "a");
  if (!fp)
    return;

  if (fprintf(fp, "Called at %llu (%u %s %02u:%02u), next alarm at %llu (%u %s %02u:%02u)\n",
      now.unixtime(), now.day(), MONTH_ARR[now.month()], now.hour(), now.minute(),
      nextAlarm.unixtime(), nextAlarm.day(), MONTH_ARR[nextAlarm.month()], nextAlarm.hour(), nextAlarm.minute()) < 0)
  {
      Serial.println("Failed to save timestamp in log.txt");  // only serial here
  }
  fclose(fp);
}

void firmwareUpdate()
{
  File firmware =  SD.open("/firmware.bin");
  if (firmware) 
  {
      LOG_PRINT("Updating the firmware...");
      Update.begin(firmware.size(), U_FLASH);
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

void setTime()
{
  DateTime datetime = myRTC.now();
  if (datetime.unixtime() < __DATE_TIME_UNIX__) // enter here on first startup or after battery replacing
  {
    LOG_PRINT("Unix time is", datetime.unixtime(), "\nCompilation time", __DATE_TIME_UNIX__);
    Clock.setEpoch(__DATE_TIME_UNIX__, true);
    Clock.setClockMode(false);
  }

  FILE* fp = fopen("/sd/timestamp.txt", "r");
  if (!fp)
    return;

  uint64_t epoch_time;
  bool isLoaded = (fscanf(fp, "%" SCNu64, &epoch_time) == 1);
  fclose(fp);

  LOG_PRINT("Found new timestamp");
  if (!isLoaded)
  {
    LOG_PRINT("Failed to read number from timestamp file");
    return;
  }
  
  if (!SD.remove("/timestamp.txt"))
  {
      LOG_PRINT("Failed to remove timestamp file");
      return;
  }

  UTC_OFFSET = clamp(UTC_OFFSET, -12, 12);
  epoch_time += UTC_OFFSET * 3600;

  Clock.setEpoch(epoch_time, false);
  Clock.setClockMode(false);

  LOG_PRINT("Epoch time updated : ", epoch_time);

  prefs.putUChar("dst", 0);
}

byte calculateNextAlarmDate()
{
    DAYS_AHEAD_MIN = clamp(DAYS_AHEAD_MIN, 0, 365);
    DAYS_AHEAD_MAX = clamp(DAYS_AHEAD_MAX, 0, 365);
    if (DAYS_AHEAD_MAX >= DAYS_AHEAD_MIN)
      return DAYS_AHEAD_MIN;
    return (esp_random() % (DAYS_AHEAD_MAX - DAYS_AHEAD_MIN + 1)) + DAYS_AHEAD_MIN;
}

byte calculateNextAlarmHour()
{
    SINCE_HOUR = clamp(SINCE_HOUR, 0, 23);
    UNTIL_HOUR = clamp(UNTIL_HOUR, 0, 23);
    if (SINCE_HOUR >= UNTIL_HOUR)
      return SINCE_HOUR;
    return (esp_random() % (UNTIL_HOUR - SINCE_HOUR + 1)) + SINCE_HOUR;
}

byte calculateNextAlarmMinute()
{
    SINCE_MIN = clamp(SINCE_MIN, 0, 59);
    UNTIL_MIN = clamp(UNTIL_MIN, 0, 59);
    if (SINCE_MIN >= UNTIL_MIN)
      return SINCE_MIN;
    return (esp_random() % (UNTIL_MIN - SINCE_MIN + 1)) + SINCE_MIN;
}

void setNextAlarm()
{
    const unsigned MINUTES_IN_HOUR = 60;
    bool alarmH12 = false;
    bool alarmPM = false;
    byte alarmHour = calculateNextAlarmHour();
    byte alarmMinute = calculateNextAlarmMinute();
    byte alarmDayAdd = calculateNextAlarmDate();
    byte alarmBits = 0b00000000; // alarm when DoM, hour, minute match
    byte a1dy = false; // true makes the alarm go on A1Day = Day of Week, false - Day of Month

    DateTime datetime = myRTC.now();
    if (alarmDayAdd == 0)
    {
      if (datetime.hour() > alarmHour ||
         (datetime.hour() == alarmHour && (datetime.minute() + (SLEEP_LIMIT_SECONDS / 60)) >= alarmMinute))
      {
        // If a date and time from the past was drawn, add a single day 
        alarmDayAdd = 1;
      }
    }

    const uint64_t timeAtMidnightToday = datetime.unixtime() - (uint64_t(datetime.hour()) * 3600ULL)
                                                             - (uint64_t(datetime.minute()) * 60ULL)
                                                             - (uint64_t(datetime.second()));
    const uint64_t nextAlarmEpochTime = timeAtMidnightToday + (uint64_t(alarmDayAdd) * 86400ULL)
                                                            + (uint64_t(alarmHour) * 3600ULL)
                                                            + (uint64_t(alarmMinute) * 60ULL);
    DateTime nextAlarm = DateTime(nextAlarmEpochTime);
  
    saveLog(datetime, nextAlarm);

    size_t wr = prefs.putULong64("nat", nextAlarmEpochTime);
    if (wr != sizeof(uint64_t))
    {
      LOG_PRINT("putULong64 saved", wr, "bajts (expeceted 8)");
    }
}

void printMusicList(void)
{
  numberOfTracks = 0;

  if(musicList[numberOfTracks].length() == 0)
  {
    LOG_PRINT("The SD card audio file scan is empty, please check whether there are audio files in the SD card that meet the format!");
  }

  while (musicList[numberOfTracks].length())
  {
    numberOfTracks++;
  }
  LOG_PRINT("Music list found", numberOfTracks, "tracks");
}
