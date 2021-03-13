/*
 * IRIS Localization and Mapping (LaMa) for ROS
 *
 * Copyright (c) 2019-today, Eurico Pedrosa, University of Aveiro - Portugal
 * All rights reserved.
 * License: New BSD
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of Aveiro nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <nav_msgs/GetMap.h>

#include "lama/ros/loc2d_ros.h"
#include "lama/time.h"

lama::Loc2DROS::Loc2DROS()
    : nh_(), pnh_("~")
{
    // Load parameters from the server.
    double tmp;

    pnh_.param("global_frame_id", global_frame_id_, std::string("map"));
    pnh_.param("odom_frame_id",   odom_frame_id_,   std::string("odom"));
    pnh_.param("base_frame_id",   base_frame_id_,   std::string("base_link"));

    pnh_.param("scan_topic", scan_topic_, std::string("/scan"));

    pnh_.param("transform_tolerance", tmp, 0.1); transform_tolerance_.fromSec(tmp);

    pnh_.param("use_map_topic", use_map_topic_, false);
    pnh_.param("first_map_only", first_map_only_, false);
    pnh_.param("use_pose_on_new_map", use_pose_on_new_map_, false);

    pnh_.param("force_update_on_initial_pose", force_update_on_initial_pose_, false);

    // Setup TF workers ...
    tf_ = new tf::TransformListener();
    tfb_= new tf::TransformBroadcaster();

    // Setup subscribers
    // Syncronized LaserScan messages with odometry transforms. This ensures that an odometry transformation
    // exists when the handler of a LaserScan message is called.
    laser_scan_sub_    = new message_filters::Subscriber<sensor_msgs::LaserScan>(nh_, scan_topic_, 100, ros::TransportHints().tcpNoDelay());
    laser_scan_filter_ = new tf::MessageFilter<sensor_msgs::LaserScan>(*laser_scan_sub_, *tf_, odom_frame_id_, 100);
    laser_scan_filter_->registerCallback(boost::bind(&Loc2DROS::onLaserScan, this, _1));

    pose_sub_ = nh_.subscribe("/initialpose", 1, &Loc2DROS::onInitialPose, this);

    // Set publishers
    pose_pub_ = nh_.advertise<geometry_msgs::PoseWithCovarianceStamped>("/pose", 2, true);

    // Services
    srv_update_ = nh_.advertiseService("/request_nomotion_update", &Loc2DROS::onTriggerUpdate, this);
    srv_global_loc_ = nh_.advertiseService("global_localization", &Loc2DROS::globalLocalizationCallback, this);

    // Fetch algorithm options
    Vector2d pos; double init_a;
    pnh_.param("initial_pos_x", pos[0], 0.0);
    pnh_.param("initial_pos_y", pos[1], 0.0);
    pnh_.param("initial_pos_a", init_a, 0.0);
    initial_prior_ = lama::Pose2D(pos, init_a);

    cur_pose_msg_.pose.pose.position.x = pos[0];
    cur_pose_msg_.pose.pose.position.y = pos[1];
    cur_pose_msg_.pose.pose.orientation.z = tf::createQuaternionFromYaw(init_a).getZ();
    cur_pose_msg_.pose.pose.orientation.w = tf::createQuaternionFromYaw(init_a).getW();
    pose_pub_.publish(cur_pose_msg_);

    pnh_.param("d_thresh", options_.trans_thresh, 0.1);
    pnh_.param("a_thresh", options_.rot_thresh, 0.2);
    pnh_.param("l2_max",   options_.l2_max, 0.5);
    pnh_.param("strategy", options_.strategy, std::string("gn"));

    int dummy;
    pnh_.param("gloc_particles", dummy, 3000);
    options_.gloc_particles = dummy;

    pnh_.param("gloc_iters", dummy, 20);
    options_.gloc_iters= dummy;

    pnh_.param("gloc_thresh", options_.gloc_thresh, 0.15);

    // Request the map if not using the map topic
    if (not use_map_topic_)
    {
        ROS_INFO("Requesting the map...");
        nav_msgs::GetMap::Request  req;
        nav_msgs::GetMap::Response resp;
        while(ros::ok() and not ros::service::call("static_map", req, resp)){
            ROS_WARN_THROTTLE(1, "Request for map failed; trying again ...");
            ros::Duration d(0.5);
            d.sleep();
        }// end while

        int itmp;
        pnh_.param("patch_size", itmp, 32);
        options_.patch_size = itmp;

        InitLoc2DFromOccupancyGridMsg(initial_prior_, resp.map);
    }
    else
    {
        map_sub_ = nh_.subscribe("/map", 10, &Loc2DROS::onMapReceived, this, ros::TransportHints().tcpNoDelay());
    }

    // Should trigger an initial global localization?
    bool do_global_loc;
    pnh_.param("do_global_loc", do_global_loc, false);
    if (do_global_loc){
        ROS_INFO("Trigger Global Localization");
        loc2d_.triggerGlobalLocalization();
    }

    ROS_INFO("2D Localization node up and running");
}

lama::Loc2DROS::~Loc2DROS()
{
    delete laser_scan_filter_;
    delete laser_scan_sub_;

    delete tf_;
    delete tfb_;
}

void lama::Loc2DROS::publishCurrentPose(bool init)
{
    if (init)
    {
        current_pose_ = loc2d_.getPose();
        current_orientation_ = tf::createQuaternionFromYaw(current_pose_.rotation());
    }
    cur_pose_msg_.pose.pose.position.x = current_pose_.x();
    cur_pose_msg_.pose.pose.position.y = current_pose_.y();
    cur_pose_msg_.pose.pose.orientation.z = current_orientation_.getZ();
    cur_pose_msg_.pose.pose.orientation.w = current_orientation_.getW();
    pose_pub_.publish(cur_pose_msg_);
}

void lama::Loc2DROS::onInitialPose(const geometry_msgs::PoseWithCovarianceStampedConstPtr& initial_pose)
{
    float x = initial_pose->pose.pose.position.x;
    float y = initial_pose->pose.pose.position.y;
    float yaw = tf::getYaw(initial_pose->pose.pose.orientation);

    onInitialPose(lama::Pose2D(x, y, yaw));
}

void lama::Loc2DROS::onInitialPose(const Pose2D& prior)
{
    ROS_INFO("Setting pose to (%f, %f, %f)", prior.x(), prior.y() ,prior.rotation());
    loc2d_.setPose(prior);
    force_update_ = force_update_on_initial_pose_;
    publishCurrentPose(true);
}

void lama::Loc2DROS::onLaserScan(const sensor_msgs::LaserScanConstPtr& laser_scan)
{
    if (not first_map_received_)
        return;

    int laser_index = -1;

    // verify if it is from a known source
    if ( frame_to_laser_.find( laser_scan->header.frame_id ) == frame_to_laser_.end() ){
        if (not initLaser(laser_scan))
            return;

        laser_index = frame_to_laser_[laser_scan->header.frame_id];
    }else{
        laser_index = frame_to_laser_[laser_scan->header.frame_id];
    }

    // Where was the robot at the time of the scan ?
    tf::Stamped<tf::Pose> identity(tf::Transform(tf::createIdentityQuaternion(), tf::Vector3(0,0,0)),
                                   laser_scan->header.stamp, base_frame_id_);
    tf::Stamped<tf::Pose> odom_tf;
    try{ tf_->transformPose(odom_frame_id_, identity, odom_tf); }
    catch(tf::TransformException& e)
    { ROS_WARN("Failed to compute odom pose, skipping scan %s", e.what() ); return; }

    lama::Pose2D odom(odom_tf.getOrigin().x(), odom_tf.getOrigin().y(),
                              tf::getYaw(odom_tf.getRotation()));

    bool update = force_update_ or loc2d_.enoughMotion(odom);

    if (update){
        size_t size = laser_scan->ranges.size();
        size_t beam_step = 1;

        float max_range = laser_scan->range_max;
        float min_range = laser_scan->range_min;
        float angle_min = laser_scan->angle_min;
        float angle_inc = laser_scan->angle_increment;

        PointCloudXYZ::Ptr cloud(new PointCloudXYZ);

        cloud->sensor_origin_ = lasers_origin_[laser_index].xyz();
        cloud->sensor_orientation_ = Quaterniond(lasers_origin_[laser_index].state.so3().matrix());

        cloud->points.reserve(laser_scan->ranges.size());
        for(size_t i = 0; i < size; i += beam_step ){
            double range;

            range = laser_scan->ranges[i];

            if (not std::isfinite(range))
                continue;

            if (range >= max_range || range <= min_range)
                continue;


            Eigen::Vector3d point;
            point << range * std::cos(angle_min+(i*angle_inc)),
                     range * std::sin(angle_min+(i*angle_inc)),
                     0;

            cloud->points.push_back( point );
        }

        loc2d_.update(cloud, odom, laser_scan->header.stamp.toSec(), force_update_);
        force_update_ = false;

        // Report global localization if enables
        if (loc2d_.globalLocalizationIsActive()){
            ROS_INFO("Global Localization RMSE: %f", loc2d_.getRMSE());
        }

        current_pose_ = loc2d_.getPose();
        current_orientation_ = tf::createQuaternionFromYaw(current_pose_.rotation());
        // subtracting base to odom from map to base and send map to odom instead
        tf::Stamped<tf::Pose> odom_to_map;
        try{
            tf::Transform tmp_tf(current_orientation_, tf::Vector3(current_pose_.x(), current_pose_.y(), 0));
            tf::Stamped<tf::Pose> tmp_tf_stamped (tmp_tf.inverse(), laser_scan->header.stamp, base_frame_id_);
            tf_->transformPose(odom_frame_id_, tmp_tf_stamped, odom_to_map);

        }catch(tf::TransformException){
            ROS_WARN("Failed to subtract base to odom transform");
            return;
        }

        latest_tf_ = tf::Transform(tf::Quaternion(odom_to_map.getRotation()),
                                   tf::Point(odom_to_map.getOrigin()));

        // We want to send a transform that is good up until a
        // tolerance time so that odom can be used
        ros::Time transform_expiration = (laser_scan->header.stamp + transform_tolerance_);
        tf::StampedTransform tmp_tf_stamped(latest_tf_.inverse(),
                                            transform_expiration,
                                            global_frame_id_, odom_frame_id_);
        tfb_->sendTransform(tmp_tf_stamped);
    } else {
        // Nothing has change, therefore, republish the last transform.
        ros::Time transform_expiration = (laser_scan->header.stamp + transform_tolerance_);
        tf::StampedTransform tmp_tf_stamped(latest_tf_.inverse(), transform_expiration,
                                            global_frame_id_, odom_frame_id_);
        tfb_->sendTransform(tmp_tf_stamped);
    } // end if (update)
    publishCurrentPose();
}

void lama::Loc2DROS::onMapReceived(const nav_msgs::OccupancyGridConstPtr& msg)
{
    if (first_map_only_ and first_map_received_)
        return;
    InitLoc2DFromOccupancyGridMsg(use_pose_on_new_map_ ? loc2d_.getPose() : initial_prior_, *msg);
}

void lama::Loc2DROS::InitLoc2DFromOccupancyGridMsg(const Pose2D& prior, const nav_msgs::OccupancyGrid& msg)
{
    options_.resolution = msg.info.resolution;
    loc2d_.Init(options_);
    onInitialPose(prior);

    ROS_INFO("Localization parameters: d_thresh: %.2f, a_thresh: %.2f, l2_max: %.2f",
             options_.trans_thresh, options_.rot_thresh, options_.l2_max);

    unsigned int width = msg.info.width;
    unsigned int height= msg.info.height;

    for (unsigned int j = 0; j < height; ++j)
    {
        for (unsigned int i = 0; i < width;  ++i)
        {
            Vector3d coords;
            coords.x() = msg.info.origin.position.x + i * msg.info.resolution;
            coords.y() = msg.info.origin.position.y + j * msg.info.resolution;

            char value = msg.data[i + j*width];
            if ( value == 0 ){
                loc2d_.occupancy_map->setFree(coords);
            } else if (value == 100) {
                loc2d_.occupancy_map->setOccupied(coords);
                loc2d_.distance_map->addObstacle(loc2d_.distance_map->w2m(coords));
            }
        }
    }

    loc2d_.distance_map->update();
    first_map_received_ = true;
}

bool lama::Loc2DROS::initLaser(const sensor_msgs::LaserScanConstPtr& laser_scan)
{
    // find the origin of the sensor in the base frame
    tf::Stamped<tf::Pose> identity(tf::Transform(tf::createIdentityQuaternion(), tf::Vector3(0,0,0)),
                                   ros::Time(), laser_scan->header.frame_id);
    tf::Stamped<tf::Pose> laser_origin;
    try{ tf_->transformPose(base_frame_id_, identity, laser_origin); }
    catch(tf::TransformException& e)
    { ROS_ERROR("Could not find origin of %s", laser_scan->header.frame_id.c_str()); return false; }

    tf::Matrix3x3 mat(laser_origin.getRotation());
    tfScalar yaw, pitch, roll;
    mat.getEulerYPR(yaw, pitch, roll);

    lama::Pose3D lp(laser_origin.getOrigin().x(),
                    laser_origin.getOrigin().y(),
                    laser_origin.getOrigin().z(),
                    roll, pitch, yaw);

    lasers_origin_.push_back( lp );
        ROS_INFO("Laser is mounted upwards.");

    int laser_index = (int)frame_to_laser_.size();  // simple ID generator :)
    frame_to_laser_[laser_scan->header.frame_id] = laser_index;

    ROS_INFO("New laser configured (id=%d frame_id=%s)", laser_index, laser_scan->header.frame_id.c_str() );
    return true;
}

bool lama::Loc2DROS::onTriggerUpdate(std_srvs::Empty::Request&, std_srvs::Empty::Response&)
{
    ROS_INFO("Forced localization update Triggered");
    force_update_ = true;
    return true;
}

bool lama::Loc2DROS::globalLocalizationCallback(std_srvs::Empty::Request& req, std_srvs::Empty::Response& resp)
{
    ROS_INFO("Global Localization Triggered");
    loc2d_.triggerGlobalLocalization();
    return true;
}

int main(int argc, char *argv[])
{
    ros::init(argc, argv, "loc2d_ros");
    lama::Loc2DROS loc2d_ros;
    ros::spin();

    return 0;
}
