#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"

#if ARDUINO_USB_MODE
#error "USB HID requires native USB-OTG mode (ARDUINO_USB_MODE=0)."
#endif

namespace {

// Hardware diagnostic mode maps physical Keys 1-13 to qwertyuiopasd and the
// encoder to f/g/h. The installed wiring has been verified, so production
// mappings are enabled. Set this to true to run the ordered key test again.
constexpr bool kDiagnosticKeyTestMode = false;

constexpr uint8_t kEncoderClkPin = 17;
constexpr uint8_t kEncoderDtPin = 18;
constexpr bool kEncoderHasPushSwitch = false;
constexpr uint8_t kEncoderSwitchPin = 12;
constexpr uint8_t kMatrixRows[] = {8, 9, 10, 11};
constexpr uint8_t kMatrixColumns[] = {4, 5, 6, 7};
constexpr uint8_t kMatrixRowCount = 4;
constexpr uint8_t kMatrixColumnCount = 4;
constexpr uint8_t kLedDataPin = 47;
constexpr uint16_t kLedCount = 23;
// Run the strip at full global brightness for maximum daytime visibility.
// Twenty-three all-white pixels can draw about 1.38 A from the 5 V supply.
constexpr uint8_t kLedBrightness = 255;
constexpr uint32_t kLedFrameIntervalMs = 25;
constexpr uint8_t kReasoningLevelMax = 4;
constexpr uint8_t kFnKey = 240;
constexpr uint8_t kMediaKeyFirst = 241;
constexpr uint8_t kMacroKeyFirst = 224;
constexpr uint8_t kMacroSlotCount = 8;
constexpr uint8_t kMacroStepCount = 8;
constexpr uint8_t kQuickLinkKeyFirst = 232;
constexpr uint8_t kQuickLinkCount = 8;
constexpr uint8_t kComboPressIntervalMs = 8;
constexpr uint8_t kHidLeftCtrl = 0xE0;
constexpr uint8_t kHidLeftShift = 0xE1;
constexpr uint8_t kHidLeftAlt = 0xE2;
constexpr uint8_t kHidLeftGui = 0xE3;
constexpr uint8_t kMacroPreferencesVersion = 2;

constexpr uint8_t kDefaultEncoderClockwiseKey =
  kDiagnosticKeyTestMode ? 'g' : 0;
constexpr uint8_t kDefaultEncoderCounterClockwiseKey =
  kDiagnosticKeyTestMode ? 'h' : 0;
constexpr uint8_t kDefaultEncoderShortPressKey =
  kDiagnosticKeyTestMode ? 'f' : 0;
constexpr uint8_t kDefaultEncoderLongPressKey = 0;

uint8_t encoderClockwiseKey = kDefaultEncoderClockwiseKey;
uint8_t encoderClockwiseModifier = 0;
uint8_t encoderCounterClockwiseKey = kDefaultEncoderCounterClockwiseKey;
uint8_t encoderCounterClockwiseModifier = 0;
uint8_t layerEncoderClockwiseKey = kDefaultEncoderClockwiseKey;
uint8_t layerEncoderClockwiseModifier = 0;
uint8_t layerEncoderCounterClockwiseKey = kDefaultEncoderCounterClockwiseKey;
uint8_t layerEncoderCounterClockwiseModifier = 0;
bool functionLayerHeld = false;

struct MacroStep {
  uint8_t key = 0;
  uint8_t modifier = 0;
  uint16_t delayMs = 20;
};

MacroStep macroSteps[kMacroSlotCount][kMacroStepCount];
uint8_t macroLengths[kMacroSlotCount] = {};

void updateFunctionLayer();

constexpr uint32_t kDebounceMs = 20;
constexpr uint32_t kEncoderSwitchDebounceMs = 30;
constexpr uint32_t kEncoderPressRotationGuardMs = 75;
constexpr uint32_t kLongPressMs = 700;
constexpr int8_t kTransitionsPerDetent = 4;

// Set this to true if clockwise and counter-clockwise are reversed after
// wiring CLK to GPIO 17 and DT to GPIO 18.
constexpr bool kReverseEncoderDirection = false;

USBHIDKeyboard keyboard;
USBHIDConsumerControl consumerControl;
Preferences macroPreferences;
bool keyTestEnabled = false;
bool calibrationEnabled = false;
bool calibrationRawState[kMatrixRowCount][kMatrixColumnCount] = {};
bool calibrationStableState[kMatrixRowCount][kMatrixColumnCount] = {};
uint32_t calibrationChangedAtMs[kMatrixRowCount][kMatrixColumnCount] = {};

void emitKeyTest(uint8_t index, const char* event) {
  if (keyTestEnabled) {
    Serial.printf("KEYTEST,%u,%s\n", index, event);
  }
}

void resetCalibrationState(uint32_t nowMs) {
  for (uint8_t row = 0; row < kMatrixRowCount; ++row) {
    for (uint8_t column = 0; column < kMatrixColumnCount; ++column) {
      calibrationRawState[row][column] = false;
      calibrationStableState[row][column] = false;
      calibrationChangedAtMs[row][column] = nowMs;
    }
  }
}

void pollCalibrationCell(uint8_t row, uint8_t column, bool pressed,
                         uint32_t nowMs) {
  if (pressed != calibrationRawState[row][column]) {
    calibrationRawState[row][column] = pressed;
    calibrationChangedAtMs[row][column] = nowMs;
  }
  if (nowMs - calibrationChangedAtMs[row][column] >= kDebounceMs &&
      calibrationStableState[row][column] != calibrationRawState[row][column]) {
    calibrationStableState[row][column] = calibrationRawState[row][column];
    Serial.printf("CAL,%u,%u,%s\n", row, column,
                  pressed ? "DOWN" : "UP");
  }
}

Adafruit_NeoPixel ledStrip(kLedCount, kLedDataPin, NEO_GRB + NEO_KHZ800);

uint32_t nextLedFrameAtMs = 0;

struct Rgb {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

constexpr Rgb kAgentColors[] = {
    {0, 180, 255},
    {30, 90, 255},
    {125, 45, 255},
    {230, 35, 210},
    {255, 105, 10},
    {80, 220, 35},
};
constexpr Rgb kCyan = {0, 190, 255};
constexpr Rgb kBlue = {25, 80, 255};
constexpr Rgb kWhite = {255, 255, 255};
constexpr Rgb kGreen = {25, 255, 65};
constexpr Rgb kRed = {255, 15, 8};
constexpr Rgb kPurple = {165, 35, 255};
constexpr Rgb kOrange = {255, 90, 5};
constexpr Rgb kMagenta = {255, 15, 115};

enum class LedAction : uint8_t {
  Task1,
  Task2,
  Task3,
  Task4,
  Task5,
  Task6,
  Submit,
  Approve,
  Reject,
  ContinueTask,
  FastMode,
  PushToTalk,
  EncoderPress,
};

enum class LedEffect : uint8_t {
  None,
  AgentPulse,
  SubmitSweep,
  ApproveConfirm,
  RejectFlash,
  ContinueWave,
  FastComet,
  EncoderGauge,
  EncoderPress,
  SidebarWipe,
};

struct LedEffectState {
  LedEffect type = LedEffect::None;
  uint32_t startedAtMs = 0;
  uint32_t durationMs = 0;
  Rgb color = {0, 0, 0};
};

LedEffectState ledEffect;
uint8_t selectedAgent = 0xFF;
uint8_t reasoningLevel = 2;
bool approveHeld = false;
bool pushToTalkHeld = false;
uint8_t pushToTalkPressCount = 0;
uint32_t approveStartedAtMs = 0;
bool encoderHasStepped = false;
uint32_t lastEncoderStepAtMs = 0;

bool encoderSteppedRecently(uint32_t nowMs) {
  return encoderHasStepped &&
         nowMs - lastEncoderStepAtMs <= kEncoderPressRotationGuardMs;
}

void ledControlPressed(LedAction action, uint32_t nowMs);
void ledControlReleased(LedAction action, uint32_t nowMs,
                        bool longPressTriggered);
void ledControlShortTriggered(LedAction action, uint32_t nowMs);
void ledControlLongTriggered(LedAction action, uint32_t nowMs);

void startLedEffect(LedEffect type, uint32_t nowMs, uint32_t durationMs,
                    Rgb color = {0, 0, 0}) {
  ledEffect.type = type;
  ledEffect.startedAtMs = nowMs;
  ledEffect.durationMs = durationMs;
  ledEffect.color = color;
}

uint32_t scaledColor(Rgb color, uint8_t intensity = 255) {
  return ledStrip.Color(
      static_cast<uint8_t>((static_cast<uint16_t>(color.red) * intensity) /
                           255),
      static_cast<uint8_t>((static_cast<uint16_t>(color.green) * intensity) /
                           255),
      static_cast<uint8_t>((static_cast<uint16_t>(color.blue) * intensity) /
                           255));
}

void setWrappedPixel(int32_t index, Rgb color, uint8_t intensity = 255) {
  while (index < 0) {
    index += kLedCount;
  }
  ledStrip.setPixelColor(static_cast<uint16_t>(index) % kLedCount,
                         scaledColor(color, intensity));
}

void fillColor(Rgb color, uint8_t intensity = 255) {
  ledStrip.fill(scaledColor(color, intensity));
}

uint16_t mediaUsage(uint8_t key) {
  switch (key) {
    case 241: return CONSUMER_CONTROL_PLAY_PAUSE;
    case 242: return CONSUMER_CONTROL_SCAN_NEXT;
    case 243: return CONSUMER_CONTROL_SCAN_PREVIOUS;
    case 244: return CONSUMER_CONTROL_VOLUME_INCREMENT;
    case 245: return CONSUMER_CONTROL_VOLUME_DECREMENT;
    case 246: return CONSUMER_CONTROL_MUTE;
    case 247: return CONSUMER_CONTROL_BRIGHTNESS_INCREMENT;
    case 248: return CONSUMER_CONTROL_BRIGHTNESS_DECREMENT;
    default: return 0;
  }
}

bool isMediaKey(uint8_t key) {
  return key >= kMediaKeyFirst && key <= 248;
}

void pressModifiers(uint8_t modifier) {
  if (modifier & 128) {
    keyboard.pressRaw(kHidLeftCtrl);
    delay(kComboPressIntervalMs);
  }
  if (modifier & 2) {
    keyboard.pressRaw(kHidLeftShift);
    delay(kComboPressIntervalMs);
  }
  if (modifier & 4) {
    keyboard.pressRaw(kHidLeftAlt);
    delay(kComboPressIntervalMs);
  }
  if (modifier & 8) {
    keyboard.pressRaw(kHidLeftGui);
    delay(kComboPressIntervalMs);
  }
}

void releaseModifiers(uint8_t modifier) {
  if (modifier & 8) {
    keyboard.releaseRaw(kHidLeftGui);
    delay(kComboPressIntervalMs);
  }
  if (modifier & 4) {
    keyboard.releaseRaw(kHidLeftAlt);
    delay(kComboPressIntervalMs);
  }
  if (modifier & 2) {
    keyboard.releaseRaw(kHidLeftShift);
    delay(kComboPressIntervalMs);
  }
  if (modifier & 128) {
    keyboard.releaseRaw(kHidLeftCtrl);
    delay(kComboPressIntervalMs);
  }
}

void pressKey(uint8_t key, uint8_t modifier = 0) {
  if (isMediaKey(key)) {
    consumerControl.press(mediaUsage(key));
    return;
  }
  if (modifier != 0) {
    pressModifiers(modifier);
  }
  delay(kComboPressIntervalMs);
  keyboard.pressRaw(key);
}

void releaseKey(uint8_t key, uint8_t modifier = 0) {
  if (isMediaKey(key)) {
    consumerControl.release();
    return;
  }
  keyboard.releaseRaw(key);
  delay(kComboPressIntervalMs);
  if (modifier != 0) {
    releaseModifiers(modifier);
  }
}

void tapKey(uint8_t key, uint8_t modifier = 0) {
  if (key >= kMacroKeyFirst && key < kMacroKeyFirst + kMacroSlotCount) {
    const uint8_t slot = key - kMacroKeyFirst;
    for (uint8_t index = 0; index < macroLengths[slot]; ++index) {
      tapKey(macroSteps[slot][index].key, macroSteps[slot][index].modifier);
      delay(macroSteps[slot][index].delayMs);
    }
    return;
  }
  if (key >= kQuickLinkKeyFirst && key < kQuickLinkKeyFirst + kQuickLinkCount) {
    tapKey(27, 8);
    delay(40);
    switch (key - kQuickLinkKeyFirst) {
      case 0: tapKey(12); break;
      case 1: tapKey(4); break;
      case 2: tapKey(23); break;
      case 3: tapKey(14); break;
      case 4: tapKey(16); break;
      case 5: tapKey(10); break;
      case 6: tapKey(8); break;
      case 7: tapKey(21); break;
      default: break;
    }
    return;
  }
  pressKey(key, modifier);
  delay(5);
  releaseKey(key, modifier);
}

enum class ButtonBehavior : uint8_t {
  Tap,
  Hold,
  LongPress,
  ShortAndLongPress,
};

class DebouncedButton {
 public:
  DebouncedButton(uint8_t pin, LedAction ledAction, ButtonBehavior behavior,
                  uint8_t primaryKey, uint8_t modifier = 0,
                  uint8_t longPressKey = 0,
                  uint32_t debounceMs = kDebounceMs)
      : pin_(pin),
        ledAction_(ledAction),
        behavior_(behavior),
        primaryKey_(primaryKey),
        modifier_(modifier),
        longPressKey_(longPressKey),
        basePrimaryKey_(primaryKey),
        baseModifier_(modifier),
        baseBehavior_(behavior),
        baseLongPressKey_(longPressKey),
        layerPrimaryKey_(primaryKey),
        layerModifier_(modifier),
        layerBehavior_(behavior),
        layerLongPressKey_(longPressKey),
        debounceMs_(debounceMs) {}

  void setMacro(uint8_t primaryKey, uint8_t modifier,
                ButtonBehavior behavior, uint8_t longPressKey) {
    basePrimaryKey_ = primaryKey;
    baseModifier_ = modifier;
    baseBehavior_ = behavior;
    baseLongPressKey_ = longPressKey;
    functionKey_ = primaryKey == kFnKey;
    if (!functionLayerHeld) {
      applyConfig(basePrimaryKey_, baseModifier_, baseBehavior_, baseLongPressKey_);
    }
  }

  void setLayerMacro(uint8_t primaryKey, uint8_t modifier,
                     ButtonBehavior behavior, uint8_t longPressKey) {
    layerPrimaryKey_ = primaryKey;
    layerModifier_ = modifier;
    layerBehavior_ = behavior;
    layerLongPressKey_ = longPressKey;
    if (functionLayerHeld) {
      applyConfig(layerPrimaryKey_, layerModifier_, layerBehavior_,
                  layerLongPressKey_);
    }
  }

  void applyLayer(bool layerActive) {
    if (functionKey_) {
      applyConfig(basePrimaryKey_, baseModifier_, baseBehavior_,
                  baseLongPressKey_);
      return;
    }
    if (layerActive) {
      applyConfig(layerPrimaryKey_, layerModifier_, layerBehavior_,
                  layerLongPressKey_);
    } else {
      applyConfig(basePrimaryKey_, baseModifier_, baseBehavior_,
                  baseLongPressKey_);
    }
  }

  uint8_t primaryKey() const { return primaryKey_; }
  uint8_t modifier() const { return modifier_; }
  ButtonBehavior behavior() const { return behavior_; }
  uint8_t longPressKey() const { return longPressKey_; }
  uint8_t layerPrimaryKey() const { return layerPrimaryKey_; }
  uint8_t layerModifier() const { return layerModifier_; }
  ButtonBehavior layerBehavior() const { return layerBehavior_; }
  uint8_t layerLongPressKey() const { return layerLongPressKey_; }
  void setTestIndex(uint8_t index) { testIndex_ = index; }

  void begin() {
    pinMode(pin_, INPUT_PULLUP);
    rawState_ = digitalRead(pin_);
    stableState_ = rawState_;
    rawStateChangedAtMs_ = millis();
  }

  void beginState() {
    rawState_ = HIGH;
    stableState_ = HIGH;
    rawStateChangedAtMs_ = millis();
  }

  void poll(uint32_t nowMs) {
    pollState(digitalRead(pin_) == LOW, nowMs);
  }

  void pollState(bool pressed, uint32_t nowMs) {
    const bool currentRawState = pressed ? LOW : HIGH;

    if (currentRawState != rawState_) {
      rawState_ = currentRawState;
      rawStateChangedAtMs_ = nowMs;
    }

    const bool debounceElapsed =
        nowMs - rawStateChangedAtMs_ >= debounceMs_;
    if (debounceElapsed && stableState_ != rawState_) {
      stableState_ = rawState_;
      if (stableState_ == LOW) {
        onPressed(nowMs);
      } else {
        onReleased(nowMs);
      }
    }

    if (ledAction_ == LedAction::EncoderPress && stableState_ == LOW &&
        encoderSteppedRecently(nowMs)) {
      suppressEncoderPress_ = true;
    }

    pollLongPress(nowMs);
  }

 private:
  void onPressed(uint32_t nowMs) {
    pressedAtMs_ = nowMs;
    longPressTriggered_ = false;
    suppressEncoderPress_ =
        ledAction_ == LedAction::EncoderPress && encoderSteppedRecently(nowMs);
    emitKeyTest(testIndex_, "DOWN");
    ledControlPressed(ledAction_, nowMs);

    if (primaryKey_ == kFnKey) {
      functionLayerHeld = true;
      updateFunctionLayer();
      return;
    }

    if (behavior_ == ButtonBehavior::Tap) {
      tapKey(primaryKey_, modifier_);
    } else if (behavior_ == ButtonBehavior::Hold) {
      if (ledAction_ == LedAction::PushToTalk) {
        if (pushToTalkPressCount++ == 0) {
          pressKey(primaryKey_, modifier_);
        }
      } else {
        pressKey(primaryKey_, modifier_);
      }
    }
  }

  void onReleased(uint32_t nowMs) {
    emitKeyTest(testIndex_, "UP");
    if (functionKey_) {
      functionLayerHeld = false;
      updateFunctionLayer();
      return;
    }
    if (behavior_ == ButtonBehavior::Hold) {
      if (ledAction_ == LedAction::PushToTalk) {
        if (pushToTalkPressCount > 0 && --pushToTalkPressCount == 0) {
          releaseKey(primaryKey_, modifier_);
        }
      } else {
        releaseKey(primaryKey_, modifier_);
      }
    } else if (behavior_ == ButtonBehavior::ShortAndLongPress &&
               !longPressTriggered_ && !suppressEncoderPress_) {
      tapKey(primaryKey_, modifier_);
      ledControlShortTriggered(ledAction_, nowMs);
    }
    ledControlReleased(ledAction_, nowMs, longPressTriggered_);
  }

  void pollLongPress(uint32_t nowMs) {
    const bool supportsLongPress =
        behavior_ == ButtonBehavior::LongPress ||
        behavior_ == ButtonBehavior::ShortAndLongPress;
    if (!supportsLongPress || stableState_ != LOW || longPressTriggered_ ||
        suppressEncoderPress_) {
      return;
    }

    if (nowMs - pressedAtMs_ < kLongPressMs) {
      return;
    }

    const uint8_t outputKey =
        behavior_ == ButtonBehavior::ShortAndLongPress ? longPressKey_
                                                       : primaryKey_;
    tapKey(outputKey, modifier_);
    longPressTriggered_ = true;
    ledControlLongTriggered(ledAction_, nowMs);
  }

  const uint8_t pin_;
  const LedAction ledAction_;
  ButtonBehavior behavior_;
  uint8_t primaryKey_;
  uint8_t modifier_;
  uint8_t longPressKey_;
  uint8_t basePrimaryKey_;
  uint8_t baseModifier_;
  ButtonBehavior baseBehavior_;
  uint8_t baseLongPressKey_;
  bool functionKey_ = false;
  uint8_t layerPrimaryKey_ = 0;
  uint8_t layerModifier_ = 0;
  ButtonBehavior layerBehavior_ = ButtonBehavior::Tap;
  uint8_t layerLongPressKey_ = 0;
  const uint32_t debounceMs_;

  void applyConfig(uint8_t primaryKey, uint8_t modifier,
                   ButtonBehavior behavior, uint8_t longPressKey) {
    primaryKey_ = primaryKey;
    modifier_ = modifier;
    behavior_ = behavior;
    longPressKey_ = longPressKey;
  }

  bool rawState_ = HIGH;
  bool stableState_ = HIGH;
  bool longPressTriggered_ = false;
  bool suppressEncoderPress_ = false;
  uint32_t rawStateChangedAtMs_ = 0;
  uint32_t pressedAtMs_ = 0;
  uint8_t testIndex_ = 255;
};

// Entries are in physical Key 1-13 order. The GPIO order below matches the
// finished hand wiring as measured by the diagnostic qwertyuiopasd test; it is
// intentionally not numerical. F14/F15 are avoided because they may be
// reserved by the host for display brightness.
DebouncedButton buttons[] = {
    {6, LedAction::Task1, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'q' : 0, 0},
    {10, LedAction::Task2, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'w' : 0, 0},
    {4, LedAction::Task3, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'e' : 0, 0},
    {7, LedAction::Task4, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'r' : 0, 0},
    {11, LedAction::Task5, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 't' : 0, 0},
    {14, LedAction::Task6, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'y' : 0, 0},
    {5, LedAction::FastMode, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'u' : 0, 0},
    {8, LedAction::Approve,
     ButtonBehavior::Tap, kDiagnosticKeyTestMode ? 'i' : 0, 0},
    {12, LedAction::Reject, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'o' : 0, 0},
    {15, LedAction::ContinueTask, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'p' : 0, 0},
    {9, LedAction::PushToTalk,
     ButtonBehavior::Tap, kDiagnosticKeyTestMode ? 'a' : 0, 0},
    {13, LedAction::PushToTalk,
     ButtonBehavior::Tap, kDiagnosticKeyTestMode ? 's' : 0, 0},
    {16, LedAction::Submit, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'd' : 0},
};

DebouncedButton encoderSwitch(
    kEncoderSwitchPin, LedAction::EncoderPress,
    kDiagnosticKeyTestMode ? ButtonBehavior::Tap
                           : ButtonBehavior::ShortAndLongPress,
    kDefaultEncoderShortPressKey, 0, kDefaultEncoderLongPressKey,
    kEncoderSwitchDebounceMs);

// Matrix positions follow the physical plate: encoder/blank, keys 1-2 on the
// top; keys 3-6; action row; microphone pair and send on the bottom.
DebouncedButton* matrixButtons[kMatrixRowCount][kMatrixColumnCount] = {
    {nullptr, &buttons[0], &buttons[1], nullptr},
    {&buttons[2], &buttons[3], &buttons[4], &buttons[5]},
    {&buttons[6], &buttons[7], &buttons[8], &buttons[9]},
  {nullptr, &buttons[11], &buttons[10], &buttons[12]},
};

void beginMatrix() {
  for (uint8_t row : kMatrixRows) {
    pinMode(row, OUTPUT);
    digitalWrite(row, HIGH);
  }
  for (uint8_t column : kMatrixColumns) {
    pinMode(column, INPUT_PULLUP);
  }
}

void pollMatrix(uint32_t nowMs) {
  for (uint8_t rowIndex = 0; rowIndex < kMatrixRowCount; ++rowIndex) {
    for (uint8_t row : kMatrixRows) {
      digitalWrite(row, HIGH);
    }
    digitalWrite(kMatrixRows[rowIndex], LOW);
    delayMicroseconds(30);
    for (uint8_t columnIndex = 0; columnIndex < kMatrixColumnCount;
         ++columnIndex) {
      const bool pressed =
          digitalRead(kMatrixColumns[columnIndex]) == LOW;
      if (calibrationEnabled) {
        pollCalibrationCell(rowIndex, columnIndex, pressed, nowMs);
      } else if (matrixButtons[rowIndex][columnIndex] != nullptr) {
        DebouncedButton* button = matrixButtons[rowIndex][columnIndex];
        button->pollState(digitalRead(kMatrixColumns[columnIndex]) == LOW,
                          nowMs);
      }
    }
  }
  for (uint8_t row : kMatrixRows) {
    digitalWrite(row, HIGH);
  }
}

void updateFunctionLayer() {
  for (DebouncedButton& button : buttons) {
    button.applyLayer(functionLayerHeld);
  }
  encoderSwitch.applyLayer(functionLayerHeld);
}

// Each entry describes one transition between the encoder's two-bit Gray-code
// states. Invalid transitions and contact bounce contribute zero movement.
constexpr int8_t kEncoderTransitionTable[16] = {
    0, -1, 1,  0,
    1, 0,  0, -1,
   -1, 0,  0,  1,
    0, 1, -1,  0,
};

uint8_t previousEncoderState = 0;
int8_t encoderTransitionCount = 0;

uint8_t readEncoderState() {
  const uint8_t clk = digitalRead(kEncoderClkPin) == HIGH ? 1 : 0;
  const uint8_t dt = digitalRead(kEncoderDtPin) == HIGH ? 1 : 0;
  return static_cast<uint8_t>((clk << 1) | dt);
}

void sendEncoderStep(bool clockwise, uint32_t nowMs) {
  if (kReverseEncoderDirection) {
    clockwise = !clockwise;
  }

  encoderHasStepped = true;
  lastEncoderStepAtMs = nowMs;
  emitKeyTest(clockwise ? 14 : 15, clockwise ? "CW" : "CCW");
    tapKey(clockwise ? (functionLayerHeld ? layerEncoderClockwiseKey
                                          : encoderClockwiseKey)
                     : (functionLayerHeld ? layerEncoderCounterClockwiseKey
                                           : encoderCounterClockwiseKey),
           clockwise ? (functionLayerHeld ? layerEncoderClockwiseModifier
                                          : encoderClockwiseModifier)
                     : (functionLayerHeld ? layerEncoderCounterClockwiseModifier
                                           : encoderCounterClockwiseModifier));

  if (clockwise && reasoningLevel < kReasoningLevelMax) {
    ++reasoningLevel;
  } else if (!clockwise && reasoningLevel > 0) {
    --reasoningLevel;
  }
  startLedEffect(LedEffect::EncoderGauge, nowMs, 900, kCyan);
}

void pollEncoder(uint32_t nowMs) {
  const uint8_t currentEncoderState = readEncoderState();
  if (currentEncoderState == previousEncoderState) {
    return;
  }

  const uint8_t transition =
      static_cast<uint8_t>((previousEncoderState << 2) | currentEncoderState);
  previousEncoderState = currentEncoderState;
  const int8_t movement = kEncoderTransitionTable[transition];
  if (movement != 0 && keyTestEnabled) {
    emitKeyTest(movement > 0 ? 14 : 15, "STEP");
  }
  encoderTransitionCount += movement;

  if (encoderTransitionCount >= kTransitionsPerDetent) {
    encoderTransitionCount = 0;
    sendEncoderStep(true, nowMs);
  } else if (encoderTransitionCount <= -kTransitionsPerDetent) {
    encoderTransitionCount = 0;
    sendEncoderStep(false, nowMs);
  }
}

void ledControlPressed(LedAction action, uint32_t nowMs) {
  const uint8_t actionIndex = static_cast<uint8_t>(action);
  if (actionIndex <= static_cast<uint8_t>(LedAction::Task6)) {
    selectedAgent = actionIndex;
    startLedEffect(LedEffect::AgentPulse, nowMs, 450,
                   kAgentColors[selectedAgent]);
    return;
  }

  switch (action) {
    case LedAction::Submit:
      startLedEffect(LedEffect::SubmitSweep, nowMs, 700, kBlue);
      break;
    case LedAction::Approve:
      approveHeld = true;
      approveStartedAtMs = nowMs;
      break;
    case LedAction::Reject:
      startLedEffect(LedEffect::RejectFlash, nowMs, 520, kRed);
      break;
    case LedAction::ContinueTask:
      startLedEffect(LedEffect::ContinueWave, nowMs, 900, kPurple);
      break;
    case LedAction::FastMode:
      startLedEffect(LedEffect::FastComet, nowMs, 2400, kOrange);
      break;
    case LedAction::PushToTalk:
      pushToTalkHeld = true;
      break;
    default:
      break;
  }
}

void ledControlReleased(LedAction action, uint32_t nowMs,
                        bool longPressTriggered) {
  (void)nowMs;
  if (action == LedAction::Approve && !longPressTriggered) {
    approveHeld = false;
  } else if (action == LedAction::PushToTalk) {
    pushToTalkHeld = pushToTalkPressCount > 0;
  }
}

void ledControlShortTriggered(LedAction action, uint32_t nowMs) {
  if (action == LedAction::EncoderPress) {
    startLedEffect(LedEffect::EncoderPress, nowMs, 500, kPurple);
  }
}

void ledControlLongTriggered(LedAction action, uint32_t nowMs) {
  if (action == LedAction::Approve) {
    approveHeld = false;
    startLedEffect(LedEffect::ApproveConfirm, nowMs, 650, kGreen);
  } else if (action == LedAction::EncoderPress) {
    startLedEffect(LedEffect::SidebarWipe, nowMs, 750, kBlue);
  }
}

void renderIdle(uint32_t nowMs) {
  const Rgb accent = selectedAgent < 6 ? kAgentColors[selectedAgent] : kCyan;
  fillColor(accent, selectedAgent < 6 ? 24 : 10);

  const int32_t head = static_cast<int32_t>((nowMs / 90) % kLedCount);
  setWrappedPixel(head, accent, 210);
  setWrappedPixel(head - 1, accent, 105);
  setWrappedPixel(head - 2, accent, 48);
}

void renderApproveProgress(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - approveStartedAtMs;
  const uint16_t lit = static_cast<uint16_t>(
      1 + (static_cast<uint32_t>(kLedCount - 1) *
           (elapsed > kLongPressMs ? kLongPressMs : elapsed)) /
              kLongPressMs);
  for (uint16_t index = 0; index < lit; ++index) {
    ledStrip.setPixelColor(index, scaledColor(kGreen, 215));
  }
  if (lit < kLedCount) {
    ledStrip.setPixelColor(lit, scaledColor(kOrange, 100));
  }
}

void renderPushToTalk(uint32_t nowMs) {
  const uint16_t phase = static_cast<uint16_t>(nowMs % 1200);
  const uint16_t triangle = phase < 600 ? phase : 1200 - phase;
  const uint8_t intensity = static_cast<uint8_t>(55 + (triangle * 145) / 600);
  fillColor(kMagenta, intensity);
  setWrappedPixel(static_cast<int32_t>((nowMs / 65) % kLedCount), kWhite,
                  185);
}

void renderTransient(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - ledEffect.startedAtMs;
  const uint32_t duration = ledEffect.durationMs == 0 ? 1 : ledEffect.durationMs;

  switch (ledEffect.type) {
    case LedEffect::AgentPulse: {
      const uint8_t intensity = static_cast<uint8_t>(
          35 + (static_cast<uint32_t>(duration - elapsed) * 220) / duration);
      fillColor(ledEffect.color, intensity);
      break;
    }
    case LedEffect::SubmitSweep: {
      const int32_t head = static_cast<int32_t>(
          (static_cast<uint32_t>(kLedCount) * elapsed) / duration);
      setWrappedPixel(head, kWhite, 255);
      setWrappedPixel(head - 1, kBlue, 210);
      setWrappedPixel(head - 2, kBlue, 100);
      setWrappedPixel(head - 3, kBlue, 40);
      break;
    }
    case LedEffect::ApproveConfirm: {
      const uint8_t intensity = static_cast<uint8_t>(
          35 + (static_cast<uint32_t>(duration - elapsed) * 220) / duration);
      fillColor(kGreen, intensity);
      break;
    }
    case LedEffect::RejectFlash:
      if ((elapsed / 130) % 2 == 0) {
        fillColor(kRed, 235);
      }
      break;
    case LedEffect::ContinueWave: {
      const int32_t head = static_cast<int32_t>((elapsed / 38) % kLedCount);
      setWrappedPixel(head, kPurple, 245);
      setWrappedPixel(head - 1, kPurple, 115);
      setWrappedPixel(head + kLedCount / 2, kPurple, 245);
      setWrappedPixel(head + kLedCount / 2 - 1, kPurple, 115);
      break;
    }
    case LedEffect::FastComet: {
      const int32_t head = static_cast<int32_t>((elapsed / 28) % kLedCount);
      for (uint8_t tail = 0; tail < 5; ++tail) {
        const uint8_t intensity = static_cast<uint8_t>(235 - tail * 45);
        setWrappedPixel(head - tail, kOrange, intensity);
        setWrappedPixel(head + kLedCount / 2 - tail, kOrange, intensity);
      }
      break;
    }
    case LedEffect::EncoderGauge: {
      const uint16_t lit = static_cast<uint16_t>(
          (static_cast<uint32_t>(reasoningLevel + 1) * kLedCount +
           kReasoningLevelMax) /
          (kReasoningLevelMax + 1));
      for (uint16_t index = 0; index < lit; ++index) {
        const Rgb gaugeColor = index < (kLedCount / 2) ? kCyan : kPurple;
        ledStrip.setPixelColor(index, scaledColor(gaugeColor, 190));
      }
      break;
    }
    case LedEffect::EncoderPress: {
      const uint16_t half = static_cast<uint16_t>(duration / 2);
      const uint32_t distance = elapsed < half ? elapsed : duration - elapsed;
      const uint8_t intensity = static_cast<uint8_t>(
          45 + (distance * 210) / (half == 0 ? 1 : half));
      fillColor(kPurple, intensity);
      break;
    }
    case LedEffect::SidebarWipe: {
      const uint16_t lit = static_cast<uint16_t>(
          1 + (static_cast<uint32_t>(kLedCount - 1) * elapsed) / duration);
      for (uint16_t offset = 0; offset < lit; ++offset) {
        const int32_t index = (offset % 2 == 0)
                                  ? static_cast<int32_t>(offset / 2)
                                  : -static_cast<int32_t>((offset + 1) / 2);
        setWrappedPixel(index, kBlue, 210);
      }
      break;
    }
    case LedEffect::None:
      break;
  }
}

void beginLedAnimation() {
  ledStrip.begin();
  ledStrip.setBrightness(kLedBrightness);
  ledStrip.clear();
  ledStrip.show();
}

void pollLedAnimation(uint32_t nowMs) {
  if (static_cast<int32_t>(nowMs - nextLedFrameAtMs) < 0) {
    return;
  }
  nextLedFrameAtMs = nowMs + kLedFrameIntervalMs;

  ledStrip.clear();

  if (approveHeld) {
    renderApproveProgress(nowMs);
  } else if (pushToTalkHeld) {
    renderPushToTalk(nowMs);
  } else {
    if (ledEffect.type != LedEffect::None &&
        nowMs - ledEffect.startedAtMs >= ledEffect.durationMs) {
      ledEffect.type = LedEffect::None;
    }
    if (ledEffect.type == LedEffect::None) {
      renderIdle(nowMs);
    } else {
      renderTransient(nowMs);
    }
  }

  ledStrip.show();
}

constexpr uint8_t kMacroCount = 16;
String serialCommand;

DebouncedButton& macroButton(uint8_t index) {
  return index < 13 ? buttons[index] : encoderSwitch;
}

void printMacro(uint8_t layer, uint8_t index) {
  if (index == 14) {
    Serial.printf("CFG,%u,%u,%u,%u,0,0\n", layer, index,
                  layer ? layerEncoderClockwiseKey : encoderClockwiseKey,
                  layer ? layerEncoderClockwiseModifier : encoderClockwiseModifier);
    return;
  }
  if (index == 15) {
    Serial.printf("CFG,%u,%u,%u,%u,0,0\n", layer, index,
                  layer ? layerEncoderCounterClockwiseKey : encoderCounterClockwiseKey,
                  layer ? layerEncoderCounterClockwiseModifier : encoderCounterClockwiseModifier);
    return;
  }
  DebouncedButton& button = macroButton(index);
  Serial.printf("CFG,%u,%u,%u,%u,%u,%u\n", layer, index,
                layer ? button.layerPrimaryKey() : button.primaryKey(),
                layer ? button.layerModifier() : button.modifier(),
                static_cast<uint8_t>(layer ? button.layerBehavior() : button.behavior()),
                layer ? button.layerLongPressKey() : button.longPressKey());
}

void printAllMacros(uint8_t layer) {
  for (uint8_t index = 0; index < kMacroCount; ++index) {
    printMacro(layer, index);
  }
  Serial.println("END");
}

void saveMacros() {
  macroPreferences.begin("macros", false);
  macroPreferences.putUChar("version", kMacroPreferencesVersion);
  for (uint8_t index = 0; index < kMacroCount; ++index) {
    char key[8];
    if (index == 14 || index == 15) {
      snprintf(key, sizeof(key), "k%u", index);
      macroPreferences.putUChar(
          key, index == 14 ? encoderClockwiseKey : encoderCounterClockwiseKey);
      snprintf(key, sizeof(key), "m%u", index);
      macroPreferences.putUChar(
          key, index == 14 ? encoderClockwiseModifier
                           : encoderCounterClockwiseModifier);
        snprintf(key, sizeof(key), "xk%u", index);
        macroPreferences.putUChar(
          key, index == 14 ? layerEncoderClockwiseKey
                   : layerEncoderCounterClockwiseKey);
        snprintf(key, sizeof(key), "xm%u", index);
        macroPreferences.putUChar(
          key, index == 14 ? layerEncoderClockwiseModifier
                   : layerEncoderCounterClockwiseModifier);
      continue;
    }
    DebouncedButton& button = macroButton(index);
    snprintf(key, sizeof(key), "k%u", index);
    macroPreferences.putUChar(key, button.primaryKey());
    snprintf(key, sizeof(key), "m%u", index);
    macroPreferences.putUChar(key, button.modifier());
    snprintf(key, sizeof(key), "b%u", index);
    macroPreferences.putUChar(key, static_cast<uint8_t>(button.behavior()));
    snprintf(key, sizeof(key), "l%u", index);
    macroPreferences.putUChar(key, button.longPressKey());
    snprintf(key, sizeof(key), "xk%u", index);
    macroPreferences.putUChar(key, button.layerPrimaryKey());
    snprintf(key, sizeof(key), "xm%u", index);
    macroPreferences.putUChar(key, button.layerModifier());
    snprintf(key, sizeof(key), "xb%u", index);
    macroPreferences.putUChar(key, static_cast<uint8_t>(button.layerBehavior()));
    snprintf(key, sizeof(key), "xl%u", index);
    macroPreferences.putUChar(key, button.layerLongPressKey());
  }
  for (uint8_t slot = 0; slot < kMacroSlotCount; ++slot) {
    char key[12];
    snprintf(key, sizeof(key), "mn%u", slot);
    macroPreferences.putUChar(key, macroLengths[slot]);
    for (uint8_t step = 0; step < kMacroStepCount; ++step) {
      snprintf(key, sizeof(key), "mk%u_%u", slot, step);
      macroPreferences.putUChar(key, macroSteps[slot][step].key);
      snprintf(key, sizeof(key), "mm%u_%u", slot, step);
      macroPreferences.putUChar(key, macroSteps[slot][step].modifier);
      snprintf(key, sizeof(key), "md%u_%u", slot, step);
      macroPreferences.putUShort(key, macroSteps[slot][step].delayMs);
    }
  }
  macroPreferences.end();
  Serial.println("OK,SAVED");
}

void loadMacros() {
  macroPreferences.begin("macros", true);
  if (macroPreferences.getUChar("version", 0) != kMacroPreferencesVersion) {
    macroPreferences.end();
    return;
  }
  for (uint8_t index = 0; index < kMacroCount; ++index) {
    char key[8];
    snprintf(key, sizeof(key), "k%u", index);
    const uint8_t primaryKey = macroPreferences.getUChar(key, 0);
    snprintf(key, sizeof(key), "m%u", index);
    const uint8_t modifier = macroPreferences.getUChar(key, 0);
    snprintf(key, sizeof(key), "b%u", index);
    const uint8_t behavior = macroPreferences.getUChar(key, 0);
    snprintf(key, sizeof(key), "l%u", index);
    const uint8_t longPressKey = macroPreferences.getUChar(key, 0);
    snprintf(key, sizeof(key), "xk%u", index);
    const uint8_t layerPrimaryKey = macroPreferences.getUChar(key, primaryKey);
    snprintf(key, sizeof(key), "xm%u", index);
    const uint8_t layerModifier = macroPreferences.getUChar(key, modifier);
    snprintf(key, sizeof(key), "xb%u", index);
    const uint8_t layerBehavior = macroPreferences.getUChar(key, behavior);
    snprintf(key, sizeof(key), "xl%u", index);
    const uint8_t layerLongPressKey = macroPreferences.getUChar(key, longPressKey);
    if (index == 14) {
      encoderClockwiseKey = primaryKey;
      encoderClockwiseModifier = modifier;
      layerEncoderClockwiseKey = layerPrimaryKey;
      layerEncoderClockwiseModifier = layerModifier;
      continue;
    }
    if (index == 15) {
      encoderCounterClockwiseKey = primaryKey;
      encoderCounterClockwiseModifier = modifier;
      layerEncoderCounterClockwiseKey = layerPrimaryKey;
      layerEncoderCounterClockwiseModifier = layerModifier;
      continue;
    }
    if (behavior <= static_cast<uint8_t>(ButtonBehavior::ShortAndLongPress)) {
      macroButton(index).setMacro(primaryKey, modifier,
                                  static_cast<ButtonBehavior>(behavior),
                                  longPressKey);
      if (layerBehavior <= static_cast<uint8_t>(ButtonBehavior::ShortAndLongPress)) {
        macroButton(index).setLayerMacro(
        layerPrimaryKey, layerModifier,
        static_cast<ButtonBehavior>(layerBehavior), layerLongPressKey);
      }
    }
  }
  for (uint8_t slot = 0; slot < kMacroSlotCount; ++slot) {
    char key[12];
    snprintf(key, sizeof(key), "mn%u", slot);
    macroLengths[slot] = macroPreferences.getUChar(key, 0);
    if (macroLengths[slot] > kMacroStepCount) macroLengths[slot] = kMacroStepCount;
    for (uint8_t step = 0; step < kMacroStepCount; ++step) {
      snprintf(key, sizeof(key), "mk%u_%u", slot, step);
      macroSteps[slot][step].key = macroPreferences.getUChar(key, 0);
      snprintf(key, sizeof(key), "mm%u_%u", slot, step);
      macroSteps[slot][step].modifier = macroPreferences.getUChar(key, 0);
      snprintf(key, sizeof(key), "md%u_%u", slot, step);
      macroSteps[slot][step].delayMs = macroPreferences.getUShort(key, 20);
    }
  }
  macroPreferences.end();
}

void printMacroDefinitions() {
  for (uint8_t slot = 0; slot < kMacroSlotCount; ++slot) {
    Serial.printf("MCR,%u,%u", slot, macroLengths[slot]);
    for (uint8_t step = 0; step < macroLengths[slot]; ++step) {
      Serial.printf(",%u,%u,%u", macroSteps[slot][step].key,
                    macroSteps[slot][step].modifier,
                    macroSteps[slot][step].delayMs);
    }
    Serial.println();
  }
  Serial.println("MEND");
}

void processMacroDefinition(const String& command) {
  char buffer[220];
  command.toCharArray(buffer, sizeof(buffer));
  char* token = strtok(buffer, ",");
  if (!token || strcmp(token, "MACRO") != 0) return;
  token = strtok(nullptr, ",");
  const int slot = token ? atoi(token) : -1;
  token = strtok(nullptr, ",");
  const int count = token ? atoi(token) : -1;
  if (slot < 0 || slot >= kMacroSlotCount || count < 0 ||
      count > kMacroStepCount) {
    Serial.println("ERR,MACRO");
    return;
  }
  for (int step = 0; step < count; ++step) {
    char* keyToken = strtok(nullptr, ",");
    char* modifierToken = strtok(nullptr, ",");
    char* delayToken = strtok(nullptr, ",");
    if (!keyToken || !modifierToken || !delayToken) {
      Serial.println("ERR,MACRO");
      return;
    }
    macroSteps[slot][step].key = static_cast<uint8_t>(atoi(keyToken));
    macroSteps[slot][step].modifier = static_cast<uint8_t>(atoi(modifierToken));
    macroSteps[slot][step].delayMs = static_cast<uint16_t>(atoi(delayToken));
  }
  macroLengths[slot] = static_cast<uint8_t>(count);
  Serial.println("OK,MACRO");
}

void processSerialCommand(const String& command) {
  if (command == "GET") {
    printAllMacros(0);
    return;
  }
  if (command == "GET,1") {
    printAllMacros(1);
    return;
  }
  if (command == "SAVE") {
    saveMacros();
    return;
  }
  if (command == "GETMACROS") {
    printMacroDefinitions();
    return;
  }
  if (command == "KEYTEST,1" || command == "KEYTEST,0") {
    keyTestEnabled = command.endsWith(",1");
    if (keyTestEnabled) calibrationEnabled = false;
    Serial.printf("OK,KEYTEST,%u\n", keyTestEnabled ? 1 : 0);
    return;
  }
  if (command == "CAL,1" || command == "CAL,0") {
    calibrationEnabled = command.endsWith(",1");
    if (calibrationEnabled) {
      keyTestEnabled = false;
      resetCalibrationState(millis());
    }
    Serial.printf("OK,CAL,%u\n", calibrationEnabled ? 1 : 0);
    return;
  }
  if (command.startsWith("MACRO,")) {
    processMacroDefinition(command);
    return;
  }
  if (command.startsWith("TEST,")) {
    const int index = command.substring(5).toInt();
    if (index >= 0 && index < kMacroCount) {
      if (index == 14 || index == 15) {
        tapKey(index == 14 ? encoderClockwiseKey : encoderCounterClockwiseKey,
               index == 14 ? encoderClockwiseModifier
                            : encoderCounterClockwiseModifier);
        Serial.println("OK,TEST");
        return;
      }
      DebouncedButton& button = macroButton(static_cast<uint8_t>(index));
      tapKey(button.primaryKey(), button.modifier());
      Serial.println("OK,TEST");
    }
    return;
  }
  const bool layerCommand = command.startsWith("SETL,1,");
  if (!command.startsWith("SET,") && !layerCommand) {
    return;
  }

  int index = -1;
  int primaryKey = 0;
  int modifier = 0;
  int behavior = 0;
  int longPressKey = 0;
  const char* setFormat = layerCommand ? "SETL,1,%d,%d,%d,%d,%d"
                                       : "SET,%d,%d,%d,%d,%d";
  if (sscanf(command.c_str(), setFormat, &index, &primaryKey,
             &modifier, &behavior, &longPressKey) != 5 || index < 0 ||
      index >= kMacroCount || primaryKey < 0 || primaryKey > 255 ||
      modifier < 0 || modifier > 255 || behavior < 0 || behavior > 3 ||
      longPressKey < 0 || longPressKey > 255) {
    Serial.println("ERR,INVALID");
    return;
  }
  if (layerCommand && index == 14) {
    layerEncoderClockwiseKey = static_cast<uint8_t>(primaryKey);
    layerEncoderClockwiseModifier = static_cast<uint8_t>(modifier);
  } else if (layerCommand && index == 15) {
    layerEncoderCounterClockwiseKey = static_cast<uint8_t>(primaryKey);
    layerEncoderCounterClockwiseModifier = static_cast<uint8_t>(modifier);
  } else if (layerCommand) {
    macroButton(static_cast<uint8_t>(index))
        .setLayerMacro(static_cast<uint8_t>(primaryKey),
                       static_cast<uint8_t>(modifier),
                       static_cast<ButtonBehavior>(behavior),
                       static_cast<uint8_t>(longPressKey));
  } else if (index == 14) {
    encoderClockwiseKey = static_cast<uint8_t>(primaryKey);
    encoderClockwiseModifier = static_cast<uint8_t>(modifier);
  } else if (index == 15) {
    encoderCounterClockwiseKey = static_cast<uint8_t>(primaryKey);
    encoderCounterClockwiseModifier = static_cast<uint8_t>(modifier);
  } else {
    macroButton(static_cast<uint8_t>(index))
        .setMacro(static_cast<uint8_t>(primaryKey),
                  static_cast<uint8_t>(modifier),
                  static_cast<ButtonBehavior>(behavior),
                  static_cast<uint8_t>(longPressKey));
  }
  Serial.println("OK,SET");
}

void pollSerial() {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (serialCommand.length() > 0) {
        processSerialCommand(serialCommand);
        serialCommand = "";
      }
    } else if (serialCommand.length() < 80) {
      serialCommand += character;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  loadMacros();
  for (uint8_t index = 0; index < 13; ++index) {
    DebouncedButton& button = buttons[index];
    button.setTestIndex(index);
    button.beginState();
  }
  beginMatrix();
  if (kEncoderHasPushSwitch) {
    encoderSwitch.setTestIndex(13);
    encoderSwitch.begin();
  }

  pinMode(kEncoderClkPin, INPUT_PULLUP);
  pinMode(kEncoderDtPin, INPUT_PULLUP);
  previousEncoderState = readEncoderState();

  keyboard.begin();
  consumerControl.begin();
  USB.begin();
  beginLedAnimation();
}

void loop() {
  const uint32_t nowMs = millis();

  pollSerial();
  pollMatrix(nowMs);
  pollEncoder(nowMs);
  if (kEncoderHasPushSwitch) {
    encoderSwitch.poll(nowMs);
  }
  pollLedAnimation(nowMs);

  delay(1);
}
