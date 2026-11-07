#pragma once
#include <nlohmann/json.hpp>
namespace SkyBuild::UI::PackEditor::View {
using Json = nlohmann::json;
void Initialize(void (*submit)(const Json &), void (*update)() = nullptr);
void SetOpen(bool open);
void OpenPickedItem();
bool IsOpen();
void Publish(Json state);
void SearchResults(Json results);
void MaterialResults(Json results);
} // namespace SkyBuild::UI::PackEditor::View
