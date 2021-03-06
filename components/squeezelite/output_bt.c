/* 
 *  Squeezelite for esp32
 *
 *  (c) Sebastien 2019
 *      Philippe G. 2019, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
 
#include "driver/gpio.h"
#include "squeezelite.h"
#include "perf_trace.h"

extern struct outputstate output;
extern struct buffer *outputbuf;
extern struct buffer *streambuf;
extern u8_t *silencebuf;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)
#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

#define STATS_REPORT_DELAY_MS 15000

extern void hal_bluetooth_init(const char * options);
extern void hal_bluetooth_stop(void);
extern u8_t config_spdif_gpio;

static log_level loglevel;
static bool running = false;
static uint8_t *btout;
static frames_t oframes;

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
								
#define DECLARE_ALL_MIN_MAX \
	DECLARE_MIN_MAX(req);\
	DECLARE_MIN_MAX(rec);\
	DECLARE_MIN_MAX(bt);\
	DECLARE_MIN_MAX(under);\
	DECLARE_MIN_MAX(stream_buf);\
	DECLARE_MIN_MAX_DURATION(lock_out_time)								
	
#define RESET_ALL_MIN_MAX \
	RESET_MIN_MAX(bt);	\
	RESET_MIN_MAX(req);  \
	RESET_MIN_MAX(rec);  \
	RESET_MIN_MAX(under);  \
	RESET_MIN_MAX(stream_buf); \
	RESET_MIN_MAX_DURATION(lock_out_time)
	
DECLARE_ALL_MIN_MAX;	
	
void output_init_bt(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
#ifdef CONFIG_SQUEEZEAMP
	gpio_pad_select_gpio(config_spdif_gpio);
	gpio_set_direction(config_spdif_gpio, GPIO_MODE_OUTPUT);
	gpio_set_level(config_spdif_gpio, 0);
#endif			
	loglevel = level;
	running = true;
	output.write_cb = &_write_frames;
	hal_bluetooth_init(device);
}

void output_close_bt(void) {
	LOCK;
	running = false;
	UNLOCK;
	hal_bluetooth_stop();
}	

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
						 s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {
	
	assert(btout != NULL);
	
	if (!silence ) {
				
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}

		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}

#if BYTES_PER_FRAME == 4
		memcpy(btout + oframes * BYTES_PER_FRAME, outputbuf->readp, out_frames * BYTES_PER_FRAME);
#else
	{
		frames_t count = out_frames;
		s32_t *_iptr = (s32_t*) outputbuf->readp;
		s16_t *_optr = (s16_t*) (btout + oframes * BYTES_PER_FRAME);
		while (count--) {
			*_optr++ = *_iptr++ >> 16;
			*_optr++ = *_iptr++ >> 16;
		}
	}
#endif

	} else {

		u8_t *buf = silencebuf;
		memcpy(btout + oframes * BYTES_PER_FRAME, buf, out_frames * BYTES_PER_FRAME);
	}

	return (int)out_frames;
}

int32_t output_bt_data(uint8_t *data, int32_t len) {
	int32_t avail_data = 0, wanted_len = 0, start_timer = 0;

	if (len < 0 || data == NULL || !running) {
		return 0;
	}
	
	btout = data;
	oframes = 0;

	// This is how the BTC layer calculates the number of bytes to
	// for us to send. (BTC_SBC_DEC_PCM_DATA_LEN * sizeof(OI_INT16) - availPcmBytes
	wanted_len=len;
	SET_MIN_MAX(len,req);
	TIME_MEASUREMENT_START(start_timer);
	LOCK;
	output.device_frames = 0; // todo: check if this is the right way do to this.
	output.updated = gettime_ms();
	output.frames_played_dmp = output.frames_played;
	SET_MIN_MAX_SIZED(_buf_used(outputbuf),bt,outputbuf->size);
	do {
		avail_data = _output_frames( wanted_len/BYTES_PER_FRAME )*BYTES_PER_FRAME; // Keep the transfer buffer full
		wanted_len-=avail_data;
	} while (wanted_len > 0 && avail_data != 0);
	
	if (wanted_len > 0) {
		SET_MIN_MAX(wanted_len, under);
	}
	output.frames_in_process = len-wanted_len;

	UNLOCK;
	SET_MIN_MAX(TIME_MEASUREMENT_GET(start_timer),lock_out_time);
	SET_MIN_MAX((len-wanted_len), rec);
	TIME_MEASUREMENT_START(start_timer);

	return len-wanted_len;
}

void output_bt_tick(void) {
	static time_t lastTime=0;
	
	if (!running) return;
		
	LOCK_S;
    SET_MIN_MAX_SIZED(_buf_used(streambuf), stream_buf, streambuf->size);
    UNLOCK_S;
	
	if (lastTime <= gettime_ms() )
	{
		lastTime = gettime_ms() + STATS_REPORT_DELAY_MS;
		LOG_INFO("Statistics over %u secs. " , STATS_REPORT_DELAY_MS/1000);
		LOG_INFO("              +==========+==========+================+=====+================+");
		LOG_INFO("              |      max |      min |        average | avg |          count |");
		LOG_INFO("              |  (bytes) |  (bytes) |        (bytes) | pct |                |");
		LOG_INFO("              +==========+==========+================+=====+================+");
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("stream avl",stream_buf));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("output avl",bt));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("requested",req));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("underrun",under));
		LOG_INFO( "              +==========+==========+================+=====+================+");
		LOG_INFO("\n");
		LOG_INFO("              ==========+==========+===========+===========+  ");
		LOG_INFO("              max (us)  | min (us) |   avg(us) |  count    |  ");
		LOG_INFO("              ==========+==========+===========+===========+  ");
		LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Out Buf Lock",lock_out_time));
		LOG_INFO("              ==========+==========+===========+===========+");
		RESET_ALL_MIN_MAX;
	}	
}	