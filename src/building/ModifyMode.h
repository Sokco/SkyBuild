#pragma once
namespace SkyBuild::ModifyMode {
bool HasTarget();
bool CanCopy();
bool CanEditProperties();
void Break();
void Copy();
void EditProperties();
void Start();
void Stop();
void Update();
void Use();
void SetOpacity(float opacity);
void SetTint(const RE::NiColor &tint);
} // namespace SkyBuild::ModifyMode
