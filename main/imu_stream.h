#ifndef IMU_STREAM_H_
#define IMU_STREAM_H_

/**
 * @brief  Initialise internal state (call once before imu_stream_start).
 */
void imu_stream_init(void);

/**
 * @brief  Spawn the 100 Hz sampling task and the TCP server task.
 *         TCP server listens on port 3334.
 *         Call after lsm9ds1_init() and wifi_manager_wait_connected().
 */
void imu_stream_start(void);

#endif /* IMU_STREAM_H_ */
