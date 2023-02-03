# Extrinsic Manual Calibration

## 1. Capture rosbag

Capture your rosbag including all the sensor topics in a static environment.

The duration is arbitrary, but about some minutes is recommended.

<details><summary>ROSBAG Example</summary>
<p>

```sh

Files:             lidar_cam_calib_2_0.db3
Bag size:          1.1 GiB
Storage id:        sqlite3
Duration:          61.957s
Start:             Feb  3 2023 14:48:37.813 (1675424917.813)
End:               Feb  3 2023 14:49:39.771 (1675424979.771)
Messages:          13571
Topic information: Topic: /tf_static | Type: tf2_msgs/msg/TFMessage | Count: 2 | Serialization Format: cdr
                   Topic: /tf | Type: tf2_msgs/msg/TFMessage | Count: 9861 | Serialization Format: cdr
                   Topic: /sensing/lidar/front/velodyne_packets | Type: velodyne_msgs/msg/VelodyneScan | Count: 612 | Serialization Format: cdr
                   Topic: /sensing/lidar/right/velodyne_packets | Type: velodyne_msgs/msg/VelodyneScan | Count: 620 | Serialization Format: cdr
                   Topic: /sensing/lidar/left/velodyne_packets | Type: velodyne_msgs/msg/VelodyneScan | Count: 620 | Serialization Format: cdr
                   Topic: /lucid_vision/camera_2/image | Type: sensor_msgs/msg/Image | Count: 928 | Serialization Format: cdr
                   Topic: /lucid_vision/camera_2/camera_info | Type: sensor_msgs/msg/CameraInfo | Count: 928 | Serialization Format: cdr
```

</p>
</details>

## 2. Launch Calibration Tools

Launch extrinsic manual calibration with the following command (on terminal 1).

```sh
ros2 launch extrinsic_calibration_manager calibration.launch.xml \
  mode:=manual sensor_model:=<sensor_model> vehicle_model:=<vehicle_model>
```

For example,

```sh
ros2 launch extrinsic_calibration_manager calibration.launch.xml \
  mode:=manual sensor_model:=isuzu_sensor vehicle_model:=isuzu
```

Play your rosbag (on terminal 2). Example for ISUZU bag:

```sh
ros2 bag play lidar_cam_calib_2/ --clock -l -r 0.2 --remap /tf:=/null/tf /tf_static:=/null/tf_static \
 /lucid_vision/camera_2/camera_info:=/sensing/camera/camera2/camera_info \ 
/lucid_vision/camera_2/image:=/sensing/camera/camera2/image_raw
```

Now you are ready to start.

## 3. Manual Calibration Process

In rqt_reconfigure, the basic usage is as follows.

1. Press `Refresh` button then press `Expand All` button.

   ![rqt_reconfigure](images/rqt_reconfigure.png)

2. Write the target frame name in `Filter` area and select `tunable_static_tf_broadcaster_node`.
3. Adjust the `tf_*` parameter manually.
4. Press the `Close` button.
5. Repeat steps 2 ~ 5 until all targets have been adjusted.

   ![rqt_reconfigure_tf](images/rqt_reconfigure_tf.png)

6. When you finish adjusting parameters, dump the results to yaml files with the following command (on terminal 3).

   ```sh
   ros2 topic pub /done std_msgs/Bool "data: true"
   ```

7. Check the output file in `$HOME/*.yaml`.

   The following sections describe how to adjust the external parameters of each sensor.

### Vehicle-to-LiDAR

1. Place two poles on the extension of the vehicle.
2. Modify the frames of the child of `base_link` in rqt_reconfigure so that the pointcloud center of the pole is placed on the extension of the `base_link`.

### Vehicle-to-IMU/GNSS

1. Enter designed values (e.g. based on CAD data) or manually measured values in rqt_reconfigure.

### LiDAR-to-LiDAR

1. Set the topic name of the target pointcloud in rviz.

   ![rviz_display_lidar](images/rviz_display_lidar.png)

2. Manually adjust parameters in rqt_reconfigure using the rviz visualization as a reference.

   ![rviz_lidar](images/rviz_lidar.png)

### LiDAR-to-Camera

1. Set the topic name of the target image in rviz.

   ![rviz_display_camera](images/rviz_display_lidar.png)

2. Specify `camera_name` and launch `camera_republisher` to visualize camera image.

   ```sh
   ros2 launch extrinsic_calibration_manager camera_republisher.launch.xml camera_name:=<camera_name>
   ```

   For example,

   ```sh
   ros2 launch extrinsic_calibration_manager camera_republisher.launch.xml camera_name:=camera0
   ```

   For handling the raw image topic (e.g. traffic light recognition camera), use `mode:=raw` option additionally.

   ```sh
   ros2 launch extrinsic_calibration_manager camera_republisher.launch.xml camera_name:=traffic_light mode:=raw
   ```

3. Manually adjust parameters in rqt_reconfigure using the rviz visualization as a reference.

   ![rviz_camera](images/rviz_camera.png)

   NOTE: After the rosbag loops, press the Reset button in rviz once.
