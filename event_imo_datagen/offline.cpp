#include <vector>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <boost/filesystem.hpp>

#include <ros/ros.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <ros/package.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/point_types.h>
#include <pcl/common/common_headers.h>
#include <pcl/common/transforms.h>
#include <pcl_conversions/pcl_conversions.h>

#include <tf/transform_broadcaster.h>
#include <sensor_msgs/Range.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/image_encodings.h>
#include <nav_msgs/Odometry.h>
#include <message_filters/subscriber.h>
#include <visualization_msgs/MarkerArray.h>
#include <image_transport/image_transport.h>

#include <cv_bridge/cv_bridge.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <vicon/Subject.h>

#include <dvs_msgs/Event.h>
#include <dvs_msgs/EventArray.h>

#include "object.h"
#include "event_vis.h"


class DatasetConfig {
public:

    // Calibration matrix
    static float fx, fy, cx, cy, k1, k2, k3, k4;
    
    // Camera resolution
    static unsigned int res_x, res_y;

    // Camera center to vicon
    static float rr0, rp0, ry0, tx0, ty0, tz0;
    static tf::Transform cam_E;

    // Background to vicon
    static tf::Transform bg_E;

    // Other parameters
    static std::map<int, bool> enabled_objects;
    static std::string window_name;
    static bool modified;

    #define MAXVAL 1000
    static int value_rr, value_rp, value_ry;
    static int value_tx, value_ty, value_tz;

    static bool init(std::string dataset_folder, unsigned int rx, unsigned int ry) {
        DatasetConfig::res_x = rx;
        DatasetConfig::res_y = ry;
        bool ret = DatasetConfig::parse_config(dataset_folder + "/config.txt");
        ret &= DatasetConfig::read_cam_intr(dataset_folder + "/calib.txt");
        ret &= DatasetConfig::read_extr(dataset_folder + "/extrinsics.txt");
        return ret;
    }

    static void init_GUI() {
        DatasetConfig::window_name = "Calibration Control";
        cv::namedWindow(DatasetConfig::window_name, cv::WINDOW_AUTOSIZE);
        cv::createTrackbar("R", DatasetConfig::window_name, &value_rr, MAXVAL, on_trackbar);
        cv::createTrackbar("P", DatasetConfig::window_name, &value_rp, MAXVAL, on_trackbar);
        cv::createTrackbar("Y", DatasetConfig::window_name, &value_ry, MAXVAL, on_trackbar);
        cv::createTrackbar("x", DatasetConfig::window_name, &value_tx, MAXVAL, on_trackbar);
        cv::createTrackbar("y", DatasetConfig::window_name, &value_ty, MAXVAL, on_trackbar);
        cv::createTrackbar("z", DatasetConfig::window_name, &value_tz, MAXVAL, on_trackbar);
    }

private:
    static bool parse_config(std::string path) {
        std::ifstream ifs;
        ifs.open(path, std::ifstream::in);
        if (!ifs.is_open()) {
            std::cout << _red("Could not open configuration file at ")
                      << path << "!" << std::endl;
            return false;
        }

        std::cout << _blue("Opening configuration file: ")
                  << path  << std::endl;
        for (int i = 0; i < 3; ++i) {
            std::string line;
            std::getline(ifs, line);
            if (line.find("true") != std::string::npos) {
                std::cout << _blue("\tEnabling object ") << i + 1 << std::endl;
                enabled_objects[i + 1] = true;
            }
        }

        ifs.close();
        return true;
    }

    static bool read_cam_intr(std::string path) {
        std::ifstream ifs;
        ifs.open(path, std::ifstream::in);
        if (!ifs.is_open()) {
            std::cout << _red("Could not open camera intrinsic calibration file at ")
                      << path << "!" << std::endl;
            return false;
        }

        ifs >> fx >> fy >> cx >> cy;
        if (!ifs.good()) {
            std::cout << _red("Camera calibration read error:") << " Expected a file with a single line, containing "
                      << "fx fy cx cy {k1 k2 k3 k4} ({} are optional)" << std::endl;
            return false;
        }
        
        k1 = k2 = k3 = k4 = 0;
        ifs >> k1 >> k2 >> k3 >> k4;
        
        std::cout << _green("Read camera calibration: (fx fy cx cy {k1 k2 k3 k4}): ")
                  << fx << " " << fy << " " << cx << " " << cy << " "
                  << k1 << " " << k2 << " " << k3 << " " << k4 << std::endl;
        ifs.close();
        DatasetConfig::update_cam_calib();
        return true;
    }

    static void on_trackbar(int, void*) {
        DatasetConfig::modified = true;
        DatasetConfig::update_cam_calib();
    }

    static float normval(int val, int maxval, int normval) {
        return float(val - maxval / 2) / float(normval);
    }

    static bool read_extr(std::string path) {
        std::ifstream ifs;
        ifs.open(path, std::ifstream::in);
        if (!ifs.is_open()) {
            std::cout << _red("Could not open extrinsic calibration file at ")
                      << path << "!" << std::endl;
            return false;
        }

        ifs >> tx0 >> ty0 >> tz0 >> rr0 >> rp0 >> ry0;
        if (!ifs.good()) {
            std::cout << _red("Camera -> Vicon is suppposed to be in <x y z R P Y> format!") << std::endl;
            return false;
        }

        float bg_tx, bg_ty, bg_tz, bg_qw, bg_qx, bg_qy, bg_qz;
        ifs >> bg_tx >> bg_ty >> bg_tz >> bg_qw >> bg_qx >> bg_qy >> bg_qz;
        if (!ifs.good()) {
            std::cout << _red("Background -> Vicon is suppposed to be in <x y z Qw Qx Qy Qz> format!") << std::endl;
            return false;
        }

        ifs.close();

        tf::Vector3 T;
        tf::Quaternion Q(bg_qx, bg_qy, bg_qz, bg_qw);
        T.setValue(bg_tx, bg_ty, bg_tz);

        bg_E.setRotation(Q);
        bg_E.setOrigin(T);
        return true;
    }

public:
    static void update_cam_calib() {
        Eigen::Matrix4f Tm;
        Tm <<  0.0,    1.0,   0.0,  0.00,
              -1.0,    0.0,   0.0,  0.00,
               0.0,    0.0,   1.0,  0.00,
                 0,      0,     0,     1;
        Tm = Tm.inverse().eval();   

        float rr = rr0 + normval(value_rr, MAXVAL, MAXVAL * 10);
        float rp = rp0 + normval(value_rp, MAXVAL, MAXVAL * 10);
        float ry = ry0 + normval(value_ry, MAXVAL, MAXVAL * 10);
        float tx = tx0 + normval(value_tx, MAXVAL, MAXVAL * 10);
        float ty = ty0 + normval(value_ty, MAXVAL, MAXVAL * 10);
        float tz = tz0 + normval(value_tz, MAXVAL, MAXVAL * 10);

        tf::Transform E;
        tf::Vector3 T;
        tf::Quaternion q;
        q.setRPY(rr, rp, ry);
        T.setValue(tx, ty, tz);
        E.setRotation(q);
        E.setOrigin(T);
        cam_E = ViObject::mat2tf(Tm) * E;
    }
};

float DatasetConfig::fx, DatasetConfig::fy, DatasetConfig::cx, DatasetConfig::cy;
unsigned int DatasetConfig::res_x, DatasetConfig::res_y;
float DatasetConfig::k1, DatasetConfig::k2, DatasetConfig::k3, DatasetConfig::k4;
float DatasetConfig::rr0, DatasetConfig::rp0, DatasetConfig::ry0;
float DatasetConfig::tx0, DatasetConfig::ty0, DatasetConfig::tz0;
tf::Transform DatasetConfig::bg_E;
tf::Transform DatasetConfig::cam_E;
std::map<int, bool> DatasetConfig::enabled_objects;
std::string DatasetConfig::window_name;
int DatasetConfig::value_rr = MAXVAL / 2, DatasetConfig::value_rp = MAXVAL / 2, DatasetConfig::value_ry = MAXVAL / 2;
int DatasetConfig::value_tx = MAXVAL / 2, DatasetConfig::value_ty = MAXVAL / 2, DatasetConfig::value_tz = MAXVAL / 2;
bool DatasetConfig::modified = true;


// Service functions
class Pose {
public:
    ros::Time ts;
    tf::Transform pq;
    Pose(ros::Time ts_, tf::Transform pq_):
        ts(ts_), pq(pq_) {}
};


class Trajectory {
public:
    std::vector<Pose> poses;
    void add(ros::Time ts_, tf::Transform pq_) {
        this->poses.push_back(Pose(ts_, pq_));
    }

    size_t size() {return this->poses.size(); }
    inline auto begin() {return this->poses.begin(); }
    inline auto end()   {return this->poses.end(); }
    inline auto operator [] (size_t idx) {return this->poses[idx]; }

    bool check() {
        if (this->size() == 0) return true;
        auto prev_ts = this->poses[0].ts;
        for (auto &p : this->poses) {
            if (p.ts < prev_ts) return false;
            prev_ts = p.ts;
        }
        return true;
    }

    void subtract_time(ros::Time t) {
        for (auto &p : this->poses) p.ts = ros::Time((p.ts - t).toSec());
    }
};


template <class T> class Slice {
protected:
    T *data;
    std::pair<uint64_t, uint64_t> pair;
    size_t current_size;

public:
    typedef T value_type;

    Slice(T &vec, std::pair<uint64_t, uint64_t> p)
        : data(&vec), pair(p), current_size(p.second - p.first) {}

    size_t size() {return this->size; }
    auto begin() {return this->data->begin() + this->pair.first; }
    auto end()   {return this->data->begin() + this->pair.second; }
};


class DatasetFrame {
protected:
    static std::shared_ptr<StaticObject> background;
    static std::map<int, std::shared_ptr<ViObject>> clouds;
    static std::list<DatasetFrame*> visualization_list;
    static std::vector<Event>* event_array;

public:
    Pose cam_pose;
    std::map<int, Pose> obj_poses;
    unsigned long int frame_id;
    std::pair<uint64_t, uint64_t> event_slice_ids;

    cv::Mat img;
    cv::Mat depth;
    cv::Mat mask;

public:
    static void add_cloud(int id, std::shared_ptr<ViObject> cl) {
        DatasetFrame::clouds[id] = cl;
    }
    static void add_background(std::shared_ptr<StaticObject> bg) {
        DatasetFrame::background = bg;
        DatasetFrame::background->transform(DatasetConfig::bg_E);
    }
    static void set_event_array(std::vector<Event>* ea) {
        DatasetFrame::event_array = ea;
    }
    static void init_cloud(int id, const vicon::Subject& subject) {
        if (DatasetFrame::clouds.find(id) == DatasetFrame::clouds.end())
            return;
        clouds[id]->init_cloud_to_vicon_tf(subject);
    }
    static void visualization_spin() {
        std::map<DatasetFrame*, std::string> window_names;
        for (auto &frame_ptr : DatasetFrame::visualization_list) {
            window_names[frame_ptr] = "Frame " + std::to_string(frame_ptr->frame_id);
            cv::namedWindow(window_names[frame_ptr], cv::WINDOW_NORMAL);
        }

        DatasetConfig::modified = true;
        DatasetConfig::init_GUI();
        bool show_mask = false;

        int code = 0; // Key code
        while (code != 27) {
            code = cv::waitKey(1);
            if (code == 32) {
                show_mask = !show_mask;
                DatasetConfig::modified = true;
            }

            if (!DatasetConfig::modified) continue;
            DatasetConfig::modified = false;

            for (auto &window : window_names) {
                window.first->generate();
                cv::Mat img;
                
                cv::Mat img_pr;
                if (DatasetFrame::event_array != nullptr) {
                    auto ev_slice = Slice<std::vector<Event>>(*DatasetFrame::event_array, 
                                                              window.first->event_slice_ids);
                    img_pr = EventFile::projection_img(&ev_slice, 1);
                }

                if (!show_mask) {
                    auto depth_img = window.first->depth;
                    img = cv::Mat(depth_img.rows, depth_img.cols, CV_8UC3, cv::Scalar(0, 0, 0));
                    cv::normalize(depth_img, depth_img, 0, 255, cv::NORM_MINMAX);
                    cv::divide(8000.0, depth_img, depth_img);
                    for(int i = 0; i < depth_img.rows; ++i) {
                        for (int j = 0; j < depth_img.cols; ++j) {
                            img.at<cv::Vec3b>(i, j)[0] = depth_img.at<float>(i, j);
                            img.at<cv::Vec3b>(i, j)[1] = depth_img.at<float>(i, j);
                            img.at<cv::Vec3b>(i, j)[2] = depth_img.at<float>(i, j);

                            if (DatasetFrame::event_array != nullptr)
                                img.at<cv::Vec3b>(i, j)[2] = img_pr.at<uint8_t>(i, j);
                        }
                    }
                } else {
                    auto mask_img = window.first->mask;
                    auto rgb_img  = window.first->img;
                    img = cv::Mat(mask_img.rows, mask_img.cols, CV_8UC3, cv::Scalar(0, 0, 0));
                    for(int i = 0; i < mask_img.rows; ++i) {
                        for (int j = 0; j < mask_img.cols; ++j) {
                            int id = std::round(mask_img.at<uint8_t>(i, j));
                            auto color = EventFile::id2rgb(id);
                            if (rgb_img.rows == mask_img.rows && rgb_img.cols == mask_img.cols) {
                                img.at<cv::Vec3b>(i, j) = rgb_img.at<cv::Vec3b>(i, j);
                                if (id > 0) {
                                    img.at<cv::Vec3b>(i, j) = rgb_img.at<cv::Vec3b>(i, j) * 0.5 + color * 0.5;
                                }
                            } else {
                                img.at<cv::Vec3b>(i, j) = color;
                            }
                        }
                    }
                }

                cv::imshow(window.second, img);
            }
        }

        cv::destroyAllWindows();
        DatasetFrame::visualization_list.clear();
    }

    // ---------
    DatasetFrame(Pose cam_p, unsigned long int fid) 
        : cam_pose(cam_p), frame_id(fid), event_slice_ids(0, 0),
          depth(DatasetConfig::res_x, DatasetConfig::res_y, CV_32F, cv::Scalar(0)),
          mask(DatasetConfig::res_x, DatasetConfig::res_y, CV_8U, cv::Scalar(0))
        {}

    void add_object_pos(int id, Pose obj_p) {
        this->obj_poses.insert(std::make_pair(id, obj_p));
    }

    void add_event_slice_ids(uint64_t event_low, uint64_t event_high) {
        this->event_slice_ids = std::make_pair(event_low, event_high);
    }

    void add_img(cv::Mat &img_) {
        this->img = img_;
    }

    void show() {
        DatasetFrame::visualization_list.push_back(this);
    }

    // Generate frame
    void generate() {
        this->depth = cv::Scalar(0);
        this->mask  = cv::Scalar(0);
        
        DatasetConfig::update_cam_calib();

        auto cam_tf = this->cam_pose.pq * DatasetConfig::cam_E;
        if (DatasetFrame::background != nullptr) {
            auto cl = DatasetFrame::background->transform_to_camframe(cam_tf);
            this->project_cloud(cl, 0);
        }

        for (auto &obj : DatasetFrame::clouds) {
            if (this->obj_poses.find(obj.first) == this->obj_poses.end()) {
                std::cout << _yellow("Warning! ") << "No pose for object "
                          << obj.first << " frame id " << this->frame_id << std::endl;
                continue;
            }
            auto cl = obj.second->transform_to_camframe(cam_tf,
                                     this->obj_poses.at(obj.first).pq);
            this->project_cloud(cl, obj.first);
        }
    }

protected:
    void project_point(pcl::PointXYZRGB p, int &u, int &v) {
        u = 0; v = 0;
        if (p.x < 0.00001)
            return;

        float x_ = p.z / p.x;
        float y_ = p.y / p.x;
        float r2 = x_ * x_ + y_ * y_;
        float r4 = r2 * r2;
        float r6 = r2 * r2 * r2;
        float dist = (1.0 + DatasetConfig::k1 * r2 + DatasetConfig::k2 * r4 +
                            DatasetConfig::k3 * r6) / (1 + DatasetConfig::k4 * r2);
        float x__ = x_ * dist;
        float y__ = y_ * dist;

        u = DatasetConfig::fx * x__ + DatasetConfig::cx;
        v = DatasetConfig::fy * y__ + DatasetConfig::cy;
    }

    void project_cloud(auto cl, int oid) {
        if (cl->size() == 0)
            return;

        for (auto &p: *cl) {
            float rng = p.x;
            if (rng < 0.001)
                continue;
            
            auto cols = this->depth.cols;
            auto rows = this->depth.rows;

            int u = 0, v = 0;
            this->project_point(p, u, v);
            if (u < 0 || v < 0 || v >= cols || u >= rows)
                continue;
     
            int patch_size = int(1.0 / rng);
            
            if (oid == 0)
                patch_size = int(5.0 / rng);

            int u_lo = std::max(u - patch_size / 2, 0);
            int u_hi = std::min(u + patch_size / 2, rows - 1);
            int v_lo = std::max(v - patch_size / 2, 0);
            int v_hi = std::min(v + patch_size / 2, cols - 1);

            for (int ii = u_lo; ii <= u_hi; ++ii) {
                for (int jj = v_lo; jj <= v_hi; ++jj) {
                    float base_rng = this->depth.at<float>(rows - ii - 1, cols - jj - 1);
                    if (base_rng > rng || base_rng < 0.001) {
                        this->depth.at<float>(rows - ii - 1, cols - jj - 1) = rng;
                        this->mask.at<uint8_t>(rows - ii - 1, cols - jj - 1) = oid;
                    }
                }
            }
        }
    }
};

std::shared_ptr<StaticObject> DatasetFrame::background;
std::map<int, std::shared_ptr<ViObject>> DatasetFrame::clouds;
std::list<DatasetFrame*> DatasetFrame::visualization_list;
std::vector<Event>* DatasetFrame::event_array = nullptr;


int main (int argc, char** argv) {

    // Initialize ROS
    std::string node_name = "event_imo_offline";
    ros::init (argc, argv, node_name);
    ros::NodeHandle nh;
 
    std::string dataset_folder = "";
    if (!nh.getParam(node_name + "/folder", dataset_folder)) {
        std::cerr << "No dataset folder specified!" << std::endl;
        return -1;
    }

    float FPS = 40.0;
    int traj_smoothing = 1;
    bool through_mode = false;
    if (!nh.getParam(node_name + "/fps", FPS)) FPS = 40;
    if (!nh.getParam(node_name + "/smoothing", traj_smoothing)) traj_smoothing = 1;
    if (!nh.getParam(node_name + "/numbering", through_mode)) through_mode = false;

    float slice_width = 0.1;
    if (!nh.getParam(node_name + "/slice_width", slice_width)) slice_width = 0.03;

    float time_bias = 0.0;
    if (!nh.getParam(node_name + "/time_bias", time_bias)) time_bias = 0.0;

    bool no_background = false;
    if (!nh.getParam(node_name + "/no_bg", no_background)) no_background = false;
    
    bool with_images = false;
    if (!nh.getParam(node_name + "/with_images", with_images)) with_images = false;
    else std::cout << _yellow("With 'with_images' option, the datased will be generated at image framerate.") << std::endl;

    int res_x = 260, res_y = 346;
    if (!nh.getParam(node_name + "/res_x", res_x)) res_x = 260;
    if (!nh.getParam(node_name + "/res_y", res_y)) res_y = 346;

    // -- camera topics
    std::string cam_pose_topic = "", event_topic = "", img_topic = "";
    std::map<int, std::string> obj_pose_topics;
    if (!nh.getParam(node_name + "/cam_pose_topic", cam_pose_topic)) cam_pose_topic = "/vicon/DVS346";
    if (!nh.getParam(node_name + "/event_topic", event_topic)) event_topic = "/dvs/events";
    if (!nh.getParam(node_name + "/img_topic", img_topic)) img_topic = "/dvs/image_raw";

    // -- parse the dataset folder
    std::string bag_name = boost::filesystem::path(dataset_folder).stem().string();
    if (bag_name == ".") {
        bag_name = boost::filesystem::path(dataset_folder).parent_path().stem().string();
    }

    bag_name = boost::filesystem::path(dataset_folder).append(bag_name + ".bag").string();
    std::cout << _blue("Procesing bag file: ") << bag_name << std::endl;

    rosbag::Bag bag;
    bag.open(bag_name, rosbag::bagmode::Read);
    rosbag::View view(bag);
    std::vector<const rosbag::ConnectionInfo *> connection_infos = view.getConnections();
    
    std::cout << std::endl << "Topics available:" << std::endl;
    for (auto &info : connection_infos) {
        std::cout << "\t" << info->topic << std::endl;
    }

    // Read datasset configuration files
    if (!DatasetConfig::init(dataset_folder, (unsigned int)res_x, (unsigned int)res_y))
        return -1;

    // Load 3D models
    std::string path_to_self = ros::package::getPath("event_imo_datagen");
    
    if (!no_background)
        DatasetFrame::add_background(std::make_shared<StaticObject>(path_to_self + "/objects/room"));
    if (DatasetConfig::enabled_objects.find(1) != DatasetConfig::enabled_objects.end()) {
        DatasetFrame::add_cloud(1, std::make_shared<ViObject>(nh, path_to_self + "/objects/toy_car", 1));
        if (!nh.getParam(node_name + "/obj_pose_topic_0", obj_pose_topics[1])) obj_pose_topics[1] = "/vicon/Object_1";
    }

    if (DatasetConfig::enabled_objects.find(2) != DatasetConfig::enabled_objects.end()) {
        DatasetFrame::add_cloud(2, std::make_shared<ViObject>(nh, path_to_self + "/objects/toy_plane", 2));
        if (!nh.getParam(node_name + "/obj_pose_topic_1", obj_pose_topics[2])) obj_pose_topics[2] = "/vicon/Object_2";
    }

    if (DatasetConfig::enabled_objects.find(3) != DatasetConfig::enabled_objects.end()) {
        DatasetFrame::add_cloud(3, std::make_shared<ViObject>(nh, path_to_self + "/objects/cup", 3));
        if (!nh.getParam(node_name + "/obj_pose_topic_2", obj_pose_topics[3])) obj_pose_topics[3] = "/vicon/Object_3";
    }

    // Extract topics from bag
    Trajectory cam_tj;
    std::map<int, Trajectory> obj_tjs;
    std::map<int, vicon::Subject> obj_cloud_to_vicon_tf;
    std::vector<cv::Mat> images;
    std::vector<ros::Time> image_ts;
    uint64_t n_events = 0;
    for (auto &m : view) {
        if (m.getTopic() == cam_pose_topic) {
            auto msg = m.instantiate<vicon::Subject>();  
            if (msg == NULL) continue;
            cam_tj.add(msg->header.stamp, ViObject::subject2tf(*msg));
            continue;
        }

        for (auto &p : obj_pose_topics) {
            if (m.getTopic() != p.second) continue;
            auto msg = m.instantiate<vicon::Subject>();
            if (msg == NULL) break;
            if (msg->occluded) break;
            obj_tjs[p.first].add(msg->header.stamp, ViObject::subject2tf(*msg));
            obj_cloud_to_vicon_tf[p.first] = *msg;
            break;
        }

        if (m.getTopic() == event_topic) {
            auto msg = m.instantiate<dvs_msgs::EventArray>();  
            if (msg == NULL) continue;
            n_events += msg->events.size();
        }

        if (with_images && (m.getTopic() == img_topic)) {
            auto msg = m.instantiate<sensor_msgs::Image>();  
            images.push_back(cv_bridge::toCvShare(msg, "bgr8")->image);
            image_ts.push_back(msg->header.stamp);
        }
    }

    if (with_images && images.size() == 0) {
        std::cout << _red("No images found! Reverting 'with_images' to 'false'") << std::endl;
        with_images = false;
    }

    std::vector<Event> event_array(n_events);
    uint64_t id = 0;
    ros::Time first_event_ts;
    ros::Time last_event_ts;
    for (auto &m : view) {
        if (m.getTopic() == event_topic) {
            auto msg = m.instantiate<dvs_msgs::EventArray>();  
            if (msg == NULL) continue;
            for (auto &e : msg->events) {
                if (id == 0) {
                    first_event_ts = e.ts;
                    last_event_ts = e.ts;
                } else {
                    if (e.ts < last_event_ts) {
                        std::cout << _red("Events are not sorted! ") 
                                  << id << ": " << last_event_ts << " -> " 
                                  << e.ts << std::endl;
                    }
                    last_event_ts = e.ts;
                }
                
                auto ts = (e.ts - first_event_ts).toNSec();
                event_array[id] = Event(e.y, e.x, ts, e.polarity ? 1 : 0);
                id ++;
            }
        }
    }

    std::cout << _green("Read ") << n_events << _green(" events") << std::endl;
    std::cout << std::endl << _green("Read ") << cam_tj.size() << _green(" camera poses and ") << std::endl;
    for (auto &obj_tj : obj_tjs) {
        if (obj_tj.second.size() == 0) continue;
        std::cout << "\t" << obj_tj.second.size() << _blue(" poses for object ") << obj_tj.first << std::endl;
        if (!obj_tj.second.check()) {
            std::cout << "\t\t" << _red("Check failed!") << std::endl;
        }
        DatasetFrame::init_cloud(obj_tj.first, obj_cloud_to_vicon_tf[obj_tj.first]);
    }
    DatasetFrame::set_event_array(&event_array);

    // Remove time offset from poses
    auto time_offset = cam_tj[0].ts;
    for (auto &obj_tj : obj_tjs)
        if (obj_tj.second.size() > 0 && obj_tj.second[0].ts < time_offset) time_offset = obj_tj.second[0].ts;
    if (time_offset < first_event_ts)
        std::cout << _yellow("Warning: ") << "event time offset is not the smallest (" << first_event_ts 
                  << " vs " << time_offset << ")" << std::endl;
    time_offset = first_event_ts; // align with events
    cam_tj.subtract_time(time_offset + ros::Duration(time_bias));
    for (auto &obj_tj : obj_tjs)
        obj_tj.second.subtract_time(time_offset + ros::Duration(time_bias));
    for (uint64_t i = 0; i < image_ts.size(); ++i)
        image_ts[i] = ros::Time((image_ts[i] - time_offset - ros::Duration(time_bias)).toSec());
    std::cout << std::endl << "Removing time offset: " << _green(std::to_string(time_offset.toSec()))
              << std::endl << std::endl;    

    // Align the timestamps
    double start_ts = 0.2;
    unsigned long int frame_id_through = 0, frame_id_real = 0;
    double dt = 1.0 / FPS;
    long int cam_tj_id = 0;
    std::map<int, long int> obj_tj_ids;
    std::vector<DatasetFrame> frames;
    uint64_t event_low = 0, event_high = 0;
    while (true) {
        if (with_images) {
            if (frame_id_real >= image_ts.size()) break;
            start_ts = image_ts[frame_id_real].toSec();
        }

        while (cam_tj_id < cam_tj.size() && cam_tj[cam_tj_id].ts.toSec() < start_ts) cam_tj_id ++;
        for (auto &obj_tj : obj_tjs)
            while (obj_tj_ids[obj_tj.first] < obj_tj.second.size()
                   && obj_tj.second[obj_tj_ids[obj_tj.first]].ts.toSec() < start_ts) obj_tj_ids[obj_tj.first] ++;

        start_ts += dt;

        bool done = false;
        if (cam_tj_id >= cam_tj.size()) done = true;
        for (auto &obj_tj : obj_tjs)
            if (obj_tj.second.size() > 0 && obj_tj_ids[obj_tj.first] >= obj_tj.second.size()) done = true;
        if (done) break;

        auto ref_ts = (with_images ? image_ts[frame_id_real].toSec() : cam_tj[cam_tj_id].ts.toSec());
        uint64_t ts_low  = (ref_ts < slice_width) ? 0 : (ref_ts - slice_width / 2.0) * 1000000000;
        uint64_t ts_high = (ref_ts + slice_width / 2.0) * 1000000000;
        while (event_low  < event_array.size() && event_array[event_low].timestamp  < ts_low)  event_low ++;
        while (event_high < event_array.size() && event_array[event_high].timestamp < ts_high) event_high ++;

        double max_ts_err = 0.0;
        for (auto &obj_tj : obj_tjs) {
            if (obj_tj.second.size() == 0) continue;
            double ts_err = std::fabs(ref_ts - obj_tj.second[obj_tj_ids[obj_tj.first]].ts.toSec());
            if (ts_err > max_ts_err) max_ts_err = ts_err;
        }

        frame_id_real ++;
        if (max_ts_err > 0.01) {
            std::cout << _red("Trajectory timestamp misalignment: ") << max_ts_err << " skipping..." << std::endl;
            continue;
        }
        frame_id_through ++;

        DatasetFrame frame(cam_tj[cam_tj_id], through_mode ? frame_id_through : frame_id_real);
        frame.add_event_slice_ids(event_low, event_high);
        if (with_images) frame.add_img(images[frame_id_real]);
        std::cout << (through_mode ? frame_id_through : frame_id_real) << ": " << cam_tj[cam_tj_id].ts << " (" << cam_tj_id << ")";
        for (auto &obj_tj : obj_tjs) {
            if (obj_tj.second.size() == 0) continue;
            std::cout << " " << obj_tj.second[obj_tj_ids[obj_tj.first]].ts << " (" << obj_tj_ids[obj_tj.first] << ")";
            frame.add_object_pos(obj_tj.first, obj_tj.second[obj_tj_ids[obj_tj.first]]);
        }
        std::cout << std::endl;
        frames.push_back(frame);
    }

    std::cout << _blue("\nTimestamp alignment done") << std::endl;
    std::cout << "\tDataset contains " << frames.size() << " frames" << std::endl;


    // Visualization
    int nframes = 3;
    for (int i = 0; i < frames.size(); i += frames.size() / nframes) {
        frames[i].show();
    }

    DatasetFrame::visualization_spin();

    return 0;
}