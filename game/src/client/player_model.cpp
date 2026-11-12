#include "player_model.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vr {
namespace {

using vxe::f32;
using vxe::f64;
using vxe::Mat4;
using vxe::usize;
using vxe::Vec3;
using vxe::Vec4;
using vxe::render::EntityVertex;

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr f32 kSkin = 64.0F;           // Skin texture size in pixels.
constexpr f32 kPixel = 1.0F / 16.0F;   // Model pixels per block.
constexpr f32 kWalkSpeed = 4.3F;       // Blocks a second where the walk swing is full.
constexpr f32 kStridePerBlock = 2.66F; // Radians of leg swing per block (Minecraft's 0.6662 x 4).
constexpr f32 kBodyLag = 0.87F;        // Radians (50 degrees) the head turns before the body follows.
constexpr f32 kSwingSeconds = 0.3F;    // Six ticks, as Minecraft's.
constexpr f32 kDarkest = 0.08F;        // Brightness in total darkness.
// First person, in the view's space (blocks; x right, y up, z backwards):
// where the shoulder is, how the arm points (radians: turned left towards
// the middle, raised forward from hanging, rolled to show its outer side),
// the middle of the hand's end (arm pixels), how far above it in the view the
// item rests, and the item's size. With a 70 degree view the hand ends about
// half way out to the right and two thirds down, the item just above it.
const Vec3 kFirstPersonShoulder(0.58F, -0.5F, -0.2F);
constexpr f32 kFirstPersonArmYaw = 0.21F;   // 12 degrees.
constexpr f32 kFirstPersonArmPitch = 1.83F; // 105 degrees.
constexpr f32 kFirstPersonArmRoll = -0.35F; // -20 degrees.
const Vec3 kFirstPersonHandEnd(1.0F, -10.0F, 0.0F);
const Vec3 kFirstPersonItemLift(0.0F, 0.15F, -0.02F);
constexpr f32 kFirstPersonBlockSize = 0.24F;
constexpr f32 kFirstPersonItemTurn = 15.0F; // Degrees around the view's up.
constexpr f32 kFirstPersonItemTip = 18.0F;  // Degrees, the top towards the eye.

// Brightness of a face by where it points, like the blocks: tops brightest,
// sides facing east or west darker than north or south, bottoms darkest.
f32 faceShade(const Vec3& normal) {
    const f32 across = std::abs(normal.x) + std::abs(normal.z);
    const f32 side = across > 1.0e-4F ? 0.6F + (0.2F * std::abs(normal.z) / across) : 0.8F;
    return normal.y >= 0.0F ? side + ((1.0F - side) * normal.y) : side + ((0.5F - side) * -normal.y);
}

struct Corner {
    Vec3 position; // Box local, in pixels.
    f32 s;         // Skin pixels, or 0 to 1 for item faces.
    f32 t;
};

// One face: four corners counterclockwise from outside, as two triangles.
void addFace(std::vector<EntityVertex>& out, const Mat4& transform, const std::array<Corner, 4>& corners,
             const Vec3& normal, f32 light, f32 layer, f32 uvScale) {
    const Vec3 turned = glm::normalize(Vec3(transform * Vec4(normal, 0.0F)));
    const f32 brightness = light * faceShade(turned);
    const auto vertex = [&](const Corner& corner) {
        const Vec3 at(transform * Vec4(corner.position, 1.0F));
        return EntityVertex{.position = Vec4(at, brightness),
                            .uv = Vec4(corner.s / uvScale, corner.t / uvScale, layer, 0.0F)};
    };
    for (const usize index : {0U, 1U, 2U, 0U, 2U, 3U}) {
        out.push_back(vertex(corners.at(index)));
    }
}

// A box of the skin: `from` and `size` in pixels, its texture at (u, v) in
// Minecraft's unwrap of a w x h x d box. `inflate` grows it (second layer)
// without changing where its texture comes from.
void addSkinBox(std::vector<EntityVertex>& out, const Mat4& transform, const Vec3& from, const Vec3& size,
                f32 u, f32 v, f32 inflate, f32 light) {
    const f32 w = size.x;
    const f32 h = size.y;
    const f32 d = size.z;
    const Vec3 lo = from - Vec3(inflate);
    const Vec3 hi = from + size + Vec3(inflate);
    const f32 x0 = lo.x;
    const f32 y0 = lo.y;
    const f32 z0 = lo.z;
    const f32 x1 = hi.x;
    const f32 y1 = hi.y;
    const f32 z1 = hi.z;
    // Front (-Z): the picture's left is the player's right (+X).
    f32 s0 = u + d;
    f32 t0 = v + d;
    addFace(out, transform,
            {{{{x1, y1, z0}, s0, t0},
              {{x0, y1, z0}, s0 + w, t0},
              {{x0, y0, z0}, s0 + w, t0 + h},
              {{x1, y0, z0}, s0, t0 + h}}},
            {0.0F, 0.0F, -1.0F}, light, 0.0F, kSkin);
    // Back (+Z).
    s0 = u + d + w + d;
    addFace(out, transform,
            {{{{x0, y1, z1}, s0, t0},
              {{x1, y1, z1}, s0 + w, t0},
              {{x1, y0, z1}, s0 + w, t0 + h},
              {{x0, y0, z1}, s0, t0 + h}}},
            {0.0F, 0.0F, 1.0F}, light, 0.0F, kSkin);
    // The player's right side (+X): the picture runs from the back to the front.
    s0 = u;
    addFace(out, transform,
            {{{{x1, y1, z1}, s0, t0},
              {{x1, y1, z0}, s0 + d, t0},
              {{x1, y0, z0}, s0 + d, t0 + h},
              {{x1, y0, z1}, s0, t0 + h}}},
            {1.0F, 0.0F, 0.0F}, light, 0.0F, kSkin);
    // The left side (-X): from the front to the back.
    s0 = u + d + w;
    addFace(out, transform,
            {{{{x0, y1, z0}, s0, t0},
              {{x0, y1, z1}, s0 + d, t0},
              {{x0, y0, z1}, s0 + d, t0 + h},
              {{x0, y0, z0}, s0, t0 + h}}},
            {-1.0F, 0.0F, 0.0F}, light, 0.0F, kSkin);
    // Top: its lower edge in the picture meets the front.
    s0 = u + d;
    t0 = v;
    addFace(out, transform,
            {{{{x1, y1, z1}, s0, t0},
              {{x0, y1, z1}, s0 + w, t0},
              {{x0, y1, z0}, s0 + w, t0 + d},
              {{x1, y1, z0}, s0, t0 + d}}},
            {0.0F, 1.0F, 0.0F}, light, 0.0F, kSkin);
    // Bottom.
    s0 = u + d + w;
    addFace(out, transform,
            {{{{x1, y0, z0}, s0, t0},
              {{x0, y0, z0}, s0 + w, t0},
              {{x0, y0, z1}, s0 + w, t0 + d},
              {{x1, y0, z1}, s0, t0 + d}}},
            {0.0F, -1.0F, 0.0F}, light, 0.0F, kSkin);
}

// The held block: a cube of `size` pixels centered at the origin, each face
// its own item layer (+X, -X, +Y, -Y, +Z, -Z).
void addItemCube(std::vector<EntityVertex>& out, const Mat4& transform, f32 size, f32 light) {
    const f32 h = size * 0.5F;
    const auto face = [&](usize index, const std::array<Vec3, 4>& at, const Vec3& normal) {
        const auto layer = static_cast<f32>(vxe::render::kEntityItemLayer + index);
        addFace(out, transform,
                {{{at[0], 0.0F, 0.0F}, {at[1], 1.0F, 0.0F}, {at[2], 1.0F, 1.0F}, {at[3], 0.0F, 1.0F}}},
                normal, light, layer, 1.0F);
    };
    face(0, {{{h, h, h}, {h, h, -h}, {h, -h, -h}, {h, -h, h}}}, {1.0F, 0.0F, 0.0F});
    face(1, {{{-h, h, -h}, {-h, h, h}, {-h, -h, h}, {-h, -h, -h}}}, {-1.0F, 0.0F, 0.0F});
    face(2, {{{-h, h, -h}, {h, h, -h}, {h, h, h}, {-h, h, h}}}, {0.0F, 1.0F, 0.0F});
    face(3, {{{-h, -h, h}, {h, -h, h}, {h, -h, -h}, {-h, -h, -h}}}, {0.0F, -1.0F, 0.0F});
    face(4, {{{-h, h, h}, {h, h, h}, {h, -h, h}, {-h, -h, h}}}, {0.0F, 0.0F, 1.0F});
    face(5, {{{h, h, -h}, {-h, h, -h}, {-h, -h, -h}, {h, -h, -h}}}, {0.0F, 0.0F, -1.0F});
}

// A flat held item (a plant): one picture, seen from both sides.
void addItemFlat(std::vector<EntityVertex>& out, const Mat4& transform, f32 size, f32 light) {
    const f32 h = size * 0.5F;
    const auto layer = static_cast<f32>(vxe::render::kEntityItemLayer);
    addFace(out, transform,
            {{{{-h, h, 0.0F}, 0.0F, 0.0F},
              {{h, h, 0.0F}, 1.0F, 0.0F},
              {{h, -h, 0.0F}, 1.0F, 1.0F},
              {{-h, -h, 0.0F}, 0.0F, 1.0F}}},
            {0.0F, 0.0F, 1.0F}, light, layer, 1.0F);
}

// The facing angle of a view's yaw: the model faces -Z turned by this
// angle around +Y, so it looks where the camera at that yaw looks.
f32 facingOf(f32 yaw) {
    vxe::render::Camera camera;
    camera.yaw = yaw;
    camera.pitch = 0.0F;
    const Vec3 forward = camera.forward();
    return std::atan2(-forward.x, -forward.z);
}

f32 wrapAngle(f32 angle) {
    while (angle > kPi) {
        angle -= 2.0F * kPi;
    }
    while (angle < -kPi) {
        angle += 2.0F * kPi;
    }
    return angle;
}

// A part's turn around its pivot, in Minecraft's order (z, then y, then x).
Mat4 partTransform(const Vec3& pivot, const Vec3& rotation) {
    Mat4 m = glm::translate(Mat4(1.0F), pivot);
    m = glm::rotate(m, rotation.z, Vec3(0.0F, 0.0F, 1.0F));
    m = glm::rotate(m, rotation.y, Vec3(0.0F, 1.0F, 0.0F));
    return glm::rotate(m, rotation.x, Vec3(1.0F, 0.0F, 0.0F));
}

f32 brightnessOf(f32 light) {
    return kDarkest + ((1.0F - kDarkest) * std::clamp(light, 0.0F, 1.0F));
}

} // namespace

void PlayerModel::update(const PlayerPose& pose, f64 deltaSeconds) {
    const auto dt = static_cast<f32>(deltaSeconds);
    const f32 headFacing = facingOf(pose.headYaw);
    if (!m_started) {
        m_started = true;
        m_lastFeet = pose.feet;
        m_bodyYaw = headFacing;
        m_walkAmount = 0.0F;
    }
    const vxe::DVec3 moved = pose.feet - m_lastFeet;
    m_lastFeet = pose.feet;
    auto across = static_cast<f32>(std::hypot(moved.x, moved.z));
    if (across > 4.0F) {
        across = 0.0F; // A teleport, not a step.
    }
    const f32 speed = dt > 0.0F ? across / dt : 0.0F;
    const f32 target = std::clamp(speed / kWalkSpeed, 0.0F, 1.0F);
    m_walkAmount += (target - m_walkAmount) * std::min(dt * 10.0F, 1.0F);
    m_walkPhase = std::fmod(m_walkPhase + (across * kStridePerBlock), 2.0F * kPi * 64.0F);
    m_age += dt;
    // The body follows the head while moving, and at rest once the head has
    // turned past the lag.
    const f32 turn = wrapAngle(headFacing - m_bodyYaw);
    if (speed > 0.5F) {
        m_bodyYaw = wrapAngle(m_bodyYaw + (turn * std::min(dt * 8.0F, 1.0F)));
    } else if (std::abs(turn) > kBodyLag) {
        m_bodyYaw = wrapAngle(headFacing - std::copysign(kBodyLag, turn));
    }
    if (m_swinging) {
        m_swing += dt / kSwingSeconds;
        if (m_swing >= 1.0F) {
            m_swing = 1.0F;
            m_swinging = false;
        }
    }
}

void PlayerModel::buildBody(const PlayerPose& pose, const vxe::render::Camera& camera,
                            std::vector<EntityVertex>& out) const {
    const f32 light = brightnessOf(pose.light);
    const Vec3 feet(pose.feet - camera.position);
    Mat4 base = glm::translate(Mat4(1.0F), feet);
    base = glm::rotate(base, m_bodyYaw, Vec3(0.0F, 1.0F, 0.0F));
    base = glm::scale(base, Vec3(kPixel));

    const f32 phase = m_walkPhase;
    const f32 amount = m_walkAmount;
    Vec3 head(std::clamp(pose.pitch, -1.5F, 1.5F), wrapAngle(facingOf(pose.headYaw) - m_bodyYaw), 0.0F);
    Vec3 body(0.0F);
    Vec3 rightArm(std::cos(phase + kPi) * amount, 0.0F, 0.0F);
    Vec3 leftArm(std::cos(phase) * amount, 0.0F, 0.0F);
    Vec3 rightLeg(std::cos(phase) * 1.4F * amount, 0.0F, 0.0F);
    Vec3 leftLeg(std::cos(phase + kPi) * 1.4F * amount, 0.0F, 0.0F);
    // Arms sway a little even at rest.
    const f32 sway = (std::cos(m_age * 1.8F) * 0.05F) + 0.05F;
    rightArm.z += sway;
    leftArm.z -= sway;
    rightArm.x += std::sin(m_age * 1.34F) * 0.05F;
    leftArm.x -= std::sin(m_age * 1.34F) * 0.05F;
    if (pose.held != HeldItem::None) {
        rightArm.x = (rightArm.x * 0.5F) + (kPi / 10.0F); // Held up a little.
    }
    if (m_swinging) {
        const f32 f = m_swing;
        const f32 eased = 1.0F - std::pow(1.0F - f, 4.0F);
        body.y = std::sin(std::sqrt(f) * 2.0F * kPi) * 0.2F;
        rightArm.x += (std::sin(eased * kPi) * 1.2F) + (std::sin(f * kPi) * (head.x + 0.7F) * 0.75F);
        rightArm.y += body.y * 2.0F;
        rightArm.z += std::sin(f * kPi) * 0.4F;
    }
    Vec3 headPivot(0.0F, 24.0F, 0.0F);
    Vec3 bodyPivot(0.0F, 24.0F, 0.0F);
    Vec3 rightArmPivot(5.0F, 22.0F, 0.0F);
    Vec3 leftArmPivot(-5.0F, 22.0F, 0.0F);
    Vec3 rightLegPivot(1.9F, 12.0F, 0.0F);
    Vec3 leftLegPivot(-1.9F, 12.0F, 0.0F);
    if (pose.crouching && !pose.flying) {
        body.x = -0.5F; // The back bends forwards.
        rightArm.x += 0.4F;
        leftArm.x += 0.4F;
        headPivot += Vec3(0.0F, -4.2F, -1.0F);
        bodyPivot += Vec3(0.0F, -3.2F, 0.0F);
        rightArmPivot += Vec3(0.0F, -3.2F, -1.0F);
        leftArmPivot += Vec3(0.0F, -3.2F, -1.0F);
        rightLegPivot += Vec3(0.0F, 0.0F, 4.0F);
        leftLegPivot += Vec3(0.0F, 0.0F, 4.0F);
    }
    const Mat4 headM = base * partTransform(headPivot, head);
    addSkinBox(out, headM, {-4.0F, 0.0F, -4.0F}, {8.0F, 8.0F, 8.0F}, 0.0F, 0.0F, 0.0F, light);
    addSkinBox(out, headM, {-4.0F, 0.0F, -4.0F}, {8.0F, 8.0F, 8.0F}, 32.0F, 0.0F, 0.5F, light);
    const Mat4 bodyM = base * partTransform(bodyPivot, body);
    addSkinBox(out, bodyM, {-4.0F, -12.0F, -2.0F}, {8.0F, 12.0F, 4.0F}, 16.0F, 16.0F, 0.0F, light);
    addSkinBox(out, bodyM, {-4.0F, -12.0F, -2.0F}, {8.0F, 12.0F, 4.0F}, 16.0F, 32.0F, 0.25F, light);
    const Mat4 rightArmM = base * partTransform(rightArmPivot, rightArm);
    addSkinBox(out, rightArmM, {-1.0F, -10.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 40.0F, 16.0F, 0.0F, light);
    addSkinBox(out, rightArmM, {-1.0F, -10.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 40.0F, 32.0F, 0.25F, light);
    const Mat4 leftArmM = base * partTransform(leftArmPivot, leftArm);
    addSkinBox(out, leftArmM, {-3.0F, -10.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 32.0F, 48.0F, 0.0F, light);
    addSkinBox(out, leftArmM, {-3.0F, -10.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 48.0F, 48.0F, 0.25F, light);
    const Mat4 rightLegM = base * partTransform(rightLegPivot, rightLeg);
    addSkinBox(out, rightLegM, {-2.0F, -12.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 0.0F, 16.0F, 0.0F, light);
    addSkinBox(out, rightLegM, {-2.0F, -12.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 0.0F, 32.0F, 0.25F, light);
    const Mat4 leftLegM = base * partTransform(leftLegPivot, leftLeg);
    addSkinBox(out, leftLegM, {-2.0F, -12.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 16.0F, 48.0F, 0.0F, light);
    addSkinBox(out, leftLegM, {-2.0F, -12.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 0.0F, 48.0F, 0.25F, light);
    // The item sits in the right hand, in front of its fingers.
    if (pose.held == HeldItem::Block) {
        Mat4 item = glm::translate(rightArmM, Vec3(1.0F, -10.5F, -2.5F));
        item = glm::rotate(item, kPi / 4.0F, Vec3(0.0F, 1.0F, 0.0F));
        addItemCube(out, item, 6.0F, light);
    } else if (pose.held == HeldItem::Flat) {
        Mat4 item = glm::translate(rightArmM, Vec3(1.0F, -12.0F, -3.0F));
        item = glm::rotate(item, kPi / 2.0F, Vec3(0.0F, 1.0F, 0.0F));
        addItemFlat(out, item, 8.0F, light);
    }
}

void PlayerModel::buildFirstPerson(const PlayerPose& pose, const vxe::render::Camera& camera,
                                   std::vector<EntityVertex>& out) const {
    const f32 light = brightnessOf(pose.light);
    // The view's own space: x right, y up, z backwards, at the eye.
    const Vec3 right = camera.right();
    const Vec3 up = camera.up();
    const Vec3 back = -camera.forward();
    const Mat4 view(Vec4(right, 0.0F), Vec4(up, 0.0F), Vec4(back, 0.0F), Vec4(0.0F, 0.0F, 0.0F, 1.0F));
    const f32 f = m_swinging ? m_swing : 1.0F;
    const f32 swingRoot = std::sin(std::sqrt(f) * kPi);
    const f32 swingArc = std::sin(f * kPi);
    // A gentle bob with the walk, like the view's.
    const f32 bob = std::sin(m_walkPhase * 0.5F) * 0.02F * m_walkAmount;

    // The right arm reaches up into the view from below the lower right
    // corner, like Bedrock's: the shoulder out of sight, the hand at the lower
    // right. The swing of breaking and placing pushes it in towards the
    // crosshair and back.
    const Vec3 shoulder(kFirstPersonShoulder.x - (0.22F * swingRoot) + bob,
                        kFirstPersonShoulder.y + (0.1F * std::sin(std::sqrt(f) * 2.0F * kPi)) - std::abs(bob),
                        kFirstPersonShoulder.z - (0.12F * swingArc));
    const f32 armYaw = kFirstPersonArmYaw + (glm::radians(28.0F) * swingRoot);
    const f32 armPitch = kFirstPersonArmPitch - (glm::radians(22.0F) * swingArc);
    Mat4 arm = glm::translate(Mat4(1.0F), shoulder);
    arm = glm::rotate(arm, armYaw, Vec3(0.0F, 1.0F, 0.0F));
    arm = glm::rotate(arm, armPitch, Vec3(1.0F, 0.0F, 0.0F));
    arm = glm::rotate(arm, kFirstPersonArmRoll, Vec3(0.0F, 1.0F, 0.0F));
    arm = glm::scale(arm, Vec3(kPixel));
    const Mat4 armM = view * arm;
    addSkinBox(out, armM, {-1.0F, -10.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 40.0F, 16.0F, 0.0F, light);
    addSkinBox(out, armM, {-1.0F, -10.0F, -2.0F}, {4.0F, 12.0F, 4.0F}, 40.0F, 32.0F, 0.25F, light);
    if (pose.held == HeldItem::None) {
        return;
    }
    // The item rests in the hand, upright in the view whatever the arm's
    // angle. It sits about 45 degrees to the right of the line of sight, so a
    // block turned only a little shows its corner: more of the side facing
    // the middle of the screen than the other, and, tipped towards the eye,
    // its top.
    const Vec3 hand = Vec3(arm * Vec4(kFirstPersonHandEnd, 1.0F)) + kFirstPersonItemLift;
    Mat4 item = glm::translate(Mat4(1.0F), hand);
    item = glm::rotate(item, glm::radians(kFirstPersonItemTip - (30.0F * swingArc)), Vec3(1.0F, 0.0F, 0.0F));
    item = glm::rotate(item, glm::radians(kFirstPersonItemTurn) + (armYaw - kFirstPersonArmYaw),
                       Vec3(0.0F, 1.0F, 0.0F));
    if (pose.held == HeldItem::Block) {
        addItemCube(out, view * item, kFirstPersonBlockSize, light);
    } else {
        // A plant stands like a picture held up, its face towards the view.
        item = glm::rotate(item, glm::radians(-45.0F), Vec3(0.0F, 1.0F, 0.0F)); // Facing the eye.
        item = glm::translate(item, Vec3(0.0F, kFirstPersonBlockSize * 0.4F, 0.0F));
        addItemFlat(out, view * item, kFirstPersonBlockSize * 1.5F, light);
    }
}

} // namespace vr
