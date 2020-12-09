#pragma once

#include <memory>

#include <glm/glm.hpp>

#include <glwx.hpp>

#include "ecs.hpp"
#include "input.hpp"

namespace comp {
using Transform = glwx::Transform;

struct Velocity {
    glm::vec3 value { 0.0f, 0.0f, 0.0f };
};

struct CylinderCollider {
    float radius;
    float height;
};

struct BoxCollider {
    glm::vec3 halfExtents;
};

struct PlayerInputController {
    template <typename T>
    PlayerInputController(SDL_Scancode forwards, SDL_Scancode backwards, SDL_Scancode left,
        SDL_Scancode right, SDL_Scancode up, SDL_Scancode down, SDL_Scancode fast, T&& lookToggle)
        : forwards(std::make_unique<KeyboardInput>(forwards))
        , backwards(std::make_unique<KeyboardInput>(backwards))
        , left(std::make_unique<KeyboardInput>(left))
        , right(std::make_unique<KeyboardInput>(right))
        , up(std::make_unique<KeyboardInput>(up))
        , down(std::make_unique<KeyboardInput>(down))
        , fast(std::make_unique<KeyboardInput>(fast))
        , lookX(std::make_unique<MouseAxisInput>(MouseAxisInput::Axis::X))
        , lookY(std::make_unique<MouseAxisInput>(MouseAxisInput::Axis::Y))
        , lookToggle(std::make_unique<T>(lookToggle))
    {
    }

    std::unique_ptr<BinaryInput> forwards;
    std::unique_ptr<BinaryInput> backwards;
    std::unique_ptr<BinaryInput> left;
    std::unique_ptr<BinaryInput> right;
    std::unique_ptr<BinaryInput> up;
    std::unique_ptr<BinaryInput> down;
    std::unique_ptr<BinaryInput> fast;

    std::unique_ptr<AnalogInput> lookX;
    std::unique_ptr<AnalogInput> lookY;
    std::unique_ptr<BinaryInput> lookToggle;
};
}

struct CollisionResult {
    glm::vec3 normal;
    float penetrationDepth;
};

std::optional<CollisionResult> findFirstCollision(ecs::World& world, ecs::EntityHandle entity,
    comp::Transform& transform, const comp::CylinderCollider& collider);

struct RayCastHit {
    ecs::EntityHandle entity;
    float t;
};

std::optional<RayCastHit> castRay(
    ecs::World& world, const glm::vec3& rayOrigin, const glm::vec3& rayDir);

void integrationSystem(ecs::World& world, float dt);

void playerControlSystem(ecs::World& world, float dt);
