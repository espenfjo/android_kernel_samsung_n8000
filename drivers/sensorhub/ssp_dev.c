/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include "ssp.h"

/* ssp mcu device ID */
#define DEVICE_ID			0x55
#define SSP_DEBUG_TIMER_SEC		(10 * HZ)

static void ssp_early_suspend(struct early_suspend *handler);
static void ssp_late_resume(struct early_suspend *handler);

/*************************************************************************/
/* SSP Debug timer function                                              */
/*************************************************************************/

static void print_sensordata(struct ssp_data *data, unsigned int uSensor)
{
	switch (uSensor) {
	case ACCELEROMETER_SENSOR:
	case GYROSCOPE_SENSOR:
	case GEOMAGNETIC_SENSOR:
	case LIGHT_SENSOR:
		ssp_dbg(" %u : %d, %d, %d (%ums)\n", uSensor,
			data->buf[uSensor].x, data->buf[uSensor].y,
			data->buf[uSensor].z,
			get_msdelay(data->adDelayBuf[uSensor]));
		break;
	case PRESSURE_SENSOR:
		ssp_dbg(" %u : %u, %u (%ums)\n", uSensor,
			data->buf[uSensor].pressure[0],
			data->buf[uSensor].pressure[1],
			get_msdelay(data->adDelayBuf[uSensor]));
		break;
	case GESTURE_SENSOR:
		ssp_dbg(" %u : %d %d %d %d (%ums)\n", uSensor,
			data->buf[uSensor].data[0], data->buf[uSensor].data[1],
			data->buf[uSensor].data[2], data->buf[uSensor].data[3],
			get_msdelay(data->adDelayBuf[uSensor]));
		break;
	}
}

static void debug_work_func(struct work_struct *work)
{
	unsigned int uSensorCnt;
	struct ssp_data *data = container_of(work, struct ssp_data, work_debug);

	ssp_dbg("[SSP]: %s - Sensor state : 0x%x\n", __func__,
		data->uAliveSensorDebug);

	for (uSensorCnt = 0; uSensorCnt < (SENSOR_MAX - 1); uSensorCnt++)
		if (atomic_read(&data->aSensorEnable) & (1 << uSensorCnt))
			print_sensordata(data, uSensorCnt);
}

static void debug_timer_func(unsigned long ptr)
{
	struct ssp_data *data = (struct ssp_data *)ptr;

	queue_work(data->debug_wq, &data->work_debug);
	mod_timer(&data->debug_timer,
		round_jiffies_up(jiffies + SSP_DEBUG_TIMER_SEC));
}

void enable_debug_timer(struct ssp_data *data)
{
	mod_timer(&data->debug_timer,
		round_jiffies_up(jiffies + SSP_DEBUG_TIMER_SEC));
}

void disable_debug_timer(struct ssp_data *data)
{
	del_timer_sync(&data->debug_timer);
	cancel_work_sync(&data->work_debug);
}

/************************************************************************/
/* interrupt happened due to transition/change of SSP MCU		*/
/************************************************************************/

static irqreturn_t sensordata_irq_thread_fn(int iIrq, void *dev_id)
{
	struct ssp_data *data = dev_id;

	data_dbg("%s\n", __func__);
	select_irq_msg(data);

	return IRQ_HANDLED;
}

/*************************************************************************/
/* initialize sensor hub						 */
/*************************************************************************/

static void initialize_variable(struct ssp_data *data)
{
	int iSensorIndex;

	for (iSensorIndex = 0; iSensorIndex < SENSOR_MAX; iSensorIndex++) {
		data->adDelayBuf[iSensorIndex] = DEFUALT_POLLING_DELAY;
		data->aiCheckStatus[iSensorIndex] = INITIALIZATION_STATE;
	}

	/* AKM Daemon Library */
	data->aiCheckStatus[GEOMAGNETIC_SENSOR] = NO_SENSOR_STATE;
	data->aiCheckStatus[ORIENTATION_SENSOR] = NO_SENSOR_STATE;

	atomic_set(&data->aSensorEnable, 0);
	data->iLibraryLength = 0;
	data->uAliveSensorDebug = 0;
	data->uFactorydataReady = 0;
	data->uFactoryProxAvg[0] = 0;

	data->bCheckSuspend = false;
	data->bDebugEnabled = false;
	data->bBarcodeEnabled = false;
	data->bProximityRawEnabled = false;
	data->bMcuIRQTestSuccessed = false;

	data->accelcal.x = 0;
	data->accelcal.y = 0;
	data->accelcal.z = 0;

	data->gyrocal.x = 0;
	data->gyrocal.y = 0;
	data->gyrocal.z = 0;

	data->iPressureCal = 0;
	data->uGyroDps = (unsigned int)GYROSCOPE_DPS500;
	initialize_function_pointer(data);
}

static int initialize_irq(struct ssp_data *data)
{
	int iRet;
	int iIrq;

	iRet = gpio_request(data->client->irq, "mpu_ap_int1");
	if (iRet < 0) {
		pr_err("%s: [SSP] gpio %d request failed (%d)\n",
		       __func__, data->client->irq, iRet);
		return iRet;
	}

	iRet = gpio_direction_input(data->client->irq);
	if (iRet < 0) {
		pr_err("%s: [SSP] failed to set gpio %d as input (%d)\n",
		       __func__, data->client->irq, iRet);
		goto err_irq_direction_input;
	}

	iIrq = gpio_to_irq(data->client->irq);

	pr_info("[SSP] requesting IRQ %d\n", iIrq);
	iRet = request_threaded_irq(iIrq, NULL, sensordata_irq_thread_fn,
				    IRQF_TRIGGER_RISING, "SSP_int", data);
	if (iRet < 0) {
		pr_err("%s: [SSP] request_irq(%d) failed for gpio %d (%d)\n",
		       __func__, iIrq, iIrq, iRet);
		goto err_request_irq;
	}

	/* start with interrupts disabled */
	data->iIrq = iIrq;
	disable_irq(data->iIrq);
	return true;

err_request_irq:
err_irq_direction_input:
	gpio_free(data->client->irq);
	return iRet;
}

static const struct file_operations akmd_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.unlocked_ioctl = akmd_ioctl,
};

static int ssp_probe(struct i2c_client *client,
	const struct i2c_device_id *devid)
{
	int iRet = 0;
	struct ssp_data *data;
	struct ssp_platform_data *pdata = client->dev.platform_data;

	if (pdata == NULL) {
		pr_err("%s: [SSP] platform_data is null..\n", __func__);
		iRet = -ENOMEM;
		goto exit;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		pr_err("%s: [SSP] failed to allocate memory for module data\n",
			__func__);
		iRet = -ENOMEM;
		goto exit;
	}

	data->client = client;
	i2c_set_clientdata(client, data);

	data->wakeup_mcu = pdata->wakeup_mcu;
	data->check_mcu_ready = pdata->check_mcu_ready;
	data->set_mcu_reset = pdata->set_mcu_reset;
	data->check_ap_rev = pdata->check_ap_rev;

	if ((data->wakeup_mcu == NULL)
		|| (data->check_mcu_ready == NULL)
		|| (data->set_mcu_reset == NULL)
		|| (data->check_ap_rev == NULL)) {
		pr_err("%s: [SSP] function callback is null\n", __func__);
		iRet = -EIO;
		goto err_reset_null;
	}

	pr_info("\n#####################################################\n");

	/* check boot loader binary */
	check_fwbl(data);

	/* read chip id */
	iRet = i2c_smbus_read_byte_data(client, MSG2SSP_AP_WHOAMI);
	pr_info("[SSP] MPU device ID = %d, reading ID = %d\n", DEVICE_ID, iRet);
	if (iRet != DEVICE_ID) {
		if (iRet < 0)
			pr_err("%s: [SSP] i2c for reading chip id failed\n",
			       __func__);
		else {
			pr_err("%s: [SSP] Device identification failed\n",
			       __func__);
			iRet = -ENODEV;
		}
		goto err_read_reg;
	}
	set_sensor_position(data);
	set_proximity_threshold(data);

	wake_lock_init(&data->ssp_wake_lock,
		WAKE_LOCK_SUSPEND, "ssp_wake_lock");

	iRet = initialize_input_dev(data);
	if (iRet < 0) {
		pr_err("%s: [SSP] could not create input device\n", __func__);
		goto err_input_register_device;
	}

	data->akmd_device.minor = MISC_DYNAMIC_MINOR;
	data->akmd_device.name = "akm8963";
	data->akmd_device.fops = &akmd_fops;

	iRet = misc_register(&data->akmd_device);
	if (iRet)
		goto err_akmd_device_register;

	setup_timer(&data->debug_timer, debug_timer_func, (unsigned long)data);

	data->debug_wq = create_singlethread_workqueue("ssp_debug_wq");
	if (!data->debug_wq) {
		iRet = -ENOMEM;
		pr_err("%s: [SSP] could not create workqueue\n", __func__);
		goto err_create_workqueue;
	}
	INIT_WORK(&data->work_debug, debug_work_func);

	initialize_variable(data);

	iRet = initialize_irq(data);
	if (iRet < 0) {
		pr_err("%s: [SSP] could not create sysfs\n", __func__);
		goto err_setup_irq;
	}

	iRet = initialize_sysfs(data);
	if (iRet < 0) {
		pr_err("%s: [SSP] could not create sysfs\n", __func__);
		goto err_sysfs_create;
	}

	data->uAliveSensorDebug = get_sensor_scanning_info(data);
	get_fuserom_data(data);

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.suspend = ssp_early_suspend;
	data->early_suspend.resume = ssp_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* init sensorhub device */
	iRet = ssp_initialize_sensorhub(data);
	if (iRet < 0) {
		pr_err("%s: ssp_initialize_sensorhub err(%d)", __func__, iRet);
		ssp_remove_sensorhub(data);
	}
#endif

	enable_irq(data->iIrq);
	pr_info("[SSP] probe success!\n");

	iRet = 0;
	goto exit;

 err_sysfs_create:
	free_irq(data->iIrq, data);
	gpio_free(data->client->irq);
 err_setup_irq:
	destroy_workqueue(data->debug_wq);
 err_create_workqueue:
	misc_deregister(&data->akmd_device);
 err_akmd_device_register:
	remove_input_dev(data);
 err_input_register_device:
	wake_lock_destroy(&data->ssp_wake_lock);
 err_read_reg:
 err_reset_null:
	kfree(data);
	pr_err("%s: [SSP] probe failed!\n", __func__);
 exit:
	pr_info("#####################################################\n\n");
	return iRet;
}

static int ssp_remove(struct i2c_client *client)
{
	struct ssp_data *data = i2c_get_clientdata(client);

	disable_irq_wake(data->iIrq);
	disable_irq(data->iIrq);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	ssp_remove_sensorhub(data);
#endif

	remove_sysfs(data);
	remove_input_dev(data);

	misc_deregister(&data->akmd_device);

	del_timer_sync(&data->debug_timer);
	cancel_work_sync(&data->work_debug);
	destroy_workqueue(data->debug_wq);

	free_irq(data->iIrq, data);
	gpio_free(data->client->irq);

	wake_lock_destroy(&data->ssp_wake_lock);
	kfree(data);
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ssp_early_suspend(struct early_suspend *handler)
{
	struct ssp_data *data;
	data = container_of(handler, struct ssp_data, early_suspend);

	func_dbg();
	if (data->bDebugEnabled)
		disable_debug_timer(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* give notice to user that AP goes to sleep */
	ssp_report_context_noti(data, MSG2SSP_AP_STATUS_SLEEP);
	ssp_sleep_mode(data);
#else
	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_sleep_mode(data);
#endif

	data->bCheckSuspend = true;
}

static void ssp_late_resume(struct early_suspend *handler)
{
	struct ssp_data *data;
	data = container_of(handler, struct ssp_data, early_suspend);

	func_dbg();
	if (data->bDebugEnabled)
		enable_debug_timer(data);

	data->bCheckSuspend = false;

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* give notice to user that AP goes to sleep */
	ssp_report_context_noti(data, MSG2SSP_AP_STATUS_WAKEUP);
	ssp_resume_mode(data);
#else
	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_resume_mode(data);
#endif
}
#else

static int ssp_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ssp_data *data = i2c_get_clientdata(client);

	func_dbg();
	if (data->bDebugEnabled)
		disable_debug_timer(data);

	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_sleep_mode(data);

	data->bCheckSuspend = true;
	return 0;
}

static int ssp_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ssp_data *data = i2c_get_clientdata(client);

	func_dbg();
	if (data->bDebugEnabled)
		enable_debug_timer(data);

	data->bCheckSuspend = false;

	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_resume_mode(data);

	return 0;
}

static const struct dev_pm_ops ssp_pm_ops = {
	.suspend = ssp_suspend,
	.resume = ssp_resume
};
#endif

static const struct i2c_device_id ssp_id[] = {
	{"ssp", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ssp_id);

static struct i2c_driver ssp_driver = {
	.probe = ssp_probe,
	.remove = __devexit_p(ssp_remove),
	.id_table = ssp_id,
	.driver = {
#ifndef CONFIG_HAS_EARLYSUSPEND
		   .pm = &ssp_pm_ops,
#endif
		   .owner = THIS_MODULE,
		   .name = "ssp"
		},
};

static int __init ssp_init(void)
{
	return i2c_add_driver(&ssp_driver);
}

static void __exit ssp_exit(void)
{
	i2c_del_driver(&ssp_driver);
}

module_init(ssp_init);
module_exit(ssp_exit);

MODULE_DESCRIPTION("ssp driver");
MODULE_AUTHOR("Kyusung Kim <gs0816.kim@samsung.com>");
MODULE_LICENSE("GPL");
