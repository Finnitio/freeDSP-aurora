#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <DNSServer.h>
#include "AsyncJson.h"
#include <ArduinoJson.h>
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#include "hwconfig.h"
#include "AK4458.h"
#include "AK5558.h"

#if HAVE_ROTARYENCODER
#include "rotaryencoder.h"
#endif

#if HAVE_IRRECEIVER
#include <IRremote.h>
#endif

#include "config.h"
#include "settings.h"
#include "adau1452.h"
#include "addons.h"
#include "display.h"
#include "plugin.h"
#include "web.h"
#include "channelnames.h"
#include "inputrouting.h"
#include "staretry.h"
#include "apswitch.h"
#include "apconfig.h"
#include "i2cworker.h"
#include "matrix.h"
#include "pendingredraw.h"

/**
 * These are required for the espmake32 automatic library resolver
 * It's less powerful than the one from the Arduino IDE.
 * The if-guards can stay
 * see https://github.com/plerup/makeEspArduino
 */
#if 0
#include <FS.h>
#include <WebServer.h>
#include <AsyncTCP.h>
#include <u8g2.h>
#endif

File fileDspProgram;

bool changeWifiState = false;
// Set by startAP(): the AP runs open because the stored password is invalid
volatile bool apOpenFallback = false;
volatile bool apUp = false;  //!< the AP is running (off with WiFi off or "AP off while connected")
const char* apPasswordError = NULL;  //!< why, while apOpenFallback is set
DNSServer dnsServer;

//------------------------------------------------------------------------------
//
// Display
//
//------------------------------------------------------------------------------
#if HAVE_DISPLAY
Display myDisplay;
#endif

//------------------------------------------------------------------------------
//
// Rotary Encoder
//
//------------------------------------------------------------------------------
#if HAVE_ROTARYENCODER
long int lastREval = 0;
long int lastREsw = 0;
#endif

//------------------------------------------------------------------------------
//
// IR Receiver
//
//------------------------------------------------------------------------------
#if HAVE_IRRECEIVER
IRrecv irReceiver( IR_RECEIVER_PIN );
#endif

//==============================================================================
/*! Uploads the firmware from ESP32 SPI flash to DSP.
 */
void uploadDspFirmware( void )
{
  Serial.print(F("Init dsp......"));

  if(!SPIFFS.exists(F("/dsp.fw")))
  {
    Serial.println(F("File does not exist yet"));
    return;
  }

  fileDspProgram = SPIFFS.open(F("/dsp.fw"));

  uint32_t numBytesToRead = 0;
  byte byteReadMSB;
  byte byteReadLSB;
  uint16_t regaddr;

  if(fileDspProgram)
  {
    size_t len = fileDspProgram.size();
    int cntr = 0;
    while(cntr < len)
    {
      uint8_t byteRead;
      fileDspProgram.read( &byteRead, 1 );
      numBytesToRead = (numBytesToRead << 8) + (uint32_t)byteRead;
      fileDspProgram.read( &byteRead, 1 );
      numBytesToRead = (numBytesToRead << 8) + (uint32_t)byteRead;
      fileDspProgram.read( &byteRead, 1 );
      numBytesToRead = (numBytesToRead << 8) + (uint32_t)byteRead;
      fileDspProgram.read( &byteRead, 1 );
      numBytesToRead = (numBytesToRead << 8) + (uint32_t)byteRead;

      cntr += 4;

      //------------------------------------------------------------------------
      //--- Send register address and value
      //------------------------------------------------------------------------
      if(numBytesToRead > 0)
      {
        fileDspProgram.read(&byteRead, 1);
        regaddr = ((uint16_t)byteRead) << 8;
        cntr++;

        fileDspProgram.read(&byteRead, 1);
        regaddr += byteRead;
        cntr++;

        if(numBytesToRead == 4)
        {
          fileDspProgram.read(&byteReadMSB, 1);
          cntr++;
          fileDspProgram.read(&byteReadLSB, 1);
          cntr++;

          if(regaddr == 0x0000)
            delay(500);
          else
          {
            ADAU1452_WRITE_REGISTER(regaddr, byteReadMSB, byteReadLSB);
            if(cntr % 4096 == 0)
              Serial.print(F("."));
          }
        }
        else if(numBytesToRead > 4)
        {
          uint16_t addr = regaddr;
          byte val[4];

          for(uint32_t ii = 0; ii < numBytesToRead - 2; ii = ii + 4)
          {
            fileDspProgram.read(&(val[0]), 1);
            fileDspProgram.read(&(val[1]), 1);
            fileDspProgram.read(&(val[2]), 1);
            fileDspProgram.read(&(val[3]), 1);
            cntr += 4;
            ADAU1452_WRITE_BLOCK(addr, val, 4);
            if(cntr % 4096 == 0)
              Serial.print(F("."));
            addr++;
          }
        }
        else
        {
          Serial.print(F("[ERROR] Bad number of bytes: "));
          Serial.println(numBytesToRead);
        }
      }
    }
    int file_size = fileDspProgram.size();

    fileDspProgram.close();
    Serial.println(F("[OK]"));
    Serial.print(F("File size: "));
    Serial.println(file_size);
  }
  else
    Serial.println(F("\n[ERROR] Failed to open file dsp.fw"));
}

//==============================================================================
/*! The redraw loop() owes. Drawn only when the bus is free, so loop() never
 *  waits for a Preset load or FIR upload on the worker.
 */
static PendingRedraw pendingRedraw;

#if HAVE_DISPLAY
static String uiStatusLine( void )
{
  bool connected = WiFi.status() == WL_CONNECTED;
  String ip = connected ? WiFi.localIP().toString() : String( F("Not Connected") );
  char line[AP_STATUS_LINE_SIZE];
  apStatusLine( line, sizeof( line ), ip.c_str(), connected, apOpenFallback, apUp );
  return String( line );
}

// The edit modes the main UI shows (volume, Preset, virtual input).
static bool editModeHasUI( void )
{
  return (editMode == 0) || (editMode == 1) || (editMode == 2);
}

//==============================================================================
/*! Updates the user interface on the display. Waits for the bus, so it is for
 *  setup() and the I2C worker's jobs (including the web handlers' jobs posted
 *  there); loop() requests a redraw from pendingRedraw instead.
 */
void updateUI( void )
{
  if(haveDisplay)
  {
    if( editModeHasUI() )
      myDisplay.drawUI(currentPlugInName.c_str(), uiStatusLine().c_str(), currentPreset, masterVolume.val, editMode);
  }
}

static bool tryDrawScreen( PendingRedraw::Screen screen )
{
  if( !haveDisplay )
    return true;
  if( screen == PendingRedraw::SWITCHING_PRESET )
    return myDisplay.tryDrawSwitchingPreset();
  if( editModeHasUI() )
    return myDisplay.tryDrawUI(currentPlugInName.c_str(), uiStatusLine().c_str(), currentPreset, masterVolume.val, editMode);
  return true;
}
#else
// Without a display there is nothing to draw; loop() and the worker jobs stay the same.
void updateUI( void ) {}
static bool tryDrawScreen( PendingRedraw::Screen ) { return true; }
#endif

//==============================================================================
/*! Starts the access point. Must be called again after every WiFi.mode() change.
 *
 */
static void startAP()
{
  IPAddress ip(192, 168, 5, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Settings from an older file or edited by hand may be invalid.
  // Copies: the web task may replace Settings while the plan points into them.
  const String name = Settings.apname;
  const String pwd = Settings.pwdap;
  const ApPlan plan = apPlan(pwd.c_str(), name.c_str(), Settings.apchannel, AP_HOSTNAME);
  if(plan.nameError)
  {
    Serial.print(F("[ERROR] AP settings: "));
    Serial.println(plan.nameError);
    Serial.println(F("Falling back to the default AP name and channel"));
  }
  if(plan.openFallback)
  {
    // Open so the web UI stays reachable; pwdap stays as stored until replaced
    Serial.print(F("[ERROR] AP settings: "));
    Serial.println(plan.pwdError);
    Serial.println(F("[WARNING] AP runs OPEN until a valid AP password is stored"));
  }
  apPasswordError = plan.pwdError;
  apOpenFallback = plan.openFallback;

  bool ok;
  if(plan.pwd)
  {
    Serial.print(F("AP password protected "));
    Serial.println(plan.name);
  }
  else
  {
    Serial.print(F("AP open: "));
    Serial.println(plan.name);
  }
  ok = WiFi.softAP(plan.name, plan.pwd, plan.channel);
  Serial.print(F("softAP "));
  Serial.print(ok ? F("started, channel ") : F("FAILED, channel "));
  Serial.println(plan.channel);
  delay(100);

  if(!WiFi.softAPConfig(ip, ip, subnet))
    Serial.println(F("AP Config Failed"));

  // Answer every DNS name with the AP address
  dnsServer.stop();
  dnsServer.start(53, "*", ip);
  apUp = true;
  needUpdateUI = true;  // after apUp: the UI task on the other core redraws from it
}

//==============================================================================
/*! Stops the access point; the STA side keeps its connection.
 *
 */
static void stopAP()
{
  dnsServer.stop();
  if(!WiFi.softAPdisconnect(true))  // true: AP_STA -> STA
    Serial.println(F("[ERROR] Stopping the AP failed"));
  Serial.println(F("AP stopped (AP off while connected)"));
  apUp = false;
  needUpdateUI = true;
}

//==============================================================================
/*! Prints why the STA side is not connected.
 *
 */
static void printStaStatus(wl_status_t state)
{
  switch(state)
  {
  case WL_IDLE_STATUS:     Serial.println(F("Idle")); break;
  case WL_NO_SSID_AVAIL:   Serial.println(F("No SSID available")); break;
  case WL_CONNECT_FAILED:  Serial.println(F("WiFi connection failed")); break;
  case WL_CONNECTION_LOST: Serial.println(F("WiFi connection lost")); break;
  case WL_DISCONNECTED:    Serial.println(F("WiFi disconnected")); break;
  default: break;
  }
}

//==============================================================================
/*! Wifi connection task
 *
 */
void myWiFiTask(void *pvParameters)
{
  bool wasConnected = false;
  wl_status_t lastState = WL_CONNECTED;
  StaRetry staRetry;
  ApSwitch apSwitch;  // "AP off while connected"; the AP is up after startAP() below
  // Credentials STA currently uses; empty SSID = STA off (pure AP)
  String ssid = Settings.ssid;
  String password = Settings.password;

  // Credentials live in our settings file; don't let the core copy them to NVS
  // and don't let it reconnect on its own (it would scan forever and drag the
  // AP channel along). Reconnects are driven by StaRetry below.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  // WiFi is never switched off. The AP is only dropped while connected if the
  // user enabled "AP off while connected" (ApSwitch below).
  WiFi.mode(ssid.length() > 0 ? WIFI_AP_STA : WIFI_AP);
  WiFi.setHostname(AP_HOSTNAME);
  startAP();

  // Start server
  server.begin();

  while(true)
  {
    // POST /wifi changes the credentials without a reboot
    if(Settings.ssid != ssid || Settings.password != password)
    {
      const bool staWasOn = ssid.length() > 0;
      ssid = Settings.ssid;
      password = Settings.password;
      const bool staOn = ssid.length() > 0;
      if(staWasOn)
        WiFi.disconnect();  // keeps the mode; a stopped AP restarts via ApSwitch
      if(staOn != staWasOn)
      {
        WiFi.mode(staOn ? WIFI_AP_STA : WIFI_AP);
        startAP();
        apSwitch = ApSwitch();  // the AP is up again
      }
      staRetry = StaRetry();  // fresh retry rule for the new network
    }

    wl_status_t state = WiFi.status();
    bool connected = (state == WL_CONNECTED);
    // Before the STA retry, so a restarted AP's mode change can't abort a new attempt
    switch(apSwitch.update(Settings.apOffWhileConnected && ssid.length() > 0, connected, millis()))
    {
    case ApSwitch::START:
      Serial.println(F("Restarting the AP"));
      WiFi.mode(WIFI_AP_STA);
      startAP();
      break;
    case ApSwitch::STOP:
      stopAP();
      break;
    default:
      break;
    }

    if(ssid.length() > 0)
    {
      if(!connected && state != lastState)
        printStaStatus(state);

      switch(staRetry.update(connected, millis()))
      {
      case StaRetry::BEGIN:
        Serial.print(F("Connecting to "));
        Serial.println(ssid.c_str());
        WiFi.disconnect();  // abort a pending attempt; keeps the mode
        WiFi.begin(ssid.c_str(), password.c_str());
        break;
      case StaRetry::STOP:
        Serial.print(F("WiFi connection failed, retrying in "));
        Serial.print(StaRetry::RETRY_PAUSE_MS / 1000);
        Serial.println(F("s"));
        WiFi.disconnect();
        break;
      default:
        break;
      }
    }
    lastState = state;

    if(connected && !wasConnected) // Report only once
    {
      Serial.println(F("Connected"));
      Serial.print(F("IP address: "));
      Serial.println(WiFi.localIP());
      Serial.println(WiFi.getHostname());
      needUpdateUI = true;
    }
    else if(!connected && wasConnected)
    {
      Serial.println(F("WiFi disconnected"));
      needUpdateUI = true;
    }
    wasConnected = connected;

    // DNS requests are rare; poll a few times within the 250 ms cycle
    for(int i = 0; i < 5; i++)
    {
      dnsServer.processNextRequest();
      vTaskDelay(50);
    }
  }
}


//==============================================================================
/*! Arduino Setup
 *
 */
void setup()
{
  Serial.begin(115200);
  Serial.println(F("AURORA Debug Log"));
  Serial.println( VERSION_STR );
  #if DEBUG_PRINT
  Serial.print("Heap: ");
  Serial.println(xPortGetFreeHeapSize());
  #endif

  //----------------------------------------------------------------------------
  //--- Init Rotary Encoder Handling
  //----------------------------------------------------------------------------
  #if HAVE_ROTARYENCODER
  rotaryEncoder.init();
  lastREsw = rotaryEncoder.getSwitchValue();
  lastREval = rotaryEncoder.getRotationValue();
  #endif

  //----------------------------------------------------------------------------
  //--- Is user pressing the rotary encoder switch during boot?
  //----------------------------------------------------------------------------
  //rotaryEncoder.init();
  //pinMode( ROTARYENCODER_PINSW, INPUT_PULLUP );
  if( !digitalRead( ROTARYENCODER_PINSW ) )
    changeWifiState = true;
  
  //----------------------------------------------------------------------------
  //--- Configure I2C
  //----------------------------------------------------------------------------
  i2cInit();
  Wire.begin( I2C_SDA_PIN, I2C_SCL_PIN );
  Wire.setClock( 400000 );
  delay( 100 );

  //----------------------------------------------------------------------------
  //--- Scan for I2C display connected?
  //----------------------------------------------------------------------------
  #if HAVE_DISPLAY
  haveDisplay = false;
  i2cLock();
  Wire.beginTransmission(SSD1309_I2C_ADDR);
  if(Wire.endTransmission(true) == 0)
  {
    Serial.println(F("Detected SSD1309 display"));
    SSD1309.setI2CAddress(SSD1309_I2C_ADDR<<1);
    myDisplay.begin(&SSD1309);
    haveDisplay = true;
  }
  if(!haveDisplay)
  {
    Wire.beginTransmission(SH1106_I2C_ADDR);
    if(Wire.endTransmission(true) == 0)
    {
      Serial.println(F("Detected SH1106 display"));
      myDisplay.begin(&SH1106);
      haveDisplay = true;
    }
  }
  i2cUnlock();

  //----------------------------------------------------------------------------
  //--- Init Display (if present)
  //----------------------------------------------------------------------------
  if(haveDisplay)
    myDisplay.drawBootScreen();
  #endif

  // wait until everything is stable
  // might be a bit to defensive
  delay(2000);

  //----------------------------------------------------------------------------
  //--- Configure DAC
  //----------------------------------------------------------------------------
  Serial.println(F("Config DAC"));
  configDAC();

  //----------------------------------------------------------------------------
  //--- Configure ADC
  //----------------------------------------------------------------------------
  Serial.println(F("Config ADC"));
  configADC();

  //----------------------------------------------------------------------------
  //--- Start SPIFFS
  //----------------------------------------------------------------------------
  if(!SPIFFS.begin(true))
  {
    Serial.println(F("An Error has occurred while mounting SPIFFS"));
    return;
  }

  //--- check for old stuff (version == 1.x.x)
  String fileName = "/dspfw.hex";
  if(SPIFFS.exists(fileName))
  {
    if( SPIFFS.remove(fileName))
      Serial.println(fileName + " deleted.");
    else
      Serial.println("[ERROR] Deleting " + fileName );
  }
  fileName = "/dspparam.001";
  if(SPIFFS.exists(fileName))
  {
    if(SPIFFS.remove(fileName))
      Serial.println(fileName + " deleted.");
    else
      Serial.println("[ERROR] Deleting " + fileName);
  }
  fileName = "/dspparam.002";
  if(SPIFFS.exists(fileName))
  {
    if(SPIFFS.remove(fileName))
      Serial.println(fileName + F(" deleted."));
    else
      Serial.println("[ERROR] Deleting " + fileName);
  }
  fileName = "/dspparam.003";
  if(SPIFFS.exists(fileName))
  {
    if(SPIFFS.remove(fileName))
      Serial.println(fileName + " deleted.");
    else
      Serial.println("[ERROR] Deleting " + fileName);
  }
  fileName = "/dspparam.004";
  if(SPIFFS.exists(fileName))
  {
    if(SPIFFS.remove(fileName))
      Serial.println(fileName + F(" deleted."));
    else
      Serial.println("[ERROR] Deleting " + fileName);
  }

  Serial.print(F("Free disk space: "));
  Serial.print( (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024 );
  Serial.println(F("KiB"));

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while(file)
  {
    Serial.print(file.name());
    Serial.print(F(" "));
    Serial.print(file.size());
    Serial.println(F(" Bytes"));
    file = root.openNextFile();
  }

  //----------------------------------------------------------------------------
  //--- Read settings file
  //----------------------------------------------------------------------------
  readSettings();
  changeChannelSummationADC();
  if( changeWifiState )
  {
    Serial.print(F("Changing WiFi status......"));
    Settings.wifiOn = Settings.wifiOn ? false : true;
    writeSettings();
    Serial.println(F("[OK]"));
  }

  Serial.print(F("Init user parameter......"));
  initUserParams();
  Serial.println(F("[OK]"));
  readPluginMeta();
  matrixInit();

  //----------------------------------------------------------------------------
  //--- Read virtual input channel routing (only if supported by plugin)
  //----------------------------------------------------------------------------
  if(currentPlugInName == String(F("stereoforever")) || currentPlugInName == String(F("The Room")))
  {
    if(!SPIFFS.exists(F("/vinputs.txt")))
      // if file does not exist, write it with defautl values
      writeVirtualInputRouting();
    else
      readVirtualInputRouting();
  }

  readChannelNames();

  //----------------------------------------------------------------------------
  //--- Configure AddOn
  //----------------------------------------------------------------------------
  updateAddOn();

  //----------------------------------------------------------------------------
  //--- Upload program to DSP
  //----------------------------------------------------------------------------
  uploadDspFirmware();
  // the DAC is still in reset (muted) until resetDAC(false) below
  matrixWriteBaseline();

  //----------------------------------------------------------------------------
  //--- Upload user parameters to DSP
  //----------------------------------------------------------------------------
  if( isMatrixPlugin() )
  {
    // The legacy upload would rewrite the Bus Sources and the SPDIF out behind the
    // model's back; only the (global) master volume is needed from it.
    setMasterVolume();
    matrixLoadPreset();
  }
  else
    uploadUserParams();

  //----------------------------------------------------------------------------
  //--- Configure Webserver
  //----------------------------------------------------------------------------
  setupWebserver();

  //----------------------------------------------------------------------------
  //--- Configure ESP for WiFi access
  //----------------------------------------------------------------------------
  // Create a connection task with 8kB stack on core 0
  if( Settings.wifiOn )
    xTaskCreatePinnedToCore(myWiFiTask, "myWiFiTask", 8192, NULL, 3, NULL, 0);

  //----------------------------------------------------------------------------
  //--- Enable Volume Potentiometer
  //----------------------------------------------------------------------------
  enableVolPot();

  //----------------------------------------------------------------------------
  //--- Init IR Receiver
  //----------------------------------------------------------------------------
  #if HAVE_IRRECEIVER
  irReceiver.enableIRIn();
  #endif

  if(currentPlugInName == String(F("stereoforever")) || currentPlugInName == String(F("The Room")))
    setVirtualInput();

  resetDAC(false);

  updateUI();

  lastREsw = rotaryEncoder.getSwitchValue();
  lastREval = rotaryEncoder.getRotationValue();

  #if DEBUG_PRINT
  Serial.print("Heap: ");
  Serial.println(xPortGetFreeHeapSize());
  #endif

  Serial.println(F("Ready"));
}

//==============================================================================
/*! Helpers
 *
 */

/*! The one Preset switch of web (POST /preset) and rotary encoder. Runs on the
 *  I2C worker, inside matrixBeginPresetSwitch()/matrixEndPresetSwitch().
 *  Matrix Plugin: the difference inside one shared gain ramp, no DAC mute and no
 *  legacy parameter upload (ADR 0003). Other Plugins: full upload under the DAC mute.
 *  redraw: update the display here (web); the encoder redraws later from loop(). Other
 *  Plugins redraw before the DAC unmute, as they always did. */
void applyPresetSwitch( uint8_t preset, bool redraw ) {
  currentPreset = preset;
  if( isMatrixPlugin() )
  {
    matrixLoadPreset();  // includes the Add-on B config, written only if it differs
    if( redraw )
      updateUI();
    return;
  }
  softMuteDAC();
  initUserParams();
  uploadUserParams();
  updateAddOn();
  if( redraw )
    updateUI();
  softUnmuteDAC();
}

void next_preset(void) {
  applyPresetSwitch( currentPreset + 1 >= MAX_NUM_PRESETS ? 0 : currentPreset + 1, false );
  matrixEndPresetSwitch();
}

void prev_preset(void) {
  applyPresetSwitch( currentPreset == 0 ? MAX_NUM_PRESETS - 1 : currentPreset - 1, false );
  matrixEndPresetSwitch();
}

void queue_next_preset(void) {
  if( !matrixBeginPresetSwitch() )
    return;
  if( !i2cPost( []() { next_preset(); needUpdateUI = true; } ) )
    matrixEndPresetSwitch();
}

void queue_prev_preset(void) {
  if( !matrixBeginPresetSwitch() )
    return;
  if( !i2cPost( []() { prev_preset(); needUpdateUI = true; } ) )
    matrixEndPresetSwitch();
}

void increase_volume(void) {
  masterVolume.val += 0.5f;
  if( masterVolume.val > 0.f )
    masterVolume.val = 0.f;
  setMasterVolume();
  matrixMasterVolumeChanged();
}

void decrease_volume(void) {
  masterVolume.val -= 0.5f;
  if( masterVolume.val <= -80.f )
    masterVolume.val = -80.f;
  setMasterVolume();
  matrixMasterVolumeChanged();
}

//==============================================================================
/*! Arduino Main Loop
 *
 */
void loop()
{
  TIMERG0.wdt_wprotect=TIMG_WDT_WKEY_VALUE;
  TIMERG0.wdt_feed=1;
  TIMERG0.wdt_wprotect=0;

  // Worker jobs and web handlers set needUpdateUI. Taken before the input below,
  // so a "switching preset" requested in this pass is not overdrawn by the old UI.
  if( needUpdateUI.exchange( false ) )
    pendingRedraw.request( PendingRedraw::UI );

  matrixLoop();


  #if HAVE_ROTARYENCODER
  if( rotaryEncoder.getSwitchValue() != lastREsw )
  {
    if(changeWifiState)
      changeWifiState = false;
    else
    {
      editMode++;
      if(currentPlugInName == String(F("stereoforever")))
      {
        if(editMode > 2)
          editMode = 0;
      }
      else
      {
        // we may have more then two modes in the future.
        if(editMode > 1)
          editMode = 0;
      }
      
      delay(300);
      needUpdateUI = true;
    }
    lastREsw = rotaryEncoder.getSwitchValue();
    lastREval = rotaryEncoder.getRotationValue();
  }
  else if( rotaryEncoder.getRotationValue() > lastREval + 1 )
  {
    if(editMode == 0)
    {
      i2cPost( []() { increase_volume(); needUpdateUI = true; } );
      lastREval = rotaryEncoder.getRotationValue();
    }
    else if( editMode == 1 )
    {
      pendingRedraw.request( PendingRedraw::SWITCHING_PRESET );

      queue_next_preset();

      lastREval = rotaryEncoder.getRotationValue();
    }
    else if(editMode == 2)
    {
      if(currentPlugInName == String(F("stereoforever")))
      {
        i2cPost( []() {
          incrementVirtualInput();
          softMuteDAC();
          updateUI();
          softUnmuteDAC();
          needUpdateUI = true;
        } );
        lastREval = rotaryEncoder.getRotationValue();
      }
    }
  }
  else if( rotaryEncoder.getRotationValue() < lastREval - 1 )
  {
    if( editMode == 0 )
    {
      i2cPost( []() { decrease_volume(); needUpdateUI = true; } );
      lastREval = rotaryEncoder.getRotationValue();
    }
    else if( editMode == 1 )
    {
      pendingRedraw.request( PendingRedraw::SWITCHING_PRESET );

      queue_prev_preset();

      lastREval = rotaryEncoder.getRotationValue();
    }
    else if(editMode == 2)
    {
      if(currentPlugInName == String(F("stereoforever")))
      {
        i2cPost( []() {
          decrementVirtualInput();
          softMuteDAC();
          updateUI();
          softUnmuteDAC();
          needUpdateUI = true;
        } );
        lastREval = rotaryEncoder.getRotationValue();
      }
    }
  }
  #endif

  #if HAVE_IRRECEIVER
  decode_results irResults;
  if( irReceiver.decode( &irResults ) )
  {
    Serial.println(irResults.value, HEX);
    uint32_t irval = (irResults.value & 0x0000ff00) >> 8;
    if((irval == APPLE_A_REMOTE_UP) || (irval == APPLE_B_REMOTE_UP))
    {
      i2cPost( []() { increase_volume(); needUpdateUI = true; } );
    }
    else if((irval == APPLE_A_REMOTE_DOWN) || (irval == APPLE_B_REMOTE_DOWN))
    {
      i2cPost( []() { decrease_volume(); needUpdateUI = true; } );
    }
    if(currentPlugInName == String(F("stereoforever")) || currentPlugInName == String(F("The Room")))
    {
      if((irval == APPLE_A_REMOTE_LEFT) || (irval == APPLE_B_REMOTE_LEFT))
      {
        i2cPost( []() { decrementVirtualInput(); setVirtualInput(); needUpdateUI = true; } );
      }
      else if((irval == APPLE_A_REMOTE_RIGHT) || (irval == APPLE_B_REMOTE_RIGHT))
      {
        i2cPost( []() { incrementVirtualInput(); setVirtualInput(); needUpdateUI = true; } );
      }
      else if((irval == APPLE_A_REMOTE_MENU) || (irval == APPLE_B_REMOTE_MENU))
      {
        pendingRedraw.request( PendingRedraw::SWITCHING_PRESET );
        queue_next_preset();
      }
    }
    else
    {
      if((irval == APPLE_A_REMOTE_LEFT) || (irval == APPLE_B_REMOTE_LEFT))
      {
        pendingRedraw.request( PendingRedraw::SWITCHING_PRESET );
        queue_prev_preset();
      }
      else if((irval == APPLE_A_REMOTE_RIGHT) || (irval == APPLE_B_REMOTE_RIGHT))
      {
        pendingRedraw.request( PendingRedraw::SWITCHING_PRESET );
        queue_next_preset();
      }
    }
    //else
    //  Serial.println(irResults.value, HEX);
    irReceiver.resume();
  }
  #endif

  // Drawn only once the bus is free, so the input above keeps being read
  // while the worker runs a Preset load or FIR upload.
  pendingRedraw.serve( tryDrawScreen );

}
