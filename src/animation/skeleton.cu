/*
 Implicit skinning
 Copyright (C) 2013 Rodolphe Vaillant, Loic Barthe, Florian Cannezin,
 Gael Guennebaud, Marie Paule Cani, Damien Rohmer, Brian Wyvill,
 Olivier Gourmel

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License 3 as published by
 the Free Software Foundation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>
 */
#include "skeleton.hpp"

#include <iostream>
#include <fstream>

#include "std_utils.hpp"
#include "blending_env.hpp"
#include "skeleton_env.hpp"
#include "precomputed_prim.hpp"
#include "hrbf_kernels.hpp"
#include "hrbf_env.hpp"
#include "conversions.hpp"
#include "std_utils.hpp"
#include "loader_skel.hpp"
#include "globals.hpp"
#include "cuda_utils.hpp"

namespace { __device__ void fix_debug() { } }

using namespace Cuda_utils;

void Skeleton::init_skel_env()
{
    std::vector<int> parents(_joints.size());
    std::vector<const Bone*> bones(_joints.size());
    for(int i = 0; i < (int) _joints.size(); ++i) {
        parents[i] = _joints[i]._parent;
        bones[i] = _joints[i]._anim_bone;
    }

    _skel_id = Skeleton_env::new_skel_instance(bones, parents);
    update_bones_pose(std::vector<Transfo>(_joints.size(), Transfo::identity()));
    Skeleton_env::update_joints_data(_skel_id, get_joints_data());
    Skeleton_env::update_bones_data (_skel_id, bones);
}

Skeleton::Skeleton(const Loader::Abs_skeleton& skel)
{
    _joints.resize(skel._bones.size());

    for(int i = 0; i < (int) _joints.size(); i++)
    {
        Skeleton_env::Joint_data d;
        d._blend_type     = EJoint::MAX;
        d._ctrl_id        = Blending_env::new_ctrl_instance();
        d._bulge_strength = 0.7f;
        _joints[i]._joint_data = d;

        _joints[i]._controller = IBL::Shape::caml();
        Blending_env::update_controller(d._ctrl_id, _joints[i]._controller);
    }

    std::vector<Transfo> _frames(skel._bones.size());
    for(int bid = 0; bid < (int) _joints.size(); bid++ )
        _frames[bid] = skel._bones[bid];

    for(int bid = 0; bid < (int) _joints.size(); bid++)
    {
        SkeletonJoint &joint = _joints[bid];

        joint._parent = skel._parents[bid];
        if(joint._parent != -1)
            _joints[joint._parent]._children.push_back(bid);

        int parent_bone_id = joint._parent;
        Vec3_cu org = Transfo::identity().get_translation();
        if(parent_bone_id != -1)
            org = _frames[parent_bone_id].get_translation();

        Vec3_cu end = _frames[bid].get_translation();

        Vec3_cu dir = end.to_point() - org.to_point();
        float length = dir.norm();

        joint._bone = Bone_cu(org.to_point(), dir, length);
        // joint._bone = Bone_cu(org.to_point(), end.to_point());

    }

    // If any bones lie on the same position as their parent, they'll have a zero length and
    // an undefined orientation.  Set them to a small length, and the same orientation as their
    // parent.
    for(int bid = 0; bid < (int) _joints.size(); bid++)
    {
        SkeletonJoint &joint = _joints[bid];
        if(joint._bone._length >= 0.000001f)
            continue;
        joint._bone._length = 0.000001f;

        if(joint._parent != -1)
            joint._bone._dir = _joints[joint._parent]._bone._dir;
        else
            joint._bone._dir = Vec3_cu(1,0,0);
    }

    for(int bid = 0; bid < (int) _joints.size(); bid++)
    {
        SkeletonJoint &joint = _joints[bid];
        joint._anim_bone = new Bone(1);
        joint._anim_bone->set_enabled(false);
        joint._anim_bone->set_length( joint._bone._length );
        joint._anim_bone->_bone_id = bid;
    }
    // must be called last
    init_skel_env();
}

// -----------------------------------------------------------------------------

Skeleton::~Skeleton()
{
    for(unsigned i = 0; i < _joints.size(); i++){
        _joints[i]._children.clear();
        delete _joints[i]._anim_bone;
        const int ctrl_id = _joints[i]._joint_data._ctrl_id;
        if( ctrl_id >= 0)
            Blending_env::delete_ctrl_instance(ctrl_id);
    }

    Skeleton_env::delete_skel_instance( _skel_id );
}

void Skeleton::set_joint_controller(Blending_env::Ctrl_id i,
                                    const IBL::Ctrl_setup& shape)
{
    assert( i >= 0);
    assert( i < (int) _joints.size());

    _joints[i]._controller = shape;
    Blending_env::update_controller(_joints[i]._joint_data._ctrl_id, shape);
}

// -----------------------------------------------------------------------------

std::vector<Skeleton_env::Joint_data> Skeleton::get_joints_data() const
{
    std::vector<Skeleton_env::Joint_data> joints_data(_joints.size());
    for(int i = 0; i < (int) _joints.size(); ++i)
        joints_data[i] = _joints[i]._joint_data;
    return joints_data;
}

void Skeleton::set_joint_blending(int i, EJoint::Joint_t type)
{
    assert( i >= 0);
    assert( i < (int) _joints.size());

    _joints[i]._joint_data._blend_type = type;

    Skeleton_env::update_joints_data(_skel_id, get_joints_data());
}

// -----------------------------------------------------------------------------

void Skeleton::set_joint_bulge_mag(int i, float m)
{
    assert( i >= 0);
    assert( i < (int) _joints.size());

    _joints[i]._joint_data._bulge_strength = std::min(std::max(m, 0.f), 1.f);
    Skeleton_env::update_joints_data(_skel_id, get_joints_data());
}

IBL::Ctrl_setup Skeleton::get_joint_controller(int i)
{
    assert( i >= 0);
    assert( i < (int) _joints.size());
    return _joints[i]._controller;
}

void Skeleton::set_transforms(const std::vector<Transfo> &transfos)
{
    assert(transfos.size() == _joints.size());
    update_bones_pose(transfos);
}

void Skeleton::update_bones_pose(const std::vector<Transfo> &transfos)
{
    // Put _anim_bones in the position specified by transfos.
    for(unsigned i = 0; i < _joints.size(); i++)
    {
        SkeletonJoint &joint = _joints[i];
        Bone *bone = joint._anim_bone;

        // Check that we don't have a zero orientation.  It's an invalid value that will
        // trickle down through a bunch of other data as IND/INFs and eventually cause other
        // assertion failures, so flag it here to make it easier to debug.
        Bone_cu b = joint._bone;
        assert(b.dir().norm_squared() > 0);

        // If this joint represents a bone, transform it by the parent joint's transform.
        Transfo bone_transform = Transfo::identity();
        if(is_bone(i))
        {
            const SkeletonJoint &joint = _joints[i];
            bone_transform  = joint._parent == -1? Transfo::identity():transfos[joint._parent];
        }

        bone->set_length( b.length() );
        bone->set_orientation(bone_transform * b.org(), bone_transform * b.dir());

        if(bone->get_type() == EBone::HRBF)
        {
            const int id = bone->get_hrbf().get_id();
            if( id > -1) HRBF_env::set_transfo(id, bone_transform);
        }
        
        if(bone->get_type() == EBone::PRECOMPUTED)
            bone->get_primitive().set_transform(bone_transform);
    }

    HRBF_env::apply_hrbf_transfos();
    Precomputed_prim::update_device_transformations();

    // In order to this call to take effect correctly it MUST be done after
    // transform_hrbf() and transform_precomputed_prim() otherwise bones
    // positions will not be updated correctly within the Skeleton_env.
    update_bones_data();
}

void Skeleton::update_bones_data()
{
    std::vector<const Bone*> bones(_joints.size());
    for(int i = 0; i < (int) _joints.size(); ++i)
        bones[i] = _joints[i]._anim_bone;

    Skeleton_env::update_bones_data(_skel_id, bones);
}

/*
  // TODO: to be deleted
void Skeleton::update_hrbf_id_to_bone_id()
{
    int res = 0;
    for(int i = 0; i < (int) _joints.size(); i++){
        if(bone_type(i) == EBone::HRBF){
            int hrbf_id = _anim_bones[i]->get_hrbf().get_id();
            res = std::max(hrbf_id , res);
        }
    }

    _hrbf_id_to_bone_id.clear();
    _hrbf_id_to_bone_id.resize(res+1);

    for(int i = 0; i < (int) _joints.size(); i++){
        if(bone_type(i) == EBone::HRBF){
            int hrbf_id = _anim_bones[i]->get_hrbf().get_id();
            _hrbf_id_to_bone_id[hrbf_id] = i;
        }
    }
}
*/

Skeleton_env::DBone_id Skeleton::get_bone_didx(Bone::Id i) const {
    return Skeleton_env::bone_hidx_to_didx(_skel_id, i);
}
