#include "utils.h"
#include <maya/MGlobal.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MDGModifier.h>
#include <maya/MDagModifier.h>
#include <maya/MPlugArray.h>
#include <maya/MDagPath.h> 
#include <maya/MMatrix.h>
#include <maya/MSelectionList.h>
#include <maya/MFnMesh.h>
#include <maya/MFnSingleIndexedComponent.h>
#include <windows.h>
#include <sstream>
#include <cstring>

// Anonymous namespace to keep this constant's linkage internal to this file.
namespace {
const uint32_t magicBitsMask3DDecode[] = {
    0x00000000, 0x000003ff, 0x000300ff, 0x0300f00f, 0x30c30c3, 0x9249249
};
}

namespace Utils {

uint32_t toMortonCode(uint32_t x, uint32_t y, uint32_t z) {
    auto spreadBits = [](uint32_t value) -> uint32_t {
        value = (value | (value << 16)) & 0x030000FF;
        value = (value | (value << 8)) & 0x0300F00F;
        value = (value | (value << 4)) & 0x030C30C3;
        value = (value | (value << 2)) & 0x09249249;
        return value;
    };

    uint32_t xBits = spreadBits(x);
    uint32_t yBits = spreadBits(y) << 1;
    uint32_t zBits = spreadBits(z) << 2;

    return xBits | yBits | zBits;
}

void fromMortonCode(uint32_t mortonCode, uint32_t& x, uint32_t& y, uint32_t& z) {
    auto compactBits = [](uint32_t value) -> uint32_t {
        value &= 0x09249249;
        value = (value ^ (value >> 2)) & 0x030C30C3;
        value = (value ^ (value >> 4)) & 0x0300F00F;
        value = (value ^ (value >> 8)) & 0x030000FF;
        value = (value ^ (value >> 16)) & 0x000003FF;
        return value;
    };

    x = compactBits(mortonCode);
    y = compactBits(mortonCode >> 1);
    z = compactBits(mortonCode >> 2);
}

DWORD loadResourceFile(HINSTANCE pluginInstance, int id, const wchar_t* type, void** resourceData) {
    HRSRC hResource = FindResource(pluginInstance, MAKEINTRESOURCE(id), type);
    if (!hResource) {
        MGlobal::displayError(MString("Failed to find resource with ID: ") + id);
        return 0;
    }

    HGLOBAL hResourceData = LoadResource(pluginInstance, hResource);
    if (!hResourceData) {
        MGlobal::displayError(MString("Failed to load resource with ID: ") + id);
        return 0;
    }
    
    *resourceData = LockResource(hResourceData);
    if (!*resourceData) {
        MGlobal::displayError(MString("Failed to lock resource with ID: ") + id);
        return 0;
    }

    DWORD resourceSize = SizeofResource(pluginInstance, hResource);
    if (resourceSize == 0) {
        MGlobal::displayError(MString("Failed to get size of resource with ID: ") + id);
        return 0;
    }

    return resourceSize;
}

void loadMELScriptByResourceID(HINSTANCE pluginInstance, int resourceID) {
	void* data = nullptr;
	DWORD size = loadResourceFile(pluginInstance, resourceID, L"MEL", &data);
	if (size == 0) {
		MGlobal::displayError("Failed to load MEL script resource.");
		return;
	}

	MString melScript(static_cast<char*>(data), size);

	// Execute the MEL script to load it into memory
	MStatus status = MGlobal::executeCommand(melScript);
	if (status != MS::kSuccess) {
		MGlobal::displayError("Failed to execute MEL script: " + status.errorString());
	}
}

/**
 * Extracts a resource from the plugin .mll file (which contains windows resource files), and writes it to the specified output file path.
 * This allows us to put files in the plugin and extract them to disk at runtime, e.g. icon files.
 */
bool extractResourceToFile(HINSTANCE pluginInstance, int resourceID, const wchar_t* type, const MString& outputFilePath) {
    void* data = nullptr;
    DWORD size = Utils::loadResourceFile(pluginInstance, resourceID, type, &data);
    if (size == 0) return false;

    // Ensure folder exists
    std::wstring wpath = std::wstring(outputFilePath.asWChar());
    size_t lastSlash = wpath.find_last_of(L"/\\");
    if (lastSlash != std::wstring::npos) {
        std::wstring dir = wpath.substr(0, lastSlash);
        CreateDirectoryW(dir.c_str(), nullptr); // OK if already exists
    }

    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        MGlobal::displayError("Failed to create icon file: " + outputFilePath);
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data, size, &written, nullptr);
    CloseHandle(hFile);
    if (!ok || written != size) {
        MGlobal::displayError("Failed to write icon file completely: " + outputFilePath);
        return false;
    }
    return true;
}

std::string HResultToString(const HRESULT& hr) {
    std::string result;
    char* msgBuf = nullptr;

    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD langId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);

    // Try to get system message string for the HRESULT
    if (FormatMessageA(flags, nullptr, hr, langId, (LPSTR)&msgBuf, 0, nullptr) && msgBuf) {
        result = msgBuf;
        LocalFree(msgBuf);
    } else {
        result = "Unknown error";
    }

    // Append HRESULT hex
    std::stringstream ss;
    ss << " (HRESULT: 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);

    // Extract facility and code
    WORD facility = HRESULT_FACILITY(hr);
    WORD code = HRESULT_CODE(hr);
    ss << ", Facility: " << std::dec << facility << ", Code: " << code;

    // Check if it's a wrapped Win32 error
    if (facility == FACILITY_WIN32) {
        char* win32Msg = nullptr;
        if (FormatMessageA(flags, nullptr, code, langId, (LPSTR)&win32Msg, 0, nullptr) && win32Msg) {
            ss << ", Win32 message: \"" << win32Msg << "\"";
            LocalFree(win32Msg);
        }
    }

    ss << ")";
    result += ss.str();

    return result;
}

uint16_t floatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000;
    uint32_t exponent = ((bits >> 23) & 0xFF) - 112;
    uint32_t mantissa = bits & 0x007FFFFF;

    if (exponent <= 0) {
        // Subnormal or zero
        return sign;
    } else if (exponent >= 31) {
        // Inf or NaN
        return sign | 0x7C00;
    } else {
        return sign | (exponent << 10) | (mantissa >> 13);
    }
}

uint32_t packTwoFloatsInUint32(float a, float b) {
    uint32_t ha = static_cast<uint32_t>(floatToHalf(a));
    uint32_t hb = static_cast<uint32_t>(floatToHalf(b));
    return (hb << 16) | ha;
}

MFloatVector sign(const MFloatVector& v) {
    return MFloatVector(
        (v.x > 0) ? 1.0f : (v.x < 0) ? -1.0f : 0.0f,
        (v.y > 0) ? 1.0f : (v.y < 0) ? -1.0f : 0.0f,
        (v.z > 0) ? 1.0f : (v.z < 0) ? -1.0f : 0.0f
    );
}

/**
 * Logical indices are sparse, mapped to contiguous physical indices.
 * This method finds the next available logical index for creating a new plug in the array.
 */
uint getNextArrayPlugIndex(const MObject& dependencyNode, const MObject& arrayAttribute) {
    MStatus status;

    uint nextIndex = 0;
    MPlug arrayPlug(dependencyNode, arrayAttribute);
    const uint numElements = arrayPlug.evaluateNumElements(&status);
    for (uint i = 0; i < numElements; ++i) {
        uint idx = arrayPlug.elementByPhysicalIndex(i, &status).logicalIndex();
        if (idx >= nextIndex) {
            nextIndex = idx + 1;
        }
    }
    return nextIndex;
}

MPlug getGlobalTimePlug() {
    MItDependencyNodes it(MFn::kTime);
    // Assumes there's only one time node in the scene, which is a pretty safe assumption.
    if (!it.isDone()) {
        MFnDependencyNode timeNode(it.thisNode());
        return timeNode.findPlug("outTime", false);
    }

    return MPlug();
}

void connectPlugs(
    const MPlug& srcPlug,
    const MPlug& dstPlug,
    bool breakConnection
) {
    MDGModifier dgMod;
    breakConnection ?
        dgMod.disconnect(srcPlug, dstPlug) :
        dgMod.connect(srcPlug, dstPlug);
    dgMod.doIt();
}

void disconnectPlugs(
    const MPlug& srcPlug,
    const MPlug& dstPlug
) {
    MDGModifier dgMod;
    dgMod.disconnect(srcPlug, dstPlug);
    dgMod.doIt();
}

void removePlugMultiInstance(const MPlug& plug, int logicalIndexToRemove) {
    MDGModifier dgMod;
    MPlug plugToRemove = plug;
    if (logicalIndexToRemove != -1) {
        plugToRemove = plug.elementByLogicalIndex(logicalIndexToRemove);
    }
    
    dgMod.removeMultiInstance(plugToRemove, true);
    dgMod.doIt();
}

int arrayPlugNumElements(const MObject& dependencyNode, const MObject& arrayAttribute) {
    return MPlug(dependencyNode, arrayAttribute).evaluateNumElements();
}

MPxNode* connectedNode(const MPlug& plug, bool nodeIsSource) {
    MPlugArray conns;
    if (!plug.connectedTo(conns, nodeIsSource, !nodeIsSource) || conns.length() == 0) return nullptr;
    MObject connectedObj = conns[0].node(); // API returns a plug array but util assumes only one connection
    MFnDependencyNode fnNode(connectedObj);
    return fnNode.userNode();
}

MObject createDGNode(const MString& typeName) 
{
    MDGModifier dgMod;
    MObject nodeObj = dgMod.createNode(typeName);
    dgMod.doIt();
    return nodeObj;
}

void deleteDGNode(const MObject& nodeObj) 
{
    MDGModifier dgMod;
    dgMod.deleteNode(nodeObj);
    dgMod.doIt();
}

MObject createDagNode(const MString& typeName, const MObject& parent, const MString& name, MDagModifier* dagMod) 
{
    MDagModifier localMod;
    MDagModifier* mod = dagMod ? dagMod : &localMod;
    MObject nodeObj = mod->createNode(typeName, parent);
    mod->doIt();
    
    MFnDependencyNode fnNode(nodeObj);
    fnNode.setName(name);
    return nodeObj;
}

MMatrix getWorldMatrixWithoutScale(const MObject& object) {
    MDagPath objPath;
    MStatus status = MDagPath::getAPathTo(object, objPath);
    if (status != MS::kSuccess) {
        return MMatrix::identity;
    }
    
    MMatrix worldMatrix = objPath.inclusiveMatrix();
    MTransformationMatrix transformMatrix(worldMatrix);

    double qx = 0.0, qy = 0.0, qz = 0.0, qw = 1.0;
    transformMatrix.getRotationQuaternion(qx, qy, qz, qw);
    MVector translation = transformMatrix.getTranslation(MSpace::kWorld);

    MTransformationMatrix unscaledMatrix;
    unscaledMatrix.setTranslation(translation, MSpace::kWorld);
    unscaledMatrix.setRotationQuaternion(qx, qy, qz, qw);

    return unscaledMatrix.asMatrix();
}

// Note: this isn't particularly fast (iterates over all nodes), so don't use in performance-critical paths.
// (A better method may involve selection lists / using Maya's built in name resolution)
MObject getNodeFromName(const MString& name) {
    MItDependencyNodes it;
    for (; !it.isDone(); it.next()) {
        MFnDependencyNode fnNode(it.thisNode());
        if (fnNode.name() == name) {
            return it.thisNode();
        }
    }
    return MObject::kNullObj;
}

MObject getMostRecentlySelectedObject() {
    MSelectionList selectionList;
    MGlobal::getActiveSelectionList(selectionList);
    if (selectionList.length() == 0) {
        return MObject::kNullObj;
    }

    MObject selectedObj;
    selectionList.getDependNode(selectionList.length() - 1, selectedObj);
    return selectedObj;
}

bool tryGetShapePathFromObject(const MObject& object, MDagPath& shapePath) {
    MStatus status = MDagPath::getAPathTo(object, shapePath);
    if (status != MS::kSuccess) return false; // Not a DAG object
    if (shapePath.hasFn(MFn::kTransform)) {
        shapePath.extendToShape();
        return shapePath.hasFn(MFn::kShape);
    }

    if (shapePath.hasFn(MFn::kShape)) {
        return true;
    }
    
    return false;
}

MDagPath getDagPathFromName(const MString& name) {
    MSelectionList selectionList;
    selectionList.add(name);
    MDagPath dagPath;
    selectionList.getDagPath(0, dagPath);
    return dagPath;
}

// Use MEL to query and perform linking (in this case, C++ is _much_ more verbose and this isn't performance critical)
void transferUVLinks(const MDagPath& srcMeshPath, const MDagPath& dstMeshPath) {
    MFnMesh srcMeshFn(srcMeshPath);
    
    // This command gets all texture nodes linked to a given UV set.
    MString queryLinkTemplateCmd = "uvLink -q -uvs " + srcMeshPath.fullPathName() + ".uvSet[^1s].uvSetName;";
    MString addLinkTemplateCmd = "uvLink -make -uvs " + dstMeshPath.fullPathName() + ".uvSet[^1s].uvSetName -texture \"^2s\";";
    MString uvSetNumStr;
    MString queryLinksCmd;
    MStringArray textureNameLinks;
    MString addLinkCmd;

    for (int i = 0; i < srcMeshFn.numUVSets(); ++i) {
        textureNameLinks.clear();
        uvSetNumStr.set(i);
        queryLinksCmd = queryLinkTemplateCmd;
        queryLinksCmd.format(queryLinkTemplateCmd, uvSetNumStr);

        MGlobal::executeCommand(queryLinksCmd, textureNameLinks);

        for (uint j = 0; j < textureNameLinks.length(); ++j) {
            MString textureName = textureNameLinks[j];
            addLinkCmd = addLinkTemplateCmd;
            addLinkCmd.format(addLinkTemplateCmd, uvSetNumStr, textureName);
            MGlobal::executeCommand(addLinkCmd);
        }
    }
}

bool MStringArrayContains(const MStringArray& array, const MString& value) {
    for (unsigned int i = 0; i < array.length(); ++i) {
        if (array[i] == value) {
            return true;
        }
    }
    return false;
}

void deleteDefaultUVSet(const MString& meshName) {
    // Can't delete the default UV set while it's current - reorder UV sets, first, if possible.
    MStringArray allUVSets;
    MGlobal::executeCommand("polyUVSet -q -allUVSets " + meshName, allUVSets);
    MString firstUVSet = allUVSets[0];

    if (allUVSets.length() < 2) {
        MGlobal::displayWarning(MString("cubit cannot delete default UV set ") + firstUVSet + " because it's the only UV set");
        return;
    }

    MString secondUVSet = allUVSets[1];
    MGlobal::executeCommand("polyUVSet -reorder -uvSet " + firstUVSet + " -newUVSet " + secondUVSet + " " + meshName + ";");
    MGlobal::executeCommand("polyUVSet -delete -uvSet " + firstUVSet + " " + meshName + ";");
}

MString getActiveModelPanelName() {
	MString result;
	MGlobal::executeCommand("playblast -ae", result);

	// Parse the result to get the active model panel name (result is in form MainPane|viewPanes|modelPanel4|modelPanel4|modelPanel4)
	MStringArray parts;
	result.split('|', parts);
	return parts[parts.length() - 1];
}

MStringArray getAllModelPanelNames() {
    MStringArray panelNames;
    MGlobal::executeCommand("getPanel -type \"modelPanel\"", panelNames);
    return panelNames;
}

MObject combineFaceComponents(MObjectArray& faceComponents) {
    MFnSingleIndexedComponent fnCombinedComponent;
    MObject combinedComponent = fnCombinedComponent.create(MFn::kMeshPolygonComponent);

    MFnSingleIndexedComponent fnComponent_i;
    for (uint i = 0; i < faceComponents.length(); ++i) {
        fnComponent_i.setObject(faceComponents[i]);
        for (int j = 0; j < fnComponent_i.elementCount(); ++j) {
            fnCombinedComponent.addElement(fnComponent_i.element(j));
        }
    }

    return combinedComponent;
}

} // namespace Utils
