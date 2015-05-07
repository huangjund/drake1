#ifndef SYSTEMS_ROBOTINTERFACES_QPLOCOMOTIONPLAN_H_
#define SYSTEMS_ROBOTINTERFACES_QPLOCOMOTIONPLAN_H_

#include <vector>
#include <map>
#include <string>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include "PiecewisePolynomial.h"
#include "ExponentialPlusPiecewisePolynomial.h"
#include "RigidBodyManipulator.h"
#include "drake/lcmt_qp_controller_input.hpp"
#include "BodyMotionData.h"
#include "Side.h"
#include <lcm/lcm-cpp.hpp>

struct TVLQRData {
  // TODO: move into its own file
  // TODO: turn into class, private members
  Eigen::MatrixXd A;
  Eigen::MatrixXd B;
  Eigen::MatrixXd C;
  Eigen::MatrixXd D;
  Eigen::VectorXd x0;
  Eigen::VectorXd y0;
  Eigen::VectorXd u0;
  Eigen::MatrixXd R;
  Eigen::MatrixXd Qy;
  Eigen::MatrixXd S;
  Eigen::VectorXd s1;
  Eigen::VectorXd s1dot;
  double s2;
  double s2dot;
};

class QuadraticLyapunovFunction {
  // TODO: move into its own file
  // TODO: make part of a Lyapunov function class hierarchy
  // TODO: more functionality
private:
  Eigen::MatrixXd S;
  ExponentialPlusPiecewisePolynomial<double> s1;

public:
  QuadraticLyapunovFunction() { }

  template<typename DerivedS>
  QuadraticLyapunovFunction(const MatrixBase<DerivedS>& S, const ExponentialPlusPiecewisePolynomial<double>& s1) :
      S(S), s1(s1) { }

  const Eigen::MatrixXd& getS() const
  {
    return S;
  }

  const ExponentialPlusPiecewisePolynomial<double>& getS1() const
  {
    return s1;
  }
};

struct RigidBodySupportStateElement {
  // TODO: turn this into a class with more functionality
  // TODO: consolidate with SupportStateElement?
  int body; // TODO: should probably be a RigidBody smart pointer
  Eigen::Matrix3Xd contact_points;
  bool use_contact_surface;
  Eigen::Vector4d support_surface; // TODO: should probably be a different type
};

typedef std::vector<RigidBodySupportStateElement> RigidBodySupportState;


enum SupportLogicType {
  REQUIRE_SUPPORT, ONLY_IF_FORCE_SENSED, ONLY_IF_KINEMATIC, KINEMATIC_OR_SENSED, PREVENT_SUPPORT
};

struct KneeSettings {
  double min_knee_angle;
  double knee_kp;
  double knee_kd;
  double knee_weight;
};

struct QPLocomotionPlanSettings {
  double duration;
  std::vector<RigidBodySupportState> supports;
  std::vector<double> support_times; // length: supports.size() + 1
  typedef std::map<std::string, Eigen::Matrix3Xd> ContactNameToContactPointsMap;
  std::vector<ContactNameToContactPointsMap> contact_groups; // one for each RigidBody
  std::vector<bool> planned_support_command;
  double early_contact_allowed_time;

  std::vector<BodyMotionData> body_motions;
  PiecewisePolynomial<double> zmp_trajectory;
  Eigen::Vector2d zmp_final;
  TVLQRData zmp_data;
  double lipm_height;
  QuadraticLyapunovFunction V;
  PiecewisePolynomial<double> q_traj;
  ExponentialPlusPiecewisePolynomial<double> com_traj;

  std::string gain_set;
  double mu;
  std::vector<Eigen::DenseIndex> plan_shift_zmp_indices;
  std::vector<Eigen::DenseIndex> plan_shift_body_motion_indices;
  double g;
  double min_foot_shift_delay = 0.2; // seconds to wait before updating foot-specific plan shifts
  bool is_quasistatic;
  KneeSettings knee_settings;
  std::string pelvis_name;
  std::map<Side, std::string> foot_names;
  std::map<Side, std::string> knee_names;
  std::vector<int> constrained_position_indices;
  std::vector<int> untracked_position_indices;

  void addSupport(const RigidBodySupportState& support_state, const ContactNameToContactPointsMap& contact_group_name_to_contact_points, double duration) {
    supports.push_back(support_state);
    contact_groups.push_back(contact_group_name_to_contact_points);
    if (support_times.empty())
      support_times.push_back(0.0);
    support_times.push_back(support_times[support_times.size() - 1] + duration);
  }

  // may be useful later
  static KneeSettings createDefaultKneeSettings() {
    KneeSettings knee_settings;
    knee_settings.min_knee_angle = 0.7;
    knee_settings.knee_kp = 40.0;
    knee_settings.knee_kd = 4.0;
    knee_settings.knee_weight = 1.0;
    return knee_settings;
  }

  // may be useful later in setting up constrained_position_indices
  static std::vector<int> findPositionIndices(RigidBodyManipulator& robot, const std::vector<std::string>& joint_name_substrings)
  {
    std::vector<int> ret;
    for (auto body_it = robot.bodies.begin(); body_it != robot.bodies.end(); ++body_it) {
      RigidBody& body = **body_it;
      if (body.hasParent()) {
        const DrakeJoint& joint = body.getJoint();
        for (auto joint_name_it = joint_name_substrings.begin(); joint_name_it != joint_name_substrings.end(); ++joint_name_it) {
          if (joint.getName().find(*joint_name_it) != std::string::npos) {
            for (int i = 0; i < joint.getNumPositions(); i++) {
              ret.push_back(body.position_num_start + i);
            }
            break;
          }
        }
      }
    }
    return ret;
  }

};

class QPLocomotionPlan
{
private:
  RigidBodyManipulator& robot; // TODO: const correctness
  QPLocomotionPlanSettings settings;
  const std::map<Side, int> foot_body_ids;
  const std::map<Side, int> knee_indices;
  const int pelvis_id;

  lcm::LCM lcm;
  std::string lcm_channel;

  double start_time;
  Vector3d plan_shift;
  std::map<Side,Vector3d> foot_shifts;
  double last_foot_shift_time = 0;
  drake::lcmt_qp_controller_input last_qp_input;
  std::map<Side, bool> toe_off_active;

  /*
   * when the plan says a given body is in support, require the controller to use that support.
   * To allow the controller to use that support only if it thinks the body is in contact with the terrain, try KINEMATIC_OR_SENSED
   */
  const static std::map<SupportLogicType, std::vector<bool> > support_logic_maps;

public:
  QPLocomotionPlan(RigidBodyManipulator& robot, const QPLocomotionPlanSettings& settings, const std::string& lcm_channel);

  /*
   * Get the input structure which can be passed to the stateless QP control loop
   * @param t the current time
   * @param q the current robot configuration
   * @param v the current robot velocity
   * @param contact_force_detected num_bodies vector indicating whether contact force
   */
  template <typename DerivedQ, typename DerivedV>
  drake::lcmt_qp_controller_input createQPControllerInput(double t_global, const Eigen::MatrixBase<DerivedQ>& q, const Eigen::MatrixBase<DerivedV>& v, const std::vector<bool>& contact_force_detected);

  void setDuration(double duration);

  void setStartTime(double start_time);

  double getStartTime() const;

  double getDuration() const;

  bool isFinished(double t) const;

  drake::lcmt_qp_controller_input getLastQPInput() const;

  const RigidBodyManipulator& getRobot() const;

private:
  drake::lcmt_zmp_data createZMPData(double t_plan) const;

  drake::lcmt_support_data createSupportDataElement(const RigidBodySupportStateElement& element, const std::vector<bool>& support_logic);

  bool isSupportingBody(int body_index, const RigidBodySupportState& support_state) const;

  std::vector<Side> getSupportSides(const RigidBodySupportState &support_state) const;

  void updateSwingTrajectory(double t_plan, BodyMotionData& body_motion_data, int body_motion_segment_index, const Eigen::VectorXd& qd);

  void updatePlanShift(double t_plan, const std::vector<bool>& contact_force_detected, int support_index);

  static const std::map<SupportLogicType, std::vector<bool> > createSupportLogicMaps();

  static const std::map<Side, int> createFootBodyIdMap(RigidBodyManipulator& robot, const std::map<Side, std::string>& foot_names);

  static const std::map<Side, int> createKneeIndicesMap(RigidBodyManipulator& robot, const std::map<Side, std::string>& foot_body_ids);
};

#endif /* SYSTEMS_ROBOTINTERFACES_QPLOCOMOTIONPLAN_H_ */
