#include "vio/smoother.hpp"

namespace bm {
namespace vio {


Smoother::Smoother(const Params& params,
                   const StereoCamera& stereo_rig)
    : params_(params),
      stereo_rig_(stereo_rig)
{
  ResetISAM2();

  cal3_stereo_ = gtsam::Cal3_S2Stereo::shared_ptr(
      new gtsam::Cal3_S2Stereo(
          stereo_rig_.fx(),
          stereo_rig_.fy(),
          kSetSkewToZero,
          stereo_rig_.cx(),
          stereo_rig_.cy(),
          stereo_rig_.Baseline()));

  // cal3_mono_ = gtsam::Cal3_S2::shared_ptr(
  //     new gtsam::Cal3_S2(
  //         stereo_rig_.fx(),
  //         stereo_rig_.fy(),
  //         kSetSkewToZero,
  //         stereo_rig_.cx(),
  //         stereo_rig_.cy()));

  // https://bitbucket.org/gtborg/gtsam/issues/420/problem-with-isam2-stereo-smart-factors-no
  lmk_stereo_factor_params_ = gtsam::SmartStereoProjectionParams(gtsam::JACOBIAN_SVD, gtsam::ZERO_ON_DEGENERACY);
}


void Smoother::ResetISAM2()
{
  // If relinearizeThreshold is zero, the graph is always relinearized on update().
  gtsam::ISAM2Params smoother_params;
  smoother_params.relinearizeThreshold = 0.0;
  smoother_params.relinearizeSkip = 1;

  // NOTE(milo): This is needed for using smart factors.
  // See: https://github.com/borglab/gtsam/blob/d6b24294712db197096cd3ea75fbed3157aea096/gtsam_unstable/slam/tests/testSmartStereoFactor_iSAM2.cpp
  smoother_params.cacheLinearizedFactors = false;
  gtsam::ISAM2 smoother_ = gtsam::ISAM2(smoother_params);
}


void Smoother::Initialize(seconds_t timestamp,
                          const gtsam::Pose3& P_world_body,
                          const gtsam::Vector3& v_world_body,
                          const ImuBias& imu_bias,
                          bool imu_available)
{
  ResetKeyposeId();
  ResetISAM2();

  // Clear out any members that store state.
  lmk_to_factor_map_.clear();;
  stereo_factors_.clear();

  const uid_t id0 = GetNextKeyposeId();
  const gtsam::Symbol P0_sym('X', id0);
  const gtsam::Symbol V0_sym('V', id0);
  const gtsam::Symbol B0_sym('B', id0);

  gtsam::NonlinearFactorGraph new_factors;
  gtsam::Values new_values;

  result_ = SmootherResult(id0, timestamp, P_world_body, imu_available, v_world_body, imu_bias);

  // Prior and initial value for the first pose.
  new_factors.addPrior<gtsam::Pose3>(P0_sym, P_world_body, params_.pose_prior_noise_model);
  new_values.insert(P0_sym, P_world_body);

  // If IMU available, add inertial variables to the graph.
  if (imu_available) {
    new_values.insert(V0_sym, v_world_body);
    new_values.insert(B0_sym, imu_bias);
    new_factors.addPrior(V0_sym, kZeroVelocity, params_.velocity_noise_model);
    new_factors.addPrior(B0_sym, kZeroImuBias, params_.bias_prior_noise_model);
  }

  smoother_.update(new_factors, new_values);
}


// Preintegrate IMU measurements since the last keypose, and add an IMU factor to the graph.
// Returns whether preintegration was successful. If so, there will be a factor constraining
// X_{t-1}, V_{t-1}, B_{t-1} <--- FACTOR ---> X_{t}, V_{t}, B_{t}.
// If the graph is missing variables for velocity and bias at (t-1), which will occur when IMU is
// unavailable, then these variables will be initialized with a ZERO-VELOCITY, ZERO-BIAS prior.
static void AddImuFactors(uid_t keypose_id,
                          const PimResult& pim_result,
                          const SmootherResult& last_smoother_result,
                          bool predict_keypose_value,
                          gtsam::Values& new_values,
                          gtsam::NonlinearFactorGraph& new_factors,
                          const Smoother::Params& params)
{
  CHECK(pim_result.valid) << "Preintegrated IMU invalid" << std::endl;

  const gtsam::Symbol keypose_sym('X', keypose_id);
  const gtsam::Symbol vel_sym('V', keypose_id);
  const gtsam::Symbol bias_sym('B', keypose_id);

  const uid_t last_keypose_id = last_smoother_result.keypose_id;
  const gtsam::Symbol last_keypose_sym('X', last_keypose_id);
  const gtsam::Symbol last_vel_sym('V', last_keypose_id);
  const gtsam::Symbol last_bias_sym('B', last_keypose_id);

  // NOTE(milo): Gravity is corrected for in predict(), not during preintegration (NavState.cpp).
  const gtsam::NavState prev_state(last_smoother_result.P_world_body,
                                    last_smoother_result.v_world_body);
  const gtsam::NavState pred_state = pim_result.pim.predict(prev_state, last_smoother_result.imu_bias);

  // If no between factor from VO, we can use IMU to get an initial guess on the current pose.
  if (predict_keypose_value) {
    new_values.insert(keypose_sym, pred_state.pose());
  }

  new_values.insert(vel_sym, pred_state.velocity());
  new_values.insert(bias_sym, last_smoother_result.imu_bias);

  // If IMU was unavailable at the last state, we initialize it here with a prior.
  // NOTE(milo): For now we assume zero velocity and zero acceleration for the first pose.
  if (!last_smoother_result.has_imu_state) {
    LOG(INFO) << "Last smoother state missing VELOCITY and BIAS variables, will add them" << std::endl;
    new_values.insert(last_vel_sym, kZeroVelocity);
    new_values.insert(last_bias_sym, kZeroImuBias);

    new_factors.addPrior(last_vel_sym, kZeroVelocity, params.velocity_noise_model);
    new_factors.addPrior(last_bias_sym, kZeroImuBias, params.bias_drift_noise_model);
  }

  const gtsam::CombinedImuFactor imu_factor(last_keypose_sym, last_vel_sym,
                                            keypose_sym, vel_sym,
                                            last_bias_sym, bias_sym,
                                            pim_result.pim);
  new_factors.push_back(imu_factor);

  // Add a prior on the change in bias.
  new_factors.push_back(gtsam::BetweenFactor<ImuBias>(
      last_bias_sym, bias_sym, kZeroImuBias, params.bias_drift_noise_model));
}


SmootherResult Smoother::UpdateGraphNoVision(const PimResult& pim_result)
{
  CHECK(pim_result.valid) << "Preintegrated IMU invalid" << std::endl;

  gtsam::NonlinearFactorGraph new_factors;
  gtsam::Values new_values;

  const uid_t keypose_id = GetNextKeyposeId();
  const seconds_t keypose_time = pim_result.to_time;
  const uid_t last_keypose_id = result_.keypose_id;

  const gtsam::Symbol keypose_sym('X', keypose_id);
  const gtsam::Symbol vel_sym('V', keypose_id);
  const gtsam::Symbol bias_sym('B', keypose_id);

  const gtsam::Symbol last_keypose_sym('X', last_keypose_id);
  const gtsam::Symbol last_vel_sym('V', last_keypose_id);
  const gtsam::Symbol last_bias_sym('B', last_keypose_id);

  //=================================== IMU PREINTEGRATION FACTOR ==================================
  AddImuFactors(
      keypose_id,
      pim_result,
      result_,
      true,
      new_values,
      new_factors,
      params_);

  //==================================== UPDATE FACTOR GRAPH =======================================
  gtsam::ISAM2Result isam_result = smoother_.update(new_factors, new_values);

  // (Optional) run the smoother a few more times to reduce error.
  for (int i = 0; i < params_.extra_smoothing_iters; ++i) {
    smoother_.update();
  }

  //================================ RETRIEVE VARIABLE ESTIMATES ===================================
  const gtsam::Values& estimate = smoother_.calculateBestEstimate();

  result_lock_.lock();
  result_ = SmootherResult(
      keypose_id,
      keypose_time,
      estimate.at<gtsam::Pose3>(keypose_sym),
      true,
      estimate.at<gtsam::Vector3>(vel_sym),
      estimate.at<ImuBias>(bias_sym));
  result_lock_.unlock();

  // TODO: reset bias in state estimator
  return result_;
}


SmootherResult Smoother::UpdateGraphWithVision(
    const StereoFrontend::Result& odom_result,
    const std::shared_ptr<PimResult>& pim_result_ptr)
{
  CHECK(odom_result.is_keyframe) << "Smoother shouldn't receive a non-keyframe odometry result" << std::endl;
  CHECK(odom_result.lmk_obs.size() > 0) << "Smoother shouln't receive a keyframe with no observations" << std::endl;

  gtsam::NonlinearFactorGraph new_factors;
  gtsam::Values new_values;

  // Needed for using ISAM2 with smart factors.
  gtsam::FastMap<gtsam::FactorIndex, gtsam::KeySet> factorNewAffectedKeys;

  // Map: ISAM2 internal FactorIndex => lmk_id.
  std::map<gtsam::FactorIndex, uid_t> map_new_factor_to_lmk_id;

  const uid_t keypose_id = GetNextKeyposeId();
  const seconds_t keypose_time = ConvertToSeconds(odom_result.timestamp);
  const uid_t last_keypose_id = result_.keypose_id;

  const gtsam::Symbol keypose_sym('X', keypose_id);
  const gtsam::Symbol vel_sym('V', keypose_id);
  const gtsam::Symbol bias_sym('B', keypose_id);

  const gtsam::Symbol last_keypose_sym('X', last_keypose_id);
  const gtsam::Symbol last_vel_sym('V', last_keypose_id);
  const gtsam::Symbol last_bias_sym('B', last_keypose_id);

  // Check if the timestamp from the LAST VO keyframe matches the last smoother result. If so, the
  // odometry measurement can be used in the graph.
  // TODO(milo): Eventually add some epsilon.
  const bool vo_is_aligned = (result_.timestamp == ConvertToSeconds(odom_result.timestamp_lkf));

  bool graph_has_vo_btw_factor = false;
  bool graph_has_imu_btw_factor = false;

  // If VO is valid, we can use it to create a between factor and guess the latest pose.
  if (vo_is_aligned) {
    const gtsam::Pose3 P_world_body = result_.P_world_body * gtsam::Pose3(odom_result.T_lkf_cam);
    new_values.insert(keypose_sym, P_world_body);

    // Add an odometry factor between the previous KF and current KF.
    const gtsam::Pose3 P_lkf_cam(odom_result.T_lkf_cam);

    new_factors.push_back(gtsam::BetweenFactor<gtsam::Pose3>(
        last_keypose_sym, keypose_sym, P_lkf_cam,
        params_.frontend_vo_noise_model));

    graph_has_vo_btw_factor = true;
  }

  //===================================== STEREO SMART FACTORS ======================================
  // Even if visual odometry didn't line up with the previous keypose, we still want to add stereo
  // landmarks, since they could be observed in future keyframes.
  for (const LandmarkObservation& lmk_obs : odom_result.lmk_obs) {
    // TODO(milo): See if we can remove this and let gtsam deal with it.
    if (lmk_obs.disparity < 1.0) {
      LOG(WARNING) << "Skipped zero-disparity observation!" << std::endl;
      continue;
    }

    const uid_t lmk_id = lmk_obs.landmark_id;

    // NEW SMART FACTOR: Creating smart stereo factor for the first time.
    if (stereo_factors_.count(lmk_id) == 0) {
      stereo_factors_.emplace(lmk_id, new SmartStereoFactor(
          params_.lmk_stereo_factor_noise_model, lmk_stereo_factor_params_));

      // Indicate that the newest factor refers to lmk_id.
      // NOTE(milo): Add the new factor to the graph. Order matters here!
      map_new_factor_to_lmk_id[new_factors.size()] = lmk_id;
      new_factors.push_back(stereo_factors_.at(lmk_id));

    // UPDATE SMART FACTOR: An existing ISAM2 factor now affects the camera pose with the current key.
    } else {
      factorNewAffectedKeys[lmk_to_factor_map_.at(lmk_id)].insert(keypose_sym);
    }

    SmartStereoFactor::shared_ptr sfptr = stereo_factors_.at(lmk_id);
    const gtsam::StereoPoint2 stereo_point2(
        lmk_obs.pixel_location.x,                      // X-coord in left image
        lmk_obs.pixel_location.x - lmk_obs.disparity,  // x-coord in right image
        lmk_obs.pixel_location.y);                     // y-coord in both images (rectified)
    sfptr->add(stereo_point2, keypose_sym, cal3_stereo_);
  }

  //================================= IMU PREINTEGRATION FACTOR ====================================
  if (pim_result_ptr && pim_result_ptr->valid) {
    AddImuFactors(keypose_id, *pim_result_ptr, result_, !graph_has_vo_btw_factor, new_values, new_factors, params_);
    graph_has_imu_btw_factor = true;
  }

  //================================= FACTOR GRAPH SAFETY CHECK ====================================
  if (!graph_has_vo_btw_factor && !graph_has_imu_btw_factor) {
    LOG(FATAL) << "Graph doesn't have a between factor from VO or IMU, so it might be under-constrained" << std::endl;
  }

  //==================================== UPDATE FACTOR GRAPH =======================================
  gtsam::ISAM2UpdateParams update_params;
  update_params.newAffectedKeys = std::move(factorNewAffectedKeys);
  const gtsam::ISAM2Result& isam_result = smoother_.update(new_factors, new_values, update_params);

  // Housekeeping: figure out what factor index has been assigned to each new factor.
  for (const auto &fct_to_lmk : map_new_factor_to_lmk_id) {
    lmk_to_factor_map_[fct_to_lmk.second] = isam_result.newFactorsIndices.at(fct_to_lmk.first);
  }

  // (Optional) run the smoother a few more times to reduce error.
  for (int i = 0; i < params_.extra_smoothing_iters; ++i) {
    smoother_.update();
  }

  //================================ RETRIEVE VARIABLE ESTIMATES ===================================
  const gtsam::Values& estimate = smoother_.calculateBestEstimate();

  result_ = SmootherResult(
      keypose_id,
      keypose_time,
      estimate.at<gtsam::Pose3>(keypose_sym),
      graph_has_imu_btw_factor,
      graph_has_imu_btw_factor ? estimate.at<gtsam::Vector3>(vel_sym) : kZeroVelocity,
      graph_has_imu_btw_factor ? estimate.at<ImuBias>(bias_sym) : kZeroImuBias);

  return result_;
}


SmootherResult Smoother::GetResult()
{
  result_lock_.lock();
  const SmootherResult out = result_;
  result_lock_.unlock();
  return out;
}


}
}
