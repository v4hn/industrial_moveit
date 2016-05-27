/*
 * goal_orientation_constraint.cpp
 *
 *  Created on: Apr 21, 2016
 *      Author: Jorge Nicho
 */

#include <ros/console.h>
#include <Eigen/Geometry>
#include <eigen_conversions/eigen_msg.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_state/conversions.h>
#include <pluginlib/class_list_macros.h>
#include <stomp_moveit/noisy_filters/underconstrained_goal.h>
#include <XmlRpcException.h>

PLUGINLIB_EXPORT_CLASS(stomp_moveit::noisy_filters::UnderconstrainedGoal,stomp_moveit::noisy_filters::StompNoisyFilter);


const static unsigned int DOF_SIZE = 6;
const static double EPSILON = 0.1;
const static double LAMBDA = 0.01;

static void reduceJacobian(const Eigen::MatrixXd& jacb,
                                          const std::vector<int>& indices,Eigen::MatrixXd& jacb_reduced)
{
  jacb_reduced.resize(indices.size(),jacb.cols());
  for(auto i = 0u; i < indices.size(); i++)
  {
    jacb_reduced.row(i) = jacb.row(indices[i]);
  }
}

static void calculateMoorePenrosePseudoInverse(const Eigen::MatrixXd& jacb,Eigen::MatrixXd& jacb_pseudo_inv)
{
  Eigen::MatrixXd jacb_transpose = jacb.transpose();
  jacb_pseudo_inv = (jacb_transpose) * ((jacb * jacb_transpose).inverse());
}

static void calculateDampedPseudoInverse(const Eigen::MatrixXd &jacb, Eigen::MatrixXd &jacb_pseudo_inv,
                                         double eps = 0.011, double lambda = 0.01)
{
  using namespace Eigen;


  //Calculate A+ (pseudoinverse of A) = V S+ U*, where U* is Hermition of U (just transpose if all values of U are real)
  //in order to solve Ax=b -> x*=A+ b
  Eigen::JacobiSVD<MatrixXd> svd(jacb, Eigen::ComputeThinU | Eigen::ComputeThinV);
  const MatrixXd &U = svd.matrixU();
  const VectorXd &Sv = svd.singularValues();
  const MatrixXd &V = svd.matrixV();

  // calculate the reciprocal of Singular-Values
  // damp inverse with lambda so that inverse doesn't oscillate near solution
  size_t nSv = Sv.size();
  VectorXd inv_Sv(nSv);
  for(size_t i=0; i< nSv; ++i)
  {
    if (fabs(Sv(i)) > eps)
    {
      inv_Sv(i) = 1/Sv(i);
    }
    else
    {
      inv_Sv(i) = Sv(i) / (Sv(i)*Sv(i) + lambda*lambda);
    }
  }

  jacb_pseudo_inv = V * inv_Sv.asDiagonal() * U.transpose();
}

static void computeTwist(const Eigen::Affine3d& p0,
                                        const Eigen::Affine3d& pf,
                                        const Eigen::ArrayXi& nullity,Eigen::VectorXd& twist)
{
  twist.resize(nullity.size());
  twist.setConstant(0);
  Eigen::Vector3d twist_pos = pf.translation() - p0.translation();

  // relative rotation -> R = inverse(R0) * Rf
  Eigen::AngleAxisd relative_rot(p0.rotation().transpose() * pf.rotation());
  double angle = relative_rot.angle();
  Eigen::Vector3d axis = relative_rot.axis();

  // forcing angle to range [-pi , pi]
  while( (angle > M_PI) || (angle < -M_PI))
  {
    angle = (angle >  M_PI) ? (angle - 2*M_PI) : angle;
    angle = (angle < -M_PI )? (angle + 2*M_PI) : angle;
  }

  // creating twist rotation relative to tool
  Eigen::Vector3d twist_rot = axis * angle;

  // assigning into full 6dof twist vector
  twist.head(3) = twist_pos;
  twist.tail(3) = twist_rot;

  // zeroing all underconstrained cartesian dofs
  twist = (nullity == 0).select(0,twist);
}

namespace stomp_moveit
{
namespace noisy_filters
{

UnderconstrainedGoal::UnderconstrainedGoal():
    name_("UnderconstrainedGoal")
{

}

UnderconstrainedGoal::~UnderconstrainedGoal()
{
  // TODO Auto-generated destructor stub
}

bool UnderconstrainedGoal::initialize(moveit::core::RobotModelConstPtr robot_model_ptr,
                        const std::string& group_name,const XmlRpc::XmlRpcValue& config)
{
  group_name_ = group_name;
  robot_model_ = robot_model_ptr;

  return configure(config);
}

bool UnderconstrainedGoal::configure(const XmlRpc::XmlRpcValue& config)
{
  using namespace XmlRpc;

  try
  {
    XmlRpcValue params = config;

    XmlRpcValue dof_nullity_param = params["constrained_dofs"];
    XmlRpcValue dof_thresholds_param = params["cartesian_convergence"];
    XmlRpcValue joint_updates_param = params["joint_update_rates"];
    if((dof_nullity_param.getType() != XmlRpcValue::TypeArray) ||
        dof_nullity_param.size() < DOF_SIZE ||
        dof_thresholds_param.getType() != XmlRpcValue::TypeArray ||
        dof_thresholds_param.size() < DOF_SIZE  ||
        joint_updates_param.getType() != XmlRpcValue::TypeArray ||
        joint_updates_param.size() == 0)
    {
      ROS_ERROR("UnderconstrainedGoal received invalid array parameters");
      return false;
    }

    dof_nullity_.resize(DOF_SIZE);
    for(auto i = 0u; i < dof_nullity_param.size(); i++)
    {
      dof_nullity_(i) = static_cast<int>(dof_nullity_param[i]);
    }

    cartesian_convergence_thresholds_.resize(DOF_SIZE);
    for(auto i = 0u; i < dof_thresholds_param.size(); i++)
    {
      cartesian_convergence_thresholds_(i) = static_cast<double>(dof_thresholds_param[i]);
    }

    joint_update_rates_.resize(joint_updates_param.size());
    for(auto i = 0u; i < joint_updates_param.size(); i++)
    {
      joint_update_rates_(i) = static_cast<double>(joint_updates_param[i]);
    }

    //update_weight_ = static_cast<double>(params["update_weight"]);
    max_iterations_ = static_cast<int>(params["max_ik_iterations"]);
  }
  catch(XmlRpc::XmlRpcException& e)
  {
    ROS_ERROR("UnderconstrainedGoal failed to load parameters, %s",e.getMessage().c_str());
    return false;
  }

  return true;
}

bool UnderconstrainedGoal::setMotionPlanRequest(const planning_scene::PlanningSceneConstPtr& planning_scene,
                 const moveit_msgs::MotionPlanRequest &req,
                 const stomp_core::StompConfiguration &config,
                 moveit_msgs::MoveItErrorCodes& error_code)
{
  using namespace Eigen;
  using namespace moveit::core;

  const JointModelGroup* joint_group = robot_model_->getJointModelGroup(group_name_);
  tool_link_ = joint_group->getLinkModelNames().back();
  state_.reset(new RobotState(robot_model_));
  robotStateMsgToRobotState(req.start_state,*state_);

  const std::vector<moveit_msgs::Constraints>& goals = req.goal_constraints;
  if(goals.empty())
  {
    ROS_ERROR("A goal constraint was not provided");
    error_code.val = error_code.INVALID_GOAL_CONSTRAINTS;
    return false;
  }

  // storing tool goal pose
  if(goals.front().position_constraints.empty() ||
      goals.front().orientation_constraints.empty())
  {
    ROS_WARN("A goal constraint for the tool link was not provided, using forward kinematics");

    // check joint constraints
    if(goals.front().joint_constraints.empty())
    {
      ROS_ERROR_STREAM("No joint values for the goal were found");
      error_code.val = error_code.INVALID_GOAL_CONSTRAINTS;
      return false;
    }

    // compute FK to obtain tool pose
    const std::vector<moveit_msgs::JointConstraint>& joint_constraints = goals.front().joint_constraints;

    // copying goal values into state
    for(auto& jc: joint_constraints)
    {
      state_->setVariablePosition(jc.joint_name,jc.position);
    }

    state_->update(true);
    state_->enforceBounds(joint_group);
    tool_goal_pose_ = state_->getGlobalLinkTransform(tool_link_);
  }
  else
  {
    // tool goal
    const moveit_msgs::PositionConstraint& pos_constraint = goals.front().position_constraints.front();
    const moveit_msgs::OrientationConstraint& orient_constraint = goals.front().orientation_constraints.front();

    geometry_msgs::Pose pose;
    pose.position = pos_constraint.constraint_region.primitive_poses[0].position;
    pose.orientation = orient_constraint.orientation;
    tf::poseMsgToEigen(pose,tool_goal_pose_);
  }

  error_code.val = error_code.SUCCESS;
  return true;
}


bool UnderconstrainedGoal::filter(std::size_t start_timestep,
                    std::size_t num_timesteps,
                    int iteration_number,
                    int rollout_number,
                    Eigen::MatrixXd& parameters,
                    bool& filtered)
{
  using namespace Eigen;
  using namespace moveit::core;

  VectorXd init_joint_pose = parameters.rightCols(1);
  VectorXd joint_pose;

  if(!runIK(tool_goal_pose_,init_joint_pose,joint_pose))
  {
    ROS_ERROR("UnderconstrainedGoal failed to find valid ik close to reference pose");
    filtered = false;
    return false;
  }

  filtered = true;
  parameters.rightCols(1) = joint_pose;

  return true;
}

bool UnderconstrainedGoal::runIK(const Eigen::Affine3d& tool_goal_pose,const Eigen::VectorXd& init_joint_pose,
                                       Eigen::VectorXd& joint_pose)
{
  using namespace Eigen;
  using namespace moveit::core;

  // joint variables
  VectorXd delta_j = VectorXd::Zero(init_joint_pose.size());
  joint_pose = init_joint_pose;
  const JointModelGroup* joint_group = robot_model_->getJointModelGroup(group_name_);
  state_->setJointGroupPositions(joint_group,joint_pose);
  Affine3d tool_current_pose = state_->getGlobalLinkTransform(tool_link_);

  // tool twist variables
  VectorXd tool_twist, tool_twist_reduced;
  std::vector<int> indices;
  for(auto i = 0u; i < dof_nullity_.size(); i++)
  {
    if(dof_nullity_(i) != 0)
    {
      indices.push_back(i);
    }
  }
  tool_twist_reduced = VectorXd::Zero(indices.size());

  // jacobian calculation variables
  MatrixXd identity = MatrixXd::Identity(init_joint_pose.size(),init_joint_pose.size());
  MatrixXd jacb, jacb_reduced, jacb_pseudo_inv;

  unsigned int iteration_count = 0;
  bool converged = false;
  while(iteration_count < max_iterations_)
  {

    // computing twist vector
    computeTwist(tool_current_pose,tool_goal_pose,dof_nullity_,tool_twist);

    // check convergence
    if((tool_twist.cwiseAbs().array() < cartesian_convergence_thresholds_).all())
    {
      // converged
      converged = true;
      ROS_DEBUG("Found numeric ik solution after %i iterations",iteration_count);
      break;
    }

    // updating reduced tool twist
    for(auto i = 0u; i < indices.size(); i++)
    {
      tool_twist_reduced(i) = tool_twist(indices[i]);
    }

    // computing jacobian
    if(!state_->getJacobian(joint_group,state_->getLinkModel(tool_link_),Vector3d::Zero(),jacb))
    {
      ROS_ERROR("Failed to get Jacobian for link %s",tool_link_.c_str());
      return false;
    }

    // transform jacobian rotational part to tool coordinates
    jacb.bottomRows(3) = tool_current_pose.rotation().transpose()*jacb.bottomRows(3);

    // reduce jacobian and compute its pseudo inverse
    reduceJacobian(jacb,indices,jacb_reduced);
    calculateDampedPseudoInverse(jacb_reduced,jacb_pseudo_inv,EPSILON,LAMBDA);

    // computing joint change
    delta_j = (jacb_pseudo_inv*tool_twist_reduced);

    // updating joint values
    joint_pose += (joint_update_rates_* delta_j.array()).matrix();

    // updating tool pose
    state_->setJointGroupPositions(joint_group,joint_pose);
    tool_current_pose = state_->getGlobalLinkTransform(tool_link_);

    iteration_count++;
  }

  ROS_DEBUG_STREAM("Final tool twist "<<tool_twist.transpose());

  return converged;
}



} /* namespace filters */
} /* namespace stomp_moveit */
