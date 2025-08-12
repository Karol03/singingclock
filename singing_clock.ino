#include <Wire.h>
#include "ds3231.h"
#include "Audio_MAX98357A.h"
#include "SD.h"
#include "driver/rtc_io.h"

// Adafruit Audio BFF pinout taken from
// https://learn.adafruit.com/adafruit-audio-bff/pinouts
#define APLIFIER_BCLK A3
#define APLIFIER_LRCLK A2
#define APLIFIER_DATA A1
#define SD_CHIP_SELECT_DATA A0
#define RTC_INT_GPIO GPIO_NUM_1


static byte ALARM_BITS = 0b00000000;
static bool IS_DAYS_OF_WEEK = true;
static unsigned DAYS_AHEAD_MIN = 1u;
static unsigned DAYS_AHEAD_MAX = 3u;
static unsigned SINCE_HOUR = 6u;
static unsigned UNTIL_HOUR = 20u;
static unsigned SINCE_MIN = 0u;
static unsigned UNTIL_MIN = 59u;


Audio_MAX98357A amplifier;
String musicList[100];
unsigned numberOfTracks;
RTClib myRTC;
DS3231 Clock;
static int isSerialEnabled = -1;

#define WAIT_FOR_SERIAL_FOR_S 2
#define WAIT_FOR_SERIAL_SINGLE_TIME() do { if (isSerialEnabled == -1) { for (int i = 0; i < (WAIT_FOR_SERIAL_FOR_S * 20) && !Serial; ++i) delay(50); isSerialEnabled = !!Serial; } } while(0)

void setup() {
  digitalWrite(LED_BUILTIN, HIGH);

  Wire.begin();
  Serial.begin(115200);

  turnOffTheAlarm();

  int i = 0;
  while (!amplifier.initI2S(/*_bclk=*/APLIFIER_BCLK, /*_lrclk=*/APLIFIER_LRCLK, /*_din=*/APLIFIER_DATA))
  {
    Serial.println("Initialize I2S failed !");
    delay(1000);
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
    delay(1000);
    if (i > 10)
    {
      end();
    }
    ++i;
  }

  Serial.println("Initialize succeed!");

  setTime();

  loadConfig();

  amplifier.scanSDMusic(musicList);
  printMusicList();
  amplifier.setVolume(5);
  amplifier.closeFilter();

  playRandom();

  digitalWrite(LED_BUILTIN, LOW);

  setNextAlarm();

  esp_deep_sleep_start();
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

        if (strcmp(key, "IS_DAYS_OF_WEEK") == 0)
        {
          IS_DAYS_OF_WEEK = atoi(value);
          Serial.print("Changed defaul value of 'IS_DAYS_OF_WEEK' to "); Serial.println(IS_DAYS_OF_WEEK);
        }
        else if (strcmp(key, "ALARM_BITS") == 0)
        {
          ALARM_BITS = atoi(value);
          Serial.print("Changed defaul value of 'ALARM_BITS' to "); Serial.println(ALARM_BITS);
        }
        else if (strcmp(key, "DAYS_AHEAD_MIN") == 0)
        {
          DAYS_AHEAD_MIN = atoi(value);
          Serial.print("Changed defaul value of 'DAYS_AHEAD_MIN' to "); Serial.println(DAYS_AHEAD_MIN);
        }
        else if (strcmp(key, "DAYS_AHEAD_MAX") == 0)
        {
          DAYS_AHEAD_MAX = atoi(value);
          Serial.print("Changed defaul value of 'DAYS_AHEAD_MAX' to "); Serial.println(DAYS_AHEAD_MAX);
        }
        else if (strcmp(key, "SINCE_HOUR") == 0)
        {
          SINCE_HOUR = atoi(value);
          Serial.print("Changed defaul value of 'SINCE_HOUR' to "); Serial.println(SINCE_HOUR);
        }
        else if (strcmp(key, "UNTIL_HOUR") == 0)
        {
          UNTIL_HOUR = atoi(value);
          Serial.print("Changed defaul value of 'UNTIL_HOUR' to "); Serial.println(UNTIL_HOUR);
        }
        else if (strcmp(key, "SINCE_MIN") == 0)
        {
          SINCE_MIN = atoi(value);
          Serial.print("Changed defaul value of 'SINCE_MIN' to "); Serial.println(SINCE_MIN);
        }
        else if (strcmp(key, "UNTIL_MIN") == 0)
        {
          UNTIL_MIN = atoi(value);
          Serial.print("Changed defaul value of 'UNTIL_MIN' to "); Serial.println(UNTIL_MIN);
        }
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
  const char* DAY_ARR[] = {"<err>", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  FILE* fp = fopen("/sd/log.txt", "a");
  DateTime datetime = myRTC.now();
  if (!fp)
    return;

  const uint64_t epochTime = datetime.unixtime();
  if (fprintf(fp, "Called at %llu (%s %02u:%02u), next alarm at %s %02u:%02u\n",
    (unsigned long long)epochTime,
    DAY_ARR[Clock.getDoW()], datetime.hour(), datetime.minute(),
    DAY_ARR[alarmDay], alarmHour, alarmMinute) < 0)
  {
      Serial.println("Failed to save timestamp in log.txt\n");
  }
  fclose(fp);
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
  Clock.setEpoch(epoch_time, false);
  Clock.setClockMode(false);
  Serial.print("Epoch time updated : "); Serial.println(epoch_time);
}

byte calculateNextAlarmDate()
{
  if (IS_DAYS_OF_WEEK)
  {
    // calculate in week days
    const byte DAYS_IN_WEEK = 7;
    byte addDays;
    if (DAYS_AHEAD_MAX >= 7)
      DAYS_AHEAD_MAX = 6;
    if (DAYS_AHEAD_MIN > DAYS_AHEAD_MAX)
      return Clock.getDoW();
    else if (DAYS_AHEAD_MIN == DAYS_AHEAD_MAX)
      addDays = DAYS_AHEAD_MIN;
    else
      addDays = (esp_random() % (DAYS_AHEAD_MAX - DAYS_AHEAD_MIN + 1)) + DAYS_AHEAD_MIN;
    return ((Clock.getDoW() + addDays - 1) % DAYS_IN_WEEK) + 1;
  }
  else
  {
    // calculate in month days
    static const byte daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool century;
    int  year   = Clock.getYear() + 2000;
    byte month = Clock.getMonth(century);
    byte dim = daysInMonth[month - 1];
    byte addDays;
    if (DAYS_AHEAD_MAX >= 29)
      DAYS_AHEAD_MAX = 28;
    if (DAYS_AHEAD_MIN > DAYS_AHEAD_MAX)
      return Clock.getDate();
    if (DAYS_AHEAD_MIN == DAYS_AHEAD_MAX)
      addDays = DAYS_AHEAD_MIN;
    else
      addDays = (esp_random() % (DAYS_AHEAD_MAX - DAYS_AHEAD_MIN + 1)) + DAYS_AHEAD_MIN;
    byte newDate = Clock.getDate() + addDays;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
      dim = 29;
    }
    if (newDate > dim)
      newDate -= dim;
    return newDate;
  }
}

byte calculateNextAlarmHour()
{
    if (UNTIL_HOUR >= 24)
      UNTIL_HOUR = 23;
    if (SINCE_HOUR >= UNTIL_HOUR)
      return SINCE_HOUR % 24;
    return (esp_random() % (UNTIL_HOUR - SINCE_HOUR + 1)) + SINCE_HOUR;
}

byte calculateNextAlarmMinute()
{
    if (UNTIL_MIN >= 60)
      UNTIL_MIN = 59;
    if (SINCE_MIN >= UNTIL_MIN)
      return SINCE_MIN % 60;
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
    byte alarmDay = calculateNextAlarmDate();
    byte alarmHour = calculateNextAlarmHour();
    byte alarmMinute = calculateNextAlarmMinute();
    byte alarmBits = (IS_DAYS_OF_WEEK ? 0b00001000 : 0b00000000) | ALARM_BITS; // 0b00001000; // alarm when DoW, hour, minute match
    byte a1dy = IS_DAYS_OF_WEEK; // true makes the alarm go on A1Day = Day of Week

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

