#pragma once
#include "navmesh/native/InfoGeometryPayload.h"
#include "navmesh/native/RegisteredFormIdentity.h"
#include <stdexcept>

namespace SkyBuild::Navmesh {
// The caller retains both forms, both infos and the native publication barrier.
// This is an identity/payload transition, not a borrower-retirement
// certificate. Advance the journal before native calls so an ordinary failure
// after mutation remains reversible. Native faults still require retained-owner
// quarantine.
template <class Entry, class Assign, class Bind>
void AdoptFormIdentity(Entry &entry, Assign assign, Bind bind) {
  if (entry.phase)
    throw std::logic_error("Identity transfer already has an active journal");
  entry.phase = 1;
  assign(entry.source, 0);
  entry.phase = 2;
  assign(entry.work, entry.sourceID);
  ExchangeFormIdentityFiles(*entry.source, *entry.work);
  entry.phase = 3;
  entry.work->formFlags = entry.sourceFlags;
  ExchangeInfoGeometryPayload(*entry.stable, *entry.scratch);
  entry.phase = 4;
  bind(*entry.stable, *entry.work);
  bind(*entry.scratch, *entry.source);
}

// Must finish before releasing the native barrier. Assign and Bind have their
// native no-throw contract here. No allocation or lookup policy lives in this
// inverse; it restores the exact owners retained by the caller's journal.
template <class Entry, class Assign, class Bind>
void RestoreFormIdentity(Entry &entry, Assign assign, Bind bind) noexcept {
  if (!entry.phase)
    return;
  if (entry.phase >= 2)
    assign(entry.work, 0);
  assign(entry.source, entry.sourceID);
  bind(*entry.stable, *entry.source);
  if (entry.phase >= 4)
    ExchangeInfoGeometryPayload(*entry.stable, *entry.scratch);
  assign(entry.work, entry.scratchID);
  entry.work->unk110 = entry.scratch;
  bind(*entry.scratch, *entry.work);
  if (entry.phase >= 3)
    ExchangeFormIdentityFiles(*entry.source, *entry.work);
  entry.source->formFlags = entry.sourceFlags;
  entry.work->formFlags = entry.scratchFlags;
  entry.phase = 0;
}
} // namespace SkyBuild::Navmesh
