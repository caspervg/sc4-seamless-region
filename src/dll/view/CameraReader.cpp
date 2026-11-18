#include "view/CameraReader.hpp"

#include "core/GameAccess.hpp"
#include "core/GameAddresses.hpp"

namespace
{
    namespace CC = GameAddresses::CameraControl;
    namespace Cam = GameAddresses::S3DCamera;

    cS3DVector3 ReadVec3(const void* object, const uint32_t offset)
    {
        return GameAccess::ReadField<cS3DVector3>(object, offset);
    }
}

bool CameraReader::Update()
{
    const void* control = GameAccess::CameraControl();
    const void* camera = GameAccess::ReadField<const void*>(control, CC::kS3DCamera);
    if (!control || !camera) {
        frame_.valid = false;
        return false;
    }

    CameraFrame& f = frame_;
    f.zoomIndex = GameAccess::ReadField<int32_t>(control, CC::kZoom);
    f.rotation = GameAccess::ReadField<int32_t>(control, CC::kRotation);
    if (!ViewMath::IsValidZoom(f.zoomIndex) || !ViewMath::IsValidRotation(f.rotation)) {
        f.valid = false;
        return false;
    }
    f.magnification = GameAccess::ReadField<float>(control, CC::kMagnification);
    f.pixelsPerMetre = GameAccess::ReadField<float>(control, CC::kPixelsPerWorldUnit);
    f.metresPerPixel = GameAccess::ReadField<float>(control, CC::kWorldUnitsPerPixel);
    f.pitchRadians = GameAccess::ReadField<float>(control, CC::kPitch);
    f.yawOffsetRadians = GameAccess::ReadField<float>(control, CC::kYawOffset);
    f.viewportWidth = GameAccess::ReadField<int32_t>(control, CC::kViewportWidth);
    f.viewportHeight = GameAccess::ReadField<int32_t>(control, CC::kViewportHeight);
    f.target = ReadVec3(control, CC::kTargetPosition);
    f.eye = ReadVec3(control, CC::kEyePosition);
    f.baseCorner = ReadVec3(control, CC::kBaseTargetPosition);
    f.citySizeX = GameAccess::ReadField<float>(control, CC::kCitySizeX);
    f.citySizeZ = GameAccess::ReadField<float>(control, CC::kCitySizeZ);
    f.liveRight = ReadVec3(control, CC::kViewRight);
    f.liveUp = ReadVec3(control, CC::kViewUp);
    f.liveForward = ReadVec3(control, CC::kViewForward);
    f.positionTweak = {GameAccess::ReadField<float>(control, CC::kPositionTweakX),
                       GameAccess::ReadField<float>(control, CC::kPositionTweakY)};
    f.scrollClampEnabled = GameAccess::ReadField<uint8_t>(control, CC::kScrollClampEnabled) != 0;
    f.viewportOffset = {GameAccess::ReadField<float>(camera, Cam::kViewportOffsetX),
                        GameAccess::ReadField<float>(camera, Cam::kViewportOffsetY)};

    // OrthoProjection::For allocates nothing and costs a few trig calls. Only rebuild it when
    // the view changes.
    if (f.projection.pixelsPerMetre == 0.0f || f.projection.zoomIndex != f.zoomIndex ||
        f.projection.rotation != f.rotation || f.projection.magnification != f.magnification) {
        f.projection = ViewMath::OrthoProjection::For(f.zoomIndex, f.rotation, f.magnification);
    }

    // cS3DCamera::Project with the ortho volume UpdateCameraZoomAndRotationParams sets:
    //   screen.x = W/2 + canvas.x(p) - canvas.x(target) - offset.x
    //   screen.y = H/2 + canvas.y(p) - canvas.y(target) + offset.y
    const cS3DVector2 ct = f.projection.WorldToCanvas(f.target);
    f.canvasToScreen = {static_cast<float>(f.viewportWidth) * 0.5f - f.viewportOffset.fX - ct.fX,
                        static_cast<float>(f.viewportHeight) * 0.5f + f.viewportOffset.fY - ct.fY};
    f.valid = f.viewportWidth > 0 && f.viewportHeight > 0 && f.pixelsPerMetre > 0.0f;
    return f.valid;
}

bool CameraReader::WorldToScreen(const cS3DVector3& world, cS3DVector2& screen) const
{
    if (!frame_.valid) {
        return false;
    }
    const cS3DVector2 c = frame_.projection.WorldToCanvas(world);
    screen = {c.fX + frame_.canvasToScreen.fX, c.fY + frame_.canvasToScreen.fY};
    return true;
}

bool CameraReader::ScreenToGround(const cS3DVector2& screen, const float groundHeight, cS3DVector3& world) const
{
    if (!frame_.valid) {
        return false;
    }
    const cS3DVector2 canvas{screen.fX - frame_.canvasToScreen.fX, screen.fY - frame_.canvasToScreen.fY};
    world = frame_.projection.CanvasToGround(canvas, groundHeight);
    return true;
}

void CameraReader::VisibleCanvas(cS3DVector2& canvasMin, cS3DVector2& canvasMax) const
{
    canvasMin = {-frame_.canvasToScreen.fX, -frame_.canvasToScreen.fY};
    canvasMax = {canvasMin.fX + static_cast<float>(frame_.viewportWidth),
                 canvasMin.fY + static_cast<float>(frame_.viewportHeight)};
}
