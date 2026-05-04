#pragma once

namespace molterm {

// State machine that flips a boolean when the camera zoom crosses a
// threshold. Hysteresis (10% deadband) avoids flickering when the user
// pans near the threshold. Header-only so it stays cheap to instantiate
// per consumer (interface, residue labels, ligand pockets, …).
class ZoomGate {
public:
    void  setThreshold(float zoom) { threshold_ = zoom; }
    float threshold() const        { return threshold_; }

    void setEnabled(bool e) {
        enabled_ = e;
        if (!e) active_ = false;
    }
    bool enabled() const { return enabled_; }
    bool active()  const { return enabled_ && active_; }

    // Returns true on the *frame* where state flipped. Otherwise false.
    // `currentZoom` is read from Camera::zoom() each frame.
    bool update(float currentZoom) {
        if (!enabled_) {
            if (active_) { active_ = false; return true; }
            return false;
        }
        const float onAt  = threshold_;
        const float offAt = threshold_ * 0.9f;     // 10% deadband
        const bool wasActive = active_;
        if (active_) {
            if (currentZoom < offAt) active_ = false;
        } else {
            if (currentZoom >= onAt) active_ = true;
        }
        return active_ != wasActive;
    }

private:
    float threshold_ = 1e9f;   // disabled-by-default sentinel
    bool  enabled_   = false;
    bool  active_    = false;
};

} // namespace molterm
