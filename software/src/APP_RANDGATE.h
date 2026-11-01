// RandGate: four-channel clocked generative gate sequencer
// Phazerville PSv1.14 / Teensy 3.2 (full-screen application)
// TR1 clock; TR2 reset; A-D independent 0/5V gates.
// Copyright (c) 2026. MIT license (see repository).
#ifdef ENABLE_APP_RANDGATE

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include "OC_apps.h"
#include "OC_core.h"
#include "OC_config.h"
#include "OC_ADC.h"
#include "OC_digital_inputs.h"
#include "OC_ui.h"
#include "HSApplication.h"

namespace {
static constexpr uint8_t RG_CHANNELS = 4;
static constexpr uint8_t RG_PRESET_COUNT = 8;
static constexpr uint8_t RG_SLOTS = 64; // 32 x 1/16 notes, 1/32 resolution
static constexpr uint16_t RG_MAGIC = 0x5247;
static constexpr uint8_t RG_VERSION = 2;

// The selected duration is a percentage of the generated rhythmic interval.
// rate: 1=1/32, 2=1/16, 3=1/8, 4=1/4.
struct RGEvent {
    uint8_t rate;
    uint8_t percent;
};

struct RGParams {
    uint16_t seed;
    uint8_t length; // in 1/16 notes, 1..32
    uint8_t chance[4]; // 1/32, 1/16, 1/8, 1/4
    uint8_t min_percent;
    uint8_t max_percent;
};

// Compact preset: a seed and generator snapshot exactly reproduce the audible gates.
// Staged (unapplied) edits are intentionally not part of a performance preset.
struct RGPreset {
    RGParams pattern[RG_CHANNELS];
    uint8_t pattern_density[RG_CHANNELS];
    uint8_t gate_scale[RG_CHANNELS];
    uint8_t global_density;
    uint8_t flags; // frozen mask bits 0..3; linked 4; retrigger 5; clock mode 6
};

struct RGPersist {
    uint16_t magic;
    uint8_t version;
    uint8_t density; // 0..200 percent
    uint8_t clock_mode; // 0: input 1/16; 1: input 1/32
    uint8_t linked;
    uint8_t retrigger;
    uint8_t frozen[RG_CHANNELS];
    uint8_t active_density[RG_CHANNELS]; // density at pattern generation
    uint8_t gate_scale[RG_CHANNELS]; // 0..200% manual gate length scaler
    uint8_t preset_valid_mask;
    uint8_t selected_preset;
    uint8_t preset_quantized; // 0=immediate, 1=next bar
    RGPreset presets[RG_PRESET_COUNT];
    RGParams edited[RG_CHANNELS];
    RGParams active[RG_CHANNELS]; // frozen generation snapshot
};

enum RGChannelParam {
    RG_LENGTH, RG_SEED, RG_P32, RG_P16, RG_P8, RG_P4,
    RG_MIN, RG_MAX, RG_GATE_SCALE, RG_FREEZE, RG_REROLL, RG_CH_PARAM_COUNT
};
enum RGGlobalParam {
    RG_DENSITY, RG_LINK, RG_CLOCKMODE, RG_RETRIG, RG_ALL_FREEZE,
    RG_ALL_REROLL, RG_GLOBAL_PARAM_COUNT
};
enum RGPresetParam {
    RG_PRESET_SLOT, RG_PRESET_LOAD, RG_PRESET_SAVE,
    RG_PRESET_QUANTIZE, RG_PRESET_PARAM_COUNT
};

static uint32_t NextRandom(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static bool Due(uint32_t now, uint32_t at) {
    return static_cast<int32_t>(now - at) >= 0;
}
static uint8_t Clamp(int value, int low, int high) {
    return static_cast<uint8_t>(constrain(static_cast<int>(value), low, high));
}
} // namespace

class RandGateApp : public HSApplication {
public:
    void Start() override {
        memset(&saved_, 0, sizeof(saved_));
        saved_.magic = RG_MAGIC;
        saved_.version = RG_VERSION;
        saved_.density = 100;
        saved_.clock_mode = 0;
        saved_.linked = 0;
        saved_.retrigger = 0;
        saved_.preset_quantized = 1;
        saved_.selected_preset = 0;
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) {
            RGParams &p = saved_.edited[ch];
            saved_.gate_scale[ch] = 100;
            p.seed = 1001 + ch * 997;
            p.length = 16;
            p.chance[0] = 15;
            p.chance[1] = 40;
            p.chance[2] = 45;
            p.chance[3] = 60;
            p.min_percent = 20;
            p.max_percent = 80;
            Generate(ch, p, saved_.density);
        }
        ResetTransport();
    }

    void Resume() override {
        ResetTransport();
        previous_clock_ = ReadInput(OC::DIGITAL_INPUT_1);
        previous_reset_ = ReadInput(OC::DIGITAL_INPUT_2);
    }

    void Suspend() {
        ResetTransport();
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch)
            GateOut(ch, false);
        pending_preset_ = -1;
    }

    void Controller() override {
        const uint32_t now = OC::CORE::ticks;
        const bool reset = ReadInput(OC::DIGITAL_INPUT_2);
        const bool clock = ReadInput(OC::DIGITAL_INPUT_1);

        if (reset && !previous_reset_) ResetTransport();
        previous_reset_ = reset;

        if (clock && !previous_clock_) OnClock(now);
        previous_clock_ = clock;

        if (midpoint_pending_ && Due(now, midpoint_due_)) {
            midpoint_pending_ = false;
            for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch)
                Fire(ch, static_cast<uint8_t>(previous_step_[ch] * 2 + 1), now);
        }

        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) {
            // Live knob/CV manipulation also applies to gates already high.
            if (high_[ch] && Due(now, start_at_[ch] + ScaledDuration(ch)))
                high_[ch] = false;
            if (pending_retrigger_[ch] && Due(now, retrigger_at_[ch])) {
                pending_retrigger_[ch] = false;
                start_at_[ch] = retrigger_at_[ch];
                high_[ch] = true;
            }
            GateOut(ch, high_[ch]);
        }
    }

    void View() override {
        gfxHeader("RandGate");
        gfxPrint(2, 13, "CH");
        gfxPos(22, 13);
        if (selected_ == RG_CHANNELS) graphics.printf("ALL");
        else graphics.printf("%c", 'A' + selected_);

        gfxPrint(54, 13, "CLK");
        gfxPos(79, 13);
        graphics.printf("%s", saved_.clock_mode ? "32" : "16");
        gfxPrint(102, 13, saved_.linked ? "LNK" : "IND");

        const bool global = selected_ == RG_CHANNELS;
        const uint8_t max_param = global ? RG_GLOBAL_PARAM_COUNT : RG_CH_PARAM_COUNT;
        if (param_ >= max_param) param_ = 0;

        gfxPrint(2, 26, global ? GlobalName(param_) : ChannelName(param_));
        if (isEditing && CursorBlink()) gfxFrame(0, 23, 126, 15);
        gfxPos(74, 26);
        PrintValue(global);

        gfxPrint(2, 41, "A  B  C  D");
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) {
            const int x = 3 + 19 * ch;
            gfxFrame(x, 51, 10, 9);
            if (high_[ch]) gfxRect(x + 2, 53, 6, 5);
            if (saved_.frozen[ch]) gfxPrint(x + 1, 61, "*");
        }
        gfxPos(83, 43);
        graphics.printf("D%3d", saved_.density);
        gfxPos(83, 55);
        graphics.printf("%s", all_frozen() ? "LOCK" : "PLAY");
    }

    void OnLeftEncoderMove(int delta) {
        if (!delta) return;
        selected_ = static_cast<uint8_t>((static_cast<int>(selected_) + (delta > 0 ? 1 : 4)) % 5);
        param_ = 0;
        CancelEdit();
        ResetCursor();
    }

    void OnLeftButtonPress() {
        if (selected_ == RG_CHANNELS) ToggleAllFreeze();
        else saved_.frozen[selected_] ^= 1;
        ResetCursor();
    }

    void OnRightButtonPress() {
        if (selected_ == RG_CHANNELS) {
            if (param_ == RG_ALL_FREEZE) { ToggleAllFreeze(); return; }
            if (param_ == RG_ALL_REROLL) { RerollAll(); return; }
        } else {
            if (param_ == RG_FREEZE) { saved_.frozen[selected_] ^= 1; return; }
            if (param_ == RG_REROLL) { Reroll(selected_); return; }
        }
        CursorToggle();
    }

    void OnRightEncoderMove(int delta) {
        if (!delta) return;
        const uint8_t count = (selected_ == RG_CHANNELS) ? RG_GLOBAL_PARAM_COUNT : RG_CH_PARAM_COUNT;
        if (!isEditing) {
            param_ = static_cast<uint8_t>((static_cast<int>(param_) + (delta > 0 ? 1 : count - 1)) % count);
        } else if (selected_ == RG_CHANNELS) {
            EditGlobal(delta > 0 ? 1 : -1);
        } else {
            EditChannel(selected_, delta > 0 ? 1 : -1);
        }
        ResetCursor();
    }

    static constexpr size_t storageSize() { return sizeof(RGPersist); }
    size_t Save(void *p) const {
        memcpy(p, &saved_, sizeof(saved_));
        return sizeof(saved_);
    }
    size_t Restore(const void *p) {
        RGPersist s;
        memcpy(&s, p, sizeof(s));
        if (s.magic != RG_MAGIC || s.version != RG_VERSION)
            return sizeof(s);
        saved_ = s;
        saved_.density = Clamp(saved_.density, 0, 200);
        saved_.clock_mode &= 1;
        saved_.linked &= 1;
        saved_.retrigger &= 1;
        saved_.selected_preset = Clamp(saved_.selected_preset, 0, RG_PRESET_COUNT - 1);
        saved_.preset_quantized &= 1;
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) {
            Sanitize(saved_.edited[ch]);
            Sanitize(saved_.active[ch]);
            saved_.frozen[ch] &= 1;
            saved_.gate_scale[ch] = Clamp(saved_.gate_scale[ch], 0, 200);
            saved_.active_density[ch] = Clamp(saved_.active_density[ch], 0, 200);
            Generate(ch, saved_.frozen[ch] ? saved_.active[ch] : saved_.edited[ch],
                     saved_.frozen[ch] ? saved_.active_density[ch] : saved_.density);
        }
        ResetTransport();
        return sizeof(s);
    }

private:
    static bool ReadInput(OC::DigitalInput input) {
        return OC::DigitalInputs::read_immediate(input);
    }

    static void Sanitize(RGParams &p) {
        if (p.seed == 0) p.seed = 1;
        p.length = Clamp(p.length, 1, 32);
        for (uint8_t i = 0; i < 4; ++i) p.chance[i] = Clamp(p.chance[i], 0, 100);
        p.min_percent = Clamp(p.min_percent, 0, 200);
        p.max_percent = Clamp(p.max_percent, p.min_percent, 200);
    }

    void Generate(uint8_t channel, const RGParams &config, uint8_t density) {
        RGParams p = config;
        Sanitize(p);
        saved_.active[channel] = p;
        saved_.active_density[channel] = density;
        uint32_t random_state = 0x9e3779b9UL ^ (static_cast<uint32_t>(p.seed) * 747796405UL);
        for (uint8_t slot = 0; slot < RG_SLOTS; ++slot) {
            RGEvent &e = pattern_[channel][slot];
            e.rate = 0;
            e.percent = 0;
            if (slot >= static_cast<uint8_t>(p.length * 2)) continue;

            // Check each eligible onset grid; coarsest successful grid wins.
            for (uint8_t rate = 1; rate <= 4; ++rate) {
                const uint8_t spacing = static_cast<uint8_t>(1 << (rate - 1));
                if ((slot % spacing) != 0) continue;
                const uint16_t chance = static_cast<uint16_t>(p.chance[rate - 1]) * density / 100;
                const uint8_t effective = (chance > 100) ? 100 : chance;
                if ((NextRandom(random_state) % 100) < effective)
                    e.rate = rate;
            }
            if (e.rate) {
                const uint16_t span = static_cast<uint16_t>(p.max_percent - p.min_percent) + 1;
                e.percent = static_cast<uint8_t>(p.min_percent + (NextRandom(random_state) % span));
            }
        }
    }

    void Reroll(uint8_t ch) {
        RGParams &p = saved_.edited[ch];
        // Every reroll gets a new, user-visible, reproducible seed.
        p.seed = static_cast<uint16_t>(p.seed * 25173u + 13849u);
        if (p.seed == 0) ++p.seed;
        Generate(ch, p, saved_.density);
        wrap_[ch] = false;
    }
    void RerollAll() {
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) Reroll(ch);
    }

    bool all_frozen() const {
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch)
            if (!saved_.frozen[ch]) return false;
        return true;
    }
    void ToggleAllFreeze() {
        const uint8_t new_state = all_frozen() ? 0 : 1;
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch)
            saved_.frozen[ch] = new_state;
    }

    void ResetTransport() {
        previous_clock_ = false;
        previous_reset_ = false;
        bar_clock_count_ = 0;
        have_clock_ = false;
        midpoint_pending_ = false;
        clock_period_ = OC_CORE_ISR_FREQ / 8; // 120 BPM (1/16 clock default)
        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) {
            phase_[ch] = 0;
            previous_step_[ch] = 0;
            wrap_[ch] = false;
            high_[ch] = false;
            pending_retrigger_[ch] = false;
            start_at_[ch] = 0;
            base_duration_[ch] = 0;
        }
    }

    void OnClock(uint32_t now) {
        if (have_clock_) {
            const uint32_t measured = now - last_clock_;
            if (measured > 2 && measured < OC_CORE_ISR_FREQ * 4)
                clock_period_ = measured;
        }
        have_clock_ = true;
        last_clock_ = now;
        midpoint_pending_ = false;
        const uint8_t ticks_per_bar = saved_.clock_mode ? 32 : 16;
        if (pending_preset_ >= 0 && (bar_clock_count_ % ticks_per_bar == 0)) {
            LoadPreset(static_cast<uint8_t>(pending_preset_));
        }
        ++bar_clock_count_;

        for (uint8_t ch = 0; ch < RG_CHANNELS; ++ch) {
            if (wrap_[ch]) {
                wrap_[ch] = false;
                if (!saved_.frozen[ch]) Reroll(ch);
            }
            const uint8_t pos = phase_[ch];
            if (saved_.clock_mode == 0) {
                previous_step_[ch] = pos;
                Fire(ch, static_cast<uint8_t>(pos * 2), now);
                phase_[ch] = static_cast<uint8_t>(pos + 1);
                if (phase_[ch] >= saved_.active[ch].length) {
                    phase_[ch] = 0;
                    wrap_[ch] = true;
                }
            } else {
                Fire(ch, pos, now);
                phase_[ch] = static_cast<uint8_t>(pos + 1);
                if (phase_[ch] >= saved_.active[ch].length * 2) {
                    phase_[ch] = 0;
                    wrap_[ch] = true;
                }
            }
        }
        if (!saved_.clock_mode) {
            midpoint_due_ = now + clock_period_ / 2;
            midpoint_pending_ = true;
        }
    }

    uint32_t ScaledDuration(uint8_t ch) const {
        // Physical CV1..CV4. Bipolar ±5 V -> ∓100 percentage points.
        // 0V is neutral, so unplugged CV leaves manual GATE SCALE unchanged.
        const ADC_CHANNEL adc_ch = (ch == 0) ? ADC_CHANNEL_1 :
            (ch == 1) ? ADC_CHANNEL_2 : (ch == 2) ? ADC_CHANNEL_3 : ADC_CHANNEL_4;
        const int cv = OC::ADC::pitch_value(adc_ch);
        const int cv_delta = constrain(cv * 100 / (5 * ONE_OCTAVE), -100, 100);
        const int scale = constrain(static_cast<int>(saved_.gate_scale[ch]) + cv_delta, 0, 200);
        return static_cast<uint32_t>((static_cast<uint64_t>(base_duration_[ch]) * scale) / 100u);
    }

    void Fire(uint8_t ch, uint8_t slot, uint32_t now) {
        if (slot >= RG_SLOTS) return;
        const RGEvent &e = pattern_[ch][slot];
        if (!e.rate || !e.percent) return;
        if (high_[ch] && !saved_.retrigger) return;

        const uint32_t unit32 = saved_.clock_mode ? clock_period_ : clock_period_ / 2;
        const uint64_t scaled = static_cast<uint64_t>(unit32) *
            (1u << (e.rate - 1)) * e.percent;
        const uint32_t duration = static_cast<uint32_t>(scaled / 100u);
        if (!duration) return;

        base_duration_[ch] = duration;
        start_at_[ch] = now;
        if (high_[ch] && saved_.retrigger) {
            // Produce a real low interval before the next gate (approx. 1ms).
            high_[ch] = false;
            retrigger_at_[ch] = now + OC_CORE_ISR_FREQ / 1000;
            pending_retrigger_[ch] = true;
        } else {
            high_[ch] = true;
        }
    }

    void EditGlobal(int delta) {
        switch (param_) {
        case RG_DENSITY:
            saved_.density = Clamp(saved_.density + delta, 0, 200);
            break;
        case RG_LINK: saved_.linked ^= 1; break;
        case RG_CLOCKMODE:
            saved_.clock_mode ^= 1;
            ResetTransport();
            break;
        case RG_RETRIG: saved_.retrigger ^= 1; break;
        default: break;
        }
    }

    void EditChannel(uint8_t ch, int delta) {
        RGParams &p = saved_.edited[ch];
        uint8_t linked_param = 0;
        switch (param_) {
        case RG_LENGTH: p.length = Clamp(p.length + delta, 1, 32); break;
        case RG_SEED: {
            int v = static_cast<int>(p.seed) + delta;
            p.seed = static_cast<uint16_t>(constrain(v, 1, 65535));
            break;
        }
        case RG_P32: case RG_P16: case RG_P8: case RG_P4:
            p.chance[param_ - RG_P32] = Clamp(p.chance[param_ - RG_P32] + delta, 0, 100);
            linked_param = 1;
            break;
        case RG_MIN:
            p.min_percent = Clamp(p.min_percent + delta, 0, p.max_percent);
            linked_param = 1;
            break;
        case RG_MAX:
            p.max_percent = Clamp(p.max_percent + delta, p.min_percent, 200);
            linked_param = 1;
            break;
        default: break;
        }
        if (saved_.linked && linked_param)
            for (uint8_t other = 0; other < RG_CHANNELS; ++other) {
                if (other == ch) continue;
                RGParams &dest = saved_.edited[other];
                if (param_ >= RG_P32 && param_ <= RG_P4)
                    dest.chance[param_ - RG_P32] = p.chance[param_ - RG_P32];
                if (param_ == RG_MIN) {
                    dest.min_percent = p.min_percent;
                    if (dest.max_percent < dest.min_percent) dest.max_percent = dest.min_percent;
                }
                if (param_ == RG_MAX) {
                    dest.max_percent = p.max_percent;
                    if (dest.min_percent > dest.max_percent) dest.min_percent = dest.max_percent;
                }
            }
        // Changes take effect at each unlocked channel's next pattern boundary.
        // Frozen patterns deliberately retain their generated event table.
    }

    static const char *ChannelName(uint8_t p) {
        static const char * const names[] = {
            "LENGTH", "SEED", "P 1/32", "P 1/16", "P 1/8", "P 1/4",
            "MIN %", "MAX %", "FREEZE", "REROLL"
        };
        return names[p];
    }
    static const char *GlobalName(uint8_t p) {
        static const char * const names[] = {
            "DENSITY", "LINK", "CLOCK IN", "RETRIG", "FREEZE ALL", "REROLL ALL"
        };
        return names[p];
    }
    void PrintValue(bool global) {
        if (global) {
            switch (param_) {
            case RG_DENSITY: graphics.printf("%d%%", saved_.density); break;
            case RG_LINK: graphics.printf("%s", saved_.linked ? "ON" : "OFF"); break;
            case RG_CLOCKMODE: graphics.printf("1/%d", saved_.clock_mode ? 32 : 16); break;
            case RG_RETRIG: graphics.printf("%s", saved_.retrigger ? "ON" : "OFF"); break;
            case RG_ALL_FREEZE: graphics.printf("%s", all_frozen() ? "LOCK" : "PRESS"); break;
            default: graphics.printf("PRESS"); break;
            }
        } else {
            const RGParams &p = saved_.edited[selected_];
            switch (param_) {
            case RG_LENGTH: graphics.printf("%d", p.length); break;
            case RG_SEED: graphics.printf("%u", p.seed); break;
            case RG_P32: case RG_P16: case RG_P8: case RG_P4:
                graphics.printf("%d%%", p.chance[param_ - RG_P32]); break;
            case RG_MIN: graphics.printf("%d%%", p.min_percent); break;
            case RG_MAX: graphics.printf("%d%%", p.max_percent); break;
            case RG_FREEZE: graphics.printf("%s", saved_.frozen[selected_] ? "LOCK" : "LIVE"); break;
            default: graphics.printf("PRESS"); break;
            }
        }
    }

    RGPersist saved_{};
    RGEvent pattern_[RG_CHANNELS][RG_SLOTS]{};
    uint32_t start_at_[RG_CHANNELS]{};
    uint32_t base_duration_[RG_CHANNELS]{};
    uint32_t retrigger_at_[RG_CHANNELS]{};
    uint32_t last_clock_ = 0;
    uint32_t midpoint_due_ = 0;
    uint32_t clock_period_ = OC_CORE_ISR_FREQ / 8;
    bool high_[RG_CHANNELS]{};
    bool pending_retrigger_[RG_CHANNELS]{};
    bool wrap_[RG_CHANNELS]{};
    bool have_clock_ = false;
    bool midpoint_pending_ = false;
    int8_t pending_preset_ = -1;
    uint8_t bar_clock_count_ = 0;
    bool previous_clock_ = false;
    bool previous_reset_ = false;
    uint8_t phase_[RG_CHANNELS]{};
    uint8_t previous_step_[RG_CHANNELS]{};
    uint8_t selected_ = 0; // A-D, ALL
    uint8_t param_ = 0;
};

RandGateApp RandGate_instance;

void RandGate_init() { RandGate_instance.BaseStart(); }
static constexpr size_t RandGate_storageSize() { return RandGateApp::storageSize(); }
static size_t RandGate_save(void *storage) { return RandGate_instance.Save(storage); }
static size_t RandGate_restore(const void *storage) { return RandGate_instance.Restore(storage); }
void RandGate_isr() { RandGate_instance.BaseController(); }
void RandGate_handleAppEvent(OC::AppEvent event) {
    switch (event) {
    case OC::APP_EVENT_RESUME: RandGate_instance.Resume(); break;
    case OC::APP_EVENT_SUSPEND: RandGate_instance.Suspend(); break;
    default: break;
    }
}
void RandGate_loop() {}
void RandGate_menu() { RandGate_instance.BaseView(); }
void RandGate_screensaver() { RandGate_instance.BaseScreensaver(); }
void RandGate_handleButtonEvent(const UI::Event &event) {
    if (event.type != UI::EVENT_BUTTON_PRESS) return;
    switch (event.control) {
    case OC::CONTROL_BUTTON_L: RandGate_instance.OnLeftButtonPress(); break;
    case OC::CONTROL_BUTTON_R: RandGate_instance.OnRightButtonPress(); break;
    default: break;
    }
}
void RandGate_handleEncoderEvent(const UI::Event &event) {
    if (event.control == OC::CONTROL_ENCODER_L)
        RandGate_instance.OnLeftEncoderMove(event.value);
    if (event.control == OC::CONTROL_ENCODER_R)
        RandGate_instance.OnRightEncoderMove(event.value);
}

#endif // ENABLE_APP_RANDGATE
