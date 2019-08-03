#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

#define VENDOR_ID_NINTENDO			0x057e
#define DEVICE_ID_NINTENDO_JOYCON_L	0x2006
#define DEVICE_ID_NINTENDO_JOYCON_R	0x2007
#define DEVICE_ID_NINTENDO_PROCON	0x2009

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dan: https://github.com/dan611");
MODULE_DESCRIPTION("Driver for Nintendo Switch Pro Controller");

#define PROCON_REPORT_SEND_USB		0x80
#define PROCON_REPORT_REPLY_USB		0x81
#define PROCON_REPORT_REPLY			0x21
#define PROCON_REPORT_TYPE			0x00
#define PROCON_REPORT_CMD_ACK		0x0E
#define PROCON_REPORT_INPUT_FULL	0x30
#define PROCON_REPORT_INPUT_SIMPLE	0x3F

#define PROCON_USB_HANDSHAKE		0x02
#define PROCON_USB_BAUD				0x03
#define PROCON_USB_ENABLE			0x04
#define PROCON_USB_DISABLE			0x05
#define PROCON_USB_DO_CMD			0x92

#define PROCON_CMD_AND_RUMBLE		0x01
#define PROCON_CMD_RUMBLE_ONLY		0x10

#define PROCON_CMD_INFO				0x02
#define PROCON_CMD_MODE				0x03
#define PROCON_CMD_BTNTIME			0x04
#define PROCON_CMD_LED				0x30
#define PROCON_CMD_LED_HOME			0x38
#define PROCON_CMD_GYRO				0x40
#define PROCON_CMD_BATTERY			0x50

#define PROCON_ARG_INPUT_FULL		0x30
#define PROCON_ARG_INPUT_SIMPLE		0x3F

#define PROCON_EVENT_TOGGLE_GYRO	0xFF

static const struct hid_device_id procon_table [] =
{
	{ HID_USB_DEVICE(VENDOR_ID_NINTENDO, DEVICE_ID_NINTENDO_PROCON) },
	{ HID_BLUETOOTH_DEVICE(VENDOR_ID_NINTENDO, DEVICE_ID_NINTENDO_PROCON) },
	{ HID_BLUETOOTH_DEVICE(VENDOR_ID_NINTENDO, DEVICE_ID_NINTENDO_JOYCON_L) },
	{ HID_BLUETOOTH_DEVICE(VENDOR_ID_NINTENDO, DEVICE_ID_NINTENDO_JOYCON_R) },
	{ },
};
MODULE_DEVICE_TABLE(hid, procon_table);

struct procon_data
{
	struct list_head list;

	struct hid_device *hdev;
	struct input_dev *input;
	struct work_struct worker_connect;
	struct work_struct worker_event;
	struct work_struct worker_rumble;

	enum modes { PROCON_MODE_SIMPLE, PROCON_MODE_FULL, PROCON_MODE_GYRO } mode, mode_new;
	int analog_dpad,
		gyro_trigger;
	bool connected;
	int order;
	u16 rumble_strong;
	u16 rumble_weak;
	
	struct power_supply *battery;
	struct power_supply_desc battery_desc;

	u8 event_cmd;
	u64 time;

	spinlock_t		lock;
	struct mutex	mutex;
} *connections[8];

static DEFINE_MUTEX(connections_lock);

static const int ledmap[] =
{
	0b0001,
	0b0011,
	0b0111,
	0b1111,
	0b1001,
	0b0101,
	0b1101,
	0b0110,
};

static const struct{bool up; bool right; bool down; bool left;} hatmap[] =
{
	{true,	false,	false,	false},
	{true,	true,	false,	false},
	{false,	true,	false,	false},
	{false,	true,	true,	false},
	{false,	false,	true,	false},
	{false,	false,	true,	true},
	{false,	false,	false,	true},
	{true,	false,	false,	true},
	{false,	false,	false,	false}
};

static int procon_send_report(struct hid_device *hdev, u8 *data, int size)
{
	struct hid_report *rep;
	unsigned id = data[0];
	u8 *buf;
	int retval;

	if(hdev->bus == BUS_USB)
	{
		rep = hdev->report_enum[HID_OUTPUT_REPORT].report_id_hash[id]; 
		if(!rep || hid_report_len(rep) != 0x40)
			return -EINVAL;
			
		buf = hid_alloc_report_buf(rep, GFP_KERNEL);
		if(!buf)
			return -ENOMEM;

		memcpy(buf, data, size);

		retval = hid_hw_raw_request(hdev, id, buf, size, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
		
		kfree(buf);
	}
	else
		retval = hid_hw_output_report(hdev, data, size);

	//~ if(retval > -1)
		//~ hid_info(hdev,"Sent %d bytes (%*ph)\n", retval, size, data);
	//~ else
		//~ hid_err(hdev,"Could not send %d bytes (%*ph) (error %d)\n", size, size, data, retval);

	return retval;	
}

static int procon_send_cmd_usb(struct hid_device *hdev, int cmd)
{
	u8 data[2] = {PROCON_REPORT_SEND_USB, cmd};
	return procon_send_report(hdev, data, 2);
}

static int procon_send_data(struct hid_device *hdev, u8 *data, int size)
{
	int retval;
	u8 buf[64] = {PROCON_REPORT_SEND_USB, PROCON_USB_DO_CMD, 0x00, 0x31, 0x00, 0x00, 0x00, 0x00};
	memcpy(buf + 8, data, size);

	if(hdev->bus == BUS_USB)
		retval = procon_send_report(hdev, buf, 64);
	else
		retval = procon_send_report(hdev, buf + 8, 49);
	return retval;
}

static int procon_send_cmd(struct hid_device *hdev, u8 cmd, u8 arg)
{
	u8 data[12] =
	{
		PROCON_CMD_AND_RUMBLE,
		0x00,
		0x00,
		0x10,
		0x40,
		0x40,
		0x00,
		0x10,
		0x40,
		0x40,
		cmd,
		arg
	};
	return procon_send_data(hdev, data, 12);
}

static void procon_work_connect(struct work_struct *work)
{
	struct procon_data *drvdata = container_of(work, struct procon_data, worker_connect);
	struct hid_device *hdev = drvdata->hdev;
	unsigned long flags;
	enum modes mode;

	//~ hid_info(hdev, "procon_work_connect\n");

	if(hdev->bus == BUS_USB)
	{
		procon_send_cmd_usb(hdev, PROCON_USB_ENABLE);
		procon_send_cmd_usb(hdev, PROCON_USB_HANDSHAKE);

		procon_send_cmd(hdev, PROCON_CMD_MODE, PROCON_ARG_INPUT_FULL);
		mode = PROCON_MODE_FULL;
	}
	else
	{
		procon_send_cmd(hdev, PROCON_CMD_MODE, PROCON_ARG_INPUT_SIMPLE);
		mode = PROCON_MODE_SIMPLE;
	}

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->mode = mode;
	drvdata->mode_new = mode;
	spin_unlock_irqrestore(&drvdata->lock, flags);
}

static void procon_work_event(struct work_struct *work)
{
	struct procon_data *drvdata = container_of(work, struct procon_data, worker_event);
	struct hid_device *hdev = drvdata->hdev;
	unsigned long flags;
	u8 mode;
	u8 mode_new;
	u8 order;
	u8 event;
	u8 data_homelight[34] =
	{
		PROCON_CMD_AND_RUMBLE,
		0x00,
		0x00,
		0x90,
		0x20,
		0x64,
		0x00,
		0x90,
		0x20,
		0x64,
		PROCON_CMD_LED_HOME,
		0x0F,
		0xF1,
		0x20,
	};
	
	//~ hid_info(hdev, "procon_work_event %d\n", event);

	spin_lock_irqsave(&drvdata->lock, flags);
	order = drvdata->order;
	event = drvdata->event_cmd;
	mode = drvdata->mode;
	mode_new = drvdata->mode_new;
	spin_unlock_irqrestore(&drvdata->lock, flags);


	switch(event)
	{
	case PROCON_CMD_MODE:
		// input mode set, ready to set the connection LED
		mutex_lock(&connections_lock);
		if(!drvdata->connected)
		{
			drvdata->connected = true;
			//~ drvdata->analog_dpad = 0;
			//~ drvdata->gyro_trigger = 0;
			for(order = 0;order < 8;order++)
				if(connections[order] == NULL)
				{
					drvdata->order = order;
					connections[order] = drvdata;
					break;
				}
			if(order == 9)
			{
				order--;
				drvdata->order = order;
			}
			hid_info(hdev, hdev->bus == BUS_USB? "Pro Controller (Wired) #%d connected\n"  : "Pro Controller (Wireless) #%d connected\n", order + 1);
			procon_send_cmd(hdev, PROCON_CMD_LED, ledmap[order]);
		}
		mutex_unlock(&connections_lock);
		
		// wireless has switched to full mode, enable gyro
		if(mode == PROCON_MODE_SIMPLE && mode_new == PROCON_MODE_GYRO)
		{
			procon_send_cmd(hdev, PROCON_CMD_GYRO, true);
			mode_new = PROCON_MODE_FULL;
		}
		else if(mode == PROCON_MODE_GYRO && mode_new == PROCON_MODE_SIMPLE)
		{
			data_homelight[12] = 0x21;
			procon_send_data(hdev, data_homelight, 34);
		}

		spin_lock_irqsave(&drvdata->lock, flags);
		drvdata->mode = mode_new;
		spin_unlock_irqrestore(&drvdata->lock, flags);
		
		break;

	case PROCON_CMD_GYRO:
		if(mode_new == PROCON_MODE_GYRO || mode_new == PROCON_MODE_FULL)
		{
			spin_lock_irqsave(&drvdata->lock, flags);
			drvdata->mode = drvdata->mode_new;
			spin_unlock_irqrestore(&drvdata->lock, flags);

			data_homelight[12] = mode_new == PROCON_MODE_GYRO? 0x20 : 0x21;
			procon_send_data(hdev, data_homelight, 34);
		}
		else
			procon_send_cmd(hdev, PROCON_CMD_MODE, PROCON_ARG_INPUT_SIMPLE);

		if(mode_new == PROCON_MODE_GYRO)
			hid_info(hdev,  "Pro Controller #%d gyroscope enabled\n", order + 1);
		else if(mode_new != PROCON_MODE_GYRO)
			hid_info(hdev,  "Pro Controller #%d gyroscope disbled\n", order + 1);
		
		break;

	case PROCON_EVENT_TOGGLE_GYRO:
		mode_new = PROCON_MODE_GYRO;
		
		// wireless must switch to full mode first to enable gyro
		if(mode == PROCON_MODE_SIMPLE)
			procon_send_cmd(hdev, PROCON_CMD_MODE, PROCON_ARG_INPUT_FULL);
		else if(mode == PROCON_MODE_FULL)
			procon_send_cmd(hdev, PROCON_CMD_GYRO, true);
		else
		{
			procon_send_cmd(hdev, PROCON_CMD_GYRO, false);
			mode_new = hdev->bus == BUS_USB? PROCON_MODE_FULL : PROCON_MODE_SIMPLE;
		}

		spin_lock_irqsave(&drvdata->lock, flags);
		drvdata->mode_new = mode_new;
		spin_unlock_irqrestore(&drvdata->lock, flags);	
		break;
		
	case PROCON_CMD_LED:
		// controller may have been unplugged and reconnected, update the home light accordingly
		if(mode != PROCON_MODE_GYRO)
		{
			data_homelight[12] = 0x21;
			procon_send_data(hdev, data_homelight, 34);
		}
		break;
		
	case PROCON_CMD_LED_HOME:
		msleep(50);
		procon_send_cmd(hdev, 0x00, 0x00);
		break;
	}
}

static void procon_work_rumble(struct work_struct *work)
{
	struct procon_data *drvdata = container_of(work, struct procon_data, worker_rumble);
	struct hid_device *hdev = drvdata->hdev;
	unsigned long flags;

	u8 data[12] = {PROCON_CMD_RUMBLE_ONLY, 0x00};

	u8 h_freq = 0x20;
	u8 l_freq = 0x28;
	u8 h_amp;
	u8 l_amp1;
	u8 l_amp2;

	spin_lock_irqsave(&drvdata->lock, flags);
	h_amp = (drvdata->rumble_weak / 649) * 2;
	l_amp1 = (drvdata->rumble_strong / 649);
	spin_unlock_irqrestore(&drvdata->lock, flags);
	l_amp2 = ((l_amp1 % 2) * 128);
	l_amp1 = (l_amp1 / 2) + 64;

	data[2] = data[6] = h_freq;
	data[4] = data[9] = l_freq;
	
	data[3] = data[7] = h_amp;
	data[4] = data[8] += l_amp2;
	data[5] = data[9] = l_amp1;

	//~ hid_info(hdev,"RUMBLE (%04hX %04hX) -> (%*ph)\n", drvdata->rumble_weak, drvdata->rumble_strong, 8, data);
	procon_send_data(hdev, data, 12);
}

static int procon_play(struct input_dev *input, void *data, struct ff_effect *effect)
{
	struct procon_data *drvdata = input_get_drvdata(input);

	if(effect->type == FF_RUMBLE)
	{
		drvdata->rumble_weak = effect->u.rumble.weak_magnitude;
		drvdata->rumble_strong = effect->u.rumble.strong_magnitude;
		schedule_work(&drvdata->worker_rumble);
	}
	return 0;
}

static int procon_input_register(struct procon_data *drvdata)
{
	struct hid_device *hdev = drvdata->hdev;
	struct input_dev *input = input_allocate_device();
	int retval;

	//~ hid_info(hdev, "procon_input_register");

	if(!input)
		return -ENOMEM;
	
	input_set_drvdata(input, drvdata);
	input->name = hdev->bus == BUS_USB? "Pro Controller (Wired)"  : "Pro Controller (Wireless)";
	input->id.bustype = hdev->bus;
	input->id.vendor = hdev->vendor;
	input->id.product = hdev->product;
	input->id.version = hdev->version;
	input->dev.parent = &hdev->dev;

	input_set_capability(input, EV_KEY, BTN_A);
	input_set_capability(input, EV_KEY, BTN_B);
	input_set_capability(input, EV_KEY, BTN_X);
	input_set_capability(input, EV_KEY, BTN_Y);
	input_set_capability(input, EV_KEY, BTN_TL);
	input_set_capability(input, EV_KEY, BTN_TR);
	input_set_capability(input, EV_KEY, BTN_TL2);
	input_set_capability(input, EV_KEY, BTN_TR2);
	input_set_capability(input, EV_KEY, BTN_SELECT);
	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_MODE);
	input_set_capability(input, EV_KEY, BTN_EXTRA);
	input_set_capability(input, EV_KEY, BTN_THUMBL);
	input_set_capability(input, EV_KEY, BTN_THUMBR);
	input_set_capability(input, EV_KEY, BTN_DPAD_UP);
	input_set_capability(input, EV_KEY, BTN_DPAD_DOWN);
	input_set_capability(input, EV_KEY, BTN_DPAD_LEFT);
	input_set_capability(input, EV_KEY, BTN_DPAD_RIGHT);
	input_set_capability(input, EV_FF, FF_RUMBLE);
	input_set_abs_params(input, ABS_X, -0x7FFF, 0x7FFF, 0, 0x7FF);
	input_set_abs_params(input, ABS_Y, -0x7FFF, 0x7FFF, 0, 0x7FF);
	input_set_abs_params(input, ABS_RX, -0x7FFF, 0x7FFF, 0, 0x7FF);
	input_set_abs_params(input, ABS_RY, -0x7FFF, 0x7FFF, 0, 0x7FF);
	input_set_abs_params(input, ABS_TILT_X, -0x7FFF, 0x7FFF, 0x0F, 0);
	input_set_abs_params(input, ABS_TILT_Y, -0x7FFF, 0x7FFF, 0x0F, 0);
	
	retval = input_register_device(input);
	if(retval)
		goto error;
	else
		drvdata->input = input;

	retval = input_ff_create_memless(input, NULL, procon_play);
	if(retval)
	{
		hid_err(hdev, "Could not enable force feedback (error %d)\n", retval);
		goto error;
	}
	
	return 0;

error:
	input_free_device(input);
	return retval;
}

static int procon_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct procon_data *drvdata;
	int retval;

	//~ hid_info(hdev, "procon_probe");

	retval = hid_parse(hdev);
	if(retval)
	{
		hid_err(hdev, "Could not parse device\n");
		return retval;
	}

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if(!drvdata)
	{
		hid_err(hdev, "Could not allocate procon_data\n");
		return -ENOMEM;
	}

	drvdata->hdev = hdev;
	hid_set_drvdata(hdev, drvdata);
	spin_lock_init(&drvdata->lock);
	mutex_init(&drvdata->mutex);
	INIT_WORK(&drvdata->worker_connect, procon_work_connect);
	INIT_WORK(&drvdata->worker_event, procon_work_event);
	INIT_WORK(&drvdata->worker_rumble, procon_work_rumble);
	retval = hid_hw_start(hdev, HID_CONNECT_HIDRAW | HID_CONNECT_HIDDEV_FORCE);
	if(retval)
	{
		hid_err(hdev, "Could not start device (error %d)\n", retval);
		goto error_start;
	}

	retval = hid_hw_open(hdev);
	if(retval)
	{
		hid_err(hdev, "Could not open device (error %d)\n", retval);
		goto error_open;
	}

	retval = procon_input_register(drvdata);
	if(retval)
	{
		hid_err(hdev, "Could not register device input (error %d)\n", retval);
		goto error_input;
	}

	schedule_work(&drvdata->worker_connect);

	return 0;

error_input:
	hid_hw_close(hdev);
error_open:
	hid_hw_stop(hdev);
error_start:
	return retval;
}

static void procon_remove(struct hid_device *hdev)
{
	struct procon_data *drvdata = hid_get_drvdata(hdev);
	u8 order;
	
	//~ hid_info(hdev, "procon_remove\n");
	
	cancel_work_sync(&drvdata->worker_connect);
	cancel_work_sync(&drvdata->worker_event);
	cancel_work_sync(&drvdata->worker_rumble);
	
	mutex_lock(&connections_lock);
	order = drvdata->order;
	connections[order] = NULL;
	mutex_unlock(&connections_lock);	
	
	hid_info(hdev, hdev->bus == BUS_USB? "Pro Controller (Wired) #%d disconnected\n"  : "Pro Controller (Wireless) #%d disconnected\n", order + 1);
	input_unregister_device(drvdata->input);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int procon_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	struct procon_data *drvdata = hid_get_drvdata(hdev);
	struct input_dev *input = drvdata->input;
	u64 drvtime;
	u64 time;
	unsigned long flags;
	
	bool home_button,
		 left_button,
		 right_button,
		 triggerl_button,
		 triggerr_button;
	int	analog_dpad,
		gyro_trigger;
	enum modes mode;

	spin_lock_irqsave(&drvdata->lock, flags);
	mode = drvdata->mode;
	analog_dpad = drvdata->analog_dpad;
	gyro_trigger = drvdata->gyro_trigger;
	drvtime = drvdata->time;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	// if bluetooth was enabled then the controller was plugged in,
	// gyroscope might still be on
	if(unlikely(data[PROCON_REPORT_TYPE] == PROCON_REPORT_INPUT_FULL && data[13] != 0x00 && mode == PROCON_MODE_FULL))
	{
		spin_lock_irqsave(&drvdata->lock, flags);
		drvdata->mode = PROCON_MODE_GYRO;
		spin_unlock_irqrestore(&drvdata->lock, flags);
		mode = PROCON_MODE_GYRO;
	}
	if(unlikely(!drvdata || !input || size < 1))
		return -EINVAL;
	
	if(data[PROCON_REPORT_TYPE] == PROCON_REPORT_REPLY_USB)
	{
		data += 10;
		size -= 10;
	}

	if(data[PROCON_REPORT_TYPE] == PROCON_REPORT_REPLY)
	{
		hid_info(hdev, "REPORT_CMD_ACK %04X", data[PROCON_REPORT_CMD_ACK]);
		//~ hid_info(hdev, "REPLY TO CMD %02hhX\n", data[PROCON_REPORT_CMD_ACK]);
		
		// after sending commands, the controller will return an acknowledgement
		// respond to each ack with the next command to set up the controller 
		if(data[PROCON_REPORT_CMD_ACK] == PROCON_CMD_MODE || 
		   data[PROCON_REPORT_CMD_ACK] == PROCON_CMD_GYRO ||
		   data[PROCON_REPORT_CMD_ACK] == PROCON_CMD_LED ||
		   data[PROCON_REPORT_CMD_ACK] == PROCON_CMD_LED_HOME)
		{
			spin_lock_irqsave(&drvdata->lock, flags);
			drvdata->event_cmd = data[PROCON_REPORT_CMD_ACK];
			spin_unlock_irqrestore(&drvdata->lock, flags);
			
			schedule_work(&drvdata->worker_event);
		}
	}

	if(data[PROCON_REPORT_TYPE] == PROCON_REPORT_INPUT_FULL || 
	   data[PROCON_REPORT_TYPE] == PROCON_REPORT_INPUT_SIMPLE)
	{
		if(mode != PROCON_MODE_SIMPLE)
		{
			// each axis is 12 bits in a 6 byte data chunk
			s16 x  = (*((u16 *) (data + 6)) << 4) & 0xFFF0;
			s16 y  =  *((u16 *) (data + 7))       & 0xFFF0;
			s16 rx = (*((u16 *) (data + 9)) << 4) & 0xFFF0;
			s16 ry =  *((u16 *) (data + 10))      & 0xFFF0;
			s16 gy  = (*((u16 *) (data + 13)) * 7);
			s16 gx  =   *((u16 *) (data + 15)) * 7;

			if(!analog_dpad)
			{
				x  -= 0x7FFF;
				y   = 0x7FFF - y;
				rx -= 0x7FFF;
				ry  = 0x7FFF - ry;
				
				if(gyro_trigger == 1 && !!(data[5] & 0x40))
				{
					if(x < 0 && gx < 0 && (s16) (x + gx) > 0)
						x = -0x7FFF;
					else if(x > 0 && gx > 0 && (s16) (x + gx) < 0)
						x = 0x7FFF;
					else
						x += gx;
					if(y < 0 && gy < 0 && (s16) (y + gy) > 0)
						y = -0x7FFF;
					else if(y > 0 && gy > 0 && (s16) (y + gy) < 0)
						y = 0x7FFF;
					else
						y += gy;
					gx = 0;
					gy = 0;
				}
				else if(gyro_trigger == 2 && !!(data[3] & 0x40))
				{
					if(rx < 0 && gx < 0 && (s16) (rx + gx) > 0)
						rx = -0x7FFF;
					else if(rx > 0 && gx > 0 && (s16) (rx + gx) < 0)
						rx = 0x7FFF;
					else
						rx += gx;
					if(ry < 0 && gy < 0 && (s16) (ry + gy) > 0)
						ry = -0x7FFF;
					else if(ry > 0 && gy > 0 && (s16) (ry + gy) < 0)
						ry = 0x7FFF;
					else
						ry += gy;
					gx = 0;
					gy = 0;
				}
			}
			else if(analog_dpad == 1)
			{
				x = (!!(data[5] & 0x04)*0x7FFF) - (!!(data[5] & 0x08))*0x7FFF;
				y = (!!(data[5] & 0x01)*0x7FFF) - (!!(data[5] & 0x02))*0x7FFF;
				rx -= 0x7FFF;
				ry  = 0x7FFF - ry;
			}
			else
			{
				x  -= 0x7FFF;
				y   = 0x7FFF - y;
				rx = (!!(data[5] & 0x04)*0x7FFF) - (!!(data[5] & 0x08))*0x7FFF;
				ry = (!!(data[5] & 0x01)*0x7FFF) - (!!(data[5] & 0x02))*0x7FFF;
			}
			
			input_report_abs(input, ABS_X, x);
			input_report_abs(input, ABS_Y, y);
			input_report_abs(input, ABS_RX, rx);
			input_report_abs(input, ABS_RY, ry);
			input_report_abs(input, ABS_TILT_X, gx);
			input_report_abs(input, ABS_TILT_Y, gy);

			input_report_key(input, BTN_A,			(data[3] & 0x08));
			input_report_key(input, BTN_B,			(data[3] & 0x04));
			input_report_key(input, BTN_X,			(data[3] & 0x02));
			input_report_key(input, BTN_Y,			(data[3] & 0x01));
			input_report_key(input, BTN_TL,			(data[5] & 0x40));
			input_report_key(input, BTN_TR,			(data[3] & 0x40));
			input_report_key(input, BTN_TL2,		(data[5] & 0x80));
			input_report_key(input, BTN_TR2,		(data[3] & 0x80));
			input_report_key(input, BTN_SELECT,		(data[4] & 0x01));
			input_report_key(input, BTN_START,		(data[4] & 0x02));
			input_report_key(input, BTN_MODE,		(data[4] & 0x10));
			input_report_key(input, BTN_EXTRA,		(data[4] & 0x20));
			input_report_key(input, BTN_THUMBL,		(data[4] & 0x08));
			input_report_key(input, BTN_THUMBR,		(data[4] & 0x04));
			if(!analog_dpad)
			{
				input_report_key(input, BTN_DPAD_UP,	(data[5] & 0x02));
				input_report_key(input, BTN_DPAD_DOWN,	(data[5] & 0x01));
				input_report_key(input, BTN_DPAD_LEFT,	(data[5] & 0x08));
				input_report_key(input, BTN_DPAD_RIGHT,	(data[5] & 0x04));
			}
			else
			{
				input_report_key(input, BTN_DPAD_UP,	false);
				input_report_key(input, BTN_DPAD_DOWN,	false);
				input_report_key(input, BTN_DPAD_LEFT,	false);
				input_report_key(input, BTN_DPAD_RIGHT,	false);
			}

			home_button = !!(data[4] & 0x10);
			left_button = !!(data[4] & 0x08);
			right_button = !!(data[4] & 0x04);
			triggerl_button = !!(data[5] & 0x40);
			triggerr_button = !!(data[3] & 0x40);
		}
		else
		{
			s16 x  = *((s16 *) (data + 4));
			s16 y  = *((s16 *) (data + 6));
			s16 rx = *((s16 *) (data + 8));
			s16 ry = *((s16 *) (data + 10));
			if(!analog_dpad)
			{
				x  -= 0x7FFF;
				y  -= 0x7FFF;
				rx -= 0x7FFF;
				ry -= 0x7FFF;
			}
			else if(analog_dpad == 1)
			{
				x = hatmap[data[3]].right*0x7FFF - hatmap[data[3]].left*0x7FFF;
				y = hatmap[data[3]].down*0x7FFF - hatmap[data[3]].up*0x7FFF;
				rx -= 0x7FFF;
				ry -= 0x7FFF;
			}
			else
			{
				x  -= 0x7FFF;
				y  -= 0x7FFF;
				rx = hatmap[data[3]].right*0x7FFF - hatmap[data[3]].left*0x7FFF;
				ry = hatmap[data[3]].down*0x7FFF - hatmap[data[3]].up*0x7FFF;
			}
			
			input_report_abs(input, ABS_X, x);
			input_report_abs(input, ABS_Y, y);
			input_report_abs(input, ABS_RX, rx);
			input_report_abs(input, ABS_RY, ry);
			input_report_abs(input, ABS_TILT_X, 0x00);
			input_report_abs(input, ABS_TILT_Y, 0x00);

			input_report_key(input, BTN_A,			(data[1] & 0x02));
			input_report_key(input, BTN_B,			(data[1] & 0x01));
			input_report_key(input, BTN_X,			(data[1] & 0x08));
			input_report_key(input, BTN_Y,			(data[1] & 0x04));
			input_report_key(input, BTN_TL,			(data[1] & 0x10));
			input_report_key(input, BTN_TR,			(data[1] & 0x20));
			input_report_key(input, BTN_TL2,		(data[1] & 0x40));
			input_report_key(input, BTN_TR2,		(data[1] & 0x80));
			input_report_key(input, BTN_SELECT,		(data[2] & 0x01));
			input_report_key(input, BTN_START,		(data[2] & 0x02));
			input_report_key(input, BTN_MODE,		(data[2] & 0x10));
			input_report_key(input, BTN_EXTRA,		(data[2] & 0x20));
			input_report_key(input, BTN_THUMBL,		(data[2] & 0x04));
			input_report_key(input, BTN_THUMBR,		(data[2] & 0x08));
			if(!analog_dpad)
			{
				input_report_key(input, BTN_DPAD_UP,	hatmap[data[3]].up);
				input_report_key(input, BTN_DPAD_DOWN,	hatmap[data[3]].down);
				input_report_key(input, BTN_DPAD_LEFT,	hatmap[data[3]].left);
				input_report_key(input, BTN_DPAD_RIGHT,	hatmap[data[3]].right);
			}
			else
			{
				input_report_key(input, BTN_DPAD_UP,	false);
				input_report_key(input, BTN_DPAD_DOWN,	false);
				input_report_key(input, BTN_DPAD_LEFT,	false);
				input_report_key(input, BTN_DPAD_RIGHT,	false);
			}

			home_button = !!(data[2] & 0x10);
			left_button = !!(data[2] & 0x04);
			right_button = !!(data[2] & 0x08);
			triggerl_button = !!(data[1] & 0x10);
			triggerr_button = !!(data[1] & 0x20);
		}
		input_sync(input);

		if(home_button)
		{
			time = ktime_get_ns();
			if(drvtime > 1 && (time - drvtime > 2000000000))
			{
				if(!(left_button || right_button))
				{
					spin_lock_irqsave(&drvdata->lock, flags);
					drvdata->event_cmd = PROCON_EVENT_TOGGLE_GYRO;
					drvdata->gyro_trigger = triggerl_button ? 1 : triggerr_button ? 2 : 0;
					drvdata->time = 1; // lock timer until key released
					spin_unlock_irqrestore(&drvdata->lock, flags);

					schedule_work(&drvdata->worker_event);
				}
				else if(left_button && !right_button)
				{
					spin_lock_irqsave(&drvdata->lock, flags);
					drvdata->event_cmd = PROCON_CMD_LED;
					drvdata->analog_dpad = analog_dpad == 1 ? 0 : 1;
					drvdata->time = 1;
					spin_unlock_irqrestore(&drvdata->lock, flags);
					
					schedule_work(&drvdata->worker_event);
				}
				else if(!left_button && right_button)
				{
					spin_lock_irqsave(&drvdata->lock, flags);
					drvdata->event_cmd = PROCON_CMD_LED;
					drvdata->analog_dpad = analog_dpad == 2 ? 0 : 2;
					drvdata->time = 1;
					spin_unlock_irqrestore(&drvdata->lock, flags);
					
					schedule_work(&drvdata->worker_event);
				}
			}
		}
		else
			time = 0;

		if((!home_button && drvtime) || (home_button && !drvtime))
		{
			spin_lock_irqsave(&drvdata->lock, flags);
			drvdata->time = time;
			spin_unlock_irqrestore(&drvdata->lock, flags);
		}
	}
	return 0;
}

static struct hid_driver procon_driver =
{
	.name =			"hid-procon",
	.probe =		procon_probe,
	.remove =		procon_remove,
	.raw_event =	procon_raw_event,
	.id_table = 	procon_table,
};
module_hid_driver(procon_driver);

