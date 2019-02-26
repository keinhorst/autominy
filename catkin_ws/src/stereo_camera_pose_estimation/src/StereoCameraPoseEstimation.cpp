#include <stereo_camera_pose_estimation/StereoCameraPoseEstimation.h>
#include <opencv2/imgcodecs.hpp>
#include <cv.hpp>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>

namespace stereo_camera_pose_estimation {
    StereoCameraPoseEstimation::StereoCameraPoseEstimation() {
        coefficients = pcl::ModelCoefficients::Ptr(new pcl::ModelCoefficients);
        rawPointCloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
        planePointCloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
        transformedPointCloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
        dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_50);
    }

    StereoCameraPoseEstimation::~StereoCameraPoseEstimation() = default;

    void StereoCameraPoseEstimation::setConfig(StereoCameraPoseEstimationConfig& config) {
        this->config = config;
    }

    void StereoCameraPoseEstimation::getPointcloud(
            const sensor_msgs::ImageConstPtr& depthImage, const sensor_msgs::CameraInfoConstPtr& depthCameraInfo,
            double maximumDepth) {
        depthCameraModel.fromCameraInfo(depthCameraInfo);

        auto center_x = static_cast<float>(depthCameraModel.cx());
        auto center_y = static_cast<float>(depthCameraModel.cy());

        // Combine unit conversion (if necessary) with scaling by focal length for computing (X,Y)
        double unit_scaling = 0.001f;
        auto constant_x = static_cast<float>(unit_scaling / depthCameraModel.fx());
        auto constant_y = static_cast<float>(unit_scaling / depthCameraModel.fy());
        auto maximum_depth = static_cast<int>(maximumDepth * 1000);

        auto depth_row = reinterpret_cast<const uint16_t*>(&depthImage->data[0]);
        int row_step = depthImage->step / sizeof(uint16_t);

        rawPointCloud->clear();
        for (int v = 0; v < (int) depthImage->height; ++v, depth_row += row_step) {
            for (int u = 0; u < (int) depthImage->width; ++u) {
                uint16_t depth = depth_row[u];

                // Skip missing points
                if (depth == 0  || depth > maximum_depth) {
                    continue;
                }

                pcl::PointXYZ p;
                p.x = (u - center_x) * depth * constant_x;
                p.y = (v - center_y) * depth * constant_y;
                p.z = depth * 0.001f;
                rawPointCloud->push_back(p);
            }
        }


        rawPointCloud->height = 1;
        rawPointCloud->width = static_cast<uint32_t>(rawPointCloud->points.size());
        rawPointCloud->is_dense = true;
        rawPointCloud->header.stamp = depthImage->header.stamp.toNSec() / 1000ull;
        rawPointCloud->header.seq = depthImage->header.seq;
        rawPointCloud->header.frame_id = depthImage->header.frame_id;
    }

    bool StereoCameraPoseEstimation::processImage(
            const sensor_msgs::ImageConstPtr& image, const sensor_msgs::CameraInfoConstPtr& cameraInfo,
            const sensor_msgs::ImageConstPtr& depthImage, const sensor_msgs::CameraInfoConstPtr& depthCameraInfo) {

        auto durationSinceLastCalibration = ros::Time::now() - lastPoseEstimationTime;
        if (durationSinceLastCalibration.toSec() < config.idle_time) {
            return false;
        }

        getPointcloud(depthImage, depthCameraInfo, config.maximum_depth);
        pcl_ros::transformPointCloud("camera_link", *rawPointCloud, *transformedPointCloud, tfListener);

        pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
        // Create the segmentation object
        pcl::SACSegmentation<pcl::PointXYZ> seg;
        seg.setOptimizeCoefficients (true);
        seg.setModelType (pcl::SACMODEL_PLANE);
        seg.setMethodType (pcl::SAC_RANSAC);
        seg.setDistanceThreshold (0.01);

        seg.setInputCloud (transformedPointCloud);
        seg.segment (*inliers, *coefficients);

        if (inliers->indices.empty())
        {
            ROS_ERROR ("Could not estimate a planar model for the given dataset.");
            return false;
        }

        extractIndicesFilter.setIndices(inliers);
        extractIndicesFilter.setInputCloud(transformedPointCloud);
        extractIndicesFilter.filter(*planePointCloud);

        Eigen::Vector3f surface(coefficients->values[0], coefficients->values[1], coefficients->values[2]);
        surface /= coefficients->values[3];

        auto height = 1.0 / surface.norm();
        auto pitch = std::atan2(surface[0], surface[2]);
        auto roll = std::atan2(surface[1], surface[2]);
        auto yaw = 0.0;

        try {
            this->image = cv_bridge::toCvCopy(image);
        }
        catch (cv_bridge::Exception& e) {
            ROS_ERROR("Could not convert from '%s' to 'bgr8'.", image->encoding.c_str());
            return false;
        }
        auto& cvImage = this->image->image;
        imageCameraModel.fromCameraInfo(cameraInfo);

        std::vector<int> ids;
        std::vector<std::vector<cv::Point2f>> corners;
        std::vector<cv::Vec3d> rvecs, tvecs;
        cv::aruco::detectMarkers(cvImage, dictionary, corners, ids);

        if (!ids.empty()) {
            cv::aruco::drawDetectedMarkers(cvImage, corners, ids);
            cv::aruco::estimatePoseSingleMarkers(corners, static_cast<float>(config.aruco_size), imageCameraModel.intrinsicMatrix(),
                                                 imageCameraModel.distortionCoeffs(), rvecs, tvecs);

            tf::StampedTransform t;
            tfListener.lookupTransform("camera_link", image->header.frame_id, ros::Time(0), t);
            for (int i = 0; i < ids.size(); i++) {
                if (ids[i] != config.aruco_id) {
                    continue;
                }

                cv::aruco::drawAxis(cvImage, imageCameraModel.intrinsicMatrix(), imageCameraModel.distortionCoeffs(),
                                    rvecs[i], tvecs[i], 0.1);
                cv::Mat1d  cameraTransformRotation;
                cv::Rodrigues(rvecs[i], cameraTransformRotation);

                tf::Matrix3x3 rotation
                        (cameraTransformRotation(0, 0), cameraTransformRotation(0, 1), cameraTransformRotation(0, 2),
                         cameraTransformRotation(1, 0), cameraTransformRotation(1, 1), cameraTransformRotation(1, 2),
                         cameraTransformRotation(2, 0), cameraTransformRotation(2, 1), cameraTransformRotation(2, 2));
                tf::Vector3 translation(tvecs[i][0], tvecs[i][1], tvecs[i][2]);

                tf::Quaternion q;
                rotation.getRotation(q);
                q = t * q;
                q = q.inverse();
                translation = t * translation;

                yaw = tf::getYaw(q);

                break;
            }
        } else {
            ROS_ERROR("Marker not found!");
            return false;
        }

        if (std::fabs(roll) > config.max_roll) {
            ROS_ERROR("Roll is bigger than configured threshold. Estimated %f but max threshold %f!", roll, config.max_roll);
            return false;
        }

        if (std::fabs(pitch) > config.max_pitch) {
            ROS_ERROR("Pitch is bigger than configured threshold. Estimated %f but max threshold %f!", pitch, config.max_pitch);
            return false;
        }

        if (std::fabs(yaw) > config.max_yaw) {
            ROS_ERROR("Yaw is bigger than configured threshold. Estimated %f but max threshold %f!", yaw, config.max_yaw);
            return false;
        }

        auto rot = tf::createQuaternionFromRPY(roll, -pitch, yaw);
        auto trans = tf::Vector3(0.0, 0.0, height);
        tf::Transform t(rot, trans);
        tf::StampedTransform transform(t, ros::Time::now(), "base_link", "camera_link");
        tfBroadcaster.sendTransform(transform);

        lastPoseEstimationTime = ros::Time::now();

        return true;
    }

    sensor_msgs::PointCloud2ConstPtr StereoCameraPoseEstimation::getPlaneCloud() {
        auto ret = boost::make_shared<sensor_msgs::PointCloud2>();
        pcl::toROSMsg(*planePointCloud, *ret);

        return ret;
    }

    sensor_msgs::ImagePtr StereoCameraPoseEstimation::getMarkerImage() {
        return image->toImageMsg();
    }
}
