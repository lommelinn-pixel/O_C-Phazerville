// Rout: latched 1-to-4 gate router for Phazerville / Ornament & Crime
// Target: Phazerville PSv1.14, Teensy 3.2
//
// TR1 : gate input
// CV1 : 0..5V route CV
// A-D : mutually-exclusive regenerated 5V gate outputs
//
// Left encoder: select mask slot A-D
// Left press  : enable/disable selected output (minimum one enabled)
// Right press : toggle DELAY / ROTATE edit target
// Right encoder:
//   DELAY  0..20ms in 0.5ms steps
//   ROTATE +0..+3 through enabled outputs
//
// Routing is always latched. On each delayed rising edge CV1 is sampled once.
// CV changes during the gate do not move the gate. Falling edge is delayed by
// the same amount, preserving gate length. Fixed ~50mV Schmitt hysteresis is
// applied around CV routing boundaries.

#ifdef ENABLE_APP_ROUT

#include <Arduino.h>
#include <stdint.h>
#include "OC_ADC.h"
#include "OC_apps.h"
#include "OC_config.h"
#include "OC_digital_inputs.h"
#include "OC_strings.h"
#include "OC_ui.h"
#include "HSApplication.h"
#include "util/util_settings.h"

namespace {

static constexpr int ROUT_CV_MAX = 5 * ONE_OCTAVE;
static constexpr int ROUT_HYSTERESIS = 77;
static constexpr uint8_t ROUT_QUEUE_SIZE = 64;
static constexpr uint8_t ROUT_QUEUE_MASK = ROUT_QUEUE_SIZE - 1;
static_assert((ROUT_QUEUE_SIZE & ROUT_QUEUE_MASK) == 0, "Rout queue size must be a power of two");

enum RoutSettings {
    ROUT_MASK,
    ROUT_DELAY_HALF_MS,
    ROUT_ROTATE,
    ROUT_SETTING_LAST
};

enum RoutRightParam : uint8_t {
    ROUT_PARAM_DELAY = 0,
    ROUT_PARAM_ROTATE = 1
};

struct RoutEdge {
    uint32_t due;
    bool high;
};

} // namespace

class RoutApp : public HSApplication,
                public settings::SettingsBase<RoutApp, ROUT_SETTING_LAST> {
public:
    void Start() override {
        InitDefaults();
        ResetRuntime();
    }

    void Resume() override {
        ResetRuntime();
        prev_input_gate_ = ReadGate();
    }

    void Suspend() {
        ResetRuntime();
    }

    void Controller() override {
        const uint32_t now = OC::CORE::ticks;
        const bool gate = ReadGate();

        if (gate != prev_input_gate_) {
            if (gate) {
                active_delay_ticks_ = DelayTicks();
                PushEdge(now + active_delay_ticks_, true);
            } else {
                PushEdge(now + active_delay_ticks_, false);
            }
            prev_input_gate_ = gate;
        }

        ProcessDueEdges(now);
        display_cv_ = ReadCV();

        for (uint8_t ch = 0; ch < 4; ++ch)
            GateOut(ch, gate_active_ && latched_output_ == ch);
    }

    void View() override {
        DrawInterface();
    }

    void OnLeftEncoderMove(int direction) {
        if (!direction) return;
        int next = static_cast<int>(mask_cursor_) + (direction > 0 ? 1 : -1);
        if (next > 3) next = 0;
        if (next < 0) next = 3;
        mask_cursor_ = static_cast<uint8_t>(next);
        ResetCursor();
    }

    void OnLeftButtonPress() {
        uint8_t mask = Mask();
        const uint8_t bit = static_cast<uint8_t>(1u << mask_cursor_);

        if (mask & bit) {
            if (ActiveCount(mask) <= 1) return;
            mask &= ~bit;
        } else {
            mask |= bit;
        }

        apply_value(ROUT_MASK, mask);
        logical_bin_ = -1;
        ResetCursor();
    }

    void OnRightButtonPress() {
        right_param_ = (right_param_ == ROUT_PARAM_DELAY)
                     ? ROUT_PARAM_ROTATE : ROUT_PARAM_DELAY;
        ResetCursor();
    }

    void OnRightEncoderMove(int direction) {
        if (!direction) return;

        if (right_param_ == ROUT_PARAM_DELAY) {
            const int delta = direction > 0 ? 1 : -1;
            if (change_value(ROUT_DELAY_HALF_MS, delta)) {
                ResetPipeline();
                prev_input_gate_ = ReadGate();
                active_delay_ticks_ = DelayTicks();
            }
        } else {
            int r = Rotation();
            r += direction > 0 ? 1 : -1;
            if (r > 3) r = 0;
            if (r < 0) r = 3;
            apply_value(ROUT_ROTATE, r);
        }
        ResetCursor();
    }

private:
    static bool ReadGate() {
        return OC::DigitalInputs::read_immediate(OC::DIGITAL_INPUT_1);
    }

    static int ReadCV() {
        int cv = OC::ADC::pitch_value(ADC_CHANNEL_1);
        return constrain(cv, 0, ROUT_CV_MAX);
    }

    uint8_t Mask() const {
        return static_cast<uint8_t>(get_value(ROUT_MASK)) & 0x0f;
    }

    uint8_t Rotation() const {
        return static_cast<uint8_t>(get_value(ROUT_ROTATE)) & 0x03;
    }

    uint16_t DelayTicks() const {
        const uint32_t half_ms = static_cast<uint32_t>(get_value(ROUT_DELAY_HALF_MS));
        return static_cast<uint16_t>((half_ms * OC_CORE_ISR_FREQ + 1000u) / 2000u);
    }

    void ResetPipeline() {
        queue_head_ = 0;
        queue_tail_ = 0;
        gate_active_ = false;
        latched_output_ = -1;
    }

    void ResetRuntime() {
        ResetPipeline();
        prev_input_gate_ = false;
        active_delay_ticks_ = DelayTicks();
        logical_bin_ = -1;
        display_cv_ = 0;
        mask_cursor_ = 0;
        right_param_ = ROUT_PARAM_DELAY;
    }

    void PushEdge(uint32_t due, bool high) {
        const uint8_t next = static_cast<uint8_t>((queue_tail_ + 1u) & ROUT_QUEUE_MASK);
        if (next == queue_head_) {
            ResetPipeline();
        }

        pending_[queue_tail_].due = due;
        pending_[queue_tail_].high = high;
        queue_tail_ = static_cast<uint8_t>((queue_tail_ + 1u) & ROUT_QUEUE_MASK);
    }

    static bool TimeReached(uint32_t now, uint32_t due) {
        return static_cast<int32_t>(now - due) >= 0;
    }

    void ProcessDueEdges(uint32_t now) {
        while (queue_head_ != queue_tail_ && TimeReached(now, pending_[queue_head_].due)) {
            const bool high = pending_[queue_head_].high;
            queue_head_ = static_cast<uint8_t>((queue_head_ + 1u) & ROUT_QUEUE_MASK);

            if (high) {
                const int cv = ReadCV();
                display_cv_ = cv;
                latched_output_ = SelectOutput(cv);
                gate_active_ = latched_output_ >= 0;
            } else {
                gate_active_ = false;
                latched_output_ = -1;
            }
        }
    }

    static uint8_t ActiveCount(uint8_t mask) {
        uint8_t count = 0;
        for (uint8_t i = 0; i < 4; ++i)
            if (mask & (1u << i)) ++count;
        return count;
    }

    static uint8_t BuildActiveList(uint8_t mask, uint8_t *active) {
        uint8_t n = 0;
        for (uint8_t i = 0; i < 4; ++i)
            if (mask & (1u << i)) active[n++] = i;
        return n;
    }

    int QuantizeWithHysteresis(int cv, uint8_t n) {
        if (n <= 1) {
            logical_bin_ = 0;
            return 0;
        }

        if (logical_bin_ < 0 || logical_bin_ >= n) {
            int bin = (cv * n) / ROUT_CV_MAX;
            if (bin >= n) bin = n - 1;
            logical_bin_ = static_cast<int8_t>(bin);
            return bin;
        }

        while (logical_bin_ < n - 1) {
            const int upper_boundary = ((logical_bin_ + 1) * ROUT_CV_MAX) / n;
            if (cv < upper_boundary + ROUT_HYSTERESIS) break;
            ++logical_bin_;
        }

        while (logical_bin_ > 0) {
            const int lower_boundary = (logical_bin_ * ROUT_CV_MAX) / n;
            if (cv >= lower_boundary - ROUT_HYSTERESIS) break;
            --logical_bin_;
        }

        return logical_bin_;
    }

    int SelectOutput(int cv) {
        uint8_t active[4];
        const uint8_t n = BuildActiveList(Mask(), active);
        if (!n) return -1;

        const int logical = QuantizeWithHysteresis(cv, n);
        const uint8_t rotated = static_cast<uint8_t>((logical + Rotation()) % n);
        return active[rotated];
    }

    void DrawInterface() {
        gfxHeader("Rout");

        gfxPrint(2, 13, "CV");
        gfxPos(20, 13);
        gfxPrintVoltage(display_cv_);

        gfxPrint(72, 13, "DLY");
        gfxPos(94, 13);
        const int half_ms = get_value(ROUT_DELAY_HALF_MS);
        graphics.printf("%d.%d", half_ms / 2, (half_ms & 1) ? 5 : 0);
        if (right_param_ == ROUT_PARAM_DELAY && CursorBlink())
            gfxFrame(69, 11, 58, 11);

        gfxPrint(72, 24, "ROT");
        gfxPos(100, 24);
        graphics.printf("+%d", Rotation());
        if (right_param_ == ROUT_PARAM_ROTATE && CursorBlink())
            gfxFrame(69, 22, 58, 11);

        gfxPrint(2, 36, "OUT");
        for (uint8_t ch = 0; ch < 4; ++ch) {
            const int x = 39 + ch * 22;
            char label[2] = { static_cast<char>('A' + ch), 0 };
            gfxPrint(x, 35, label);
            gfxFrame(x - 1, 44, 8, 8);
            if (gate_active_ && latched_output_ == ch)
                gfxRect(x + 1, 46, 4, 4);
        }

        gfxPrint(2, 56, "MASK");
        const uint8_t mask = Mask();
        for (uint8_t ch = 0; ch < 4; ++ch) {
            const int x = 39 + ch * 22;
            char label[2] = { static_cast<char>('A' + ch), 0 };
            gfxPrint(x, 55, label);
            if (!(mask & (1u << ch)))
                gfxLine(x - 1, 60, x + 6, 60);
            if (mask_cursor_ == ch && CursorBlink())
                gfxFrame(x - 3, 53, 12, 11);
        }
    }

    RoutEdge pending_[ROUT_QUEUE_SIZE];
    uint8_t queue_head_ = 0;
    uint8_t queue_tail_ = 0;

    bool prev_input_gate_ = false;
    bool gate_active_ = false;
    int8_t latched_output_ = -1;
    int8_t logical_bin_ = -1;
    uint16_t active_delay_ticks_ = 0;

    int display_cv_ = 0;
    uint8_t mask_cursor_ = 0;
    RoutRightParam right_param_ = ROUT_PARAM_DELAY;
};

SETTINGS_DECLARE(RoutApp, ROUT_SETTING_LAST) {
    {15, 1, 15, "Mask",   NULL, settings::STORAGE_TYPE_U8},
    { 4, 0, 40, "Delay",  NULL, settings::STORAGE_TYPE_U8},
    { 0, 0,  3, "Rotate", NULL, settings::STORAGE_TYPE_U8},
};

RoutApp Rout_instance;

void Rout_init() {
    Rout_instance.BaseStart();
}

static constexpr size_t Rout_storageSize() {
    return RoutApp::storageSize();
}

static size_t Rout_save(void *storage) {
    return Rout_instance.Save(storage);
}

static size_t Rout_restore(const void *storage) {
    return Rout_instance.Restore(storage);
}

void Rout_isr() {
    Rout_instance.BaseController();
}

void Rout_handleAppEvent(OC::AppEvent event) {
    switch (event) {
    case OC::APP_EVENT_RESUME:
        Rout_instance.Resume();
        break;
    case OC::APP_EVENT_SUSPEND:
        Rout_instance.Suspend();
        break;
    default:
        break;
    }
}

void Rout_loop() {}

void Rout_menu() {
    Rout_instance.BaseView();
}

void Rout_screensaver() {
    Rout_instance.BaseScreensaver();
}

void Rout_handleButtonEvent(const UI::Event &event) {
    if (event.type != UI::EVENT_BUTTON_PRESS) return;

    switch (event.control) {
    case OC::CONTROL_BUTTON_L:
        Rout_instance.OnLeftButtonPress();
        break;
    case OC::CONTROL_BUTTON_R:
        Rout_instance.OnRightButtonPress();
        break;
    default:
        break;
    }
}

void Rout_handleEncoderEvent(const UI::Event &event) {
    if (event.control == OC::CONTROL_ENCODER_L)
        Rout_instance.OnLeftEncoderMove(event.value);
    if (event.control == OC::CONTROL_ENCODER_R)
        Rout_instance.OnRightEncoderMove(event.value);
}

#endif // ENABLE_APP_ROUT
