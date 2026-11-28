#ifndef GEOMETRYPROVIDER_H__
#define GEOMETRYPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_GeometryProvider.h"

// ── Seashell ────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Seashell, Create)
{
    (void)ctx;
    double a = 0, b = 0, turns = 0;
    data >> a >> b >> turns;
    data.writeString(self.Create(a, b, turns) ? "ok" : "fail");
}
DEFINE_WORK_FUNC(Seashell, SetParams)
{
    (void)ctx;
    double a = 0, b = 0, turns = 0;
    data >> a >> b >> turns;
    data.writeString(self.SetParams(a, b, turns) ? "ok" : "fail");
}
DEFINE_WORK_FUNC(Seashell, SetPose)
{
    (void)ctx;
    double x=0,y=0,z=0, qw=1,qx=0,qy=0,qz=0;
    data >> x >> y >> z >> qw >> qx >> qy >> qz;
    self.SetPose(x, y, z, qw, qx, qy, qz);
    data.writeString("ok");
}
DEFINE_WORK_FUNC(Seashell, Sample)
{
    (void)ctx;
    int n = 64;
    data >> n;
    data.writeString(self.Sample(n).c_str());
}
DEFINE_WORK_FUNC(Seashell, Mesh)
{
    (void)ctx;
    data.writeString(self.Mesh().c_str());
}
DEFINE_WORK_FUNC(Seashell, Delete)
{
    (void)ctx;
    self.Delete();
    data.writeString("ok");
}

// ── Grain ───────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Grain, Create)
{
    (void)ctx;
    double r = 0.05;
    data >> r;
    data.writeString(self.Create(r) ? "ok" : "fail");
}
DEFINE_WORK_FUNC(Grain, SetMaterial)
{
    (void)ctx;
    double repose = 30.0, friction = 0.5;
    data >> repose >> friction;
    data.writeString(self.SetMaterial(repose, friction) ? "ok" : "fail");
}
DEFINE_WORK_FUNC(Grain, Delete)
{
    (void)ctx;
    self.Delete();
    data.writeString("ok");
}

// ── GrainSet ────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(GrainSet, Create)
{
    (void)ctx;
    data.writeString(self.Create() ? "ok" : "fail");
}
DEFINE_WORK_FUNC(GrainSet, AddGrain)
{
    (void)ctx;
    ETCS::RID g = 0;
    data >> g;
    data.writeString(self.AddGrain(g) ? "ok" : "fail");
}
DEFINE_WORK_FUNC(GrainSet, SpawnGrain)
{
    (void)ctx;
    double x=0,y=0,z=0,r=0.05;
    data >> x >> y >> z >> r;
    data.writeString(self.SpawnGrain(x, y, z, r).c_str());
}
DEFINE_WORK_FUNC(GrainSet, SetRepose)
{
    (void)ctx;
    double deg = 30.0;
    data >> deg;
    self.SetRepose(deg);
    data.writeString("ok");
}
// Observe -- trigger (or re-trigger) the lazy cascade resolve on the
// Causal observation edge. Returns a short status + the memo hash.
DEFINE_WORK_FUNC(GrainSet, Observe)
{
    (void)ctx;
    data.writeString(self.Observe().c_str());
}
DEFINE_WORK_FUNC(GrainSet, Sample)
{
    (void)ctx;
    data.writeString(self.Sample().c_str());
}
DEFINE_WORK_FUNC(GrainSet, Delete)
{
    (void)ctx;
    self.Delete();
    data.writeString("ok");
}

// ── Pyramid ─────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Pyramid, Create)
{
    (void)ctx;
    double base = 1.0, height = 1.0;
    int sides = 4;
    data >> base >> height >> sides;
    data.writeString(self.Create(base, height, sides) ? "ok" : "fail");
}
DEFINE_WORK_FUNC(Pyramid, SetFractal)
{
    (void)ctx;
    int depth = 0;
    double scale = 0.5;
    data >> depth >> scale;
    self.SetFractal(depth, scale);
    data.writeString("ok");
}
DEFINE_WORK_FUNC(Pyramid, PathOnFace)
{
    (void)ctx;
    int face = 0;
    double t = 0.0;
    data >> face >> t;
    data.writeString(self.PathOnFace(face, t).c_str());
}
DEFINE_WORK_FUNC(Pyramid, PathEdges)
{
    (void)ctx;
    data.writeString(self.PathEdges().c_str());
}
DEFINE_WORK_FUNC(Pyramid, Sample)
{
    (void)ctx;
    int n = 32;
    data >> n;
    data.writeString(self.Sample(n).c_str());
}
DEFINE_WORK_FUNC(Pyramid, Mesh)
{
    (void)ctx;
    data.writeString(self.Mesh().c_str());
}
DEFINE_WORK_FUNC(Pyramid, Delete)
{
    (void)ctx;
    self.Delete();
    data.writeString("ok");
}

#endif // GEOMETRYPROVIDER_H__
