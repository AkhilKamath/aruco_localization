#include "aruco_localization/ArucoLocalizer.h"

namespace aruco_localizer {

// ----------------------------------------------------------------------------

ArucoLocalizer::ArucoLocalizer() :
    nh_(ros::NodeHandle()), nh_private_("~"), it_(nh_)
{

    // Read in ROS params
    std::string mmConfigFile = nh_private_.param<std::string>("markermap_config", "");
    double markerSize = nh_private_.param<double>("marker_size", 0.0298);
    nh_private_.param<bool>("show_output_video", show_output_video_, false);

    // Subscribe to input video feed and publish output video feed
    it_ = image_transport::ImageTransport(nh_);
    image_sub_ = it_.subscribeCamera("input_image", 1, &ArucoLocalizer::cameraCallback, this);
    image_pub_ = it_.advertise("output_image", 1);

    // Create ROS publishers
    // tag_list_pub = nh_.advertise<aprilvo::AprilTagList>("apriltags", 100);
    estimate_pub_ = nh_private_.advertise<nav_msgs::Odometry>("estimate", 1);

    //
    // Set up the ArUco detector
    //

    // Set up the Marker Map dimensions, spacing, dictionary, etc from the YAML
    mmConfig_.readFromFile(mmConfigFile);

    // Prepare the marker detector by:
    // (1) setting the dictionary we are using
    mDetector_.setDictionary(mmConfig_.getDictionary());
    // (2) setting the corner refinement method
    // ... TODO -- make this corner sub pix or something
    mDetector_.setCornerRefinementMethod(aruco::MarkerDetector::LINES);

    // set markmap size. Convert to meters if necessary
    if (mmConfig_.isExpressedInPixels())
        mmConfig_ = mmConfig_.convertToMeters(markerSize);

    // Configuring of Pose Tracker is done once a
    // CameraInfo message has been received
}

// ----------------------------------------------------------------------------

ArucoLocalizer::~ArucoLocalizer() { }

// ----------------------------------------------------------------------------
// Private Methods
// ----------------------------------------------------------------------------

void ArucoLocalizer::sendtf(const cv::Mat& rvec, const cv::Mat& tvec) {
    static tf::TransformBroadcaster br;

    // Create the transform from the camera to the ArUco Marker Map
    tf::Transform transform = aruco2tf(rvec, tvec);

    // tf::StampedTransform cam2Ref;
    // cam2Ref.setIdentity();

    ros::Time now = ros::Time::now();

    br.sendTransform(tf::StampedTransform(transform, now, "aruco", "camera"));

    //
    // Link camera to the quad body
    //

    transform.setIdentity();
    transform.setOrigin(tf::Vector3(0.0, 0.0, 0));
    tf::Quaternion q; q.setRPY(0.0, -1.5707, 0.0);
    transform.setRotation(q);
    br.sendTransform(tf::StampedTransform(transform, now, "camera", "chiny"));


    //
    // Link ArUco Marker Map to the world
    //

    transform.setIdentity();
    transform.setOrigin(tf::Vector3(0.0, 0.0, -0.4064));
    br.sendTransform(tf::StampedTransform(transform, now, "world", "aruco"));

}

// ----------------------------------------------------------------------------

void ArucoLocalizer::processImage(cv::Mat& frame) {

    // Detection of the board
    std::vector<aruco::Marker> detected_markers = mDetector_.detect(frame);

    // print the markers detected that belongs to the markerset
    for (auto idx : mmConfig_.getIndices(detected_markers))
        detected_markers[idx].draw(frame, cv::Scalar(0, 0, 255), 1);

    // If the Pose Tracker was properly initialized, find 3D pose information
    if (mmPoseTracker_.isValid()) {
        if (mmPoseTracker_.estimatePose(detected_markers)) {
            aruco::CvDrawingUtils::draw3dAxis(frame, camParams_, mmPoseTracker_.getRvec(), mmPoseTracker_.getTvec(), mmConfig_[0].getMarkerSize()*2);
            // std::map<int,cv::Mat> frame_pose_map;//set of poses and the frames they were detected
            // frame_pose_map.insert(std::make_pair(index, mmPoseTracker_.getRTMatrix()));
            // std::cout << "pose rt=" << mmPoseTracker_.getRvec() << " " << mmPoseTracker_.getTvec() << std::endl;

            sendtf(mmPoseTracker_.getRvec(), mmPoseTracker_.getTvec());
        }
    }

}

// ----------------------------------------------------------------------------

void ArucoLocalizer::cameraCallback(const sensor_msgs::ImageConstPtr& image, const sensor_msgs::CameraInfoConstPtr& cinfo) {
    cv_bridge::CvImagePtr cv_ptr;
    try {
        cv_ptr = cv_bridge::toCvCopy(image, sensor_msgs::image_encodings::BGR8);
    } catch (cv_bridge::Exception& e) {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    // update the camera model with the camera's intrinsic parameters
    cam_model_.fromCameraInfo(cinfo);

    // Configure the Pose Tracker if it has not been configured before
    if (!mmPoseTracker_.isValid() && mmConfig_.isExpressedInMeters()) {

        camParams_ = ros2arucoCamParams(cinfo);

        // Now, if the camera params have been ArUco-ified, set up the tracker
        if (camParams_.isValid())
            mmPoseTracker_.setParams(camParams_, mmConfig_);

    }

    // ==========================================================================
    // Process the incoming video frame

    // Get image as a regular Mat
    cv::Mat frame = cv_ptr->image;

    // Process the image and do ArUco localization on it
    processImage(frame);

    if (show_output_video_) {
        // Update GUI Window
        cv::imshow("detections", frame);
        cv::waitKey(1);
    }

    // ==========================================================================

    // Output modified video stream
    image_pub_.publish(cv_ptr->toImageMsg());
}

// ----------------------------------------------------------------------------

aruco::CameraParameters ArucoLocalizer::ros2arucoCamParams(const sensor_msgs::CameraInfoConstPtr& cinfo) {
    cv::Mat cameraMatrix(3, 3, CV_64FC1);
    cv::Mat distortionCoeff(4, 1, CV_64FC1);
    cv::Size size(cinfo->height, cinfo->width);

    // Make a regular 3x3 K matrix from CameraInfo
    for(int i=0; i<9; ++i)
        cameraMatrix.at<double>(i%3, i-(i%3)*3) = cinfo->K[i];

    // The ArUco library requires that there are only 4 distortion params (k1, k2, p1, p2, 0) 
    if (cinfo->D.size() == 4 || cinfo->D.size() == 5) {

        // Make a regular 4x1 D matrix from CameraInfo
        for(int i=0; i<4; ++i)
            distortionCoeff.at<double>(i, 0) = cinfo->D[i];

    } else {

        ROS_WARN("[aruco] Length of distortion matrix is not 4, assuming zero distortion.");
        for(int i=0; i<4; ++i)
            distortionCoeff.at<double>(i, 0) = 0;

    }

    return aruco::CameraParameters(cameraMatrix, distortionCoeff, size);
}

// ----------------------------------------------------------------------------

tf::Transform ArucoLocalizer::aruco2tf(const cv::Mat& rvec, const cv::Mat& tvec) {
    // convert rvec and tvec to doubles
    cv::Mat rvec64; rvec.convertTo(rvec64, CV_64FC1);
    cv::Mat tvec64; tvec.convertTo(tvec64, CV_64FC1);

    // Unpack Rodrigues paramaterization of the rotation
    cv::Mat rot(3, 3, CV_64FC1);
    cv::Rodrigues(rvec64, rot);

    cv::Mat rotate_to_ros(3, 3, CV_64FC1);
    // -1 0 0
    // 0 0 1
    // 0 1 0
    rotate_to_ros.at<double>(0,0) = -1.0;
    rotate_to_ros.at<double>(0,1) = 0.0;
    rotate_to_ros.at<double>(0,2) = 0.0;
    rotate_to_ros.at<double>(1,0) = 0.0;
    rotate_to_ros.at<double>(1,1) = 0.0;
    rotate_to_ros.at<double>(1,2) = 1.0;
    rotate_to_ros.at<double>(2,0) = 0.0;
    rotate_to_ros.at<double>(2,1) = 1.0;
    rotate_to_ros.at<double>(2,2) = 0.0;
    rot = rot*rotate_to_ros.t();

    tf::Matrix3x3 tf_rot(rot.at<double>(0,0), rot.at<double>(0,1), rot.at<double>(0,2),
                         rot.at<double>(1,0), rot.at<double>(1,1), rot.at<double>(1,2),
                         rot.at<double>(2,0), rot.at<double>(2,1), rot.at<double>(2,2));

    tf::Vector3 tf_orig(tvec64.at<double>(0), tvec64.at<double>(1), tvec64.at<double>(2));

    // this transform describes how to get to the ArUco marker map pose from the camera pose
    return tf::Transform(tf_rot, tf_orig);
}

// ----------------------------------------------------------------------------

}