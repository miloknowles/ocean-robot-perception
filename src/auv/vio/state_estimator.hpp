#pragma once

#include <thread>
#include <atomic>

#include <gtsam/navigation/NavState.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Key.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Cal3_S2.h>
#include <gtsam/geometry/Cal3_S2Stereo.h>
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam/slam/SmartProjectionPoseFactor.h>
#include <gtsam_unstable/slam/SmartStereoProjectionPoseFactor.h>

#include "core/macros.hpp"
#include "core/eigen_types.hpp"
#include "core/cv_types.hpp"
#include "core/thread_safe_queue.hpp"
#include "core/stereo_image.hpp"
#include "core/imu_measurement.hpp"
#include "vio/stereo_frontend.hpp"
#include "vio/state_estimate_3d.hpp"

namespace bm {
namespace vio {

using namespace core;

static const size_t kWaitForDataMilliseconds = 100;

typedef gtsam::SmartStereoProjectionPoseFactor SmartStereoFactor;
typedef gtsam::SmartProjectionPoseFactor<gtsam::Cal3_S2> SmartMonoFactor;
typedef gtsam::PinholePose<gtsam::Cal3_S2> Camera;

typedef std::unordered_map<uid_t, SmartMonoFactor::shared_ptr> SmartMonoFactorMap;
typedef std::unordered_map<uid_t, SmartStereoFactor::shared_ptr> SmartStereoFactorMap;
typedef std::map<uid_t, gtsam::FactorIndex> LmkToFactorMap;


// Waits for a queue item for timeout_sec. Returns whether an item arrived before the timeout.
template <typename QueueType>
bool WaitForResultOrTimeout(QueueType& queue, double timeout_sec)
{
  double elapsed = 0;
  const size_t ms_each_wait = (timeout_sec < kWaitForDataMilliseconds) ? \
                               kWaitForDataMilliseconds / 5 : kWaitForDataMilliseconds;
  while (queue.Empty() && elapsed < timeout_sec) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms_each_wait));
    elapsed += static_cast<double>(1e-3 * ms_each_wait);
  }

  return elapsed >= timeout_sec;
}


// The smoother changes its behavior depending on whether vision is available/unavailable.
enum class SmootherMode { VISION_AVAILABLE, VISION_UNAVAILABLE };


// Returns a summary of the smoother update.
struct SmootherResult final
{
  explicit SmootherResult(bool added_keypose,
                          const gtsam::Key& new_keypose_key,
                          const gtsam::Pose3& T_world_keypose,
                          double new_keypose_time)
      : added_keypose(added_keypose),
        new_keypose_key(new_keypose_key),
        T_world_keypose(T_world_keypose),
        new_keypose_time(new_keypose_time) {}

  bool added_keypose = false;
  gtsam::Key new_keypose_key;
  gtsam::Pose3 T_world_keypose;
  double new_keypose_time;
};


struct FilterResult final
{
  FilterResult(double T_world_cam_time,
               const gtsam::Pose3& T_world_cam)
      : T_world_cam_time(T_world_cam_time), T_world_cam(T_world_cam) {}

  double T_world_cam_time;
  gtsam::Pose3 T_world_cam;
};


typedef std::function<void(const SmootherResult&)> SmootherResultCallback;
typedef std::function<void(const FilterResult&)> FilterResultCallback;


class StateEstimator final {
 public:
  struct Options final
  {
    Options() = default;

    StereoFrontend::Options stereo_frontend_options;

    int max_queue_size_stereo = 20;
    int max_queue_size_imu = 1000;
    int max_queue_size_aps = 20;

    int reliable_vision_min_lmks = 12;
    double max_sec_btw_keyframes = 5.0;

    int ISAM2_extra_smoothing_iters = 2;

    // If vision is available, wait longer for stereo measurements to come in.
    double smoother_wait_vision_available = 5.0; // sec

    // If vision is unavailable, don't waste time waiting around for it.
    double smoother_wait_vision_unavailable = 0.1; // sec
  };

  MACRO_DELETE_COPY_CONSTRUCTORS(StateEstimator);

  StateEstimator(const Options& opt, const StereoCamera& stereo_rig);

  void ReceiveStereo(const StereoImage& stereo_pair);
  void ReceiveImu(const ImuMeasurement& imu_data);

  // Add a function that gets called whenever the smoother finished an update.
  // NOTE(milo): Callbacks will block the smoother thread, so keep them fast!
  void RegisterSmootherResultCallback(const SmootherResultCallback& cb);
  void RegisterFilterResultCallback(const FilterResultCallback& cb);

  // This call blocks until all queued stereo pairs have been processed.
  void BlockUntilFinished();

  // Tells all of the threads to exit, then joins them, then exits.
  void Shutdown();

 private:
  // Tracks features from stereo images, and decides what to do with the results.
  void StereoFrontendLoop();

  void SmootherLoop();

  SmootherResult UpdateGraphNoVision();

  SmootherResult UpdateGraphWithVision(gtsam::ISAM2& smoother,
                                      SmartStereoFactorMap& stereo_factors,
                                      LmkToFactorMap& lmk_to_factor_map,
                                      const gtsam::SharedNoiseModel& stereo_factor_noise,
                                      const gtsam::SmartProjectionParams& stereo_factor_params,
                                      const gtsam::Cal3_S2Stereo::shared_ptr& cal3_stereo,
                                      const gtsam::Pose3& T_world_lkf);

  void FilterLoop();

  void HandleReinitializeVision(const gtsam::Key& current_cam_key,
                                const StereoFrontend::Result& result,
                                gtsam::NonlinearFactorGraph& new_factors,
                                gtsam::Values& new_values);

  // A central place to allocate new "keypose" ids. They are called "keyposes" because they could
  // come from vision OR other data sources (e.g acoustic localization).
  uid_t GetNextKeyposeId() { return next_kf_id_++; }
  uid_t GetPrevKeyposeId() { return next_kf_id_ - 1; }

 private:
  Options opt_;
  StereoCamera stereo_rig_;
  std::atomic_bool is_shutdown_;  // Set this to trigger a *graceful* shutdown.

  StereoFrontend stereo_frontend_;

  std::thread stereo_frontend_thread_;
  std::thread smoother_thread_;
  std::thread filter_thread_;

  //================================================================================================
  // After solving the factor graph, the smoother updates this pose.
  std::mutex mutex_smoother_result_;
  SmootherResult smoother_result_{false, gtsam::Key(0), gtsam::Pose3::identity(), -1};
  std::atomic_bool trigger_sync_filter_{false};
  //================================================================================================

  //================================================================================================
  // The filter maintains the pose of the camera in the world.
  std::mutex mutex_filter_result_;
  double filter_T_world_cam_time_ = -1;
  gtsam::Pose3 filter_T_world_cam_ = gtsam::Pose3::identity();
  //================================================================================================

  ThreadsafeQueue<StereoImage> raw_stereo_queue_;
  ThreadsafeQueue<StereoFrontend::Result> smoother_vo_queue_;
  ThreadsafeQueue<ImuMeasurement> smoother_imu_queue_;

  ThreadsafeQueue<StereoFrontend::Result> filter_vo_queue_;
  ThreadsafeQueue<ImuMeasurement> filter_imu_queue_;

  uid_t next_kf_id_ = 0;
  double last_kf_time_ = 0;

  std::vector<SmootherResultCallback> smoother_result_callbacks_;
  std::vector<FilterResultCallback> filter_result_callbacks_;
};

}
}
