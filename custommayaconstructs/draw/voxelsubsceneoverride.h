#pragma once

#include <maya/MPxSubSceneOverride.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnMesh.h>
#include <maya/MShaderManager.h>
#include "voxelshape.h"
#include "../../cube.h"
#include "../commands/changevoxeleditmodecommand.h"
#include "../../event.h"
#include <maya/MGeometryExtractor.h>
#include <maya/MPxComponentConverter.h>
#include <memory>
#include <maya/MUserData.h>
#include <maya/MSharedPtr.h>
#include <maya/MEventMessage.h>
#include <maya/MCommandMessage.h>
#include <maya/MCallbackIdArray.h>
#include <functional>
#include <maya/M3dView.h>

using namespace MHWRender;
using std::unique_ptr;
using std::make_unique;

struct RenderItemInfo {
    MIndexBufferDescriptor indexDesc;
    MShaderInstance* shaderInstance;
    MString renderItemName;

    RenderItemInfo(const MIndexBufferDescriptor& idx, MShaderInstance* shader, const MString& name)
        : indexDesc(idx), shaderInstance(shader), renderItemName(name) {}
};

class SelectionCustomData : public MUserData {
public:
    SelectionCustomData(std::function<void(int)> onHover)
        : MUserData(), hoverCallback(std::move(onHover))
    {}
    ~SelectionCustomData() override = default;

    std::function<void(int)> hoverCallback;
};

/**
 * This converter is registered with the render item that writes to the selection buffer.
 * Generally, component converters are for converting index buffer indices to components, but in this case
 * we're just using it as an intersection machine for getting which voxels were clicked or hovered.
 */
class VoxelSubSceneComponentConverter : public MPxComponentConverter {
public:
    static MPxComponentConverter* creator() {
        return new VoxelSubSceneComponentConverter();
    }

    VoxelSubSceneComponentConverter() = default;
    ~VoxelSubSceneComponentConverter() override = default;

    void addIntersection(MIntersection& intersection) override {
        // Instance IDs are 1-based, so subtract 1 to get a 0-based index.
        int instanceID = intersection.instanceID() - 1;
        if (instanceID < 0) return;

        MFnSingleIndexedComponent fnFaceComp;
        fnFaceComp.setObject(componentObj);

        // Hijack this face component to store the voxel instance ID rather than a face index.
        fnFaceComp.addElement(instanceID);
        customData->hoverCallback(instanceID);
    }

    MSelectionMask selectionMask() const override {
        MSelectionMask mask;
        mask.addMask(MSelectionMask::kSelectMeshFaces);
        mask.addMask(MSelectionMask::kSelectMeshVerts);
        return mask;
    }

    void initialize(const MRenderItem& renderItem) override {
        componentObj = fnComp.create(MFn::kMeshPolygonComponent);
        customData = static_cast<SelectionCustomData*>(renderItem.getCustomData().get());
    }

    MObject component() override {
        return componentObj;
    }

private:
    SelectionCustomData* customData = nullptr;
    MObject componentObj = MObject::kNullObj;
    MFnSingleIndexedComponent fnComp;
};

class VoxelSubSceneOverride : public MPxSubSceneOverride {
private:
    VoxelShape* voxelShape;
    MObject voxelShapeObj;

    enum class ShowHideStateChange {
        None,
        HideSelected,
        ShowAll,
        ShowSelected
    } showHideStateChange = ShowHideStateChange::None;

    using RenderItemFaceIdxMap = std::unordered_map<MString, std::vector<uint32_t>, Utils::MStringHash, Utils::MStringEq>;
    
    bool shouldUpdate = true;
    bool selectionChanged = false;
    bool editModeChanged = true;
    bool hideAllowed = true;
    bool hoveredVoxelChanged = false;
    EventBase::Unsubscribe unsubscribeFromVoxelEditModeChanges;
    MCallbackIdArray callbackIds;
    MMatrixArray selectedVoxelMatrices;
    MMatrixArray hoveredVoxelMatrices; // Will only ever have 0 or 1 matrix in it.
    std::unordered_set<uint> voxelsToHide;
    std::vector<uint> visibleVoxelIdToGlobalId;  // maps visible voxel instance IDs to global voxel IDs (including hidden ones)
    RenderItemFaceIdxMap hiddenFaces;            // hidden face indices per render item
    RenderItemFaceIdxMap recentlyHiddenFaces;    // the most recent faces to be hidden (again mapped by render item)
    std::unordered_set<uint> hiddenVoxels;              // global voxel IDs that are currently hidden
    std::unordered_set<uint> recentlyHiddenVoxels;      // the most recent global voxel IDs to be hidden

    // Must be static because addProcCallback is special and only allows one registered callback at a time
    inline static MCallbackId showHideCallbackId = 0;

    inline static const MString voxelSelectedHighlightItemName = "VoxelSelectedHighlightItem";
    inline static const MString voxelPreviewSelectionHighlightItemName = "VoxelPreviewSelectionHighlightItem";
    inline static const MString voxelWireframeRenderItemName = "VoxelWireframeRenderItem";
    inline static const MString voxelSelectionRenderItemName = "VoxelSelectionItem";
    // Enabled state of the voxel decoration render items. (Note: actual state may be more restricted; i.e. if instance transform array is empty)
    std::unordered_map<MString, bool, Utils::MStringHash, Utils::MStringEq> voxelRenderItemsEnabledState = {
        { voxelSelectedHighlightItemName, false },
        { voxelPreviewSelectionHighlightItemName, false },
        { voxelWireframeRenderItemName, false },
        { voxelSelectionRenderItemName, false }
    };

    ComPtr<ID3D11Buffer> positionsBuffer;
    ComPtr<ID3D11UnorderedAccessView> positionsUAV;

    ComPtr<ID3D11Buffer> normalsBuffer;
    ComPtr<ID3D11UnorderedAccessView> normalsUAV;

    // The deform shader also needs the original vertex positions and normals to do its transformations
    ComPtr<ID3D11Buffer> originalPositionsBuffer;
    ComPtr<ID3D11ShaderResourceView> originalPositionsSRV;

    ComPtr<ID3D11Buffer> originalNormalsBuffer;
    ComPtr<ID3D11ShaderResourceView> originalNormalsSRV;

    // These are just stored to persist the buffers. Subscene owns any geometry buffers it creates.
    std::vector<unique_ptr<MVertexBuffer>> meshVertexBuffers;
    std::unordered_map<MString, unique_ptr<MIndexBuffer>, Utils::MStringHash, Utils::MStringEq> meshIndexBuffers; // Stored by render item name, so we can update them easily.
    std::vector<uint32_t> allMeshIndices; // Mesh vertex indices, _not_ split per render item but rather for the entire mesh.
    std::unordered_set<MUint64> meshRenderItemIDs;
    std::vector<uint32_t> extractedVertexIdMap; // Maps extracted vertex IDs to original vertex IDs (see getVertexIdMapping)

    unique_ptr<MVertexBuffer> voxelVertexBuffer;
    std::unordered_map<MGeometry::Primitive, unique_ptr<MIndexBuffer>> voxelIndexBuffers;

    VoxelSubSceneOverride(const MObject& obj)
    : MPxSubSceneOverride(obj), voxelShapeObj(obj) {
        MFnDependencyNode dn(obj);
        voxelShape = static_cast<VoxelShape*>(dn.userNode());

        MCallbackId callbackId = MEventMessage::addEventCallback("SelectionChanged", onSelectionChanged, this);
        callbackIds.append(callbackId);

        unsubscribeFromVoxelEditModeChanges = ChangeVoxelEditModeCommand::subscribe([this](const EditModeChangedEventArgs& args) {
            this->onEditModeChange(args.newMode, args.shapeName);
        });
    }

    static void onSelectionChanged(void* clientData) {
        VoxelSubSceneOverride* subscene = static_cast<VoxelSubSceneOverride*>(clientData);
        // Collect the voxel instances that are selected
        const MObjectArray& activeComponents = subscene->voxelShape->activeComponents();
        const MMatrixArray& voxelMatrices = subscene->voxelShape->getVoxels().get()->modelMatrices;
        const std::vector<uint>& visibleVoxelIdToGlobalId = subscene->visibleVoxelIdToGlobalId;
        subscene->selectedVoxelMatrices.clear();
        subscene->hoveredVoxelMatrices.clear();

        for (const MObject& comp : activeComponents) {
            MFnSingleIndexedComponent fnComp(comp);
            for (int i = 0; i < fnComp.elementCount(); ++i) {
                int voxelInstanceId = fnComp.element(i);
                if (voxelInstanceId >= (int)visibleVoxelIdToGlobalId.size()) continue;

                const MMatrix& voxelMatrix = voxelMatrices[visibleVoxelIdToGlobalId[voxelInstanceId]];
                subscene->selectedVoxelMatrices.append(voxelMatrix);
            }
        }

        subscene->shouldUpdate = true;
        subscene->selectionChanged = true;
        invalidateRecentlyHidden(subscene);
    }

    /**
     * Surprisingly, neither MpxSurfaceShape nor MPxSubsceneOverride provide any mechanism for hooking into hiding components.
     * To handle this, we just have to listen for commands that contain "hide" or "showHidden", etc.
     */
    static void onShowHideStateChange(const MString& procName, unsigned int procId, bool isProcEntry, unsigned int type, void* clientData) {
        // Only need to run this callback once (but it's invoked on entry and exit of the procedure)
        if (!isProcEntry) return;
        
        bool toggleHideCommand = (procName.indexW("toggleVisibilityAndKeepSelection") != -1);
        bool hideCommand = (procName == "hide");
        bool showHiddenCommand = (procName.indexW("showHidden") != -1);
        if (!toggleHideCommand && !hideCommand && !showHiddenCommand) return;

        VoxelSubSceneOverride* subscene = static_cast<VoxelSubSceneOverride*>(clientData);
        subscene->shouldUpdate = true;

        if (hideCommand) {
            // To match Maya behavior, the plain hide command only works once until something invalidates the hidden selection
            subscene->showHideStateChange = (subscene->hideAllowed) ? ShowHideStateChange::HideSelected : ShowHideStateChange::None;
        }
        else if (showHiddenCommand) {
            subscene->showHideStateChange = ShowHideStateChange::ShowAll;
        }
        else if (toggleHideCommand) {
            subscene->showHideStateChange = (subscene->recentlyHiddenVoxels.size() > 0) ? ShowHideStateChange::ShowSelected : 
                                            (subscene->voxelShape->hasActiveComponents()) ? ShowHideStateChange::HideSelected : 
                                                                                            ShowHideStateChange::ShowAll;
        }

        // Force a subscene update to occur by refreshing the viewport.
        // This won't necessarily happen on its own, because Maya doesn't consider a custom shape's components to be valid for hiding/showing,
        // (which is why we have to implement the behavior ourselves), so it thinks nothing has changed and will not refresh immediately.
	    M3dView::active3dView().scheduleRefresh();

        if (subscene->showHideStateChange != ShowHideStateChange::HideSelected) return;
        subscene->hideAllowed = false;

        const MObjectArray& activeComponents = subscene->voxelShape->activeComponents();
        const std::vector<uint>& visibleVoxelIdToGlobalId = subscene->visibleVoxelIdToGlobalId;

        for (const MObject& comp : activeComponents) {
            MFnSingleIndexedComponent voxelComponent(comp);
            for (int i = 0; i < voxelComponent.elementCount(); ++i) {
                int voxelInstanceId = voxelComponent.element(i);

                // The voxel instance ID reported by the intersection is an ID into the list of visible voxels.
                // We need to convert that to an ID into the global list of voxels (which includes hidden ones).
                subscene->voxelsToHide.insert(visibleVoxelIdToGlobalId[voxelInstanceId]);
            }
        }
    }

    /**
     * Invoked whenever the voxel edit mode changes on any voxel shape in the scene. 
     * Depending on whether the shapeName corresponds to this shape, and the mode, mark 
     * the voxel edit render items for enable/disable in the next update.
     */
    void onEditModeChange(VoxelEditMode newMode, const MString& shapeName) {
        bool isThisShape = (shapeName == voxelShape->name());
        bool isObjectMode = (newMode == VoxelEditMode::Object);
        bool isPaintMode = (newMode == VoxelEditMode::FacePaint || newMode == VoxelEditMode::ParticlePaint);
        bool isSelectionMode = (newMode == VoxelEditMode::Selection);

        voxelRenderItemsEnabledState[voxelSelectedHighlightItemName] = !(isObjectMode || isPaintMode);
        voxelRenderItemsEnabledState[voxelPreviewSelectionHighlightItemName] = !(isObjectMode || isPaintMode);
        voxelRenderItemsEnabledState[voxelSelectionRenderItemName] = !(isObjectMode || isPaintMode);
        voxelRenderItemsEnabledState[voxelWireframeRenderItemName] = !isObjectMode;

        // If this event is for a different shape, disable everything.
        for (auto& [itemName, enabled] : voxelRenderItemsEnabledState) {
            voxelRenderItemsEnabledState[itemName] = (enabled && isThisShape);
        }
                
        if (isSelectionMode && isThisShape) {
            MCommandMessage::removeCallback(showHideCallbackId);
            showHideCallbackId = MCommandMessage::addProcCallback(onShowHideStateChange, this, nullptr);
        }

        voxelShape->unsubscribePaintStateChanges();
        if (isPaintMode && isThisShape) {
            sendVoxelInfoToPaintRenderOp(newMode);
            voxelShape->subscribeToPaintStateChanges(newMode);
        }

        shouldUpdate = true;
        editModeChanged = true;
    }

    /**
     * Merge the recentlyHidden* data into the central hidden* data. E.g. clearing the cache of what was last hidden.
     * This operation happens, for instance, when the user clears their selection or toggles hide to show what was last hidden. (To be consistent with Maya's own hide/show behaviour). 
     */
    static void invalidateRecentlyHidden(VoxelSubSceneOverride* const subscene) {
        RenderItemFaceIdxMap& recentlyHiddenFaces = subscene->recentlyHiddenFaces;
        RenderItemFaceIdxMap& hiddenFaces = subscene->hiddenFaces;
        std::unordered_set<uint>& recentlyHiddenVoxels = subscene->recentlyHiddenVoxels;
        std::unordered_set<uint>& hiddenVoxels = subscene->hiddenVoxels;

        for (auto& [itemName, faceIdxs] : recentlyHiddenFaces) {
            auto it = hiddenFaces.find(itemName);
            if (it == hiddenFaces.end()) {
                hiddenFaces[itemName] = std::move(faceIdxs);
                continue;
            }

            it->second.insert(it->second.end(), faceIdxs.begin(), faceIdxs.end());
        }
        recentlyHiddenFaces.clear();

        hiddenVoxels.insert(recentlyHiddenVoxels.begin(), recentlyHiddenVoxels.end());
        recentlyHiddenVoxels.clear();

        subscene->hideAllowed = true;
    }

    void onHoveredVoxelChange(int hoveredVoxelInstanceId) {
        // Already called this frame (likely because we did a drag-select and this gets called for each intersection)
        if (hoveredVoxelChanged) return;
        M3dView::active3dView().scheduleRefresh();
        
        hoveredVoxelMatrices.clear();
        const MMatrixArray& voxelMatrices = voxelShape->getVoxels().get()->modelMatrices;
        
        int globalVoxelId = visibleVoxelIdToGlobalId[hoveredVoxelInstanceId];
        hoveredVoxelMatrices.append(voxelMatrices[globalVoxelId]);

        shouldUpdate = true;
        hoveredVoxelChanged = true;
    }

    /**
     * Given a list of voxels to hide (from which we can get the contained mesh indices to hide), iterate over each mesh render item and remove the corresponding indices from its index buffer.
     * Unfortunately, there's no faster way to do this, but it's not terribly slow if we use a set for the indices to hide.
     */
    void hideSelectedMeshFaces(MSubSceneContainer& container) {
        MSubSceneContainer::Iterator* it = container.getIterator();
        it->reset();
        MRenderItem* item = nullptr;

        // Convert voxelsToHide to a map of face indices to hide (where key is face index and value is voxel instance ID)
        std::unordered_map<uint, uint> indicesToHide;
        MFnSingleIndexedComponent faceComponent;
        const MObjectArray& voxelSurfaceFaces = voxelShape->getVoxels().get()->surfaceFaceComponents;
        const MObjectArray& voxelInteriorFaces = voxelShape->getVoxels().get()->interiorFaceComponents;

        auto addIndicesToHide = [&](const MObjectArray& faceComponents, uint voxelInstanceId) {
            faceComponent.setObject(faceComponents[voxelInstanceId]);
            for (int j = 0; j < faceComponent.elementCount(); ++j) {
                int faceIdx = faceComponent.element(j);
                indicesToHide.insert({allMeshIndices[faceIdx * 3 + 0], voxelInstanceId});
                indicesToHide.insert({allMeshIndices[faceIdx * 3 + 1], voxelInstanceId});
                indicesToHide.insert({allMeshIndices[faceIdx * 3 + 2], voxelInstanceId});
            }
        };

        for (uint voxelInstanceId : voxelsToHide) {
            addIndicesToHide(voxelSurfaceFaces, voxelInstanceId);
            addIndicesToHide(voxelInteriorFaces, voxelInstanceId);
        }

        // Now go through each (mesh) render item and remove those indices from its index buffer.
        while ((item = it->next()) != nullptr) {
            if (meshRenderItemIDs.find(item->InternalObjectId()) == meshRenderItemIDs.end()) continue;
            const MString& itemName = item->name();

            // Note: do not get the index buffer from the render item's MGeometry; it seems to be stale / hold an old view of the buffer.
            MIndexBuffer* indexBuffer = meshIndexBuffers[itemName].get();
            uint32_t* indices = static_cast<uint32_t*>(indexBuffer->map());
            std::vector<uint32_t> newIndices;
            newIndices.reserve(indexBuffer->size());
            
            for (unsigned int i = 0; i < indexBuffer->size(); ++i) {
                // Didn't find this index in the set of indices to hide, so keep it.
                if (indicesToHide.find(indices[i]) == indicesToHide.end()) {
                    newIndices.push_back(indices[i]);
                    continue;
                };

                recentlyHiddenVoxels.insert(indicesToHide[indices[i]]);
                recentlyHiddenFaces[itemName].push_back(indices[i]);
            }
            
            indexBuffer->unmap();
            indexBuffer->update(newIndices.data(), 0, static_cast<unsigned int>(newIndices.size()), true /* truncateIfSmaller */);
            updateRenderItemIndexBuffer(item, indexBuffer);
        }
        
        it->destroy();
    }

    /**
     * When recreating an index buffer, or even changing the size of an existing one, it's not sufficient to call the index buffer's update method.
     * We must also re-call setGeometryForRenderItem. This method retrieves the existing buffers from a render item and re-sets them.
     */
    void updateRenderItemIndexBuffer(MRenderItem* const item, const MIndexBuffer* const newIndexBuffer) {
        MVertexBufferArray vertexBuffers;
        const MBoundingBox& bbox = item->boundingBox();
        
        for (int i = 0; i < item->geometry()->vertexBufferCount(); ++i) {
            MVertexBuffer* vb = item->geometry()->vertexBuffer(i);
            vertexBuffers.addBuffer(vb->descriptor().name(), vb);
        }
        setGeometryForRenderItem(*item, vertexBuffers, *newIndexBuffer, &bbox);
    }

    /**
     * Add the hidden (selected) face indices back into the relevant render items' index buffers (by creating a new merged index buffer).
     */
    void showSelectedMeshFaces(MSubSceneContainer& container, RenderItemFaceIdxMap& selectedRenderItemFaces) {

        for (const auto& [itemName, hiddenFaceIdxs] : selectedRenderItemFaces) {
            MRenderItem* item = container.find(itemName);
            if (!item) continue;
            if (hiddenFaceIdxs.size() == 0) continue;

            // Note: do not get the index buffer from the render item's MGeometry; it seems to be stale / hold an old view of the buffer.
            MIndexBuffer* indexBuffer = meshIndexBuffers[itemName].get();
            auto newIndexBuffer = make_unique<MIndexBuffer>(MGeometry::kUnsignedInt32);

            const unsigned int oldCount = indexBuffer->size();
            const unsigned int addCount = static_cast<unsigned int>(hiddenFaceIdxs.size());
            uint32_t* merged = static_cast<uint32_t*>(newIndexBuffer->acquire(oldCount + addCount, true));

            uint32_t* oldData = static_cast<uint32_t*>(indexBuffer->map());
            std::copy(oldData, oldData + oldCount, merged);
            indexBuffer->unmap();
            std::copy(hiddenFaceIdxs.begin(), hiddenFaceIdxs.end(), merged + oldCount);
            newIndexBuffer->commit(merged);
            
            meshIndexBuffers[itemName] = std::move(newIndexBuffer);
            updateRenderItemIndexBuffer(item, meshIndexBuffers[itemName].get());
        }

        selectedRenderItemFaces.clear();
    }

    /**
     * Create new instanced transform arrays for the voxel render items, excluding any hidden voxels.
     */
    void hideSelectedVoxels(MSubSceneContainer& container) {
        MMatrixArray visibleVoxelMatrices;

        // First of all, the selection highlight render items should show 0 voxels now, so use the cleared array.
        updateVoxelRenderItem(container, voxelSelectedHighlightItemName, visibleVoxelMatrices);
        updateVoxelRenderItem(container, voxelPreviewSelectionHighlightItemName, visibleVoxelMatrices);

        // Filter the voxel matrices array to exclude any hidden voxels.
        const MMatrixArray& allVoxelMatrices = voxelShape->getVoxels().get()->modelMatrices;
        std::vector<uint> newVisibleVoxelIdToGlobalId;
        newVisibleVoxelIdToGlobalId.reserve(visibleVoxelIdToGlobalId.size() - voxelsToHide.size());

        for (size_t i = 0; i < visibleVoxelIdToGlobalId.size(); ++i) {
            uint globalVoxelId = visibleVoxelIdToGlobalId[i];
            if (voxelsToHide.find(globalVoxelId) != voxelsToHide.end()) continue;

            visibleVoxelMatrices.append(allVoxelMatrices[globalVoxelId]);
            newVisibleVoxelIdToGlobalId.push_back(globalVoxelId);
        }

        visibleVoxelIdToGlobalId = std::move(newVisibleVoxelIdToGlobalId);

        updateVoxelRenderItem(container, voxelWireframeRenderItemName, visibleVoxelMatrices);
        updateVoxelRenderItem(container, voxelSelectionRenderItemName, visibleVoxelMatrices);

        voxelsToHide.clear();
    }

    /**
     * Create new instanced transform arrays for the voxel render items, including currently visible voxels
     * plus any selected hidden ones.
     */
    void showSelectedVoxels(MSubSceneContainer& container, std::unordered_set<uint>& selectedVoxels, bool highlightSelected = true) {
        MMatrixArray visibleVoxelMatrices;
        MMatrixArray selectedVoxelMatrices;

        const MMatrixArray& allVoxelMatrices = voxelShape->getVoxels().get()->modelMatrices;

        for (uint voxelId : selectedVoxels) {
            if (highlightSelected) selectedVoxelMatrices.append(allVoxelMatrices[voxelId]);
            visibleVoxelIdToGlobalId.push_back(voxelId);
        }

        std::sort(visibleVoxelIdToGlobalId.begin(), visibleVoxelIdToGlobalId.end());

        for (uint globalVoxelId : visibleVoxelIdToGlobalId) {
            visibleVoxelMatrices.append(allVoxelMatrices[globalVoxelId]);
        }

        updateVoxelRenderItem(container, voxelSelectedHighlightItemName, selectedVoxelMatrices);
        updateVoxelRenderItem(container, voxelWireframeRenderItemName, visibleVoxelMatrices);
        updateVoxelRenderItem(container, voxelSelectionRenderItemName, visibleVoxelMatrices);

        selectedVoxels.clear();
    }

    void updateVoxelRenderItem(MSubSceneContainer& container, const MString& itemName, const MMatrixArray& voxelMatrices) {
        MRenderItem* item = container.find(itemName);
        bool enabled = voxelRenderItemsEnabledState[itemName] && voxelMatrices.length() > 0;
        item->enable(enabled);

        if (voxelMatrices.length() == 0) return; // Maya doesn't like setting empty instance arrays.
        setInstanceTransformArray(*item, voxelMatrices);
    }

    MShaderInstance* getVertexBufferDescriptorsForShader(const MObject& shaderNode, const MDagPath& geomDagPath, MVertexBufferDescriptorList& vertexBufferDescriptors) {
        MRenderer* renderer = MRenderer::theRenderer();
        if (!renderer) return nullptr;

        const MShaderManager* shaderManager = renderer->getShaderManager();
        if (!shaderManager) return nullptr;

        MShaderInstance* shaderInstance = shaderManager->getShaderFromNode(shaderNode, geomDagPath);
        if (!shaderInstance) return nullptr;

        shaderInstance->requiredVertexBuffers(vertexBufferDescriptors);

        return shaderInstance;
    }

    MObject getShaderNodeFromShadingSet(const MObject& shadingSet) {
        MFnDependencyNode fnShadingSet(shadingSet);
        MPlug shaderPlug = fnShadingSet.findPlug("surfaceShader", true); // TODO: support for other shaders? (There's surface, volume, and displacement)
        MPlugArray conns;
        if (shaderPlug.isNull() || !shaderPlug.connectedTo(conns, true, false) || conns.length() == 0) return MObject::kNullObj;
        return conns[0].node(); // API returns a plug array but there can only be one shader connected.
    }

    MObjectArray getShadingSetFaceComponents(const MObjectArray& shadingSets, const MIntArray& faceIdxToShader) {
        MObjectArray shadingSetFaceComponents;
        shadingSetFaceComponents.setLength(shadingSets.length());
        MFnSingleIndexedComponent fnFaceComponent;

        for (uint i = 0; i < shadingSets.length(); ++i) {
            shadingSetFaceComponents[i] = fnFaceComponent.create(MFn::kMeshPolygonComponent);
        }

        for (uint i = 0; i < faceIdxToShader.length(); ++i) {
            int shadingSetIdx = faceIdxToShader[i];
            if (shadingSetIdx < 0 || shadingSetIdx >= (int)shadingSets.length()) continue;
            
            fnFaceComponent.setObject(shadingSetFaceComponents[shadingSetIdx]);
            fnFaceComponent.addElement(i);
        }

        return shadingSetFaceComponents;
    }

    void buildGeometryRequirements(
        const MObjectArray& shadingSets, 
        const MObjectArray& shadingSetFaceComponents,
        const MDagPath& originalGeomPath,
        MGeometryRequirements& geomReqs,
        std::vector<RenderItemInfo>& renderItemInfos
    ) {
        MFnSingleIndexedComponent fnFaceComponent;
        // Need to deduplicate requirements across shaders (e.g. two shaders may both request POSITION)
        std::unordered_set<MString, Utils::MStringHash, Utils::MStringEq> existingVBRequirements;

        for (uint i = 0; i < shadingSets.length(); ++i) {
            fnFaceComponent.setObject(shadingSetFaceComponents[i]);
            if (fnFaceComponent.elementCount() == 0) continue;

            MObject shaderNode = getShaderNodeFromShadingSet(shadingSets[i]);
            if (shaderNode.isNull()) continue;

            MVertexBufferDescriptorList vbDescList;
            MShaderInstance* shaderInstance = getVertexBufferDescriptorsForShader(shaderNode, originalGeomPath, vbDescList);
            if (!shaderInstance) continue;

            for (int j = 0; j < vbDescList.length(); ++j) {
                MVertexBufferDescriptor vbDesc;
                if (!vbDescList.getDescriptor(j, vbDesc)) continue;
                if (existingVBRequirements.find(vbDesc.semanticName()) != existingVBRequirements.end()) continue;

                existingVBRequirements.insert(vbDesc.semanticName());
                geomReqs.addVertexRequirement(vbDesc);
            }

            MIndexBufferDescriptor indexDesc(
                MIndexBufferDescriptor::kTriangle,
                MString(), // unused for kTriangle
                MGeometry::kTriangles,
                0, // unused for kTriangle
                shadingSetFaceComponents[i]
            );

            geomReqs.addIndexingRequirement(indexDesc);
            renderItemInfos.emplace_back(indexDesc, shaderInstance, "voxelRenderItem_" + MFnDependencyNode(shadingSets[i]).name());
        }
    }

    void createMeshVertexBuffer(const MVertexBufferDescriptor& vbDesc, const MGeometryExtractor& extractor, uint vertexCount, MVertexBufferArray& vertexBufferArray) {
        auto vertexBuffer = make_unique<MVertexBuffer>(vbDesc);
        const MGeometry::Semantic semantic = vbDesc.semantic();
        
        // Position and normal buffers need to be created with flags for binding (write-ably) to a DX11 compute shader (for the deform vertex compute step).
        // So create them as DX11 buffers with the unordered access flag, then pass the underlying resource handle to the Maya MVertexBuffer.
        if (semantic == MGeometry::kPosition || semantic == MGeometry::kNormal) {
            ComPtr<ID3D11Buffer>& buffer = (semantic == MGeometry::kPosition) ? positionsBuffer : normalsBuffer;
            
            // Create the buffer (cannot be a structured buffer due to bind flags Maya has set. Also requires R32_FLOAT format for views).
            std::vector<float> data(vertexCount * vbDesc.dimension(), 0.0f);
            extractor.populateVertexBuffer(data.data(), vertexCount, vbDesc);
            buffer = DirectX::createReadWriteBuffer(data, false, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
            vertexBuffer->resourceHandle(buffer.Get(), data.size());

            ComPtr<ID3D11UnorderedAccessView>& uav = (semantic == MGeometry::kPosition) ? positionsUAV : normalsUAV;
            uav = DirectX::createUAV(buffer, vertexCount * vbDesc.dimension(), 0, DXGI_FORMAT_R32_FLOAT);

            // Also need to create a buffer with the original positions/normals for the deform shader to read from
            ComPtr<ID3D11Buffer>& originalBuffer = (semantic == MGeometry::kPosition) ? originalPositionsBuffer : originalNormalsBuffer;
            originalBuffer = DirectX::createReadOnlyBuffer(data, true, 0, sizeof(float) * vbDesc.dimension());
            
            ComPtr<ID3D11ShaderResourceView>& originalSRV = (semantic == MGeometry::kPosition) ? originalPositionsSRV : originalNormalsSRV;
            originalSRV = DirectX::createSRV(originalBuffer);
        }
        else {
            void* data = vertexBuffer->acquire(vertexCount, true);
            extractor.populateVertexBuffer(data, vertexCount, vbDesc);
            vertexBuffer->commit(data);
        }
        
        vertexBufferArray.addBuffer(vbDesc.name(), vertexBuffer.get());
        meshVertexBuffers.push_back(std::move(vertexBuffer));
    }

    MIndexBuffer* createMeshIndexBuffer(const RenderItemInfo& itemInfo, const MGeometryExtractor& extractor) {
        unsigned int numTriangles = extractor.primitiveCount(itemInfo.indexDesc);
        if (numTriangles == 0) return nullptr;
    
        auto indexBuffer = make_unique<MIndexBuffer>(MGeometry::kUnsignedInt32);
        void* indexData = indexBuffer->acquire(3 * numTriangles, true);

        extractor.populateIndexBuffer(indexData, numTriangles, itemInfo.indexDesc);
        indexBuffer->commit(indexData);

        MIndexBuffer* rawIndexBuffer = indexBuffer.get();
        meshIndexBuffers[itemInfo.renderItemName] = std::move(indexBuffer);
        
        return rawIndexBuffer;
    }

    MRenderItem* createSingleMeshRenderItem(MSubSceneContainer& container, const RenderItemInfo& itemInfo) {
        MRenderItem* renderItem = container.find(itemInfo.renderItemName);
        if (renderItem) return renderItem;

        renderItem = MRenderItem::Create(itemInfo.renderItemName, MRenderItem::MaterialSceneItem, MGeometry::kTriangles);
        renderItem->setDrawMode(static_cast<MGeometry::DrawMode>(MGeometry::kShaded | MGeometry::kTextured));
        renderItem->setWantConsolidation(false);
        renderItem->castsShadows(true);
        renderItem->receivesShadows(true);
        renderItem->setShader(itemInfo.shaderInstance);
        container.add(renderItem);

        meshRenderItemIDs.insert(renderItem->InternalObjectId());
        releaseShaderInstance(itemInfo.shaderInstance);
        return renderItem;
    }

    void releaseShaderInstance(MShaderInstance* shaderInstance) {
        MRenderer* renderer = MRenderer::theRenderer();
        if (!renderer) return;

        const MShaderManager* shaderManager = renderer->getShaderManager();
        if (!shaderManager) return;

        shaderManager->releaseShader(shaderInstance);
    }

    unsigned int getAllMeshIndices(
        const MGeometryExtractor& extractor
    ) {
        // No face component arg --> whole mesh
        MIndexBufferDescriptor indexDesc(MIndexBufferDescriptor::kTriangle, MString(), MGeometry::kTriangles, 0);
        const unsigned int primitiveCount = extractor.primitiveCount(indexDesc);
        allMeshIndices.resize(primitiveCount * 3);
        extractor.populateIndexBuffer(allMeshIndices.data(), primitiveCount, indexDesc);

        return extractor.vertexCount(); 
    }

    // This extracts a mapping from the geometry extractor vertices to the original mesh vertices.
    // The two are not 1:1 because the geometry extractor may have to split vertices to satisfy per-face shader requirements (split normals, UVs, etc).
    // Right now, we use this primarily during simulation export. (See VoxelShape.h and DeformVerticesCompute.h)
    void getVertexIdMapping(
        const MGeometryExtractor& extractor
    ) {
        // The data must be requested as floats, but we'll cast and store as uints.
        MVertexBufferDescriptor vertexIdDesc("", MGeometry::kTexture, MGeometry::kFloat, 1);
        vertexIdDesc.setSemanticName("vertexid");

        unsigned int vertexCount = extractor.vertexCount();
        std::vector<float> data(vertexCount, 0.0f);
        extractor.populateVertexBuffer(data.data(), vertexCount, vertexIdDesc);

        for (unsigned int i = 0; i < vertexCount; ++i) {
            extractedVertexIdMap.push_back(static_cast<uint>(data[i]));
        }
    }

    void updateSelectionGranularity(
		const MDagPath& path,
		MSelectionContext& selectionContext) 
    {
        // TODO: possibly change based on edit mode
        selectionContext.setSelectionLevel(MHWRender::MSelectionContext::kComponent);    
    }

    void createVoxelWireframeRenderItem(MSubSceneContainer& container) {
        MRenderItem* renderItem = MRenderItem::Create(voxelWireframeRenderItemName, MRenderItem::DecorationItem, MGeometry::kLines);
        MShaderInstance* shader = MRenderer::theRenderer()->getShaderManager()->getStockShader(MShaderManager::k3dSolidShader);
        const float solidColor[] = {0.0f, 1.0f, 0.25f, 1.0f};
        shader->setParameter("solidColor", solidColor);

        renderItem->setDrawMode(static_cast<MGeometry::DrawMode>(MGeometry::kWireframe | MGeometry::kShaded | MGeometry::kTextured));
        renderItem->depthPriority(MRenderItem::sActiveWireDepthPriority);
        renderItem->setWantConsolidation(false);
        renderItem->setHideOnPlayback(true);
        renderItem->setShader(shader);
        container.add(renderItem);

        setVoxelGeometryForRenderItem(*renderItem, MGeometry::kLines);

        const MMatrixArray& voxelInstanceTransforms = voxelShape->getVoxels().get()->modelMatrices;
        setInstanceTransformArray(*renderItem, voxelInstanceTransforms);
    }

    void createVoxelSelectionRenderItem(MSubSceneContainer& container) {
        MRenderItem* renderItem = MRenderItem::Create(voxelSelectionRenderItemName, MRenderItem::DecorationItem, MGeometry::kTriangles);
        MShaderInstance* shader = MRenderer::theRenderer()->getShaderManager()->getStockShader(MShaderManager::k3dDefaultMaterialShader);
        MSharedPtr<MUserData> customData(new SelectionCustomData(
            std::bind(&VoxelSubSceneOverride::onHoveredVoxelChange, this, std::placeholders::_1)
        ));

        MSelectionMask selMask;
        selMask.addMask(MSelectionMask::kSelectMeshFaces);
        selMask.addMask(MSelectionMask::kSelectMeshVerts);
        selMask.addMask(MSelectionMask::kSelectMeshes);

        renderItem->setDrawMode(MGeometry::kSelectionOnly);
        renderItem->setSelectionMask(selMask);
        renderItem->depthPriority(MRenderItem::sSelectionDepthPriority);
        renderItem->setWantConsolidation(false);
        renderItem->setHideOnPlayback(true);
        renderItem->setShader(shader);
        renderItem->setCustomData(customData);
        container.add(renderItem);

        setVoxelGeometryForRenderItem(*renderItem, MGeometry::kTriangles);

        const MMatrixArray& voxelInstanceTransforms = voxelShape->getVoxels().get()->modelMatrices;
        setInstanceTransformArray(*renderItem, voxelInstanceTransforms);
    }

    void createVoxelSelectedHighlightRenderItem(MSubSceneContainer& container, const MString& renderItemName, const std::array<float, 4>& color) {
        MRenderItem* renderItem = MRenderItem::Create(renderItemName, MRenderItem::DecorationItem, MGeometry::kTriangles);
        MShaderInstance* shader = MRenderer::theRenderer()->getShaderManager()->getStockShader(MShaderManager::k3dSolidShader);
        shader->setParameter("solidColor", color.data());

        renderItem->setDrawMode(static_cast<MGeometry::DrawMode>(MGeometry::kWireframe | MGeometry::kShaded | MGeometry::kTextured));
        renderItem->depthPriority(MRenderItem::sActivePointDepthPriority);
        renderItem->setWantConsolidation(false);
        renderItem->setHideOnPlayback(true);
        renderItem->setShader(shader);
        container.add(renderItem);

        setVoxelGeometryForRenderItem(*renderItem, MGeometry::kTriangles);
    }

    void sendVoxelInfoToPaintRenderOp(VoxelEditMode paintMode) {
        VoxelRendererOverride* voxelRendererOverride = VoxelRendererOverride::instance();
        if (!voxelRendererOverride) return;

        const MMatrixArray& voxelMatrices = voxelShape->getVoxels().get()->modelMatrices;
        PingPongView& paintView = voxelShape->getPaintView(paintMode);
        float particleRadius = voxelShape->getVoxels()->voxelSize * 0.25f;

        voxelRendererOverride->sendVoxelInfoToPaintRenderOp(paintMode, voxelMatrices, visibleVoxelIdToGlobalId, paintView, particleRadius);
    }

    void createVoxelGeometryBuffers() {
        voxelIndexBuffers.clear();

        MVertexBufferDescriptor posDesc("", MGeometry::kPosition, MGeometry::kFloat, 3);
        voxelVertexBuffer = make_unique<MVertexBuffer>(posDesc);
        float* posData = static_cast<float*>(voxelVertexBuffer->acquire(8, true));
        std::copy(cubeCornersFlattened.begin(), cubeCornersFlattened.end(), posData);
        voxelVertexBuffer->commit(posData);

        auto makeIndexBuffer = [&](MGeometry::Primitive prim, const auto& src) {
            auto buf = make_unique<MIndexBuffer>(MGeometry::kUnsignedInt32);
            uint32_t* data = static_cast<uint32_t*>(buf->acquire(static_cast<uint>(src.size()), true));
            for (size_t i = 0; i < src.size(); ++i) {
                data[i] = static_cast<uint32_t>(src[i]);
            }
            buf->commit(data);
            voxelIndexBuffers[prim] = std::move(buf);
        };

        makeIndexBuffer(MGeometry::kTriangles, cubeFacesFlattened);
        makeIndexBuffer(MGeometry::kLines, cubeEdgesFlattened);
        makeIndexBuffer(MGeometry::kPoints, cubeCornersFlattened);
    }

    void setVoxelGeometryForRenderItem(
        MRenderItem& renderItem,
        MGeometry::Primitive primitiveType
    ) {
        MVertexBufferArray vbArray;
        vbArray.addBuffer("", voxelVertexBuffer.get());
        const MBoundingBox bounds(MPoint(-0.5, -0.5, -0.5), MPoint(0.5, 0.5, 0.5));
        setGeometryForRenderItem(renderItem, vbArray, *voxelIndexBuffers[primitiveType].get(), &bounds);
    }

    void setMeshRenderItemsVisibility(
        MSubSceneContainer& container,
        bool visible
    ) {
        MSubSceneContainer::Iterator* it = container.getIterator();
        it->reset();
        MRenderItem* item = nullptr;

        while ((item = it->next()) != nullptr) {
            if (meshRenderItemIDs.find(item->InternalObjectId()) == meshRenderItemIDs.end()) continue;
            item->enable(visible);
        }

        it->destroy();
    }

    /**
     * Creates the actual, visible, voxelized mesh render items (multiple, possibly, if the original, unvoxelized mesh has multiple shaders / face sets).
     */
    void createMeshRenderItems(MSubSceneContainer& container) {
        MStatus status;
        meshVertexBuffers.clear();
        meshIndexBuffers.clear();
        meshRenderItemIDs.clear();
        allMeshIndices.clear();

        const MDagPath originalGeomPath = voxelShape->pathToOriginalGeometry();
        MFnMesh originalMeshFn(originalGeomPath.node());
        if (originalMeshFn.numVertices() == 0) return;
        
        // Get all shaders from the original mesh. It will tell us the required vertex buffers,
        // and its mapping of faces to shaders will tell us how to create index buffers and render items.
        MObjectArray shadingSets;
        MIntArray faceIdxToShader;
        status = originalMeshFn.getConnectedShaders(originalGeomPath.instanceNumber(), shadingSets, faceIdxToShader);
        if (status != MStatus::kSuccess) return;
        MObjectArray shadingSetFaceComponents = getShadingSetFaceComponents(shadingSets, faceIdxToShader);

        // Extract the geometry requirements (vertex and index buffer descriptors) from the shaders.
        // Then use MGeometryExtractor to extract the vertex and index buffers from the original mesh.
        MGeometryRequirements geomReqs;
        std::vector<RenderItemInfo> renderItemInfos;
        renderItemInfos.reserve(shadingSets.length());
        buildGeometryRequirements(shadingSets, shadingSetFaceComponents, originalGeomPath, geomReqs, renderItemInfos);
        MGeometryExtractor extractor(geomReqs, originalGeomPath, kPolyGeom_Normal, &status);
        if (status != MStatus::kSuccess) return;

        MVertexBufferArray vertexBufferArray;        
        const unsigned int vertexCount = extractor.vertexCount(); 
        const MVertexBufferDescriptorList& vbDescList = geomReqs.vertexRequirements();
        for (int i = 0; i < vbDescList.length(); ++i) {
            MVertexBufferDescriptor vbDesc;
            if (!vbDescList.getDescriptor(i, vbDesc)) continue;

            createMeshVertexBuffer(vbDesc, extractor, vertexCount, vertexBufferArray);
        }

        // Create an index buffer + render item for each shading set of the original mesh (which corresponds to an indexing requirement)
        // Use an effectively infinite bounding box because the voxel shape can deform and shatter.
        double bound = 1e10;
        const MBoundingBox bounds(MPoint(-bound, -bound, -bound), MPoint(bound, bound, bound));
        for (const RenderItemInfo& itemInfo : renderItemInfos) {
            MIndexBuffer* rawIndexBuffer = createMeshIndexBuffer(itemInfo, extractor);
            if (!rawIndexBuffer) continue;

            MRenderItem* renderItem = createSingleMeshRenderItem(container, itemInfo);
            setGeometryForRenderItem(*renderItem, vertexBufferArray, *rawIndexBuffer, &bounds);
        }

        // The voxel shape needs the whole mesh's vertex indices to tag each vertex with the voxel it belongs to.
        // It's important to do the tagging using the vertex buffer that MGeometryExtractor provides.
        unsigned int numVertices = getAllMeshIndices(extractor);
        getVertexIdMapping(extractor);
                
        voxelShape->initializeDeformVerticesCompute(
            allMeshIndices,
            extractedVertexIdMap,
            numVertices,
            positionsUAV,
            normalsUAV,
            originalPositionsSRV, 
            originalNormalsSRV
        );
    }

public:
    inline static MString drawDbClassification = "drawdb/subscene/voxelSubsceneOverride";
    inline static MString drawRegistrantId = "VoxelSubSceneOverridePlugin";

    static MPxSubSceneOverride* creator(const MObject& obj) 
    {
        return new VoxelSubSceneOverride(obj);
    }

    /**
     * Overriding this to tell Maya that any instance of a render item that gets selected still belongs
     * to the same original shape node.
     */
    bool getInstancedSelectionPath(
        const MRenderItem& renderItem, 
        const MIntersection& intersection, 
        MDagPath& dagPath) const override
    {
        if (!voxelShape) return false;
        MFnDagNode fnDag(voxelShapeObj);
        fnDag.getPath(dagPath);
        return true;
    }

    ~VoxelSubSceneOverride() override {
        if (positionsBuffer) {
            DirectX::notifyMayaOfMemoryUsage(positionsBuffer);
            positionsBuffer.Reset();
        }

        if (normalsBuffer) {
            DirectX::notifyMayaOfMemoryUsage(normalsBuffer);
            normalsBuffer.Reset();
        }

        if (originalPositionsBuffer) {
            DirectX::notifyMayaOfMemoryUsage(originalPositionsBuffer);
            originalPositionsBuffer.Reset();
        }

        if (originalNormalsBuffer) {
            DirectX::notifyMayaOfMemoryUsage(originalNormalsBuffer);
            originalNormalsBuffer.Reset();
        }

        unsubscribeFromVoxelEditModeChanges();
        MEventMessage::removeCallbacks(callbackIds);
        MCommandMessage::removeCallback(showHideCallbackId);
        showHideCallbackId = 0;
    }
    
    DrawAPI supportedDrawAPIs() const override {
        return kDirectX11;
    }

    bool requiresUpdate(
        const MSubSceneContainer& container,
        const MFrameContext& frameContext) const override
    {
        bool rebuildGeometry = voxelShape->requiresGeometryRebuild();
        bool meshVisibilityUpdate = voxelShape->requiresMeshVisibilityUpdate();

        return shouldUpdate || rebuildGeometry || meshVisibilityUpdate;
    }

    /**
     * This method is responsible for populating the MSubSceneContainer with render items. In our case, we want the our custom VoxelShape
     * to have the same geometry, topology, and shading as the original mesh it deforms. To do so, we use the shading sets of the original mesh
     * to tell us what geometry requirements we need to extract and recreate here.
     */
    void update(MSubSceneContainer& container, const MFrameContext& frameContext) override
    {
        if (!voxelShape) return;

        if (voxelShape->requiresGeometryRebuild()) {
            container.clear();
            voxelShape->clearGeometryRebuildFlag();
            editModeChanged = true;
        }
        
        if (voxelShape->requiresMeshVisibilityUpdate()) {
            voxelShape->clearMeshVisibilityUpdateFlag();
            bool visible = !(MPlug(voxelShapeObj, VoxelShape::aExporting).asBool());
            setMeshRenderItemsVisibility(container, visible);
        }

        if (container.count() <= 0) {
            recentlyHiddenFaces.clear();
            recentlyHiddenVoxels.clear();
            hiddenFaces.clear();
            hiddenVoxels.clear();

            // Initialize the visibleVoxelIdToGlobalId map to a 1:1 mapping, which will then be updated as voxels get (un)hidden.
            int numVoxels = voxelShape->getVoxels().get()->numOccupied;
            visibleVoxelIdToGlobalId.resize(numVoxels);
            std::iota(visibleVoxelIdToGlobalId.begin(), visibleVoxelIdToGlobalId.end(), 0);

            // The render items for the actual, voxelized mesh.
            createMeshRenderItems(container); 
            // Geometry buffers for a simple unit cube, reused for all voxel render items.
            createVoxelGeometryBuffers();
            // The visible wireframe render item
            createVoxelWireframeRenderItem(container);
            // Invisible item, only gets drawn to the selection buffer to enable selection
            createVoxelSelectionRenderItem(container);
            // Shows highlights for selected voxels
            createVoxelSelectedHighlightRenderItem(container, voxelSelectedHighlightItemName, {0.0f, 1.0f, 0.25f, 0.5f});
            // Shows highlight for hovered voxel
            createVoxelSelectedHighlightRenderItem(container, voxelPreviewSelectionHighlightItemName, {1.0f, 1.0f, 0.0f, 0.5f});
        }

        if (editModeChanged) {
            for (const auto& [itemName, enabled] : voxelRenderItemsEnabledState) {
                MRenderItem* item = container.find(itemName);
                item->enable(enabled);
            }

            // Special case: the edit mode may dictate that the preview highlight is enabled, but there may be no hovered voxel, so give the chance to re-disable it.
            updateVoxelRenderItem(container, voxelPreviewSelectionHighlightItemName, hoveredVoxelMatrices);
            editModeChanged = false;
        }
        
        if (selectionChanged) {
            updateVoxelRenderItem(container, voxelSelectedHighlightItemName, selectedVoxelMatrices);
            selectionChanged = false;
        }

        if (hoveredVoxelChanged) {
            updateVoxelRenderItem(container, voxelPreviewSelectionHighlightItemName, hoveredVoxelMatrices);
            hoveredVoxelChanged = false;
        }

        switch (showHideStateChange)
        {
        case ShowHideStateChange::None:
            break;
        case ShowHideStateChange::ShowAll:
            invalidateRecentlyHidden(this);
            showSelectedMeshFaces(container, hiddenFaces);
            showSelectedVoxels(container, hiddenVoxels, false);
            break;
        case ShowHideStateChange::ShowSelected:
            showSelectedMeshFaces(container, recentlyHiddenFaces);
            showSelectedVoxels(container, recentlyHiddenVoxels);
            invalidateRecentlyHidden(this);
            break;
        case ShowHideStateChange::HideSelected:
            hideSelectedMeshFaces(container);
            hideSelectedVoxels(container);
            break;
        }
        showHideStateChange = ShowHideStateChange::None;

        shouldUpdate = false;
    }
};
