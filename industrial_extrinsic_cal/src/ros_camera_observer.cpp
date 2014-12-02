/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2014, Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <industrial_extrinsic_cal/ros_camera_observer.h>
namespace industrial_extrinsic_cal
{

struct combinedtarget_points_sorter {
  bool operator()(const cv::Point& lhs, const cv::Point& rhs) {
    return lhs.y < rhs.y || lhs.x < rhs.x;
  }
};

ROSCameraObserver::ROSCameraObserver(const std::string &camera_topic) :
    sym_circle_(true), pattern_(pattern_options::Chessboard), pattern_rows_(0), pattern_cols_(0)
{
  image_topic_ = camera_topic;
  //ROS_DEBUG_STREAM("ROSCameraObserver created with image topic: "<<image_topic_);
  results_pub_ = nh_.advertise<sensor_msgs::Image>("observer_results_image", 100);
}

bool ROSCameraObserver::addTarget(boost::shared_ptr<Target> targ, Roi &roi, Cost_function cost_type)
{
  // TODO make a list of targets so that the camera may make more than one set of observations at a time
  // This was what was inteneded by the interface definition, I'm not sure why the first implementation didn't do it.

  cost_type_ = cost_type; 

  //set pattern based on target
  ROS_INFO_STREAM("Target type: "<<targ->target_type_);
  instance_target_ = targ;
  switch (targ->target_type_)
  {
    case pattern_options::Chessboard:
      pattern_ = pattern_options::Chessboard;
      break;
    case pattern_options::CircleGrid:
      pattern_ = pattern_options::CircleGrid;
      break;
    case pattern_options::CombinedCircleGrid:
      pattern_ = pattern_options::CombinedCircleGrid;
      break;
    case pattern_options::ARtag:
      pattern_ = pattern_options::ARtag;
      break;
    default:
      ROS_ERROR_STREAM("target_type does not correlate to a known pattern option (Chessboard, CircleGrid, CombinedCircleGrid or ARTag)");
      return false;
      break;
  }

  if (pattern_ != 0 && pattern_ != 1 && pattern_ != 2)
  {
    ROS_ERROR_STREAM("Unknown pattern, based on target_type");
    return false;
  }

  //set pattern rows/cols based on target
  switch (pattern_)
  {
    case pattern_options::Chessboard:
      pattern_rows_ = targ->checker_board_parameters_.pattern_rows;
      pattern_cols_ = targ->checker_board_parameters_.pattern_cols;
      break;
    case pattern_options::CircleGrid:
      pattern_rows_ = targ->circle_grid_parameters_.pattern_rows;
      pattern_cols_ = targ->circle_grid_parameters_.pattern_cols;
      sym_circle_ = targ->circle_grid_parameters_.is_symmetric;
      break;
    case pattern_options::CombinedCircleGrid:
      pattern_rows_ = targ->circle_grid_parameters_.pattern_rows;
      pattern_cols_ = targ->circle_grid_parameters_.pattern_cols;
      subpattern_rows_ = targ->circle_grid_parameters_.subpattern_rows;
      subpattern_cols_ = targ->circle_grid_parameters_.subpattern_cols;
      break;
    case pattern_options::ARtag:
      ROS_ERROR_STREAM("AR Tag recognized but pattern not supported yet");
      return false;
      break;
    default:
      ROS_ERROR_STREAM("pattern_ does not correlate to a known pattern option (Chessboard, CircleGrid, CombinedCircleGrid or ARTag)");
      return false;
      break;
  }

  input_roi_.x=roi.x_min;
  input_roi_.y= roi.y_min;
  input_roi_.width= roi.x_max - roi.x_min;
  input_roi_.height= roi.y_max - roi.y_min;
  ROS_INFO_STREAM("ROSCameraObserver added target and roi");

  return true;
}

void ROSCameraObserver::clearTargets()
{
  instance_target_.reset();
  //ROS_INFO_STREAM("Targets cleared from observer");
}

void ROSCameraObserver::clearObservations()
{
  camera_obs_.clear();
  //ROS_INFO_STREAM("Observations cleared from observer");
}

int ROSCameraObserver::getObservations(CameraObservations &cam_obs)
{
  bool successful_find = false;

  ROS_INFO_STREAM("image ROI region created: "<<input_roi_.x<<" "<<input_roi_.y<<" "<<input_roi_.width<<" "<<input_roi_.height);
  if (input_bridge_->image.cols < input_roi_.width || input_bridge_->image.rows < input_roi_.height)
  {
    ROS_ERROR_STREAM("ROI too big for image size");
    return 0;
  }

  image_roi_ = input_bridge_->image(input_roi_);

  observation_pts_.clear();
  std::vector<cv::KeyPoint> key_points;
  ROS_INFO("Pattern type %d, rows %d, cols %d",pattern_,pattern_rows_,pattern_cols_);
  
  cv::Size pattern_size(pattern_cols_, pattern_rows_); // note they use cols then rows for some unknown reason
  switch (pattern_)
    {
    case pattern_options::Chessboard:
      ROS_INFO_STREAM("Finding Chessboard Corners...");
      successful_find = cv::findChessboardCorners(image_roi_, pattern_size, observation_pts_, cv::CALIB_CB_ADAPTIVE_THRESH);
      break;
    case pattern_options::CircleGrid:
      if (sym_circle_) // symetric circle grid
	{
	  ROS_INFO_STREAM("Finding Circles in grid, symmetric...");
	  successful_find = cv::findCirclesGrid(image_roi_, pattern_size, observation_pts_, cv::CALIB_CB_SYMMETRIC_GRID);
	}
      else         // asymetric circle grid
	{
	  ROS_INFO_STREAM("Finding Circles in grid, asymmetric...");
	  successful_find = cv::findCirclesGrid(image_roi_, pattern_size , observation_pts_, 
						cv::CALIB_CB_ASYMMETRIC_GRID | cv::CALIB_CB_CLUSTERING);
	}
      break;
    case pattern_options::CombinedCircleGrid:
      {
        ROS_INFO_STREAM("Finding Circles in combined grid...");
        int successes = 0;
        bool successful_find_tmp = false;
        do {
          std::vector<cv::Point2f> observation_pts_tmp;
          cv::Size subpattern_size(subpattern_cols_, subpattern_rows_);
          successful_find_tmp = cv::findCirclesGrid(image_roi_, subpattern_size, observation_pts_tmp, cv::CALIB_CB_SYMMETRIC_GRID);
          if (successful_find_tmp) {
            successes++;
            observation_pts_.insert(observation_pts_.end(), observation_pts_tmp.begin(), observation_pts_tmp.end());
            /*
            ROS_INFO_STREAM("Found subtarget with " << observation_pts_tmp.size() << " circles.");
            for (int pntIdx = 0; pntIdx < observation_pts_tmp.size(); pntIdx++) {
              ROS_INFO_STREAM("pnt " << pntIdx << ": " << observation_pts_tmp[pntIdx]);
            }
            */

            // mask already found pattern
            std::vector<cv::Point> observation_corner_pnts;
            observation_corner_pnts.push_back(observation_pts_tmp[0]);
            observation_corner_pnts.push_back(observation_pts_tmp[subpattern_cols_-1]);
            observation_corner_pnts.push_back(observation_pts_tmp[(subpattern_rows_-1)* subpattern_cols_]);
            observation_corner_pnts.push_back(observation_pts_tmp[(subpattern_rows_ * subpattern_cols_) - 1]);
            cv::fillConvexPoly(image_roi_, &observation_corner_pnts[0], observation_corner_pnts.size(), cv::Scalar(255,255,255));
          }
        } while (successful_find_tmp);
        if (successes > 0) {
          successful_find = true;
          for (int pntIdx = 0; pntIdx < observation_pts_.size(); pntIdx++) {
            ROS_INFO_STREAM("pnt " << pntIdx << ": " << observation_pts_[pntIdx]);
          }
          ROS_INFO_STREAM("Sorting points");
          std::sort(observation_pts_.begin(), observation_pts_.end(), combinedtarget_points_sorter());
          for (int pntIdx = 0; pntIdx < observation_pts_.size(); pntIdx++) {
            ROS_INFO_STREAM("pnt " << pntIdx << ": " << observation_pts_[pntIdx]);
          }
        }
      }
      break;
    }
  
  if(successful_find)  ROS_INFO_STREAM("FOUND");
  ROS_INFO_STREAM("Number of keypoints found: "<<observation_pts_.size());
  
  // next block of code for publishing the roi as an image, when target is found, circles are placed on image, with a line between pt1 and pt2
  for(int i=0;i<(int)observation_pts_.size();i++){
    cv::Point p;
    p.x = observation_pts_[i].x;
    p.y = observation_pts_[i].y;
    circle(image_roi_,p,10.0,255,5);
  }
  
  // Draw line through first column of observe points. These correspond to the first set of point in the target
  if(observation_pts_.size()>pattern_cols_){
    cv::Point p1,p2;
    p1.x = observation_pts_[0].x; 
    p1.y = observation_pts_[0].y; 
    p2.x = observation_pts_[pattern_cols_-1].x; 
    p2.y = observation_pts_[pattern_cols_-1].y; 
    line(image_roi_,p1,p2,255,3);
  }
  out_bridge_->image = image_roi_;
  results_pub_.publish(out_bridge_->toImageMsg());
  
  if(!successful_find){
    ROS_WARN_STREAM("Pattern not found for pattern: "<<pattern_ <<" with symmetry: "<< sym_circle_);
      cv::Point p;
      p.x = image_roi_.cols/2;
      p.y = image_roi_.rows/2;
      circle(image_roi_,p,10.0,255,10);
      out_bridge_->image = image_roi_;
      results_pub_.publish(out_bridge_->toImageMsg());
    return 0;
  }

  // copy the points found into a camera observation structure indicating their corresponece with target points
  camera_obs_.resize(observation_pts_.size());
  for (int i = 0; i < observation_pts_.size(); i++)
  {
    camera_obs_.at(i).target = instance_target_;
    camera_obs_.at(i).point_id = i;
    camera_obs_.at(i).image_loc_x = observation_pts_.at(i).x;
    camera_obs_.at(i).image_loc_y = observation_pts_.at(i).y;
    camera_obs_.at(i).cost_type = cost_type_;
  }

  cam_obs = camera_obs_;
  return 1;
}

void ROSCameraObserver::triggerCamera()
{
  ROS_INFO("rosCameraObserver, waiting for image from topic %s",image_topic_.c_str());
  sensor_msgs::ImageConstPtr recent_image = ros::topic::waitForMessage<sensor_msgs::Image>(image_topic_);

  ROS_INFO("GOT IT");
  try
  {
    input_bridge_ = cv_bridge::toCvCopy(recent_image, "mono8");
    output_bridge_ = cv_bridge::toCvCopy(recent_image, "bgr8");
    out_bridge_ = cv_bridge::toCvCopy(recent_image, "mono8");
    ROS_INFO_STREAM("cv image created based on ros image");
  }
  catch (cv_bridge::Exception& ex)
  {
    ROS_ERROR("Failed to convert image");
    ROS_WARN_STREAM("cv_bridge exception: "<<ex.what());
    return;
  }

}

bool ROSCameraObserver::observationsDone()
{
  //if (camera_obs_.observations.size() != 0)
  if(!input_bridge_)
  {
    return false;
  }
  return true;
}
} //industrial_extrinsic_cal
