#pragma once
#include <maya/MPxSurfaceShape.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnSingleIndexedComponent.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MCallbackIdArray.h>
#include <maya/MNodeMessage.h>
#include <maya/MFnUnitAttribute.h>
#include "../../voxelizer.h"
#include "../usernodes/pbdnode.h"
#include "../data/particledata.h"
#include "../data/d3d11data.h"
#include "../data/voxeldata.h"
#include "../../directx/compute/deformverticescompute.h"
#include "../../directx/compute/paintdeltacompute.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "directx/directx.h"
#include "../../utils.h"
#include "../tools/voxelpaintcontext.h"
#include "../../directx/pingpongview.h"
using Microsoft::WRL::ComPtr;

class VoxelShape : public MPxSurfaceShape {
    
public:
    inline static MTypeId id = { 0x0012A3B4 };
    inline static MString typeName = "VoxelShape";
    inline static MString drawDbClassification = "drawdb/subscene/voxelSubsceneOverride/voxelshape";
    inline static MString exportDummyTimeAttrName = "exportDummyTime";
    
    inline static MObject aInputGeom;
    inline static MObject aParticleSRV;
    inline static MObject aParticleData;
    inline static MObject aVoxelData;
    inline static MObject aTrigger;
    inline static MObject aInteriorMaterial;
    inline static MObject aExporting; // Used to indicate that an export is in progress

    static void* creator() { return new VoxelShape(); }
    
    static MStatus initialize() {
        MStatus status;
        MFnTypedAttribute tAttr;
        aInputGeom = tAttr.create("inMesh", "in", MFnData::kMesh, MObject::kNullObj, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        tAttr.setStorable(false);
        tAttr.setReadable(false);
        tAttr.setWritable(true);
        status = addAttribute(aInputGeom);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        // Contains the particle positions (on the CPU) and a few other things not used by this node.
        aParticleData = tAttr.create("particleData", "pdt", ParticleData::id, MObject::kNullObj, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        tAttr.setStorable(false); // NOT storable - just for initialization
        tAttr.setWritable(true);
        tAttr.setReadable(false); 
        status = addAttribute(aParticleData);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        aParticleSRV = tAttr.create("particleSRV", "psrv", D3D11Data::id, MObject::kNullObj, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        tAttr.setStorable(false);
        tAttr.setWritable(true);
        tAttr.setReadable(false);
        status = addAttribute(aParticleSRV);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        aVoxelData = tAttr.create("voxelData", "vxd", VoxelData::id, MObject::kNullObj, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        tAttr.setStorable(false);
        tAttr.setWritable(true);
        tAttr.setReadable(false);
        status = addAttribute(aVoxelData);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        // This is the output of the PBD sim node, which is just used to trigger evaluation of the deformer.
        MFnNumericAttribute nAttr;
        aTrigger = nAttr.create("trigger", "trg", MFnNumericData::kBoolean, false, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        nAttr.setStorable(false);
        nAttr.setWritable(true);
        nAttr.setReadable(false);
        status = addAttribute(aTrigger);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        aInteriorMaterial = tAttr.create("interiorMaterial", "intmat", MFnData::kString, MObject::kNullObj, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        tAttr.setStorable(true);
        tAttr.setWritable(true);
        tAttr.setReadable(true);
        status = addAttribute(aInteriorMaterial);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        aExporting = nAttr.create("exporting", "exp", MFnNumericData::kBoolean, false, &status);
        CHECK_MSTATUS_AND_RETURN_IT(status);
        nAttr.setStorable(false);
        nAttr.setWritable(true);
        nAttr.setReadable(false);
        status = addAttribute(aExporting);
        CHECK_MSTATUS_AND_RETURN_IT(status);

        return MS::kSuccess;
    }
   
    static MObject createVoxelShapeNode(const MObject& pbdNodeObj, const MDagPath& voxelTransformDagPath) {
        MStatus status;
        MObject voxelTransform = voxelTransformDagPath.node();
        MDagPath voxelMeshDagPath = voxelTransformDagPath;
        status = voxelMeshDagPath.extendToShape();

        // Create the new shape under the existing transform
        MObject newShapeObj = Utils::createDagNode(typeName, voxelTransform);

        // Relegate the old shape to an intermediate object
        MFnDagNode oldShapeDagNode(voxelMeshDagPath);
        oldShapeDagNode.setIntermediateObject(true);

        // Add a time-driven dummy attribute for use during export so that AbcExport sees the mesh as time-dynamic.
        // Otherwise it will export a static mesh.
        MFnUnitAttribute uAttr;
        MObject dummyTimeAttr = uAttr.create(exportDummyTimeAttrName, "edt", MFnUnitAttribute::kTime, 0.0);
        MFnDependencyNode(voxelMeshDagPath.node()).addAttribute(dummyTimeAttr);
        
        Utils::connectPlugs(voxelMeshDagPath.node(), MString("outMesh"), newShapeObj, aInputGeom);
        Utils::connectPlugs(pbdNodeObj, PBDNode::aTriggerOut, newShapeObj, aTrigger);
        Utils::connectPlugs(pbdNodeObj, PBDNode::aParticleData, newShapeObj, aParticleData);
        Utils::connectPlugs(pbdNodeObj, PBDNode::aParticleSRV, newShapeObj, aParticleSRV);
        Utils::connectPlugs(pbdNodeObj, PBDNode::aVoxelDataOut, newShapeObj, aVoxelData);

        return newShapeObj;
    }

    /**
     * Since this shape can shatter, and grow unbounded, it doesn't really make sense to return a bounding box.
     * Note that, in the subscene override, we do need to pass in some bounds - so we use an effectively infinite bounding box there.
     */
    bool isBounded() const override { return false; }

    MDagPath pathToOriginalGeometry() const {
        MPlug inPlug(thisMObject(), aInputGeom);
        if (inPlug.isNull()) return MDagPath();

        MPlugArray sources;
        if (!inPlug.connectedTo(sources, true, false) || sources.length() <= 0) return MDagPath();

        const MPlug& srcPlug = sources[0];
        MObject srcNode = srcPlug.node();
        if (srcNode.isNull() || !srcNode.hasFn(MFn::kMesh)) return MDagPath();

        MDagPath srcDagPath;
        MStatus status = MDagPath::getAPathTo(srcNode, srcDagPath);
        if (status != MS::kSuccess) return MDagPath();

        return srcDagPath;
    }
    
    bool excludeAsPluginShape() const override {
        // Always display this shape in the outliner, even when plugin shapes are excluded.
        return false;
    }
    
    MSharedPtr<Voxels> getVoxels() {
        Utils::PluginData<VoxelData> voxelData(thisMObject(), aVoxelData);
        return voxelData.get()->getVoxels();
    }

    MSelectionMask getShapeSelectionMask() const override {
        return MSelectionMask::kSelectMeshes;
    }

    MSelectionMask getComponentSelectionMask() const override {
        MSelectionMask mask;
        mask.addMask(MSelectionMask::kSelectMeshFaces);
        mask.addMask(MSelectionMask::kSelectMeshVerts);
        return mask;
    }

    /**
     * Invoked by the subscene override after it has created geometry buffers to fulfill shader requirements.
     * In addition to the GPU resources it passes in, we need to pull CPU-side data from this node's connected plugs and
     * upload them to the GPU (done in the constructor of DeformVerticesCompute).
     */
    void initializeDeformVerticesCompute(
        const std::vector<uint>& vertexIndices,
        const std::vector<uint>& exportVertexIdMap, // used for export only
        const unsigned int numVertices,
        const ComPtr<ID3D11UnorderedAccessView>& positionsUAV,
        const ComPtr<ID3D11UnorderedAccessView>& normalsUAV,
        const ComPtr<ID3D11ShaderResourceView>& originalPositionsUAV,
        const ComPtr<ID3D11ShaderResourceView>& originalNormalsSRV
    ) {

        std::vector<uint> vertexVoxelIds = getVoxelIdsForVertices(vertexIndices, numVertices, getVoxels());

        Utils::PluginData<ParticleData> particleData(thisMObject(), aParticleData);
        Utils::PluginData<D3D11Data> particleSRVData(thisMObject(), aParticleSRV);
        const ParticleDataContainer& particleDataContainer = particleData.get()->getData();
        Utils::PluginData<VoxelData> voxelData(thisMObject(), aVoxelData);
        const VoxelizationGrid& voxelGrid = voxelData.get()->getVoxelizationGrid();

        deformVerticesCompute = DeformVerticesCompute(
            particleDataContainer.numParticles,
            numVertices,
            voxelGrid.gridTransform.asRotateMatrix().inverse(),
            *particleDataContainer.particles,
            vertexVoxelIds,
            positionsUAV,
            normalsUAV,
            originalPositionsUAV,
            originalNormalsSRV,
            particleSRVData.get()->getSRV(),
            exportVertexIdMap
        );

        isInitialized = true;
    }

    PingPongView& getPaintView(VoxelEditMode paintMode) {
        if (!paintDeltaBuffer) {
            allocatePaintDeltaBuffer();
        }

        bool facePaintMode = (paintMode == VoxelEditMode::FacePaint);
        PingPongView& paintViews = facePaintMode ? facePaintViews : particlePaintViews;
        if (!paintViews.isInitialized()) {
            int elementsPerVoxel = facePaintMode ? 6 : 8;
            ComPtr<ID3D11Buffer>& paintBufferA = facePaintMode ? facePaintBufferA : particlePaintBufferA;
            ComPtr<ID3D11Buffer>& paintBufferB = facePaintMode ? facePaintBufferB : particlePaintBufferB;
            allocatePaintBuffers(elementsPerVoxel, paintBufferA, paintBufferB, paintViews);
        }

        return paintViews;
    }

    const ComPtr<ID3D11Buffer>& getPaintDeltaBuffer() {
        return paintDeltaBuffer;
    }

    // Invoked by the owning subscene on edit mode changes
    void subscribeToPaintStateChanges(VoxelEditMode paintMode) {
        bool isFacePaintMode = (paintMode == VoxelEditMode::FacePaint);
        PingPongView* paintViews = isFacePaintMode ? &facePaintViews : &particlePaintViews;
        const uint numElements = getVoxels()->numOccupied * (isFacePaintMode ? 6 : 8);
        paintDeltaCompute.setPaintViews(paintViews, numElements);

        unsubPaintStateChanges = VoxelPaintContext::subscribeToPaintDragStateChange([this, paintViews, paintMode](const PaintDragState& paintState) {
            if (paintState.isDragging) {
                // At the beginning of a paint stroke, copy the before-paint values into the delta buffer
                DirectX::copyBufferToBuffer(paintViews->UAV(), paintDeltaUAV);
                return; 
            }

            // At the end of a paint stroke, compute the before-after delta and update the PBD constraints
            paintDeltaCompute.dispatch();
            updatePBDConstraints(paintMode);

            // Records the paint delta for undo/redo. On undo/redo, applies the delta back to the paint values.
            // Necessary to invoke as a MEL command to enable journaling. (Could use MPxToolCommand but this is simpler).
            MString uuidStr = MFnDependencyNode(thisMObject()).uuid().asString();
            MString modeStr = MString() + static_cast<int>(paintMode);
            MString cmd = "applyVoxelPaint -vid \"" + uuidStr + "\" -mod " + modeStr;
            MGlobal::executeCommand(cmd, false, true /* undoable */);
        });
    }

    void unsubscribePaintStateChanges() {
        unsubPaintStateChanges();
    }

    void undoRedoPaint(const std::vector<uint16_t>& paintDelta, int direction, VoxelEditMode paintMode) {
        bool isFacePaintMode = (paintMode == VoxelEditMode::FacePaint);
        PingPongView& paintViews = isFacePaintMode ? facePaintViews : particlePaintViews;

        DirectX::getContext()->UpdateSubresource(paintDeltaBuffer.Get(), 0, nullptr, paintDelta.data(), 0, 0);
        
        // Using the right sign, we can reuse the paint delta compute shader to _apply_ the delta to the paint values.
        paintDeltaCompute.updateSign(direction);
        paintDeltaCompute.dispatch();
        paintDeltaCompute.updateSign(-1); // Reset sign to default (see paintdelta.hlsl)

        // The compute pass writes into the delta buffer - copy it to the face paint buffer to keep them in sync.
        DirectX::copyBufferToBuffer(paintDeltaUAV, paintViews.UAV());

        updatePBDConstraints(paintMode);
    }

    /**
     * Pass the updated paint values to the PBD node to update its face constraints.
     * This may not be the canonical way to have nodes interact, but it needs to happen at a specific moment,
     * not whenever the DG is next evaluated. (There still may be a better way but, if it works...)
     */
    void updatePBDConstraints(VoxelEditMode paintMode) {
        MPlug triggerPlug(thisMObject(), aTrigger);
        PBDNode* pbdNode = static_cast<PBDNode*>(Utils::connectedNode(triggerPlug));
        
        if (paintMode == VoxelEditMode::FacePaint) {
            pbdNode->updateFaceConstraintsWithPaintValues(paintDeltaUAV, facePaintViews.UAV());
        } else {
            pbdNode->updateParticleMassWithPaintValues(paintDeltaUAV, particlePaintViews.UAV());
        }
    }

    bool requiresGeometryRebuild() const {
        return rebuildGeometry;
    }

    void clearGeometryRebuildFlag() {
        rebuildGeometry = false;
    }

private:
    bool isInitialized = false;
    bool isParticleSRVPlugDirty = false;
    bool rebuildGeometry = false;
    MCallbackIdArray callbackIds;
    EventBase::Unsubscribe unsubPaintStateChanges;
    DeformVerticesCompute deformVerticesCompute;
    PaintDeltaCompute paintDeltaCompute;
    // Holds the weight values of each voxel (face or corner), for use with the Voxel Paint tool.
    ComPtr<ID3D11Buffer> facePaintBufferA;
    ComPtr<ID3D11Buffer> facePaintBufferB;
    PingPongView facePaintViews;
    ComPtr<ID3D11Buffer> particlePaintBufferA;
    ComPtr<ID3D11Buffer> particlePaintBufferB;
    PingPongView particlePaintViews;
    ComPtr<ID3D11Buffer> paintDeltaBuffer;
    ComPtr<ID3D11UnorderedAccessView> paintDeltaUAV;

    
    VoxelShape() = default;
    ~VoxelShape() override {
        MMessage::removeCallbacks(callbackIds);
        deformVerticesCompute.reset();
    };

    MStatus compute(const MPlug& plug, MDataBlock& dataBlock) override {
        if (!isInitialized) return MS::kSuccess;
        if (plug != aTrigger) return MS::kUnknownParameter;

        if (isParticleSRVPlugDirty) {
            // The particle SRV has changed, so we need to update the compute shader with the new one.
            MDataHandle d3d11DataHandle = dataBlock.inputValue(aParticleSRV);
            D3D11Data* particleSRVData = static_cast<D3D11Data*>(d3d11DataHandle.asPluginData());
            deformVerticesCompute.setParticlesSRV(particleSRVData->getSRV());
            isParticleSRVPlugDirty = false;
        }

        PBDNode* pbdNode = static_cast<PBDNode*>(Utils::connectedNode(MPlug(thisMObject(), aTrigger)));
        pbdNode->mergeRenderParticles();
        deformVerticesCompute.dispatch();

        // During export, we copy the vertices and normals back to the original mesh because AbcExport doesn't support custom shapes.
        MDataHandle exportingHandle = dataBlock.inputValue(aExporting);
        bool isExporting = exportingHandle.asBool();
        if (isExporting) {
            deformVerticesCompute.copyGeometryDataToMesh(pathToOriginalGeometry());
        }

        return MS::kSuccess;
    }

    MPxNode::SchedulingType schedulingType() const override {
        // Evaluated serially amongst nodes of the same type
        // Necessary because Maya provides a single threaded D3D11 device
        return MPxNode::kGloballySerial;
    }

    /**
     * Since this node has no outputs, nothing pulls new values of this plug if it gets dirty, so the plug will always have stale data.
     * Use a dirty plug callback to detect when it gets dirtied, and then pull the new value in compute().
     */
    static void onParticleSRVPlugDirty(MObject& node, MPlug& plug, void* clientData) {
        if (plug != aParticleSRV) return;
        
        VoxelShape* voxelShape = static_cast<VoxelShape*>(clientData);
        voxelShape->isParticleSRVPlugDirty = true;
    }

    /**
     * The user can assign a new interior voxel material to this shape using the marking menu. All the menu option does is set
     * the aInteriorMaterial string attribute (the name of a shading group) on this shape. Then we use that string to set the 
     * interior faces of the _original_ voxelized geometry to use that shading group.
     * 
     * Finally, the subscene override that draws this shape sees the interior material changed, and rebuilds its render items,
     * re-extracting the original geometry and shaders (including the new interior shader)!
     */
    static void onInteriorMaterialChanged(MNodeMessage::AttributeMessage msg, MPlug& plug, MPlug& otherPlug, void* clientData) {
        if (plug != aInteriorMaterial || !(msg & MNodeMessage::kAttributeSet)) return;

        VoxelShape* voxelShape = static_cast<VoxelShape*>(clientData);
        MString interiorMaterialShaderGroup = plug.asString();
        voxelShape->rebuildGeometry = true;

        MSelectionList interiorFacesSelectList;
        MDagPath originalGeomPath = voxelShape->pathToOriginalGeometry();
        MObjectArray interiorFaceComponents = voxelShape->getVoxels()->interiorFaceComponents;
        MObject interiorFaceComponent = Utils::combineFaceComponents(interiorFaceComponents);

        interiorFacesSelectList.add(originalGeomPath, interiorFaceComponent);
        MGlobal::setActiveSelectionList(interiorFacesSelectList);
        MGlobal::executeCommand("sets -e -forceElement \"" + interiorMaterialShaderGroup + "\"", false, true);
    }

    /**
     * To support export via Alembic, we need to do a few shenanigans. AbcExport doesn't support custom shapes, so we need to
     * temporarily swap out our VoxelShape with the original geometry (a regular mesh) by setting or unsetting it as an intermediate object,
     * and disabling the mesh render items on the subscene override.
     */
    static void onExportingChanged(MNodeMessage::AttributeMessage msg, MPlug& plug, MPlug& otherPlug, void* clientData) {
        if (plug != aExporting || !(msg & MNodeMessage::kAttributeSet)) return;

        VoxelShape* voxelShape = static_cast<VoxelShape*>(clientData);
        bool isExporting = plug.asBool();

        MDagPath originalGeomPath = voxelShape->pathToOriginalGeometry();
        if (!originalGeomPath.isValid()) return;

        MFnDagNode originalGeomDagNode(originalGeomPath);
        originalGeomDagNode.setIntermediateObject(!isExporting);

        // We rebuild the geometry for export in order to exclude UVs, which necessitate more vertex splitting -> more data to 
        // update each export frame, and complicate mapping normals back to the original geometry.
        voxelShape->rebuildGeometry = true;

        // (Dis)connect the old mesh's dummy time plug to global time so that AbcExport sees the mesh as time-dynamic.
        MFnDependencyNode meshDepNode(originalGeomPath.node());
        MPlug timeAttrPlug = meshDepNode.findPlug(exportDummyTimeAttrName, false);
        Utils::connectPlugs(Utils::getGlobalTimePlug(), timeAttrPlug, !isExporting);
    }

    void postConstructor() override {
        MPxSurfaceShape::postConstructor();
        setRenderable(true);

        MCallbackId callbackId = MNodeMessage::addNodeDirtyPlugCallback(thisMObject(), onParticleSRVPlugDirty, this);
        callbackIds.append(callbackId);

        callbackId = MNodeMessage::addAttributeChangedCallback(thisMObject(), onInteriorMaterialChanged, this);
        callbackIds.append(callbackId);

        callbackId = MNodeMessage::addAttributeChangedCallback(thisMObject(), onExportingChanged, this);
        callbackIds.append(callbackId);

        // Effectively a destructor callback to clean up when the node is deleted
        // This is more reliable than a destructor, because Maya won't necessarily call destructors on node deletion (unless undo queue is flushed)
        callbackId = MNodeMessage::addNodePreRemovalCallback(thisMObject(), [](MObject& node, void* clientData) {
            VoxelShape* voxelShape = static_cast<VoxelShape*>(clientData);
            MMessage::removeCallbacks(voxelShape->callbackIds);
            voxelShape->unsubPaintStateChanges();
        }, this);
        callbackIds.append(callbackId);

        callbackId = MNodeMessage::addNodeAboutToDeleteCallback(thisMObject(), [](MObject &node, MDGModifier& dgMod, void* clientData) {
            VoxelShape* voxelShape = static_cast<VoxelShape*>(clientData);
            
            PBDNode* pbdNode = static_cast<PBDNode*>(Utils::connectedNode(MPlug(node, aTrigger)));
            dgMod.deleteNode(pbdNode->thisMObject());

            MObject originalGeom = voxelShape->pathToOriginalGeometry().node();
            dgMod.deleteNode(originalGeom);

        }, this);
        callbackIds.append(callbackId);
    }

    /**
     * Associate each vertex in the buffer created by the subscene override with a voxel ID it belongs to.
     * We do this by iterating over the faces indices of each voxel face component, using them to access the index buffer of the whole mesh,
     * and tagging the vertices of each face with the voxel ID.
     * 
     * Note that this makes implicit assumptions about the order of face indices from MGeometryExtractor.
     * 
     * We do this now, instead of in the voxelizer, because the subscene override is the ultimate source of truth on the order of vertices in the GPU buffers.
     * Supporting split normals, UV seams, etc. requires duplicating vertices. So we have to do this step after the subscene override has created the final vertex buffers.
     */
    std::vector<uint> getVoxelIdsForVertices(
        const std::vector<uint>& vertexIndices,
        const unsigned int numVertices,
        const MSharedPtr<Voxels>& voxels
    ) const {
        std::vector<uint> vertexVoxelIds(numVertices, UINT_MAX);
        const MObjectArray& surfaceFaceComponents = voxels->surfaceFaceComponents;
        const MObjectArray& interiorFaceComponents = voxels->interiorFaceComponents;
        const std::vector<uint32_t>& mortonCodes = voxels->mortonCodes;
        const std::unordered_map<uint32_t, uint32_t>& mortonCodesToSortedIdx = voxels->mortonCodesToSortedIdx;

        MFnSingleIndexedComponent fnFaceComponent;
        auto addVoxelIdToVerts = [&](const MObjectArray& faceComponents, int voxelIndex) {
            MObject faceComponent = faceComponents[voxelIndex];
            fnFaceComponent.setObject(faceComponent);

            for (int j = 0; j < fnFaceComponent.elementCount(); ++j) {
                int faceIndex = fnFaceComponent.element(j);

                for (int k = 0; k < 3; ++k) {
                    uint vertexIndex = vertexIndices[3 * faceIndex + k];
                    vertexVoxelIds[vertexIndex] = voxelIndex;
                }
            }
        };

        for (int i = 0; i < voxels->numOccupied; ++i) {
            int voxelIndex = mortonCodesToSortedIdx.at(mortonCodes[i]);
            addVoxelIdToVerts(surfaceFaceComponents, voxelIndex);
            addVoxelIdToVerts(interiorFaceComponents, voxelIndex);
        }

        return vertexVoxelIds;
    }

    void allocatePaintBuffers(
        int elementsPerVoxel,
        ComPtr<ID3D11Buffer>& paintBufferA,
        ComPtr<ID3D11Buffer>& paintBufferB,
        PingPongView& paintViews
    ) {
        MSharedPtr<Voxels> voxels = getVoxels();
        if (!voxels) return;

        const int numVoxels = voxels->numOccupied;
        
        // Paint values start at 0. Use uint16_t to get the size right, but it will really be half-floats in the shader.
        // Need to use a typed buffer to get half-float support.
        // Need three copies of the buffer: A and B for ping-ponging during paint strokes, and one to hold the "before paint" state for delta calculations.
        int elementCount = numVoxels * elementsPerVoxel;
        const std::vector<uint16_t> emptyData(elementCount, 0);
        paintBufferA = DirectX::createReadWriteBuffer(emptyData, false);
        paintBufferB = DirectX::createReadWriteBuffer(emptyData, false);
        paintViews = PingPongView(
            DirectX::createSRV(paintBufferB, elementCount, 0, DXGI_FORMAT_R16_FLOAT),
            DirectX::createSRV(paintBufferA, elementCount, 0, DXGI_FORMAT_R16_FLOAT),
            DirectX::createUAV(paintBufferB, elementCount, 0, DXGI_FORMAT_R16_FLOAT),
            DirectX::createUAV(paintBufferA, elementCount, 0, DXGI_FORMAT_R16_FLOAT)
        );
    }

    // The paint delta buffer is shared between face and particle paint modes, and sized according to the larger of the two (particle mode).
    void allocatePaintDeltaBuffer() {
        MSharedPtr<Voxels> voxels = getVoxels();
        if (!voxels) return;

        const int numVoxels = voxels->numOccupied;
        int elementCount = numVoxels * 8;
        const std::vector<uint16_t> emptyData(elementCount, 0);

        paintDeltaBuffer = DirectX::createReadWriteBuffer(emptyData, false);
        paintDeltaUAV = DirectX::createUAV(paintDeltaBuffer, elementCount, 0, DXGI_FORMAT_R16_FLOAT);

        paintDeltaCompute = PaintDeltaCompute(paintDeltaUAV);
    }
};