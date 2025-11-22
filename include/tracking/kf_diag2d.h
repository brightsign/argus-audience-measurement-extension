#pragma once
#include "byte_types.h"

namespace bytetrack {

// Very small diagonal 2D box tracker: state = [cx,cy,w,h, vx,vy] (6D)
// Diagonal covariance (independent dims) to avoid matrix libraries.
// It behaves like a proper constant-velocity KF but with diagonal cov/Q/R.

struct KFDiag2D {
  // State
  float cx=0, cy=0, w=0, h=0, vx=0, vy=0;

  // Diagonal covariance for [cx,cy,w,h,vx,vy]
  float P[6] = {0};

  // Process / measurement noise (diagonal)
  struct Params {
    float q_pos = 3.0f;     // process noise pos/size (px per step)
    float q_vel = 10.0f;    // process noise vel (px/s per step)
    float r_pos = 8.0f;     // meas noise pos/size (px)
  } prm;

  void init(const BBox& b, float init_vel_var = 100.f) {
    cx = b.cx(); cy = b.cy(); w = std::max(1.f,b.w()); h = std::max(1.f,b.h());
    vx=0; vy=0;
    // Conservative initial covariance
    P[0]=P[1]=P[2]=P[3]=prm.r_pos*prm.r_pos;
    P[4]=P[5]=init_vel_var;
  }

  void predict(float dt) {
    if (dt <= 0) dt = 1.f/30.f;
    // x_k+ = F x_k, with F integrating velocity for cx,cy only
    cx += vx * dt;
    cy += vy * dt;

    // Covariance update (diagonal):
    // P_pos += Q_pos; P_vel += Q_vel
    const float qpos = prm.q_pos*prm.q_pos;
    const float qvel = prm.q_vel*prm.q_vel;
    // position cov also grows from velocity uncertainty over dt
    // For a simple diagonal approx:
    P[0] += qpos + P[4]*dt*dt; // cx
    P[1] += qpos + P[5]*dt*dt; // cy
    P[2] += qpos;              // w
    P[3] += qpos;              // h
    P[4] += qvel;              // vx
    P[5] += qvel;              // vy
  }

  // Measurement z = [cx,cy,w,h]
  void update(const BBox& meas) {
    const float z[4] = { meas.cx(), meas.cy(), std::max(1.f,meas.w()), std::max(1.f,meas.h()) };
    float xhat[4]    = { cx, cy, w, h };
    float* Pdiag[4]  = { &P[0], &P[1], &P[2], &P[3] }; // the measured dims

    const float R = prm.r_pos*prm.r_pos; // same for all measured dims

    // per-dimension scalar Kalman update
    for (int i=0;i<4;++i) {
      const float S  = (*Pdiag[i]) + R + 1e-6f; // innovation variance
      const float K  = (*Pdiag[i]) / S;         // scalar gain
      const float y  = z[i] - xhat[i];          // innovation
      // Apply to state
      if (i==0) cx += K*y;
      else if (i==1) cy += K*y;
      else if (i==2) w  += K*y;
      else          h  += K*y;
      // Covariance shrink
      (*Pdiag[i]) = (1.f - K) * (*Pdiag[i]);
    }

    // Optionally 'nudge' velocity from position residual (helps snappier response)
    // vx,vy gain is tiny (alpha)
    const float alpha = 0.1f;
    vx += alpha * (z[0] - xhat[0]);
    vy += alpha * (z[1] - xhat[1]);
  }

  BBox to_bbox() const {
    return BBox{ cx - 0.5f*w, cy - 0.5f*h, cx + 0.5f*w, cy + 0.5f*h };
  }
};

}  // namespace bytetrack
