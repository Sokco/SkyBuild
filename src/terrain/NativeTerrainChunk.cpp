#include "terrain/NativeTerrainChunk.h"
#include "RE/H/hkMemoryRouter.h"
#include "RE/H/hkpMoppBvTreeShape.h"
#include "RE/H/hkpMoppCode.h"
#include "RE/S/ScrapHeap.h"
#include "terrain/LandscapeGeometry.h"
#include "terrain/TerrainBuffers.h"
#include "terrain/TerrainMaterialCache.h"
#include "terrain/TerrainMaterialSharing.h"
#include "terrain/TerrainNativeOwnership.h"
#include "terrain/TerrainWeldingPolicy.h"
#include "terrain/brush-overlay/BrushOverlayTrace.h"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <d3d11.h>
#include <wrl/client.h>

namespace SkyBuild::Terrain {
namespace {
using WeldingFn = void (*)(RE::hkpShapeCollection *, const RE::hkpShape *,
                           std::int32_t, bool, bool);
WeldingFn originalWelding{};
bool weldingPolicyInstalled{};

void ApplyTerrainWeldingPolicy(RE::hkpShapeCollection *mesh,
                               const RE::hkpShape *tree, std::int32_t type,
                               bool flagA, bool flagB) {
  originalWelding(
      mesh, tree,
      WeldingPolicy::EffectiveType(mesh, mesh->disableWelding, type), flagA,
      flagB);
}
// These factories/constructors were traced in the SE and AE runtime images.
// Use Address Library identities, never offsets into a caller's instructions.
// 70655: no-argument BSTriShape factory (fully constructs + engine allocates).
// 79434: no-argument bhkCompressedMeshShape wrapper factory (0x30 bytes).
// 80696: Skyrim hkpCompressedMeshShape ctor(this, indexBits, radius), 0x130
// bytes. 78147: bhkRigidBody ctor(this), 78372: bhkNiCollisionObject
// ctor(this). The latter is the base of bhkCollisionObject; install the
// concrete vtable as its native CreateClone does immediately after calling that
// constructor.
template <class T> T *Factory(REL::RelocationID id) {
  const REL::Relocation<T *(*)()> create{id};
  auto result = create();
  if (!result)
    throw std::bad_alloc();
  return result;
}

struct SharedTerrainMaterial {
  // Retaining the source also prevents its material identity being recycled
  // while retired chunks still hold the corresponding cache entry.
  RE::NiPointer<RE::BSLightingShaderProperty> source, keeper;
};
std::shared_ptr<SharedTerrainMaterial>
AcquireMaterial(const TerrainTextures &textures) {
  static WeakMaterialCache<std::uintptr_t, SharedTerrainMaterial> cache;
  return cache.Acquire(
      reinterpret_cast<std::uintptr_t>(textures.property->material), [&] {
        auto shared = std::make_shared<SharedTerrainMaterial>();
        shared->source = textures.property;
        shared->keeper = RE::NiPointer<RE::BSLightingShaderProperty>(
            RE::BSLightingShaderProperty::Create());
        if (!shared->keeper)
          throw std::bad_alloc();
        RetainTerrainMaterial(*shared->keeper, textures.property->material);
        if (shared->keeper->material != textures.property->material)
          throw std::runtime_error("Landscape material sharing failed");
        return shared;
      });
}

// Native interfaces traced from bhkMoppBvTreeShape's construction path in
// SE and AE. The compiler input is initialized by the game, rather than
// redeclaring Havok's private tolerance/optimization fields.
struct alignas(16) MoppCompilerInput {
  std::array<std::byte, 0x30> storage{};
};
static_assert(sizeof(MoppCompilerInput) == 0x30);
struct MoppShapeInfo {
  std::uint32_t flags{};
  std::uint32_t padding{};
  RE::hkpShape *child{};
  RE::hkpMoppCode *code{};
  float scale{1};
  std::uint32_t tailPadding{};
};
static_assert(offsetof(MoppShapeInfo, child) == 0x08);
static_assert(offsetof(MoppShapeInfo, code) == 0x10);
static_assert(offsetof(MoppShapeInfo, scale) == 0x18);
static_assert(sizeof(MoppShapeInfo) == 0x20);

RE::NiPointer<RE::bhkShape>
AccelerateCollision(const RE::NiPointer<RE::bhkShape> &meshWrapper) {
  auto mesh = static_cast<RE::hkpCompressedMeshShape *>(
      meshWrapper->referencedObject.get());
  const auto container = mesh->GetContainer();
  if (!container)
    throw std::runtime_error("Missing terrain MOPP source container");
  // Native GetNextKey skips triangles rejected by Havok's degeneracy test.
  // The MOPP compiler sees that same container, not every stored array entry.
  const auto collidableCount = container->GetNumChildShapes();
  if (collidableCount <= 0 || collidableCount > mesh->bigTriangles.size())
    throw std::runtime_error(std::format(
        "Invalid terrain MOPP source: {} collidable keys, {} stored triangles",
        collidableCount, mesh->bigTriangles.size()));
  MoppCompilerInput input;
  const REL::Relocation<void (*)(MoppCompilerInput *)> initialize{
      REL::RelocationID(78796, 80780)};
  const REL::Relocation<RE::hkpMoppCode *(*)(const RE::hkpShapeContainer *,
                                             const MoppCompilerInput *, void *)>
      compile{REL::RelocationID(78801, 80785)};
  initialize(&input);
  // Native defaults disable chunk subdivision. Preserve the original child
  // shape keys: no triangle remapping and no additional chunk code to manage.
  if (input.storage[0x1e] != std::byte{0})
    throw std::runtime_error("Unsupported terrain MOPP compiler defaults");
  RE::hkpMoppCode *compiled{};
  {
    compiled = compile(container, &input, nullptr);
  }
  if (!compiled)
    throw std::runtime_error("Native terrain MOPP compilation failed");
  RE::hkRefPtr<RE::hkpMoppCode> code(compiled);
  compiled->RemoveReference();
  if (compiled->data.empty() ||
      compiled->buildType !=
          RE::hkpMoppCode::BuildType::kBuildWithoutChunkSubdivision)
    throw std::runtime_error("Invalid native terrain MOPP code");

  // Use the game's complete wrapper construction path. It creates Skyrim's
  // specialized MOPP shape, retains the original code, installs userData,
  // and manages native child/code ownership. Do not synthesize Havok vtables.
  RE::NiPointer<RE::bhkShape> wrapper;
  const REL::Relocation<void (*)(RE::bhkShape *, const MoppShapeInfo *)> build{
      REL::RelocationID(77775, 79643)};
  const MoppShapeInfo info{0, 0, mesh, compiled, 1, 0};

  WeldingPolicy::State welding{mesh};
  // Only our verified uncompressed triangle terrain takes this policy. Keep
  // native wrapper construction and completion, but honor the existing
  // disabled-welding setting using Havok's actual None initializer.
  welding.honorDisabled = weldingPolicyInstalled && mesh->disableWelding &&
                          mesh->chunks.empty() && mesh->convexPieces.empty() &&
                          !mesh->bigTriangles.empty();
  static_assert(int(RE::hkpWeldingUtility::WeldingType::kAntiClockwise) == 0);
  static_assert(int(RE::hkpWeldingUtility::WeldingType::kNone) == 6);
  // The wrapper's completion method (79646) consumes one reference from
  // info.code. Transfer an extra reference; retain our local owner while
  // validating. Skyrim's specialized tree separately retains the code at +70.
  {
    wrapper = RE::NiPointer<RE::bhkShape>(Factory<RE::bhkShape>(REL::RelocationID(77768, 79636)));
    compiled->AddReference();
    WeldingPolicy::Scope capture(&welding);
    build(wrapper.get(), &info);
  }

  if (welding.honorDisabled &&
      (!mesh->disableWelding || container->IsWeldingEnabled() ||
       int(mesh->weldingType.underlying()) !=
           int(RE::hkpWeldingUtility::WeldingType::kNone)))
    throw std::runtime_error(
        "Terrain disabled-welding construction state changed");
  const auto raw = static_cast<RE::hkpShape *>(wrapper->referencedObject.get());
  if (!raw || raw->type != RE::hkpShapeType::kMOPP)
    throw std::runtime_error("Native terrain MOPP wrapper construction failed");
  const auto tree = static_cast<const RE::hkpMoppBvTreeShape *>(raw);
  if (tree->child.childShape != mesh || !tree->code || !tree->moppData ||
      !tree->moppDataSize)
    throw std::runtime_error("Native terrain MOPP child/code mismatch");

  return wrapper;
}

template <class T>
void AllocateCollisionArray(RE::hkArray<T> &array, std::int32_t count) {
  const REL::Relocation<void (*)(RE::hkMemoryAllocator *, void *, std::int32_t,
                                 std::int32_t)>
      reserve{REL::RelocationID(56641, 57048)};
  AllocateEmptyHavokArray(
      array, count, [&](void *storage, auto capacity, auto stride) {
        reserve(RE::hkContainerHeapAllocator::GetSingleton(), storage, capacity,
                stride);
      });
}

RE::NiPointer<RE::bhkShape> CollisionShape(const Mesh &mesh, Vec origin) {
  auto router = RE::hkMemoryRouter::GetInstancePtr();
  if (!router || !router->Heap)
    throw std::runtime_error("Terrain Havok allocator is unavailable");
  auto memory = router->Heap->BlockAlloc(0x130);
  if (!memory)
    throw std::bad_alloc();
  std::memset(memory, 0, 0x130);
  const REL::Relocation<RE::hkpCompressedMeshShape *(*)(void *, int, float)>
      ctor{REL::RelocationID(78718, 80696)};
  const auto shape = ctor(memory, 17, .01F);
  shape->memSizeAndFlags = 0x130; // ctor's hkReferencedObject default is 0xFFFF
  RE::hkRefPtr<RE::hkpCompressedMeshShape> owned(shape);
  shape->RemoveReference();
  shape->disableWelding = true;
  shape->materialType = RE::hkpCompressedMeshShape::MaterialType::MATERIAL_NONE;
  shape->error = .001F;
  shape->defaultCollisionFilterInfo.SetCollisionLayer(RE::COL_LAYER::kGround);
  const auto scale = RE::bhkWorld::GetWorldScale();
  AllocateCollisionArray(shape->bigVertices,
                         static_cast<int>(mesh.vertices.size()));
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
    const auto p = (mesh.vertices[i].position - origin) * scale;
    shape->bigVertices[static_cast<int>(i)] = RE::NiPoint3{p.x, p.y, p.z};
  }
  // Initialize bounds in Havok units (including the first vertex).
  const auto first = (mesh.vertices.front().position - origin) * scale;
  Vec lo = first, hi = first;
  for (const auto &v : mesh.vertices) {
    const auto p = (v.position - origin) * scale;
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  shape->bounds.min = RE::NiPoint3{lo.x - .01F, lo.y - .01F, lo.z - .01F};
  shape->bounds.max = RE::NiPoint3{hi.x + .01F, hi.y + .01F, hi.z + .01F};
  AllocateCollisionArray(shape->bigTriangles,
                         static_cast<int>(mesh.triangles.size()));
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
    auto &t = shape->bigTriangles[static_cast<int>(i)];
    t = {};
    t.a = mesh.triangles[i][0];
    t.b = mesh.triangles[i][1];
    t.c = mesh.triangles[i][2];
    t.material = 3106094762U;
    t.transformIndex = 0xFFFF;
  }
  // The factory returns the native compressed-mesh material wrapper. Its
  // SetReferencedObject installs userData and owns one Havok reference.
  RE::NiPointer<RE::bhkShape> wrapper(Factory<RE::bhkShape>(REL::RelocationID(77559, 79434)));
  wrapper->materialID = static_cast<RE::MATERIAL_ID>(3106094762U);
  wrapper->SetReferencedObject(shape);
  return wrapper;
}
} // namespace

void InitializeTerrainWeldingPolicy() {
  if (weldingPolicyInstalled)
    return;
  const auto version = REL::Module::get().version();
  if (version != REL::Version(1, 5, 97, 0) &&
      version != REL::Version(1, 6, 1170, 0) &&
      version != REL::Version(1, 7, 104, 0)) {
    spdlog::warn("Terrain welding policy is not verified for this runtime.");
    return;
  }
  const REL::Relocation<std::uintptr_t> target{REL::RelocationID(59626, 60310)};
  // Pinned offline verification covers the ABI and this function entry.
  // Decline instrumentation if another plugin has already replaced it.
  constexpr std::array<std::uint8_t, 7> entry{0x48, 0x8b, 0xc4, 0x48,
                                              0x89, 0x58, 0x08};
  if (std::memcmp(reinterpret_cast<void *>(target.address()), entry.data(),
                  entry.size())) {
    spdlog::warn("Terrain welding policy skipped: native entry differs.");
    return;
  }
  const auto initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
    spdlog::warn("Terrain welding policy: hook initialization failed.");
    return;
  }
  auto address = reinterpret_cast<void *>(target.address());
  if (MH_CreateHook(address,
                    reinterpret_cast<void *>(&ApplyTerrainWeldingPolicy),
                    reinterpret_cast<void **>(&originalWelding)) != MH_OK) {
    spdlog::warn("Terrain welding policy: hook creation failed.");
    return;
  }
  if (MH_EnableHook(address) != MH_OK) {
    MH_RemoveHook(address);
    spdlog::warn("Terrain welding policy: hook activation failed.");
    return;
  }
  weldingPolicyInstalled = true;
}

TerrainTextures CaptureTerrainTextures(RE::BSTriShape *geometry) {
  if (!geometry)
    throw std::runtime_error("Missing original landscape geometry");
  TerrainTextures result;
  auto &runtime = geometry->GetGeometryRuntimeData();
  result.property = RE::NiPointer<RE::BSLightingShaderProperty>(
      netimmerse_cast<RE::BSLightingShaderProperty *>(
          runtime.shaderProperty.get()));
  if (!result.property || !result.property->material)
    throw std::runtime_error("Missing original landscape material");
  const auto feature = result.property->material->GetFeature();
  if (feature != RE::BSShaderMaterial::Feature::kMultiTexLand &&
      feature != RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend)
    throw std::runtime_error("Unsupported original landscape shader feature");
  const auto renderer =
      static_cast<RE::BSGraphics::TriShape *>(runtime.rendererData);
  const auto &counts = geometry->GetTrishapeRuntimeData();
  if (!renderer || !renderer->rawVertexData || !counts.vertexCount ||
      counts.vertexCount > 4096 || !counts.triangleCount)
    throw std::runtime_error(
        "Original landscape CPU vertex/index data unavailable");
  const auto descriptor = renderer->vertexDesc;
  std::uint64_t bits{};
  std::memcpy(&bits, &descriptor, sizeof(bits));
  const auto stride = unsigned(bits & 15) * 4;
  using V = RE::BSGraphics::Vertex;
  const auto uv = descriptor.GetAttributeOffset(V::VA_TEXCOORD0);
  const auto color = descriptor.GetAttributeOffset(V::VA_COLOR);
  const auto land = descriptor.GetAttributeOffset(V::VA_LANDDATA);
  if (!descriptor.HasFlag(V::VF_VERTEX) || !descriptor.HasFlag(V::VF_UV) ||
      !descriptor.HasFlag(V::VF_COLORS) ||
      !descriptor.HasFlag(V::VF_LANDDATA) || stride < 16 || uv + 4 > stride ||
      color + 4 > stride || land + 8 > stride)
    throw std::runtime_error(
        "Unsupported original landscape vertex descriptor");
  // LAND transforms are translations. Keep its model-space origin so native
  // landBlendParams and LOD blending continue to use the original coordinates.
  const auto &transform = geometry->world;
  const auto &r = transform.rotate.entry;
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      if (std::abs(r[y][x] - (x == y ? 1.F : 0.F)) > 1e-5F)
        throw std::runtime_error("Unsupported rotated landscape geometry");
  if (std::abs(transform.scale - 1) > 1e-5F)
    throw std::runtime_error("Unsupported scaled landscape geometry");
  result.renderOrigin = {transform.translate.x, transform.translate.y,
                         transform.translate.z};
  result.surface.vertices.reserve(counts.vertexCount);
  for (unsigned i = 0; i < counts.vertexCount; ++i) {
    const auto data = renderer->rawVertexData + i * stride;
    LandscapeSample vertex;
    // SSE's input declaration always binds POSITION as R32G32B32A32_FLOAT,
    // including descriptors without VF_FULLPREC (AE 1.6.1170 RVA E4BF3D).
    std::memcpy(&vertex.position, data, sizeof(Vec));
    vertex.position = vertex.position + result.renderOrigin;
    std::array<std::uint16_t, 2> texcoord{};
    std::memcpy(texcoord.data(), data + uv, sizeof(texcoord));
    vertex.uv = {Unhalf(texcoord[0]), Unhalf(texcoord[1])};
    for (unsigned j = 0; j < 4; ++j)
      vertex.color[j] = data[color + j] / 255.F;
    for (unsigned j = 0; j < 8; ++j)
      vertex.blend[j] = data[land + j] / 255.F;
    result.surface.vertices.push_back(vertex);
  }
  const auto sourceIndices = LandscapeIndices(geometry, renderer);
  result.surface.triangles.resize(counts.triangleCount);
  std::memcpy(result.surface.triangles.data(), sourceIndices,
              counts.triangleCount * sizeof(result.surface.triangles.front()));
  result.surface.Prepare();
  return result;
}

struct NativeChunk::Data {
  Buffers cpu;
  BrushOverlay::Trace::Stamp overlayStamp;
  std::vector<LandscapeVertex> landscapeVertices;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices;
  RE::BSGraphics::TriShape renderer{};
  RE::NiPointer<RE::BSTriShape> geometry;
  RE::NiPointer<RE::NiNode> root;
  RE::NiPointer<RE::bhkShape> shape, meshShape;
  std::shared_ptr<SharedTerrainMaterial> sharedMaterial;
  RE::NiPointer<RE::bhkRigidBody> body;
  ~Data() {
    if (body)
      body->RemoveFromCurrentWorld();
    if (root && root->parent)
      root->parent->DetachChild(root.get());
    // Our CPU vectors and COM pointers own these buffers. The native geometry
    // must not pass our TriShape record to Skyrim's renderer allocation pool.
    if (geometry)
      geometry->GetGeometryRuntimeData().rendererData = nullptr;
    if (root)
      root->collisionObject.reset();
    root.reset();
    geometry.reset();
    sharedMaterial.reset();
    body.reset();
    shape.reset();
    meshShape.reset();
  }
};
NativeChunk::NativeChunk() : data(std::make_unique<Data>()) {}
NativeChunk::~NativeChunk() = default;
void NativeChunk::StampOverlay(std::uint32_t cell, unsigned chunk,
                               std::shared_ptr<const TerrainSurface> surface) {
  static std::atomic_uint64_t next{1};
  data->overlayStamp = {(std::uint64_t(cell) << 32) | chunk, next.fetch_add(1)};
  BrushOverlay::Trace::Tag(data->vertices.Get(), data->overlayStamp,
                           std::move(surface));
}
RE::NiNode *NativeChunk::Root() const { return data->root.get(); }
RE::bhkRigidBody *NativeChunk::Body() const { return data->body.get(); }

std::shared_ptr<NativeChunk>
NativeChunk::Create(const Mesh &mesh, Vec origin,
                    const TerrainTextures &textures) {
  auto chunk = std::shared_ptr<NativeChunk>(new NativeChunk());
  auto &d = *chunk->data;
  {
    const auto charts = ProjectLandscape(mesh);
    d.cpu = PackBuffers(charts.mesh, textures.renderOrigin);
    d.landscapeVertices =
        PackLandscape(charts.mesh, d.cpu, textures.surface, charts.axes);
    std::vector<PackedVertex>().swap(d.cpu.vertices);
  }
  {
    d.geometry = RE::NiPointer<RE::BSTriShape>(Factory<RE::BSTriShape>(REL::RelocationID(69284, 70655)));
    d.root = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(1));
    if (!d.root)
      throw std::bad_alloc();
  }
  {
    const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
      throw std::runtime_error("Terrain renderer is unavailable");
    const auto device =
        reinterpret_cast<ID3D11Device *>(renderer->GetRuntimeData().forwarder);
    if (!device)
      throw std::runtime_error("Terrain D3D device is unavailable");
    // ID3D11Device resource creation is re-entrant on the normal threaded
    // device. We never call the immediate context. Do not acquire Skyrim's
    // global render critical section from the player update: another render
    // thread may own it while waiting for this update to finish.
    const auto creationFlags = device->GetCreationFlags();

    if (creationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED)
      throw std::runtime_error("Terrain requires a thread-safe D3D11 device");
    D3D11_BUFFER_DESC desc{};
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.ByteWidth =
        static_cast<UINT>(d.landscapeVertices.size() * sizeof(LandscapeVertex));
    D3D11_SUBRESOURCE_DATA upload{};
    upload.pSysMem = d.landscapeVertices.data();
    auto result =
        device->CreateBuffer(&desc, &upload, d.vertices.GetAddressOf());
    if (FAILED(result))
      throw std::runtime_error(
          std::format("Terrain vertex upload failed ({:08X})",
                      static_cast<unsigned>(result)));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.ByteWidth = static_cast<UINT>(d.cpu.triangles.size() *
                                       sizeof(d.cpu.triangles.front()));
    upload.pSysMem = d.cpu.triangles.data();
    result = device->CreateBuffer(&desc, &upload, d.indices.GetAddressOf());
    if (FAILED(result))
      throw std::runtime_error(
          std::format("Terrain index upload failed ({:08X})",
                      static_cast<unsigned>(result)));
  }
  {
    std::memcpy(&d.renderer.vertexDesc, &LandscapeVertexDescriptor,
                sizeof(LandscapeVertexDescriptor));
    d.renderer.vertexBuffer =
        reinterpret_cast<REX::W32::ID3D11Buffer *>(d.vertices.Get());
    d.renderer.indexBuffer =
        reinterpret_cast<REX::W32::ID3D11Buffer *>(d.indices.Get());
    d.renderer.refCount = 1;
    d.renderer.rawVertexData =
        reinterpret_cast<std::uint8_t *>(d.landscapeVertices.data());
    d.renderer.rawIndexData =
        reinterpret_cast<std::uint16_t *>(d.cpu.triangles.data());
    auto &geometry = d.geometry->GetGeometryRuntimeData();
    geometry.vertexDesc = d.renderer.vertexDesc;
    geometry.rendererData = &d.renderer;
    d.geometry->GetTrishapeRuntimeData().vertexCount =
        static_cast<std::uint16_t>(d.landscapeVertices.size());
    d.geometry->GetTrishapeRuntimeData().triangleCount =
        static_cast<std::uint16_t>(d.cpu.triangles.size());
    d.geometry->GetModelData().modelBound.center = {
        d.cpu.center.x, d.cpu.center.y, d.cpu.center.z};
    d.geometry->GetModelData().modelBound.radius = d.cpu.radius;
  }
  {
    RE::NiPointer<RE::BSLightingShaderProperty> property(
        RE::BSLightingShaderProperty::Create());
    if (!property)
      throw std::bad_alloc();
    d.sharedMaterial = AcquireMaterial(textures);
    RetainTerrainMaterial(*property, d.sharedMaterial->keeper->material);
    const auto bound =
        static_cast<RE::BSLightingShaderMaterialBase *>(property->material);
    if (bound != d.sharedMaterial->keeper->material)
      throw std::runtime_error(
          "Terrain property did not retain its shared material");
    property->CopyMembers(textures.property.get());
    d.geometry->GetGeometryRuntimeData().shaderProperty =
        RE::NiPointer<RE::BSShaderProperty>(property.get());
  }
  {
    const auto property = static_cast<RE::BSLightingShaderProperty *>(
        d.geometry->GetGeometryRuntimeData().shaderProperty.get());
    if (!property->SetupGeometry(d.geometry.get()) ||
        !property->FinishSetupGeometry(d.geometry.get()))
      throw std::runtime_error("Terrain material setup failed");
    if (property->material != d.sharedMaterial->keeper->material)
      throw std::runtime_error(
          "Terrain setup changed shared material identity");
    d.geometry->local.translate = {textures.renderOrigin.x - origin.x,
                                   textures.renderOrigin.y - origin.y,
                                   textures.renderOrigin.z - origin.z};
    d.geometry->SetMaterialNeedsUpdate(true);
    d.root->AttachChild(d.geometry.get(), true);
  }
  return chunk;
}
std::shared_ptr<NativeChunk>
NativeChunk::CreateCollision(const Mesh &mesh, Vec origin,
                             RE::bhkRigidBody *sourceBody) {
  if (!sourceBody || !sourceBody->GetRigidBody())
    throw std::runtime_error("Missing terrain collision template");
  auto chunk = std::shared_ptr<NativeChunk>(new NativeChunk());
  auto &d = *chunk->data;
  d.root = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(0));
  if (!d.root)
    throw std::bad_alloc();
  {
    d.meshShape = CollisionShape(mesh, origin);
    d.shape = AccelerateCollision(d.meshShape);
  }
  // Clone the native fixed body, not the source scene graph or its render data.
  // It remains detached while replacing its shape, pose and filter.
  RE::hkpRigidBody *native{};
  {
    native = sourceBody->GetRigidBody()->Clone();
  }
  if (!native || native == sourceBody->GetRigidBody())
    throw std::runtime_error("Could not clone fixed terrain body");
  RE::hkRefPtr<RE::hkpRigidBody> owned(native);
  native->RemoveReference();
  {
    auto bodyMemory = RE::malloc<RE::bhkRigidBody>();
    if (!bodyMemory)
      throw std::bad_alloc();
    const REL::Relocation<RE::bhkRigidBody *(*)(RE::bhkRigidBody *)> bodyCtor{
        REL::RelocationID(76315, 78147)};
    d.body = RE::NiPointer<RE::bhkRigidBody>(bodyCtor(bodyMemory));
    d.body->SetReferencedObject(native);
  }
  {
    native->SetShape(
        static_cast<RE::hkpShape *>(d.shape->referencedObject.get()));
    if (native->GetShape() != d.shape->referencedObject.get())
      throw std::runtime_error("Terrain collision shape replacement failed");
    // Keep this a ground surface at both the body and triangle-filter levels.
    // Do not inherit heightfield-specific penetration tolerances into a thin
    // triangle shell: the character must find a supporting contact at its face.
    native->collidable.broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(
        RE::COL_LAYER::kGround);

    native->material.SetResponseType(
        RE::hkpMaterial::ResponseType::kSimpleContact);
    native->collidable.allowedPenetrationDepth = .01F;
  }
  {
    auto collisionMemory = RE::malloc<RE::bhkCollisionObject>();
    if (!collisionMemory)
      throw std::bad_alloc();
    const REL::Relocation<RE::bhkCollisionObject *(*)(RE::bhkCollisionObject *)>
        collisionCtor{REL::RelocationID(76530, 78372)};
    auto collision = collisionCtor(collisionMemory);
    RE::stl::emplace_vtable<RE::bhkCollisionObject>(collision);
    collision->sceneObject = d.root.get();
    collision->flags.reset(RE::bhkNiCollisionObject::Flag::kSyncOnUpdate);
    collision->body = RE::NiPointer<RE::bhkWorldObject>(d.body.get());
    d.root->collisionObject = RE::NiPointer<RE::NiCollisionObject>(collision);
  }
  return chunk;
}
void NativeChunk::Attach(RE::NiNode *parent, RE::bhkWorld *world, Vec origin) {
  RE::NiTransform pose;
  pose.translate = {origin.x, origin.y, origin.z};
  data->root->local = parent->world.Invert() * pose;
  parent->AttachChild(data->root.get(), true);
  RE::NiUpdateData update{};
  update.flags.set(RE::NiUpdateData::Flag::kDirty);
  data->root->Update(update);
  RE::hkVector4 position;
  position = pose.translate * RE::bhkWorld::GetWorldScale();
  RE::hkQuaternion rotation;
  rotation.vec.quad = _mm_set_ps(1, 0, 0, 0);
  if (data->body) {
    data->body->SetPositionAndRotation(position, rotation);
    data->body->MoveToWorld(world);
    if (!data->body->GetWorld1())
      throw std::runtime_error("Terrain collision publication failed");
  }
}
void NativeChunk::Detach() {
  if (data->body)
    data->body->RemoveFromCurrentWorld();
  if (data->root) {
    data->root->SetAppCulled(true);
    if (data->root->parent)
      data->root->parent->DetachChild(data->root.get());
  }
}
} // namespace SkyBuild::Terrain
