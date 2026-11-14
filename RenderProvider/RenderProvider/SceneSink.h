#ifndef RENDERPROVIDER_SCENESINK_H__
#define RENDERPROVIDER_SCENESINK_H__

#include "DeviceFrame.h"

/*
 * WHERE A DEVICE PROJECTION LANDS: the camera, as the thing that collects a
 * scene's mesh ops for the device to draw (DeviceFrame.h, DeviceScene).
 *
 * A camera on the device path does not receive pixels or rectangle runs
 * from the scene; it receives the scene -- a view, a lens, one op per node
 * -- and hands that to the surface it is blitted into, which hands it to
 * the device. This is the seam between the two, and it is this module's
 * own rather than a family: nothing outside RenderProvider names it, and a
 * camera reaches it by registering the pointer under this name, the same
 * mechanism the ontology uses for its families and the one that keeps a
 * cast out of the scene's code (Scene3D.h looks it up, never casts a
 * Camera_ to a Camera3D).
 */
class SceneSink
{
public:
    virtual ~SceneSink() = default;

    // A new projection starts: everything before it is dropped.
    virtual void BeginScene(const float view[16], const float proj[16]) = 0;
    // One node.
    virtual void AddMesh(const DeviceMeshOp& op) = 0;
    // The surface's half: the recording since the last take, or false when
    // nothing was projected since -- a still scene is not re-sent.
    virtual bool TakeScene(DeviceScene& out) = 0;
};

static constexpr const char* RP_SCENE_SINK = "SceneSink";

/*
 * THE OTHER END: a surface that can take a scene -- one drawing through a
 * device right now. A camera asks its destination this every time it is
 * drawn into it (Camera3D::DrawIntoConcrete), because a window surface
 * moves on and off its device by itself (HostSurface chooses its sink at
 * each Present), and a device projection blitted into host pixels is a
 * blank frame. The answer is what ontology/Camera.h calls
 * DeviceProjectionLands: derived at the draw, never a stored mode.
 */
class SceneTaker
{
public:
    virtual ~SceneTaker() = default;
    virtual bool TakesScenes() const = 0;
};

static constexpr const char* RP_SCENE_TAKER = "SceneTaker";

#endif // RENDERPROVIDER_SCENESINK_H__
