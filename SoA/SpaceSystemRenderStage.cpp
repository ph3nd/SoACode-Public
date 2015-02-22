#include "stdafx.h"
#include "SpaceSystem.h"
#include "SpaceSystemRenderStage.h"

#include <glm\gtc\type_ptr.hpp>
#include <glm\gtc\quaternion.hpp>
#include <glm\gtx\quaternion.hpp>
#include <glm\gtc\matrix_transform.hpp>

#include <Vorb/graphics/DepthState.h>
#include <Vorb/graphics/GLProgram.h>
#include <Vorb/graphics/SamplerState.h>
#include <Vorb/graphics/SpriteBatch.h>
#include <Vorb/graphics/SpriteFont.h>
#include <Vorb/colors.h>
#include <Vorb/utils.h>

#include "App.h"
#include "Camera.h"
#include "DebugRenderer.h"
#include "Errors.h"
#include "GLProgramManager.h"
#include "MainMenuSystemViewer.h"
#include "RenderUtils.h"
#include "SpaceSystemComponents.h"
#include "TerrainPatch.h"

SpaceSystemRenderStage::SpaceSystemRenderStage(ui32v2 viewport,
                                               SpaceSystem* spaceSystem,
                                               GameSystem* gameSystem,
                                               const MainMenuSystemViewer* systemViewer,
                                               const Camera* camera,
                                               const Camera* voxelCamera,
                                               vg::GLProgram* colorProgram,
                                               VGTexture selectorTexture) :
    m_viewport(viewport),
    m_spaceSystem(spaceSystem),
    m_gameSystem(gameSystem),
    m_mainMenuSystemViewer(systemViewer),
    m_camera(camera),
    m_voxelCamera(voxelCamera),
    m_colorProgram(colorProgram),
    m_selectorTexture(selectorTexture) {
    // Empty
}

SpaceSystemRenderStage::~SpaceSystemRenderStage() {
    // Empty
}

void SpaceSystemRenderStage::draw() {
    drawBodies();
    if (1 || m_mainMenuSystemViewer) { // TODO(Ben): 1 || is temporary
        drawPaths();
        drawHud();
    }
}

void SpaceSystemRenderStage::drawBodies() {
    glEnable(GL_CULL_FACE);

    f64v3 lightPos;
    // For caching light for far terrain
    std::map<vcore::EntityID, SpaceLightComponent*> lightCache;

    // Render spherical terrain
    for (auto& it : m_spaceSystem->m_sphericalTerrainCT) {
        auto& cmp = it.second;

        SpaceLightComponent* lightCmp = getBrightestLight(cmp, lightPos);
        lightCache[it.first] = lightCmp;

        m_sphericalTerrainComponentRenderer.draw(cmp, m_camera,
                                                 lightPos,
                                                 lightCmp,
                                                 &m_spaceSystem->m_namePositionCT.getFromEntity(it.first),
                                                 &m_spaceSystem->m_axisRotationCT.getFromEntity(it.first));
    }

    // Render far terrain
    if (m_voxelCamera) {
        for (auto& it : m_spaceSystem->m_farTerrainCT) {
            auto& cmp = it.second;

            SpaceLightComponent* lightCmp = lightCache[it.first];

            m_farTerrainComponentRenderer.draw(cmp, m_voxelCamera,
                                               lightPos,
                                               lightCmp,
                                               &m_spaceSystem->m_axisRotationCT.getFromEntity(it.first));
        }
    }

    DepthState::FULL.set();
}

void SpaceSystemRenderStage::drawPaths() {

    DepthState::READ.set();
    float alpha;

    // Draw paths
    m_colorProgram->use();
    m_colorProgram->enableVertexAttribArrays();
    glDepthMask(GL_FALSE);
    glLineWidth(3.0f);

    f32m4 wvp = m_camera->getProjectionMatrix() * m_camera->getViewMatrix();
    for (auto& it : m_spaceSystem->m_orbitCT) {
        
        // Get the augmented reality data
        if (m_mainMenuSystemViewer) {
            const MainMenuSystemViewer::BodyArData* bodyArData = m_mainMenuSystemViewer->finBodyAr(it.first);
            if (bodyArData == nullptr) continue;

            // Interpolated alpha
            alpha = 0.15 + 0.85 * hermite(bodyArData->hoverTime);
        } else {
            alpha = 0.15;
        }

        auto& cmp = it.second;
        if (cmp.parentNpId) {
            m_orbitComponentRenderer.drawPath(cmp, m_colorProgram, wvp, &m_spaceSystem->m_namePositionCT.getFromEntity(it.first),
                         m_camera->getPosition(), alpha, &m_spaceSystem->m_namePositionCT.get(cmp.parentNpId));
        } else {
            m_orbitComponentRenderer.drawPath(cmp, m_colorProgram, wvp, &m_spaceSystem->m_namePositionCT.getFromEntity(it.first),
                         m_camera->getPosition(), alpha);
        }
    }
    m_colorProgram->disableVertexAttribArrays();
    m_colorProgram->unuse();
    glDepthMask(GL_TRUE);
}

SpaceLightComponent* SpaceSystemRenderStage::getBrightestLight(SphericalTerrainComponent& cmp, OUT f64v3& pos) {
    SpaceLightComponent* rv = nullptr;
    f64 closestDist = 9999999999999999.0;
    for (auto& it : m_spaceSystem->m_spaceLightCT) {
        auto& npCmp = m_spaceSystem->m_namePositionCT.get(it.second.parentNpId);
        // TODO(Ben): Optimize out sqrt
        f64 dist = glm::length(npCmp.position - m_spaceSystem->m_namePositionCT.get(cmp.namePositionComponent).position);
        if (dist < closestDist) {
            closestDist = dist;
            rv = &it.second;
        }
    }
    return rv;
}


void SpaceSystemRenderStage::drawHud() {
    // Currently we need a viewer for this
    if (!m_mainMenuSystemViewer) return;

    const f32 ROTATION_FACTOR = (f32)(M_PI * 2.0 + M_PI / 4.0);
    static f32 dt = 0.0;
    dt += 0.01;

    // Lazily load spritebatch
    if (!m_spriteBatch) {
        m_spriteBatch = new SpriteBatch(true, true);
        m_spriteFont = new SpriteFont("Fonts/orbitron_bold-webfont.ttf", 32);
    }

    m_spriteBatch->begin();

    // Render all bodies
    for (auto& it : m_spaceSystem->m_namePositionCT) {

        // Get the augmented reality data
        const MainMenuSystemViewer::BodyArData* bodyArData = m_mainMenuSystemViewer->finBodyAr(it.first);
        if (bodyArData == nullptr) continue;

        vcore::ComponentID componentID;

        f64v3 position = it.second.position;
        f64v3 relativePos = position - m_camera->getPosition();
        color4 textColor;

        f32 hoverTime = bodyArData->hoverTime;

        if (bodyArData->inFrustum) {

            // Get screen position 
            f32v3 screenCoords = m_camera->worldToScreenPoint(relativePos);
            f32v2 xyScreenCoords(screenCoords.x * m_viewport.x, screenCoords.y * m_viewport.y);

            // Get a smooth interpolator with hermite
            f32 interpolator = hermite(hoverTime);
            
            // Find its orbit path color and do color interpolation
            componentID = m_spaceSystem->m_orbitCT.getComponentID(it.first);
            if (componentID) {
                const ui8v4& tcolor = m_spaceSystem->m_orbitCT.get(componentID).pathColor;
                ColorRGBA8 targetColor(tcolor.r, tcolor.g, tcolor.b, tcolor.a);
                textColor.lerp(color::White, targetColor, interpolator);
            } else {
                textColor.lerp(color::White, color::Aquamarine, interpolator);
            }

            f32 selectorSize = bodyArData->selectorSize;
          
            // Only render if it isn't too big
            if (selectorSize < MainMenuSystemViewer::MAX_SELECTOR_SIZE) {

                // Draw Indicator
                m_spriteBatch->draw(m_selectorTexture, nullptr, nullptr,
                                    xyScreenCoords,
                                    f32v2(0.5f, 0.5f),
                                    f32v2(selectorSize),
                                    interpolator * ROTATION_FACTOR,
                                    textColor, screenCoords.z);

                // Text offset and scaling
                const f32v2 textOffset(selectorSize / 2.0f, -selectorSize / 2.0f);
                const f32v2 textScale((((selectorSize - MainMenuSystemViewer::MIN_SELECTOR_SIZE) /
                    (MainMenuSystemViewer::MAX_SELECTOR_SIZE - MainMenuSystemViewer::MIN_SELECTOR_SIZE)) * 0.5 + 0.5) * 0.6);

                // Draw Text
                m_spriteBatch->drawString(m_spriteFont,
                                          it.second.name.c_str(),
                                          xyScreenCoords + textOffset,
                                          textScale,
                                          textColor,
                                          screenCoords.z);

            }
            // Land selector
            if (bodyArData->isLandSelected) {
                f32v3 selectedPos = bodyArData->selectedPos;
                // Apply axis rotation if applicable
                componentID = m_spaceSystem->m_axisRotationCT.getComponentID(it.first);
                if (componentID) {
                    f64q rot = m_spaceSystem->m_axisRotationCT.get(componentID).currentOrientation;
                    selectedPos = f32v3(rot * f64v3(selectedPos));
                }

                relativePos = (position + f64v3(selectedPos)) - m_camera->getPosition();
                screenCoords = m_camera->worldToScreenPoint(relativePos);
                xyScreenCoords = f32v2(screenCoords.x * m_viewport.x, screenCoords.y * m_viewport.y);

                color4 sColor = color::Red;
                sColor.a = 155;
                m_spriteBatch->draw(m_selectorTexture, nullptr, nullptr,
                                    xyScreenCoords,
                                    f32v2(0.5f, 0.5f),
                                    f32v2(22.0f) + cos(dt * 8.0f) * 4.0f,
                                    dt * ROTATION_FACTOR,
                                    sColor, 0.0f);
            }
        }
    }

    m_spriteBatch->end();
    m_spriteBatch->renderBatch(m_viewport, nullptr, &DepthState::READ);

    // Restore depth state
    DepthState::FULL.set();
}