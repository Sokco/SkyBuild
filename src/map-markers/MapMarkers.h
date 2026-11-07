#pragma once
#include "map-markers/MarkerData.h"
namespace SKSE {
class SerializationInterface;
}
namespace SkyBuild::MapMarkers {
void Initialize();
void Open();
void Close();
bool IsOpen();
void Update();
void Restore();
void Reset();
void Save(SKSE::SerializationInterface *serialization);
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length);
} // namespace SkyBuild::MapMarkers
