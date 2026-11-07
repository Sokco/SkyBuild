#pragma once
#include <functional>
namespace SkyBuild::Containers {
bool RegisterPapyrus(RE::BSScript::IVirtualMachine *vm);
void Initialize();
void Update();
void Reset();
bool PrepareNewPlacement(RE::TESObjectREFR *reference);
bool CanRemove(RE::TESObjectREFR *reference);
void RequestRemoval(RE::TESObjectREFR *reference, std::function<void()> remove);
} // namespace SkyBuild::Containers
