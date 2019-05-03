#include <pcl_conversions/pcl_conversions.h>

#include "ouster/os1.h"
#include "ouster/os1_packet.h"
#include "ouster_ros/os1_ros.h"

#include <pcl/filters/frustum_culling.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

namespace ouster_ros {
namespace OS1 {

using namespace ouster::OS1;

sensor_msgs::PointCloud2 publishFovTrimmedCloud(float hfov, float vfov, Eigen::Matrix4f ousterPose, sensor_msgs::PointCloud2 msg) {
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloudPtr(
      new pcl::PointCloud<pcl::PointXYZ>);
  //Converting from ROS PointCloud2 to pcl::PointCloud<pcl::PointXYZ>
  //Note that converting to CloudOS1 causes linking error since precompiled pcl library doesn't know about CloudOS1 datatype.
  pcl::fromROSMsg(msg, *cloudPtr);
  //from http://docs.pointclouds.org/trunk/classpcl_1_1_frustum_culling.html
  pcl::FrustumCulling<pcl::PointXYZ> fc;
  fc.setInputCloud(cloudPtr);
  fc.setVerticalFOV(vfov);
  fc.setHorizontalFOV(hfov);
  //fc.setHorizontalFOV(120);

  fc.setNearPlaneDistance(0.01);
  fc.setFarPlaneDistance(100);
  Eigen::Matrix4f ouster2frustum; // this thing assusms pose x-forward, y-up, z right. rotate
  // -90 along x-axis to align with x-forward, y-left, z-up.
  ouster2frustum << -1, 0, 0, 0,
      0, -1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1;
  // init2lidar <<  1, 0, 0, 0,
  //                0, 0, -1, 0,
  //                0, 1, 0, 0,
  //                0, 0, 0, 1;
  Eigen::Matrix4f pose_new = ousterPose * ouster2frustum;
  fc.setCameraPose(pose_new);
  pcl::PointCloud<pcl::PointXYZ> filtered_pc;
  fc.filter(filtered_pc);
  sensor_msgs::PointCloud2 ret;
  pcl::toROSMsg(filtered_pc, ret);
  ret.header.frame_id = msg.header.frame_id;
  ret.header = msg.header;
  return ret;
}

ns timestamp_of_imu_packet(const PacketMsg& pm) {
    return ns(imu_gyro_ts(pm.buf.data()));
}

ns timestamp_of_lidar_packet(const PacketMsg& pm) {
    return ns(col_timestamp(nth_col(0, pm.buf.data())));
}

bool read_imu_packet(const client& cli, PacketMsg& m) {
    m.buf.resize(imu_packet_bytes + 1);
    return read_imu_packet(cli, m.buf.data());
}

bool read_lidar_packet(const client& cli, PacketMsg& m) {
    m.buf.resize(lidar_packet_bytes + 1);
    return read_lidar_packet(cli, m.buf.data());
}

sensor_msgs::Imu packet_to_imu_msg(const PacketMsg& p) {
    const double standard_g = 9.80665;
    sensor_msgs::Imu m;
    const uint8_t* buf = p.buf.data();

    m.header.stamp.fromNSec(imu_gyro_ts(buf));
    m.header.frame_id = "os1_imu";

    m.orientation.x = 0;
    m.orientation.y = 0;
    m.orientation.z = 0;
    m.orientation.w = 0;

    m.linear_acceleration.x = imu_la_x(buf) * standard_g;
    m.linear_acceleration.y = imu_la_y(buf) * standard_g;
    m.linear_acceleration.z = imu_la_z(buf) * standard_g;

    m.angular_velocity.x = imu_av_x(buf) * M_PI / 180.0;
    m.angular_velocity.y = imu_av_y(buf) * M_PI / 180.0;
    m.angular_velocity.z = imu_av_z(buf) * M_PI / 180.0;

    for (int i = 0; i < 9; i++) {
        m.orientation_covariance[i] = -1;
        m.angular_velocity_covariance[i] = 0;
        m.linear_acceleration_covariance[i] = 0;
    }
    for (int i = 0; i < 9; i += 4) {
        m.linear_acceleration_covariance[i] = 0.01;
        m.angular_velocity_covariance[i] = 6e-4;
    }

    return m;
}

sensor_msgs::PointCloud2 cloud_to_cloud_msg(const CloudOS1& cloud, ns timestamp,
                                            const std::string& frame) {
    sensor_msgs::PointCloud2 msg;
    pcl::toROSMsg(cloud, msg);
    msg.header.frame_id = frame;
    msg.header.stamp.fromNSec(timestamp.count());
    return msg;
}

static PointOS1 nth_point(int ind, const uint8_t* col_buf) {
    float h_angle_0 = col_h_angle(col_buf);
    auto tte = trig_table[ind];
    const uint8_t* px_buf = nth_px(ind, col_buf);
    float r = px_range(px_buf) / 1000.0;
    float h_angle = tte.beam_azimuth_angles + h_angle_0;

    PointOS1 point;
    point.reflectivity = px_reflectivity(px_buf);
    point.intensity = px_signal_photons(px_buf);
    point.x = -r * tte.cos_beam_altitude_angles * cosf(h_angle);
    point.y = r * tte.cos_beam_altitude_angles * sinf(h_angle);
    point.z = r * tte.sin_beam_altitude_angles;
    point.ring = ind;

    return point;
}
void add_packet_to_cloud(ns scan_start_ts, ns scan_duration,
                         const PacketMsg& pm, CloudOS1& cloud) {
    const uint8_t* buf = pm.buf.data();
    for (int icol = 0; icol < columns_per_buffer; icol++) {
        const uint8_t* col_buf = nth_col(icol, buf);
        int32_t ts = col_timestamp(col_buf) - scan_start_ts.count();

        for (int ipx = 0; ipx < pixels_per_column; ipx++) {
            auto p = nth_point(ipx, col_buf);
            p.time_offset_us = ts/1000;
            cloud.push_back(p);
        }
    }
}

void spin(const client& cli,
          const std::function<void(const PacketMsg& pm)>& lidar_handler,
          const std::function<void(const PacketMsg& pm)>& imu_handler) {
    PacketMsg lidar_packet, imu_packet;
    lidar_packet.buf.resize(lidar_packet_bytes + 1);
    imu_packet.buf.resize(imu_packet_bytes + 1);

    while (ros::ok()) {
        auto state = poll_client(cli);
        if (state & ERROR) {
            ROS_ERROR("spin: poll_client returned error");
            return;
        }
        if (state & LIDAR_DATA) {
            if (read_lidar_packet(cli, lidar_packet.buf.data()))
                lidar_handler(lidar_packet);
        }
        if (state & IMU_DATA) {
            if (read_imu_packet(cli, imu_packet.buf.data()))
                imu_handler(imu_packet);
        }
        ros::spinOnce();
    }
}

static ns nearest_scan_dur(ns scan_dur, ns ts) {
    return ns((ts.count() / scan_dur.count()) * scan_dur.count());
};

std::function<void(const PacketMsg&)> batch_packets(
    ns scan_dur, const std::function<void(ns, const CloudOS1&)>& f) {
    auto cloud = std::make_shared<OS1::CloudOS1>();
    auto scan_ts = ns(-1L);

    return [=](const PacketMsg& pm) mutable {
        ns packet_ts = OS1::timestamp_of_lidar_packet(pm);
        if (scan_ts.count() == -1L)
            scan_ts = nearest_scan_dur(scan_dur, packet_ts);

        OS1::add_packet_to_cloud(scan_ts, scan_dur, pm, *cloud);

        auto batch_dur = packet_ts - scan_ts;
        if (batch_dur >= scan_dur || batch_dur < ns(0)) {
            f(scan_ts, *cloud);

            cloud->clear();
            scan_ts = ns(-1L);
        }
    };
}
}
}
