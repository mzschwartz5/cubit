#include "plugin.h"
#include "custommayaconstructs/tools/voxeldragcontextcommand.h"
#include "custommayaconstructs/tools/voxelpaintcontextcommand.h"
#include <maya/MProgressWindow.h>
#include "custommayaconstructs/data/voxeldata.h"
#include "custommayaconstructs/data/particledata.h"
#include "custommayaconstructs/data/functionaldata.h"
#include "custommayaconstructs/data/d3d11data.h"
#include "custommayaconstructs/data/colliderdata.h"
#include "custommayaconstructs/draw/voxelshape.h"
#include "custommayaconstructs/draw/voxelsubsceneoverride.h"
#include "custommayaconstructs/draw/colliderdrawoverride.h"
#include "custommayaconstructs/usernodes/pbdnode.h"
#include "custommayaconstructs/usernodes/voxelizernode.h"
#include "custommayaconstructs/usernodes/boxcollider.h"
#include "custommayaconstructs/usernodes/spherecollider.h"
#include "custommayaconstructs/usernodes/capsulecollider.h"
#include "custommayaconstructs/usernodes/cylindercollider.h"
#include "custommayaconstructs/usernodes/planecollider.h"
#include "custommayaconstructs/commands/createcollidercommand.h"
#include "custommayaconstructs/commands/changevoxeleditmodecommand.h"
#include "custommayaconstructs/commands/applyvoxelpaintcommand.h"
#include "simulationcache.h"
#include <maya/MDrawRegistry.h>
#include <maya/MTransformationMatrix.h>
#include <maya/MAnimControl.h>
#include "directx/compute/computeshader.h"
#include "globalsolver.h"
#include <maya/M3dView.h>
#include <maya/MFnPlugin.h>

// define EXPORT for exporting dll functions
#define EXPORT __declspec(dllexport)

VoxelRendererOverride* plugin::voxelRendererOverride = nullptr;
MCallbackId plugin::toolChangedCallbackId;

// Maya Plugin creator function
void* plugin::creator()
{
	return new plugin;
}

MSyntax plugin::syntax()
{
	MSyntax syntax;
	syntax.addFlag("-n", "-selectedMeshName", MSyntax::kString);
	syntax.addFlag("-px", "-positionX", MSyntax::kDouble);
	syntax.addFlag("-py", "-positionY", MSyntax::kDouble);
	syntax.addFlag("-pz", "-positionZ", MSyntax::kDouble);
	syntax.addFlag("-rx", "-rotationX", MSyntax::kDouble);
	syntax.addFlag("-ry", "-rotationY", MSyntax::kDouble);
	syntax.addFlag("-rz", "-rotationZ", MSyntax::kDouble);
	syntax.addFlag("-vsz", "-voxelSize", MSyntax::kDouble);
	syntax.addFlag("-vx", "-numVoxelsX", MSyntax::kLong);
	syntax.addFlag("-vy", "-numVoxelsY", MSyntax::kLong);
	syntax.addFlag("-vz", "-numVoxelsZ", MSyntax::kLong);
	syntax.addFlag("-t", "-type", MSyntax::kLong);
	return syntax;
}

MStatus plugin::doIt(const MArgList& argList)
{
	MGlobal::executeCommand("undoInfo -openChunk", false, false); // make everything from here to the end of the function undoable in one command
	MProgressWindow::reserve();
	MProgressWindow::setTitle("Mesh Preparation Progress");
	MProgressWindow::startProgress();
	
	MTime::setUIUnit(MTime::k60FPS);
	MGlobal::executeCommand("optionVar -iv \"cachedPlaybackEnable\" 0;"); // disable built-in caching system (cubit uses its own caching system)
    SimulationCache::instance()->resetCache();

	PluginArgs pluginArgs = parsePluginArgs(argList);
	MSelectionList selectedMesh;
	selectedMesh.add(pluginArgs.selectedMeshName);
	MGlobal::setActiveSelectionList(selectedMesh);
	MDagPath selectedMeshDagPath;
	selectedMesh.getDagPath(0, selectedMeshDagPath);

	if (!pluginArgs.clipTriangles) {
		// Enable two sided lighting if not clipping triangles (their backsides will be visible)
		for (auto& panelName : Utils::getAllModelPanelNames()) {
			MGlobal::executeCommand("modelEditor -e -twoSidedLighting true " + panelName, false, true);
		}
	}

	// Progress window message updates done within the voxelizer (for finer-grained control)
	double rotation[3];
	MTransformationMatrix gridTransform;
	pluginArgs.rotation.get(rotation);
	gridTransform.setTranslation(MVector(pluginArgs.position), MSpace::kWorld);
	gridTransform.setRotation(rotation, MTransformationMatrix::kXYZ);
	const VoxelizationGrid voxelizationGrid {
		pluginArgs.voxelSize * 1.005, // To avoid precision / cut off issues, scale up the voxelization grid very slightly.
		pluginArgs.voxelsPerEdge,
		gridTransform
	};

	MDagPath voxelizedMeshDagPath;
	MStatus status = MS::kSuccess;
	MObject voxelizerNodeObj = VoxelizerNode::createVoxelizerNode(
		voxelizationGrid,
		selectedMeshDagPath,
		pluginArgs.voxelizeSurface,
		pluginArgs.voxelizeInterior,
		!pluginArgs.renderAsVoxels,
		pluginArgs.clipTriangles,
		voxelizedMeshDagPath,
		status
	);

	if (status != MS::kSuccess) {
		MProgressWindow::endProgress();
		MGlobal::executeCommand("undoInfo -closeChunk", false, false);
		return status;
	}
	
	MProgressWindow::setProgressStatus("Creating PBD particles and face constraints..."); MProgressWindow::setProgressRange(0, 100); MProgressWindow::setProgress(0);
	MObject pbdNodeObj = PBDNode::createPBDNode(voxelizerNodeObj);
	MObject voxelShapeObj = VoxelShape::createVoxelShapeNode(pbdNodeObj, voxelizedMeshDagPath);
	MProgressWindow::setProgress(100);

	PlaneCollider::createGroundColliderIfNoneExists();

	MProgressWindow::endProgress();

	// Switch the active model panel to use the VoxelRendererOverride (used for dragging and painting support)
	MString activeModelPanel = Utils::getActiveModelPanelName();
	status = MGlobal::executeCommandOnIdle(MString("modelEditor -edit -rnm $gViewport2 -rom " + VoxelRendererOverride::voxelRendererOverrideName + " " + activeModelPanel));

	MGlobal::executeCommand("undoInfo -closeChunk", false, false); // close the undo chunk

    MTime startTime = MAnimControl::minTime();
	MAnimControl::setCurrentTime(startTime);

	return MS::kSuccess;
}

PluginArgs plugin::parsePluginArgs(const MArgList& args) {
	PluginArgs pluginArgs;
	MStatus status;
	MArgDatabase argData(syntax(), args, &status);
	if (status != MS::kSuccess) {
		MGlobal::displayError("Failed to parse arguments: " + status.errorString());
		return pluginArgs;
	}

	if (argData.isFlagSet("-n")) {
		status = argData.getFlagArgument("-n", 0, pluginArgs.selectedMeshName);
	}
	
	if (argData.isFlagSet("-px")) {
		status = argData.getFlagArgument("-px", 0, pluginArgs.position.x);
	}

	if (argData.isFlagSet("-py")) {
		status = argData.getFlagArgument("-py", 0, pluginArgs.position.y);
	}

	if (argData.isFlagSet("-pz")) {
		status = argData.getFlagArgument("-pz", 0, pluginArgs.position.z);
	}

	if (argData.isFlagSet("-rx")) {
		status = argData.getFlagArgument("-rx", 0, pluginArgs.rotation.x);
	}

	if (argData.isFlagSet("-ry")) {
		status = argData.getFlagArgument("-ry", 0, pluginArgs.rotation.y);
	}

	if (argData.isFlagSet("-rz")) {
		status = argData.getFlagArgument("-rz", 0, pluginArgs.rotation.z);
	}

	if (argData.isFlagSet("-vsz")) {
		status = argData.getFlagArgument("-vsz", 0, pluginArgs.voxelSize);
	}
	
	if (argData.isFlagSet("-vx")) {
		status = argData.getFlagArgument("-vx", 0, pluginArgs.voxelsPerEdge[0]);
	}

	if (argData.isFlagSet("-vy")) {
		status = argData.getFlagArgument("-vy", 0, pluginArgs.voxelsPerEdge[1]);
	}

	if (argData.isFlagSet("-vz")) {
		status = argData.getFlagArgument("-vz", 0, pluginArgs.voxelsPerEdge[2]);
	}

	if (argData.isFlagSet("-t")) {
		int type;
		status = argData.getFlagArgument("-t", 0, type);

		pluginArgs.voxelizeSurface = (type & 0x1) != 0;
		pluginArgs.voxelizeInterior = (type & 0x2) != 0;
		pluginArgs.renderAsVoxels = (type & 0x4) != 0;
		pluginArgs.clipTriangles = (type & 0x8) != 0;
	}

	return pluginArgs;
}

// Initialize Maya Plugin upon loading
EXPORT MStatus initializePlugin(MObject obj)
{
	MStatus status;
	M3dView activeView = M3dView::active3dView();
	if (activeView.getRendererName(&status) != M3dView::kViewport2Renderer) {
		MGlobal::displayError("cubit requires Viewport 2.0 to be the current renderer.");
		return MStatus::kFailure;
	}

	if (MRenderer::theRenderer()->drawAPI() != DrawAPI::kDirectX11) {
		MGlobal::displayError("cubit requires DirectX 11 to be the current Viewport 2.0 rendering engine.");
		return MStatus::kFailure;
	}

	// Initialize DirectX
	// MhInstPlugin is a global variable defined in the MfnPlugin.h file
	status = DirectX::initialize(MhInstPlugin);
	CHECK_MSTATUS_AND_RETURN_IT(status);
	plugin::toolChangedCallbackId = MEventMessage::addEventCallback("PostToolChanged", ChangeVoxelEditModeCommand::onExternalToolChange, nullptr);
	plugin::voxelRendererOverride = new VoxelRendererOverride(VoxelRendererOverride::voxelRendererOverrideName);

	// Register all commands, nodes, and custom plug data types
	MFnPlugin plugin(obj, "cubit", "1.0", "Any");
	status = plugin.registerCommand("cubit", plugin::creator, plugin::syntax);
	CHECK_MSTATUS(status);
	status = plugin.registerCommand(CreateColliderCommand::commandName, CreateColliderCommand::creator, CreateColliderCommand::syntax);
	CHECK_MSTATUS(status);
	status = plugin.registerCommand(ChangeVoxelEditModeCommand::commandName, ChangeVoxelEditModeCommand::creator, ChangeVoxelEditModeCommand::syntax);
	CHECK_MSTATUS(status);
	status = plugin.registerCommand(ApplyVoxelPaintCommand::commandName, ApplyVoxelPaintCommand::creator, ApplyVoxelPaintCommand::syntax);
	CHECK_MSTATUS(status);
	status = plugin.registerData(VoxelData::fullName, VoxelData::id, VoxelData::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerData(ParticleData::fullName, ParticleData::id, ParticleData::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerData(FunctionalData::fullName, FunctionalData::id, FunctionalData::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerData(D3D11Data::fullName, D3D11Data::id, D3D11Data::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerData(ColliderData::fullName, ColliderData::id, ColliderData::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(PBDNode::pbdNodeName, PBDNode::id, PBDNode::creator, PBDNode::initialize, MPxNode::kDependNode);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(VoxelizerNode::typeName, VoxelizerNode::id, VoxelizerNode::creator, VoxelizerNode::initialize, MPxNode::kDependNode);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(BoxCollider::typeName, BoxCollider::id, BoxCollider::creator, BoxCollider::initialize, MPxNode::kLocatorNode, &ColliderDrawOverride::drawDbClassification);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(SphereCollider::typeName, SphereCollider::id, SphereCollider::creator, SphereCollider::initialize, MPxNode::kLocatorNode, &ColliderDrawOverride::drawDbClassification);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(CapsuleCollider::typeName, CapsuleCollider::id, CapsuleCollider::creator, CapsuleCollider::initialize, MPxNode::kLocatorNode, &ColliderDrawOverride::drawDbClassification);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(CylinderCollider::typeName, CylinderCollider::id, CylinderCollider::creator, CylinderCollider::initialize, MPxNode::kLocatorNode, &ColliderDrawOverride::drawDbClassification);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(PlaneCollider::typeName, PlaneCollider::id, PlaneCollider::creator, PlaneCollider::initialize, MPxNode::kLocatorNode, &ColliderDrawOverride::drawDbClassification);
	CHECK_MSTATUS(status);
	status = plugin.registerContextCommand("voxelDragContextCommand", VoxelDragContextCommand::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerContextCommand("voxelPaintContextCommand", VoxelPaintContextCommand::creator);
	CHECK_MSTATUS(status);
	status = MRenderer::theRenderer()->registerOverride(plugin::voxelRendererOverride);
	CHECK_MSTATUS(status);
	status = MDrawRegistry::registerDrawOverrideCreator(ColliderDrawOverride::drawDbClassification, ColliderDrawOverride::drawRegistrantId, ColliderDrawOverride::creator);
	CHECK_MSTATUS(status);
	status = plugin.registerNode(GlobalSolver::globalSolverNodeName, GlobalSolver::id, GlobalSolver::creator, GlobalSolver::initialize, MPxNode::kDependNode);
	CHECK_MSTATUS(status);
	status = plugin.registerShape(VoxelShape::typeName, VoxelShape::id, VoxelShape::creator, VoxelShape::initialize, &VoxelShape::drawDbClassification);
	CHECK_MSTATUS(status);
	status = MDrawRegistry::registerComponentConverter(MString("VoxelSelectionItem"), VoxelSubSceneComponentConverter::creator);
	CHECK_MSTATUS(status);
	status = MDrawRegistry::registerSubSceneOverrideCreator(VoxelSubSceneOverride::drawDbClassification, VoxelSubSceneOverride::drawRegistrantId, VoxelSubSceneOverride::creator);
	CHECK_MSTATUS(status);
	
	// Switch the active model panel to use the VoxelRendererOverride (used for dragging and painting support)
	MString activeModelPanel = Utils::getActiveModelPanelName();
	status = MGlobal::executeCommandOnIdle(MString("modelEditor -edit -rnm $gViewport2 -rom " + VoxelRendererOverride::voxelRendererOverrideName + " " + activeModelPanel));
	// VoxelShapeMarkingMenu
	Utils::loadMELScriptByResourceID(MhInstPlugin, IDR_MEL1);
	// VoxelizerMenu
	Utils::loadMELScriptByResourceID(MhInstPlugin, IDR_MEL2);
	// AETemplates
	Utils::loadMELScriptByResourceID(MhInstPlugin, IDR_MEL3);
	// deleteShelfTabNoPrompt
	Utils::loadMELScriptByResourceID(MhInstPlugin, IDR_MEL6);

	// Unlike other MEL scripts, these two can't be loaded into memory, but have to be copied (into the user scripts directory)
	// Maya specifically looks for these files by name when setting up a tool's property sheet.
	MString scriptsDir;
	MGlobal::executeCommand("internalVar -usd", scriptsDir);
	Utils::extractResourceToFile(MhInstPlugin, IDR_MEL4, L"MEL", scriptsDir + "VoxelPaintContextProperties.mel");
	Utils::extractResourceToFile(MhInstPlugin, IDR_MEL5, L"MEL", scriptsDir + "VoxelPaintContextValues.mel");
	Utils::extractResourceToFile(MhInstPlugin, IDR_MEL7, L"MEL", scriptsDir + "VoxelDragContextProperties.mel");
	Utils::extractResourceToFile(MhInstPlugin, IDR_MEL8, L"MEL", scriptsDir + "VoxelDragContextValues.mel");

	// Write icon files to user pref's directory
	MString prefsDir;
    MGlobal::executeCommand("internalVar -userPrefDir", prefsDir);
	MString iconsDir = prefsDir + "icons/";
    Utils::extractResourceToFile(MhInstPlugin, IDR_PNG_VOXELIZER, L"PNG", iconsDir + "Voxelize.png");
    Utils::extractResourceToFile(MhInstPlugin, IDR_PNG_VOXELDRAG, L"PNG", iconsDir + "VoxelDrag.png");
    Utils::extractResourceToFile(MhInstPlugin, IDR_PNG_VOXELCOLLIDER, L"PNG", iconsDir + "VoxelCollider.png");
    Utils::extractResourceToFile(MhInstPlugin, IDR_PNG_VOXELPAINT, L"PNG", iconsDir + "VoxelPaint.png");

	MGlobal::executeCommand("VoxelizerMenu_initializeUI");
	return status;
}

// Cleanup Plugin upon unloading
EXPORT MStatus uninitializePlugin(MObject obj)
{
	MGlobal::executeCommand("VoxelizerMenu_tearDownUI");

	// Deregister all commands, nodes, and custom plug data types
    MStatus status;
    MFnPlugin plugin(obj);
    status = plugin.deregisterCommand("cubit");
	CHECK_MSTATUS(status);
	status = plugin.deregisterCommand(CreateColliderCommand::commandName);
	CHECK_MSTATUS(status);
	status = plugin.deregisterCommand(ChangeVoxelEditModeCommand::commandName);
	CHECK_MSTATUS(status);
	status = plugin.deregisterCommand(ApplyVoxelPaintCommand::commandName);
	CHECK_MSTATUS(status);
    status = plugin.deregisterContextCommand("voxelDragContextCommand");
	CHECK_MSTATUS(status);
	status = plugin.deregisterContextCommand("voxelPaintContextCommand");
	CHECK_MSTATUS(status);
	status = plugin.deregisterData(VoxelData::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterData(ParticleData::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterData(FunctionalData::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterData(D3D11Data::id);	status = plugin.deregisterData(ColliderData::id);	status = plugin.deregisterNode(PBDNode::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(VoxelizerNode::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(BoxCollider::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(SphereCollider::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(CapsuleCollider::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(CylinderCollider::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(PlaneCollider::id);
	CHECK_MSTATUS(status);
    status = MRenderer::theRenderer()->deregisterOverride(plugin::voxelRendererOverride);
	CHECK_MSTATUS(status);
	status = MDrawRegistry::deregisterDrawOverrideCreator(ColliderDrawOverride::drawDbClassification, ColliderDrawOverride::drawRegistrantId);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(GlobalSolver::id);
	CHECK_MSTATUS(status);
	status = plugin.deregisterNode(VoxelShape::id);
	CHECK_MSTATUS(status);
	status = MDrawRegistry::deregisterComponentConverter("VoxelSelectionItem");
	CHECK_MSTATUS(status);
	status = MDrawRegistry::deregisterSubSceneOverrideCreator(VoxelSubSceneOverride::drawDbClassification, VoxelSubSceneOverride::drawRegistrantId);
	CHECK_MSTATUS(status);

	GlobalSolver::tearDown();
    delete plugin::voxelRendererOverride;
    plugin::voxelRendererOverride = nullptr;
	ComputeShader::clearShaderCache();
	MEventMessage::removeCallback(plugin::toolChangedCallbackId);

	return status;
}
