#pragma once
#include <string>
#include <cstdint>
namespace SkyBuild::UI::PackEditor {
void Initialize();
void Open();
bool CanPickObject(RE::FormID id);
void OpenForObject(RE::FormID id);
void CancelSelection();
void Close();
void Update();
void RegenerateResourceIcons();
void QueueResourceIcon(const std::string &plugin, std::uint32_t local,
                       bool temporary = false);
bool IsOpen();
} // namespace SkyBuild::UI::PackEditor
