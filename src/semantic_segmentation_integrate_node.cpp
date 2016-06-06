// local includes
#include "utils/data_loader.h"
#include "utils/config.h"
#include "voxel.h"

// dense crf includes
#include "densecrf.h"

// libforest includes
#include "libforest/libforest.h"

// PCL includes
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_types.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl/conversions.h>
#include <pcl_ros/transforms.h>

//Ros includes
#include "ros/ros.h"
#include "sensor_msgs/PointCloud2.h"

//STL includes
#include <exception>
#include <map>
#include <string>
#include <vector>

//Service includes
#include "semantic_segmentation/LabelIntegratedPointCloud.h"
#include "semantic_map_publisher/ObservationService.h"
#include "semantic_segmentation/LabelIntegratedPointInstanceCloud.h"
#include "semantic_map_publisher/ObservationInstanceService.h"
#include "semantic_map_publisher/SensorOriginService.h"


class Labeler{
public:
  Labeler(std::string config_file, std::string forest_file, ros::ServiceClient& cloud_service, ros::ServiceClient& origin_service, ros::Publisher& cloud_publisher) :
    _conf(Utils::Config(config_file, std::map<std::string, std::string>())),
    _dl(Utils::DataLoader(_conf, false)),
    _label_converter(_dl.getLabelConverter()),
    _C(_label_converter.getValidLabelCount()),
    _minimum_points(_conf.get<int>("min_point_count")),
    _cloud_service(cloud_service),
    _origin_service(origin_service),
    _cloud_publisher(cloud_publisher){
    _forest = new libf::RandomForest<libf::DecisionTree>();
    std::filebuf fb;
    if (fb.open (forest_file,std::ios::in)){
      std::istream is(&fb);
      _forest->read(is);
    }else{
      throw std::runtime_error("Could not load the random forest data file, please make sure it is there!\n oUse the \"download_rf.sh\" to download a rf model.");
    }
    fb.close();
  }

  ~Labeler(){
    delete _forest;
  };

  bool label_cloud(semantic_segmentation::LabelIntegratedPointCloud::Request  &req,
                   semantic_segmentation::LabelIntegratedPointCloud::Response &res){

    //Get the cloud and the origin.
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZRGBA>);
    semantic_map_publisher::ObservationService srv;
    srv.request.waypoint_id = req.waypoint_id;
    srv.request.resolution = 0.01;
    if (_cloud_service.call(srv)){
      semantic_map_publisher::SensorOriginService origin_srv;
      origin_srv.request.waypoint_id = req.waypoint_id;
      if (_origin_service.call(origin_srv)){
        //Get the cloud and convert it to a pcl format
        pcl::PCLPointCloud2 pcl_pc2;
        pcl_conversions::toPCL(srv.response.cloud,pcl_pc2);
        pcl::fromPCLPointCloud2(pcl_pc2, *cloud);

        //Get the origin
        cloud->sensor_origin_ = Eigen::Vector4f(origin_srv.response.origin.x, origin_srv.response.origin.y, origin_srv.response.origin.z, 1.0);
        ROS_INFO("Cloud received, a total of %lu points found", cloud->size());
        _frame_id = srv.response.cloud.header.frame_id;
      }else{
        ROS_ERROR("Didn't receive an sensor origin for the cloud!");
      return false;
      }
    }else{
      ROS_ERROR("Didn't receive a pointcloud");
      return false;
    }

    //Convert the cloud to lab
    cv::Mat rgb(cloud->size(), 1, CV_8UC3);
    uchar* rgb_ptr = rgb.ptr<uchar>(0);
    for(auto p : cloud->points){
      *rgb_ptr++ = p.b;
      *rgb_ptr++ = p.g;
      *rgb_ptr++ = p.r;
    }
    cv::cvtColor(rgb, rgb, CV_BGR2Lab);
    rgb_ptr = rgb.ptr<uchar>(0);
    for(int i = 0; i <cloud->points.size(); ++i){
      cloud->points[i].b = *rgb_ptr++;
      cloud->points[i].g = *rgb_ptr++;
      cloud->points[i].r = *rgb_ptr++;
    }

    //Push it through the voxelization
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr voxelized_cloud;
    std::map<int, std::shared_ptr<Voxel> > voxels;
    _dl.extractVoxels(cloud, voxels, voxelized_cloud);
    ROS_INFO("Voxelized the cloud, got %lu supervoxels.", voxels.size());


    // Decide which voxels to label and which to discard directly!
    int N = 0;
    for(auto v_it = voxels.begin(); v_it != voxels.end(); ){
      int n = v_it->second->getSize();
      // only look at large enough voxels.
      if(n >= _minimum_points){
        N += n;
        v_it->second->computeFeatures();
        ++v_it;
      }else{
        // Otherwise remove them completely!
        voxels.erase(v_it++);
      }
    }
    ROS_INFO("Remaining valid points: %i", N);

    uint point_index = 0; //We can't use the point indices as these now have changed due to a set of missing ones.
    float appearance_color_sigma = _conf.get<float>("appearance_color_sigma");
    float appearance_range_sigma = _conf.get<float>("appearance_range_sigma");
    float appearance_weight      = _conf.get<float>("appearance_weight");
    float smoothnes_range_sigma  = _conf.get<float>("smoothnes_range_sigma");
    float smoothnes_weight       = _conf.get<float>("smoothnes_weight");
    DenseCRF crf(N, _C);
    Eigen::MatrixXf unary(_C, N);
    Eigen::MatrixXf feature(6, N);
    Eigen::MatrixXf feature2(3, N);
    std::vector<float> probs;
    for(auto v : voxels){
      const libf::DataPoint& feat = v.second->getFeatures();
      _forest->classLogPosterior(feat, probs);
      v.second->addDataToCrfMats(unary, feature, feature2, point_index, probs, appearance_color_sigma, appearance_range_sigma, smoothnes_range_sigma);
    }
    crf.setUnaryEnergy( unary );
    crf.addPairwiseEnergy( feature, new PottsCompatibility( appearance_weight ) );
    crf.addPairwiseEnergy( feature2, new PottsCompatibility( smoothnes_weight ) );
    Eigen::MatrixXf map = crf.inference(_conf.get<int>("crf_iterations"));

    //Just for visualizing, the none voxelized stuff will be black, no time to remove them right now.
    for(int i = 0; i < voxelized_cloud->size(); ++i){
      voxelized_cloud->points[i].r = 0;
      voxelized_cloud->points[i].g = 0;
      voxelized_cloud->points[i].b = 0;
    }


    std::vector<float> result_prob(N*_C, 0);
    std::vector<int> result_labels(N, 0);
    std::vector<float> label_frequencies(_C, 0);
    //For each voxel, apply the RF
    //std::vector<float> probs;
    point_index =0;
    for(auto v : voxels){
      const std::vector<int>& indices = v.second->getIndices();
      for(int i : indices){
        int max_label = 0;
        float max_prob = map(0,point_index);
        for(int c = 1; c < _C; ++c){
          if(map(c,point_index)  > max_prob){
            max_prob = map(c,point_index);
            max_label = c;
          }
        }
        uchar r,g,b;
        _label_converter.labelToRgb(max_label, r,g,b);
        voxelized_cloud->points[i].r = r;
        voxelized_cloud->points[i].g = g;
        voxelized_cloud->points[i].b = b;

        result_labels[point_index] = max_label;
        int prob_idx = _C*point_index;
        for(int c = 0; c < _C; c++){
          result_prob[prob_idx++] = map(c,point_index);
          label_frequencies[c] += map(c,point_index);
        }
        point_index++;
      }
    }
    ROS_INFO("Done classifying all the supervoxels.");

    //Normalize the frequencies.
    for(int j = 0; j < _C; j++){
      label_frequencies[j] /= float(N);
    }



    //Calling my stuff to get labels for each point with valid (aka !isnan) x,y,z values.
    res.index_to_label_name = _label_converter.getLabelNames();
    res.label = result_labels;
    res.label_probabilities = result_prob;
    res.label_frequencies = label_frequencies;
    res.points.resize(N);

    point_index = 0;
    for(auto v : voxels){
      const std::vector<int>& indices = v.second->getIndices();
      for(int i : indices){
        res.points[point_index].x = voxelized_cloud->points[i].x;
        res.points[point_index].y = voxelized_cloud->points[i].y;
        res.points[point_index].z = voxelized_cloud->points[i].z;
        point_index++;
      }
    }

    _stored_waypoints[req.waypoint_id] = voxelized_cloud;
    publish_clouds();

    //Done
    return true;
  }

  bool label_cloud_plus(semantic_segmentation::LabelIntegratedPointInstanceCloud::Request  &req,
                   semantic_segmentation::LabelIntegratedPointInstanceCloud::Response &res){

    //Get the cloud and the origin.
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZRGBA>);
    semantic_map_publisher::ObservationInstanceService srv;
    srv.request.waypoint_id = req.waypoint_id;
    srv.request.instance_number=req.instance_number;
    srv.request.resolution = 0.01;
    if (_cloud_service.call(srv)){
      semantic_map_publisher::SensorOriginService origin_srv;
      origin_srv.request.waypoint_id = req.waypoint_id;
      //origin_srv.request.instance_number = req.instance_number;
      if (_origin_service.call(origin_srv)){
        //Get the cloud and convert it to a pcl format
        pcl::PCLPointCloud2 pcl_pc2;
        pcl_conversions::toPCL(srv.response.cloud,pcl_pc2);
        pcl::fromPCLPointCloud2(pcl_pc2, *cloud);

        //Get the origin
        cloud->sensor_origin_ = Eigen::Vector4f(origin_srv.response.origin.x, origin_srv.response.origin.y, origin_srv.response.origin.z, 1.0);
        ROS_INFO("Cloud received, a total of %lu points found", cloud->size());
        _frame_id = srv.response.cloud.header.frame_id;
      }else{
        ROS_ERROR("Didn't receive an sensor origin for the cloud!");
      return false;
      }
    }else{
      ROS_ERROR("Didn't receive a pointcloud plus");
      return false;
    }

    //Convert the cloud to lab
    cv::Mat rgb(cloud->size(), 1, CV_8UC3);
    uchar* rgb_ptr = rgb.ptr<uchar>(0);
    for(auto p : cloud->points){
      *rgb_ptr++ = p.b;
      *rgb_ptr++ = p.g;
      *rgb_ptr++ = p.r;
    }
    cv::cvtColor(rgb, rgb, CV_BGR2Lab);
    rgb_ptr = rgb.ptr<uchar>(0);
    for(int i = 0; i <cloud->points.size(); ++i){
      cloud->points[i].b = *rgb_ptr++;
      cloud->points[i].g = *rgb_ptr++;
      cloud->points[i].r = *rgb_ptr++;
    }

    //Push it through the voxelization
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr voxelized_cloud;
    std::map<int, std::shared_ptr<Voxel> > voxels;
    _dl.extractVoxels(cloud, voxels, voxelized_cloud);
    ROS_INFO("Voxelized the cloud, got %lu supervoxels.", voxels.size());


    // Decide which voxels to label and which to discard directly!
    int N = 0;
    for(auto v_it = voxels.begin(); v_it != voxels.end(); ){
      int n = v_it->second->getSize();
      // only look at large enough voxels.
      if(n >= _minimum_points){
        N += n;
        v_it->second->computeFeatures();
        ++v_it;
      }else{
        // Otherwise remove them completely!
        voxels.erase(v_it++);
      }
    }
    ROS_INFO("Remaining valid points: %i", N);

    uint point_index = 0; //We can't use the point indices as these now have changed due to a set of missing ones.
    float appearance_color_sigma = _conf.get<float>("appearance_color_sigma");
    float appearance_range_sigma = _conf.get<float>("appearance_range_sigma");
    float appearance_weight      = _conf.get<float>("appearance_weight");
    float smoothnes_range_sigma  = _conf.get<float>("smoothnes_range_sigma");
    float smoothnes_weight       = _conf.get<float>("smoothnes_weight");
    DenseCRF crf(N, _C);
    Eigen::MatrixXf unary(_C, N);
    Eigen::MatrixXf feature(6, N);
    Eigen::MatrixXf feature2(3, N);
    std::vector<float> probs;
    for(auto v : voxels){
      const libf::DataPoint& feat = v.second->getFeatures();
      _forest->classLogPosterior(feat, probs);
      v.second->addDataToCrfMats(unary, feature, feature2, point_index, probs, appearance_color_sigma, appearance_range_sigma, smoothnes_range_sigma);
    }
    crf.setUnaryEnergy( unary );
    crf.addPairwiseEnergy( feature, new PottsCompatibility( appearance_weight ) );
    crf.addPairwiseEnergy( feature2, new PottsCompatibility( smoothnes_weight ) );
    Eigen::MatrixXf map = crf.inference(_conf.get<int>("crf_iterations"));

    //Just for visualizing, the none voxelized stuff will be black, no time to remove them right now.
    for(int i = 0; i < voxelized_cloud->size(); ++i){
      voxelized_cloud->points[i].r = 0;
      voxelized_cloud->points[i].g = 0;
      voxelized_cloud->points[i].b = 0;
    }


    std::vector<float> result_prob(N*_C, 0);
    std::vector<int> result_labels(N, 0);
    std::vector<float> label_frequencies(_C, 0);
    //For each voxel, apply the RF
    //std::vector<float> probs;
    point_index =0;
    for(auto v : voxels){
      const std::vector<int>& indices = v.second->getIndices();
      for(int i : indices){
        int max_label = 0;
        float max_prob = map(0,point_index);
        for(int c = 1; c < _C; ++c){
          if(map(c,point_index)  > max_prob){
            max_prob = map(c,point_index);
            max_label = c;
          }
        }
        uchar r,g,b;
        _label_converter.labelToRgb(max_label, r,g,b);
        voxelized_cloud->points[i].r = r;
        voxelized_cloud->points[i].g = g;
        voxelized_cloud->points[i].b = b;

        result_labels[point_index] = max_label;
        int prob_idx = _C*point_index;
        for(int c = 0; c < _C; c++){
          result_prob[prob_idx++] = map(c,point_index);
          label_frequencies[c] += map(c,point_index);
        }
        point_index++;
      }
    }
    ROS_INFO("Done classifying all the supervoxels.");

    //Normalize the frequencies.
    for(int j = 0; j < _C; j++){
      label_frequencies[j] /= float(N);
    }



    //Calling my stuff to get labels for each point with valid (aka !isnan) x,y,z values.
    res.index_to_label_name = _label_converter.getLabelNames();
    res.label = result_labels;
    res.label_probabilities = result_prob;
    res.label_frequencies = label_frequencies;
    res.points.resize(N);

    point_index = 0;
    for(auto v : voxels){
      const std::vector<int>& indices = v.second->getIndices();
      for(int i : indices){
        res.points[point_index].x = voxelized_cloud->points[i].x;
        res.points[point_index].y = voxelized_cloud->points[i].y;
        res.points[point_index].z = voxelized_cloud->points[i].z;
        point_index++;
      }
    }

    _stored_waypoints[req.waypoint_id] = voxelized_cloud;
    publish_clouds();

    //Done
    return true;
  }

private:
  void publish_clouds(){
    //fuse the pointclouds
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr fused(new pcl::PointCloud<pcl::PointXYZRGB>());

    int size = 0;
    for(auto cld : _stored_waypoints){
      size += cld.second->size();
    }

    fused->points.reserve(size);
    for(auto cld : _stored_waypoints){
      for(auto p : cld.second->points){
        fused->points.push_back(pcl::PointXYZRGB());
        fused->points.back().x = p.x;
        fused->points.back().y = p.y;
        fused->points.back().z = p.z;
        fused->points.back().r = p.r;
        fused->points.back().g = p.g;
        fused->points.back().b = p.b;
      }
    }

    //convert to sensor_msgs/PointCloud2
    sensor_msgs::PointCloud2 out;
    pcl::toROSMsg<pcl::PointXYZRGB>(*fused,out);

    out.header.frame_id = _frame_id;

    //publish
    _cloud_publisher.publish(out);

  }

private:
    Utils::Config _conf;
    Utils::DataLoader _dl;
    Utils::RgbLabelConversion _label_converter;
    libf::RandomForest<libf::DecisionTree>* _forest;
    int _C;
    int _minimum_points;
    ros::ServiceClient& _cloud_service;
    ros::ServiceClient& _origin_service;
    ros::Publisher& _cloud_publisher;
    std::map<std::string, pcl::PointCloud<pcl::PointXYZRGBA>::Ptr> _stored_waypoints;
    std::string _frame_id;
};

int main(int argc, char **argv){
  ros::init(argc, argv, "semantic_segmentation_service");
  
  //Initialize everything needed for labeling
  if(argc < 2){
    ROS_ERROR("Usage: %s <config.json> <rf.dat>", argv[0]);
    return 1;
  }

  //get some services.
  ros::NodeHandle nh("~");
  ros::ServiceClient client_get_cloud_plus    = nh.serviceClient<semantic_map_publisher::ObservationInstanceService>("/semantic_map_publisher/SemanticMapPublisher/ObservationInstanceService");
  ros::ServiceClient client_get_cloud    = nh.serviceClient<semantic_map_publisher::ObservationService>("/semantic_map_publisher/SemanticMapPublisher/ObservationService");
  ros::ServiceClient client_cloud_origin = nh.serviceClient<semantic_map_publisher::SensorOriginService>("/semantic_map_publisher/SemanticMapPublisher/SensorOriginService");

  //Advertise a topic for publishing the colored clouds.
  ros::Publisher cloud_publisher = nh.advertise<sensor_msgs::PointCloud2>("/semantic_segmentation_clouds", 1, true);


  //The robot height is quick and dirty :( This is needed as the integrated clouds lack a sensor origin.
  std::shared_ptr<Labeler> l1;
  std::shared_ptr<Labeler> l2;
  try{
    l2 = std::shared_ptr<Labeler>(new Labeler(argv[1], argv[2], client_get_cloud_plus, client_cloud_origin, cloud_publisher));
    l1 = std::shared_ptr<Labeler>(new Labeler(argv[1], argv[2], client_get_cloud, client_cloud_origin, cloud_publisher));

  } catch (std::exception& e){
    ROS_ERROR("%s",e.what());
    return 1;
  }

  //Advertise the service.
  ros::ServiceServer label_service_plus = nh.advertiseService("label_integrated_cloud_plus", &Labeler::label_cloud_plus, l2.get());
  ros::ServiceServer label_service = nh.advertiseService("label_integrated_cloud", &Labeler::label_cloud, l1.get());



  ROS_INFO("Semantic segmentation service ready.");
  ros::spin();

  return 0;
}
