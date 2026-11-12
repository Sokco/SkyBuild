#pragma once
namespace SkyBuild::ControlBindings { struct Event; }
#include "activators/ConnectionData.h"
namespace SkyBuild::Activators {
void Initialize();
void Update();
bool SupportsProperties(RE::TESObjectREFR *object);
void OpenProperties(RE::TESObjectREFR *object);
void Begin(RE::TESObjectREFR *source);
void Cancel();
bool IsConnecting();
bool HasTarget();
bool HandleButton(const ControlBindings::Event *button);
void Reset();
void Save(SKSE::SerializationInterface *serialization);
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length);
} // namespace SkyBuild::Activators
