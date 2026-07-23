#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

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
constexpr uint8_t kEncoderSwitchPin = 21;
constexpr uint8_t kLedDataPin = 47;
constexpr uint16_t kLedCount = 23;
// Run the strip at full global brightness for maximum daytime visibility.
// Twenty-three all-white pixels can draw about 1.38 A from the 5 V supply.
constexpr uint8_t kLedBrightness = 255;
constexpr uint32_t kLedFrameIntervalMs = 25;
constexpr uint8_t kReasoningLevelMax = 4;

constexpr uint8_t kEncoderClockwiseKey =
    kDiagnosticKeyTestMode ? 'g' : KEY_F17;
constexpr uint8_t kEncoderCounterClockwiseKey =
    kDiagnosticKeyTestMode ? 'h' : KEY_F18;
constexpr uint8_t kEncoderShortPressKey =
    kDiagnosticKeyTestMode ? 'f' : KEY_F16;
constexpr uint8_t kEncoderLongPressKey = KEY_F13;

constexpr uint32_t kDebounceMs = 20;
constexpr uint32_t kEncoderSwitchDebounceMs = 30;
constexpr uint32_t kEncoderPressRotationGuardMs = 75;
constexpr uint32_t kLongPressMs = 700;
constexpr int8_t kTransitionsPerDetent = 4;

// Set this to true if clockwise and counter-clockwise are reversed after
// wiring CLK to GPIO 17 and DT to GPIO 18.
constexpr bool kReverseEncoderDirection = false;

USBHIDKeyboard keyboard;
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

void pressKey(uint8_t key, uint8_t modifier = 0) {
  if (modifier != 0) {
    keyboard.press(modifier);
  }
  keyboard.press(key);
}

void releaseKey(uint8_t key, uint8_t modifier = 0) {
  // USBHIDKeyboard updates its modifier bitmap without transmitting a report
  // until a non-modifier key changes. Clear the modifier first, then release
  // the key so the final report tells the host that both are up. Reversing
  // this order can leave macOS believing Command or Shift is still held.
  if (modifier != 0) {
    keyboard.release(modifier);
  }
  keyboard.release(key);
}

void tapKey(uint8_t key, uint8_t modifier = 0) {
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
        debounceMs_(debounceMs) {}

  void begin() {
    pinMode(pin_, INPUT_PULLUP);
    rawState_ = digitalRead(pin_);
    stableState_ = rawState_;
    rawStateChangedAtMs_ = millis();
  }

  void poll(uint32_t nowMs) {
    const bool currentRawState = digitalRead(pin_);

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
    ledControlPressed(ledAction_, nowMs);

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
  const ButtonBehavior behavior_;
  const uint8_t primaryKey_;
  const uint8_t modifier_;
  const uint8_t longPressKey_;
  const uint32_t debounceMs_;

  bool rawState_ = HIGH;
  bool stableState_ = HIGH;
  bool longPressTriggered_ = false;
  bool suppressEncoderPress_ = false;
  uint32_t rawStateChangedAtMs_ = 0;
  uint32_t pressedAtMs_ = 0;
};

// Entries are in physical Key 1-13 order. The GPIO order below matches the
// finished hand wiring as measured by the diagnostic qwertyuiopasd test; it is
// intentionally not numerical. F14/F15 are avoided because macOS reserves
// them for display brightness.
DebouncedButton buttons[] = {
    {6, LedAction::Task1, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'q' : '1',
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_GUI},
    {10, LedAction::Task2, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'w' : '2',
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_GUI},
    {4, LedAction::Task3, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'e' : '3',
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_GUI},
    {7, LedAction::Task4, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'r' : '4',
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_GUI},
    {11, LedAction::Task5, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 't' : '5',
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_GUI},
    {14, LedAction::Task6, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'y' : '6',
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_GUI},
    {5, LedAction::FastMode, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'u' : KEY_F20,
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_SHIFT},
    {8, LedAction::Approve,
     kDiagnosticKeyTestMode ? ButtonBehavior::Tap
                            : ButtonBehavior::LongPress,
     kDiagnosticKeyTestMode ? 'i' : KEY_F19,
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_CTRL},
    {12, LedAction::Reject, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'o' : KEY_F20,
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_CTRL},
    {15, LedAction::ContinueTask, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'p' : KEY_F19,
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_SHIFT},
    {9, LedAction::PushToTalk,
     kDiagnosticKeyTestMode ? ButtonBehavior::Tap : ButtonBehavior::Hold,
     kDiagnosticKeyTestMode ? 'a' : KEY_F13,
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_CTRL},
    {13, LedAction::PushToTalk,
     kDiagnosticKeyTestMode ? ButtonBehavior::Tap : ButtonBehavior::Hold,
     kDiagnosticKeyTestMode ? 's' : KEY_F13,
     kDiagnosticKeyTestMode ? 0 : KEY_LEFT_CTRL},
    {16, LedAction::Submit, ButtonBehavior::Tap,
     kDiagnosticKeyTestMode ? 'd' : KEY_F20},
};

DebouncedButton encoderSwitch(
    kEncoderSwitchPin, LedAction::EncoderPress,
    kDiagnosticKeyTestMode ? ButtonBehavior::Tap
                           : ButtonBehavior::ShortAndLongPress,
    kEncoderShortPressKey, 0, kEncoderLongPressKey,
    kEncoderSwitchDebounceMs);

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
  tapKey(clockwise ? kEncoderClockwiseKey
                   : kEncoderCounterClockwiseKey);

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

}  // namespace

void setup() {
  for (DebouncedButton& button : buttons) {
    button.begin();
  }
  encoderSwitch.begin();

  pinMode(kEncoderClkPin, INPUT_PULLUP);
  pinMode(kEncoderDtPin, INPUT_PULLUP);
  previousEncoderState = readEncoderState();

  keyboard.begin();
  USB.begin();
  beginLedAnimation();
}

void loop() {
  const uint32_t nowMs = millis();

  for (DebouncedButton& button : buttons) {
    button.poll(nowMs);
  }
  pollEncoder(nowMs);
  encoderSwitch.poll(nowMs);
  pollLedAnimation(nowMs);

  delay(1);
}
