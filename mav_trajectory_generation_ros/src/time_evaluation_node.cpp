#include <ros/ros.h>

#include <mav_visualization/helpers.h>
#include <mav_trajectory_generation/polynomial_optimization_linear.h>
#include <mav_trajectory_generation/polynomial_optimization_nonlinear.h>
#include <mav_trajectory_generation/timing.h>

#include "mav_trajectory_generation_ros/ros_conversions.h"
#include "mav_trajectory_generation_ros/ros_visualization.h"
#include "mav_trajectory_generation_ros/trajectory_sampling.h"

namespace mav_trajectory_generation {

// Benchmarking utilities to evaluate different methods of time allocation for
// polynomial trajectories.

struct TimeAllocationBenchmarkResult {
  TimeAllocationBenchmarkResult()
      : trial_number(-1),
        method_name("none"),
        num_segments(0),
        nominal_length(0.0),
        optimization_success(false),
        bounds_violated(false),
        trajectory_time(0.0),
        trajectory_length(0.0),
        computation_time(0.0) {}

  // Evaluation settings
  int trial_number;
  std::string method_name;

  // Trajectory settings
  int num_segments;
  double nominal_length;

  // Evaluation results
  bool optimization_success;
  bool bounds_violated;
  double trajectory_time;
  double trajectory_length;
  double computation_time;
  double a_max_actual;
  double v_max_actual;

  // More to come: convex hull/bounding box, etc.
};

class TimeEvaluationNode {
 public:
  TimeEvaluationNode(const ros::NodeHandle& nh,
                     const ros::NodeHandle& nh_private);

  // Number of Coefficients
  const static int kN = 10;  // has to be even !!
  // Dimension
  const static int kDim = 3;

  // Running the actual benchmark, one trial at a time (so that it can be
  // paused between for visualization).
  void runBenchmark(int trial_number, int num_segments);

  // Generate trajectories with different methods.
  void runNfabian(const Vertex::Vector& vertices, Trajectory* trajectory) const;
  void runNonlinear(const Vertex::Vector& vertices,
                    Trajectory* trajectory) const;

  void evaluateTrajectory(const std::string& method_name,
                          const Trajectory& traj,
                          TimeAllocationBenchmarkResult* result) const;

  void visualizeTrajectory(const std::string& method_name,
                           const Trajectory& traj,
                           visualization_msgs::MarkerArray* markers);

  // Accessors.
  bool visualize() const { return visualize_; }

  // Helpers.
  visualization_msgs::Marker createMarkerForPath(
      mav_msgs::EigenTrajectoryPointVector& path,
      const std_msgs::ColorRGBA& color, const std::string& name,
      double scale = 0.05) const;

  double computePathLength(mav_msgs::EigenTrajectoryPointVector& path) const;

 private:
  ros::NodeHandle nh_;
  ros::NodeHandle nh_private_;

  // General settings.
  std::string frame_id_;
  bool visualize_;

  // Dynamic constraints.
  double v_max_;
  double a_max_;

  // General trajectory settings.
  int max_derivative_order_;

  // Store all the results.
  std::vector<TimeAllocationBenchmarkResult> results_;

  // ROS stuff.
  ros::Publisher path_marker_pub_;
};

TimeEvaluationNode::TimeEvaluationNode(const ros::NodeHandle& nh,
                                       const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      frame_id_("world"),
      visualize_(true),
      v_max_(1.0),
      a_max_(2.0),
      max_derivative_order_(derivative_order::JERK) {
  nh_private_.param("frame_id", frame_id_, frame_id_);
  nh_private_.param("visualize", visualize_, visualize_);
  nh_private_.param("v_max", v_max_, v_max_);
  nh_private_.param("a_max", a_max_, a_max_);

  path_marker_pub_ =
      nh_private_.advertise<visualization_msgs::MarkerArray>("path", 1, true);
}

void TimeEvaluationNode::runBenchmark(int trial_number, int num_segments) {
  srand(trial_number);

  const Eigen::VectorXd min_pos = Eigen::VectorXd::Constant(kDim, -5.0);
  const Eigen::VectorXd max_pos = -min_pos;

  // Use trial number as seed to create the trajectory.
  Vertex::Vector vertices;
  vertices = createRandomVertices(max_derivative_order_, num_segments, min_pos,
                                  max_pos, trial_number);

  TimeAllocationBenchmarkResult result;
  // Fill in all the basics in the results that are shared between all the
  // evaluations.
  result.trial_number = trial_number;
  result.num_segments = num_segments;
  double nominal_length = 0.0;
  // TODO(helenol): compute nominal length from the vertices...

  visualization_msgs::MarkerArray markers;

  // Run all the evaluations.
  std::string method_name = "nfabian";
  Trajectory trajectory_nfabian;
  runNfabian(vertices, &trajectory_nfabian);
  evaluateTrajectory(method_name, trajectory_nfabian, &result);
  results_.push_back(result);
  if (visualize_) {
    visualizeTrajectory(method_name, trajectory_nfabian, &markers);
  }

  method_name = "nonlinear";
  Trajectory trajectory_nonlinear;
  runNonlinear(vertices, &trajectory_nonlinear);
  evaluateTrajectory(method_name, trajectory_nonlinear, &result);
  results_.push_back(result);
  if (visualize_) {
    visualizeTrajectory(method_name, trajectory_nonlinear, &markers);
  }
}

void TimeEvaluationNode::runNfabian(const Vertex::Vector& vertices,
                                    Trajectory* trajectory) const {
  std::vector<double> segment_times;
  segment_times =
      mav_trajectory_generation::estimateSegmentTimes(vertices, v_max_, a_max_);

  mav_trajectory_generation::PolynomialOptimization<kN> linopt(kDim);
  linopt.setupFromVertices(vertices, segment_times, max_derivative_order_);
  linopt.solveLinear();
  linopt.getTrajectory(trajectory);
}

void TimeEvaluationNode::runNonlinear(const Vertex::Vector& vertices,
                                      Trajectory* trajectory) const {
  std::vector<double> segment_times;
  segment_times =
      mav_trajectory_generation::estimateSegmentTimes(vertices, v_max_, a_max_);

  mav_trajectory_generation::NonlinearOptimizationParameters nlopt_parameters;
  mav_trajectory_generation::PolynomialOptimizationNonLinear<kN> nlopt(
      kDim, nlopt_parameters, false);
  nlopt.setupFromVertices(vertices, segment_times, max_derivative_order_);
  nlopt.addMaximumMagnitudeConstraint(derivative_order::VELOCITY, v_max_);
  nlopt.addMaximumMagnitudeConstraint(derivative_order::ACCELERATION, a_max_);
  nlopt.optimize();
  nlopt.getTrajectory(trajectory);
}

void TimeEvaluationNode::visualizeTrajectory(
    const std::string& method_name, const Trajectory& traj,
    visualization_msgs::MarkerArray* markers) {
  // Maybe hash the method name to a color somehow????
  // Just hardcode it for now per method name.
  mav_visualization::Color trajectory_color;

  if (method_name == "nfabian") {
    trajectory_color = mav_visualization::Color::Yellow();
  } else if (method_name == "nonlinear") {
    trajectory_color = mav_visualization::Color::Red();
  } else {
    trajectory_color = mav_visualization::Color::White();
  }

  const double kDefaultSamplingTime = 0.1;  // In seconds.
  mav_msgs::EigenTrajectoryPointVector path;
  sampleWholeTrajectory(traj, kDefaultSamplingTime, &path);

  visualization_msgs::Marker marker;
  marker = createMarkerForPath(path, trajectory_color, method_name);

  markers->markers.push_back(marker);
}

void TimeEvaluationNode::evaluateTrajectory(
    const std::string& method_name, const Trajectory& traj,
    TimeAllocationBenchmarkResult* result) const {
  result->method_name = method_name;

  result->trajectory_time = traj.getMaxTime();

  // Evaluate path length.
  const double kDefaultSamplingTime = 0.1;  // In seconds.
  mav_msgs::EigenTrajectoryPointVector path;
  mav_trajectory_generation::sampleWholeTrajectory(traj, kDefaultSamplingTime,
                                                   &path);
  result->trajectory_length = computePathLength(path);

  // TODO(helenol): evaluate min/max extrema, bounds violations, etc.
}

visualization_msgs::Marker TimeEvaluationNode::createMarkerForPath(
    mav_msgs::EigenTrajectoryPointVector& path,
    const std_msgs::ColorRGBA& color, const std::string& name,
    double scale) const {
  visualization_msgs::Marker path_marker;

  const int kPublishEveryNSamples = 1;
  const double kMaxMagnitude = 100.0;

  path_marker.header.frame_id = "world";

  path_marker.header.stamp = ros::Time::now();
  path_marker.type = visualization_msgs::Marker::LINE_STRIP;
  path_marker.color = color;
  path_marker.ns = name;
  path_marker.scale.x = scale;

  path_marker.points.reserve(path.size() / kPublishEveryNSamples);
  int i = 0;
  for (const mav_msgs::EigenTrajectoryPoint& point : path) {
    i++;
    if (i % kPublishEveryNSamples != 0) {
      continue;
    }
    // Check that we're in some reasonable bounds.
    // Makes rviz stop crashing.
    if (point.position_W.maxCoeff() > kMaxMagnitude ||
        point.position_W.minCoeff() < -kMaxMagnitude) {
      continue;
    }

    geometry_msgs::Point point_msg;
    tf::pointEigenToMsg(point.position_W, point_msg);
    path_marker.points.push_back(point_msg);
  }

  return path_marker;
}

double TimeEvaluationNode::computePathLength(
    mav_msgs::EigenTrajectoryPointVector& path) const {
  Eigen::Vector3d last_point;
  double distance = 0;
  for (int i = 0; i < path.size(); ++i) {
    const mav_msgs::EigenTrajectoryPoint& point = path[i];

    if (i > 0) {
      distance += (point.position_W - last_point).norm();
    }
    last_point = point.position_W;
  }

  return distance;
}

}  // namespace mav_trajectory_generation

int main(int argc, char** argv) {
  ros::init(argc, argv, "time_evaluation_node");
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  ros::NodeHandle nh("");
  ros::NodeHandle nh_private("~");

  mav_trajectory_generation::TimeEvaluationNode time_eval_node(nh, nh_private);

  ROS_INFO("Initialized time evaluation node.");

  ros::spin();
  return 0;
}