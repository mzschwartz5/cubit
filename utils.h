#pragma once
#include <maya/MFloatVector.h>
#include <maya/MPlug.h>
#include <maya/MFnPluginData.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MDataBlock.h>
#include <maya/MDataHandle.h>
#include <maya/MObject.h>
#include <maya/MString.h>
#include <windows.h>
#include <cstdint>
#include <type_traits>

namespace Utils {

uint32_t toMortonCode(uint32_t x, uint32_t y, uint32_t z);
void fromMortonCode(uint32_t mortonCode, uint32_t& x, uint32_t& y, uint32_t& z);
DWORD loadResourceFile(HINSTANCE pluginInstance, int id, const wchar_t* type, void** resourceData);
void loadMELScriptByResourceID(HINSTANCE pluginInstance, int resourceID);
bool extractResourceToFile(HINSTANCE pluginInstance, int resourceID, const wchar_t* type, const MString& outputFilePath);
std::string HResultToString(const HRESULT& hr);

inline int divideRoundUp(int numerator, int denominator) {
    return (numerator + denominator - 1) / denominator;
}

inline int ilogbaseceil(int x, int base) {
    // Using change of bases property of logarithms:
    return static_cast<int>(std::ceil(std::log(x) / std::log(base)));
}

uint16_t floatToHalf(float value);

uint32_t packTwoFloatsInUint32(float a, float b);

MFloatVector sign(const MFloatVector& v);

/**
 * Helper to get the MPxData from a plug of type MFnPluginData.
 * We use a struct rather than a function because the plug object and MFnPluginData
 * must remain alive while the returned MPxData pointer is used.
 */
template<typename T>
struct PluginData {
    MObject plugObj;
    MFnPluginData plugFn;
    T* data = nullptr;

    PluginData(const MObject& dependencyNode, const MObject& plugAttribute) {
        MPlug plug(dependencyNode, plugAttribute);
        plug.getValue(plugObj);
        plugFn.setObject(plugObj);
        data = static_cast<T*>(plugFn.data());
    }

    PluginData(const MPlug& plug) {
        plug.getValue(plugObj);
        plugFn.setObject(plugObj);
        data = static_cast<T*>(plugFn.data());
    }

    T* get() const { return data; }
};

/**
 * Create an instance of (subclass of) MPxData, initialize it using the provided initializer function,
 * then set the provided plug on the dependency node to the new data object.
 */
template<typename T, typename Initializer>
MStatus createPluginData(const MObject& dependencyNode, const MObject& plugAttribute, Initializer&& initializer) {
    MStatus status;
    MFnPluginData fnData;
    MObject dataObj = fnData.create(T::id, &status);
    if (status != MStatus::kSuccess) return status;

    T* data = static_cast<T*>(fnData.data(&status));
    if (status != MStatus::kSuccess || !data) return status;

    // Call the initializer function passed in by the user
    std::forward<Initializer>(initializer)(data);

    // Set the plug value to the new data object
    MPlug plug(dependencyNode, plugAttribute);
    status = plug.setValue(dataObj);
    return status;
}

/**
 * Overload: create MPxData, initialize it, and set it onto an MDataBlock output handle.
 * Useful inside compute() implementations.
 */
template<typename T, typename Initializer>
MStatus createPluginData(MDataBlock& dataBlock, const MObject& outputAttribute, Initializer&& initializer) {
    MStatus status;
    MFnPluginData fnData;
    MObject dataObj = fnData.create(T::id, &status);
    if (status != MStatus::kSuccess) return status;

    T* data = static_cast<T*>(fnData.data(&status));
    if (status != MStatus::kSuccess || !data) return status;

    // Initialize the MPxData instance
    std::forward<Initializer>(initializer)(data);

    // Get the output handle and attach the MObject
    MDataHandle outHandle = dataBlock.outputValue(outputAttribute, &status);
    if (status != MStatus::kSuccess) return status;

    outHandle.setMObject(dataObj);
    outHandle.setClean();
    return status;
}

uint getNextArrayPlugIndex(const MObject& dependencyNode, const MObject& arrayAttribute);

MPlug getGlobalTimePlug();

struct MStringHash {
    size_t operator()(const MString& s) const noexcept {
        int len = 0;
        const char* utf = s.asUTF8(len);
        return std::hash<std::string_view>()(std::string_view(utf, static_cast<size_t>(len)));
    }
};
struct MStringEq {
    bool operator()(const MString& a, const MString& b) const noexcept { return a == b; }
};

// Helper overloads which build MPlug from either attribute name or MObject
inline MPlug plugFromAttr(const MObject& node, const MObject& attr) {
    return MPlug(node, attr);
}

inline MPlug plugFromAttr(const MObject& node, const MString& attrName) {
    MFnDependencyNode fn(node);
    return fn.findPlug(attrName, false);
}

template<typename SrcAttrT, typename DstAttrT>
void connectPlugs(
    const MObject& srcNode, 
    const SrcAttrT& srcAttr, 
    const MObject& dstNode, 
    const DstAttrT& dstAttr,
    int srcLogicalIndex = -1,
    int dstLogicalIndex = -1,
    bool breakConnection = false
) {
    MPlug srcPlug = plugFromAttr(srcNode, srcAttr);
    MPlug dstPlug = plugFromAttr(dstNode, dstAttr);

    if (srcLogicalIndex != -1) {
        srcPlug = srcPlug.elementByLogicalIndex(srcLogicalIndex);
    }

    if (dstLogicalIndex != -1) {
        dstPlug = dstPlug.elementByLogicalIndex(dstLogicalIndex);
    }

    connectPlugs(srcPlug, dstPlug, breakConnection);
}

void connectPlugs(
    const MPlug& srcPlug,
    const MPlug& dstPlug,
    bool breakConnection = false
);

void removePlugMultiInstance(const MPlug& plug, int logicalIndexToRemove = -1);

int arrayPlugNumElements(const MObject& dependencyNode, const MObject& arrayAttribute);

/**
 * Gets the MPxNode connected to the given plug. Assumes only one connection.
 */
MPxNode* connectedNode(const MPlug& plug, bool nodeIsSource = true);

MObject createDGNode(const MString& typeName);

void deleteDGNode(const MObject& nodeObj);

MObject createDagNode(const MString& typeName, const MObject& parent = MObject::kNullObj, const MString& name = "", MDagModifier* dagMod = nullptr);

MMatrix getWorldMatrixWithoutScale(const MObject& object);

MObject getNodeFromName(const MString& name);

MObject getMostRecentlySelectedObject();

bool tryGetShapePathFromObject(const MObject& object, MDagPath& shapePath);

MDagPath getDagPathFromName(const MString& name);

/**
 * Transfers UV set links from the source mesh to the destination mesh. Assumes both meshes have the same UV sets and shading engines.
 * 
 * Note: in the voxelizer, we transfer attributes (including uv sets) and shading sets. However,
 * the links between uv sets and shaders are not transferred, so we have to do that manually.
 */
void transferUVLinks(const MDagPath& srcMeshPath, const MDagPath& dstMeshPath);

bool MStringArrayContains(const MStringArray& array, const MString& value);

void deleteDefaultUVSet(const MString& meshName);

MString getActiveModelPanelName();

MStringArray getAllModelPanelNames();

MObject combineFaceComponents(MObjectArray& faceComponents);

} // namespace Utils