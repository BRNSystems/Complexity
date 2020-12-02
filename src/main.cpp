#include <filesystem>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>

#include <gltf.hpp>
#include <glw.hpp>
#include <glwx.hpp>

#include "ecs.hpp"
#include "input.hpp"

using namespace std::literals;
namespace fs = std::filesystem;

/* TODO
 * ECS: Make EntityId fully internal and only use EntityHandle outside of ECS implementation
 * ECS: Move all (short) implementations out of the header part
 */

struct Material {
    std::shared_ptr<glw::Texture> baseColorTexture = nullptr;
    glm::vec4 baseColor = glm::vec4(1.0f);

    Material()
        : baseColorTexture(getDefaultTexture())
    {
    }

    static std::shared_ptr<glw::Texture> getDefaultTexture()
    {
        static auto defaultTexture
            = std::make_shared<glw::Texture>(glwx::makeTexture2D(glm::vec4(1.0f)));
        return defaultTexture;
    }

    static std::shared_ptr<Material> getDefaultMaterial()
    {
        static auto defaultMaterial = std::make_shared<Material>();
        return defaultMaterial;
    }
};

struct Mesh {
    struct Primitive {
        glwx::Primitive primitive;
        std::shared_ptr<Material> material;
        // This is just so the mesh can keep ownership its buffers
        std::vector<std::shared_ptr<glw::Buffer>> buffers;
    };

    std::vector<Primitive> primitives;
};

namespace components {
// Going full blown on this right away was a mistake
struct Hierarchy {
    ecs::EntityHandle parent;
    ecs::EntityHandle firstChild;
    ecs::EntityHandle prevSibling;
    ecs::EntityHandle nextSibling;

    static void removeParent(ecs::EntityHandle& entity)
    {
        auto& entityHierarchy = entity.get<Hierarchy>();
        if (entityHierarchy.parent) {
            auto& parentHierarchy = entityHierarchy.parent.get<Hierarchy>();
            assert(parentHierarchy.firstChild);
            if (parentHierarchy.firstChild == entity) {
                assert(!entityHierarchy.prevSibling);
                parentHierarchy.firstChild = entityHierarchy.nextSibling;
            } else {
                assert(entityHierarchy.prevSibling);
                entityHierarchy.prevSibling.get<Hierarchy>().nextSibling
                    = entityHierarchy.nextSibling;
            }
            entityHierarchy.nextSibling.get<Hierarchy>().prevSibling = entityHierarchy.prevSibling;
        }
        entityHierarchy.parent = ecs::EntityHandle();
        entityHierarchy.prevSibling = ecs::EntityHandle();
        entityHierarchy.nextSibling = ecs::EntityHandle();
    }

    static void setParent(ecs::EntityHandle& entity, ecs::EntityHandle& parent)
    {
        removeParent(entity);
        auto& entityHierarchy = entity.getOrAdd<Hierarchy>();
        entityHierarchy.parent = parent;
        auto& parentHierarchy = parent.getOrAdd<Hierarchy>();
        if (!parentHierarchy.firstChild) {
            parentHierarchy.firstChild = entity;
            entityHierarchy.prevSibling = ecs::EntityHandle();
        } else {
            auto last = parentHierarchy.firstChild;
            while (true) {
                const auto& h = last.get<Hierarchy>();
                if (h.nextSibling) {
                    last = h.nextSibling;
                } else {
                    break;
                }
            }
            last.get<Hierarchy>().nextSibling = entity;
            entityHierarchy.prevSibling = last;
        }
        entityHierarchy.nextSibling = ecs::EntityHandle();
    }
};

// This thing is not data driven AT ALL
using Mesh = std::shared_ptr<Mesh>;

using Transform = glwx::Transform;
}
namespace comp = components;

ecs::World& getWorld()
{
    static ecs::World world;
    return world;
}

template <typename T, typename Container>
T makeGlm(const Container& vals)
{
    T ret;
    const auto ptr = glm::value_ptr(ret);
    static constexpr auto N = sizeof(T) / sizeof(decltype(*ptr)); // not clean
    assert(vals.size() == N);
    for (size_t i = 0; i < vals.size(); ++i)
        ptr[i] = vals[i];
    return ret;
}

namespace AttributeLocations {
constexpr size_t Position = 0;
constexpr size_t Normal = 1;
constexpr size_t Tangent = 2;
constexpr size_t TexCoord0 = 3;
constexpr size_t TexCoord1 = 4;
constexpr size_t Color0 = 5;
constexpr size_t Joints0 = 6;
constexpr size_t Weights0 = 7;
}

std::unordered_map<std::string, size_t> attributeLocations = {
    { "POSITION", AttributeLocations::Position },
    { "NORMAL", AttributeLocations::Normal },
    { "TANGENT", AttributeLocations::Tangent },
    { "TEXCOORD_0", AttributeLocations::TexCoord0 },
    { "TEXCOORD_1", AttributeLocations::TexCoord1 },
    { "COLOR_0", AttributeLocations::Color0 },
    { "JOINTS_0", AttributeLocations::Joints0 },
    { "WEIGHTS_0", AttributeLocations::Weights0 },
};

struct GltfImportCache {
    std::unordered_map<gltf::NodeIndex, ecs::EntityHandle> entityMap;
    std::unordered_map<gltf::MeshIndex, std::shared_ptr<Mesh>> meshMap;
    std::unordered_map<gltf::BufferViewIndex, std::shared_ptr<glw::Buffer>> bufferMap;
    std::unordered_map<gltf::MaterialIndex, std::shared_ptr<Material>> materialMap;
    std::unordered_map<gltf::TextureIndex, std::shared_ptr<glw::Texture>> textureMap;

    std::shared_ptr<glw::Buffer> getBuffer(
        const gltf::Gltf& gltfFile, gltf::BufferViewIndex bvIndex)
    {
        const auto it = bufferMap.find(bvIndex);
        if (it != bufferMap.end())
            return it->second;

        const auto& bv = gltfFile.bufferViews[bvIndex];
        assert(bv.target);
        const auto target = static_cast<glw::Buffer::Target>(*bv.target);
        const auto data = gltfFile.getBufferViewData(bvIndex);
        auto buffer = std::make_shared<glw::Buffer>();
        buffer->data(target, glw::Buffer::UsageHint::StaticDraw, data.first, data.second);
        bufferMap.emplace(bvIndex, buffer);
        return buffer;
    }

    std::shared_ptr<glw::Texture> getTexture(
        const gltf::Gltf& gltfFile, gltf::TextureIndex textureIndex)
    {
        const auto it = textureMap.find(textureIndex);
        if (it != textureMap.end())
            return it->second;

        const auto& gtexture = gltfFile.textures[textureIndex];

        auto minFilter = glw::Texture::MinFilter::LinearMipmapNearest;
        auto magFilter = glw::Texture::MagFilter::Linear;
        auto wrapS = glw::Texture::WrapMode::Repeat;
        auto wrapT = glw::Texture::WrapMode::Repeat;
        if (gtexture.sampler) {
            const auto& sampler = gltfFile.samplers[*gtexture.sampler];
            if (sampler.minFilter)
                minFilter = static_cast<glw::Texture::MinFilter>(*sampler.minFilter);
            if (sampler.magFilter)
                magFilter = static_cast<glw::Texture::MagFilter>(*sampler.magFilter);
            wrapS = static_cast<glw::Texture::WrapMode>(sampler.wrapS);
            wrapT = static_cast<glw::Texture::WrapMode>(sampler.wrapT);
        }
        const auto mipmaps = static_cast<GLenum>(minFilter)
            >= static_cast<GLenum>(glw::Texture::MinFilter::NearestMipmapNearest);

        assert(gtexture.source);
        const auto data = gltfFile.getImageData(*gtexture.source);

        auto tex = glwx::makeTexture2D(data.first, data.second, mipmaps);
        assert(tex);
        tex->setMinFilter(minFilter);
        tex->setMagFilter(magFilter);
        tex->setWrap(wrapS, wrapT);

        auto texture = std::make_shared<glw::Texture>(std::move(*tex));
        textureMap.emplace(textureIndex, texture);
        return texture;
    }

    std::shared_ptr<Material> getMaterial(
        const gltf::Gltf& gltfFile, gltf::MaterialIndex materialIndex)
    {
        const auto it = materialMap.find(materialIndex);
        if (it != materialMap.end())
            return it->second;

        const auto& gmaterial = gltfFile.materials[materialIndex];
        const auto material = std::make_shared<Material>();
        assert(gmaterial.pbrMetallicRoughness);
        const auto& pbr = *gmaterial.pbrMetallicRoughness;
        material->baseColor = makeGlm<glm::vec4>(pbr.baseColorFactor);
        if (pbr.baseColorTexture) {
            const auto& texInfo = *pbr.baseColorTexture;
            assert(texInfo.texCoord == 0);
            material->baseColorTexture = getTexture(gltfFile, texInfo.index);
        }
        materialMap.emplace(materialIndex, material);
        return material;
    }

    std::shared_ptr<Mesh> getMesh(const gltf::Gltf& gltfFile, gltf::MeshIndex meshIndex)
    {
        const auto it = meshMap.find(meshIndex);
        if (it != meshMap.end())
            return it->second;

        const auto& gmesh = gltfFile.meshes[meshIndex];
        auto mesh = std::make_shared<Mesh>();
        for (const auto& gprim : gmesh.primitives) {
            const auto mode = static_cast<glw::DrawMode>(gprim.mode);
            auto& prim = mesh->primitives.emplace_back(
                Mesh::Primitive { glwx::Primitive(mode), Material::getDefaultMaterial(), {} });

            // I need to determine a vertex format per buffer (which may be used in multiple
            // attributes), so I determine the set of buffers first, then build the formats.
            std::set<gltf::BufferViewIndex> bufferViews;
            for (const auto& attr : gprim.attributes)
                bufferViews.insert(gltfFile.accessors[attr.accessor].bufferView.value());

            for (const auto bvIndex : bufferViews) {
                glw::VertexFormat vfmt;
                std::optional<size_t> vertexCount;
                // Now find the attributes that use this buffer again to build the vertex format
                for (const auto& attr : gprim.attributes) {
                    const auto& accessor = gltfFile.accessors[attr.accessor];
                    if (accessor.bufferView.value() == bvIndex) {
                        const auto count = static_cast<size_t>(accessor.type);
                        assert(count >= 1 && count <= 4);
                        const auto componentType
                            = static_cast<glw::AttributeType>(accessor.componentType);
                        vfmt.add(accessor.byteOffset, attributeLocations.at(attr.id), count,
                            componentType, accessor.normalized);
                        vertexCount
                            = vertexCount ? std::min(*vertexCount, accessor.count) : accessor.count;
                    }
                }

                const auto& bufferView = gltfFile.bufferViews[bvIndex];
                if (bufferView.byteStride)
                    vfmt.setStride(*bufferView.byteStride);

                const auto buffer = getBuffer(gltfFile, bvIndex);
                prim.primitive.addVertexBuffer(*buffer, vfmt);
                prim.primitive.vertexRange = glwx::Primitive::Range { 0, vertexCount.value() };
                prim.buffers.push_back(std::move(buffer));
            }

            if (gprim.indices) {
                const auto& accessor = gltfFile.accessors[*gprim.indices];
                const auto type = static_cast<glw::IndexType>(accessor.componentType);
                assert(type == glw::IndexType::U8 || type == glw::IndexType::U16
                    || type == glw::IndexType::U32);
                const auto buffer = getBuffer(gltfFile, accessor.bufferView.value());
                prim.primitive.setIndexBuffer(*buffer, type);
                prim.primitive.indexRange
                    = glwx::Primitive::Range { accessor.byteOffset / glw::getIndexTypeSize(type),
                          accessor.count };
            }

            if (gprim.material) {
                prim.material = getMaterial(gltfFile, *gprim.material);
            }
        }
        meshMap.emplace(meshIndex, mesh);
        return mesh;
    }

    ecs::EntityHandle getEntity(
        ecs::World& world, const gltf::Gltf& gltfFile, gltf::NodeIndex nodeIndex)
    {
        const auto it = entityMap.find(nodeIndex);
        if (it != entityMap.end())
            return it->second;

        const auto& node = gltfFile.nodes[nodeIndex];
        auto entity = world.createEntity();
        entity.add<comp::Hierarchy>();
        entity.add<comp::Transform>().setMatrix(makeGlm<glm::mat4>(node.getTransformMatrix()));
        if (node.mesh) {
            entity.add<comp::Mesh>(getMesh(gltfFile, *node.mesh));
        }
        if (node.parent) {
            auto parent = getEntity(world, gltfFile, *node.parent);
            comp::Hierarchy::setParent(entity, parent);
        }
        entityMap.emplace(nodeIndex, entity);
        return entity;
    }
};

bool loadMap(const fs::path& path, ecs::World& world)
{
    const auto gltfFileOpt = gltf::load(path);
    if (!gltfFileOpt) {
        fmt::print("Could not load gltf file");
        return false;
    }
    const auto& gltfFile = *gltfFileOpt;
    assert(gltfFile.scenes.size() == 1);

    GltfImportCache importCache;

    for (const auto nodeIndex : gltfFile.scenes[0].nodes) {
        importCache.getEntity(world, gltfFile, nodeIndex);
    }

    return true;
}

struct Camera {
    glm::mat4 projection;
    glwx::Transform transform;
};

const auto vert = R"(
    #version 330 core

    uniform mat4 modelMatrix;
    uniform mat4 viewMatrix;
    uniform mat4 projectionMatrix;
    uniform mat3 normalMatrix;

    layout (location = 0) in vec3 attrPosition;
    layout (location = 1) in vec3 attrNormal;
    layout (location = 3) in vec2 attrTexCoords;

    out vec2 texCoords;
    out vec3 normal; // view space

    void main() {
        texCoords = attrTexCoords;
        normal = normalMatrix * attrNormal;
        gl_Position = projectionMatrix * viewMatrix * modelMatrix * vec4(attrPosition, 1.0);
    }
)"sv;

const auto frag = R"(
    #version 330 core

    const float ambient = 0.4;
    const float lightIntensity = 0.6;

    uniform vec4 baseColorFactor;
    uniform sampler2D baseColorTexture;
    uniform vec3 lightDir; // view space

    in vec2 texCoords;
    in vec3 normal;

    out vec4 fragColor;

    void main() {
        vec4 base = baseColorFactor * texture2D(baseColorTexture, texCoords);
        float nDotL = max(dot(lightDir, normalize(normal)), 0.0);
        fragColor = vec4(base.rgb * ambient + base.rgb * nDotL * lightIntensity, base.a);
    }
)"sv;

void renderSystem(const Camera& camera, ecs::World& world)
{
    static glw::ShaderProgram shader = glwx::makeShaderProgram(vert, frag).value();
    shader.bind();
    shader.setUniform("lightDir", glm::vec3(0.0f, 0.0f, 1.0f));

    shader.setUniform("projectionMatrix", camera.projection);
    const auto view = glm::inverse(camera.transform.getMatrix());
    shader.setUniform("viewMatrix", view);

    world.forEachEntity<const comp::Hierarchy, const comp::Transform, const comp::Mesh>(
        [&view](const comp::Hierarchy& hierarchy, const comp::Transform& transform,
            const comp::Mesh& mesh) {
            auto parent = hierarchy.parent;
            const auto model = parent && parent.has<comp::Transform>()
                ? parent.get<comp::Transform>().getMatrix() * transform.getMatrix()
                : transform.getMatrix();

            shader.setUniform("modelMatrix", model);
            const auto modelView = view * model;
            const auto normal = glm::mat3(glm::transpose(glm::inverse(modelView)));
            shader.setUniform("normalMatrix", normal);

            for (const auto& prim : mesh->primitives) {
                const auto& material
                    = prim.material ? *prim.material : *Material::getDefaultMaterial();
                material.baseColorTexture->bind(0);
                shader.setUniform("baseColorTexture", 0);
                shader.setUniform("baseColorFactor", material.baseColor);

                prim.primitive.draw();
            }
        });
}

void move(const Controller& ctrl, glwx::Transform& transform, float dt)
{
    if (ctrl.lookToggle->getState()) {
        const auto sensitivity = 0.01f;
        const auto look = glm::vec2(ctrl.lookX->getDelta(), ctrl.lookY->getDelta()) * sensitivity;
        transform.rotate(glm::angleAxis(-look.x, glm::vec3(0.0f, 1.0f, 0.0f)));
        transform.rotateLocal(glm::angleAxis(-look.y, glm::vec3(1.0f, 0.0f, 0.0f)));
    }

    const auto forward = ctrl.forwards->getState() - ctrl.backwards->getState();
    const auto sideways = ctrl.right->getState() - ctrl.left->getState();
    const auto updown = ctrl.up->getState() - ctrl.down->getState();
    const auto speed = ctrl.fast->getState() ? 10.f : 2.f;
    const auto move = speed * dt * glm::vec3(sideways, updown, -forward); // forward is -z
    if (glm::length(move) > 0.0f) {
        transform.moveLocal(move);
    }
}

int main(int, char**)
{
    glwx::Window::Properties props;
    props.msaaSamples = 8;
    const auto window = glwx::makeWindow("7DFPS", 1024, 768, props).value();
    glw::State::instance().setViewport(window.getSize().x, window.getSize().y);

#ifndef NDEBUG
    glwx::debug::init();
#endif

    auto& world = getWorld();
    if (!loadMap("media/box_test.glb", world)) {
        return 1;
    }
    world.flush();

    const auto aspect = static_cast<float>(window.getSize().x) / window.getSize().y;
    Camera camera { glm::perspective(glm::radians(45.0f), aspect, 0.1f, 200.0f), {} };
    camera.transform.lookAtPos(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f, 2.0f, 0.0f));

    Controller controller(SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
        SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_LSHIFT, MouseButtonInput(1));

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    SDL_Event event;
    bool running = true;
    float time = glwx::getTime();
    while (running) {
        while (SDL_PollEvent(&event) != 0) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                }
                break;
            }
        }

        const auto now = glwx::getTime();
        const auto dt = now - time;
        time = now;

        InputManager::instance().update();
        move(controller, camera.transform, dt);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        renderSystem(camera, world);

        window.swap();
    }

    return 0;
}
