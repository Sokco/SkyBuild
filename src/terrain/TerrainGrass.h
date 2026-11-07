#pragma once
#include "terrain/GrassHeightSnapshot.h"
#include <memory>
namespace SkyBuild::Terrain::Grass {
std::shared_ptr<const HeightSnapshot> CaptureHeights();
inline constexpr std::uint32_t ClearSaveRecord = 0x524C4347; // GCLR
void Initialize();
void SetWorld(RE::FormID world);
bool Paint(RE::FormID world, const Brush &brush, bool restore);
std::uint64_t UndoStroke(RE::FormID world);
std::uint64_t RedoStroke(RE::FormID world);
void History(RE::FormID world, bool redo);
void ClearRedo();
void ResetPersistent();
void Save(SKSE::SerializationInterface *serialization);
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length);
void Publish(RE::TESObjectCELL *cell, const Surfaces &surfaces,
             const Volume &volume);
void BrushArea(const Brush &brush);
void FinishBrush(std::uint64_t heldStroke, bool building);
void Remove(RE::FormID cell);
void Clear();

void Update();
} // namespace SkyBuild::Terrain::Grass
