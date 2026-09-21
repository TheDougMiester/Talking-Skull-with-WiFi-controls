/*
 * File: GC9A01_Eyes.h
 * Purpose: Implements dual-bus GC9A01 demon-eye rendering, autonomous animation, manual control, and DMA workers.
 *
 * MIT License
 *
 * Copyright (c) 2026 Doug Brann https://github.com/TheDougMiester
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <new>
#include "freertos/event_groups.h"
#include "PinDefinitions.h"
#include "AudioFFTProcessor_ESP32_Optimized.h"
#include "WiFiSkullController.h"
#include "uncanny/DemonEye184.h"

// Set either option to 1 when that physical GC9A01 is installed upside down.
// A value of 0 preserves the original right-side-up mounting orientation.
#define LEFT_GC9A01_INVERTED  1
#define RIGHT_GC9A01_INVERTED 1

#if (LEFT_GC9A01_INVERTED != 0 && LEFT_GC9A01_INVERTED != 1)
#error "LEFT_GC9A01_INVERTED must be 0 or 1"
#endif
#if (RIGHT_GC9A01_INVERTED != 0 && RIGHT_GC9A01_INVERTED != 1)
#error "RIGHT_GC9A01_INVERTED must be 0 or 1"
#endif

/*
 * GC9A01 Eyes DUAL BUS ZERO-LAG
 * - Left eye uses SPI2_HOST on GPIO15/16/17/18/8
 * - Right eye uses SPI3_HOST on GPIO10/11/12/13/14
 * - Two independent buses and two persistent workers preserve parallel pushes
 * - One PSRAM sprite per eye; neither is redrawn until its prior push completes
 */

inline uint16_t eye_rgb(uint8_t r,uint8_t g,uint8_t b){ return ((r>>3)<<11)|((g>>2)<<5)|(b>>3); }

class GC9A01_Eyes {
private:
  lgfx::Bus_SPI _busL;
  lgfx::Bus_SPI _busR;

  class LGFX_Eye : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel;
  public:
    LGFX_Eye(lgfx::Bus_SPI* bus, int cs, int rst){
      _panel.setBus(bus);
      auto pcfg = _panel.config();
      pcfg.pin_cs = cs; pcfg.pin_rst = rst; pcfg.pin_busy = -1;
      pcfg.memory_width=240; pcfg.memory_height=240;
      pcfg.panel_width=240; pcfg.panel_height=240;
      pcfg.offset_x=0; pcfg.offset_y=0;
      pcfg.dummy_read_pixel=8; pcfg.dummy_read_bits=1;
      pcfg.readable=false; pcfg.invert=true; pcfg.rgb_order=false;
      pcfg.bus_shared=false; // each eye has its own bus, not shared
      _panel.config(pcfg); setPanel(&_panel);
    }
  };

  LGFX_Eye* eyeL = nullptr;
  LGFX_Eye* eyeR = nullptr;
  lgfx::LGFX_Sprite* sprL = nullptr;
  lgfx::LGFX_Sprite* sprR = nullptr;

  static constexpr EventBits_t LEFT_PUSH_DONE = BIT0;
  static constexpr EventBits_t RIGHT_PUSH_DONE = BIT1;
  static constexpr EventBits_t BOTH_PUSH_DONE = LEFT_PUSH_DONE | RIGHT_PUSH_DONE;
  EventGroupHandle_t pushEvents = nullptr;
  TaskHandle_t leftWorker = nullptr;
  TaskHandle_t rightWorker = nullptr;
  volatile bool shuttingDown = false;
  bool ready = false;
  bool frameInFlight = false;
  bool frameDelayWarned = false;
  uint32_t frameStartedMs = 0;
  uint32_t skippedBusyFrames = 0;
  uint32_t workerCreateFailures = 0;
  volatile UBaseType_t leftStackMinWords = 0;
  volatile UBaseType_t rightStackMinWords = 0;

  // 184x184 offline-resampled, palette-neutral demon iris centered on each
  // GC9A01. The web page selects green, blue, red, or hazel; the vertical
  // pupil is always black. Set false for immediate fallback to the old renderer.
  static constexpr bool UNCANNY_GOAT_ANIMATED = true;
  static constexpr int GOAT_RENDER_SIZE = DEMON_EYE_WIDTH;
  static constexpr int GOAT_OFFSET = (240 - GOAT_RENDER_SIZE) / 2; // 28px rim
  enum EyeColorMode : uint8_t {
    EYE_GREEN=0, EYE_BLUE=1, EYE_RED=2, EYE_HAZEL=3
  };
  EyeColorMode eyeColorMode = EYE_GREEN;
  bool manualModeLast = false;
  int manualCanvasX = 0;
  int manualCanvasY = 0;

  struct GoatGlowState {
    uint16_t currentQ8 = 256;  // 1.0x
    uint16_t targetQ8 = 256;
    uint32_t nextEventMs = 0;
    uint32_t eventUntilMs = 0;
  };
  GoatGlowState glowL;
  GoatGlowState glowR;
  uint32_t nextGoatSaccadeL = 0;
  uint32_t nextGoatSaccadeR = 0;
  int goatTargetXL = 0, goatTargetYL = 0;
  int goatTargetXR = 0, goatTargetYR = 0;

  float squintAmount = 0.0f;   // upper-lid threshold, 0=open
  float squintTarget = 0.0f;
  uint32_t nextSquintMs = 0;
  uint32_t squintUntilMs = 0;

  static bool timeReached(uint32_t now, uint32_t deadline){
    return static_cast<int32_t>(now - deadline) >= 0;
  }

  static int approachInt(int current, int target){
    const int delta = target - current;
    if(delta == 0) return current;
    int step = abs(delta) / 3;
    if(step < 1) step = 1;
    return current + (delta > 0 ? step : -step);
  }

  void stepGoatGlowTowardTarget(GoatGlowState& glow){
    // Fast rise, slower decay. Integer minimum steps prevent stalling one
    // count away from the destination.
    if(glow.currentQ8 < glow.targetQ8){
      const uint16_t delta = glow.targetQ8 - glow.currentQ8;
      const uint16_t step = delta / 3U ? delta / 3U : 1U;
      glow.currentQ8 += step;
      if(glow.currentQ8 > glow.targetQ8) glow.currentQ8 = glow.targetQ8;
    } else if(glow.currentQ8 > glow.targetQ8){
      const uint16_t delta = glow.currentQ8 - glow.targetQ8;
      const uint16_t step = delta / 7U ? delta / 7U : 1U;
      glow.currentQ8 -= step;
      if(glow.currentQ8 < glow.targetQ8) glow.currentQ8 = glow.targetQ8;
    }
  }

  void updateGoatGlow(GoatGlowState& glow, uint32_t now){
    if(glow.nextEventMs == 0){
      glow.nextEventMs = now + static_cast<uint32_t>(random(4500, 11000));
    }
    if(glow.eventUntilMs != 0 && timeReached(now, glow.eventUntilMs)){
      glow.eventUntilMs = 0;
      glow.targetQ8 = 256;
      glow.nextEventMs = now + static_cast<uint32_t>(random(5000, 13000));
    } else if(glow.eventUntilMs == 0 && timeReached(now, glow.nextEventMs)){
      glow.targetQ8 = static_cast<uint16_t>(random(330, 430)); // 1.29x..1.68x
      glow.eventUntilMs = now + static_cast<uint32_t>(random(500, 1400));
    }

    stepGoatGlowTowardTarget(glow);
  }

  void updateGoatSquint(uint32_t now){
    if(nextSquintMs == 0){
      nextSquintMs = now + static_cast<uint32_t>(random(7000, 16000));
    }
    if(squintUntilMs != 0 && timeReached(now, squintUntilMs)){
      squintUntilMs = 0;
      squintTarget = 0.0f;
      nextSquintMs = now + static_cast<uint32_t>(random(8000, 18000));
    } else if(squintUntilMs == 0 && timeReached(now, nextSquintMs)){
      squintTarget = static_cast<float>(random(95, 171));
      squintUntilMs = now + static_cast<uint32_t>(random(700, 1900));
    }
    const float alpha = squintTarget > squintAmount ? 0.20f : 0.12f;
    squintAmount += alpha * (squintTarget - squintAmount);
    if(fabsf(squintTarget - squintAmount) < 0.5f) squintAmount = squintTarget;
  }

  // Apply the selected hue to one palette-neutral source intensity. A zero
  // source pixel stays exactly black in all modes, preserving the slit pupil.
  static uint16_t demonTextureColor(uint8_t sourceIntensity,
                                    uint16_t brightnessQ8,
                                    EyeColorMode mode){
    if(sourceIntensity == 0) return TFT_BLACK;
    uint32_t v = (static_cast<uint32_t>(sourceIntensity) * brightnessQ8 + 128U) >> 8;
    if(v > 255U) v = 255U;
    uint8_t r=0, g=0, b=0;
    if(mode == EYE_BLUE){
      r=static_cast<uint8_t>((v*10U)/100U);
      g=static_cast<uint8_t>((v*42U)/100U);
      b=static_cast<uint8_t>(v);
    } else if(mode == EYE_RED){
      r=static_cast<uint8_t>(v);
      g=static_cast<uint8_t>((v*4U)/100U);
      b=static_cast<uint8_t>((v*2U)/100U);
    } else if(mode == EYE_HAZEL){
      // Deep olive-brown fibers rise into an unsettling gold/green highlight.
      // Increasing the green share with intensity keeps shadows earthy while
      // the independent flare animation gives bright fibers a predatory glow.
      const uint32_t greenPercent = 45U + (v * 31U) / 255U; // 45%..76%
      r=static_cast<uint8_t>((v*90U)/100U);
      g=static_cast<uint8_t>((v*greenPercent)/100U);
      b=static_cast<uint8_t>((v*7U)/100U);
    } else {
      r=static_cast<uint8_t>((v*8U)/100U);
      g=static_cast<uint8_t>(v);
      b=static_cast<uint8_t>((v*28U)/100U);
    }
    return eye_rgb(r,g,b);
  }

  uint32_t lastSaccade=0, lastBlink=0, blinkStart=0, lastMicro=0, lastWide=0, wideUntil=0, lastDilate=0, dilateUntil=0;
  bool isBlinking=false, isWide=false, isDilated=false;
  int lookX=0, lookY=0, lookX2=0, lookY2=0;
  int pupilSize=40, targetPupil=40;

  void drawGoatFrame(lgfx::LGFX_Sprite& s, bool rightEye,
                     uint8_t upperThreshold, uint8_t lowerThreshold,
                     int lookOffsetX, int lookOffsetY,
                     int canvasOffsetX, int canvasOffsetY,
                     uint16_t brightnessQ8){
    static_assert(DEMON_EYE_WIDTH == 184 && DEMON_EYE_HEIGHT == 184,
                  "Demon eye asset must be 184x184");
    static_assert(DEMON_LID_WIDTH == 128 && DEMON_LID_HEIGHT == 128,
                  "Demon eyelid masks must be 128x128");

    s.fillScreen(TFT_BLACK);
    const int destinationX = GOAT_OFFSET + canvasOffsetX + lookOffsetX;
    const int destinationY = GOAT_OFFSET + canvasOffsetY + lookOffsetY;
    for(int y=0; y<GOAT_RENDER_SIZE; ++y){
      // Scale the original 128px threshold masks over the offline-resampled
      // 208px iris. Texture scaling itself is not done on the ESP32.
      const int lidY = (y * DEMON_LID_HEIGHT) / GOAT_RENDER_SIZE;
      for(int x=0; x<GOAT_RENDER_SIZE; ++x){
        const int sourceLidX = (x * DEMON_LID_WIDTH) / GOAT_RENDER_SIZE;
        const int lidX = rightEye
            ? sourceLidX : (DEMON_LID_WIDTH - 1 - sourceLidX);
        uint16_t color = TFT_BLACK;
        if(pgm_read_byte(demonLower128 + lidY * DEMON_LID_WIDTH + lidX)
               > lowerThreshold &&
           pgm_read_byte(demonUpper128 + lidY * DEMON_LID_WIDTH + lidX)
               > upperThreshold){
          const uint8_t intensity =
              pgm_read_byte(demonEyeIntensity184 + y * DEMON_EYE_WIDTH + x);
          color = demonTextureColor(intensity, brightnessQ8, eyeColorMode);
        }
        s.drawPixel(destinationX + x, destinationY + y, color);
      }
      if((y & 7) == 7) yield();
    }
  }

  void drawVein(lgfx::LGFX_Sprite &s,int cx,int cy,int innerR,int outerR,float baseAng,float chaos,uint32_t now){
    float wobble = sin(now/700.0 + baseAng*2)*0.2;
    float a = baseAng + wobble;
    int x0=cx+cos(a)*innerR, y0=cy+sin(a)*innerR;
    for(int seg=1; seg<=2; ++seg){
      float t=(float)seg/2; float na=a+sin(t*3.14159f)*chaos;
      int x1=cx+cos(na)*(innerR + (outerR-innerR)*t);
      int y1=cy+sin(na)*(innerR + (outerR-innerR)*t);
      s.drawLine(x0,y0,x1,y1,TFT_RED); x0=x1; y0=y1;
    }
  }

  void drawEyeInternal(lgfx::LGFX_Sprite &s,int pupilSz,int lx,int ly,int lid,bool suppressDefaultLid){
    s.fillScreen(TFT_BLACK);
    uint32_t now=millis();
    s.fillCircle(120,120,110,TFT_WHITE);
    for(int i=0;i<6;i++){ float baseAng=(float)i/6*2*3.14159f; drawVein(s,120,120,pupilSz+28,108,baseAng,0.2f,now); }
    for(int i=0;i<3;i++){ float ang=now/1200.0f+i*1.8f; float dist=60+20*sin(now/900.0f+i*2); int bx=120+cos(ang)*dist; int by=120+sin(ang*0.9f)*dist; float d=sqrt((bx-120)*(bx-120)+(by-120)*(by-120)); if(d>pupilSz+30 && d<102) s.fillCircle(bx,by,3,TFT_RED); }
    int cx=120+lx, cy=120+ly;
    int irisR = (int)((pupilSz+22)*1.3f);
    if(irisR>100) irisR=100;
    s.fillCircle(cx,cy,irisR+3,TFT_BLACK);
    s.fillCircle(cx,cy,irisR,TFT_RED);
    for(int a=0;a<360;a+=20){ float rad=radians((float)a); s.drawLine(cx+cos(rad)*(pupilSz+2), cy+sin(rad)*(pupilSz+2), cx+cos(rad)*(irisR-2), cy+sin(rad)*(irisR-2), eye_rgb(180,0,0)); }
    s.fillCircle(cx,cy,pupilSz,TFT_BLACK);
    s.fillCircle(cx-8,cy-8,4,TFT_WHITE);
    int base=suppressDefaultLid ? 0 : 72; if(!suppressDefaultLid && isWide) base=4;
    int total=lid+base; if(total>0){ int h=total/2; if(h>120) h=120; s.fillRect(0,0,240,h,TFT_BLACK); s.fillRect(0,240-h,240,h,TFT_BLACK); }
  }

  static void pushLeftWorker(void* arg){
    auto* self = static_cast<GC9A01_Eyes*>(arg);
    for(;;){
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      if(self->shuttingDown) break;
      self->sprL->pushSprite(self->eyeL,0,0);
      const UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
      if(self->leftStackMinWords == 0 || watermark < self->leftStackMinWords) self->leftStackMinWords = watermark;
      xEventGroupSetBits(self->pushEvents, LEFT_PUSH_DONE);
    }
    xEventGroupSetBits(self->pushEvents, LEFT_PUSH_DONE);
    vTaskDelete(nullptr);
  }

  static void pushRightWorker(void* arg){
    auto* self = static_cast<GC9A01_Eyes*>(arg);
    for(;;){
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      if(self->shuttingDown) break;
      self->sprR->pushSprite(self->eyeR,0,0);
      const UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
      if(self->rightStackMinWords == 0 || watermark < self->rightStackMinWords) self->rightStackMinWords = watermark;
      xEventGroupSetBits(self->pushEvents, RIGHT_PUSH_DONE);
    }
    xEventGroupSetBits(self->pushEvents, RIGHT_PUSH_DONE);
    vTaskDelete(nullptr);
  }

public:
  GC9A01_Eyes(){}
  ~GC9A01_Eyes(){}

  bool begin(){
    // Left bus SPI2 left side cluster
    auto bcfgL = _busL.config();
    bcfgL.spi_host=SPI2_HOST; bcfgL.spi_mode=0; bcfgL.freq_write=40000000; bcfgL.freq_read=16000000;
    bcfgL.spi_3wire=false; bcfgL.use_lock=true; bcfgL.dma_channel=SPI_DMA_CH_AUTO;
    bcfgL.pin_sclk=GC9A01_SCL_LEFT_PIN; bcfgL.pin_mosi=GC9A01_SDA_LEFT_PIN; bcfgL.pin_miso=-1; bcfgL.pin_dc=GC9A01_DC_LEFT_PIN;
    _busL.config(bcfgL); _busL.init();

    // Right bus SPI3 right side cluster - no crossing
    auto bcfgR = _busR.config();
    bcfgR.spi_host=SPI3_HOST; bcfgR.spi_mode=0; bcfgR.freq_write=40000000; bcfgR.freq_read=16000000;
    bcfgR.spi_3wire=false; bcfgR.use_lock=true; bcfgR.dma_channel=SPI_DMA_CH_AUTO;
    bcfgR.pin_sclk=GC9A01_SCL_RIGHT_PIN; bcfgR.pin_mosi=GC9A01_SDA_RIGHT_PIN; bcfgR.pin_miso=-1; bcfgR.pin_dc=GC9A01_DC_RIGHT_PIN;
    _busR.config(bcfgR); _busR.init();

    eyeL = new(std::nothrow) LGFX_Eye(&_busL, GC9A01_CS_LEFT_PIN, GC9A01_RES_LEFT_PIN);
    eyeR = new(std::nothrow) LGFX_Eye(&_busR, GC9A01_CS_RIGHT_PIN, GC9A01_RES_RIGHT_PIN);
    sprL = new(std::nothrow) lgfx::LGFX_Sprite();
    sprR = new(std::nothrow) lgfx::LGFX_Sprite();
    pushEvents = xEventGroupCreate();
    if(!eyeL || !eyeR || !sprL || !sprR || !pushEvents){
      Serial.println("[Eyes] ERROR: object/event allocation failed");
      return false;
    }

    eyeL->init(); eyeL->setRotation(LEFT_GC9A01_INVERTED ? 2 : 0);
    eyeR->init(); eyeR->setRotation(RIGHT_GC9A01_INVERTED ? 2 : 0);
    sprL->setPsram(true);
    sprR->setPsram(true);
    if(!sprL->createSprite(240,240) || !sprR->createSprite(240,240)){
      Serial.println("[Eyes] ERROR: PSRAM sprite allocation failed");
      return false;
    }

    BaseType_t leftOK = xTaskCreatePinnedToCore(pushLeftWorker, "eyePushL", 4096, this, 3, &leftWorker, 0);
    BaseType_t rightOK = xTaskCreatePinnedToCore(pushRightWorker, "eyePushR", 4096, this, 3, &rightWorker, 1);
    if(leftOK != pdPASS || rightOK != pdPASS){
      workerCreateFailures++;
      shuttingDown = true;
      if(leftWorker) xTaskNotifyGive(leftWorker);
      if(rightWorker) xTaskNotifyGive(rightWorker);
      Serial.printf("[Eyes] ERROR: persistent worker creation L=%d R=%d\n", leftOK == pdPASS, rightOK == pdPASS);
      return false;
    }
    ready = true;
    eyeL->fillScreen(TFT_BLACK); eyeR->fillScreen(TFT_BLACK);
    randomSeed(esp_random());
    return true;
  }

  bool isReady() const { return ready; }
  UBaseType_t getLeftStackMinWords() const { return leftStackMinWords; }
  UBaseType_t getRightStackMinWords() const { return rightStackMinWords; }
  uint32_t getSkippedBusyFrames() const { return skippedBusyFrames; }
  uint32_t getWorkerCreateFailures() const { return workerCreateFailures; }

  void update(){
    if(!ready) return;
    uint32_t now = millis();

    // Never redraw a sprite while either persistent worker may still be
    // reading it. A busy transfer skips this animation tick without blocking
    // Wi-Fi, audio control, or jaw scheduling.
    if(frameInFlight){
      const EventBits_t bits = xEventGroupGetBits(pushEvents);
      if((bits & BOTH_PUSH_DONE) != BOTH_PUSH_DONE){
        skippedBusyFrames++;
        if(!frameDelayWarned && now - frameStartedMs > 150){
          Serial.printf("[Eyes] WARNING: frame push still active after %u ms bits=0x%02x\n",
                        static_cast<unsigned>(now - frameStartedMs), static_cast<unsigned>(bits));
          frameDelayWarned = true;
        }
        return;
      }
      frameInFlight = false;
      frameDelayWarned = false;
    }

    // Manual override from /eyes page
    int manX=0, manY=0, manLid=-1, manPupil=-1; bool manActive=false;
    bool hasManual = WiFiSkullController::getEyeManualFull(manX, manY, manLid, manPupil, manActive);
    bool manualLook = false; int manualLidOverride=-1;

    // Copy one coherent cross-core FFT snapshot for this entire eye frame.
    AudioMetricsFast audio = AudioFFTProcessorFast::getMetrics();
    float vowel = audio.vowelEnergy;
    float sibilant = audio.sibilantEnergy;
    float jaw = audio.jawFactor;

    if(hasManual && manActive){
      manualLook=true;
      lookX=manX; lookY=manY; lookX2=manX; lookY2=manY;
      // Keep autonomous targets in their own smaller coordinate space so
      // leaving manual mode cannot drag a full-screen manual coordinate into
      // the automatic motion state.
      goatTargetXL=constrain(manX,-28,28); goatTargetYL=constrain(manY,-20,20);
      goatTargetXR=goatTargetXL; goatTargetYR=goatTargetYL;
      if(manLid>=0) manualLidOverride=manLid;
      if(manPupil>=15){
        targetPupil=manPupil; isDilated=false; // retained for procedural fallback
        eyeColorMode = manPupil < 32 ? EYE_GREEN :
                       (manPupil < 64 ? EYE_BLUE :
                        (manPupil < 96 ? EYE_RED : EYE_HAZEL));
      }
    } else {
      int baseTarget = constrain(map((int)vowel, 0, 2000, 28, 62), 28, 70);
      if(jaw>0.6) baseTarget-=4;
      if(!isDilated && now-lastDilate>random(5000,10000)){ isDilated=true; dilateUntil=now+random(800,1500); targetPupil=random(64,76); lastDilate=now; }
      if(isDilated && now>dilateUntil){ isDilated=false; lastDilate=now; }
      if(!isDilated) targetPupil=baseTarget;
    }
    if(manualLook != manualModeLast){
      // Automatic mode always renders with a centered canvas, so begin and end
      // manual mode from that same neutral point rather than carrying a stale
      // full-screen offset across the mode transition.
      manualCanvasX = 0;
      manualCanvasY = 0;
      if(!manualLook){
        glowL.targetQ8 = glowR.targetQ8 = 256;
        glowL.eventUntilMs = glowR.eventUntilMs = 0;
        glowL.nextEventMs = glowR.nextEventMs = 0;
      }
      manualModeLast = manualLook;
    }
    pupilSize = (pupilSize*6 + targetPupil)/7;

    if(UNCANNY_GOAT_ANIMATED){
      // Color selection is independent of the existing autonomous brightness
      // flares, which continue in both manual and automatic position modes.
      updateGoatGlow(glowL, now);
      updateGoatGlow(glowR, now);
      updateGoatSquint(now);
    }

    if(!manualLook){
      if(now-lastMicro>130){
        if(UNCANNY_GOAT_ANIMATED){
          goatTargetXL=constrain(goatTargetXL+random(-3,4),-28,28);
          goatTargetYL=constrain(goatTargetYL+random(-3,4),-20,20);
          goatTargetXR=constrain(goatTargetXR+random(-3,4),-28,28);
          goatTargetYR=constrain(goatTargetYR+random(-3,4),-20,20);
        } else {
          lookX+=random(-3,4); lookY+=random(-3,4); lookX=constrain(lookX,-28,28); lookY=constrain(lookY,-20,20);
          lookX2+=random(-3,4); lookY2+=random(-3,4); lookX2=constrain(lookX2,-28,28); lookY2=constrain(lookY2,-20,20);
        }
        lastMicro=now;
      }
      if(UNCANNY_GOAT_ANIMATED){
        if(nextGoatSaccadeL == 0) nextGoatSaccadeL = now + random(700, 2300);
        if(nextGoatSaccadeR == 0) nextGoatSaccadeR = now + random(700, 2300);
        if(timeReached(now, nextGoatSaccadeL)){
          goatTargetXL = random(-26, 27); goatTargetYL = random(-18, 19);
          nextGoatSaccadeL = now + random(850, 2800);
        }
        if(timeReached(now, nextGoatSaccadeR)){
          goatTargetXR = random(-26, 27); goatTargetYR = random(-18, 19);
          nextGoatSaccadeR = now + random(850, 2800);
        }
        lookX = approachInt(lookX, goatTargetXL);
        lookY = approachInt(lookY, goatTargetYL);
        lookX2 = approachInt(lookX2, goatTargetXR);
        lookY2 = approachInt(lookY2, goatTargetYR);
      } else if(now-lastSaccade>random(900,2200)){
        if(random(0,100)<40){ lookX=random(-26,26); lookY=random(-18,18); lookX2=random(-26,26); lookY2=random(-18,18); }
        else { int nx=random(-24,24); int ny=random(-16,16); lookX=nx; lookY=ny; lookX2=nx+random(-6,7); lookY2=ny+random(-6,7); }
        lastSaccade=now;
      }
      if(!isWide && now-lastWide>random(6000,12000)){ isWide=true; wideUntil=now+random(500,1200); lastWide=now; }
      if(isWide && now>wideUntil) isWide=false;
    }

    int lid=0;
    if(manualLook && manualLidOverride>=0){ lid=manualLidOverride; }
    else {
      bool canBlink = (now-lastBlink>6000);
      bool sibilantBlink = false;
      if(canBlink && sibilant>3000 && audio.envelope>60) sibilantBlink = true;
      bool autoBlink = (now-lastBlink>random(12000,20000));
      if(!isBlinking && (sibilantBlink || autoBlink)){ isBlinking=true; blinkStart=now; lastBlink=now; }
      if(isBlinking){ uint32_t t=now-blinkStart; if(t<180) lid=map(t,0,180,0,240); else if(t<380) lid=240; else if(t<680) lid=map(t,380,680,240,0); else isBlinking=false; }
    }

    // Draw both into sprites at coherent blink/squint positions. Blink closes
    // both masks symmetrically. Squint lowers the upper lid strongly and raises
    // the lower lid slightly, producing an expression rather than a slow blink.
    if(UNCANNY_GOAT_ANIMATED){
      const uint8_t blinkThreshold = static_cast<uint8_t>(
          (static_cast<uint32_t>(constrain(lid, 0, 240)) * 255U + 120U) / 240U);
      const uint8_t squintUpper = manualLidOverride >= 0 ? 0U :
          static_cast<uint8_t>(constrain(static_cast<int>(squintAmount + 0.5f), 0, 220));
      const uint8_t squintLower = static_cast<uint8_t>(squintUpper / 3U);
      const uint8_t upperThreshold = max(blinkThreshold, squintUpper);
      const uint8_t lowerThreshold = max(blinkThreshold, squintLower);

      // Autonomous motion shifts the iris under stationary lids by a modest
      // amount. Manual-pad motion translates the visible green eye far enough
      // that its CENTER can reach every GC9A01 edge. The 128px source is
      // intentionally clipped at extremes; fillScreen(BLACK) supplies the
      // newly exposed area on the opposite side.
      //
      // Screen coordinates are 0..239 with the neutral eye centered at 120.
      // The web pad sends one-pixel offsets: X=-120..119 and Y=-119..120.
      // Physical left/right is reversed by the installed display orientation,
      // so invert X here while preserving exact asymmetric edge endpoints:
      // pad-left -120 -> canvas +119; pad-right +119 -> canvas -120.
      const int goatLookLX = manualLook ? 0 : constrain((lookX * 12) / 28, -12, 12);
      const int goatLookLY = manualLook ? 0 : constrain((lookY * 10) / 20, -10, 10);
      const int goatLookRX = manualLook ? 0 : constrain((lookX2 * 12) / 28, -12, 12);
      const int goatLookRY = manualLook ? 0 : constrain((lookY2 * 10) / 20, -10, 10);

      const int logicalX = constrain(lookX, -120, 119);
      const int logicalY = constrain(lookY, -119, 120);
      const int targetCanvasX = logicalX < 0
          ? ((-logicalX) * 119) / 120
          : (logicalX > 0 ? -(logicalX * 120) / 119 : 0);
      const int targetCanvasY = -logicalY;

      // Pointer samples and completed display frames do not arrive at perfectly
      // uniform intervals. Ease one-third of the remaining distance each frame
      // to remove visible stepping without introducing a slow fixed-rate crawl.
      if(manualLook){
        manualCanvasX = approachInt(manualCanvasX, targetCanvasX);
        manualCanvasY = approachInt(manualCanvasY, targetCanvasY);
      } else {
        manualCanvasX = 0;
        manualCanvasY = 0;
      }
      const int canvasX = manualCanvasX;
      const int canvasY = manualCanvasY;

      drawGoatFrame(*sprL, false, upperThreshold, lowerThreshold,
                    goatLookLX, goatLookLY, canvasX, canvasY,
                    glowL.currentQ8);
      drawGoatFrame(*sprR, true, upperThreshold, lowerThreshold,
                    goatLookRX, goatLookRY, canvasX, canvasY,
                    glowR.currentQ8);
    } else {
      const bool manualLid = manualLook && manualLidOverride >= 0;
      drawEyeInternal(*sprL, pupilSize, lookX, lookY, lid, manualLid);
      drawEyeInternal(*sprR, pupilSize, lookX2, lookY2, lid, manualLid);
    }

    // Wake both persistent workers together. Their priorities are below the
    // audio task, and each eye retains its own SPI host and DMA channel.
    xEventGroupClearBits(pushEvents, BOTH_PUSH_DONE);
    frameInFlight = true;
    frameStartedMs = millis();
    xTaskNotifyGive(leftWorker);
    xTaskNotifyGive(rightWorker);
  }
};
