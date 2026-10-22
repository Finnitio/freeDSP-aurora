#ifndef DISPLAY_H_
#define DISPLAY_H_

#include "OLED128x64_SH1106.h"
#include "OLED128x64_SSD1309.h"
#include "i2cworker.h"

extern bool haveDisplay;
extern bool needUpdateUI;
extern int editMode;

extern OLED128x64_SH1106 SH1106;
extern OLED128x64_SSD1309 SSD1309;

// The display shares the I2C bus with the DSP: every call holds the bus mutex.
class Display
{
public:
  Display(void) {}

  void begin(DisplayDriver* drv)
  {
    I2cLock lock;
    pDisplayDriver = drv;
    if(pDisplayDriver)
      pDisplayDriver->begin();
  }

  void drawBootScreen(void)
  {
    I2cLock lock;
    if(pDisplayDriver)
      pDisplayDriver->drawBootScreen();
  }

  void drawUI(const char* plugin, const char* ip, const int currentPreset, float vol, int editMode = 0)
  {
    I2cLock lock;
    if(pDisplayDriver)
      pDisplayDriver->drawUI(plugin, ip, currentPreset, vol, editMode);
  }

  void drawSwitchingPreset(void)
  {
    I2cLock lock;
    if(pDisplayDriver)
      pDisplayDriver->drawSwitchingPreset();
  }

private:
  DisplayDriver* pDisplayDriver = nullptr;
};

#endif
