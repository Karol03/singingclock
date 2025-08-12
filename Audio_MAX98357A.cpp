/*!
 * @file  Audio_MAX98357A.cpp
 * @brief  Define the infrastructure Audio_MAX98357A class
 * @details  Configure a classic Bluetooth, pair with Bluetooth devices, receive Bluetooth audio, 
 * @n        Process simple audio signal, and pass it into the amplifier using I2S communication
 * @copyright  Copyright (c) 2010 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license  The MIT License (MIT)
 * @author  [qsjhyy](yihuan.huang@dfrobot.com)
 * @version  V1.0
 * @date  2022-01-21
 * @url  https://github.com/DFRobot/Audio_MAX98357A
 */
#include "Audio_MAX98357A.h"

float _volume = 1.0;   // Change the coefficient of audio signal volume
int32_t _sampleRate = 44100;   // I2S communication frequency
bool _filterFlag = false;   // Filter enabling flag

uint8_t _voiceSource = MAX98357A_VOICE_FROM_BT;   // The audio source, used to correct left and right audio

Biquad _filterLLP[NUMBER_OF_FILTER];   // Left channel low-pass filter
Biquad _filterRLP[NUMBER_OF_FILTER];   // Right channel low-pass filter
Biquad _filterLHP[NUMBER_OF_FILTER];   // Left channel high-pass filter
Biquad _filterRHP[NUMBER_OF_FILTER];   // Right channel high-pass filter

char fileName[100];
uint8_t SDAmplifierMark = SD_AMPLIFIER_STOP;   // SD card play flag
xTaskHandle xPlayWAV = NULL;   // SD card play Task
String _musicList[100];   // SD card music list
uint8_t musicCount = 0;   // SD card music count

/**
 * @struct sWavParse_t
 * @brief The struct for parsing audio information in WAV format
 */
typedef struct
{
    char                  riffType[4];
    unsigned int          riffSize;
    char                  waveType[4];
    char                  formatType[4];
    unsigned int          formatSize;
    uint16_t              compressionCode;
    i2s_channel_t         numChannels;
    uint32_t              sampleRate;
    unsigned int          bytesPerSecond;
    unsigned short        blockAlign;
    i2s_bits_per_sample_t bitsPerSample;
    char                  dataType1[1];
    char                  dataType2[3];
    unsigned int          dataSize;
    char                  data[800];
}sWavParse_t;

/**
 * @struct sWavInfo_t
 * @brief All the information of the audio in WAV format
 */
typedef struct
{
    sWavParse_t header;
    FILE *fp;
}sWavInfo_t;

/*************************** Init ******************************/

Audio_MAX98357A::Audio_MAX98357A()
{
}

bool Audio_MAX98357A::begin(const char *btName, int bclk, int lrclk, int din)
{
  // Initialize I2S
  if (!initI2S(bclk, lrclk, din)){
    DBG("Initialize I2S failed !");
    return false;
  }

  // Initialize the filter
  setFilter(_filterLLP, bq_type_lowpass, 20000.0);
  setFilter(_filterRLP, bq_type_lowpass, 20000.0);
  setFilter(_filterLHP, bq_type_highpass, 2.0);
  setFilter(_filterRHP, bq_type_highpass, 2.0);

  return true;
}

bool Audio_MAX98357A::initI2S(int _bclk, int _lrclk, int _din)
{
  static const i2s_config_t i2s_config = {
    .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),   // The main controller can transmit data but not receive.
    .sample_rate = _sampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,   // 16 bits per sample
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // 2-channels
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,   // I2S communication I2S Philips standard, data launch at second BCK
    .intr_alloc_flags = ESP_INTR_FLAG_NMI,   // Interrupt level 1
    .dma_buf_count = 12,   // number of buffers, 128 max.
    .dma_buf_len = 1024,   // size of each buffer, AVRC communication may be affected if the value is too high.
    .use_apll = false,   // For the application of a high precision clock, select the APLL_CLK clock source in the frequency range of 16 to 128 MHz. It's not the case here, so select false.
    .tx_desc_auto_clear = true
  };

  static const i2s_pin_config_t pin_config = {
    .bck_io_num = _bclk,   // Serial clock (SCK), aka bit clock (BCK)
    .ws_io_num = _lrclk,   // Word select (WS), i.e. command (channel) select, used to switch between left and right channel data
    .data_out_num = _din,   // Serial data signal (SD), used to transmit audio data in two's complement format
    .data_in_num = I2S_PIN_NO_CHANGE   // Not used
  };

  if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL)){
    DBG("Install and start I2S driver failed !");
    return false;
  }
  if (i2s_set_pin(I2S_NUM_0, &pin_config)){
    DBG("Set I2S pin number failed !");
    return false;
  }

  return true;
}

bool Audio_MAX98357A::initSDCard(uint8_t csPin)
{
  if(!SD.begin(csPin)){
    DBG("Card Mount Failed");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if(cardType == CARD_NONE){
    DBG("No SD card attached");
    return false;
  }

  DBG("SD Card Type: ");
  if(cardType == CARD_MMC){
    DBG("MMC");
  } else if(cardType == CARD_SD){
    DBG("SDSC");
  } else if(cardType == CARD_SDHC){
    DBG("SDHC");
  } else {
    DBG("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  DBG("SD Card Size: ");
  DBG(cardSize);
  // Serial.printf("SD Card Size: %lluMB\n", cardSize);

  _voiceSource = MAX98357A_VOICE_FROM_SD;

  SDAmplifierMark = SD_AMPLIFIER_STOP;
  xTaskCreate(&playWAV, "playWAV", 2048, NULL, 5, &xPlayWAV);

  return true;
}

/*************************** Function ******************************/

void Audio_MAX98357A::reverseLeftRightChannels(void)
{
  _voiceSource = (_voiceSource ? MAX98357A_VOICE_FROM_SD : MAX98357A_VOICE_FROM_BT);
}

void Audio_MAX98357A::listDir(fs::FS &fs, const char * dirName)
{
  DBG(dirName);
  DBG("|");

  File root = fs.open(dirName);
  if(!root){
    DBG("Failed to open directory");
    return;
  }
  if(!root.isDirectory()){
    DBG("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while(file){
    // DBG(file.name());
    if(file.isDirectory()){
      listDir(fs, file.path());
    } else {
      DBG(file.path());
      if(strstr(file.name(), ".wav")){
        _musicList[musicCount] = file.path();
        musicCount++;
      }
    }
    file = root.openNextFile();
  }
}

void Audio_MAX98357A::scanSDMusic(String * musicList)
{
  musicCount = 0;   // Discard original list
  listDir(SD, "/");
  uint8_t i = 0;
  while(i < musicCount){
    musicList[i] = _musicList[i];
    i++;
  }

  // Set playing music by default
  char SDName[80]="/sd";
  strcat(SDName, musicList[0].c_str());
  strcpy(fileName, SDName);
}

void Audio_MAX98357A::playSDMusic(const char *musicName)
{
  SDPlayerControl(SD_AMPLIFIER_STOP);
  char SDName[80]="/sd";   // The default SD card mount point in SD.h
  strcat(SDName, musicName);   // It need to be an absolute path.
  strcpy(fileName, SDName);
  SDPlayerControl(SD_AMPLIFIER_PLAY);
}

bool Audio_MAX98357A::isStopped() const
{
  return SDAmplifierMark == SD_AMPLIFIER_STOP;
}

void Audio_MAX98357A::SDPlayerControl(uint8_t CMD)
{
  SDAmplifierMark = CMD;
  delay(10);   // Wait music playback to stop
}

void Audio_MAX98357A::setVolume(float vol)
{
  vol /= 5.0;   // vol range is 0-9
  _volume = constrain(vol, 0.0, 2.0);
}

void Audio_MAX98357A::openFilter(int type, float fc)
{
  if(bq_type_lowpass == type){   // Set low-pass filter
    setFilter(_filterLLP, type, fc);
    setFilter(_filterRLP, type, fc);
  }else{   // Set high-pass filter
    setFilter(_filterLHP, type, fc);
    setFilter(_filterRHP, type, fc);
  }
  _filterFlag = true;
}

void Audio_MAX98357A::closeFilter(void)
{
  _filterFlag = false;
}

void Audio_MAX98357A::setFilter(Biquad * _filter, int _type, float _fc)
{
  _fc = (constrain(_fc, 2.0, 20000.0)) / (float)_sampleRate;   // Ratio of filter threshold to sampling frequency, range: 0.0-0.5
  float Q;
  for(int i; i<NUMBER_OF_FILTER; i++){
    Q = 1 / (2 * cos( PI / (NUMBER_OF_FILTER * 4) + i * PI / (NUMBER_OF_FILTER * 2) ));
    DBG("\n-------- Q ");
    DBG(Q);
    DBG("++++++++ _fc ");
    DBG(_fc);
    DBG("++++++++ _type ");
    DBG(_type);
    _filter[i].setBiquad(_type, _fc, Q, 0);
  }
}

int16_t Audio_MAX98357A::filterToWork(Biquad * filterHP, Biquad * filterLP, float rawData)
{
  for(int i; i<NUMBER_OF_FILTER; i++){
    rawData = filterLP[i].process(rawData);
  }

  for(int i; i<NUMBER_OF_FILTER; i++){
    rawData = filterHP[i].process(rawData);
  }

  return (int16_t)(constrain(rawData, -32767, 32767));
}

/*************************** Function ******************************/

void Audio_MAX98357A::audioDataProcessCallback(const uint8_t *data, uint32_t len)
{
  int16_t* data16 = (int16_t*)data;   // Convert to 16-bit sample data
  int16_t processedData[2];   // Store the processed audio data
  int count = len / 4;   // The number of audio data to be processed in int16_t[2]
  size_t i2s_bytes_write = 0;   // i2s_write() the variable storing the number of data to be written

  if(!_filterFlag){   // Change sample data only according to volume multiplier
    for(int i=0; i<count; i++){
      processedData[0+_voiceSource] = (int16_t)((*data16) * _volume);   // Change audio data volume of left channel
      DBG(processedData[0], HEX);
      data16++;

      processedData[1-_voiceSource] = (int16_t)((*data16) * _volume);   // Change audio data volume of right channel
      DBG(processedData[1], HEX);
      data16++;

      i2s_write(I2S_NUM_0,  processedData, 4, &i2s_bytes_write, 20);   // Transfer audio data to the amplifier via I2S
    }
  }else{   // Filtering with a simple digital filter
    for(int i=0; i<count; i++){
      processedData[0+_voiceSource] = filterToWork(_filterLHP, _filterLLP, ((*data16) * _volume));   // Change audio data volume of left channel, and perform filtering operation
      data16++;

      processedData[1-_voiceSource] = filterToWork(_filterRHP, _filterRLP, ((*data16) * _volume));   // Change audio data volume of right channel, and perform filtering operation
      data16++;

      i2s_write(I2S_NUM_0, processedData, 4, &i2s_bytes_write, 100);   // Transfer audio data to the amplifier via I2S
    }
  }
}

void Audio_MAX98357A::playWAV(void *arg)
{
  while(1){
    while(SD_AMPLIFIER_STOP == SDAmplifierMark){
      vTaskDelay(100);
    }

    sWavInfo_t * wav = (sWavInfo_t *)calloc(1, sizeof(sWavInfo_t));
    if(wav == NULL){
      DBG("Unable to allocate WAV struct.");
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;
    }

    wav->fp = fopen(fileName, "rb");
    if(wav->fp == NULL){
      DBG("Unable to open wav file.");
      DBG(fileName);
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;
    }
    if(fread(&(wav->header.riffType), 1, 4, wav->fp) != 4){
      DBG("couldn't read RIFF_ID.");
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;  /* bad error "couldn't read RIFF_ID" */
    }
    if(strncmp("RIFF", wav->header.riffType, 4)){
      DBG("RIFF descriptor not found.") ;
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;
    }
    fread(&(wav->header.riffSize), 4, 1, wav->fp);
    if(fread(&wav->header.waveType, 1, 4, wav->fp) != 4){
      DBG("couldn't read format");
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;  /* bad error "couldn't read format" */
    }
    if(strncmp("WAVE", wav->header.waveType, 4)){
      DBG("WAVE chunk ID not found.") ;
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;
    }
    if(fread(&(wav->header.formatType), 1, 4, wav->fp) != 4){
      DBG("couldn't read format_ID");
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;  /* bad error "couldn't read format_ID" */
    }
    if(strncmp("fmt", wav->header.formatType, 3)){
      DBG("fmt chunk format not found.");
      SDAmplifierMark = SD_AMPLIFIER_STOP;
      continue;
    }
    fread(&(wav->header.formatSize), 4, 1, wav->fp);
    fread(&(wav->header.compressionCode), 2, 1, wav->fp);
    fread(&(wav->header.numChannels), 2, 1, wav->fp);
    fread(&(wav->header.sampleRate), 4, 1, wav->fp);
    fread(&(wav->header.bytesPerSecond), 4, 1, wav->fp);
    fread(&(wav->header.blockAlign), 2, 1, wav->fp);
    fread(&(wav->header.bitsPerSample), 2, 1, wav->fp);
    while(1){
      if(fread(&wav->header.dataType1, 1, 1, wav->fp) != 1){
        DBG("Unable to read data chunk ID.");
        free(wav);
        break;
      }
      if(strncmp("d", wav->header.dataType1, 1) == 0){
        fread(&wav->header.dataType2, 3, 1, wav->fp);
        if(strncmp("ata", wav->header.dataType2, 3) == 0){
          fread(&(wav->header.dataSize),4,1,wav->fp);
          break;
        }
      }
    }

    i2s_set_sample_rates(I2S_NUM_0, wav->header.sampleRate);   // Set I2S sampling rate based on the parsed audio sampling frequency

    unsigned chunkSize = 0;
    while((chunkSize = fread(&wav->header.data, 1 , 800 , wav->fp))){
      audioDataProcessCallback((uint8_t *)&wav->header.data, chunkSize);   // Send the parsed audio data to the amplifier broadcast function
      if(SD_AMPLIFIER_STOP == SDAmplifierMark){
        break;
      }
      while(SD_AMPLIFIER_PAUSE == SDAmplifierMark){
        vTaskDelay(100);
      }
    }

    fclose(wav->fp);
    free(wav);
    SDAmplifierMark = SD_AMPLIFIER_STOP;
    vTaskDelay(100);
  }
  vTaskDelete(xPlayWAV);
}