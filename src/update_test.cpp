#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <cv_bridge/cv_bridge.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_types.h>
#include <pcl/filters/passthrough.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.hpp> 
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <yolov8_msgs/msg/detection_array.hpp>
#include <geometry_msgs/msg/pose_array.hpp>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <Eigen/Dense>

// Define the LidarCameraFusionNode class which inherits from rclcpp::Node
class LidarCameraFusionNode : public rclcpp::Node
{
public:

  // Constructor for the node
  LidarCameraFusionNode();

private:

    // Subscribers
    message_filters::Subscriber<sensor_msgs::msg::Image> image_sub_;
    message_filters::Subscriber<yolov8_msgs::msg::DetectionArray> detections_sub_;
    message_filters::Subscriber<sensor_msgs::msg::PointCloud2> scan_sub_;
    
    // Synchronizer
    typedef message_filters::sync_policies::ApproximateTime<
        yolov8_msgs::msg::DetectionArray,
        sensor_msgs::msg::Image,
        sensor_msgs::msg::PointCloud2>
        SyncPolicy;
    message_filters::Synchronizer<SyncPolicy> sync_;
    
    // Camera info subscriber
    rclcpp::Subscription<sensor_msgs::msg::CameraInfo>::SharedPtr caminfo_sub_;
    
    // Publishers
    rclcpp::Publisher<geometry_msgs::msg::PoseArray>::SharedPtr object_pose_pub_;
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr overlay_image_pub_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr object_point_cloud_pub_;
    
    // TF buffer and listener
    std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
    std::shared_ptr<tf2_ros::TransformListener> tf_listener_;

    // Camera intrinsics
    double fx_, fy_, cx_, cy_;
    bool camera_info_received_;

    // Filtering parameters
    float min_x_, max_x_, min_y_, max_y_, min_z_, max_z_;

    // Frame IDs
    std::string lidar_frame_;
    std::string camera_frame_;

    // Image dimensions
    unsigned int image_width_;
    unsigned int image_height_;

    // Projected points
    std::vector<cv::Point2d> projected_points_;

    // Bounding boxes
    struct BoundingBox {
        double x_min, y_min, x_max, y_max;
        double sum_x = 0, sum_y = 0, sum_z = 0;  // Sum of coordinates
        int count = 0;  // Number of points within the bounding box
        bool valid = false;  // Indicates if the bbox is valid
        int id = -1;  // ID of the bounding box
    };
    std::vector<BoundingBox> bounding_boxes;

    // Callback functions
    void detection_image_scan_callback(
        const yolov8_msgs::msg::DetectionArray::ConstSharedPtr &detections_msg,
        const sensor_msgs::msg::Image::ConstSharedPtr &image_msg,
        const sensor_msgs::msg::PointCloud2::ConstSharedPtr &scan_msg);

    void caminfoCallback(const sensor_msgs::msg::CameraInfo::SharedPtr msg);

    void compute_lidar_points(
        const sensor_msgs::msg::PointCloud2::ConstSharedPtr &scan_msg,
        std::vector<Eigen::Vector3d> &points_lidar,
        std::vector<size_t> &indices);

    bool transform_lidar_points_to_camera_frame(
        const std::vector<Eigen::Vector3d> &points_lidar,
        const std::string &source_frame,
        const std::string &target_frame,
        const rclcpp::Time &time_stamp,
        std::vector<Eigen::Vector3d> &points_camera);

    void project_points_to_image_plane(
        const std::vector<Eigen::Vector3d> &points_camera,
        double fx, double fy, double cx, double cy,
        const sensor_msgs::msg::Image::ConstSharedPtr &image_msg,
        const std::vector<size_t> &indices,
        std::vector<int> &u, std::vector<int> &v,
        std::vector<double> &x_cam, std::vector<double> &y_cam, std::vector<double> &z_cam,
        std::vector<size_t> &valid_indices);
};

LidarCameraFusionNode::LidarCameraFusionNode() 
    : Node("lidar_camera_fusion_node"), 
      image_sub_(this, "bgr_image"),
      detections_sub_(this, "/yolo/detections"),
      scan_sub_(this, "/scan"),
      sync_(SyncPolicy(10), detections_sub_, image_sub_, scan_sub_),
      camera_info_received_(false)
    {
    // Declare and retrieve parameters for filtering the point cloud
    this->declare_parameter<float>("min_x", -10.0);
    this->declare_parameter<float>("max_x", 10.0);
    this->declare_parameter<float>("min_y", -10.0);
    this->declare_parameter<float>("max_y", 10.0);
    this->declare_parameter<float>("min_z", -2.0);
    this->declare_parameter<float>("max_z", 2.0);
    this->get_parameter("min_x", min_x_);
    this->get_parameter("max_x", max_x_);
    this->get_parameter("min_y", min_y_);
    this->get_parameter("max_y", max_y_);
    this->get_parameter("min_z", min_z_);
    this->get_parameter("max_z", max_z_);

    // Initialize frame IDs
    lidar_frame_ = "lidar_frame";   // Replace with your actual lidar frame ID
    camera_frame_ = "camera_frame"; // Replace with your actual camera frame ID

    tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
    tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

    sync_.registerCallback(std::bind(&LidarCameraFusionNode::detection_image_scan_callback, this,
                                     std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    caminfo_sub_ = this->create_subscription<sensor_msgs::msg::CameraInfo>(
        "camera_info", 10, std::bind(&LidarCameraFusionNode::caminfoCallback, this, std::placeholders::_1));

    object_pose_pub_ = this->create_publisher<geometry_msgs::msg::PoseArray>("detected_object_positions", 10);
    overlay_image_pub_ = this->create_publisher<sensor_msgs::msg::Image>("overlay_image", 10);
    object_point_cloud_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("detected_points_cloud", 10);

    RCLCPP_INFO(this->get_logger(), "Initialized lidar_camera_fusion_node");
  }

void LidarCameraFusionNode::caminfoCallback(const sensor_msgs::msg::CameraInfo::SharedPtr msg)
{
    image_width_ = msg->width;
    image_height_ = msg->height;
    fx_ = msg->k[0]; // fx
    fy_ = msg->k[4]; // fy
    cx_ = msg->k[2]; // cx
    cy_ = msg->k[5]; // cy
    camera_info_received_ = true;
}

void LidarCameraFusionNode::detection_image_scan_callback(
    const yolov8_msgs::msg::DetectionArray::ConstSharedPtr &detections_msg,
    const sensor_msgs::msg::Image::ConstSharedPtr &image_msg,
    const sensor_msgs::msg::PointCloud2::ConstSharedPtr &scan_msg)
{
    if (!camera_info_received_) {
        RCLCPP_WARN(this->get_logger(), "Camera info not yet received");
        return;
    }

    // Process detections
    bounding_boxes.clear();  // Clear previous detections

    for (const auto& detection : detections_msg->detections) {
        BoundingBox bbox;
        bbox.x_min = detection.bbox.center.position.x - detection.bbox.size.x / 2.0;
        bbox.y_min = detection.bbox.center.position.y - detection.bbox.size.y / 2.0;
        bbox.x_max = detection.bbox.center.position.x + detection.bbox.size.x / 2.0;
        bbox.y_max = detection.bbox.center.position.y + detection.bbox.size.y / 2.0;
        bbox.valid = true;
        try {
            bbox.id = std::stoi(detection.id);  // Convert string ID to integer
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to convert detection ID to integer: %s", e.what());
            continue;
        }
        bounding_boxes.push_back(bbox);  
    }

    // Process scan message
    std::vector<Eigen::Vector3d> points_lidar;
    std::vector<size_t> indices;

    compute_lidar_points(scan_msg, points_lidar, indices);

    // Transform lidar points to camera frame
    std::vector<Eigen::Vector3d> points_camera;

    if (!transform_lidar_points_to_camera_frame(points_lidar, scan_msg->header.frame_id, camera_frame_, scan_msg->header.stamp, points_camera)) {
        RCLCPP_ERROR(this->get_logger(), "Failed to transform lidar points to camera frame");
        return;
    }

    // Project points onto image plane
    std::vector<int> u, v;
    std::vector<double> x_cam, y_cam, z_cam;
    std::vector<size_t> valid_indices;

    project_points_to_image_plane(points_camera, fx_, fy_, cx_, cy_, image_msg, indices, u, v, x_cam, y_cam, z_cam, valid_indices);

    // For each projected point, check if it falls within any bounding box
    projected_points_.clear();

    // Create a vector to store point clouds for each detected object
    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> object_point_clouds;
    for (const auto& bbox : bounding_boxes) {
        object_point_clouds.push_back(pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>()));
    }

    for (size_t i = 0; i < valid_indices.size(); ++i) {
        int u_i = u[i];
        int v_i = v[i];
        double x = x_cam[i];
        double y = y_cam[i];
        double z = z_cam[i];

        cv::Point2d uv(u_i, v_i);

        // Check if the point is within any bounding box
        for (size_t bbox_idx = 0; bbox_idx < bounding_boxes.size(); ++bbox_idx) {
            auto& bbox = bounding_boxes[bbox_idx];
            if (bbox.valid && u_i >= bbox.x_min && u_i <= bbox.x_max && v_i >= bbox.y_min && v_i <= bbox.y_max) {
                projected_points_.push_back(uv);  // Store valid points
                bbox.sum_x += x;
                bbox.sum_y += y;
                bbox.sum_z += z;
                bbox.count++;  // Increment count of points within this bbox

                // Add point to object-specific point cloud
                pcl::PointXYZ point_pcl(x, y, z);
                object_point_clouds[bbox_idx]->points.push_back(point_pcl);
            }
        }
    }

    // Publish object point clouds
    for (size_t bbox_idx = 0; bbox_idx < bounding_boxes.size(); ++bbox_idx) {
        const auto& bbox = bounding_boxes[bbox_idx];
        auto& object_cloud = object_point_clouds[bbox_idx];
        
        if (!object_cloud->points.empty()) {
            object_cloud->width = object_cloud->points.size();
            object_cloud->height = 1;
            object_cloud->is_dense = true;

            // Convert PCL point cloud to ROS message
            sensor_msgs::msg::PointCloud2 object_cloud_msg;
            pcl::toROSMsg(*object_cloud, object_cloud_msg);
            
            // Set the header
            object_cloud_msg.header.stamp = scan_msg->header.stamp;
            object_cloud_msg.header.frame_id = camera_frame_;

            // Publish the point cloud
            object_point_cloud_pub_->publish(object_cloud_msg);
        }
    }

    // Calculate average position for each bounding box
    geometry_msgs::msg::PoseArray pose_array;
    pose_array.header.stamp = this->get_clock()->now();
    pose_array.header.frame_id = camera_frame_;

    for (const auto& bbox : bounding_boxes) {
        if (bbox.count > 0) {  // Ensure there is at least one point
            double avg_x = bbox.sum_x / bbox.count;
            double avg_y = bbox.sum_y / bbox.count;
            double avg_z = bbox.sum_z / bbox.count;

            // Create a new pose
            geometry_msgs::msg::Pose pose;
            pose.position.x = avg_x;
            pose.position.y = avg_y;
            pose.position.z = avg_z;
            pose.orientation.w = 1.0;  // Default orientation (no rotation)

            // Add the pose to the array
            pose_array.poses.push_back(pose);
        }
    }

    object_pose_pub_->publish(pose_array);

    // Overlay projected points onto image
    cv_bridge::CvImagePtr cv_ptr;
    try {
        cv_ptr = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);
    } catch (cv_bridge::Exception& e) {
        RCLCPP_ERROR(this->get_logger(), "cv_bridge exception: %s", e.what());
        return;
    }

    // Draw points
    for (const auto& uv : projected_points_) {
        cv::circle(cv_ptr->image, cv::Point(uv.x, uv.y), 5, CV_RGB(255,0,0), -1);
    }

    // Publish modified image
    overlay_image_pub_->publish(*cv_ptr->toImageMsg());
}

void LidarCameraFusionNode::compute_lidar_points(
    const sensor_msgs::msg::PointCloud2::ConstSharedPtr &scan_msg,
    std::vector<Eigen::Vector3d> &points_lidar,
    std::vector<size_t> &indices)
{
    // Convert ROS point cloud message to PCL point cloud
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::fromROSMsg(*scan_msg, *cloud);

    // Apply pass-through filters on x, y, z
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::PassThrough<pcl::PointXYZ> pass;
    pass.setInputCloud(cloud);

    pass.setFilterFieldName("x");
    pass.setFilterLimits(min_x_, max_x_);
    pass.filter(*cloud_filtered);

    pass.setInputCloud(cloud_filtered);
    pass.setFilterFieldName("y");
    pass.setFilterLimits(min_y_, max_y_);
    pass.filter(*cloud_filtered);

    pass.setInputCloud(cloud_filtered);
    pass.setFilterFieldName("z");
    pass.setFilterLimits(min_z_, max_z_);
    pass.filter(*cloud_filtered);

    // Extract the points
    for (size_t i = 0; i < cloud_filtered->points.size(); ++i) {
        const auto& point = cloud_filtered->points[i];
        points_lidar.emplace_back(point.x, point.y, point.z);
        indices.push_back(i);
    }
}

bool LidarCameraFusionNode::transform_lidar_points_to_camera_frame(
    const std::vector<Eigen::Vector3d> &points_lidar,
    const std::string &source_frame,
    const std::string &target_frame,
    const rclcpp::Time &time_stamp,
    std::vector<Eigen::Vector3d> &points_camera)
{
    geometry_msgs::msg::TransformStamped transformStamped;
    try {
        transformStamped = tf_buffer_->lookupTransform(target_frame, source_frame, time_stamp, tf2::durationFromSec(1.0));
    } catch (tf2::TransformException &ex) {
        RCLCPP_WARN(this->get_logger(), "Could not transform %s to %s: %s", source_frame.c_str(), target_frame.c_str(), ex.what());
        return false;
    }

    // Convert geometry_msgs Transform to Eigen
    Eigen::Affine3d transform = tf2::transformToEigen(transformStamped.transform);

    // Transform the points
    for (const auto& point : points_lidar) {
        Eigen::Vector3d point_camera = transform * point;
        points_camera.push_back(point_camera);
    }

    return true;
}

void LidarCameraFusionNode::project_points_to_image_plane(
    const std::vector<Eigen::Vector3d> &points_camera,
    double fx, double fy, double cx, double cy,
    const sensor_msgs::msg::Image::ConstSharedPtr &image_msg,
    const std::vector<size_t> &indices,
    std::vector<int> &u, std::vector<int> &v,
    std::vector<double> &x_cam, std::vector<double> &y_cam, std::vector<double> &z_cam,
    std::vector<size_t> &valid_indices)
{
    size_t width = image_msg->width;
    size_t height = image_msg->height;

    for (size_t i = 0; i < points_camera.size(); ++i) {
        const auto& point = points_camera[i];
        if (point.z() <= 0) continue; // Points behind the camera

        int u_i = static_cast<int>((point.x() / point.z()) * fx + cx);
        int v_i = static_cast<int>((point.y() / point.z()) * fy + cy);

        if (u_i >= 0 && u_i < static_cast<int>(width) && v_i >= 0 && v_i < static_cast<int>(height)) {
            u.push_back(u_i);
            v.push_back(v_i);
            x_cam.push_back(point.x());
            y_cam.push_back(point.y());
            z_cam.push_back(point.z());
            valid_indices.push_back(indices[i]);
        }
    }
}

int main(int argc, char **argv)
{
  // Initialize the ROS2 node
  rclcpp::init(argc, argv);
  // Create and spin the LidarCameraFusionNode
  auto node = std::make_shared<LidarCameraFusionNode>();
  rclcpp::spin(node);
  // Shutdown ROS2 cleanly
  rclcpp::shutdown();
  return 0;
}
