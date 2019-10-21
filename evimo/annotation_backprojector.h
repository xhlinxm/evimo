#include <thread>

#include <pcl/io/pcd_io.h>
#include <pcl/common/common_headers.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/features/normal_3d.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/registration/transformation_estimation_svd.h>
#include <unordered_map>

#include <dataset.h>
#include <dataset_frame.h>

#ifndef ANNOTATION_BACKPROJECTOR_H
#define ANNOTATION_BACKPROJECTOR_H

class Backprojector {
protected:
    double timestamp;
    double window_size;
    std::vector<DatasetFrame> frames;

    pcl::visualization::PCLVisualizer::Ptr viewer;
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr event_pc, event_pc_roi, mask_pc;
    pcl::KdTreeFLANN<pcl::PointXYZRGB> epc_kdtree;

    std::unordered_map<int, std::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB>>> mask_pointclouds;
    std::unordered_map<int, std::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB>>> roi_pointclouds;

public:
    Backprojector(double timestamp, double window_size, double framerate)
        : timestamp(timestamp), window_size(window_size)
        , event_pc(new pcl::PointCloud<pcl::PointXYZRGB>)
        , event_pc_roi(new pcl::PointCloud<pcl::PointXYZRGB>)
        , mask_pc(new pcl::PointCloud<pcl::PointXYZRGB>) {

        if (this->timestamp < 0 || this->window_size < 0) {
            this->timestamp = (Dataset::event_array[Dataset::event_array.size() - 1].get_ts_sec() + Dataset::event_array[0].get_ts_sec()) / 2.0;
            this->window_size = (Dataset::event_array[Dataset::event_array.size() - 1].get_ts_sec() - Dataset::event_array[0].get_ts_sec());
        }

        std::vector<double> ts_arr;
        if (framerate > 0) {
            ts_arr.reserve(1.2 * this->window_size / framerate);
            for (double ts = std::max(0.0, timestamp - window_size / 2.0);
                ts < timestamp + window_size / 2.0; ts += 1.0 / framerate) {
                ts_arr.push_back(ts);
            }
        } else {
            ts_arr.reserve(Dataset::cam_tj.size());
            for (uint64_t i = 0; i < Dataset::cam_tj.size(); ++i) {
                ts_arr.push_back(Dataset::cam_tj[i].ts.toSec());
            }
        }

        uint32_t i = 0;
        uint64_t last_cam_pos_id = 0;
        std::pair<uint64_t, uint64_t> last_event_slice_ids(0, 0);
        for (auto &ts : ts_arr) {
            frames.emplace_back(last_cam_pos_id, ts, i);
            auto &frame = frames.back();
            last_cam_pos_id = frame.cam_pose_id;

            for (auto &obj_tj : Dataset::obj_tjs) {
                frame.add_object_pos_id(obj_tj.first, frame.cam_pose_id);
            }

            frame.add_event_slice_ids(last_event_slice_ids.first, last_event_slice_ids.second);
            last_event_slice_ids = frame.event_slice_ids;

            i += 1;
        }
    }

    pcl::PCLPointCloud2 with_normals(pcl::PointCloud<pcl::PointXYZRGB> &in_, float k=0, float r=0.025) {
        pcl::PCLPointCloud2 output_cloud;
        pcl::toPCLPointCloud2 (in_, output_cloud);

        pcl::PointCloud<pcl::PointXYZRGB>::Ptr in (new pcl::PointCloud<pcl::PointXYZRGB>);
        pcl::fromPCLPointCloud2 (output_cloud, *in);

        pcl::PointCloud<pcl::Normal> normals;
        pcl::NormalEstimation<pcl::PointXYZRGB, pcl::Normal> ne;
        ne.setInputCloud(in);
        ne.setSearchMethod(pcl::search::KdTree<pcl::PointXYZRGB>::Ptr(new pcl::search::KdTree<pcl::PointXYZRGB>));
        ne.setKSearch(k);
        ne.setRadiusSearch(r);
        ne.compute(normals);

        pcl::PCLPointCloud2 output_cloud2;
        pcl::PCLPointCloud2 output_normals;
        pcl::toPCLPointCloud2 (normals, output_normals);
        pcl::concatenateFields (output_cloud, output_normals, output_cloud2);
        return output_cloud2;
    }

    void save_clouds(std::string dir) {
        std::cout << "Saving clouds in " << dir << std::endl;

        std::cout << "Generating cloud" << std::endl;
        this->refresh_ec();

        std::cout << "Generating gt" << std::endl;
        this->generate();

        pcl::PLYWriter w;
        w.writeBinary(dir + "/raw_cloud.ply", this->with_normals(*this->event_pc));

        for (auto &oid : this->mask_pointclouds) {
            w.writeBinary(dir + "/mask_cloud_" + std::to_string(oid.first) + ".ply", this->with_normals(*oid.second));
        }
        for (auto &oid : this->roi_pointclouds) {
            w.writeBinary(dir + "/roi_cloud_" + std::to_string(oid.first) + ".ply", this->with_normals(*oid.second));
        }
    }

    // Conver timestamp to z coordinate
    double ts_to_z(double ts) {
        return (ts - (this->timestamp - this->window_size / 2.0)) * 2.0;
    }

    void refresh_ec() {
        this->event_pc->clear();
        auto e_slice = TimeSlice(Dataset::event_array,
          std::make_pair(std::max(0.0, this->timestamp - this->window_size / 2.0), this->timestamp + this->window_size / 2.0),
          std::make_pair(this->frames.front().event_slice_ids.first, this->frames.back().event_slice_ids.second));
        for (auto &e : e_slice) {
            pcl::PointXYZRGB p;
            p.x = float(e.get_x()) / 200.0; p.y = float(e.get_y()) / 200.0f; p.z = this->ts_to_z(e.get_ts_sec());
            uint32_t rgb = (static_cast<uint32_t>(0) << 16 |
                            static_cast<uint32_t>(20) << 8 |
                            static_cast<uint32_t>(255));
            p.rgb = *reinterpret_cast<float*>(&rgb);
            this->event_pc->push_back(p);
        }

        pcl::RadiusOutlierRemoval<pcl::PointXYZRGB> outrem;
        outrem.setInputCloud(this->event_pc);
        outrem.setRadiusSearch(5.0 / 200.0);
        outrem.setMinNeighborsInRadius(10);
        outrem.filter (*this->event_pc);

        this->epc_kdtree.setInputCloud(this->event_pc);

        if (this->viewer) {
            viewer->removePointCloud("event cloud");
            pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> ec_rgb(this->event_pc);
            viewer->addPointCloud<pcl::PointXYZRGB>(this->event_pc, ec_rgb, "event cloud");
            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "event cloud");
        }
    }

    void refresh_ec_roi() {
        this->event_pc_roi->clear();
        this->roi_pointclouds.clear();

        std::vector<int> pointIdxRadiusSearch;
        std::vector<float> pointRadiusSquaredDistance;

        for (auto &oid : this->mask_pointclouds) {
            std::set<int> idxes;
            this->roi_pointclouds[oid.first] = std::make_shared<pcl::PointCloud<pcl::PointXYZRGB>>();
            for (auto &p : *oid.second) {
                this->epc_kdtree.radiusSearch(p, 3.0 / 200.0, pointIdxRadiusSearch, pointRadiusSquaredDistance);
                for (auto &idx : pointIdxRadiusSearch) {
                    if (idxes.find(idx) != idxes.end())
                        continue;

                    idxes.insert(idx);
                    auto p_ = this->event_pc->points[idx];
                    p_.rgb = p.rgb;
                    this->roi_pointclouds[oid.first]->push_back(p_);
                    this->event_pc_roi->push_back(p_);
                }
            }
        }

        if (this->viewer) {
            viewer->removePointCloud("event cloud roi");
            pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> ec_rgb(this->event_pc_roi);
            viewer->addPointCloud<pcl::PointXYZRGB>(this->event_pc_roi, ec_rgb, "event cloud roi");
            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "event cloud roi");
        }
    }

    double score() {
        std::vector<int> pointIdxRadiusSearch(1);
        std::vector<float> pointRadiusSquaredDistance(1);

        double cnt = 0.0;
        double rng = 0.0;
        for (auto &p : *this->mask_pc) {
            this->epc_kdtree.nearestKSearch(p, 1, pointIdxRadiusSearch, pointRadiusSquaredDistance);
            if (pointRadiusSquaredDistance[0] > 4.0 / 200.0) continue;

            cnt += 1;
            rng += pointRadiusSquaredDistance[0];
        }

        return (cnt < 1) ? 0 : rng / cnt;
    }

    double inverse_score() {
        pcl::KdTreeFLANN<pcl::PointXYZRGB> kdtree;
        kdtree.setInputCloud(this->mask_pc);

        std::vector<int> pointIdxRadiusSearch(1);
        std::vector<float> pointRadiusSquaredDistance(1);

        double cnt = 0.0;
        double rng = 0.0;
        for (auto &p : *this->event_pc_roi) {
            kdtree.nearestKSearch(p, 1, pointIdxRadiusSearch, pointRadiusSquaredDistance);
            if (pointRadiusSquaredDistance[0] > 4.0 / 200.0) continue;

            cnt += 1;
            rng += pointRadiusSquaredDistance[0];
        }

        return (cnt < 1) ? 0 : rng / cnt;
    }

    void minimization_step() {
        pcl::PointCloud<pcl::PointXYZ>::Ptr source(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PointCloud<pcl::PointXYZ>::Ptr target(new pcl::PointCloud<pcl::PointXYZ>);

        pcl::KdTreeFLANN<pcl::PointXYZRGB> kdtree;
        kdtree.setInputCloud(this->mask_pc);

        std::cout << "Score (before): " << this->score() << "\t" << this->inverse_score() << "\n";
        auto initial = this->inverse_score();

        //std::valarray<float> T = {0, 0, 0};
        //std::valarray<float> R = {0, 0, 0};

        Dataset::set_sliders(0.001, 0, 0, 0, 0, 0);
        generate();

        if (this->inverse_score() > initial) {
            Dataset::set_sliders(-0.002, 0, 0, 0, 0, 0);
            generate();
        }

        if (this->inverse_score() > initial) {
            Dataset::set_sliders(0.001, 0, 0, 0, 0, 0);
            generate();
        }

        Dataset::set_sliders(0, 0.001, 0, 0, 0, 0);
        generate();

        if (this->inverse_score() > initial) {
            Dataset::set_sliders(0, -0.002, 0, 0, 0, 0);
            generate();
        }

        if (this->inverse_score() > initial) {
            Dataset::set_sliders(0, 0.001, 0, 0, 0, 0);
            generate();
        }

        std::cout << "Score (after): " << this->score() << "\t" << this->inverse_score() << "\n";

//        Dataset::set_sliders(0.0, 0.001, 0, 0, 0, 0);
//        if (this->inverse_score() < initial)
//            Dataset::set_sliders(0.0, -0.002, 0, 0, 0, 0);

/*
        std::vector<int> pointIdxRadiusSearch(1);
        std::vector<float> pointRadiusSquaredDistance(1);

        for (auto &p : *this->event_pc_roi) {
            kdtree.nearestKSearch(p, 1, pointIdxRadiusSearch, pointRadiusSquaredDistance);
            if (pointRadiusSquaredDistance[0] > 4.0 / 200.0) continue;
            auto &p_mask = this->mask_pc->at(pointIdxRadiusSearch[0]);

            pcl::PointXYZ p_src, p_tgt;
            DatasetFrame::unproject_point(p_src, p.x * 200, p.y * 200);
            DatasetFrame::unproject_point(p_tgt, p_mask.x * 200, p_mask.y * 200);

            //std::cout << "(" << p_src.x << ";\t" << p_src.y << ")\t->\t"
            //          << "(" << p_tgt.x << ";\t" << p_tgt.y << ")\n";

            source->push_back(p_src);
            target->push_back(p_tgt);
        }

        Eigen::Matrix4f SVD;
        const pcl::registration::TransformationEstimationSVD<pcl::PointXYZ, pcl::PointXYZ> trans_est_svd;
        trans_est_svd.estimateRigidTransformation(*source, *target, SVD);
        auto pose = Pose(ros::Time(0), ViObject::mat2tf(SVD));

        auto T = pose.getT();
        auto R = pose.getR();

        std::cout << "T = " << T[0] << "\t" << T[1] << "\t" << T[2] << "\n";
        std::cout << "R = " << R[0] << "\t" << R[1] << "\t" << R[2] << "\n\n";

        Dataset::set_sliders(T[0], T[1], T[2], R[0], R[1], R[2]);
        */
    }

    void generate() {
        this->mask_pc->clear();
        this->mask_pointclouds.clear();

        // Mask trace cloud
        for (auto &f : this->frames) f.generate_async();
        for (auto &f : this->frames) f.join();

        for (auto &f : this->frames) {
            auto cl = this->mask_to_cloud(f.mask, this->ts_to_z(f.get_timestamp()));
            for (auto &oid : cl) {
                if (!this->mask_pointclouds[oid.first]) this->mask_pointclouds[oid.first] = std::make_shared<pcl::PointCloud<pcl::PointXYZRGB>>();
                *this->mask_pointclouds[oid.first] += *oid.second;
                *this->mask_pc += *oid.second;
            }
        }

        refresh_ec_roi();
        if (this->viewer) {
            viewer->removePointCloud("mask cloud");
            pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> m_rgb(this->mask_pc);
            viewer->addPointCloud<pcl::PointXYZRGB>(this->mask_pc, m_rgb, "mask cloud");
            viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, "mask cloud");
        }
    }

    std::unordered_map<int, std::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB>>>
    mask_to_cloud(cv::Mat mask, double z) {
        std::unordered_map<int, std::shared_ptr<pcl::PointCloud<pcl::PointXYZRGB>>> ret;

        auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
        std::vector<std::vector<uint8_t>> colors {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};

        cv::Mat boundary, dil;
        cv::dilate(mask, dil, kernel);
        boundary = dil - mask;

        auto cols = mask.cols;
        auto rows = mask.rows;

        for (uint32_t i = 0; i < rows; ++i) {
            for (uint32_t j = 0; j < cols; ++j) {
                if (boundary.at<uint8_t>(i, j) == 0) continue;
                auto oid = boundary.at<uint8_t>(i, j);

                pcl::PointXYZRGB p;
                p.x = float(i) / 200; p.y = float(j) / 200; p.z = z;
                auto clr = colors[oid % colors.size()];
                uint32_t rgb = (static_cast<uint32_t>(clr[0]) << 16 |
                                static_cast<uint32_t>(clr[1]) << 8    |
                                static_cast<uint32_t>(clr[2]));
                p.rgb = *reinterpret_cast<float*>(&rgb);
                if (!ret[oid]) ret[oid] = std::make_shared<pcl::PointCloud<pcl::PointXYZRGB>>();
                ret[oid]->push_back(p);
            }
        }

        return ret;
    }

    // Visualization
    void visualize_parallel() {
        for (auto &frame : this->frames) {
            frame.show();
        }

        DatasetFrame::visualization_spin();
    }

    void initViewer() {
        this->viewer = pcl::visualization::PCLVisualizer::Ptr(new pcl::visualization::PCLVisualizer("3D Viewer"));
        this->viewer->setBackgroundColor (0.9, 0.9, 0.9);
        this->viewer->addCoordinateSystem (1.0);
        this->viewer->initCameraParameters();
        this->viewer->registerKeyboardCallback(&Backprojector::keyboard_handler, *this);
        this->refresh_ec();

        //this->generate();

        //using namespace std::chrono_literals;
        //while (!viewer->wasStopped()) {
        //this->maybeViewerSpinOnce();
        //    std::this_thread::sleep_for(100ms);
        //}

        //this->viewer = pcl::visualization::PCLVisualizer::Ptr();
    }

    void maybeViewerSpinOnce() {
        if (!this->viewer) return;
        this->viewer->spinOnce(100);
    }

    bool show_mask   = false;
    bool show_ec     = true;
    bool show_ec_roi = false;
    void keyboard_handler(const pcl::visualization::KeyboardEvent &event,
                          void* viewer_void) {
        auto key = event.getKeySym();
        if (key == "1") {
            show_mask = !show_mask;
            if (show_mask) {
                pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> m_rgb(this->mask_pc);
                this->viewer->addPointCloud<pcl::PointXYZRGB>(this->mask_pc, m_rgb, "mask cloud");
                this->viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, "mask cloud");
            } else {
                this->viewer->removePointCloud("mask cloud");
            }
        }

        if (key == "2") {
            show_ec = !show_ec;
            show_ec_roi = !show_ec;

            if (show_ec) {
                pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> ec_rgb(this->event_pc);
                viewer->addPointCloud<pcl::PointXYZRGB>(this->event_pc, ec_rgb, "event cloud");
                viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1, "event cloud");
            } else {
                viewer->removePointCloud("event cloud");
            }

            if (show_ec_roi) {
                pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> ec_rgb(this->event_pc_roi);
                viewer->addPointCloud<pcl::PointXYZRGB>(this->event_pc_roi, ec_rgb, "event cloud roi");
                viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2, "event cloud roi");
            } else {
                viewer->removePointCloud("event cloud roi");
            }
        }

        if (key == "z") {
            this->minimization_step();
        }

        if (event.getKeyCode() == 27) {
            this->viewer->close();
            //this->viewer = pcl::visualization::PCLVisualizer::Ptr();
        }
    }
};

#endif // ANNOTATION_BACKPROJECTOR_H
