// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html

// This file is based on file issued with the following license:

/*
BSD 3-Clause License

Copyright (c) 2020, George Terzakis
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// #include "cv/precomp.hpp"
#include "tier4_tag_utils/cv/sqpnp.hpp"

#include <opencv2/calib3d.hpp>
#include <opencv2/core/matx.hpp>
#include <opencv2/core/types.hpp>

template <typename _Tp, int m, int n>
cv::Matx<_Tp, m, n> div(const cv::Matx<_Tp, m, n> & a, double alpha)
{
  return cv::Matx<_Tp, m, n>(a, 1. / alpha, cv::Matx_ScaleOp());
}

namespace cv
{
namespace sqpnp
{
const double PoseSolver::RANK_TOLERANCE = 1e-7;
const double PoseSolver::SQP_SQUARED_TOLERANCE = 1e-10;
const double PoseSolver::SQP_DET_THRESHOLD = 1.001;
const double PoseSolver::ORTHOGONALITY_SQUARED_ERROR_THRESHOLD = 1e-8;
const double PoseSolver::EQUAL_VECTORS_SQUARED_DIFF = 1e-10;
const double PoseSolver::EQUAL_SQUARED_ERRORS_DIFF = 1e-6;
const double PoseSolver::POINT_VARIANCE_THRESHOLD = 1e-5;
const double PoseSolver::SQRT3 = std::sqrt(3);
const int PoseSolver::SQP_MAX_ITERATION = 15;

// No checking done here for overflow, since this is not public all call instances
// are assumed to be valid
template <typename tp, int snrows, int sncols, int dnrows, int dncols>
void set(
  int row, int col, cv::Matx<tp, dnrows, dncols> & dest,
  const cv::Matx<tp, snrows, sncols> & source)
{
  for (int y = 0; y < snrows; y++) {
    for (int x = 0; x < sncols; x++) {
      dest(row + y, col + x) = source(y, x);
    }
  }
}

PoseSolver::PoseSolver() : num_null_vectors_(-1), num_solutions_(0) {}

void PoseSolver::solve(
  InputArray objectPoints, InputArray imagePoints, OutputArrayOfArrays rvecs,
  OutputArrayOfArrays tvecs)
{
  // Input checking
  int objType = objectPoints.getMat().type();
  CV_CheckType(
    objType, objType == CV_32FC3 || objType == CV_64FC3,
    "Type of objectPoints must be CV_32FC3 or CV_64FC3");

  int imgType = imagePoints.getMat().type();
  CV_CheckType(
    imgType, imgType == CV_32FC2 || imgType == CV_64FC2,
    "Type of imagePoints must be CV_32FC2 or CV_64FC2");

  CV_Assert(objectPoints.rows() == 1 || objectPoints.cols() == 1);
  CV_Assert(objectPoints.rows() >= 3 || objectPoints.cols() >= 3);
  CV_Assert(imagePoints.rows() == 1 || imagePoints.cols() == 1);
  CV_Assert(imagePoints.rows() * imagePoints.cols() == objectPoints.rows() * objectPoints.cols());

  Mat _imagePoints;
  if (imgType == CV_32FC2) {
    imagePoints.getMat().convertTo(_imagePoints, CV_64F);
  } else {
    _imagePoints = imagePoints.getMat();
  }

  Mat _objectPoints;
  if (objType == CV_32FC3) {
    objectPoints.getMat().convertTo(_objectPoints, CV_64F);
  } else {
    _objectPoints = objectPoints.getMat();
  }

  num_null_vectors_ = -1;
  num_solutions_ = 0;

  computeOmega(_objectPoints, _imagePoints);
  solveInternal();

  int depthRot = rvecs.fixedType() ? rvecs.depth() : CV_64F;
  int depthTrans = tvecs.fixedType() ? tvecs.depth() : CV_64F;

  rvecs.create(
    num_solutions_, 1,
    CV_MAKETYPE(depthRot, rvecs.fixedType() && rvecs.kind() == _InputArray::STD_VECTOR ? 3 : 1));
  tvecs.create(
    num_solutions_, 1,
    CV_MAKETYPE(depthTrans, tvecs.fixedType() && tvecs.kind() == _InputArray::STD_VECTOR ? 3 : 1));

  for (int i = 0; i < num_solutions_; i++) {
    Mat rvec;
    Mat rotation = Mat(solutions_[i].r_hat).reshape(1, 3);
    Rodrigues(rotation, rvec);

    rvecs.getMatRef(i) = rvec;
    tvecs.getMatRef(i) = Mat(solutions_[i].t);
  }
}

void PoseSolver::computeOmega(InputArray objectPoints, InputArray imagePoints)
{
  omega_ = cv::Matx<double, 9, 9>::zeros();
  cv::Matx<double, 3, 9> qa_sum = cv::Matx<double, 3, 9>::zeros();

  cv::Point2d sum_img(0, 0);
  cv::Point3d sum_obj(0, 0, 0);
  double sq_norm_sum = 0;

  Mat _imagePoints = imagePoints.getMat();
  Mat _objectPoints = objectPoints.getMat();

  int n = _objectPoints.cols * _objectPoints.rows;

  for (int i = 0; i < n; i++) {
    const cv::Point2d & img_pt = _imagePoints.at<cv::Point2d>(i);
    const cv::Point3d & obj_pt = _objectPoints.at<cv::Point3d>(i);

    sum_img += img_pt;
    sum_obj += obj_pt;

    const double &x = img_pt.x, &y = img_pt.y;
    const double &X = obj_pt.x, &Y = obj_pt.y, &Z = obj_pt.z;
    double sq_norm = x * x + y * y;
    sq_norm_sum += sq_norm;

    double X2 = X * X, XY = X * Y, XZ = X * Z, Y2 = Y * Y, YZ = Y * Z, Z2 = Z * Z;

    omega_(0, 0) += X2;
    omega_(0, 1) += XY;
    omega_(0, 2) += XZ;
    omega_(1, 1) += Y2;
    omega_(1, 2) += YZ;
    omega_(2, 2) += Z2;

    // Populating this manually saves operations by only calculating upper triangle
    omega_(0, 6) += -x * X2;
    omega_(0, 7) += -x * XY;
    omega_(0, 8) += -x * XZ;
    omega_(1, 7) += -x * Y2;
    omega_(1, 8) += -x * YZ;
    omega_(2, 8) += -x * Z2;

    omega_(3, 6) += -y * X2;
    omega_(3, 7) += -y * XY;
    omega_(3, 8) += -y * XZ;
    omega_(4, 7) += -y * Y2;
    omega_(4, 8) += -y * YZ;
    omega_(5, 8) += -y * Z2;

    omega_(6, 6) += sq_norm * X2;
    omega_(6, 7) += sq_norm * XY;
    omega_(6, 8) += sq_norm * XZ;
    omega_(7, 7) += sq_norm * Y2;
    omega_(7, 8) += sq_norm * YZ;
    omega_(8, 8) += sq_norm * Z2;

    // Compute qa_sum
    qa_sum(0, 0) += X;
    qa_sum(0, 1) += Y;
    qa_sum(0, 2) += Z;
    qa_sum(1, 3) += X;
    qa_sum(1, 4) += Y;
    qa_sum(1, 5) += Z;

    qa_sum(0, 6) += -x * X;
    qa_sum(0, 7) += -x * Y;
    qa_sum(0, 8) += -x * Z;
    qa_sum(1, 6) += -y * X;
    qa_sum(1, 7) += -y * Y;
    qa_sum(1, 8) += -y * Z;

    qa_sum(2, 0) += -x * X;
    qa_sum(2, 1) += -x * Y;
    qa_sum(2, 2) += -x * Z;
    qa_sum(2, 3) += -y * X;
    qa_sum(2, 4) += -y * Y;
    qa_sum(2, 5) += -y * Z;

    qa_sum(2, 6) += sq_norm * X;
    qa_sum(2, 7) += sq_norm * Y;
    qa_sum(2, 8) += sq_norm * Z;
  }

  omega_(1, 6) = omega_(0, 7);
  omega_(2, 6) = omega_(0, 8);
  omega_(2, 7) = omega_(1, 8);
  omega_(4, 6) = omega_(3, 7);
  omega_(5, 6) = omega_(3, 8);
  omega_(5, 7) = omega_(4, 8);
  omega_(7, 6) = omega_(6, 7);
  omega_(8, 6) = omega_(6, 8);
  omega_(8, 7) = omega_(7, 8);

  omega_(3, 3) = omega_(0, 0);
  omega_(3, 4) = omega_(0, 1);
  omega_(3, 5) = omega_(0, 2);
  omega_(4, 4) = omega_(1, 1);
  omega_(4, 5) = omega_(1, 2);
  omega_(5, 5) = omega_(2, 2);

  // Mirror upper triangle to lower triangle
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < r; c++) {
      omega_(r, c) = omega_(c, r);
    }
  }

  cv::Matx<double, 3, 3> q;
  q(0, 0) = n;
  q(0, 1) = 0;
  q(0, 2) = -sum_img.x;
  q(1, 0) = 0;
  q(1, 1) = n;
  q(1, 2) = -sum_img.y;
  q(2, 0) = -sum_img.x;
  q(2, 1) = -sum_img.y;
  q(2, 2) = sq_norm_sum;

  double inv_n = 1.0 / n;
  double detQ = n * (n * sq_norm_sum - sum_img.y * sum_img.y - sum_img.x * sum_img.x);
  double point_coordinate_variance = detQ * inv_n * inv_n * inv_n;

  CV_Assert(point_coordinate_variance >= POINT_VARIANCE_THRESHOLD);

  Matx<double, 3, 3> q_inv;
  analyticalInverse3x3Symm(q, q_inv);

  p_ = -q_inv * qa_sum;

  omega_ += qa_sum.t() * p_;

  cv::SVD omega_svd(omega_, cv::SVD::FULL_UV);
  s_ = omega_svd.w;
  u_ = cv::Mat(omega_svd.vt.t());

  CV_Assert(s_(0) >= 1e-7);

  while (s_(7 - num_null_vectors_) < RANK_TOLERANCE) num_null_vectors_++;

  CV_Assert(++num_null_vectors_ <= 6);

  point_mean_ = cv::Vec3d(sum_obj.x / n, sum_obj.y / n, sum_obj.z / n);
}

void PoseSolver::solveInternal()
{
  double min_sq_err = std::numeric_limits<double>::max();
  int num_eigen_points = num_null_vectors_ > 0 ? num_null_vectors_ : 1;

  for (int i = 9 - num_eigen_points; i < 9; i++) {
    const cv::Matx<double, 9, 1> e = SQRT3 * u_.col(i);
    double orthogonality_sq_err = orthogonalityError(e);

    SQPSolution solutions[2];

    // If e is orthogonal, we can skip SQP
    if (orthogonality_sq_err < ORTHOGONALITY_SQUARED_ERROR_THRESHOLD) {
      solutions[0].r_hat = det3x3(e) * e;
      solutions[0].t = p_ * solutions[0].r_hat;
      checkSolution(solutions[0], min_sq_err);
    } else {
      Matx<double, 9, 1> r;
      nearestRotationMatrix(e, r);
      solutions[0] = runSQP(r);
      solutions[0].t = p_ * solutions[0].r_hat;
      checkSolution(solutions[0], min_sq_err);

      nearestRotationMatrix(-e, r);
      solutions[1] = runSQP(r);
      solutions[1].t = p_ * solutions[1].r_hat;
      checkSolution(solutions[1], min_sq_err);
    }
  }

  int c = 1;

  while (min_sq_err > 3 * s_[9 - num_eigen_points - c] && 9 - num_eigen_points - c > 0) {
    int index = 9 - num_eigen_points - c;

    const cv::Matx<double, 9, 1> e = u_.col(index);
    SQPSolution solutions[2];

    Matx<double, 9, 1> r;
    nearestRotationMatrix(e, r);
    solutions[0] = runSQP(r);
    solutions[0].t = p_ * solutions[0].r_hat;
    checkSolution(solutions[0], min_sq_err);

    nearestRotationMatrix(-e, r);
    solutions[1] = runSQP(r);
    solutions[1].t = p_ * solutions[1].r_hat;
    checkSolution(solutions[1], min_sq_err);

    c++;
  }
}

PoseSolver::SQPSolution PoseSolver::runSQP(const cv::Matx<double, 9, 1> & r0)
{
  cv::Matx<double, 9, 1> r = r0;

  double delta_squared_norm = std::numeric_limits<double>::max();
  cv::Matx<double, 9, 1> delta;

  int step = 0;
  while (delta_squared_norm > SQP_SQUARED_TOLERANCE && step++ < SQP_MAX_ITERATION) {
    solveSQPSystem(r, delta);
    r += delta;
    delta_squared_norm = cv::norm(delta, cv::NORM_L2SQR);
  }

  SQPSolution solution;

  double det_r = det3x3(r);
  if (det_r < 0) {
    r = -r;
    det_r = -det_r;
  }

  if (det_r > SQP_DET_THRESHOLD) {
    nearestRotationMatrix(r, solution.r_hat);
  } else {
    solution.r_hat = r;
  }

  return solution;
}

void PoseSolver::solveSQPSystem(const cv::Matx<double, 9, 1> & r, cv::Matx<double, 9, 1> & delta)
{
  double sqnorm_r1 = r(0) * r(0) + r(1) * r(1) + r(2) * r(2),
         sqnorm_r2 = r(3) * r(3) + r(4) * r(4) + r(5) * r(5),
         sqnorm_r3 = r(6) * r(6) + r(7) * r(7) + r(8) * r(8);
  double dot_r1r2 = r(0) * r(3) + r(1) * r(4) + r(2) * r(5),
         dot_r1r3 = r(0) * r(6) + r(1) * r(7) + r(2) * r(8),
         dot_r2r3 = r(3) * r(6) + r(4) * r(7) + r(5) * r(8);

  cv::Matx<double, 9, 3> N;
  cv::Matx<double, 9, 6> H;
  cv::Matx<double, 6, 6> JH;

  computeRowAndNullspace(r, H, N, JH);

  cv::Matx<double, 6, 1> g;
  g(0) = 1 - sqnorm_r1;
  g(1) = 1 - sqnorm_r2;
  g(2) = 1 - sqnorm_r3;
  g(3) = -dot_r1r2;
  g(4) = -dot_r2r3;
  g(5) = -dot_r1r3;

  cv::Matx<double, 6, 1> x;
  x(0) = g(0) / JH(0, 0);
  x(1) = g(1) / JH(1, 1);
  x(2) = g(2) / JH(2, 2);
  x(3) = (g(3) - JH(3, 0) * x(0) - JH(3, 1) * x(1)) / JH(3, 3);
  x(4) = (g(4) - JH(4, 1) * x(1) - JH(4, 2) * x(2) - JH(4, 3) * x(3)) / JH(4, 4);
  x(5) = (g(5) - JH(5, 0) * x(0) - JH(5, 2) * x(2) - JH(5, 3) * x(3) - JH(5, 4) * x(4)) / JH(5, 5);

  delta = H * x;

  cv::Matx<double, 3, 9> nt_omega = N.t() * omega_;
  cv::Matx<double, 3, 3> W = nt_omega * N, W_inv;

  analyticalInverse3x3Symm(W, W_inv);

  cv::Matx<double, 3, 1> y = -W_inv * nt_omega * (delta + r);
  delta += N * y;
}

bool PoseSolver::analyticalInverse3x3Symm(
  const cv::Matx<double, 3, 3> & Q, cv::Matx<double, 3, 3> & Qinv, const double & threshold)
{
  // 1. Get the elements of the matrix
  double a = Q(0, 0), b = Q(1, 0), d = Q(1, 1), c = Q(2, 0), e = Q(2, 1), f = Q(2, 2);

  // 2. Determinant
  double t2, t4, t7, t9, t12;
  t2 = e * e;
  t4 = a * d;
  t7 = b * b;
  t9 = b * c;
  t12 = c * c;
  double det = -t4 * f + a * t2 + t7 * f - 2.0 * t9 * e + t12 * d;

  if (fabs(det) < threshold) return false;

  // 3. Inverse
  double t15, t20, t24, t30;
  t15 = 1.0 / det;
  t20 = (-b * f + c * e) * t15;
  t24 = (b * e - c * d) * t15;
  t30 = (a * e - t9) * t15;
  Qinv(0, 0) = (-d * f + t2) * t15;
  Qinv(0, 1) = Qinv(1, 0) = -t20;
  Qinv(0, 2) = Qinv(2, 0) = -t24;
  Qinv(1, 1) = -(a * f - t12) * t15;
  Qinv(1, 2) = Qinv(2, 1) = t30;
  Qinv(2, 2) = -(t4 - t7) * t15;

  return true;
}

void PoseSolver::computeRowAndNullspace(
  const cv::Matx<double, 9, 1> & r, cv::Matx<double, 9, 6> & H, cv::Matx<double, 9, 3> & N,
  cv::Matx<double, 6, 6> & K, const double & norm_threshold)
{
  H = cv::Matx<double, 9, 6>::zeros();

  // 1. q1
  double norm_r1 = sqrt(r(0) * r(0) + r(1) * r(1) + r(2) * r(2));
  double inv_norm_r1 = norm_r1 > 1e-5 ? 1.0 / norm_r1 : 0.0;
  H(0, 0) = r(0) * inv_norm_r1;
  H(1, 0) = r(1) * inv_norm_r1;
  H(2, 0) = r(2) * inv_norm_r1;
  K(0, 0) = 2 * norm_r1;

  // 2. q2
  double norm_r2 = sqrt(r(3) * r(3) + r(4) * r(4) + r(5) * r(5));
  double inv_norm_r2 = 1.0 / norm_r2;
  H(3, 1) = r(3) * inv_norm_r2;
  H(4, 1) = r(4) * inv_norm_r2;
  H(5, 1) = r(5) * inv_norm_r2;
  K(1, 0) = 0;
  K(1, 1) = 2 * norm_r2;

  // 3. q3 = (r3'*q2)*q2 - (r3'*q1)*q1 ; q3 = q3/norm(q3)
  double norm_r3 = sqrt(r(6) * r(6) + r(7) * r(7) + r(8) * r(8));
  double inv_norm_r3 = 1.0 / norm_r3;
  H(6, 2) = r(6) * inv_norm_r3;
  H(7, 2) = r(7) * inv_norm_r3;
  H(8, 2) = r(8) * inv_norm_r3;
  K(2, 0) = K(2, 1) = 0;
  K(2, 2) = 2 * norm_r3;

  // 4. q4
  double dot_j4q1 = r(3) * H(0, 0) + r(4) * H(1, 0) + r(5) * H(2, 0),
         dot_j4q2 = r(0) * H(3, 1) + r(1) * H(4, 1) + r(2) * H(5, 1);

  H(0, 3) = r(3) - dot_j4q1 * H(0, 0);
  H(1, 3) = r(4) - dot_j4q1 * H(1, 0);
  H(2, 3) = r(5) - dot_j4q1 * H(2, 0);
  H(3, 3) = r(0) - dot_j4q2 * H(3, 1);
  H(4, 3) = r(1) - dot_j4q2 * H(4, 1);
  H(5, 3) = r(2) - dot_j4q2 * H(5, 1);
  double inv_norm_j4 = 1.0 / sqrt(
                               H(0, 3) * H(0, 3) + H(1, 3) * H(1, 3) + H(2, 3) * H(2, 3) +
                               H(3, 3) * H(3, 3) + H(4, 3) * H(4, 3) + H(5, 3) * H(5, 3));

  H(0, 3) *= inv_norm_j4;
  H(1, 3) *= inv_norm_j4;
  H(2, 3) *= inv_norm_j4;
  H(3, 3) *= inv_norm_j4;
  H(4, 3) *= inv_norm_j4;
  H(5, 3) *= inv_norm_j4;

  K(3, 0) = r(3) * H(0, 0) + r(4) * H(1, 0) + r(5) * H(2, 0);
  K(3, 1) = r(0) * H(3, 1) + r(1) * H(4, 1) + r(2) * H(5, 1);
  K(3, 2) = 0;
  K(3, 3) = r(3) * H(0, 3) + r(4) * H(1, 3) + r(5) * H(2, 3) + r(0) * H(3, 3) + r(1) * H(4, 3) +
            r(2) * H(5, 3);

  // 5. q5
  double dot_j5q2 = r(6) * H(3, 1) + r(7) * H(4, 1) + r(8) * H(5, 1);
  double dot_j5q3 = r(3) * H(6, 2) + r(4) * H(7, 2) + r(5) * H(8, 2);
  double dot_j5q4 = r(6) * H(3, 3) + r(7) * H(4, 3) + r(8) * H(5, 3);

  H(0, 4) = -dot_j5q4 * H(0, 3);
  H(1, 4) = -dot_j5q4 * H(1, 3);
  H(2, 4) = -dot_j5q4 * H(2, 3);
  H(3, 4) = r(6) - dot_j5q2 * H(3, 1) - dot_j5q4 * H(3, 3);
  H(4, 4) = r(7) - dot_j5q2 * H(4, 1) - dot_j5q4 * H(4, 3);
  H(5, 4) = r(8) - dot_j5q2 * H(5, 1) - dot_j5q4 * H(5, 3);
  H(6, 4) = r(3) - dot_j5q3 * H(6, 2);
  H(7, 4) = r(4) - dot_j5q3 * H(7, 2);
  H(8, 4) = r(5) - dot_j5q3 * H(8, 2);

  Matx<double, 9, 1> q4 = H.col(4);
  q4 = div(q4, cv::norm(q4));
  set<double, 9, 1, 9, 6>(0, 4, H, q4);

  K(4, 0) = 0;
  K(4, 1) = r(6) * H(3, 1) + r(7) * H(4, 1) + r(8) * H(5, 1);
  K(4, 2) = r(3) * H(6, 2) + r(4) * H(7, 2) + r(5) * H(8, 2);
  K(4, 3) = r(6) * H(3, 3) + r(7) * H(4, 3) + r(8) * H(5, 3);
  K(4, 4) = r(6) * H(3, 4) + r(7) * H(4, 4) + r(8) * H(5, 4) + r(3) * H(6, 4) + r(4) * H(7, 4) +
            r(5) * H(8, 4);

  // 4. q6
  double dot_j6q1 = r(6) * H(0, 0) + r(7) * H(1, 0) + r(8) * H(2, 0);
  double dot_j6q3 = r(0) * H(6, 2) + r(1) * H(7, 2) + r(2) * H(8, 2);
  double dot_j6q4 = r(6) * H(0, 3) + r(7) * H(1, 3) + r(8) * H(2, 3);
  double dot_j6q5 = r(0) * H(6, 4) + r(1) * H(7, 4) + r(2) * H(8, 4) + r(6) * H(0, 4) +
                    r(7) * H(1, 4) + r(8) * H(2, 4);

  H(0, 5) = r(6) - dot_j6q1 * H(0, 0) - dot_j6q4 * H(0, 3) - dot_j6q5 * H(0, 4);
  H(1, 5) = r(7) - dot_j6q1 * H(1, 0) - dot_j6q4 * H(1, 3) - dot_j6q5 * H(1, 4);
  H(2, 5) = r(8) - dot_j6q1 * H(2, 0) - dot_j6q4 * H(2, 3) - dot_j6q5 * H(2, 4);

  H(3, 5) = -dot_j6q5 * H(3, 4) - dot_j6q4 * H(3, 3);
  H(4, 5) = -dot_j6q5 * H(4, 4) - dot_j6q4 * H(4, 3);
  H(5, 5) = -dot_j6q5 * H(5, 4) - dot_j6q4 * H(5, 3);

  H(6, 5) = r(0) - dot_j6q3 * H(6, 2) - dot_j6q5 * H(6, 4);
  H(7, 5) = r(1) - dot_j6q3 * H(7, 2) - dot_j6q5 * H(7, 4);
  H(8, 5) = r(2) - dot_j6q3 * H(8, 2) - dot_j6q5 * H(8, 4);

  Matx<double, 9, 1> q5 = H.col(5);
  q5 = div(q5, cv::norm(q5));
  set<double, 9, 1, 9, 6>(0, 5, H, q5);

  K(5, 0) = r(6) * H(0, 0) + r(7) * H(1, 0) + r(8) * H(2, 0);
  K(5, 1) = 0;
  K(5, 2) = r(0) * H(6, 2) + r(1) * H(7, 2) + r(2) * H(8, 2);
  K(5, 3) = r(6) * H(0, 3) + r(7) * H(1, 3) + r(8) * H(2, 3);
  K(5, 4) = r(6) * H(0, 4) + r(7) * H(1, 4) + r(8) * H(2, 4) + r(0) * H(6, 4) + r(1) * H(7, 4) +
            r(2) * H(8, 4);
  K(5, 5) = r(6) * H(0, 5) + r(7) * H(1, 5) + r(8) * H(2, 5) + r(0) * H(6, 5) + r(1) * H(7, 5) +
            r(2) * H(8, 5);

  // Great! Now H is an orthogonalized, sparse basis of the Jacobian row space and K is filled.
  //
  // Now get a projector onto the null space H:
  const cv::Matx<double, 9, 9> Pn = cv::Matx<double, 9, 9>::eye() - (H * H.t());

  // Now we need to pick 3 columns of P with non-zero norm (> 0.3) and some angle between them (>
  // 0.3).
  //
  // Find the 3 columns of Pn with largest norms
  int index1 = 0, index2 = 0, index3 = 0;
  double max_norm1 = std::numeric_limits<double>::min();
  double min_dot12 = std::numeric_limits<double>::max();
  double min_dot1323 = std::numeric_limits<double>::max();

  double col_norms[9];
  for (int i = 0; i < 9; i++) {
    col_norms[i] = cv::norm(Pn.col(i));
    if (col_norms[i] >= norm_threshold) {
      if (max_norm1 < col_norms[i]) {
        max_norm1 = col_norms[i];
        index1 = i;
      }
    }
  }

  Matx<double, 9, 1> v1 = Pn.col(index1);
  v1 = div(v1, max_norm1);
  set<double, 9, 1, 9, 3>(0, 0, N, v1);

  for (int i = 0; i < 9; i++) {
    if (i == index1) continue;
    if (col_norms[i] >= norm_threshold) {
      double cos_v1_x_col = fabs(Pn.col(i).dot(v1) / col_norms[i]);

      if (cos_v1_x_col <= min_dot12) {
        index2 = i;
        min_dot12 = cos_v1_x_col;
      }
    }
  }

  Matx<double, 9, 1> v2 = Pn.col(index2);
  Matx<double, 9, 1> n0 = N.col(0);
  v2 -= v2.dot(n0) * n0;
  v2 = div(v2, cv::norm(v2));
  set<double, 9, 1, 9, 3>(0, 1, N, v2);

  for (int i = 0; i < 9; i++) {
    if (i == index2 || i == index1) continue;
    if (col_norms[i] >= norm_threshold) {
      double cos_v1_x_col = fabs(Pn.col(i).dot(v1) / col_norms[i]);
      double cos_v2_x_col = fabs(Pn.col(i).dot(v2) / col_norms[i]);

      if (cos_v1_x_col + cos_v2_x_col <= min_dot1323) {
        index3 = i;
        min_dot1323 = cos_v2_x_col + cos_v2_x_col;
      }
    }
  }

  Matx<double, 9, 1> v3 = Pn.col(index3);
  Matx<double, 9, 1> n1 = N.col(1);
  v3 -= (v3.dot(n1)) * n1 - (v3.dot(n0)) * n0;
  v3 = div(v3, cv::norm(v3));
  set<double, 9, 1, 9, 3>(0, 2, N, v3);
}

// faster nearest rotation computation based on FOAM (see:
// http://users.ics.forth.gr/~lourakis/publ/2018_iros.pdf )
/* Solve the nearest orthogonal approximation problem
 * i.e., given e, find R minimizing ||R-e||_F
 *
 * The computation borrows from Markley's FOAM algorithm
 * "Attitude Determination Using Vector Observations: A Fast Optimal Matrix Algorithm", J.
 * Astronaut. Sci.
 *
 * See also M. Lourakis: "An Efficient Solution to Absolute Orientation", ICPR 2016
 *
 *  Copyright (C) 2019 Manolis Lourakis (lourakis **at** ics forth gr)
 *  Institute of Computer Science, Foundation for Research & Technology - Hellas
 *  Heraklion, Crete, Greece.
 */
void PoseSolver::nearestRotationMatrix(const cv::Matx<double, 9, 1> & e, cv::Matx<double, 9, 1> & r)
{
  int i;
  double l, lprev, det_e, e_sq, adj_e_sq, adj_e[9];

  // e's adjoint
  adj_e[0] = e(4) * e(8) - e(5) * e(7);
  adj_e[1] = e(2) * e(7) - e(1) * e(8);
  adj_e[2] = e(1) * e(5) - e(2) * e(4);
  adj_e[3] = e(5) * e(6) - e(3) * e(8);
  adj_e[4] = e(0) * e(8) - e(2) * e(6);
  adj_e[5] = e(2) * e(3) - e(0) * e(5);
  adj_e[6] = e(3) * e(7) - e(4) * e(6);
  adj_e[7] = e(1) * e(6) - e(0) * e(7);
  adj_e[8] = e(0) * e(4) - e(1) * e(3);

  // det(e), ||e||^2, ||adj(e)||^2
  det_e = e(0) * e(4) * e(8) - e(0) * e(5) * e(7) - e(1) * e(3) * e(8) + e(2) * e(3) * e(7) +
          e(1) * e(6) * e(5) - e(2) * e(6) * e(4);
  e_sq = e(0) * e(0) + e(1) * e(1) + e(2) * e(2) + e(3) * e(3) + e(4) * e(4) + e(5) * e(5) +
         e(6) * e(6) + e(7) * e(7) + e(8) * e(8);
  adj_e_sq = adj_e[0] * adj_e[0] + adj_e[1] * adj_e[1] + adj_e[2] * adj_e[2] + adj_e[3] * adj_e[3] +
             adj_e[4] * adj_e[4] + adj_e[5] * adj_e[5] + adj_e[6] * adj_e[6] + adj_e[7] * adj_e[7] +
             adj_e[8] * adj_e[8];

  // compute l_max with Newton-Raphson from FOAM's characteristic polynomial, i.e. eq.(23) - (26)
  for (i = 200, l = 2.0, lprev = 0.0; fabs(l - lprev) > 1E-12 * fabs(lprev) && i > 0; --i) {
    double tmp, p, pp;

    tmp = (l * l - e_sq);
    p = (tmp * tmp - 8.0 * l * det_e - 4.0 * adj_e_sq);
    pp = 8.0 * (0.5 * tmp * l - det_e);

    lprev = l;
    l -= p / pp;
  }

  // the rotation matrix equals ((l^2 + e_sq)*e + 2*l*adj(e') - 2*e*e'*e) / (l*(l*l-e_sq) -
  // 2*det(e)), i.e. eq.(14) using (18), (19)
  {
    // compute (l^2 + e_sq)*e
    double tmp[9], e_et[9], denom;
    const double a = l * l + e_sq;

    // e_et=e*e'
    e_et[0] = e(0) * e(0) + e(1) * e(1) + e(2) * e(2);
    e_et[1] = e(0) * e(3) + e(1) * e(4) + e(2) * e(5);
    e_et[2] = e(0) * e(6) + e(1) * e(7) + e(2) * e(8);

    e_et[3] = e_et[1];
    e_et[4] = e(3) * e(3) + e(4) * e(4) + e(5) * e(5);
    e_et[5] = e(3) * e(6) + e(4) * e(7) + e(5) * e(8);

    e_et[6] = e_et[2];
    e_et[7] = e_et[5];
    e_et[8] = e(6) * e(6) + e(7) * e(7) + e(8) * e(8);

    // tmp=e_et*e
    tmp[0] = e_et[0] * e(0) + e_et[1] * e(3) + e_et[2] * e(6);
    tmp[1] = e_et[0] * e(1) + e_et[1] * e(4) + e_et[2] * e(7);
    tmp[2] = e_et[0] * e(2) + e_et[1] * e(5) + e_et[2] * e(8);

    tmp[3] = e_et[3] * e(0) + e_et[4] * e(3) + e_et[5] * e(6);
    tmp[4] = e_et[3] * e(1) + e_et[4] * e(4) + e_et[5] * e(7);
    tmp[5] = e_et[3] * e(2) + e_et[4] * e(5) + e_et[5] * e(8);

    tmp[6] = e_et[6] * e(0) + e_et[7] * e(3) + e_et[8] * e(6);
    tmp[7] = e_et[6] * e(1) + e_et[7] * e(4) + e_et[8] * e(7);
    tmp[8] = e_et[6] * e(2) + e_et[7] * e(5) + e_et[8] * e(8);

    // compute R as (a*e + 2*(l*adj(e)' - tmp))*denom; note that adj(e')=adj(e)'
    denom = l * (l * l - e_sq) - 2.0 * det_e;
    denom = 1.0 / denom;
    r(0) = (a * e(0) + 2.0 * (l * adj_e[0] - tmp[0])) * denom;
    r(1) = (a * e(1) + 2.0 * (l * adj_e[3] - tmp[1])) * denom;
    r(2) = (a * e(2) + 2.0 * (l * adj_e[6] - tmp[2])) * denom;

    r(3) = (a * e(3) + 2.0 * (l * adj_e[1] - tmp[3])) * denom;
    r(4) = (a * e(4) + 2.0 * (l * adj_e[4] - tmp[4])) * denom;
    r(5) = (a * e(5) + 2.0 * (l * adj_e[7] - tmp[5])) * denom;

    r(6) = (a * e(6) + 2.0 * (l * adj_e[2] - tmp[6])) * denom;
    r(7) = (a * e(7) + 2.0 * (l * adj_e[5] - tmp[7])) * denom;
    r(8) = (a * e(8) + 2.0 * (l * adj_e[8] - tmp[8])) * denom;
  }
}

double PoseSolver::det3x3(const cv::Matx<double, 9, 1> & e)
{
  return e(0) * e(4) * e(8) + e(1) * e(5) * e(6) + e(2) * e(3) * e(7) - e(6) * e(4) * e(2) -
         e(7) * e(5) * e(0) - e(8) * e(3) * e(1);
}

inline bool PoseSolver::positiveDepth(const SQPSolution & solution) const
{
  const cv::Matx<double, 9, 1> & r = solution.r_hat;
  const cv::Matx<double, 3, 1> & t = solution.t;
  const cv::Vec3d & mean = point_mean_;
  return (r(6) * mean(0) + r(7) * mean(1) + r(8) * mean(2) + t(2) > 0);
}

void PoseSolver::checkSolution(SQPSolution & solution, double & min_error)
{
  if (positiveDepth(solution)) {
    solution.sq_error = (omega_ * solution.r_hat).ddot(solution.r_hat);
    if (fabs(min_error - solution.sq_error) > EQUAL_SQUARED_ERRORS_DIFF) {
      if (min_error > solution.sq_error) {
        min_error = solution.sq_error;
        solutions_[0] = solution;
        num_solutions_ = 1;
      }
    } else {
      bool found = false;
      for (int i = 0; i < num_solutions_; i++) {
        if (
          cv::norm(solutions_[i].r_hat - solution.r_hat, cv::NORM_L2SQR) <
          EQUAL_VECTORS_SQUARED_DIFF) {
          if (solutions_[i].sq_error > solution.sq_error) {
            solutions_[i] = solution;
          }
          found = true;
          break;
        }
      }

      if (!found) {
        solutions_[num_solutions_++] = solution;
      }
      if (min_error > solution.sq_error) min_error = solution.sq_error;
    }
  }
}

double PoseSolver::orthogonalityError(const cv::Matx<double, 9, 1> & e)
{
  double sq_norm_e1 = e(0) * e(0) + e(1) * e(1) + e(2) * e(2);
  double sq_norm_e2 = e(3) * e(3) + e(4) * e(4) + e(5) * e(5);
  double sq_norm_e3 = e(6) * e(6) + e(7) * e(7) + e(8) * e(8);
  double dot_e1e2 = e(0) * e(3) + e(1) * e(4) + e(2) * e(5);
  double dot_e1e3 = e(0) * e(6) + e(1) * e(7) + e(2) * e(8);
  double dot_e2e3 = e(3) * e(6) + e(4) * e(7) + e(5) * e(8);

  return (sq_norm_e1 - 1) * (sq_norm_e1 - 1) + (sq_norm_e2 - 1) * (sq_norm_e2 - 1) +
         (sq_norm_e3 - 1) * (sq_norm_e3 - 1) +
         2 * (dot_e1e2 * dot_e1e2 + dot_e1e3 * dot_e1e3 + dot_e2e3 * dot_e2e3);
}

}  // namespace sqpnp
}  // namespace cv
