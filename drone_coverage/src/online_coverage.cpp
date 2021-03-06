/*
* Copyright (c) 2019 Kosmas Tsiakas
*
* GNU GENERAL PUBLIC LICENSE
*    Version 3, 29 June 2007
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "drone_coverage/online_coverage.h"

namespace drone_coverage
{
OnlineCoverage::OnlineCoverage()
{
  ROS_INFO("Coverage object created\n");
  _octomap_loaded = 0;

  _map_sub = _nh.subscribe<octomap_msgs::Octomap>("/octomap_binary", 1, &OnlineCoverage::octomapCallback, this);
  _pose_sub = _nh.subscribe("/amcl_pose", 1000, &OnlineCoverage::poseCallback, this);

  _covered_pub = _nh.advertise<octomap_msgs::Octomap>("/octomap_covered", 1000);
  _percentage_pub = _nh.advertise<drone_gazebo::Float64Stamped>("/octomap_covered/percentage", 1000);
  _volume_pub = _nh.advertise<drone_gazebo::Float64Stamped>("/octomap_covered/volume", 1000);

  _nh.param<double>("/world/min_obstacle_height", _min_obstacle_height, 0.3);
  _nh.param<double>("/world/max_obstacle_height", _max_obstacle_height, 2.0);

  // Get configurations
  _nh.param<double>("/sensor/rfid/range", _rfid_range, 1);
  _nh.param<double>("/sensor/rfid/hfov", _rfid_hfov, 60);
  _nh.param<double>("/sensor/rfid/vfov", _rfid_vfov, 30);
  _nh.param<std::string>("/sensor/rfid/shape", _sensor_shape, "circular");
  _nh.param<double>("/sensor/rfid/direction/x", _rfid_direction_x, 1);
  _nh.param<double>("/sensor/rfid/direction/y", _rfid_direction_y, 0);
  _nh.param<double>("/sensor/rfid/direction/z", _rfid_direction_z, 0);

  // Adjust values
  _rfid_hfov = (_rfid_hfov / 180.0) * M_PI;
  _rfid_vfov = (_rfid_vfov / 180.0) * M_PI;
}

OnlineCoverage::~OnlineCoverage()
{
  if (_octomap != NULL)
    delete _octomap;
  if (_covered != NULL)
    delete _covered;
}

void OnlineCoverage::octomapCallback(const octomap_msgs::OctomapConstPtr& msg)
{
  // Load octomap msg
  octomap::AbstractOcTree* abstract = octomap_msgs::msgToMap(*msg);
  if (abstract)
  {
    octomap::ColorOcTree* coloroctree = dynamic_cast<octomap::ColorOcTree*>(abstract);
    _octomap = reinterpret_cast<octomap::OcTree*>(coloroctree);

    if (_octomap == NULL)
    {
      ROS_WARN("Octomap message does not contain an OcTree\n");
      return;
    }
    else
    {
      ROS_INFO("Octomap successfully loaded\n");
      _octomap->expand();  // bbx work currently only with expanded tree
    }
  }
  else
  {
    ROS_WARN("Could not deserialize message to OcTree");
    return;
  }

  _octomap_resolution = _octomap->getResolution();
  _octomap_volume = calculateOccupiedVolume(_octomap);

  // Now that we have the resolution we can initialize the new octomap
  _covered = new octomap::ColorOcTree(_octomap_resolution);

  _octomap_loaded = 1;
}

void OnlineCoverage::poseCallback(const geometry_msgs::PoseStampedConstPtr& msg)
{
  if (!_octomap_loaded)
    return;

  if (_sensor_shape == "circular")
    calculateCircularCoverage(msg->pose);
  else
    calculateOrthogonalCoverage(msg->pose);

  publishCoveredSurface();
  publishPercentage();
}

void OnlineCoverage::calculateOrthogonalCoverage(const geometry_msgs::Pose pose)
{
  octomap::point3d wall_point;

  octomath::Vector3 position(pose.position.x, pose.position.y, pose.position.z);
  octomath::Quaternion orientation(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);

  double yaw = orientation.toEuler().yaw();
  // Horizontal FOV degrees
  for (double horizontal = yaw - _rfid_hfov / 2; horizontal <= yaw + _rfid_hfov / 2; horizontal += DEGREE)
  {
    // Vertical FOV degrees
    for (double vertical = -_rfid_vfov / 2; vertical <= _rfid_vfov / 2; vertical += DEGREE)
    {
      // direction at which we are facing the point
      octomap::point3d direction(_rfid_direction_x, _rfid_direction_y, _rfid_direction_z);

      if (_octomap->castRay(position, direction.rotate_IP(0, vertical, horizontal), wall_point, true, _rfid_range))
      {
        // Ground elimination
        if (wall_point.z() < _min_obstacle_height || wall_point.z() > _max_obstacle_height)
          continue;

        if (_covered->insertRay(position, wall_point, _rfid_range))
        {
          octomap::ColorOcTreeNode* node = _covered->search(wall_point);
          if (node != NULL)
            node->setColor(0.5, 0.5, 0.5);
        }
      }
    }  // vertical loop
  }    // horizontal loop
}

void OnlineCoverage::calculateCircularCoverage(const geometry_msgs::Pose pose)
{
  octomap::point3d wall_point;

  octomath::Vector3 position(pose.position.x, pose.position.y, pose.position.z);
  octomath::Quaternion orientation(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);

  double yaw = orientation.toEuler().yaw();
  // Horizontal FOV degrees
  for (double horizontal = yaw - _rfid_hfov / 2; horizontal <= yaw + _rfid_hfov / 2; horizontal += DEGREE)
  {
    // Vertical FOV degrees
    for (double vertical = -_rfid_vfov / 2; vertical <= _rfid_vfov / 2; vertical += DEGREE)
    {
      // direction at which we are facing the point
      octomap::point3d direction(_rfid_direction_x, _rfid_direction_y, _rfid_direction_z);

      if (_octomap->castRay(position, direction.rotate_IP(0, vertical, horizontal), wall_point, true, _rfid_range))
      {
        // Ground elimination
        if (wall_point.z() < _min_obstacle_height || wall_point.z() > _max_obstacle_height)
          continue;

        // Make the coverage circular, cut the points that are larger than the range==radius
        if (position.distanceXY(wall_point) > _rfid_range)
          continue;

        if (_covered->insertRay(position, wall_point, _rfid_range))
        {
          octomap::ColorOcTreeNode* node = _covered->search(wall_point);
          if (node != NULL)
            node->setColor(1, 0, 0);
        }
      }
    }  // vertical loop
  }    // horizontal loop
}

void OnlineCoverage::publishCoveredSurface()
{
  octomap_msgs::Octomap msg;
  msg.header.stamp = ros::Time::now();
  msg.header.frame_id = "map";
  msg.binary = false;
  msg.id = _covered->getTreeType();
  msg.resolution = _covered->getResolution();
  if (octomap_msgs::fullMapToMsg(*_covered, msg))
    _covered_pub.publish(msg);
}

float OnlineCoverage::calculateOccupiedVolume(octomap::ColorOcTree* octomap)
{
  float vol_occ = 0;
  double bbxMinX, bbxMinY, bbxMinZ, bbxMaxX, bbxMaxY, bbxMaxZ;
  octomap->getMetricMax(bbxMaxX, bbxMaxY, bbxMaxZ);
  octomap->getMetricMin(bbxMinX, bbxMinY, bbxMinZ);

  octomap::point3d min(bbxMinX, bbxMinY, bbxMinZ);
  octomap::point3d max(bbxMaxX, bbxMaxY, bbxMaxZ);

  if (octomap)
  {  // can be NULL
    for (octomap::ColorOcTree::leaf_bbx_iterator it = octomap->begin_leafs_bbx(min, max),
                                                 end = octomap->end_leafs_bbx();
         it != end; ++it)
    {
      if (it.getCoordinate().z() < _min_obstacle_height || it.getCoordinate().z() > _max_obstacle_height)
        continue;

      if (octomap->isNodeOccupied(*it))
        // occupied leaf node
        vol_occ += it.getSize() * it.getSize() * it.getSize();
    }
  }
  return vol_occ;
}

float OnlineCoverage::calculateOccupiedVolume(octomap::OcTree* octomap)
{
  float vol_occ = 0;
  double bbxMinX, bbxMinY, bbxMinZ, bbxMaxX, bbxMaxY, bbxMaxZ;
  octomap->getMetricMax(bbxMaxX, bbxMaxY, bbxMaxZ);
  octomap->getMetricMin(bbxMinX, bbxMinY, bbxMinZ);

  octomap::point3d min(bbxMinX, bbxMinY, bbxMinZ);
  octomap::point3d max(bbxMaxX, bbxMaxY, bbxMaxZ);

  if (octomap)
  {  // can be NULL
    for (octomap::OcTree::leaf_bbx_iterator it = octomap->begin_leafs_bbx(min, max), end = octomap->end_leafs_bbx();
         it != end; ++it)
    {
      bool exclude = 0;  // variable to be set if a node should not be included in volume estimation

      if (it.getCoordinate().z() < _min_obstacle_height || it.getCoordinate().z() > _max_obstacle_height)
        continue;

      if (octomap->isNodeOccupied(*it))
      // occupied leaf node
      {
        // i: 0 checking for x-axis neighbors
        // i: 1 checking for y-axis neighbors
        // [1] -> [2]] -> [3] ---> if all nodes are occupied it is probably an obstacle
        // if the [3] is unknown, it is probably noise from the octomap
        octomap::OcTreeKey key = it.getKey();
        for (int i = 0; i <= 1; i++)
        {
          octomap::OcTreeKey first_neighbor_key = key;
          first_neighbor_key[i] += 1;
          octomap::OcTreeNode* first_node = octomap->search(first_neighbor_key);
          if (first_node != NULL && octomap->isNodeOccupied(first_node))
          {
            octomap::OcTreeKey second_neighbor_key = key;
            second_neighbor_key[i] += 2;
            octomap::OcTreeNode* second_node = octomap->search(second_neighbor_key);
            if (second_node == NULL)
            {
              exclude = 1;
              break;
            }
          }
        }
        if (!exclude)
          vol_occ += it.getSize() * it.getSize() * it.getSize();
      }
    }
  }
  return vol_occ;
}

void OnlineCoverage::publishPercentage()
{
  float covered_volume = calculateOccupiedVolume(_covered);

  drone_gazebo::Float64Stamped msg;
  msg.header.stamp = ros::Time::now();
  msg.data = covered_volume;
  _volume_pub.publish(msg);

  msg.data = 100 * (covered_volume / _octomap_volume);
  _percentage_pub.publish(msg);
}

}  // namespace drone_coverage
