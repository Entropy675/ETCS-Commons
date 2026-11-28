#include "GeometryProvider.h"

// Geometry reference types for the rest of the runtime (Kart props, visual
// landmarks, soft obstacles). Grain/GrainSet participate in the existing
// Causal observation edge: cascades are lazy-resolved and memoized under
// the subtree hashes ETCS already maintains (XXH3 below type-root
// boundaries). Angle of repose is a material value the resolve step
// consults; no new ontology trait is required.
ETCS_MODULE_EXPORT_MAIN(GeometryProvider, "Seashell Grain GrainSet Pyramid")

ETCS_TAG_BLOCK_BASIC(Seashell,
    Create, SetParams, SetPose, Sample, Mesh, Delete)

ETCS_TAG_BLOCK_BASIC(Grain,
    Create, SetMaterial, Delete)

ETCS_TAG_BLOCK_BASIC(GrainSet,
    Create, AddGrain, SpawnGrain, SetRepose, Observe, Sample, Delete)

ETCS_TAG_BLOCK_BASIC(Pyramid,
    Create, SetFractal, PathOnFace, PathEdges, Sample, Mesh, Delete)
