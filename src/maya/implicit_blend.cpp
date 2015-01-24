#define NO_CUDA

#include "implicit_blend.hpp"

#include <maya/MGlobal.h> 
#include <maya/MArrayDataBuilder.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnCompoundAttribute.h>
#include <maya/MFnMatrixAttribute.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnMatrixData.h>
#include <maya/MFnPluginData.h>
#include <maya/MDataBlock.h>
#include <maya/MDataHandle.h>
#include <maya/MArrayDataHandle.h>

#include "maya/maya_helpers.hpp"
#include "maya/maya_data.hpp"
#include "utils/misc_utils.hpp"

#include "skeleton.hpp"

#include <string.h>
#include <math.h>
#include <assert.h>
#include <algorithm>
#include <map>
using namespace std;

MTypeId ImplicitBlend::id(0xEA119);
void *ImplicitBlend::creator() { return new ImplicitBlend(); }
DagHelpers::MayaDependencies ImplicitBlend::dependencies;

MObject ImplicitBlend::surfaces;
MObject ImplicitBlend::parentJoint;
MObject ImplicitBlend::implicit;
MObject ImplicitBlend::meshGeometryUpdateAttr;
MObject ImplicitBlend::worldImplicit;

namespace {
    MStatus setImplicitSurfaceData(MDataBlock &dataBlock, MObject attr, shared_ptr<const Skeleton> skel)
    {
        MStatus status = MStatus::kSuccess;

        MFnPluginData dataCreator;
        dataCreator.create(ImplicitSurfaceData::id, &status);
        if(status != MS::kSuccess) return status;

        ImplicitSurfaceData *data = (ImplicitSurfaceData *) dataCreator.data(&status);
        if(status != MS::kSuccess) return status;

        data->setSkeleton(skel);

        MDataHandle worldImplicitHandle = dataBlock.outputValue(attr, &status);
        if(status != MS::kSuccess) return status;

        status = worldImplicitHandle.set(data);
        if(status != MS::kSuccess) return status;

        return MStatus::kSuccess;
    }
}

MStatus ImplicitBlend::initialize()
{
    MStatus status = MStatus::kSuccess;

    MFnNumericAttribute numAttr;
    MFnCompoundAttribute cmpAttr;
    MFnTypedAttribute typedAttr;

    meshGeometryUpdateAttr = numAttr.create("meshGeometryUpdate", "meshGeometryUpdate", MFnNumericData::Type::kInt, 0, &status);
    numAttr.setStorable(false);
    numAttr.setHidden(true);
    addAttribute(meshGeometryUpdateAttr);

    // Note that this attribute isn't set to worldSpace.  The input surfaces are world space, and the
    // output combined surfaces are world space, but we ignore the position of this actual node.
    worldImplicit = typedAttr.create("worldImplicit", "worldImplicit", ImplicitSurfaceData::id, MObject::kNullObj, &status);
    if(status != MS::kSuccess) return status;
    typedAttr.setUsesArrayDataBuilder(true);
    typedAttr.setWritable(false);
    addAttribute(worldImplicit);

    implicit = typedAttr.create("implicit", "implicit", ImplicitSurfaceData::id, MObject::kNullObj, &status);
    if(status != MS::kSuccess) return status;
    typedAttr.setReadable(false);
    dependencies.add(implicit, worldImplicit);
    dependencies.add(ImplicitBlend::worldImplicit, ImplicitBlend::meshGeometryUpdateAttr);
    addAttribute(implicit);

    parentJoint = numAttr.create("parentIdx", "parentIdx", MFnNumericData::Type::kInt, -1, &status);
    addAttribute(parentJoint);
    dependencies.add(parentJoint, worldImplicit);

    surfaces = cmpAttr.create("surfaces", "surfaces", &status);
    cmpAttr.setReadable(false);
    cmpAttr.setArray(true);
    cmpAttr.addChild(implicit);
    cmpAttr.addChild(parentJoint);
    addAttribute(surfaces);
    dependencies.add(ImplicitBlend::surfaces, ImplicitBlend::worldImplicit);

    status = dependencies.apply();
    if(status != MS::kSuccess) return status;

    return MStatus::kSuccess;
}

MStatus ImplicitBlend::setDependentsDirty(const MPlug &plug_, MPlugArray &plugArray)
{
    MStatus status = MStatus::kSuccess;

    MPlug plug(plug_);
    MString s = plug.name();

    // If the plug that was changed is a child, eg. point[0].x, move up to the parent
    // compound plug, eg. point[0].
    if(plug.isChild()) {
        plug = plug.parent(&status);
        if(status != MS::kSuccess) return status;
    }

    // The rendered geometry is based on meshGeometryUpdateAttr.  If the node that was changed
    // affects that, then tell Maya that it needs to redraw the geometry.  This will
    // trigger ImplicitSurfaceGeometryOverride::updateDG, etc. if the shape is visible.
    // It looks like setAffectsAppearance() on meshGeometryUpdateAttr should do this for
    // us, but that doesn't seem to work.
    MObject node = plug.attribute();
    s = plug.name();
    if(dependencies.isAffectedBy(node, ImplicitBlend::meshGeometryUpdateAttr))
        MHWRender::MRenderer::setGeometryDrawDirty(thisMObject());

    return MPxSurfaceShape::setDependentsDirty(plug, plugArray);
}

MStatus ImplicitBlend::compute(const MPlug &plug, MDataBlock &dataBlock)
{
    if(plug == worldImplicit) return load_world_implicit(plug, dataBlock);
    else if(plug == meshGeometryUpdateAttr) return load_mesh_geometry(dataBlock);
    return MStatus::kUnknownParameter;
}

const MeshGeom &ImplicitBlend::get_mesh_geometry()
{
    // Update and return meshGeometry for the preview renderer.
    MStatus status = MStatus::kSuccess;
    MDataBlock dataBlock = forceCache();
    dataBlock.inputValue(ImplicitBlend::meshGeometryUpdateAttr, &status);
    return meshGeometry;
}

// On meshGeometryUpdateAttr, update meshGeometry.
MStatus ImplicitBlend::load_mesh_geometry(MDataBlock &dataBlock)
{
    MStatus status = MStatus::kSuccess;

    dataBlock.inputValue(ImplicitBlend::worldImplicit, &status);
    if(status != MS::kSuccess) return status;

    if(skeleton.get() == NULL)
        return MStatus::kSuccess;

    meshGeometry = MeshGeom();
    MarchingCubes::compute_surface(meshGeometry, skeleton.get());

    return MStatus::kSuccess;
}

MStatus ImplicitBlend::update_skeleton(MDataBlock &dataBlock)
{
    MStatus status = MStatus::kSuccess;

    // Retrieve our input surfaces.  This will also update their transforms, etc. if needed.
    MArrayDataHandle surfacesHandle = dataBlock.inputArrayValue(ImplicitBlend::surfaces, &status);
    if(status != MS::kSuccess) return status;

    // Create a list of our input surfaces and their relationships.
    vector<shared_ptr<const Skeleton> > implicitBones;
    vector<int> surfaceParents;
    for(int i = 0; i < (int) surfacesHandle.elementCount(); ++i)
    {
        status = surfacesHandle.jumpToElement(i);
        if(status != MS::kSuccess) return status;

        int logicalIndex = surfacesHandle.elementIndex(&status);
        if(status != MS::kSuccess) return status;

        implicitBones.resize(max(logicalIndex+1, (int) implicitBones.size()));
        surfaceParents.resize(max(logicalIndex+1, (int) surfaceParents.size()));

        MDataHandle implicitHandle = surfacesHandle.inputValue(&status).child(ImplicitBlend::implicit);
        if(status != MS::kSuccess) return status;

        ImplicitSurfaceData *implicitSurfaceData = (ImplicitSurfaceData *) implicitHandle.asPluginData();
        implicitBones[logicalIndex] = implicitSurfaceData->getSkeleton();

        MDataHandle parentJointHandle = surfacesHandle.inputValue(&status).child(ImplicitBlend::parentJoint);
        if(status != MS::kSuccess) return status;

        int parentIdx = DagHelpers::readHandle<int>(parentJointHandle, &status);
        if(status != MS::kSuccess) return status;

        surfaceParents[logicalIndex] = parentIdx;
    }

    // If the actual bones and their parenting hasn't changed, we're already up to date.
    if(implicitBones == lastImplicitBones && surfaceParents == lastParents)
        return MStatus::kSuccess;

    lastImplicitBones = implicitBones;
    lastParents = surfaceParents;

    // Get the hierarchy order of the inputs, so we can create parents before children.
    vector<int> hierarchyOrder;
    if(!MiscUtils::getHierarchyOrder(surfaceParents, hierarchyOrder)) {
        // The input contains cycles.
        MDagPath dagPath = MDagPath::getAPathTo(thisMObject(), &status);
        if(status != MS::kSuccess) return status;

        MString path = dagPath.partialPathName(&status);
        if(status != MS::kSuccess) return status;
        MGlobal::displayError("The ImplicitBlend node " + path + " contains cycles.");
        return MStatus::kSuccess;
    }

    // Each entry in implicitBones represents a Skeleton.  These will usually be skeletons with
    // just a single bone, representing an ImplicitSurface, but they can also have multiple bones,
    // if the input is another ImplicitBlend.  Add all bones in the input into our skeleton.
    // We can have the same bone more than once, if multiple skeletons give it to us, but a skeleton
    // can never have the same bone more than once.
    std::vector<shared_ptr<const Bone> > bones;
    std::vector<Bone::Id> parents;

    // firstBonePerSkeleton[n] is the index of the first bone (in bones) added for implicitBones[n].
    std::vector<int> firstBonePerSkeleton(surfaceParents.size(), -1);
    for(int i = 0; i < (int) hierarchyOrder.size(); ++i)
    {
        int idx = hierarchyOrder[i];
        shared_ptr<const Skeleton> subSkeleton = implicitBones[idx];
        int surfaceParentIdx = surfaceParents[idx];

        int firstBoneIdx = -1;

        // Add all of the bones.
        map<Bone::Id,int> boneIdToIdx;
        for(Bone::Id boneId: subSkeleton->get_bone_ids())
        {
            shared_ptr<const Bone> bone = subSkeleton->get_bone(boneId);
            firstBoneIdx = (int) bones.size();
            boneIdToIdx[bone->get_bone_id()] = (int) bones.size();
            bones.push_back(bone);
        }

        for(Bone::Id boneId: subSkeleton->get_bone_ids())
        {
            int parentBoneIdx = -1;

            Bone::Id parentBoneId = subSkeleton->parent(boneId);
            if(parentBoneId != -1)
            {
                // If the bone within the subskeleton has a parent, it's another bone in the same
                // skeleton.  Add the parent bone's index within bones as the parent.
                parentBoneIdx = boneIdToIdx.at(parentBoneId);
            }
            else if(surfaceParentIdx != -1)
            {
                // This bone is at the root of its skeleton.  Use the first bone of the parent
                // surface.  If the parent surface doesn't actually have any bones, leave this
                // as a root joint.  It's guaranteed that we've already created the parent,
                // since we're traversing in hierarchy order.
                parentBoneIdx = firstBonePerSkeleton[surfaceParentIdx];
            }

            parents.push_back(parentBoneIdx);
        }

        firstBonePerSkeleton[idx] = firstBoneIdx;
    }

    // Skeletons can't have zero bones, so don't create one if we have no data.
    if(bones.size() == 0) {
        skeleton.reset();
        return MStatus::kSuccess;
    }

    // Create a skeleton containing the bones, replacing any previous skeleton.
    skeleton.reset(new Skeleton(bones, parents));

    return MStatus::kSuccess;
}

MStatus ImplicitBlend::load_world_implicit(const MPlug &plug, MDataBlock &dataBlock)
{
    MStatus status = MStatus::kSuccess;

    status = update_skeleton(dataBlock);
    if(status != MS::kSuccess) return status;

    if(skeleton.get() != NULL) {
        // Update our skeleton based on the bone data.  This lets the skeleton know that the bones
        // may have changed orientation.
        skeleton->update_bones_data();
    }

    // Set ImplicitBlend::worldImplicit to our skeleton.  This may be NULL.
    status = setImplicitSurfaceData(dataBlock, ImplicitBlend::worldImplicit, skeleton);
    if(status != MS::kSuccess) return status;

    return MStatus::kSuccess;
}



