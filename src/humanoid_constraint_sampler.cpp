/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, JSK, The University of Tokyo.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the JSK, The University of Tokyo nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman
*/

#include <moveit_humanoid_stability/humanoid_constraint_sampler.h>
#include <set>
#include <cassert>
#include <eigen_conversions/eigen_msg.h>
#include <boost/format.hpp>

namespace moveit_humanoid_stability
{
bool HumanoidConstraintSampler::configure(const moveit_msgs::Constraints &constr)
{
  moveit_msgs::Constraints constraints = constr; // copy to non-const

  logError("%s: CONFIGURE HUMANOID CONSTRAINT SAMPLER", sampler_name_.c_str());
  //std::cout << "message:\n " << constr << std::endl;

  // Humanoid custom constraints: define here --------------------
  moveit_msgs::JointConstraint jc1;
  moveit_msgs::OrientationConstraint oc1;

  double workspace = 1;

  // x axis orientation
  oc1.link_name = "BODY";
  oc1.header.frame_id = "BODY";
  oc1.orientation.x = 0;
  oc1.orientation.y = 0;
  oc1.orientation.z = 0;
  oc1.orientation.w = 1;
  oc1.absolute_x_axis_tolerance = 0.001;
  oc1.absolute_y_axis_tolerance = 0.001;
  oc1.absolute_z_axis_tolerance = 0.001;
  /*  TODO re-enable this
      oc1.absolute_x_axis_tolerance = 0.34906585;  // 20 degrees
      oc1.absolute_y_axis_tolerance = 0.34906585;
      oc1.absolute_z_axis_tolerance = 0.34906585;
  */
  oc1.weight = 1;
  constraints.orientation_constraints.push_back(oc1);

  // construct the joint constraints
  std::vector<kinematic_constraints::JointConstraint> jc;
  for (std::size_t i = 0 ; i < constraints.joint_constraints.size() ; ++i)
  {
    kinematic_constraints::JointConstraint j(scene_->getRobotModel());
    if (j.configure(constraints.joint_constraints[i]))
      jc.push_back(j);
  }

  // construct the *single* orientation constraint
  if (constraints.orientation_constraints.size() > 1)
  {
    logError("This constraint sampler is only able to process one orientation constraint currently");
  }
  else if (constraints.orientation_constraints.size() == 1)
  {
    // Create our one orientation constraint
    orientation_constraint_.reset(
                                  new kinematic_constraints::OrientationConstraint(scene_->getRobotModel()));
    orientation_constraint_->configure(constraints.orientation_constraints[0], scene_->getTransforms());
  }

  // Load visual tools
  visual_tools_.reset(new moveit_visual_tools::VisualTools("/odom", "/hrp2_visual_markers", scene_->getRobotModel()));  
  visual_tools_->loadRobotStatePub("/hrp2_state_sampler");
  
  // Configure stability checker
  humanoid_stability_.reset(new moveit_humanoid_stability::HumanoidStability(verbose_, scene_->getCurrentState(), visual_tools_));

  // Verbose mode text display setting
  text_pose_.position.x = scene_->getCurrentState().getFakeBaseTransform().translation().x();
  text_pose_.position.y = scene_->getCurrentState().getFakeBaseTransform().translation().y();
  text_pose_.position.z = scene_->getCurrentState().getFakeBaseTransform().translation().z() + 2;

  return configureJoint(jc);
}

bool HumanoidConstraintSampler::configureJoint(const std::vector<kinematic_constraints::JointConstraint> &jc)
{
  clear();

  if (!jmg_)
  {
    logError("NULL planning group specified for constraint sampler");
    return false;
  }

  if (jc.empty())
  {
    sampler_name_ = "NO_Joint_Constraints";
    logError("No joint constraints passed to joint constraint sampler 2");
    //return false;
  }
  else
    sampler_name_ = "HAS_Joint_Constraints";

  // find and keep the constraints that operate on the group we sample
  // also keep bounds for joints for convenience
  std::map<std::string, JointInfo> bound_data;
  for (std::size_t i = 0 ; i < jc.size() ; ++i)
  {
    // Check that joint constraint is enabled
    if (!jc[i].enabled())
      continue;

    // Check that joint constraint has valid joint model
    const robot_model::JointModel *jm = jc[i].getJointModel();
    if (!jmg_->hasJointModel(jm->getName()))
      continue;

    // Get the bounds of this variable
    const robot_model::VariableBounds& joint_bounds = jm->getVariableBounds(jc[i].getJointVariableName());

    // Populate the joint info object
    JointInfo ji;
    // Check if this variable already has bounds set (for some reason_
    std::map<std::string, JointInfo>::iterator it = bound_data.find(jc[i].getJointVariableName());
    if (it != bound_data.end())
      ji = it->second;
    else
      ji.index_ = jmg_->getVariableGroupIndex(jc[i].getJointVariableName()); // copy the index of the variable with respect to the joint model group

    // Attempt to tighten the variables bounds if applicable from the constraint
    ji.potentiallyAdjustMinMaxBounds(std::max(joint_bounds.min_position_, jc[i].getDesiredJointPosition() - jc[i].getJointToleranceBelow()),
                                     std::min(joint_bounds.max_position_, jc[i].getDesiredJointPosition() + jc[i].getJointToleranceAbove()));

    // Debug
    logDebug("Bounds for %s JointConstraint are %g %g", jc[i].getJointVariableName().c_str(), ji.min_bound_, ji.max_bound_);

    // Error check
    if (ji.min_bound_ > ji.max_bound_ + std::numeric_limits<double>::epsilon())
    {
      std::stringstream cs; jc[i].print(cs);
      logError("The constraints for joint '%s' have no possible values for the joint: min_bound: %g, max_bound: %g. Failing.\n",
               jm->getName().c_str(), ji.min_bound_, ji.max_bound_);
      clear();
      return false;
    }

    // Save this new joint info
    bound_data[jc[i].getJointVariableName()] = ji;
  }

  // Copy our larger bound data structure into a more compact one
  for (std::map<std::string, JointInfo>::iterator it = bound_data.begin(); it != bound_data.end(); ++it)
    bounds_.push_back(it->second);

  // get a separate list of joints that are not bounded; we will sample these randomly
  const std::vector<const robot_model::JointModel*> &joints = jmg_->getJointModels();
  for (std::size_t i = 0 ; i < joints.size() ; ++i)
    if (bound_data.find(joints[i]->getName()) == bound_data.end() &&
        joints[i]->getVariableCount() > 0 &&
        joints[i]->getMimic() == NULL)
    {
      // check if all the vars of the joint are found in bound_data instead
      const std::vector<std::string> &vars = joints[i]->getVariableNames();
      if (vars.size() > 1)
      {
        bool all_found = true;
        for (std::size_t j = 0 ; j < vars.size() ; ++j)
          if (bound_data.find(vars[j]) == bound_data.end())
          {
            all_found = false;
            break;
          }
        if (all_found)
          continue;
      }
      unbounded_.push_back(joints[i]);
      // Get the first variable name of this joint and find its index position in the planning group
      uindex_.push_back(jmg_->getVariableGroupIndex(vars[0]));
      //logInform("Adding variable index %d for joint index %d",jmg_->getVariableGroupIndex(vars[0]), i);
    }

  values_.resize(jmg_->getVariableCount());
  is_valid_ = true;

  logWarn("%s: finished configuring joint sampler. Bounded: %d, Unbounded: %d", sampler_name_.c_str(), bounds_.size(), unbounded_.size());
  return true;
}

bool HumanoidConstraintSampler::sample(robot_state::RobotState &robot_state, const robot_state::RobotState & /* reference_state */,
                                        unsigned int max_attempts)
{
  if (!jmg_)
    logError("no joint model group loaded");

  if (!is_valid_)
  {
    logWarn("HumanoidConstraintSampler not configured, won't sample");
    return false;
  }

  logWarn("%s: HumanoidConstraintSampler SAMPLING -----------------------------",sampler_name_.c_str());

  //const robot_model::JointModel *vjoint = jmg_->getJointModel("virtual_joint");
  const robot_model::JointModel *vjoint = robot_state.getJointModel("virtual_joint");
  const robot_model::LinkModel  *rfoot  = jmg_->getLinkModel("RLEG_LINK5");
  const robot_model::LinkModel  *lfoot  = jmg_->getLinkModel("LLEG_LINK5");
  const robot_model::JointModelGroup *whole_body_minus_left_leg = robot_state.getRobotModel()->getJointModelGroup("whole_body_minus_left_leg");
  const robot_model::JointModelGroup *left_leg = robot_state.getRobotModel()->getJointModelGroup("left_leg");

  max_attempts = 100000; // TODO this might be a bad hack

  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt)
  {
    if (verbose_)
      logInform("Sampling attempt number %d for group %s", attempt, jmg_->getName().c_str() );

    if (!ros::ok())
      return false;

    if (verbose_)
      visual_tools_->deleteAllMarkers();

    // Decide how to sample the joints
    bool use_constraint_sampling = false;
    // Generate simple movement states
    if (false)
    {
      double lleg_position[1];
      lleg_position[0] = robot_state.getJointPositions("LLEG_JOINT1")[0] + 0.01;
      robot_state.setJointPositions("LLEG_JOINT1", lleg_position);

      // Update with leg fixed
      robot_state.updateStateWithFakeBase();
      use_constraint_sampling = true;
    }
    else if (bounds_.size() > 0)
    {
      ROS_INFO_STREAM_NAMED("temp","Sampling joints using joint constraints");
      use_constraint_sampling = true;

      // Calculate random position of robot
      // \todo: don't sample virtual joint orientation and the legs to save time
      if (!sampleJoints(robot_state))
      {
        logError("Unable to sample joints");
        return false;
      }

      // Update with leg fixed
      robot_state.updateStateWithFakeBase();
    }
    else
    {
      //ROS_INFO_STREAM_NAMED("temp","Sampling joints using robot state random variables");
      // Generate random state for leg only
      robot_state.setToRandomPositions(left_leg);

      // Update only the virtual joint and the leg we just updated
      robot_state.updateSingleChainWithFakeBase();
    }


    if (verbose_)
    {
      visual_tools_->publishRobotState(robot_state);
    }


    if (!humanoid_stability_->isApproximateValidBase(robot_state))
    {
      if (verbose_)
      {
        ROS_WARN_STREAM_NAMED("temp","Sample outside VIRTUAL JOINT constraints");
        visual_tools_->publishText(text_pose_, "OUTSIDE virtual joint bounding box", moveit_visual_tools::RED, moveit_visual_tools::LARGE);
        ros::Duration(1.1).sleep();
      }

      continue; // stop checking
    }

    // Is Valid
    if (verbose_)
    {
      visual_tools_->publishText(text_pose_, "Within virtual joint bounding box", moveit_visual_tools::WHITE, moveit_visual_tools::LARGE);
      visual_tools_->publishRobotState(robot_state);
      ros::Duration(1.1).sleep();
    }

    if (!use_constraint_sampling)
    {
      // Generate remainder of body random state
      robot_state.setToRandomPositions(whole_body_minus_left_leg);
      robot_state.update(true); // update entire robot using previously computed virtual joint tranform
    }

    if (verbose_)
    {
      visual_tools_->publishRobotState(robot_state);
    }

    // Check if the right foot is above ground
    if (!humanoid_stability_->isApproximateValidFoot(robot_state))
    {
      if (verbose_)
      {
        ROS_WARN_STREAM_NAMED("temp","Sample outside RIGHT FOOT ground constraint ");
        visual_tools_->publishText(text_pose_, "OUTSIDE right foot ground constraint", moveit_visual_tools::RED, moveit_visual_tools::LARGE);
        ros::Duration(1.1).sleep();
      }

      continue; // stop checking
    }

    // Check COM balance constraints --------------------------------------------------------------
    if (!humanoid_stability_->isValidCOM(robot_state))
    {
      if (verbose_)
      {
        ROS_WARN("Pose is NOT stable");
        visual_tools_->publishText(text_pose_, "NOT stable from center of mass", moveit_visual_tools::RED, moveit_visual_tools::LARGE);
        ros::Duration(1.1).sleep();
      }
      continue;
    }

    if (verbose_)
      ROS_INFO("Pose is stable");   

    bool check_verbose = false;
    if (!scene_->isStateValid(robot_state, "", check_verbose)) // second argument is what planning group to collision check, "" is everything
    {
      if (verbose_)
      {
        ROS_ERROR_STREAM_NAMED("temp","Pose not valid (self or enviornment collision)");
        visual_tools_->publishText(text_pose_, "NOT valid from self or env collision", moveit_visual_tools::RED, moveit_visual_tools::LARGE);
        ros::Duration(1.1).sleep();
      }
      continue;
    }

    // Find min/max
    /*min_x_ = std::min(vjoint_positions[0], min_x_);
      max_x_ = std::max(vjoint_positions[0], max_x_);

      min_y_ = std::min(vjoint_positions[1], min_y_);
      max_y_ = std::max(vjoint_positions[1], max_y_);

      min_z_ = std::min(vjoint_positions[2], min_z_);
      max_z_ = std::max(vjoint_positions[2], max_z_);*/

    if (verbose_)
    {
      visual_tools_->publishText(text_pose_, "Sample VALID!", moveit_visual_tools::WHITE, moveit_visual_tools::LARGE);
      ros::Duration(3.0).sleep();
    }

    ROS_DEBUG_STREAM_NAMED("temp","Passed - valid sample found on attempts " << attempt);

    return true;
  } // for attempts

  ROS_DEBUG_STREAM_NAMED("temp","Aborted - ran out of attempts (" << max_attempts << ")");

  return false;
}

bool HumanoidConstraintSampler::sampleJoints(robot_state::RobotState &robot_state)
{
  // sample the unbounded joints first (in case some joint varipables are bounded)
  std::vector<double> v;
  for (std::size_t i = 0 ; i < unbounded_.size() ; ++i)
  {
    v.resize(unbounded_[i]->getVariableCount());

    if (false)
      logInform("%s: UNCONSTRAINED: Joint number %d named %s with variables %d", sampler_name_.c_str(),
                i, unbounded_[i]->getName().c_str(),v.size());

    unbounded_[i]->getVariableRandomPositions(random_number_generator_, &v[0]);

    for (std::size_t j = 0 ; j < v.size() ; ++j)
    {
      values_[uindex_[i] + j] = v[j];
    }
  }

  // enforce the constraints for the constrained components (could be all of them)
  for (std::size_t i = 0 ; i < bounds_.size() ; ++i)
  {
    if (false)
      logInform("%s: CONSTRAINED: Joint number %d named %s bounds [%f,%f]", sampler_name_.c_str(), bounds_[i].index_,
                jmg_->getVariableNames()[ bounds_[i].index_ ].c_str(),
                bounds_[i].min_bound_, bounds_[i].max_bound_);

    values_[bounds_[i].index_] = random_number_generator_.uniformReal(bounds_[i].min_bound_, bounds_[i].max_bound_);
  }

  robot_state.setJointGroupPositions(jmg_, values_);

  return true;
}

// TODO remove
bool HumanoidConstraintSampler::sampleOrientationConstraints(robot_state::RobotState &robot_state)
{
  if (!orientation_constraint_->enabled())
  {
    logWarn("Orientation constraint is not enabled, skipping");
    return false;
  }

  Eigen::Quaterniond quat;

  // sample a rotation matrix within the allowed bounds
  double angle_x = 2.0 * (random_number_generator_.uniform01() - 0.5) * (orientation_constraint_->getXAxisTolerance()-std::numeric_limits<double>::epsilon());
  double angle_y = 2.0 * (random_number_generator_.uniform01() - 0.5) * (orientation_constraint_->getYAxisTolerance()-std::numeric_limits<double>::epsilon());
  double angle_z = 2.0 * (random_number_generator_.uniform01() - 0.5) * (orientation_constraint_->getZAxisTolerance()-std::numeric_limits<double>::epsilon());
  Eigen::Affine3d diff(Eigen::AngleAxisd(angle_x, Eigen::Vector3d::UnitX())
                       * Eigen::AngleAxisd(angle_y, Eigen::Vector3d::UnitY())
                       * Eigen::AngleAxisd(angle_z, Eigen::Vector3d::UnitZ()));
  Eigen::Affine3d reqr(orientation_constraint_->getDesiredRotationMatrix() * diff.rotation());
  quat = Eigen::Quaterniond(reqr.rotation());

  // if this constraint is with respect a mobile frame, we need to convert this rotation to the root frame of the model
  if (orientation_constraint_->mobileReferenceFrame() && false) // TODO
  {
    logError("is mobile reference frame (?)");
    const Eigen::Affine3d &t = robot_state.getFrameTransform(orientation_constraint_->getReferenceFrame());
    Eigen::Affine3d rt(t.rotation() * quat.toRotationMatrix());
    quat = Eigen::Quaterniond(rt.rotation());
  }

  // Now set the virtual joint quaternion to this result
  robot_state.setVariablePosition("virtual_joint/rot_x", quat.x());
  robot_state.setVariablePosition("virtual_joint/rot_y", quat.y());
  robot_state.setVariablePosition("virtual_joint/rot_z", quat.x());
  robot_state.setVariablePosition("virtual_joint/rot_w", quat.w());

  return true;
}

bool HumanoidConstraintSampler::project(robot_state::RobotState &robot_state,
                                         unsigned int max_attempts)
{
  return sample(robot_state, robot_state, max_attempts);
}

void HumanoidConstraintSampler::clear()
{
  ConstraintSampler::clear();
  bounds_.clear();
  unbounded_.clear();
  uindex_.clear();
  values_.clear();
}

void HumanoidConstraintSampler::setVerbose(bool verbose)
{
  humanoid_stability_->setVerbose(verbose);
}


} //namespace
