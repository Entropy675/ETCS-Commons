#include "GeometryProvider.h"

// Geometry reference types. Pattern matches RenderProvider::Mesh: claim
// Deletable, WIRE_TYPE_IDENTITY, expose Sample/Mesh (and material values).
// Not Drawable3D/Causal leaves yet -- those require the full *Concrete
// surface; these are spawnable parameter bags the rest of the runtime
// can bind the way Kart binds burst meshes.
ETCS_MODULE_EXPORT_MAIN(GeometryProvider, "Seashell Grain GrainSet Pyramid")

ETCS_TAG_BLOCK_BASIC(Seashell,
    Create, SetParams, SetPose, Sample, Mesh, Delete)

ETCS_TAG_BLOCK_BASIC(Grain,
    Create, SetMaterial, Delete)

ETCS_TAG_BLOCK_BASIC(GrainSet,
    Create, AddGrain, SpawnGrain, SetRepose, Observe, Sample, Delete)

ETCS_TAG_BLOCK_BASIC(Pyramid,
    Create, SetFractal, SetPose, PathOnFace, PathEdges, Sample, Mesh, Delete)
