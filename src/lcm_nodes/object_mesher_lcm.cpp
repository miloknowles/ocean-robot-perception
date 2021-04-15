#include <glog/logging.h>

#include <lcm/lcm-cpp.hpp>

#include <opencv2/imgproc.hpp>

#include "core/image_util.hpp"
#include "core/params_base.hpp"
#include "core/path_util.hpp"
#include "lcm_util/decode_image.hpp"
#include "lcm_util/util_mesh_t.hpp"
#include "mesher/object_mesher.hpp"

#include "vehicle/stereo_image_t.hpp"
#include "vehicle/mesh_stamped_t.hpp"

using namespace bm;
using namespace core;
using namespace mesher;


class ObjectMesherLcm final {
 public:
  struct Params : public ParamsBase
  {
    MACRO_PARAMS_STRUCT_CONSTRUCTORS(Params);

    std::string channel_input_stereo;
    std::string channel_output_mesh;
    bool visualize = true;
    int mesher_input_height = 480;    // Downsample images to have this height.

    ObjectMesher::Params mesher_params;

   private:
    void LoadParams(const YamlParser& parser) override
    {
      channel_input_stereo = YamlToString(parser.GetYamlNode("channel_input_stereo"));
      channel_output_mesh = YamlToString(parser.GetYamlNode("channel_output_mesh"));
      parser.GetYamlParam("visualize", &visualize);
      parser.GetYamlParam("mesher_input_height", &mesher_input_height);
      mesher_params = ObjectMesher::Params(parser.Subtree("ObjectMesher"));
    }
  };

  ObjectMesherLcm(const Params& params)
      : params_(params),
        mesher_(params.mesher_params)
  {
    if (!lcm_.good()) {
      LOG(WARNING) << "Failed to initialize LCM" << std::endl;
      return;
    }

    lcm_.subscribe(params_.channel_input_stereo.c_str(), &ObjectMesherLcm::HandleStereo, this);
    LOG(INFO) << "Listening for stereo images on: " << params_.channel_input_stereo << std::endl;
    LOG(INFO) << "Will publish mesh on: " << params_.channel_output_mesh << std::endl;
  }

  void Spin()
  {
    while (0 == lcm_.handle() && !is_shutdown_);
  }

  void HandleStereo(const lcm::ReceiveBuffer*,
                    const std::string&,
                    const vehicle::stereo_image_t* msg)
  {
    CHECK_EQ(msg->img_left.encoding, msg->img_right.encoding)
        << "Left and right images have different encodings!" << std::endl;

    const std::string encoding = msg->img_left.encoding;

    StereoImage1b stereo_pair(msg->header.timestamp, msg->header.seq, Image1b(), Image1b());

    if (encoding == "jpg") {
      cv::Mat left, right;
      bm::DecodeJPG(msg->img_left, left);
      bm::DecodeJPG(msg->img_right, right);

      stereo_pair.left_image = MaybeConvertToGray(left);
      stereo_pair.right_image = MaybeConvertToGray(right);

      if (stereo_pair.left_image.rows == 0 || stereo_pair.left_image.cols == 0) {
        LOG(WARNING) << "Problem decoding left image" << std::endl;
        return;
      }
      if (stereo_pair.right_image.rows == 0 || stereo_pair.right_image.cols == 0) {
        LOG(WARNING) << "Problem decoding right image" << std::endl;
        return;
      }

      TriangleMesh mesh;

      if (stereo_pair.left_image.rows > params_.mesher_input_height) {
        StereoImage1b pair_downsized = StereoImage1b(stereo_pair.timestamp, stereo_pair.camera_id, Image1b(), Image1b());
        const double height = static_cast<double>(stereo_pair.left_image.rows);
        const double scale_factor = static_cast<double>(params_.mesher_input_height) / height;
        const cv::Size input_size(static_cast<int>(scale_factor * stereo_pair.left_image.cols), params_.mesher_input_height);
        cv::resize(stereo_pair.left_image, pair_downsized.left_image, input_size, 0, 0, cv::INTER_LINEAR);
        cv::resize(stereo_pair.right_image, pair_downsized.right_image, input_size, 0, 0, cv::INTER_LINEAR);
        mesh = mesher_.ProcessStereo(std::move(pair_downsized));
      } else {
        mesh = mesher_.ProcessStereo(std::move(stereo_pair));
      }

      vehicle::mesh_stamped_t out;
      out.header = msg->header;
      pack_mesh_t(mesh.vertices, mesh.triangles, out.mesh);
      lcm_.publish(params_.channel_output_mesh.c_str(), &out);

    } else {
      LOG(FATAL) << "Unsupported encoding: " << encoding << std::endl;
    }
  }

 private:
  std::atomic_bool is_shutdown_{false};
  Params params_;
  ObjectMesher mesher_;
  lcm::LCM lcm_;
};


int main(int argc, char const *argv[])
{
  // Set up glog.
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = 1;

  CHECK_EQ(3ul, argc)
      << "Requires (2) args: node_params_path and shared_params_path."
      << "They should be relative to vehicle/config" << std::endl;

  std::string node_params_path = std::string(argv[1]);
  const std::string shared_params_path = std::string(argv[2]);

  ObjectMesherLcm::Params params(
    config_path(node_params_path),
    config_path(shared_params_path));

  ObjectMesherLcm node(params);
  node.Spin();

  LOG(INFO) << "DONE" << std::endl;

  return 0;
}

