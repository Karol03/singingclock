#include <Wire.h>
#include "ds3231.h"
#include "Audio_MAX98357A.h"
#include "SD.h"
#include "driver/rtc_io.h"
#include <EEPROM.h>
#include <Update.h>

#define FIRMWARE_VERSION 1.00

// Adafruit Audio BFF pinout taken from
// https://learn.adafruit.com/adafruit-audio-bff/pinouts
#define APLIFIER_BCLK A3
#define APLIFIER_LRCLK A2
#define APLIFIER_DATA A1
#define SD_CHIP_SELECT_DATA GPIO_NUM_43
#define RTC_INT_GPIO GPIO_NUM_1
#define BFF_POWER_OFF GPIO_NUM_44

#define DST_FLAG_ADDR 0

#define clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))

#define PARSE_VARIABLE(name) if (strcmp(key, #name) == 0) { name = atoi(value); Serial.print("Changed defaul value of '"#name"' to "); Serial.println(name); }


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
static unsigned SUMMER_TIME_START_END = 0u;
static unsigned VOLUME = 4;


Audio_MAX98357A amplifier;
String musicList[100];
unsigned numberOfTracks;
RTClib myRTC;
DS3231 Clock;
static int isSerialEnabled = -1;

#define WAIT_FOR_SERIAL_FOR_S 2
#define WAIT_FOR_SERIAL_SINGLE_TIME() do { if (isSerialEnabled == -1) { for (int i = 0; i < (WAIT_FOR_SERIAL_FOR_S * 20) && !Serial; ++i) delay(50); isSerialEnabled = !!Serial; } } while(0)

void setup() {
  EEPROM.begin(1);  // single byte for checking daylight saving time

  digitalWrite(LED_BUILTIN, LOW);

  pinMode(BFF_POWER_OFF, OUTPUT);
  digitalWrite(BFF_POWER_OFF, HIGH);

  Wire.begin();
  Serial.begin(115200);

  turnOffTheAlarm();

  int i = 0;
  while (!amplifier.initI2S(/*_bclk=*/APLIFIER_BCLK, /*_lrclk=*/APLIFIER_LRCLK, /*_din=*/APLIFIER_DATA))
  {
    Serial.println("Initialize I2S failed !");
    if (i > 10)
    {
      end();
    }
    ++i;
  }
  
  i = 0;
  while (!amplifier.initSDCard(/*csPin=*/SD_CHIP_SELECT_DATA))
  {
    Serial.println("Initialize SD card failed !");
    if (i > 10)
    {
      end();
    }
    ++i;
  }

  Serial.println("Initialize succeed!");

  firmwareUpdate();

  setTime();

  loadConfig();

  updateForDaylightTime();  // must be called after loadConfig to first load required variables from file

  amplifier.scanSDMusic(musicList);
  printMusicList();
  VOLUME = clamp(VOLUME, 1, 9);
  amplifier.setVolume(VOLUME);
  amplifier.closeFilter();

  playRandom();

  Serial.print("Finished v");
  Serial.println(FIRMWARE_VERSION);
  
  end();
}

void loop() {  
}

void end()
{
  digitalWrite(LED_BUILTIN, LOW);
  setNextAlarm();
  esp_deep_sleep_start();
}

void turnOffTheAlarm()
{
    rtc_gpio_deinit(RTC_INT_GPIO);  // deinit to use A0 as CS gpio

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
  if (SUMMER_TIME_START_MONTH < SUMMER_TIME_START_END)
  {
    return (SUMMER_TIME_START_MONTH <= month && month <= SUMMER_TIME_START_END);
  }
  return (month <= SUMMER_TIME_START_MONTH || SUMMER_TIME_START_END <= month);
}

void updateForDaylightTime()
{
  if (!ENABLE_DAYLIGHT_SAVING_TIME || DAYLIGHT_SAVING_TIME_OFFSET == 0)
  {
    EEPROM.end();
    return;
  }

  bool isSummerTime = isSummerTimeNow();
  uint8_t dstFlag = EEPROM.read(DST_FLAG_ADDR);
  const uint8_t SUMMER_TIME_FLAG = 0xAB;
  const uint8_t WINTER_TIME_FLAG = 0xBC;
  if (dstFlag != SUMMER_TIME_FLAG && isSummerTime)
  {
    // set summer time
    uint64_t epoch_time = myRTC.now().unixtime() + int64_t(DAYLIGHT_SAVING_TIME_OFFSET) * 3600ull;
    Clock.setEpoch(epoch_time, false);
    Clock.setClockMode(false);
    EEPROM.write(DST_FLAG_ADDR, SUMMER_TIME_FLAG);   // set daylight summer time flag 
    EEPROM.commit();
    Serial.println("Update to daylight summer time");
  }
  else if (dstFlag != WINTER_TIME_FLAG && !isSummerTime)
  {
    // set winter time
    uint64_t epoch_time = myRTC.now().unixtime() - int64_t(DAYLIGHT_SAVING_TIME_OFFSET) * 3600ull;
    Clock.setEpoch(epoch_time, false);
    Clock.setClockMode(false);
    EEPROM.write(DST_FLAG_ADDR, WINTER_TIME_FLAG);   // set daylight summer time flag 
    EEPROM.commit();
    Serial.println("Update to winter time");
  }
  EEPROM.end();
}

void loadConfig()
{
    FILE* file = fopen("/sd/config.txt", "r");
    if (!file)
    {
        Serial.println("Failed to open config file, use default values");
        return;
    }

    WAIT_FOR_SERIAL_SINGLE_TIME();

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
        else PARSE_VARIABLE(UTC_OFFSET)
        else PARSE_VARIABLE(ENABLE_DAYLIGHT_SAVING_TIME)
        else PARSE_VARIABLE(DAYLIGHT_SAVING_TIME_OFFSET)
        else PARSE_VARIABLE(SUMMER_TIME_START_MONTH)
        else PARSE_VARIABLE(SUMMER_TIME_START_END)
        else PARSE_VARIABLE(VOLUME)
    }

    Serial.println("Config loaded from file");
    fclose(file);
}

void playRandom()
{
  if (numberOfTracks > 0)
  {
    const unsigned trackNo = esp_random() % numberOfTracks;
    amplifier.playSDMusic(musicList[trackNo].c_str());
    do { delay(200); } while (!amplifier.isStopped());
  }
}

void saveLog(byte alarmDay, byte alarmHour, byte alarmMinute)
{
  const char* MONTH_ARR[] = {"<err>", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  FILE* fp = fopen("/sd/log.txt", "a");
  if (!fp)
    return;

  bool century;
  DateTime datetime = myRTC.now();
  const uint64_t epochTime = datetime.unixtime();
  byte currentMonth = Clock.getMonth(century);
  byte currentDate = Clock.getDate();
  byte nextMonth = currentDate > alarmDay ? (currentMonth % 12 + 1) : currentMonth;
  if (fprintf(fp, "Called at %llu (%u %s %02u:%02u), next alarm at %u %s %02u:%02u\n",
    (unsigned long long)epochTime,
    currentDate, MONTH_ARR[currentMonth], datetime.hour(), datetime.minute(),
    alarmDay, MONTH_ARR[nextMonth], alarmHour, alarmMinute) < 0)
  {
      Serial.println("Failed to save timestamp in log.txt\n");
  }
  fclose(fp);
}

void firmwareUpdate()
{
  File firmware =  SD.open("/firmware.bin");
  if (firmware) 
  {
      Serial.println("Updating the firmware...");
      Update.begin(firmware.size(), U_FLASH);
      Update.writeStream(firmware);
      if (Update.end())
      {
          firmware.close();
          SD.remove("/firmware.bin");
          Serial.println("Firmware updated!");
          FILE* fp = fopen("/sd/log.txt", "a");
          if (fp)
          {
            fprintf(fp, "Firmware updated!\n");
            fclose(fp);
          }
          delay(200);
          ESP.restart();
      }
      else
      {
          firmware.close();
          Serial.println("Failed to update firmware!");
          Serial.println(Update.getError());
          FILE* fp = fopen("/sd/log.txt", "a");
          if (fp)
          {
            fprintf(fp, "Failed to update the firmware, err = %u\n", Update.getError());
            fclose(fp);
          }
      }
  }
}

void setTime()
{
  DateTime datetime = myRTC.now();
  Serial.print("Unix time is "); Serial.println(datetime.unixtime());
  Serial.print("Compilation time "); Serial.println(__DATE_TIME_UNIX__);
  if (datetime.unixtime() < __DATE_TIME_UNIX__) // enter here on first startup or after battery replacing
  {
    Clock.setEpoch(__DATE_TIME_UNIX__, false);
    Clock.setClockMode(false);
  }

  FILE* fp = fopen("/sd/timestamp.txt", "r");
  if (!fp)
    return;

  WAIT_FOR_SERIAL_SINGLE_TIME();

  Serial.println("Found new timestamp");
  uint64_t epoch_time;
  if (fscanf(fp, "%" SCNu64, &epoch_time) != 1)
  {
      Serial.println("Failed to read number from timestamp file\n");
      fclose(fp);
      return;
  }
  fclose(fp);
  
  if (!SD.remove("/timestamp.txt"))
  {
      Serial.println("Failed to remove timestamp file\n");
      return;
  }

  epoch_time += UTC_OFFSET * 3600;

  Clock.setEpoch(epoch_time, false);
  Clock.setClockMode(false);

  Serial.print("Epoch time updated : "); Serial.println(epoch_time);

  EEPROM.write(DST_FLAG_ADDR, 0);   // set daylight summer time flag
  EEPROM.commit();
}

byte calculateNextAlarmDate(byte alarmHour, byte alarmMinute)
{
    // calculate in month days
    static const byte daysInMonth[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    bool century;
    int  year   = Clock.getYear() + 2000;
    byte month = Clock.getMonth(century);
    byte dim = daysInMonth[month];
    byte addDays;
    DAYS_AHEAD_MIN = clamp(DAYS_AHEAD_MIN, 0, 28);
    DAYS_AHEAD_MAX = clamp(DAYS_AHEAD_MAX, 0, 28);
    if (DAYS_AHEAD_MIN > DAYS_AHEAD_MAX)
      return Clock.getDate();
    if (DAYS_AHEAD_MIN == DAYS_AHEAD_MAX)
      addDays = DAYS_AHEAD_MIN;
    else
      addDays = (esp_random() % (DAYS_AHEAD_MAX - DAYS_AHEAD_MIN + 1)) + DAYS_AHEAD_MIN;
    if (addDays == 0)
    {
      DateTime datetime = myRTC.now();
      if (datetime.hour() > alarmHour || (datetime.hour() == alarmHour && datetime.minute() >= alarmMinute - 2))
      {
        // If a date and time from the past was drawn, add a single day 
        addDays = 1;
      }
    }
    byte newDate = Clock.getDate() + addDays;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
      dim = 29;
    }
    if (newDate > dim)
      newDate -= dim;
    return newDate;
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
    // Format: .setA*Time(DoW|Date, Hour, Minute, Second, 0x0, DoW|Date, 12h|24h, am|pm)
    //                    |                                    |         |        |
    //                    |                                    |         |        +--> when set for 12h time, true for pm, false for am
    //                    |                                    |         +--> true if setting time based on 12 hour, false if based on 24 hour
    //                    |                                    +--> true if you're setting DoW, false for absolute date
    //                    +--> INTEGER representing day of the week, 1 to 7 (Monday to Sunday)
    //
    const unsigned MINUTES_IN_HOUR = 60;
    bool alarmH12 = false;
    bool alarmPM = false;
    byte alarmHour = calculateNextAlarmHour();
    byte alarmMinute = calculateNextAlarmMinute();
    byte alarmDay = calculateNextAlarmDate(alarmHour, alarmMinute);
    byte alarmBits = 0b00000000; // alarm when DoM, hour, minute match
    byte a1dy = false; // true makes the alarm go on A1Day = Day of Week, false - Day of Month

    saveLog(alarmDay, alarmHour, alarmMinute);

    Clock.setA2Time(
       alarmDay, alarmHour, alarmMinute,
       alarmBits, a1dy, alarmH12, alarmPM);
    Clock.checkIfAlarm(2);
    // now it is safe to enable interrupt output
    Clock.turnOnAlarm(2);

    rtc_gpio_init(RTC_INT_GPIO);
    rtc_gpio_set_direction(RTC_INT_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(RTC_INT_GPIO);
    rtc_gpio_pullup_en(RTC_INT_GPIO);
    esp_sleep_enable_ext0_wakeup(RTC_INT_GPIO, 0);
}

void printMusicList(void)
{
  numberOfTracks = 0;

  if(musicList[numberOfTracks].length())
  {
    Serial.println("\nMusic List: ");
  }
  else
  {
    Serial.println("The SD card audio file scan is empty, please check whether there are audio files in the SD card that meet the format!");
  }

  while (musicList[numberOfTracks].length())
  {
    Serial.print("\t");
    Serial.print(numberOfTracks);
    Serial.print("  -  ");
    Serial.println(musicList[numberOfTracks]);
    numberOfTracks++;
  }
}

