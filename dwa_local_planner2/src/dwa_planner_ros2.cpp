/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
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
*   * Neither the name of Willow Garage, Inc. nor the names of its
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
*
* Author: Eitan Marder-Eppstein
* Modified by: Jeeseon Kim
*********************************************************************/

#include <dwa_local_planner2/dwa_planner_ros2.h>
#include <Eigen/Core>
#include <cmath>

#include <ros/console.h>

#include <pluginlib/class_list_macros.h>

#include <base_local_planner/goal_functions.h>


#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>

//#!
#include <algorithm>
#include <nav_msgs/GetMap.h>
#define MAX_VAL 10000
#define MIN_VAL -10000
#define COLL_PROB_ALPHA 0.8
#define COLL_PROB_BETA  0.05
#define SIGMA   0.4
#define CORR    1/sqrt(2 * M_PI * SIGMA)
#define GAUSS_ALPHA  0.1
#define EPSILON 0.0001

bool no_obstacles_ = false;
//#!

//register this planner as a BaseLocalPlanner plugin
PLUGINLIB_EXPORT_CLASS(dwa_local_planner2::DWAPlannerROS2, nav_core::BaseLocalPlanner)

using namespace std;

namespace dwa_local_planner2 {

  void DWAPlannerROS2::reconfigureCB(DWAPlanner2Config &config, uint32_t level) {
      if (setup_ && config.restore_defaults) {
        config = default_config_;
        config.restore_defaults = false;
      }
      if ( ! setup_) {
        default_config_ = config;
        setup_ = true;
      }

      // update generic local planner params
      base_local_planner::LocalPlannerLimits limits;
      limits.max_trans_vel = config.max_trans_vel;
      limits.min_trans_vel = config.min_trans_vel;
      limits.max_vel_x = config.max_vel_x;
      limits.min_vel_x = config.min_vel_x;
      limits.max_vel_y = config.max_vel_y;
      limits.min_vel_y = config.min_vel_y;
      limits.max_rot_vel = config.max_rot_vel;
      limits.min_rot_vel = config.min_rot_vel;
      limits.acc_lim_x = config.acc_lim_x;
      limits.acc_lim_y = config.acc_lim_y;
      limits.acc_lim_theta = config.acc_lim_theta;
      limits.acc_limit_trans = config.acc_limit_trans;
      limits.xy_goal_tolerance = config.xy_goal_tolerance;
      limits.yaw_goal_tolerance = config.yaw_goal_tolerance;
      limits.prune_plan = config.prune_plan;
      limits.trans_stopped_vel = config.trans_stopped_vel;
      limits.rot_stopped_vel = config.rot_stopped_vel;
      planner_util_.reconfigureCB(limits, config.restore_defaults);

      // update dwa specific configuration
      dp_->reconfigure(config);
  }

  DWAPlannerROS2::DWAPlannerROS2() : initialized_(false),
      odom_helper_("odom"), setup_(false) {

  }

  void DWAPlannerROS2::mapProcess(const nav_msgs::OccupancyGrid& map){

      ROS_INFO_STREAM(map.header.frame_id);
      ROS_INFO("map initialize");

      map_inf map_info;
      map_info.size_x = map.info.width;
      map_info.size_y = map.info.height;
      map_info.scale = map.info.resolution;
      map_info.origin_x = map.info.origin.position.x + (map_info.size_x / 2) * map_info.scale;
      map_info.origin_y = map.info.origin.position.y + (map_info.size_y / 2) * map_info.scale;

      float w_x, w_y;

      map_position_.clear();

      for (std::size_t j=0; j < map_info.size_y; j++) {
          for (std::size_t i=0; i < map_info.size_x; i++) {
              if(map.data[MAP_INDEX(map_info, i, j)]==100){
                  // convert to world position
                  w_x = MAP_WXGX(map_info, i);
                  w_y = MAP_WYGY(map_info, j);
                  map_position_.push_back(std::make_pair(w_x, w_y));
              }
          }
      }
  }

  float obstacles_prev[10][2];
  float time_to_collide[10][2];
  int cnt_ = 1;

  void DWAPlannerROS2::computeTTC(){
    float obs_curr_x, obs_curr_y;     //obstacle position

    if(cnt_ % 2 != 0){

        findObstacles();

        if(no_obstacles_){
            for(int idx = 0; idx < rcv_msg_.ranges.size(); idx++){
                robot_safe_dir_.push_back(1.0);
            }
            dp_->setProbability(robot_safe_dir_);

            //clear
            curr_obs_.clear();
            obs_direction_.clear();
            obs_safe_prob_.clear();
            return;
        }

        float robot_vec[2] = {current_pose_.getOrigin().getX() - previous_pose_.getOrigin().getX(),
                               current_pose_.getOrigin().getY() - previous_pose_.getOrigin().getY()};   //robot vec
        float obs_vec[2] = {0, 0};
        float v_rel[2];

        for(int idx = 0; idx < curr_obs_.size(); idx++){
            obs_curr_x = curr_obs_[idx].first;
            obs_curr_y = curr_obs_[idx].second;

            float min_dist_ = MAX_VAL;
            int min_idx;

            //find previous obstacle j who has minimum distance with obstacle idx
            for(int j = 0; j < 10 ; j++){
                float dist_ = sqrt(powf(obs_curr_x - obstacles_prev[j][0], 2.0) + powf(obs_curr_y - obstacles_prev[j][1], 2.0));
                if(dist_ < min_dist_){
                    min_dist_ = dist_;
                    min_idx = j;
                }
            }
            obs_vec[0] = obs_curr_x - obstacles_prev[min_idx][0];
            obs_vec[1] = obs_curr_y - obstacles_prev[min_idx][1];   //obs vec

            v_rel[0] = robot_vec[0] - obs_vec[0];
            v_rel[1] = robot_vec[1] - obs_vec[1];

            float robot_vec_s  = sqrt(powf(robot_vec[0], 2.0) + powf(robot_vec[1], 2.0));
            float obs_vec_s = sqrt(powf(obs_vec[0], 2.0) + powf(obs_vec[1], 2.0));
            float f_dot = robot_vec[0] * obs_vec[0] + robot_vec[1] * obs_vec[1];    //inner product
            float cos_theta = f_dot / (robot_vec_s * obs_vec_s);    //cosine theta between 2 vec

            float d_rel_s = sqrt(powf(obs_curr_x - current_pose_.getOrigin().getX(), 2.0)
                               + powf(obs_curr_y - current_pose_.getOrigin().getY(), 2.0));
            float v_rel_s = sqrt(powf(v_rel[0], 2.0) + powf(v_rel[1], 2.0));

            float ttc = d_rel_s / (v_rel_s * cos_theta);

            float safety_prob = 1 - COLL_PROB_ALPHA * powf(M_E, -1 * (COLL_PROB_BETA * ttc) * (COLL_PROB_BETA * ttc) );

            obs_safe_prob_.push_back(safety_prob);
       }//end for

        //update previous state to current state
        previous_pose_ = current_pose_;

        for(int idx = 0; idx < 10; idx++){
            if(idx < curr_obs_.size()){
                obstacles_prev[idx][0] = curr_obs_[idx].first;
                obstacles_prev[idx][1] = curr_obs_[idx].second;
            }
            else{
                obstacles_prev[idx][0] = MAX_VAL;
                obstacles_prev[idx][1] = MAX_VAL;
            }
        }

        double gauss_prob;
        double min_prob = MAX_VAL;

        //compute safe probability for all directions
        for(int idx = 0; idx < rcv_msg_.ranges.size(); idx++){
            min_prob = MAX_VAL;
            for(int j = 0;  j < curr_obs_.size(); j++){
                gauss_prob = 1 - (CORR * powf(M_E, -1 * (GAUSS_ALPHA*(idx - obs_direction_[j])) * (GAUSS_ALPHA*(idx - obs_direction_[j])) / (2*SIGMA))
                         * (1/CORR)) * (1 - obs_safe_prob_[j]);
                if(min_prob > gauss_prob){
                    min_prob = gauss_prob;
                }
            }
            robot_safe_dir_.push_back(min_prob);
        }

        int min_prob_idx;
        int max_prob_idx;
        if( !((1 - robot_safe_dir_[0]) < EPSILON && (1 - robot_safe_dir_[rcv_msg_.ranges.size() - 1]) < EPSILON) ){ //obs in front of robot head direction

            if((1 - robot_safe_dir_[0]) >= EPSILON){ // 0 ~
                min_prob_idx = 0;   max_prob_idx = 0;

                for(int i = 0 ; i < rcv_msg_.ranges.size(); i++){   //find index with minimum prob value, last index which robot_safe_dir_[index] < 1
                    if(robot_safe_dir_[i] < robot_safe_dir_[min_prob_idx]){
                        min_prob_idx = i;
                    }
                    if((1 - robot_safe_dir_[i]) < EPSILON){
                        max_prob_idx = i;
                        break;
                    }
                }

                for(int i = min_prob_idx + 1; i <= max_prob_idx; i++){  //update probabilities for the opposite side
                    if(min_prob_idx - (i - min_prob_idx) < 0){
                        int offset = min_prob_idx - (i - min_prob_idx);
                        robot_safe_dir_[rcv_msg_.ranges.size() + offset] = robot_safe_dir_[i];
                    }
                }
            }

            else if((1 - robot_safe_dir_[rcv_msg_.ranges.size() - 1]) >= EPSILON){   // ~ 359
                int max_i = rcv_msg_.ranges.size() - 1;
                min_prob_idx = max_i;   max_prob_idx = max_i;

                for(int i = max_i ; i >= 0; i--){   //find index with minimum prob value, last index which robot_safe_dir_[index] < 1
                    if(robot_safe_dir_[i] < robot_safe_dir_[min_prob_idx]){
                        min_prob_idx = i;
                    }
                    if((1 - robot_safe_dir_[i]) < EPSILON){
                        max_prob_idx = i;
                        break;
                    }
                }

                for(int i = min_prob_idx - 1; i >= max_prob_idx; i--){  //update probabilities for the opposite side
                    if(min_prob_idx - (i - min_prob_idx) > max_i){
                        int offset = min_prob_idx - (i - min_prob_idx);
                        robot_safe_dir_[offset - rcv_msg_.ranges.size()] = robot_safe_dir_[i];
                    }
                }
            }
        }

        //send robot_safe_dir_ to base_local_planner::ProbabilityCostFunction
        dp_->setProbability(robot_safe_dir_);

        //clear
        curr_obs_.clear();
        obs_direction_.clear();
        obs_safe_prob_.clear();
    }
    cnt_++;
  }

  void DWAPlannerROS2::findObstacles(){

      float pt_x, pt_y;
      float rb_yaw; //robot yaw
      float w_x = 0, w_y = 0, dist_sq = 0;

      int obs_count = 0;
      float min_dist;
      bool dynamic;

      int *p1, *p2, *p3;
      int *grp_start_end;
      float *center_pos;

      rb_yaw = tf::getYaw(current_pose_.getRotation());

      if(rb_yaw < 0){
          rb_yaw += 2 * M_PI;
      }

      //find dynamic points, add dynamic points to obs_idx_
      for(int i = 0; i < rcv_msg_.ranges.size() ; i++){
          if(rcv_msg_.ranges[i] < rcv_msg_.range_max){
              pt_x = current_pose_.getOrigin().getX()
                      + rcv_msg_.ranges[i] * std::cos(rcv_msg_.angle_increment * i + rb_yaw);
              pt_y = current_pose_.getOrigin().getY()
                      + rcv_msg_.ranges[i] * std::sin(rcv_msg_.angle_increment * i + rb_yaw);    //sensed position

              int j;
              for(j = 0; j < map_position_.size(); j++){
                  w_x = map_position_[j].first;     //world map position
                  w_y = map_position_[j].second;
                  dist_sq = sqrt(powf(w_x - pt_x, 2.0) + powf(w_y - pt_y, 2.0));

                  if(dist_sq <= 0.20){ //near map
                      break;
                  }
              }
              if(j == map_position_.size()){
                  dynamic = true;
              }
              if(dynamic){
                  obs_idx_.push_back(i);
              }
          }
          dynamic = false;
      }

      no_obstacles_ = false;
      if(obs_idx_.size() < 1){
          no_obstacles_ = true;
          obs_idx_.clear();
          return;
      }

      int former_idx = obs_idx_.at(0);

      //count obstacles
      for (std::vector<int>::iterator itr = obs_idx_.begin(); itr != obs_idx_.end(); ++itr) {
          if(*itr - former_idx != 1){
              obs_count++;
          }
          former_idx = *itr;
      }

      //allocate index array for each obstacle
      p1 = new int[obs_count];    //start
      p2 = new int[obs_count];    //min
      p3 = new int[obs_count];    //end
      grp_start_end = new int[obs_count * 2];

      //fill grp_start_end[]: [i*2]: start idx for i-th obs / [i*2+1]: end idx for i-th obs
      int sep_idx_ = 0;
      grp_start_end[sep_idx_] = obs_idx_.front();
      grp_start_end[obs_count * 2 - 1] = obs_idx_.back();

      former_idx = obs_idx_.at(0);

      for (std::vector<int>::iterator itr = obs_idx_.begin() + 1; itr != obs_idx_.end(); ++itr) {
          if(*itr - former_idx != 1){ //new obstacle
              sep_idx_++;
              grp_start_end[sep_idx_] = *(--itr);
              sep_idx_++;
              grp_start_end[sep_idx_] = *(++itr);
          }
          former_idx = *itr;
      }

      //find lsr index for each obstacle
      for (int i = 0; i < obs_count; i++) {
          p1[i] = grp_start_end[i*2];     //start index
          p3[i] = grp_start_end[i*2+1];   //end index
          min_dist = rcv_msg_.range_max;

          for (int j = p1[i]; j <= p3[i]; j++) {
              if(rcv_msg_.ranges[j] < min_dist){   //find index with minimum distance
                  min_dist = rcv_msg_.ranges[j];
                  p2[i] = j;
              }
          }
      }

      //Obstacle exists at robot head direction (remove in case for duplicate count)
      if(obs_idx_.front() == 0 && obs_idx_.back() == rcv_msg_.ranges.size() - 1){
          if(rcv_msg_.ranges[p2[0]] >= rcv_msg_.ranges[p2[obs_count - 1]]){
             p2[0] = p2[obs_count - 1];
          }
          p1[0] = p1[obs_count - 1];
          p1[obs_count - 1] = 0;
          p2[obs_count - 1] = 0;
          p3[obs_count - 1] = 0;
          obs_count--;
      }

      center_pos = new float[obs_count * 2];

      //form triangle with p1, p2, p3 and assume center position of obstacles
      for (int i = 0; i < obs_count; i++) {

          float tri_a_x, tri_a_y, tri_c_x, tri_c_y, tri_b_x, tri_b_y;
          float a_vec_x, a_vec_y;
          float b_vec_x, b_vec_y;
          float a_vec_sq, b_vec_sq, a_b_in;
          float p, q;

          tri_a_x = rcv_msg_.ranges[p1[i]] * std::cos(rcv_msg_.angle_increment * p1[i]);    //A
          tri_a_y = rcv_msg_.ranges[p1[i]] * std::sin(rcv_msg_.angle_increment * p1[i]);
          tri_c_x = rcv_msg_.ranges[p2[i]] * std::cos(rcv_msg_.angle_increment * p2[i]);    //C
          tri_c_y = rcv_msg_.ranges[p2[i]] * std::sin(rcv_msg_.angle_increment * p2[i]);
          tri_b_x = rcv_msg_.ranges[p3[i]] * std::cos(rcv_msg_.angle_increment * p3[i]);    //B
          tri_b_y = rcv_msg_.ranges[p3[i]] * std::sin(rcv_msg_.angle_increment * p3[i]);

          a_vec_x = tri_a_x - tri_c_x; //CA == OA - OC
          a_vec_y = tri_a_y - tri_c_y;
          b_vec_x = tri_b_x - tri_c_x; //CB == OB - OC
          b_vec_y = tri_b_y - tri_c_y;

          a_vec_sq = a_vec_x * a_vec_x + a_vec_y * a_vec_y;   //  |CA|^2
          b_vec_sq = b_vec_x * b_vec_x + b_vec_y * b_vec_y;   //  |CB|^2
          a_b_in = a_vec_x * b_vec_x + a_vec_y * b_vec_y;     //  CA dot CB

          p = 1 / (a_vec_sq * a_b_in - b_vec_sq * a_b_in) * (a_b_in * 0.5 * a_vec_sq - a_b_in * 0.5 * b_vec_sq); //inverse matrix
          q = 1 / (a_vec_sq * a_b_in - b_vec_sq * a_b_in) * (-1*b_vec_sq * 0.5 * a_vec_sq + a_vec_sq * 0.5 * b_vec_sq);

          center_pos[2*i] = tri_c_x + p * a_vec_x + q * b_vec_x;
          center_pos[2*i + 1] = tri_c_y + p * a_vec_y + q * b_vec_y;

          if(std::isnan(center_pos[2 * i + 1]) || std::isnan(center_pos[2 * i])){   //remove sensed obstacle if assumed position is nan value
                //obs_count_mod--;
          }
          else{
              curr_obs_.push_back(make_pair(current_pose_.getOrigin().getX() + center_pos[2*i], current_pose_.getOrigin().getY() + center_pos[2*i + 1]));    //obs position
              obs_direction_.push_back(p2[i]);   //direction of min
          }
      }

      delete [] p1;
      delete [] p2;
      delete [] p3;
      delete [] grp_start_end;
      delete [] center_pos;

      p1 = NULL;
      p2 = NULL;
      p3 = NULL;
      grp_start_end = NULL;
      center_pos = NULL;

      obs_idx_.clear();
  }

  void DWAPlannerROS2::scanCallBack(const sensor_msgs::LaserScan::ConstPtr& msg){
      //laser data to rcv_msg_
      rcv_msg_.header = msg->header;
      rcv_msg_.angle_min = msg->angle_min;
      rcv_msg_.angle_max = msg->angle_max;
      rcv_msg_.angle_increment = msg->angle_increment;
      rcv_msg_.time_increment = msg->time_increment;
      rcv_msg_.scan_time = msg->scan_time;
      rcv_msg_.range_min = msg->range_min;
      rcv_msg_.range_max = msg->range_max;
      rcv_msg_.ranges = msg->ranges;
      rcv_msg_.intensities = msg->intensities;
  }

  void DWAPlannerROS2::initialize(
      std::string name,
      tf::TransformListener* tf,
      costmap_2d::Costmap2DROS* costmap_ros) {

    if (! isInitialized()) {

      ros::NodeHandle private_nh("~/" + name);
      g_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
      l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);
      tf_ = tf;
      costmap_ros_ = costmap_ros;
      costmap_ros_->getRobotPose(current_pose_);


      // make sure to update the costmap we'll use for this cycle
      costmap_2d::Costmap2D* costmap = costmap_ros_->getCostmap();

      planner_util_.initialize(tf, costmap, costmap_ros_->getGlobalFrameID());

      //create the actual planner that we'll use.. it'll configure itself from the parameter server
      dp_ = boost::shared_ptr<DWAPlanner2>(new DWAPlanner2(name, &planner_util_));

      if( private_nh.getParam( "odom_topic", odom_topic_ ))
      {
        odom_helper_.setOdomTopic( odom_topic_ );
      }
      
      //latchedStopRotateController_.initialize(name);

      initialized_ = true;

      dsrv_ = new dynamic_reconfigure::Server<DWAPlanner2Config>(private_nh);
      dynamic_reconfigure::Server<DWAPlanner2Config>::CallbackType cb = boost::bind(&DWAPlannerROS2::reconfigureCB, this, _1, _2);
      dsrv_->setCallback(cb);


      //#!
      scan_sub = private_nh.subscribe<sensor_msgs::LaserScan>("/scan", 1, &DWAPlannerROS2::scanCallBack, this);


      nav_msgs::GetMap::Request  req;
      nav_msgs::GetMap::Response resp;
      ROS_INFO("Requesting the map..");

      while(!ros::service::call("static_map", req, resp))
      {
        ROS_WARN("Request for map failed; trying again...");
      }
      current_map_ = resp.map;
      mapProcess(current_map_);
      //#!

    }
    else{
      ROS_WARN("This planner has already been initialized, doing nothing.");
    }
  }

  bool DWAPlannerROS2::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan) {
    if (! isInitialized()) {
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }
    //when we get a new plan, we also want to clear any latch we may have on goal tolerances
    latchedStopRotateController_.resetLatching();

    ROS_INFO("Got new plan");
    return dp_->setPlan(orig_global_plan);
  }

  bool DWAPlannerROS2::isGoalReached() {
    if (! isInitialized()) {
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }
    if ( ! costmap_ros_->getRobotPose(current_pose_)) {
      ROS_ERROR("Could not get robot pose");
      return false;
    }

    if(latchedStopRotateController_.isGoalReached(&planner_util_, odom_helper_, current_pose_)) {
      ROS_INFO("Goal reached");
      return true;
    } else {
      return false;
    }
  }

  void DWAPlannerROS2::publishLocalPlan(std::vector<geometry_msgs::PoseStamped>& path) {
    base_local_planner::publishPlan(path, l_plan_pub_);
  }


  void DWAPlannerROS2::publishGlobalPlan(std::vector<geometry_msgs::PoseStamped>& path) {
    base_local_planner::publishPlan(path, g_plan_pub_);
  }

  DWAPlannerROS2::~DWAPlannerROS2(){
    //make sure to clean things up
    delete dsrv_;
  }


  bool DWAPlannerROS2::dwaComputeVelocityCommands(tf::Stamped<tf::Pose> &global_pose, geometry_msgs::Twist& cmd_vel) {

    // dynamic window sampling approach to get useful velocity commands
    if(! isInitialized()){
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }

    tf::Stamped<tf::Pose> robot_vel;
    odom_helper_.getRobotVel(robot_vel);

    /* For timing uncomment
    struct timeval start, end;
    double start_t, end_t, t_diff;
    gettimeofday(&start, NULL);
    */

    //compute what trajectory to drive along
    tf::Stamped<tf::Pose> drive_cmds;
    drive_cmds.frame_id_ = costmap_ros_->getBaseFrameID();

    //


    // call with updated footprint
    base_local_planner::Trajectory path = dp_->findBestPath(global_pose, robot_vel, drive_cmds);
    //ROS_ERROR("Best: %.2f, %.2f, %.2f, %.2f", path.xv_, path.yv_, path.thetav_, path.cost_);

    /* For timing uncomment
    gettimeofday(&end, NULL);
    start_t = start.tv_sec + double(start.tv_usec) / 1e6;
    end_t = end.tv_sec + double(end.tv_usec) / 1e6;
    t_diff = end_t - start_t;
    ROS_INFO("Cycle time: %.9f", t_diff);
    */

    //pass along drive commands
    cmd_vel.linear.x = drive_cmds.getOrigin().getX();
    cmd_vel.linear.y = drive_cmds.getOrigin().getY();
    cmd_vel.angular.z = tf::getYaw(drive_cmds.getRotation());

    //if we cannot move... tell someone
    std::vector<geometry_msgs::PoseStamped> local_plan;
    if(path.cost_ < 0) {
      ROS_DEBUG_NAMED("dwa_local_planner2",
          "The dwa local planner2 failed to find a valid plan, cost functions discarded all candidates. This can mean there is an obstacle too close to the robot.");
      local_plan.clear();
      publishLocalPlan(local_plan);
      return false;
    }

    ROS_DEBUG_NAMED("dwa_local_planner2", "A valid velocity command of (%.2f, %.2f, %.2f) was found for this cycle.",
                    cmd_vel.linear.x, cmd_vel.linear.y, cmd_vel.angular.z);

    // Fill out the local plan
    for(unsigned int i = 0; i < path.getPointsSize(); ++i) {
      double p_x, p_y, p_th;
      path.getPoint(i, p_x, p_y, p_th);

      tf::Stamped<tf::Pose> p =
              tf::Stamped<tf::Pose>(tf::Pose(
                      tf::createQuaternionFromYaw(p_th),
                      tf::Point(p_x, p_y, 0.0)),
                      ros::Time::now(),
                      costmap_ros_->getGlobalFrameID());
      geometry_msgs::PoseStamped pose;
      tf::poseStampedTFToMsg(p, pose);
      local_plan.push_back(pose);
    }

    //publish information to the visualizer

    publishLocalPlan(local_plan);
    return true;
  }


  bool DWAPlannerROS2::computeVelocityCommands(geometry_msgs::Twist& cmd_vel) {
    // dispatches to either dwa sampling control or stop and rotate control, depending on whether we have been close enough to goal


    if ( ! costmap_ros_->getRobotPose(current_pose_)) {
      ROS_ERROR("Could not get robot pose");
      return false;
    }
    std::vector<geometry_msgs::PoseStamped> transformed_plan;

    //#!
    robot_safe_dir_.clear();
    computeTTC();
    //#!

    if ( ! planner_util_.getLocalPlan(current_pose_, transformed_plan)) {
      ROS_ERROR("Could not get local plan");
      return false;
    }

    //if the global plan passed in is empty... we won't do anything
    if(transformed_plan.empty()) {
      ROS_WARN_NAMED("dwa_local_planner2", "Received an empty transformed plan.");
      return false;
    }
    ROS_DEBUG_NAMED("dwa_local_planner2", "Received a transformed plan with %zu points.", transformed_plan.size());

    // update plan in dwa_planner even if we just stop and rotate, to allow checkTrajectory
    dp_->updatePlanAndLocalCosts(current_pose_, transformed_plan, costmap_ros_->getRobotFootprint());

    if (latchedStopRotateController_.isPositionReached(&planner_util_, current_pose_)) {
      //publish an empty plan because we've reached our goal position
      std::vector<geometry_msgs::PoseStamped> local_plan;
      std::vector<geometry_msgs::PoseStamped> transformed_plan;
      publishGlobalPlan(transformed_plan);
      publishLocalPlan(local_plan);
      base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();
      return latchedStopRotateController_.computeVelocityCommandsStopRotate(
          cmd_vel,
          limits.getAccLimits(),
          dp_->getSimPeriod(),
          &planner_util_,
          odom_helper_,
          current_pose_,
          boost::bind(&DWAPlanner2::checkTrajectory, dp_, _1, _2, _3));
    } else {
      bool isOk = dwaComputeVelocityCommands(current_pose_, cmd_vel);
      if (isOk) {
        publishGlobalPlan(transformed_plan);
      } else {
        ROS_WARN_NAMED("dwa_local_planner2", "DWA planner failed to produce path.");
        std::vector<geometry_msgs::PoseStamped> empty_plan;
        publishGlobalPlan(empty_plan);
      }
      return isOk;
    }
  }



};
