/*
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* Author: Brian Gerkey */

#define ROS_INFO RCUTILS_LOG_INFO
#define ROS_ERROR RCUTILS_LOG_ERROR
#define ROS_FATAL RCUTILS_LOG_FATAL
#define ROS_WARN RCUTILS_LOG_WARN
#define ROS_DEBUG RCUTILS_LOG_DEBUG

#include <algorithm>
#include <vector>
#include <map>
#include <mutex>
#include <cmath>

// Signal handling
#include <signal.h>

#include "amcl/map/map.h"
#include "amcl/pf/pf.h"
#include "amcl/sensors/amcl_odom.h"
#include "amcl/sensors/amcl_laser.h"

// #include "ros/assert.h"

// roscpp
#include "rclcpp/rclcpp.hpp"
#include "rclcpp/time.hpp"
#include "rcutils/cmdline_parser.h"
#include "rcutils/logging_macros.h"

// Messages that I need
#include "sensor_msgs/msg/laser_scan.hpp"
#include "geometry_msgs/msg/pose_with_covariance_stamped.hpp"
#include "geometry_msgs/msg/pose_array.hpp"
#include "geometry_msgs/msg/pose.hpp"
#include "nav_msgs/srv/get_map.hpp"
#include "nav_msgs/srv/set_map.hpp"
#include "std_srvs/srv/empty.hpp"

// For transform support
#include "tf2_ros/buffer.h"
#include "tf2_ros/transform_broadcaster.h"
#include "tf2_ros/transform_listener.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "tf2/LinearMath/Transform.h"
#include "tf2/LinearMath/Quaternion.h"
#include "tf2/utils.h"
#include "tf2/transform_datatypes.h"
// TODO(dhood): Re-enable message filters
/*
#include "tf/message_filter.h"
#include "message_filters/subscriber.h"
*/

// TODO(dhood): Re-enable dynamic reconfigure
// Dynamic_reconfigure
// #include "dynamic_reconfigure/server.h"
// #include "amcl/AMCLConfig.h"

// TODO(dhood): Re-enable rosbag playback
// Allows AMCL to run from bag file
/*
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <boost/foreach.hpp>
*/

#define NEW_UNIFORM_SAMPLING 1

using namespace amcl;

// Pose hypothesis
typedef struct
{
  // Total weight (weights sum to 1)
  double weight;

  // Mean of pose esimate
  pf_vector_t pf_pose_mean;

  // Covariance of pose estimate
  pf_matrix_t pf_pose_cov;

} amcl_hyp_t;

static double
normalize(double z)
{
  return atan2(sin(z),cos(z));
}
static double
angle_diff(double a, double b)
{
  double d1, d2;
  a = normalize(a);
  b = normalize(b);
  d1 = a-b;
  d2 = 2*M_PI - fabs(d1);
  if(d1 > 0)
    d2 *= -1.0;
  if(fabs(d1) < fabs(d2))
    return(d1);
  else
    return(d2);
}

static const std::string scan_topic_ = "scan";

class AmclNode
{
  public:
    AmclNode(std::shared_ptr<rclcpp::Node> node, bool use_map_topic_);
    ~AmclNode();

    // TODO(dhood): Re-enable rosbag playback
    /**
     * @brief Uses TF and LaserScan messages from bag file to drive AMCL instead
     */
    // void runFromBag(const std::string &in_bag_fn);

    int process();
    void savePoseToServer();

  private:
    tf2_ros::TransformBroadcaster* tfb_;

    tf2_ros::Buffer* tf2_buffer_;

    tf2_ros::TransformListener* tfl_;

    bool sent_first_transform_;

    tf2::Transform latest_tf_;
    bool latest_tf_valid_;

    // Pose-generating function used to uniformly distribute particles over
    // the map
    static pf_vector_t uniformPoseGenerator(void* arg);
#if NEW_UNIFORM_SAMPLING
    static std::vector<std::pair<int,int> > free_space_indices;
#endif
    // Callbacks
    void globalLocalizationCallback(const std::shared_ptr<std_srvs::srv::Empty::Request> req,
                                    std::shared_ptr<std_srvs::srv::Empty::Response> res);
    void nomotionUpdateCallback(const std::shared_ptr<std_srvs::srv::Empty::Request> req,
                                    std::shared_ptr<std_srvs::srv::Empty::Response> res);
    void setMapCallback(const std::shared_ptr<nav_msgs::srv::SetMap::Request> req,
                        std::shared_ptr<nav_msgs::srv::SetMap::Response> res);

    void laserReceived(const std::shared_ptr<sensor_msgs::msg::LaserScan> laser_scan);
    void initialPoseReceived(const std::shared_ptr<geometry_msgs::msg::PoseWithCovarianceStamped> msg);
    void handleInitialPoseMessage(const geometry_msgs::msg::PoseWithCovarianceStamped& msg);
    void mapReceived(const std::shared_ptr<nav_msgs::msg::OccupancyGrid> msg);

    void handleMapMessage(const nav_msgs::msg::OccupancyGrid& msg);
    void freeMapDependentMemory();
    map_t* convertMap( const nav_msgs::msg::OccupancyGrid& map_msg );
    void updatePoseFromServer();
    void applyInitialPose();

    double getYaw(tf2::Transform& t);

    //parameter for what odom to use
    std::string odom_frame_id_;

    //paramater to store latest odom pose
    tf2::Stamped<tf2::Transform> latest_odom_pose_;

    //parameter for what base to use
    std::string base_frame_id_;
    std::string global_frame_id_;

    bool use_map_topic_;
    bool first_map_only_;

    tf2::Duration gui_publish_period;
    tf2::TimePoint save_pose_last_time;
    tf2::Duration save_pose_period;

    geometry_msgs::msg::PoseWithCovarianceStamped last_published_pose;

    map_t* map_;
    char* mapdata;
    int sx, sy;
    double resolution;

    // TODO(dhood): Re-enable message filters
    // message_filters::Subscriber<sensor_msgs::msg::LaserScan>* laser_scan_sub_;
    rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr laser_scan_sub_;
    // tf::MessageFilter<sensor_msgs::msg::LaserScan>* laser_scan_filter_;
    rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initial_pose_sub_;
    std::vector< AMCLLaser* > lasers_;
    std::vector< bool > lasers_update_;
    std::map< std::string, int > frame_to_laser_;

    // Particle filter
    pf_t *pf_;
    double pf_err_, pf_z_;
    bool pf_init_;
    pf_vector_t pf_odom_pose_;
    double d_thresh_, a_thresh_;
    int resample_interval_;
    int resample_count_;
    double laser_min_range_;
    double laser_max_range_;

    //Nomotion update control
    bool m_force_update;  // used to temporarily let amcl update samples even when no motion occurs...

    AMCLOdom* odom_;
    AMCLLaser* laser_;

    tf2::Duration cloud_pub_interval;
    tf2::TimePoint last_cloud_pub_time;

    // For slowing play-back when reading directly from a bag file
    // ros::WallDuration bag_scan_period_;

    void requestMap();

    // Helper to get odometric pose from transform system
    bool getOdomPose(tf2::Stamped<tf2::Transform>& pose,
                     double& x, double& y, double& yaw,
                     const builtin_interfaces::msg::Time& t, const std::string& f);

    //time for tolerance on the published transform,
    //basically defines how long a map->odom transform is good for
    tf2::Duration transform_tolerance_;

    std::shared_ptr<rclcpp::Node> node;
    rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr pose_pub_;
    rclcpp::Publisher<geometry_msgs::msg::PoseArray>::SharedPtr particlecloud_pub_;
    rclcpp::ServiceBase::SharedPtr global_loc_srv_;
    rclcpp::ServiceBase::SharedPtr nomotion_update_srv_; //to let amcl update samples without requiring motion
    rclcpp::ServiceBase::SharedPtr set_map_srv_;
    rclcpp::Subscription<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initial_pose_sub_old_;
    rclcpp::Subscription<nav_msgs::msg::OccupancyGrid>::SharedPtr map_sub_;

    amcl_hyp_t* initial_pose_hyp_;
    bool first_map_received_;
    bool first_reconfigure_call_;

    std::recursive_mutex configuration_mutex_;
    // TODO(dhood): Re-enable dynamic reconfigure
    /*
    dynamic_reconfigure::Server<amcl::AMCLConfig> *dsrv_;
    amcl::AMCLConfig default_config_;
    */
    rclcpp::TimerBase::SharedPtr check_laser_timer_;

    int max_beams_, min_particles_, max_particles_;
    double alpha1_, alpha2_, alpha3_, alpha4_, alpha5_;
    double alpha_slow_, alpha_fast_;
    double z_hit_, z_short_, z_max_, z_rand_, sigma_hit_, lambda_short_;
  //beam skip related params
    bool do_beamskip_;
    double beam_skip_distance_, beam_skip_threshold_, beam_skip_error_threshold_;
    double laser_likelihood_max_dist_;
    odom_model_t odom_model_type_;
    double init_pose_[3];
    double init_cov_[3];
    laser_model_t laser_model_type_;
    bool tf_broadcast_;

    // void reconfigureCB(amcl::AMCLConfig &config, uint32_t level);

    tf2::TimePoint last_laser_received_ts_;
    tf2::Duration laser_check_interval_;
    void checkLaserReceived();
};

std::vector<std::pair<int,int> > AmclNode::free_space_indices;

#define USAGE "USAGE: amcl"

std::shared_ptr<AmclNode> amcl_node_ptr;

// TODO(dhood): Re-enable signal handler when global parameter server in use
/*
void sigintHandler(int sig)
{
  // Save latest pose as we're shutting down.
  amcl_node_ptr->savePoseToServer();
  rclcpp::shutdown();
}
*/

void print_usage()
{
  printf("Usage for amcl:\n");
  printf("amcl [--use-map-topic] [-h]\n");
  printf("options:\n");
  printf("-h : Print this help function.\n");
  printf("--use-map-topic: listen for the map on a topic instead of making a service call.\n");
}

int
main(int argc, char** argv)
{
  // force flush of the stdout buffer.
  // this ensures a correct sync of all prints
  // even when executed simultaneously within the launch file.
  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  rclcpp::init(argc, argv);
  rcutils_logging_initialize();

  auto node = rclcpp::Node::make_shared("amcl");
  auto parameter_service = std::make_shared<rclcpp::ParameterService>(node);

  if (rcutils_cli_option_exist(argv, argv + argc, "-h")) {
    print_usage();
    return 0;
  }
  bool use_map_topic = false;
  if (rcutils_cli_option_exist(argv, argv + argc, "--use-map-topic")) {
    use_map_topic = true;
  }

  // Override default sigint handler
  // signal(SIGINT, sigintHandler);

  // Make our node available to sigintHandler
  amcl_node_ptr.reset(new AmclNode(node, use_map_topic));

  rclcpp::spin(node);
  /*
  if (argc == 1)
  {
    // run using ROS input
    rclcpp::spin(node);
  }
  else if ((argc == 3) && (std::string(argv[1]) == "--run-from-bag"))
  {
    amcl_node_ptr->runFromBag(argv[2]);
  }
  */

  // Without this, our locks are not shut down nicely
  amcl_node_ptr.reset();

  // To quote Morgan, Hooray!
  return(0);
}

AmclNode::AmclNode(std::shared_ptr<rclcpp::Node> node_, bool use_map_topic) :
        sent_first_transform_(false),
        latest_tf_valid_(false),
        map_(NULL),
        pf_(NULL),
        resample_count_(0),
        odom_(NULL),
        laser_(NULL),
        initial_pose_hyp_(NULL),
        first_map_received_(false),
        first_reconfigure_call_(true)
{
  std::lock_guard<std::recursive_mutex> l(configuration_mutex_);
  node = node_;
  
  last_laser_received_ts_ = tf2_ros::fromMsg(rclcpp::Time::now());

  // Grab params off the param server

  // TODO(dhood): restore this paramter in place of command line option
  // node->get_parameter_or("use_map_topic", use_map_topic_, false);
  use_map_topic_ = use_map_topic;
  node->get_parameter_or("first_map_only", first_map_only_, false);

  double tmp;
  node->get_parameter_or("gui_publish_rate", tmp, -1.0);
  gui_publish_period = tf2::durationFromSec(1.0/tmp);
  node->get_parameter_or("save_pose_rate", tmp, 0.5);
  save_pose_period = tf2::durationFromSec(1.0/tmp);

  node->get_parameter_or("laser_min_range", laser_min_range_, -1.0);
  node->get_parameter_or("laser_max_range", laser_max_range_, -1.0);
  node->get_parameter_or("laser_max_beams", max_beams_, 30);
  node->get_parameter_or("min_particles", min_particles_, 100);
  node->get_parameter_or("max_particles", max_particles_, 5000);
  node->get_parameter_or("kld_err", pf_err_, 0.01);
  node->get_parameter_or("kld_z", pf_z_, 0.99);
  node->get_parameter_or("odom_alpha1", alpha1_, 0.2);
  node->get_parameter_or("odom_alpha2", alpha2_, 0.2);
  node->get_parameter_or("odom_alpha3", alpha3_, 0.2);
  node->get_parameter_or("odom_alpha4", alpha4_, 0.2);
  node->get_parameter_or("odom_alpha5", alpha5_, 0.2);
  
  node->get_parameter_or("do_beamskip", do_beamskip_, false);
  node->get_parameter_or("beam_skip_distance", beam_skip_distance_, 0.5);
  node->get_parameter_or("beam_skip_threshold", beam_skip_threshold_, 0.3);
  node->get_parameter_or("beam_skip_error_threshold_", beam_skip_error_threshold_, 0.9);

  node->get_parameter_or("laser_z_hit", z_hit_, 0.95);
  node->get_parameter_or("laser_z_short", z_short_, 0.1);
  node->get_parameter_or("laser_z_max", z_max_, 0.05);
  node->get_parameter_or("laser_z_rand", z_rand_, 0.05);
  node->get_parameter_or("laser_sigma_hit", sigma_hit_, 0.2);
  node->get_parameter_or("laser_lambda_short", lambda_short_, 0.1);
  node->get_parameter_or("laser_likelihood_max_dist", laser_likelihood_max_dist_, 2.0);
  std::string tmp_model_type;
  node->get_parameter_or("laser_model_type", tmp_model_type, std::string("likelihood_field"));
  if(tmp_model_type == "beam")
    laser_model_type_ = LASER_MODEL_BEAM;
  else if(tmp_model_type == "likelihood_field")
    laser_model_type_ = LASER_MODEL_LIKELIHOOD_FIELD;
  else if(tmp_model_type == "likelihood_field_prob"){
    laser_model_type_ = LASER_MODEL_LIKELIHOOD_FIELD_PROB;
  }
  else
  {
    ROS_WARN("Unknown laser model type \"%s\"; defaulting to likelihood_field model",
             tmp_model_type.c_str());
    laser_model_type_ = LASER_MODEL_LIKELIHOOD_FIELD;
  }

  node->get_parameter_or("odom_model_type", tmp_model_type, std::string("diff"));
  if(tmp_model_type == "diff")
    odom_model_type_ = ODOM_MODEL_DIFF;
  else if(tmp_model_type == "omni")
    odom_model_type_ = ODOM_MODEL_OMNI;
  else if(tmp_model_type == "diff-corrected")
    odom_model_type_ = ODOM_MODEL_DIFF_CORRECTED;
  else if(tmp_model_type == "omni-corrected")
    odom_model_type_ = ODOM_MODEL_OMNI_CORRECTED;
  else
  {
    ROS_WARN("Unknown odom model type \"%s\"; defaulting to diff model",
             tmp_model_type.c_str());
    odom_model_type_ = ODOM_MODEL_DIFF;
  }

  node->get_parameter_or("update_min_d", d_thresh_, 0.2);
  node->get_parameter_or("update_min_a", a_thresh_, M_PI/6.0);
  node->get_parameter_or("odom_frame_id", odom_frame_id_, std::string("odom"));
  node->get_parameter_or("base_frame_id", base_frame_id_, std::string("base_link"));
  node->get_parameter_or("global_frame_id", global_frame_id_, std::string("map"));
  node->get_parameter_or("resample_interval", resample_interval_, 2);
  double tmp_tol;
  node->get_parameter_or("transform_tolerance", tmp_tol, 0.1);
  node->get_parameter_or("recovery_alpha_slow", alpha_slow_, 0.001);
  node->get_parameter_or("recovery_alpha_fast", alpha_fast_, 0.1);
  node->get_parameter_or("tf_broadcast", tf_broadcast_, true);

  transform_tolerance_ = tf2::durationFromSec(tmp_tol);

  /*
  {
    double bag_scan_period;
    node->get_parameter_or("bag_scan_period", bag_scan_period, -1.0);
    bag_scan_period_.fromSec(bag_scan_period);
  }
  */

  updatePoseFromServer();
 

  cloud_pub_interval = tf2::durationFromSec(1.0);
  tfb_ = new tf2_ros::TransformBroadcaster(node);
  tf2_buffer_ = new tf2_ros::Buffer();
  tf2_buffer_->setUsingDedicatedThread(true);
  tfl_ = new tf2_ros::TransformListener(*tf2_buffer_, node, false);

  rmw_qos_profile_t qos = rmw_qos_profile_default;
  qos.depth = 2;
  qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  pose_pub_ = node->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>("amcl_pose", qos);
  particlecloud_pub_ = node->create_publisher<geometry_msgs::msg::PoseArray>("particlecloud", qos);
  global_loc_srv_ = node->create_service<std_srvs::srv::Empty>("global_localization",
					 std::bind(&AmclNode::globalLocalizationCallback, this, std::placeholders::_1, std::placeholders::_2));
  nomotion_update_srv_= node->create_service<std_srvs::srv::Empty>("request_nomotion_update",
					 std::bind(&AmclNode::nomotionUpdateCallback, this, std::placeholders::_1, std::placeholders::_2));
  set_map_srv_= node->create_service<nav_msgs::srv::SetMap>("set_map",
					 std::bind(&AmclNode::setMapCallback, this, std::placeholders::_1, std::placeholders::_2));

  // laser_scan_sub_ = new message_filters::Subscriber<sensor_msgs::msg::LaserScan>(nh_, scan_topic_, 100);
  qos = rmw_qos_profile_sensor_data;
  qos.depth = 100;
  laser_scan_sub_ = node->create_subscription<sensor_msgs::msg::LaserScan>(scan_topic_, std::bind(&AmclNode::laserReceived, this, std::placeholders::_1), qos);
  /*
  laser_scan_filter_ =
          new tf::MessageFilter<sensor_msgs::msg::LaserScan>(*laser_scan_sub_,
                                                        *tf_,
                                                        odom_frame_id_,
                                                        100);
  laser_scan_filter_->registerCallback(std::bind(&AmclNode::laserReceived,
                                                   this, _1));
 */
  qos = rmw_qos_profile_default;
  qos.depth = 2;
  initial_pose_sub_ = node->create_subscription<geometry_msgs::msg::PoseWithCovarianceStamped>("initialpose", std::bind(&AmclNode::initialPoseReceived, this, std::placeholders::_1), qos);

  if(use_map_topic_) {
    qos.depth = 2;
    // The map server publishes the map message once but with transient_local durability.
    // If the map has been published before our subscription was connected, we can utilize the transient_local durability to receive the previous message.
    // This will, however, render the subscription unable to connect to other publishers with volatile durability.
    qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;

    map_sub_ = node->create_subscription<nav_msgs::msg::OccupancyGrid>("map", std::bind(&AmclNode::mapReceived, this, std::placeholders::_1), qos);
  } else {
    requestMap();
  }
  m_force_update = false;

  /*
  dsrv_ = new dynamic_reconfigure::Server<amcl::AMCLConfig>(ros::NodeHandle("~"));
  dynamic_reconfigure::Server<amcl::AMCLConfig>::CallbackType cb = std::bind(&AmclNode::reconfigureCB, this, _1, _2);
  dsrv_->setCallback(cb);
  */

  // 15s timer to warn on lack of receipt of laser scans, #5209
  laser_check_interval_ = std::chrono::seconds(15);
  check_laser_timer_ = node->create_wall_timer(laser_check_interval_,
                                               std::bind(&AmclNode::checkLaserReceived, this));
}

/*
void AmclNode::reconfigureCB(AMCLConfig &config, uint32_t level)
{
  std::lock_guard<std::recursive_mutex> cfl(configuration_mutex_);

  //we don't want to do anything on the first call
  //which corresponds to startup
  if(first_reconfigure_call_)
  {
    first_reconfigure_call_ = false;
    default_config_ = config;
    return;
  }

  if(config.restore_defaults) {
    config = default_config_;
    //avoid looping
    config.restore_defaults = false;
  }

  d_thresh_ = config.update_min_d;
  a_thresh_ = config.update_min_a;

  resample_interval_ = config.resample_interval;

  laser_min_range_ = config.laser_min_range;
  laser_max_range_ = config.laser_max_range;

  gui_publish_period = tf2::durationFromSec(1.0/config.gui_publish_rate);
  save_pose_period = tf2::durationFromSec(1.0/config.save_pose_rate);

  transform_tolerance_.fromSec(config.transform_tolerance);

  max_beams_ = config.laser_max_beams;
  alpha1_ = config.odom_alpha1;
  alpha2_ = config.odom_alpha2;
  alpha3_ = config.odom_alpha3;
  alpha4_ = config.odom_alpha4;
  alpha5_ = config.odom_alpha5;

  z_hit_ = config.laser_z_hit;
  z_short_ = config.laser_z_short;
  z_max_ = config.laser_z_max;
  z_rand_ = config.laser_z_rand;
  sigma_hit_ = config.laser_sigma_hit;
  lambda_short_ = config.laser_lambda_short;
  laser_likelihood_max_dist_ = config.laser_likelihood_max_dist;

  if(config.laser_model_type == "beam")
    laser_model_type_ = LASER_MODEL_BEAM;
  else if(config.laser_model_type == "likelihood_field")
    laser_model_type_ = LASER_MODEL_LIKELIHOOD_FIELD;
  else if(config.laser_model_type == "likelihood_field_prob")
    laser_model_type_ = LASER_MODEL_LIKELIHOOD_FIELD_PROB;

  if(config.odom_model_type == "diff")
    odom_model_type_ = ODOM_MODEL_DIFF;
  else if(config.odom_model_type == "omni")
    odom_model_type_ = ODOM_MODEL_OMNI;
  else if(config.odom_model_type == "diff-corrected")
    odom_model_type_ = ODOM_MODEL_DIFF_CORRECTED;
  else if(config.odom_model_type == "omni-corrected")
    odom_model_type_ = ODOM_MODEL_OMNI_CORRECTED;

  if(config.min_particles > config.max_particles)
  {
    ROS_WARN("You've set min_particles to be greater than max particles, this isn't allowed so they'll be set to be equal.");
    config.max_particles = config.min_particles;
  }

  min_particles_ = config.min_particles;
  max_particles_ = config.max_particles;
  alpha_slow_ = config.recovery_alpha_slow;
  alpha_fast_ = config.recovery_alpha_fast;
  tf_broadcast_ = config.tf_broadcast;

  do_beamskip_= config.do_beamskip; 
  beam_skip_distance_ = config.beam_skip_distance; 
  beam_skip_threshold_ = config.beam_skip_threshold; 

  pf_ = pf_alloc(min_particles_, max_particles_,
                 alpha_slow_, alpha_fast_,
                 (pf_init_model_fn_t)AmclNode::uniformPoseGenerator,
                 (void *)map_);
  pf_err_ = config.kld_err; 
  pf_z_ = config.kld_z; 
  pf_->pop_err = pf_err_;
  pf_->pop_z = pf_z_;

  // Initialize the filter
  pf_vector_t pf_init_pose_mean = pf_vector_zero();
  pf_init_pose_mean.v[0] = last_published_pose.pose.pose.position.x;
  pf_init_pose_mean.v[1] = last_published_pose.pose.pose.position.y;
  pf_init_pose_mean.v[2] = tf::getYaw(last_published_pose.pose.pose.orientation);
  pf_matrix_t pf_init_pose_cov = pf_matrix_zero();
  pf_init_pose_cov.m[0][0] = last_published_pose.pose.covariance[6*0+0];
  pf_init_pose_cov.m[1][1] = last_published_pose.pose.covariance[6*1+1];
  pf_init_pose_cov.m[2][2] = last_published_pose.pose.covariance[6*5+5];
  pf_init(pf_, pf_init_pose_mean, pf_init_pose_cov);
  pf_init_ = false;

  // Instantiate the sensor objects
  // Odometry
  delete odom_;
  odom_ = new AMCLOdom();
  // ROS_ASSERT(odom_);
  odom_->SetModel( odom_model_type_, alpha1_, alpha2_, alpha3_, alpha4_, alpha5_ );
  // Laser
  delete laser_;
  laser_ = new AMCLLaser(max_beams_, map_);
  // ROS_ASSERT(laser_);
  if(laser_model_type_ == LASER_MODEL_BEAM)
    laser_->SetModelBeam(z_hit_, z_short_, z_max_, z_rand_,
                         sigma_hit_, lambda_short_, 0.0);
  else if(laser_model_type_ == LASER_MODEL_LIKELIHOOD_FIELD_PROB){
    ROS_INFO("Initializing likelihood field model; this can take some time on large maps...");
    laser_->SetModelLikelihoodFieldProb(z_hit_, z_rand_, sigma_hit_,
					laser_likelihood_max_dist_, 
					do_beamskip_, beam_skip_distance_, 
					beam_skip_threshold_, beam_skip_error_threshold_);
    ROS_INFO("Done initializing likelihood field model with probabilities.");
  }
  else if(laser_model_type_ == LASER_MODEL_LIKELIHOOD_FIELD){
    ROS_INFO("Initializing likelihood field model; this can take some time on large maps...");
    laser_->SetModelLikelihoodField(z_hit_, z_rand_, sigma_hit_,
                                    laser_likelihood_max_dist_);
    ROS_INFO("Done initializing likelihood field model.");
  }

  odom_frame_id_ = config.odom_frame_id;
  base_frame_id_ = config.base_frame_id;
  global_frame_id_ = config.global_frame_id;

  delete laser_scan_filter_;
  laser_scan_filter_ = 
          new tf::MessageFilter<sensor_msgs::msg::LaserScan>(*laser_scan_sub_, 
                                                        *tf_, 
                                                        odom_frame_id_, 
                                                        100);
  laser_scan_filter_->registerCallback(std::bind(&AmclNode::laserReceived,
                                                   this, _1));

  initial_pose_sub_ = nh_.subscribe("initialpose", 2, &AmclNode::initialPoseReceived, this);
}
*/


/*
void AmclNode::runFromBag(const std::string &in_bag_fn)
{
  rosbag::Bag bag;
  bag.open(in_bag_fn, rosbag::bagmode::Read);
  std::vector<std::string> topics;
  topics.push_back(std::string("tf"));
  std::string scan_topic_name = "base_scan"; // TODO determine what topic this actually is from ROS
  topics.push_back(scan_topic_name);
  rosbag::View view(bag, rosbag::TopicQuery(topics));

  ros::Publisher laser_pub = nh_.advertise<sensor_msgs::msg::LaserScan>(scan_topic_name, 100);
  ros::Publisher tf_pub = nh_.advertise<tf2_msgs::TFMessage>("/tf", 100);

  // Sleep for a second to let all subscribers connect
  ros::WallDuration(1.0).sleep();

  ros::WallTime start(ros::WallTime::now());

  // Wait for map
  while (ros::ok())
  {
    {
      std::lock_guard<std::recursive_mutex> cfl(configuration_mutex_);
      if (map_)
      {
        ROS_INFO("Map is ready");
        break;
      }
    }
    ROS_INFO("Waiting for map...");
    ros::getGlobalCallbackQueue()->callAvailable(ros::WallDuration(1.0));
  }

  BOOST_FOREACH(rosbag::MessageInstance const msg, view)
  {
    if (!ros::ok())
    {
      break;
    }

    // Process any ros messages or callbacks at this point
    ros::getGlobalCallbackQueue()->callAvailable(ros::WallDuration());

    tf2_msgs::TFMessage::ConstPtr tf_msg = msg->instantiate<tf2_msgs::TFMessage>();
    if (tf_msg != NULL)
    {
      tf_pub.publish(msg);
      for (size_t ii=0; ii<tf_msg->transforms.size(); ++ii)
      {
        tf_->getBuffer().setTransform(tf_msg->transforms[ii], "rosbag_authority");
      }
      continue;
    }

    sensor_msgs::msg::LaserScan::::ConstPtr base_scan = msg->instantiate<sensor_msgs::msg::LaserScan>();
    if (base_scan != NULL)
    {
      laser_pub.publish(msg);
      laser_scan_filter_->add(base_scan);
      if (bag_scan_period_ > ros::WallDuration(0))
      {
        bag_scan_period_.sleep();
      }
      continue;
    }

    // ROS_WARN_STREAM("Unsupported message type" << msg->getTopic());
  }

  bag.close();

  double runtime = (ros::WallTime::now() - start).toSec();
  ROS_INFO("Bag complete, took %.1f seconds to process, shutting down", runtime);

  const geometry_msgs::msg::Quaternion & q(last_published_pose.pose.pose.orientation);
  double yaw, pitch, roll;
  tf::Matrix3x3(tf::Quaternion(q.x, q.y, q.z, q.w)).getEulerYPR(yaw,pitch,roll);
  ROS_INFO("Final location %.3f, %.3f, %.3f with stamp=%f",
            last_published_pose.pose.pose.position.x,
            last_published_pose.pose.pose.position.y,
            yaw, last_published_pose.header.stamp.toSec()
            );

  ros::shutdown();
}
*/


void AmclNode::savePoseToServer()
{
  // We need to apply the last transform to the latest odom pose to get
  // the latest map pose to store.  We'll take the covariance from
  // last_published_pose.
  tf2::Transform map_pose = latest_tf_.inverse() * latest_odom_pose_;
  double yaw,pitch,roll;
  map_pose.getBasis().getEulerYPR(yaw, pitch, roll);

  ROS_DEBUG("Saving pose to server. x: %.3f, y: %.3f", map_pose.getOrigin().x(), map_pose.getOrigin().y() );

  auto set_parameters_results = node->set_parameters_atomically(
      {
       rclcpp::ParameterVariant("initial_pose_x", map_pose.getOrigin().x()),
       rclcpp::ParameterVariant("initial_pose_y", map_pose.getOrigin().y()),
       rclcpp::ParameterVariant("initial_pose_a", yaw),
       rclcpp::ParameterVariant("initial_cov_xx",
                                  last_published_pose.pose.covariance[6*0+0]),
       rclcpp::ParameterVariant("initial_cov_yy",
                                  last_published_pose.pose.covariance[6*1+1]),
       rclcpp::ParameterVariant("initial_cov_yy",
                                  last_published_pose.pose.covariance[6*5+5]),
      }
    );
  if (!set_parameters_results.successful) {
    ROS_ERROR("Failed to set parameter: %s", set_parameters_results.reason.c_str());
  }
}

void AmclNode::updatePoseFromServer()
{
  init_pose_[0] = 0.0;
  init_pose_[1] = 0.0;
  init_pose_[2] = 0.0;
  init_cov_[0] = 0.5 * 0.5;
  init_cov_[1] = 0.5 * 0.5;
  init_cov_[2] = (M_PI/12.0) * (M_PI/12.0);
  // Check for NAN on input from param server, #5239
  double tmp_pos;
  node->get_parameter_or("initial_pose_x", tmp_pos, init_pose_[0]);
  if(!std::isnan(tmp_pos))
    init_pose_[0] = tmp_pos;
  else 
    ROS_WARN("ignoring NAN in initial pose X position");
  node->get_parameter_or("initial_pose_y", tmp_pos, init_pose_[1]);
  if(!std::isnan(tmp_pos))
    init_pose_[1] = tmp_pos;
  else
    ROS_WARN("ignoring NAN in initial pose Y position");
  node->get_parameter_or("initial_pose_a", tmp_pos, init_pose_[2]);
  if(!std::isnan(tmp_pos))
    init_pose_[2] = tmp_pos;
  else
    ROS_WARN("ignoring NAN in initial pose Yaw");
  node->get_parameter_or("initial_cov_xx", tmp_pos, init_cov_[0]);
  if(!std::isnan(tmp_pos))
    init_cov_[0] =tmp_pos;
  else
    ROS_WARN("ignoring NAN in initial covariance XX");
  node->get_parameter_or("initial_cov_yy", tmp_pos, init_cov_[1]);
  if(!std::isnan(tmp_pos))
    init_cov_[1] = tmp_pos;
  else
    ROS_WARN("ignoring NAN in initial covariance YY");
  node->get_parameter_or("initial_cov_aa", tmp_pos, init_cov_[2]);
  if(!std::isnan(tmp_pos))
    init_cov_[2] = tmp_pos;
  else
    ROS_WARN("ignoring NAN in initial covariance AA");
}

void 
AmclNode::checkLaserReceived()
{
  tf2::Duration d = tf2_ros::fromMsg(rclcpp::Time::now()) - last_laser_received_ts_;
  if(d > laser_check_interval_)
  {
    ROS_WARN("No laser scan received (and thus no pose updates have been published) for %f seconds.  Verify that data is being published on the %s topic.",
             tf2::durationToSec(d),
             // ros::names::resolve(scan_topic_).c_str());
             scan_topic_.c_str());
  }
}

void
AmclNode::requestMap()
{
  std::lock_guard<std::recursive_mutex> ml(configuration_mutex_);

  // get map via RPC
  auto req = std::make_shared<nav_msgs::srv::GetMap::Request>();
  std::shared_ptr<nav_msgs::srv::GetMap::Response> resp;
  auto client = node->create_client<nav_msgs::srv::GetMap>("static_map");
  bool map_received = false;

  while (!client->wait_for_service(std::chrono::seconds(1))) {
    if (!rclcpp::ok()) {
      return;
    }
    ROS_INFO("Waiting for map service to appear...");
  }

  rclcpp::Rate r(std::chrono::milliseconds(500));
  while(!map_received)
  {
    auto result_future = client->async_send_request(req);
    if (rclcpp::spin_until_future_complete(node, result_future, std::chrono::seconds(1)) !=
      rclcpp::executor::FutureReturnCode::SUCCESS)
    {
      ROS_WARN("Request for map failed; trying again...");
      r.sleep();
    }
    else{
      resp = result_future.get();
      map_received = true;
    }
    if (!rclcpp::ok()) {
      return;
    }
  }
  handleMapMessage( resp->map );
}

void
AmclNode::mapReceived(const std::shared_ptr<nav_msgs::msg::OccupancyGrid> msg)
{
  if( first_map_only_ && first_map_received_ ) {
    return;
  }

  handleMapMessage( *msg );

  first_map_received_ = true;
}

void
AmclNode::handleMapMessage(const nav_msgs::msg::OccupancyGrid& msg)
{
  std::lock_guard<std::recursive_mutex> cfl(configuration_mutex_);

  ROS_INFO("Received a %d X %d map @ %.3f m/pix",
           msg.info.width,
           msg.info.height,
           msg.info.resolution);

  freeMapDependentMemory();
  // Clear queued laser objects because they hold pointers to the existing
  // map, #5202.
  lasers_.clear();
  lasers_update_.clear();
  frame_to_laser_.clear();

  map_ = convertMap(msg);

#if NEW_UNIFORM_SAMPLING
  // Index of free space
  free_space_indices.resize(0);
  for(int i = 0; i < map_->size_x; i++)
    for(int j = 0; j < map_->size_y; j++)
      if(map_->cells[MAP_INDEX(map_,i,j)].occ_state == -1)
        free_space_indices.push_back(std::make_pair(i,j));
#endif
  // Create the particle filter
  pf_ = pf_alloc(min_particles_, max_particles_,
                 alpha_slow_, alpha_fast_,
                 (pf_init_model_fn_t)AmclNode::uniformPoseGenerator,
                 (void *)map_);
  pf_->pop_err = pf_err_;
  pf_->pop_z = pf_z_;

  // Initialize the filter
  updatePoseFromServer();
  pf_vector_t pf_init_pose_mean = pf_vector_zero();
  pf_init_pose_mean.v[0] = init_pose_[0];
  pf_init_pose_mean.v[1] = init_pose_[1];
  pf_init_pose_mean.v[2] = init_pose_[2];
  pf_matrix_t pf_init_pose_cov = pf_matrix_zero();
  pf_init_pose_cov.m[0][0] = init_cov_[0];
  pf_init_pose_cov.m[1][1] = init_cov_[1];
  pf_init_pose_cov.m[2][2] = init_cov_[2];
  pf_init(pf_, pf_init_pose_mean, pf_init_pose_cov);
  pf_init_ = false;

  // Instantiate the sensor objects
  // Odometry
  delete odom_;
  odom_ = new AMCLOdom();
  // ROS_ASSERT(odom_);
  odom_->SetModel( odom_model_type_, alpha1_, alpha2_, alpha3_, alpha4_, alpha5_ );
  // Laser
  delete laser_;
  laser_ = new AMCLLaser(max_beams_, map_);
  // ROS_ASSERT(laser_);
  if(laser_model_type_ == LASER_MODEL_BEAM)
    laser_->SetModelBeam(z_hit_, z_short_, z_max_, z_rand_,
                         sigma_hit_, lambda_short_, 0.0);
  else if(laser_model_type_ == LASER_MODEL_LIKELIHOOD_FIELD_PROB){
    ROS_INFO("Initializing likelihood field model; this can take some time on large maps...");
    laser_->SetModelLikelihoodFieldProb(z_hit_, z_rand_, sigma_hit_,
					laser_likelihood_max_dist_, 
					do_beamskip_, beam_skip_distance_, 
					beam_skip_threshold_, beam_skip_error_threshold_);
    ROS_INFO("Done initializing likelihood field model.");
  }
  else
  {
    ROS_INFO("Initializing likelihood field model; this can take some time on large maps...");
    laser_->SetModelLikelihoodField(z_hit_, z_rand_, sigma_hit_,
                                    laser_likelihood_max_dist_);
    ROS_INFO("Done initializing likelihood field model.");
  }

  // In case the initial pose message arrived before the first map,
  // try to apply the initial pose now that the map has arrived.
  applyInitialPose();

}

void
AmclNode::freeMapDependentMemory()
{
  if( map_ != NULL ) {
    map_free( map_ );
    map_ = NULL;
  }
  if( pf_ != NULL ) {
    pf_free( pf_ );
    pf_ = NULL;
  }
  delete odom_;
  odom_ = NULL;
  delete laser_;
  laser_ = NULL;
}

/**
 * Convert an OccupancyGrid map message into the internal
 * representation.  This allocates a map_t and returns it.
 */
map_t*
AmclNode::convertMap( const nav_msgs::msg::OccupancyGrid& map_msg )
{
  map_t* map = map_alloc();
  // ROS_ASSERT(map);

  map->size_x = map_msg.info.width;
  map->size_y = map_msg.info.height;
  map->scale = map_msg.info.resolution;
  map->origin_x = map_msg.info.origin.position.x + (map->size_x / 2) * map->scale;
  map->origin_y = map_msg.info.origin.position.y + (map->size_y / 2) * map->scale;
  // Convert to player format
  map->cells = (map_cell_t*)malloc(sizeof(map_cell_t)*map->size_x*map->size_y);
  // ROS_ASSERT(map->cells);
  for(int i=0;i<map->size_x * map->size_y;i++)
  {
    if(map_msg.data[i] == 0)
      map->cells[i].occ_state = -1;
    else if(map_msg.data[i] == 100)
      map->cells[i].occ_state = +1;
    else
      map->cells[i].occ_state = 0;
  }

  return map;
}

AmclNode::~AmclNode()
{
  // delete dsrv_;
  freeMapDependentMemory();
  // delete laser_scan_filter_;
  delete tfb_;
  delete tf2_buffer_;
  delete tfl_;
  // TODO: delete everything allocated in constructor
}

bool
AmclNode::getOdomPose(tf2::Stamped<tf2::Transform>& odom_pose,
                      double& x, double& y, double& yaw,
                      const builtin_interfaces::msg::Time& /*t*/, const std::string& f)
{
  // Get the robot's pose
  tf2::Stamped<tf2::Transform> ident (tf2::Transform(tf2::Quaternion::getIdentity(),
                                           tf2::Vector3(0,0,0)), tf2::TimePoint(), f);

  // wait a little for the latest tf to become available
  try{
    geometry_msgs::msg::TransformStamped transformStamped;
    transformStamped = this->tf2_buffer_->lookupTransform(odom_frame_id_, f, tf2::TimePoint());
  } catch (tf2::TransformException &e) {
    ROS_WARN("Failed to find odom transform, skipping scan (%s)", e.what());
    return false;
  }
  try
  {
    geometry_msgs::msg::TransformStamped odom_pose_msg;
    this->tf2_buffer_->transform(
        tf2::toMsg<tf2::Stamped<tf2::Transform>, geometry_msgs::msg::TransformStamped>(ident),
        odom_pose_msg, odom_frame_id_, tf2::durationFromSec(3.0));
    tf2::fromMsg(odom_pose_msg, odom_pose);
  }
  catch(tf2::TransformException & e)
  {
    ROS_WARN("Failed to compute odom pose, skipping scan (%s)", e.what());
    return false;
  }
  x = odom_pose.getOrigin().x();
  y = odom_pose.getOrigin().y();
  double pitch,roll;
  odom_pose.getBasis().getEulerYPR(yaw, pitch, roll);

  return true;
}


pf_vector_t
AmclNode::uniformPoseGenerator(void* arg)
{
  map_t* map = (map_t*)arg;
#if NEW_UNIFORM_SAMPLING
  unsigned int rand_index = drand48() * free_space_indices.size();
  std::pair<int,int> free_point = free_space_indices[rand_index];
  pf_vector_t p;
  p.v[0] = MAP_WXGX(map, free_point.first);
  p.v[1] = MAP_WYGY(map, free_point.second);
  p.v[2] = drand48() * 2 * M_PI - M_PI;
#else
  double min_x, max_x, min_y, max_y;

  min_x = (map->size_x * map->scale)/2.0 - map->origin_x;
  max_x = (map->size_x * map->scale)/2.0 + map->origin_x;
  min_y = (map->size_y * map->scale)/2.0 - map->origin_y;
  max_y = (map->size_y * map->scale)/2.0 + map->origin_y;

  pf_vector_t p;

  ROS_DEBUG("Generating new uniform sample");
  for(;;)
  {
    p.v[0] = min_x + drand48() * (max_x - min_x);
    p.v[1] = min_y + drand48() * (max_y - min_y);
    p.v[2] = drand48() * 2 * M_PI - M_PI;
    // Check that it's a free cell
    int i,j;
    i = MAP_GXWX(map, p.v[0]);
    j = MAP_GYWY(map, p.v[1]);
    if(MAP_VALID(map,i,j) && (map->cells[MAP_INDEX(map,i,j)].occ_state == -1))
      break;
  }
#endif
  return p;
}

void
AmclNode::globalLocalizationCallback(const std::shared_ptr<std_srvs::srv::Empty::Request> req,
                                     std::shared_ptr<std_srvs::srv::Empty::Response> res)
{
  (void)req;
  (void)res;
  if( map_ == NULL ) {
    return;
  }
  std::lock_guard<std::recursive_mutex> gl(configuration_mutex_);
  ROS_INFO("Initializing with uniform distribution");
  pf_init_model(pf_, (pf_init_model_fn_t)AmclNode::uniformPoseGenerator,
                (void *)map_);
  ROS_INFO("Global initialisation done!");
  pf_init_ = false;
}

// force nomotion updates (amcl updating without requiring motion)
void
AmclNode::nomotionUpdateCallback(const std::shared_ptr<std_srvs::srv::Empty::Request> req,
                                     std::shared_ptr<std_srvs::srv::Empty::Response> res)
{
  (void)req;
  (void)res;
	m_force_update = true;
	ROS_INFO("Requesting no-motion update");
}

void
AmclNode::setMapCallback(const std::shared_ptr<nav_msgs::srv::SetMap::Request> req,
                         std::shared_ptr<nav_msgs::srv::SetMap::Response> res)
{
  handleMapMessage(req->map);
  handleInitialPoseMessage(req->initial_pose);
  res->success = true;
}

void
AmclNode::laserReceived(const std::shared_ptr<sensor_msgs::msg::LaserScan> laser_scan)
{
  last_laser_received_ts_ = tf2_ros::fromMsg(rclcpp::Time::now());
  if( map_ == NULL ) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lr(configuration_mutex_);
  int laser_index = -1;

  // Do we have the base->base_laser Tx yet?
  std::string laser_scan_frame_id = laser_scan->header.frame_id;
  if(frame_to_laser_.find(laser_scan_frame_id) == frame_to_laser_.end())
  {
    ROS_DEBUG("Setting up laser %d (frame_id=%s)", (int)frame_to_laser_.size(), laser_scan_frame_id.c_str());
    lasers_.push_back(new AMCLLaser(*laser_));
    lasers_update_.push_back(true);
    laser_index = frame_to_laser_.size();

    tf2::Stamped<tf2::Transform> ident (tf2::Transform(tf2::Quaternion::getIdentity(),
                                             tf2::Vector3(0,0,0)),
                                 tf2::TimePoint(), laser_scan_frame_id);
    tf2::Stamped<tf2::Transform> laser_pose;
    try
    {
      geometry_msgs::msg::TransformStamped laser_pose_msg;
      this->tf2_buffer_->transform(tf2::toMsg<tf2::Stamped<tf2::Transform>, geometry_msgs::msg::TransformStamped>(ident), laser_pose_msg, base_frame_id_, tf2::durationFromSec(3.0));
      tf2::fromMsg(laser_pose_msg, laser_pose);
    }
    catch(tf2::TransformException& e)
    {
      ROS_ERROR("Couldn't transform from %s to %s, "
                "even though the message notifier is in use",
                laser_scan_frame_id.c_str(),
                base_frame_id_.c_str());
      return;
    }

    pf_vector_t laser_pose_v;
    laser_pose_v.v[0] = laser_pose.getOrigin().x();
    laser_pose_v.v[1] = laser_pose.getOrigin().y();
    // laser mounting angle gets computed later -> set to 0 here!
    laser_pose_v.v[2] = 0;
    lasers_[laser_index]->SetLaserPose(laser_pose_v);
    ROS_DEBUG("Received laser's pose wrt robot: %.3f %.3f %.3f",
              laser_pose_v.v[0],
              laser_pose_v.v[1],
              laser_pose_v.v[2]);

    frame_to_laser_[laser_scan_frame_id] = laser_index;
  } else {
    // we have the laser pose, retrieve laser index
    laser_index = frame_to_laser_[laser_scan_frame_id];
  }

  // Where was the robot when this scan was taken?
  pf_vector_t pose;
  if(!getOdomPose(latest_odom_pose_, pose.v[0], pose.v[1], pose.v[2],
                  laser_scan->header.stamp, base_frame_id_))
  {
    ROS_ERROR("Couldn't determine robot's pose associated with laser scan");
    return;
  }
 
  pf_vector_t delta = pf_vector_zero();

  if(pf_init_)
  {
    // Compute change in pose
    //delta = pf_vector_coord_sub(pose, pf_odom_pose_);
    delta.v[0] = pose.v[0] - pf_odom_pose_.v[0];
    delta.v[1] = pose.v[1] - pf_odom_pose_.v[1];
    delta.v[2] = angle_diff(pose.v[2], pf_odom_pose_.v[2]);

    // See if we should update the filter
    bool update = fabs(delta.v[0]) > d_thresh_ ||
                  fabs(delta.v[1]) > d_thresh_ ||
                  fabs(delta.v[2]) > a_thresh_;
    update = update || m_force_update;
    m_force_update=false;

    // Set the laser update flags
    if(update)
      for(unsigned int i=0; i < lasers_update_.size(); i++)
        lasers_update_[i] = true;
  }

  bool force_publication = false;
  if(!pf_init_)
  {
    // Pose at last filter update
    pf_odom_pose_ = pose;

    // Filter is now initialized
    pf_init_ = true;

    // Should update sensor data
    for(unsigned int i=0; i < lasers_update_.size(); i++)
      lasers_update_[i] = true;

    force_publication = true;

    resample_count_ = 0;
  }
  // If the robot has moved, update the filter
  else if(pf_init_ && lasers_update_[laser_index])
  {
    //printf("pose\n");
    //pf_vector_fprintf(pose, stdout, "%.3f");

    AMCLOdomData odata;
    odata.pose = pose;
    // HACK
    // Modify the delta in the action data so the filter gets
    // updated correctly
    odata.delta = delta;

    // Use the action data to update the filter
    odom_->UpdateAction(pf_, (AMCLSensorData*)&odata);

    // Pose at last filter update
    //this->pf_odom_pose = pose;
  }

  bool resampled = false;
  // If the robot has moved, update the filter
  if(lasers_update_[laser_index])
  {
    AMCLLaserData ldata;
    ldata.sensor = lasers_[laser_index];
    ldata.range_count = laser_scan->ranges.size();

    // To account for lasers that are mounted upside-down, we determine the
    // min, max, and increment angles of the laser in the base frame.
    //
    // Construct min and max angles of laser, in the base_link frame.
    tf2::Quaternion q;
    q.setRPY(0.0, 0.0, laser_scan->angle_min);
    tf2::Stamped<tf2::Quaternion> min_q(q, tf2::TimePoint(),
                                      laser_scan_frame_id);
    q.setRPY(0.0, 0.0, laser_scan->angle_min + laser_scan->angle_increment);
    tf2::Stamped<tf2::Quaternion> inc_q(q, tf2::TimePoint(),
                                      laser_scan_frame_id);
    try
    {
      geometry_msgs::msg::QuaternionStamped min_q_msg, inc_q_msg;
      tf2_buffer_->transform(tf2::toMsg<tf2::Stamped<tf2::Quaternion>, geometry_msgs::msg::QuaternionStamped>(min_q), min_q_msg, base_frame_id_, tf2::durationFromSec(3.0));
      tf2_buffer_->transform(tf2::toMsg<tf2::Stamped<tf2::Quaternion>, geometry_msgs::msg::QuaternionStamped>(inc_q), inc_q_msg, base_frame_id_, tf2::durationFromSec(3.0));
      tf2::fromMsg(min_q_msg, min_q);
      tf2::fromMsg(inc_q_msg, inc_q);
    }
    catch(tf2::TransformException& e)
    {
      ROS_WARN("Unable to transform min/max laser angles into base frame: %s",
               e.what());
      return;
    }

    double angle_min = tf2::getYaw(min_q);
    double angle_increment = tf2::getYaw(inc_q) - angle_min;

    // wrapping angle to [-pi .. pi]
    angle_increment = fmod(angle_increment + 5*M_PI, 2*M_PI) - M_PI;

    ROS_DEBUG("Laser %d angles in base frame: min: %.3f inc: %.3f", laser_index, angle_min, angle_increment);

    // Apply range min/max thresholds, if the user supplied them
    if(laser_max_range_ > 0.0)
      ldata.range_max = std::min(laser_scan->range_max, (float)laser_max_range_);
    else
      ldata.range_max = laser_scan->range_max;
    double range_min;
    if(laser_min_range_ > 0.0)
      range_min = std::max(laser_scan->range_min, (float)laser_min_range_);
    else
      range_min = laser_scan->range_min;
    // The AMCLLaserData destructor will free this memory
    ldata.ranges = new double[ldata.range_count][2];
    // ROS_ASSERT(ldata.ranges);
    for(int i=0;i<ldata.range_count;i++)
    {
      // amcl doesn't (yet) have a concept of min range.  So we'll map short
      // readings to max range.
      if(laser_scan->ranges[i] <= range_min)
        ldata.ranges[i][0] = ldata.range_max;
      else
        ldata.ranges[i][0] = laser_scan->ranges[i];
      // Compute bearing
      ldata.ranges[i][1] = angle_min +
              (i * angle_increment);
    }

    lasers_[laser_index]->UpdateSensor(pf_, (AMCLSensorData*)&ldata);

    lasers_update_[laser_index] = false;

    pf_odom_pose_ = pose;

    // Resample the particles
    if(!(++resample_count_ % resample_interval_))
    {
      pf_update_resample(pf_);
      resampled = true;
    }

    pf_sample_set_t* set = pf_->sets + pf_->current_set;
    ROS_DEBUG("Num samples: %d", set->sample_count);

    // Publish the resulting cloud
    // TODO: set maximum rate for publishing
    if (!m_force_update) {
      geometry_msgs::msg::PoseArray cloud_msg;
      cloud_msg.header.stamp = rclcpp::Time::now();
      cloud_msg.header.frame_id = global_frame_id_;
      cloud_msg.poses.resize(set->sample_count);
      for(int i=0;i<set->sample_count;i++)
      {
        tf2::Quaternion q;
        q.setRPY(0.0, 0.0, set->samples[i].pose.v[2]);
        tf2::toMsg(tf2::Transform(q,
                                 tf2::Vector3(set->samples[i].pose.v[0],
                                           set->samples[i].pose.v[1], 0)),
                        cloud_msg.poses[i]);
      }
      particlecloud_pub_->publish(cloud_msg);
    }
  }

  if(resampled || force_publication)
  {
    if (!resampled)
    {
	    // re-compute the cluster statistics
	    pf_cluster_stats(pf_, pf_->sets);
    }
    // Read out the current hypotheses
    double max_weight = 0.0;
    int max_weight_hyp = -1;
    std::vector<amcl_hyp_t> hyps;
    hyps.resize(pf_->sets[pf_->current_set].cluster_count);
    for(int hyp_count = 0;
        hyp_count < pf_->sets[pf_->current_set].cluster_count; hyp_count++)
    {
      double weight;
      pf_vector_t pose_mean;
      pf_matrix_t pose_cov;
      if (!pf_get_cluster_stats(pf_, hyp_count, &weight, &pose_mean, &pose_cov))
      {
        ROS_ERROR("Couldn't get stats on cluster %d", hyp_count);
        break;
      }

      hyps[hyp_count].weight = weight;
      hyps[hyp_count].pf_pose_mean = pose_mean;
      hyps[hyp_count].pf_pose_cov = pose_cov;

      if(hyps[hyp_count].weight > max_weight)
      {
        max_weight = hyps[hyp_count].weight;
        max_weight_hyp = hyp_count;
      }
    }

    if(max_weight > 0.0)
    {
      ROS_DEBUG("Max weight pose: %.3f %.3f %.3f",
                hyps[max_weight_hyp].pf_pose_mean.v[0],
                hyps[max_weight_hyp].pf_pose_mean.v[1],
                hyps[max_weight_hyp].pf_pose_mean.v[2]);

      /*
         puts("");
         pf_matrix_fprintf(hyps[max_weight_hyp].pf_pose_cov, stdout, "%6.3f");
         puts("");
       */

      geometry_msgs::msg::PoseWithCovarianceStamped p;
      // Fill in the header
      p.header.frame_id = global_frame_id_;
      p.header.stamp = laser_scan->header.stamp;
      // Copy in the pose
      p.pose.pose.position.x = hyps[max_weight_hyp].pf_pose_mean.v[0];
      p.pose.pose.position.y = hyps[max_weight_hyp].pf_pose_mean.v[1];
      tf2::Quaternion q;
      q.setRPY(0.0, 0.0, hyps[max_weight_hyp].pf_pose_mean.v[2]);
      p.pose.pose.orientation = tf2::toMsg(q);

      // Copy in the covariance, converting from 3-D to 6-D
      pf_sample_set_t* set = pf_->sets + pf_->current_set;
      for(int i=0; i<2; i++)
      {
        for(int j=0; j<2; j++)
        {
          // Report the overall filter covariance, rather than the
          // covariance for the highest-weight cluster
          //p.covariance[6*i+j] = hyps[max_weight_hyp].pf_pose_cov.m[i][j];
          p.pose.covariance[6*i+j] = set->cov.m[i][j];
        }
      }
      // Report the overall filter covariance, rather than the
      // covariance for the highest-weight cluster
      //p.covariance[6*5+5] = hyps[max_weight_hyp].pf_pose_cov.m[2][2];
      p.pose.covariance[6*5+5] = set->cov.m[2][2];

      /*
         printf("cov:\n");
         for(int i=0; i<6; i++)
         {
         for(int j=0; j<6; j++)
         printf("%6.3f ", p.covariance[6*i+j]);
         puts("");
         }
       */

      pose_pub_->publish(p);
      last_published_pose = p;

      ROS_DEBUG("New pose: %6.3f %6.3f %6.3f",
               hyps[max_weight_hyp].pf_pose_mean.v[0],
               hyps[max_weight_hyp].pf_pose_mean.v[1],
               hyps[max_weight_hyp].pf_pose_mean.v[2]);

      // subtracting base to odom from map to base and send map to odom instead
      tf2::Stamped<tf2::Transform> odom_to_map;
      try
      {
        tf2::Quaternion q;
        q.setRPY(0.0, 0.0, hyps[max_weight_hyp].pf_pose_mean.v[2]);
        tf2::Transform tmp_tf(q,
                             tf2::Vector3(hyps[max_weight_hyp].pf_pose_mean.v[0],
                                         hyps[max_weight_hyp].pf_pose_mean.v[1],
                                         0.0));
        tf2::Stamped<tf2::Transform> tmp_tf_stamped(tmp_tf.inverse(),
                                                    tf2::TimePoint(), // TODO(dhood): restore stamp: tf2_ros::fromMsg(laser_scan->header.stamp)
                                                    base_frame_id_);
        geometry_msgs::msg::TransformStamped odom_to_map_msg;

        this->tf2_buffer_->transform(tf2::toMsg<tf2::Stamped<tf2::Transform>, geometry_msgs::msg::TransformStamped>(tmp_tf_stamped),
                                 odom_to_map_msg,
                                 odom_frame_id_, tf2::durationFromSec(3.0));
        tf2::fromMsg(odom_to_map_msg, odom_to_map);
      }
      catch(tf2::TransformException &)
      {
        ROS_DEBUG("Failed to subtract base to odom transform");
        return;
      }

      latest_tf_ = tf2::Transform(tf2::Quaternion(odom_to_map.getRotation()),
                                 tf2::Vector3(odom_to_map.getOrigin()));
      latest_tf_valid_ = true;

      if (tf_broadcast_ == true)
      {
        // We want to send a transform that is good up until a
        // tolerance time so that odom can be used
        auto stamp = tf2_ros::fromMsg(laser_scan->header.stamp);
        tf2::TimePoint transform_expiration = stamp +
                                          transform_tolerance_;
        geometry_msgs::msg::TransformStamped tmp_tf_stamped;
        tmp_tf_stamped.header.frame_id = global_frame_id_;
        tmp_tf_stamped.child_frame_id = odom_frame_id_;
        tmp_tf_stamped.header.stamp = tf2_ros::toMsg(transform_expiration);
        tmp_tf_stamped.transform = tf2::toMsg(latest_tf_.inverse());
        this->tfb_->sendTransform(tmp_tf_stamped);
        sent_first_transform_ = true;
      }
    }
    else
    {
      ROS_ERROR("No pose!");
    }
  }
  else if(latest_tf_valid_)
  {
    if (tf_broadcast_ == true)
    {
      // Nothing changed, so we'll just republish the last transform, to keep
      // everybody happy.
      tf2::TimePoint transform_expiration = tf2_ros::fromMsg(laser_scan->header.stamp) +
                                            transform_tolerance_;
      geometry_msgs::msg::TransformStamped tmp_tf_stamped;
      tmp_tf_stamped.header.frame_id = global_frame_id_;
      tmp_tf_stamped.child_frame_id = odom_frame_id_;
      tmp_tf_stamped.header.stamp = tf2_ros::toMsg(transform_expiration);
      tmp_tf_stamped.transform = tf2::toMsg(latest_tf_.inverse());
      this->tfb_->sendTransform(tmp_tf_stamped);
    }

    // Is it time to save our last pose to the param server
    tf2::TimePoint now = tf2_ros::fromMsg(rclcpp::Time::now());
    if((tf2::durationToSec(save_pose_period) > 0.0) &&
       (now - save_pose_last_time) >= save_pose_period)
    {
      this->savePoseToServer();
      save_pose_last_time = now;
    }
  }

}

double
AmclNode::getYaw(tf2::Transform& t)
{
  double yaw, pitch, roll;
  t.getBasis().getEulerYPR(yaw,pitch,roll);
  return yaw;
}

void
AmclNode::initialPoseReceived(const std::shared_ptr<geometry_msgs::msg::PoseWithCovarianceStamped> msg)
{
  handleInitialPoseMessage(*msg);
}

void
AmclNode::handleInitialPoseMessage(const geometry_msgs::msg::PoseWithCovarianceStamped& msg)
{
  std::lock_guard<std::recursive_mutex> prl(configuration_mutex_);
  if(msg.header.frame_id == "")
  {
    // This should be removed at some point
    ROS_WARN("Received initial pose with empty frame_id.  You should always supply a frame_id.");
  }
  // We only accept initial pose estimates in the global frame, #5148.
  else if(msg.header.frame_id != global_frame_id_)
  {
    ROS_WARN("Ignoring initial pose in frame \"%s\"; initial poses must be in the global frame, \"%s\"",
             msg.header.frame_id.c_str(),
             global_frame_id_.c_str());
    return;
  }

  // In case the client sent us a pose estimate in the past, integrate the
  // intervening odometric change.
  tf2::Stamped <tf2::Transform> tx_odom;
  try
  {
    tf2::TimePoint now = tf2_ros::fromMsg(rclcpp::Time::now());
    // wait a little for the latest tf to become available

    if (!tf2_buffer_->canTransform(base_frame_id_, tf2_ros::fromMsg(msg.header.stamp), base_frame_id_, now, odom_frame_id_, tf2::durationFromSec(3.0)))
    {
      ROS_WARN("Failed to find odom transform, skipping scan");
    }
    geometry_msgs::msg::TransformStamped tx_odom_tmp = tf2_buffer_->lookupTransform(base_frame_id_, tf2_ros::fromMsg(msg.header.stamp), base_frame_id_, tf2::TimePoint(), odom_frame_id_);
    tf2::fromMsg(tx_odom_tmp, tx_odom);
  }
  catch(tf2::TransformException & e)
  {
    // If we've never sent a transform, then this is normal, because the
    // global_frame_id_ frame doesn't exist.  We only care about in-time
    // transformation for on-the-move pose-setting, so ignoring this
    // startup condition doesn't really cost us anything.
    if(sent_first_transform_)
      ROS_WARN("Failed to transform initial pose in time (%s)", e.what());
    tx_odom.setIdentity();
  }

  tf2::Transform pose_old, pose_new;
  tf2::fromMsg(msg.pose.pose, pose_old);
  pose_new = pose_old * tx_odom;

  // Transform into the global frame

  ROS_INFO("Setting pose (%.6f): %.3f %.3f %.3f",
           tf2::timeToSec(tf2::get_now()),
           pose_new.getOrigin().x(),
           pose_new.getOrigin().y(),
           getYaw(pose_new));
  // Re-initialize the filter
  pf_vector_t pf_init_pose_mean = pf_vector_zero();
  pf_init_pose_mean.v[0] = pose_new.getOrigin().x();
  pf_init_pose_mean.v[1] = pose_new.getOrigin().y();
  pf_init_pose_mean.v[2] = getYaw(pose_new);
  pf_matrix_t pf_init_pose_cov = pf_matrix_zero();
  // Copy in the covariance, converting from 6-D to 3-D
  for(int i=0; i<2; i++)
  {
    for(int j=0; j<2; j++)
    {
      pf_init_pose_cov.m[i][j] = msg.pose.covariance[6*i+j];
    }
  }
  pf_init_pose_cov.m[2][2] = msg.pose.covariance[6*5+5];

  delete initial_pose_hyp_;
  initial_pose_hyp_ = new amcl_hyp_t();
  initial_pose_hyp_->pf_pose_mean = pf_init_pose_mean;
  initial_pose_hyp_->pf_pose_cov = pf_init_pose_cov;
  applyInitialPose();
}

/**
 * If initial_pose_hyp_ and map_ are both non-null, apply the initial
 * pose to the particle filter state.  initial_pose_hyp_ is deleted
 * and set to NULL after it is used.
 */
void
AmclNode::applyInitialPose()
{
  std::lock_guard<std::recursive_mutex> cfl(configuration_mutex_);
  if( initial_pose_hyp_ != NULL && map_ != NULL ) {
    pf_init(pf_, initial_pose_hyp_->pf_pose_mean, initial_pose_hyp_->pf_pose_cov);
    pf_init_ = false;

    delete initial_pose_hyp_;
    initial_pose_hyp_ = NULL;
  }
}
