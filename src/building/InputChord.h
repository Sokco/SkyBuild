#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace SkyBuild {
struct InputChord {
  std::vector<std::uint32_t> keys;
  InputChord() = default;
  InputChord(std::uint32_t code) { if (code) keys.push_back(code); }
  InputChord(std::initializer_list<std::uint32_t> codes) {
    for (auto code : codes) if (code) keys.push_back(code);
  }
  explicit operator bool() const { return !keys.empty(); }
  bool operator==(const InputChord &) const = default;
};

namespace Input {
enum class Phase { Down, Held, Up };
struct ActionEvent {
  int action;
  std::uint32_t terminal;
  Phase phase;
  float duration{};
};
struct ChordBinding {
  InputChord chord;
  unsigned scopes{};
};

// Resolves held keys in press order; only ambiguous single-key actions wait
// for release. Native events remain untouched for other input consumers.
class ChordResolver {
public:
  static constexpr std::size_t KeyCount = 282;
  void Configure(std::vector<ChordBinding> bindings) {
    bindings_ = std::move(bindings);
    states_.assign(bindings_.size(), {});
    held_.fill(0);
    used_.fill(false);
    captured_.fill(false);
    scope_ = 0;
  }
  template <class Emit> void SetScope(unsigned scope, Emit emit) {
    if (scope == scope_) return;
    for (std::size_t i = 0; i < states_.size(); ++i) {
      auto &state = states_[i];
      state.pending = false;
      if (state.active && !(bindings_[i].scopes & scope)) {
        emit(ActionEvent{static_cast<int>(i), bindings_[i].chord.keys.back(), Phase::Up});
        state.active = false;
      }
    }
    if (!scope) {
      held_.fill(0);
      used_.fill(false);
      captured_.fill(false);
    }
    scope_ = scope;
  }
  template <class Emit>
  bool Process(std::uint32_t key, Phase phase, float duration, Emit emit) {
    if (key >= KeyCount || !scope_) return false;
    const bool wheel = key == 264 || key == 265;
    bool consumed = captured_[key];
    if (phase == Phase::Down && held_[key]) {
      // Recover from a missed release without firing a deferred tap.
      for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto &keys = bindings_[i].chord.keys;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) continue;
        if (states_[i].active)
          emit(ActionEvent{static_cast<int>(i), keys.back(), Phase::Up});
        states_[i] = {};
      }
      held_[key] = 0;
    }
    if (phase == Phase::Down && !held_[key]) {
      held_[key] = ++sequence_;
      used_[key] = false;
      captured_[key] = false;
      auto best = bindings_.size();
      for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const auto &binding = bindings_[i];
        if (!(binding.scopes & scope_) || !binding.chord) continue;
        const auto &keys = binding.chord.keys;
        if (keys.back() != key || !Ordered(keys)) continue;
        if (best == bindings_.size() ||
            (keys.size() == 2 && (bindings_[best].chord.keys.size() == 1 ||
             held_[keys.front()] > held_[bindings_[best].chord.keys.front()]))) best = i;
      }
      if (best != bindings_.size()) {
        captured_[key] = consumed = true;
        const auto &keys = bindings_[best].chord.keys;
        if (keys.size() == 1 && IsModifier(key)) {
          states_[best].pending = true;
        } else {
          if (keys.size() > 1)
            for (auto component : keys) used_[component] = true;
          states_[best].active = true;
          emit(ActionEvent{static_cast<int>(best), key, Phase::Down});
        }
      }
    } else if (phase == Phase::Held && held_[key]) {
      for (std::size_t i = 0; i < states_.size(); ++i) {
        if (states_[i].active && bindings_[i].chord.keys.back() == key)
          emit(ActionEvent{static_cast<int>(i), key, Phase::Held, duration});
      }
    }
    for (std::size_t i = 0; i < states_.size(); ++i) {
      const auto &keys = bindings_[i].chord.keys;
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) continue;
      consumed |= states_[i].active || states_[i].pending;
      if (phase != Phase::Up && !wheel) continue;
      auto &state = states_[i];
      if (state.pending && !used_[key] && held_[key]) {
        emit(ActionEvent{static_cast<int>(i), key, Phase::Down});
        emit(ActionEvent{static_cast<int>(i), key, Phase::Up});
      }
      if (state.active)
        emit(ActionEvent{static_cast<int>(i), keys.back(), Phase::Up});
      state = {};
    }
    if (phase == Phase::Up || wheel) {
      held_[key] = 0; used_[key] = false; captured_[key] = false;
    }
    return consumed;
  }
private:
  bool Ordered(const std::vector<std::uint32_t> &keys) const {
    if (keys.empty() || keys.size() > 2 || keys.back() >= KeyCount || !held_[keys.back()])
      return false;
    return keys.size() == 1 ||
           (keys.front() < KeyCount && held_[keys.front()] &&
            held_[keys.front()] < held_[keys.back()]);
  }
  bool IsModifier(std::uint32_t key) const {
    for (const auto &binding : bindings_) {
      const auto &keys = binding.chord.keys;
      if ((binding.scopes & scope_) && keys.size() == 2 && keys.front() == key) return true;
    }
    return false;
  }
  struct State { bool active{}, pending{}; };
  std::vector<ChordBinding> bindings_;
  std::vector<State> states_;
  std::array<std::uint64_t, KeyCount> held_{};
  std::array<bool, KeyCount> used_{};
  std::array<bool, KeyCount> captured_{};
  std::uint64_t sequence_{};
  unsigned scope_{};
};
} // namespace Input
} // namespace SkyBuild
