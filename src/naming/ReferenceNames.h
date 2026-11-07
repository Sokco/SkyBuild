#pragma once
#include "naming/ReferenceNameData.h"
namespace SkyBuild::Naming {
void Initialize();
void Reset();
void Set(RE::FormID id, const std::string &name);
void Save(SKSE::SerializationInterface *serialization);
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length);
} // namespace SkyBuild::Naming
