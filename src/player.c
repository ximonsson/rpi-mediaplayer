// ffmpeg
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include "bcm_host.h"
#include "ilclient.h"
#include "rpi_mp.h"
#include "rpi_mp_packet_buffer.h"
#include "rpi_mp_utils.h"

#define FIFO_SLEEPY_TIME               10000
#define DIGITAL_AUDIO_DESTINATION_NAME "hdmi"
#define ANALOG_AUDIO_DESTINATION_NAME  "local"


/* OMX Component ports --------------------- */
enum omxports
{
	VIDEO_DECODE_INPUT_PORT     = 130,
	VIDEO_DECODE_OUT_PORT       = 131,
	VIDEO_RENDER_INPUT_PORT     =  90,
	VIDEO_SCHEDULER_INPUT_PORT  =  10,
	VIDEO_SCHEDULER_OUT_PORT    =  11,
	VIDEO_SCHEDULER_CLOCK_PORT  =  12,
	EGL_RENDER_INPUT_PORT       = 220,
	EGL_RENDER_OUT_PORT         = 221,
	AUDIO_RENDER_INPUT_PORT     = 100,
	AUDIO_RENDER_CLOCK_PORT     = 101,
	CLOCK_VIDEO_PORT            =  80,
	CLOCK_AUDIO_PORT            =  81
};

/* FLAGS ----------------------------------- */
enum flags
{
	STOPPED               = 0x0001,
	PAUSED                = 0x0002,
	FIRST_VIDEO           = 0x0004,
	FIRST_AUDIO           = 0x0008,
	PORT_SETTINGS_CHANGED = 0x0010,
	HARDWARE_DECODE_AUDIO = 0x0020,
	DONE_READING          = 0x0040,
	RENDER_2_TEXTURE      = 0x0080,
	VIDEO_PAUSED          = 0x0100,
	AUDIO_PAUSED          = 0x0200,
	VIDEO_STOPPED         = 0x0400,
	AUDIO_STOPPED         = 0x0800,
	ANALOG_AUDIO_OUT      = 0x1000,
};

#define OUT_CHANNELS(num_channels) ((num_channels) > 4 ? 8 : (num_channels) > 2 ? 4 : (num_channels))
#define WAIT_WHILE_PAUSED { pthread_mutex_lock (&pause_mutex); ret = pthread_cond_wait (&pause_condition, &pause_mutex); pthread_mutex_unlock (&pause_mutex); }
#define SET_FLAG(flag) { pthread_mutex_lock (&flags_mutex); flags |= flag; pthread_mutex_unlock (&flags_mutex); }
#define UNSET_FLAG(flag) { pthread_mutex_lock (&flags_mutex); flags &= ~flag; pthread_mutex_unlock (&flags_mutex); }
#define OMX_INIT_PARAM(type) memset (&type, 0x0, sizeof (type)); type.nSize = sizeof (type); type.nVersion.nVersion = OMX_VERSION;

// Demuxing variables (ffmpeg)
static AVFormatContext      * fmt_ctx          = NULL;
static AVCodecContext       * video_codec_ctx  = NULL,
                            * audio_codec_ctx  = NULL;
static AVStream             * video_stream     = NULL,
                            * audio_stream     = NULL;
static int                    video_stream_idx =   -1,
                              audio_stream_idx =   -1;
static AVPacket               av_packet,
                              video_packet,
                              audio_packet;
static AVFrame              * av_frame;

// Decoding variables (OMX)
static COMPONENT_T          * video_decode    = NULL,
                            * video_scheduler = NULL,
                            * video_render    = NULL,
                            * video_clock     = NULL,
                            * audio_decode    = NULL,
                            * audio_render    = NULL,
                            * egl_render      = NULL;

static TUNNEL_T               video_tunnel[4];
static TUNNEL_T               audio_tunnel[3];
static COMPONENT_T          * list[7];
static ILCLIENT_T           * client;

static OMX_BUFFERHEADERTYPE * omx_video_buffer,
                            * omx_audio_buffer,
                            * omx_egl_buffer;

static void                 * egl_image = NULL;
static int32_t                flags     =  0;

// Helpers
static packet_buffer video_packet_fifo, audio_packet_fifo;

// Thread variables
static pthread_mutex_t flags_mutex        = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pause_mutex        = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t video_mutex        = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audio_mutex        = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pause_condition    = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t buffer_filled_mut  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  buffer_filled_cond = PTHREAD_COND_INITIALIZER;


/**
 *  Convert PTS (from libFFmpeg) to OMX_TICKS struct.
 */
static inline OMX_TICKS pts__omx_timestamp (double pts)
{
	OMX_TICKS ticks;
	ticks.nLowPart 	= (int64_t) pts;
	ticks.nHighPart = (int64_t) pts >> 32;
	return ticks;
}

/**
 *	Converts AVPacket timestamp to an OMX_TICKS timestamp
 */
static inline OMX_TICKS omx_timestamp (AVPacket p)
{
	uint64_t pts = p.pts != AV_NOPTS_VALUE ? p.pts : p.dts != AV_NOPTS_VALUE ? p.dts : 0;
	int      num = fmt_ctx->streams[p.stream_index]->time_base.num;
	int      den = fmt_ctx->streams[p.stream_index]->time_base.den;

	double timestamp = (double) pts * num / den * AV_TIME_BASE;
	return pts__omx_timestamp (timestamp);
}

/**
 *  Lock decoding threads, i.e. pause.
 */
static inline void lock ()
{
	pthread_mutex_lock (&video_mutex);
	pthread_mutex_lock (&audio_mutex);
}

/**
 *  Unlock decoding threads, i.e. unpause.
 */
static inline void unlock ()
{
	pthread_mutex_unlock (&video_mutex);
	pthread_mutex_unlock (&audio_mutex);
}

/**
 *  Fill the EGL render buffer with decoded raw image data.
 *  Should only be called as callback for the fillbuffer event when video decoding
 *  of a frame is finished.
 */
static void fill_egl_texture_buffer (void* data, COMPONENT_T* c)
{
	pthread_mutex_lock (&buffer_filled_mut);
	if ((~flags & STOPPED) &&
		OMX_FillThisBuffer (ilclient_get_handle (egl_render), omx_egl_buffer) != OMX_ErrorNone)
	{
		fprintf (stderr, "OMX_FillThisBuffer failed for egl buffer in callback\n");
	}
	pthread_cond_broadcast (&buffer_filled_cond);
	pthread_mutex_unlock (&buffer_filled_mut);
}

/**
 *	Decodes the current AVPacket as containing video data.
 *  @return int 0 on success, non-zero on error
 */
static inline int decode_video_packet ()
{
	int packet_size = 0;
	OMX_TICKS ticks = omx_timestamp (video_packet);
	// packet data can be larger than decoder buffer
	while (video_packet.size > 0)
	{
		// feed data to video decoder
		if ((omx_video_buffer = ilclient_get_input_buffer (video_decode, VIDEO_DECODE_INPUT_PORT, 1)) == NULL)
		{
			fprintf (stderr, "Error getting buffer to video decoder\n");
			return 1;
		}
		packet_size                  = video_packet.size > omx_video_buffer->nAllocLen ? omx_video_buffer->nAllocLen : video_packet.size;
		omx_video_buffer->nFilledLen = packet_size;
		omx_video_buffer->nOffset    = 0;
		omx_video_buffer->nFlags     = 0;
		omx_video_buffer->nTimeStamp = ticks;
		// copy data to buffer
		memcpy (omx_video_buffer->pBuffer, video_packet.data, packet_size);
		video_packet.size -= packet_size;
		video_packet.data += packet_size;

		if (flags & FIRST_VIDEO)
		{
			omx_video_buffer->nFlags = OMX_BUFFERFLAG_STARTTIME;
			UNSET_FLAG (FIRST_VIDEO)
		}
		else if (omx_video_buffer->nTimeStamp.nLowPart == 0 && omx_video_buffer->nTimeStamp.nHighPart == 0)
			omx_video_buffer->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;

		// end of frame
		if (video_packet.size == 0)
			omx_video_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

		// Check for changes in port settings
		if ((~flags & PORT_SETTINGS_CHANGED) && (
		    (packet_size >  0 && ilclient_remove_event   (video_decode, OMX_EventPortSettingsChanged, VIDEO_DECODE_OUT_PORT, 0, 0, 1) == 0 ) ||
		    (packet_size == 0 && ilclient_wait_for_event (video_decode, OMX_EventPortSettingsChanged, VIDEO_DECODE_OUT_PORT, 0, 0, 1, ILCLIENT_EVENT_ERROR | ILCLIENT_PARAMETER_CHANGED, 10000) == 0)))
		{
			SET_FLAG (PORT_SETTINGS_CHANGED)
			// setup tunnel between video decoder and scheduler
			if (ilclient_setup_tunnel (video_tunnel, 0, 0) != 0)
			{
				fprintf (stderr, "Error setting up tunnel between video decoder and scheduler\n");
				return 1;
			}
			ilclient_change_component_state (video_scheduler, OMX_StateExecuting);
			// setup tunnel between video scheduler and render
			if (ilclient_setup_tunnel (video_tunnel + 1, 0, 1000) != 0)
			{
				fprintf (stderr, "Error setting up tunnel between video scheduler and render\n");
				return 1;
			}
			// if we are rendering to texture we need to some setup to the egl component
			if (flags & RENDER_2_TEXTURE)
			{
				ilclient_change_component_state (egl_render, OMX_StateIdle);
				// Enable the output port and tell egl_render to use the texture as a buffer
				//ilclient_enable_port(egl_render, 221); THIS BLOCKS SO CANT BE USED
				if (OMX_SendCommand (ILC_GET_HANDLE (egl_render), OMX_CommandPortEnable, EGL_RENDER_OUT_PORT, NULL) != OMX_ErrorNone)
				{
					fprintf (stderr, "OMX_CommandPortEnable failed.\n");
					return 1;
				}
				if (OMX_UseEGLImage (ILC_GET_HANDLE (egl_render), &omx_egl_buffer, EGL_RENDER_OUT_PORT, NULL, egl_image) != OMX_ErrorNone)
				{
					fprintf (stderr, "OMX_UseEGLImage failed.\n");
					return 1;
				}
				// Set egl_render to executing
				ilclient_change_component_state (egl_render, OMX_StateExecuting);
				// Request egl_render to write data to the texture buffer
				if (OMX_FillThisBuffer (ILC_GET_HANDLE (egl_render), omx_egl_buffer) != OMX_ErrorNone)
				{
					fprintf (stderr, "OMX_FillThisBuffer failed for egl buffer.\n");
					return 1;
				}
			}
			// if we are not rendering to texture we just need to change the video renderer to excecuting
			else
				ilclient_change_component_state (video_render, OMX_StateExecuting);
		}
		// empty buffer
		if (OMX_EmptyThisBuffer (ILC_GET_HANDLE (video_decode), omx_video_buffer) != OMX_ErrorNone)
		{
			fprintf (stderr, "Error emptying video decode buffer\n");
			return 1;
		}
	}
	return 0;
}

/**
 *  Thread for decoding video packets.
 *  Polls the video packet buffer for new packets to decode and
 *  present on screen.
 */
static void video_decoding_thread ()
{
	uint8_t *d;
	int ret;
	while (~flags & STOPPED && (~flags & DONE_READING || video_packet_fifo.n_packets))
	{
		// check pause
		if (flags & PAUSED)
		{
			WAIT_WHILE_PAUSED
		}
		// get packet
		pthread_mutex_lock (&video_mutex);
		if ((ret = pop_packet (&video_packet_fifo, &video_packet)) != 0)
		{
			pthread_mutex_unlock (&video_mutex);
			usleep (FIFO_SLEEPY_TIME);
			continue;
		}
		// decode
		d = video_packet.data;
		ret = decode_video_packet ();
		video_packet.data = d;
		av_packet_unref (&video_packet);
		pthread_mutex_unlock (&video_mutex);
		if (ret != 0)
		{
			fprintf (stderr, "Error while decoding, ending thread\n");
			break;
		}
	}
	printf ("stopping video decoding thread\n");
}

/**
	Decode audio packet (using FFMPEG) and send it to hardware for rendering
 *	return int 0 on success, non-zero on failure
 */
static inline int decode_audio_packet ()
{
	int got_frame = 0, ret = 0, data_size = 0;
	uint8_t *audio_data, *audio_data_p = NULL;
	OMX_TICKS ticks = omx_timestamp (audio_packet);

	// some audio decoders only decode part of the data
	while (audio_packet.size > 0)
	{
		if ((ret = avcodec_decode_audio4 (audio_codec_ctx, av_frame, &got_frame, &audio_packet)) < 0)
		{
			fprintf (stderr, "Error decoding audio packet \n");
			return ret; // we return that it's alright to continue
		}
		audio_packet.size -= ret;
		audio_packet.data += ret;

		if (got_frame)
		{
			if ((data_size = av_samples_get_buffer_size (NULL,
                                                         audio_codec_ctx->channels,
                                                         av_frame->nb_samples,
                                                         audio_codec_ctx->sample_fmt,
                                                         1)) <= 0)
			{
				fprintf (stderr, "Error getting samples buffer size\n");
				break;
			}
			int bps = av_get_bytes_per_sample (audio_codec_ctx->sample_fmt);

			// interleave data if it is planar
			if (av_sample_fmt_is_planar (audio_codec_ctx->sample_fmt))
			{
				int i, ch;
				audio_data = (uint8_t *) malloc (data_size);
				audio_data_p = audio_data;
				for (i = 0; i < av_frame->nb_samples; i ++)
					for (ch = 0; ch < audio_codec_ctx->channels; ch ++, audio_data_p += bps)
						memcpy (audio_data_p, av_frame->data[ch] + i * bps, bps);
				audio_data_p = audio_data;
			}
			else
				audio_data = av_frame->data[0];

			// if 32-bit we need to resample to 16-bit
			// (we are assuming it's floating point in this case)
			if (bps > 2)
			{
				uint8_t *tmp = NULL;
				flt_to_s16 (audio_data, &tmp, data_size);

				if (audio_data_p)
					free (audio_data_p);

				data_size /= 2;
				audio_data = audio_data_p = tmp;
			}

			// send frame data to audio render
			while (data_size > 0)
			{
				if ((omx_audio_buffer = ilclient_get_input_buffer (audio_render, AUDIO_RENDER_INPUT_PORT, 1)) == NULL)
				{
					fprintf ( stderr, "Error getting buffer to audio decoder\n" );
					return 1; // errors with hardware, stop trying to render audio
				}
				omx_audio_buffer->nFilledLen = data_size > omx_audio_buffer->nAllocLen ? omx_audio_buffer->nAllocLen : data_size;
				omx_audio_buffer->nOffset    = 0;
				omx_audio_buffer->nFlags	 = 0;
				// copy data
				memcpy (omx_audio_buffer->pBuffer, audio_data, omx_audio_buffer->nFilledLen);
				audio_data += omx_audio_buffer->nFilledLen;
				data_size  -= omx_audio_buffer->nFilledLen;

				// first audio packet of stream
				if (flags & FIRST_AUDIO)
				{
					omx_audio_buffer->nFlags = OMX_BUFFERFLAG_STARTTIME;
					UNSET_FLAG (FIRST_AUDIO)
				}
				else
				{
					omx_audio_buffer->nTimeStamp = ticks;
					if (omx_audio_buffer->nTimeStamp.nLowPart == 0 && omx_audio_buffer->nTimeStamp.nHighPart == 0)
						omx_audio_buffer->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;
				}
				// last packet of frame
				if (data_size == 0 && audio_packet.size == 0)
					omx_audio_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
				// empty the buffer for render
				if (OMX_EmptyThisBuffer (ILC_GET_HANDLE (audio_render), omx_audio_buffer) != OMX_ErrorNone)
				{
					fprintf (stderr, "Error emptying audio render buffer\n");
					return 1; // errors with hardware, stop trying to render audio
				}
			}
		}
	}
	if (audio_data_p)
		free (audio_data_p);
	audio_packet.size = 0;
	audio_packet.data = NULL;
	return 0;
}


static int hardwaredecode_audio_packet ()
{
	OMX_TICKS ticks;
	while (audio_packet.size > 0)
	{
		// get buffer handler to audio decoder
		if ((omx_audio_buffer = ilclient_get_input_buffer (audio_decode, 120, 1)) == NULL)
		{
			fprintf (stderr, "Error getting buffer to audio decoder\n");
			return 1;
		}
		// copy data to the buffer
		omx_audio_buffer->nFilledLen = audio_packet.size < omx_audio_buffer->nAllocLen ? audio_packet.size : omx_audio_buffer->nAllocLen;
		memcpy (omx_audio_buffer->pBuffer, audio_packet.data, omx_audio_buffer->nFilledLen);

		audio_packet.size -= omx_audio_buffer->nFilledLen;
		audio_packet.data += omx_audio_buffer->nFilledLen;

		omx_audio_buffer->nOffset = 0;
		omx_audio_buffer->nFlags  = OMX_BUFFERFLAG_TIME_UNKNOWN;

		// first audio packet
		if (flags & FIRST_AUDIO)
		{
			omx_audio_buffer->nFlags = OMX_BUFFERFLAG_STARTTIME;
			UNSET_FLAG (FIRST_AUDIO)
		}
		ticks.nLowPart  = audio_packet.pts;
		ticks.nHighPart = audio_packet.pts >> 32;
		omx_audio_buffer->nTimeStamp = ticks;
		if (OMX_EmptyThisBuffer (ILC_GET_HANDLE (audio_decode), omx_audio_buffer) != OMX_ErrorNone)
		{
			fprintf (stderr, "Error emptying audio render buffer\n");
			return 1; // errors with hardware, stop trying to render audio
		}
	}
	return 0;
}

/**
 *  Audio decoding thread.
 *  Polls the audio packet buffer for new packets to decode
 *  and send for playback.
 */
static void audio_decoding_thread ()
{
	// AVPacket tmp_pack;
	uint8_t *d;
	int ret;
	while (~flags & STOPPED)
	{
		// check if we are done demuxing
		if (flags & DONE_READING && !audio_packet_fifo.n_packets)
			break;
		// paused
		if (flags & PAUSED)
		{
			WAIT_WHILE_PAUSED
		}
		// pop a audio packet from the decoding queue
		pthread_mutex_lock (&audio_mutex);
		if ((ret = pop_packet (&audio_packet_fifo, &audio_packet)) != 0)
		{
			pthread_mutex_unlock (&audio_mutex);
			usleep (FIFO_SLEEPY_TIME);
			continue;
		}
		// send data for decoding
		d = audio_packet.data;
		ret = flags & HARDWARE_DECODE_AUDIO ? hardwaredecode_audio_packet () : decode_audio_packet () ;
		audio_packet.data = d;
		pthread_mutex_unlock (&audio_mutex);

		// deallocate packet
		if (ret == 0)
			av_packet_unref (&audio_packet);
		else if (ret > 0)
		{
			fprintf (stderr, "Error while decoding audio packet, ending thread\n");
			break;
		}
	}
	printf ("stopping audio decoding thread\n");
}

/**
 *  Takes the current demuxed packet and sorts it to the correct buffer polled
 *  by decoding threads.
 */
static inline int process_packet ()
{
	int ret = 0;
	packet_buffer* buf = NULL;
	// negative size ???
	if (av_packet.size < 0)
		return ret;

	// current packet is video
	if (av_packet.stream_index == video_stream_idx)
		buf = &video_packet_fifo;
	// current packet is audio
	else if (av_packet.stream_index == audio_stream_idx)
		buf = &audio_packet_fifo;
	// not interrested
	else
		return ret;

	// the buffer might be full therefor we need to keep trying until there room has been
	// made by either decoding threads, hence the while loop
	while (~flags & STOPPED)
	{
		if (flags & PAUSED)
		WAIT_WHILE_PAUSED
		ret = push_packet (buf, av_packet);
		// if we successfully added the packet break
		if (ret == 0)
			break;
		// sleep a little to save CPU
		usleep (FIFO_SLEEPY_TIME);
	}
	return 0;
}

/**
 *	Open video.
 *	Create components and setup tunnels and buffers between them.
 *  @return int 0 on success, non-zero on failure.
 */
static int open_video ()
{
	int ret = 0;
	OMX_VIDEO_PARAM_PORTFORMATTYPE video_format;
	int render_input_port = VIDEO_RENDER_INPUT_PORT;

	memset (video_tunnel, 0, sizeof (video_tunnel));
	// create video decode component
	if (ilclient_create_component (client, &video_decode, "video_decode", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS) != 0)
	{
		fprintf (stderr, "Error creating IL COMPONENT video decoder\n");
		ret = -14;
	}
	list[0] = video_decode;

	// create the render component which is either a video_render (the display) or egl_render (texture)
	if (flags & RENDER_2_TEXTURE)
	{
		// ilclient_set_fill_buffer_done_callback (client, fill_egl_texture_buffer, 0);
		// create egl_render component
		if (ilclient_create_component (client, &egl_render, "egl_render", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_OUTPUT_BUFFERS) != 0)
		{
			fprintf (stderr, "Error creating IL COMPONENT egl render\n");
			ret = -14;
		}
		list[1] = egl_render;
		render_input_port = EGL_RENDER_INPUT_PORT;
	}
	else
	{
		// create video render component
		if (ilclient_create_component (client, &video_render, "video_render", ILCLIENT_DISABLE_ALL_PORTS) != 0)
		{
			fprintf (stderr, "Error creating IL COMPONENT video render\n");
			ret = -14;
		}
		list[1] = video_render;
	}
	// create video scheduler
	if (ilclient_create_component (client, &video_scheduler, "video_scheduler", ILCLIENT_DISABLE_ALL_PORTS) != 0)
	{
		fprintf (stderr, "Error creating IL COMPONENT video scheduler\n");
		ret = -13;
	}
	list[3] = video_scheduler;
	// setup tunnels
	set_tunnel (video_tunnel, 		video_decode, 		 VIDEO_DECODE_OUT_PORT, 	video_scheduler, 	VIDEO_SCHEDULER_INPUT_PORT);
	set_tunnel (video_tunnel + 1, 	video_scheduler, 	 VIDEO_SCHEDULER_OUT_PORT,  list[1], 			render_input_port);
	set_tunnel (video_tunnel + 2, 	video_clock, 		 CLOCK_VIDEO_PORT, 			video_scheduler, 	VIDEO_SCHEDULER_CLOCK_PORT);
	// setup clock tunnel
	if (ilclient_setup_tunnel (video_tunnel + 2, 0, 0) != 0)
	{
		fprintf (stderr, "Error setting up tunnel\n");
		ret = -15;
	}
	// setup decoding
	if (ret == 0)
		ilclient_change_component_state (video_decode, OMX_StateIdle);

	memset (&video_format, 0, sizeof (OMX_VIDEO_PARAM_PORTFORMATTYPE));
	video_format.nSize 			     = sizeof (OMX_VIDEO_PARAM_PORTFORMATTYPE);
	video_format.nVersion.nVersion   = OMX_VERSION;
	video_format.nPortIndex 		 = VIDEO_DECODE_INPUT_PORT;

	if (video_stream->r_frame_rate.den > 0)
		video_format.xFramerate	= (long long) (video_stream->r_frame_rate.num / video_stream->r_frame_rate.den) * (1 << 16);

	switch (video_codec_ctx->codec_id)
	{
		case AV_CODEC_ID_H264:
			video_format.eCompressionFormat = OMX_VIDEO_CodingAVC;
			break;

		case AV_CODEC_ID_MPEG4:
			video_format.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
			break;

		case AV_CODEC_ID_MPEG2VIDEO:
			video_format.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
			break;

		default:
			video_format.eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
			break;
	}
	// set format parameters for video decoder
	if (OMX_SetParameter (ILC_GET_HANDLE (video_decode), OMX_IndexParamVideoPortFormat, &video_format) != OMX_ErrorNone)
	{
		fprintf (stderr, "Error setting port format parameter on video decoder \n");
		return 1;
	}
	// enable video decoder buffers
	if (ilclient_enable_port_buffers (video_decode, VIDEO_DECODE_INPUT_PORT, NULL, NULL, NULL) == 0)
	{
		ilclient_change_component_state (video_decode, OMX_StateExecuting);
		// send decoding extra information
		if (video_codec_ctx->extradata)
		{
			if ((omx_video_buffer = ilclient_get_input_buffer (video_decode, VIDEO_DECODE_INPUT_PORT, 1)) == NULL)
			{
				fprintf (stderr, "Error getting input buffer to video decoder to send decoding information\n");
				return 1;
			}
			omx_video_buffer->nOffset = 0;
			omx_video_buffer->nFilledLen = video_codec_ctx->extradata_size;
			memset (omx_video_buffer->pBuffer, 0x0, omx_video_buffer->nAllocLen);
			memcpy (omx_video_buffer->pBuffer, video_codec_ctx->extradata, video_codec_ctx->extradata_size);
			omx_video_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

			if (OMX_EmptyThisBuffer (ILC_GET_HANDLE (video_decode), omx_video_buffer) != OMX_ErrorNone)
			{
				fprintf (stderr, "Error emptying buffer with extra decoder information\n");
				return 1;
			}
		}
	}
	else
	{
		fprintf (stderr, "Could not enable port buffers on video decoder\n");
		return 1;
	}
	return 0;
}

/**
 *	Close video
 * 	Destroy components and tunnels between them,
 */
static void close_video ()
{
	if ((omx_video_buffer = ilclient_get_input_buffer (video_decode, VIDEO_DECODE_INPUT_PORT, 1)) != NULL)
	{
		omx_video_buffer->nFilledLen = 0;
		omx_video_buffer->nFlags 	 = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_TIME_UNKNOWN;
		if (OMX_EmptyThisBuffer (ILC_GET_HANDLE (video_decode), omx_video_buffer) != OMX_ErrorNone)
            fprintf (stderr, "error emptying last buffer =/\n");
	}
	else
        fprintf (stderr, "Could not send EOS flag to video decoder\n");

	// wait for EOS from render
	if (~flags & RENDER_2_TEXTURE)
		ilclient_wait_for_event (video_render, OMX_EventBufferFlag, VIDEO_RENDER_INPUT_PORT, 0, OMX_BUFFERFLAG_EOS, 0, ILCLIENT_BUFFER_FLAG_EOS, 10000);

	// need to flush the renderer to allow video_decode to disable its input port
	ilclient_flush_tunnels        (video_tunnel, 0);
	ilclient_disable_port_buffers (video_decode, VIDEO_DECODE_INPUT_PORT, NULL, NULL, NULL);
	ilclient_disable_tunnel       (video_tunnel);
	ilclient_disable_tunnel       (video_tunnel + 1);
	ilclient_disable_tunnel       (video_tunnel + 2);
	ilclient_teardown_tunnels     (video_tunnel);

	if (video_codec_ctx)
        avcodec_close (video_codec_ctx);
}

/**
 *	Open audio
 *	Create audio components and tunnels with their buffers.
 */
static int open_audio ()
{
	int ret = 0;
	OMX_CONFIG_BRCMAUDIODESTINATIONTYPE audio_destination;
	OMX_ERRORTYPE omx_error;
	OMX_AUDIO_PARAM_PCMMODETYPE pcm;
	OMX_AUDIO_PARAM_PORTFORMATTYPE audio_format;

	memset (audio_tunnel, 0, sizeof (audio_tunnel));

	// create audio render component
	if (ilclient_create_component (client, &audio_render, "audio_render", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS) != 0)
	{
		fprintf (stderr, "Error creating IL COMPONENT audio render\n");
		ret = -14;
	}
	list[4] = audio_render;

	// setup audio decoder parameters
	// 	this will be used if audio decoding is supported by the hardware
	memset (&audio_format, 0x0, sizeof (OMX_AUDIO_PARAM_PORTFORMATTYPE));
	audio_format.nSize 				= sizeof (OMX_AUDIO_PARAM_PORTFORMATTYPE);
	audio_format.nVersion.nVersion 	= OMX_VERSION;
	audio_format.nPortIndex 		= 120;

	// check if we can decode audio on hardware
	switch (audio_codec_ctx->codec_id)
	{
		case AV_CODEC_ID_MP2:
		case AV_CODEC_ID_MP3:
			audio_format.eEncoding = OMX_AUDIO_CodingMP3;
		break;

		case AV_CODEC_ID_DTS:
			audio_format.eEncoding = OMX_AUDIO_CodingDTS;
			SET_FLAG ( HARDWARE_DECODE_AUDIO )
		break;

    	case AV_CODEC_ID_AC3:
    	case AV_CODEC_ID_EAC3:
			audio_format.eEncoding = OMX_AUDIO_CodingDDP;
		break;

		default:
            break;
	}

	// if the hardware supports the audio encoder we setup new IL components to handle audio decoding
	if (flags & HARDWARE_DECODE_AUDIO)
	{
		printf ("We will be decoding audio on the hardware\n");
		// create component
		if (ilclient_create_component (client, &audio_decode, "audio_decode", ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS) != 0)
		{
			fprintf (stderr, "Error create IL COMPONENT audio decoder\n");
			ret = -14;
		}
		list[5] = audio_decode;

		// setup tunnels between audio decoder and audio renderer, as well as between clock and renderer
		set_tunnel (audio_tunnel,     audio_decode,  121, audio_render, 100);
		set_tunnel (audio_tunnel + 1, video_clock,    81, audio_render, 101);

		// set it to idle, that way we can modify it
		if (ilclient_change_component_state (audio_decode, OMX_StateIdle) != 0)
            fprintf (stderr, "error settings audio decoder component to idle\n");

		// set parameters and enable its buffers
		if ((omx_error = OMX_SetParameter (ILC_GET_HANDLE (audio_decode), OMX_IndexParamAudioPortFormat, &audio_format)) != OMX_ErrorNone ||
		     ilclient_enable_port_buffers (audio_decode, 120, NULL, NULL, NULL) != 0)
		{
			if (omx_error != OMX_ErrorNone)
				fprintf (stderr, "Error setting parameters for audio decoder. OMX ERROR: 0x%08x\n", omx_error);
			else
                fprintf (stderr, "Error enabling port buffers for audio decoder\n");

			return 1;
		}
		// now it's ready - set it as executing
		ilclient_change_component_state (audio_decode, OMX_StateExecuting);
	}
	// else we just create a tunnel between the clock and audio renderer
	else
        set_tunnel (audio_tunnel, video_clock, 81, audio_render, 101);

	// setup clock tunnel
	if (ilclient_setup_tunnel (audio_tunnel, 0, 0 ) != 0)
	{
		fprintf (stderr, "Error setting up tunnel between clock and audio render.\n");
		ret = -15;
		return ret;
	}

	if (flags & HARDWARE_DECODE_AUDIO)
		// setup decode tunnel
		if (ilclient_setup_tunnel (audio_tunnel + 1, 0, 0) != 0)
		{
			fprintf (stderr, "Error setting up tunnel between decoder and audio render.\n");
			ret = -15;
			return ret;
		}

	ilclient_change_component_state (audio_render, OMX_StateIdle);

	// set audio destination
	memset (&audio_destination, 0x0, sizeof (OMX_CONFIG_BRCMAUDIODESTINATIONTYPE));
	char* destination_name 			    = flags & ANALOG_AUDIO_OUT ? ANALOG_AUDIO_DESTINATION_NAME : DIGITAL_AUDIO_DESTINATION_NAME;
	audio_destination.nSize 			= sizeof (OMX_CONFIG_BRCMAUDIODESTINATIONTYPE);
	audio_destination.nVersion.nVersion = OMX_VERSION;
	strcpy ((char*) audio_destination.sName, destination_name);

	if ((omx_error = OMX_SetConfig (ILC_GET_HANDLE (audio_render), OMX_IndexConfigBrcmAudioDestination, &audio_destination)) != OMX_ErrorNone)
	{
		fprintf (stderr, "Error setting audio destination: 0x%08x\n", omx_error);
		return 1;
	}
	// set the PCM parameters
	memset (&pcm, 0, sizeof (OMX_AUDIO_PARAM_PCMMODETYPE));
	pcm.nSize 				= sizeof (OMX_AUDIO_PARAM_PCMMODETYPE);
	pcm.nVersion.nVersion 	= OMX_VERSION;
	pcm.nPortIndex 			= AUDIO_RENDER_INPUT_PORT;
	pcm.nChannels 			= OUT_CHANNELS (audio_codec_ctx->channels);
	pcm.eNumData 			= OMX_NumericalDataSigned;
	pcm.eEndian 			= OMX_EndianLittle;
	pcm.nSamplingRate 		= audio_codec_ctx->sample_rate;
	pcm.bInterleaved 		= OMX_TRUE;
	pcm.ePCMMode 			= OMX_AUDIO_PCMModeLinear;

	switch (audio_codec_ctx->sample_fmt)
	{
		case AV_SAMPLE_FMT_U8:
		case AV_SAMPLE_FMT_U8P:
			pcm.nBitPerSample 						= 8;
			audio_codec_ctx->bits_per_coded_sample 	= 8;
		break;

		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
		case AV_SAMPLE_FMT_S32:
		case AV_SAMPLE_FMT_S32P:
		default:
			pcm.nBitPerSample 						= 16;
			audio_codec_ctx->bits_per_coded_sample 	= 16;
		break;
	}
	// setup channel mapping
    switch (audio_codec_ctx->channels)
    {
        case 1:
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelCF;
        break;

        case 3:
            pcm.eChannelMapping[2] = OMX_AUDIO_ChannelCF;
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
        break;

        case 8:
            pcm.eChannelMapping[7] = OMX_AUDIO_ChannelRS;
        case 7:
            pcm.eChannelMapping[6] = OMX_AUDIO_ChannelLS;
        case 6:
            pcm.eChannelMapping[5] = OMX_AUDIO_ChannelRR;
        case 5:
            pcm.eChannelMapping[4] = OMX_AUDIO_ChannelLR;
        case 4:
            pcm.eChannelMapping[3] = OMX_AUDIO_ChannelLFE;
            pcm.eChannelMapping[2] = OMX_AUDIO_ChannelCF;
        case 2:
            pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
        break;
    }
    // set parameters for the audio renderer
    if ((omx_error = OMX_SetParameter (ILC_GET_HANDLE (audio_render), OMX_IndexParamAudioPcm, &pcm)) != OMX_ErrorNone)
    {
    	fprintf (stderr, "Error setting PCM parameters for audio renderer; error: 0x%08x\n", omx_error);
    	return 1;
    }
    // change audio renderer state to executing
    ilclient_enable_port_buffers    (audio_render, AUDIO_RENDER_INPUT_PORT, NULL, NULL, NULL);
    ilclient_change_component_state (audio_render, OMX_StateExecuting);

	return ret;
}


static void close_audio ()
{
	if ((omx_audio_buffer = ilclient_get_input_buffer (audio_render, AUDIO_RENDER_INPUT_PORT, 1)) != NULL)
	{
		omx_audio_buffer->nFilledLen = 0;
		omx_audio_buffer->nFlags 	 = OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_TIME_UNKNOWN;
		if (OMX_EmptyThisBuffer (ILC_GET_HANDLE (audio_render), omx_audio_buffer) != OMX_ErrorNone)
            fprintf ( stderr, "error emptying last audio buffer =/\n" );
	}
	else
        fprintf (stderr, "Could not send EOS flag to audio renderer\n");

	// wait for EOS from render
	ilclient_wait_for_event (audio_render, OMX_EventBufferFlag, AUDIO_RENDER_INPUT_PORT, 0, OMX_BUFFERFLAG_EOS, 0, ILCLIENT_BUFFER_FLAG_EOS, 10000);
	// need to flush the tunnel to allow audio_render to disable its input port
	ilclient_flush_tunnels        (audio_tunnel, 0);
	ilclient_disable_port_buffers (audio_render, AUDIO_RENDER_INPUT_PORT, NULL, NULL, NULL);
	ilclient_disable_tunnel       (audio_tunnel);
	ilclient_teardown_tunnels     (audio_tunnel);

	if (audio_codec_ctx)
        avcodec_close (audio_codec_ctx);
}


static int open_codec_context (int* stream_idx, enum AVMediaType type)
{
	int 			ret;
	AVStream* 	    stream;
	AVCodecContext* codec_ctx 	= NULL;
	AVCodec* 	    codec 		= NULL;

	ret = av_find_best_stream (fmt_ctx, type, -1, -1, NULL, 0);
	*stream_idx = ret;
	if (ret < 0)
	{
		fprintf (stderr, "Could not find %s stream in input file\n", type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
		return ret;
	}

	stream    = fmt_ctx->streams[*stream_idx];
	codec_ctx = stream->codec;
	codec     = avcodec_find_decoder (codec_ctx->codec_id);

	if (!codec)
	{
		fprintf (stderr, "Failed to find %s codec\n", type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
		return 1;
	}
	if ((ret = avcodec_open2 (codec_ctx, codec, NULL)) < 0)
	{
		fprintf (stderr, "Failed to open %s codec\n", type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
		return ret;
	}
	return 0;
}


static int create_hw_clock ()
{
	int ret = 0;
	// create clock
	if (ilclient_create_component (client, &video_clock, "clock", ILCLIENT_DISABLE_ALL_PORTS) != 0)
	{
		fprintf (stderr, "Error creating IL COMPONENT video clock\n");
		ret = -14;
	}
	if (video_clock == NULL)
		fprintf (stderr, "Error?\n");

	list[2] = video_clock;
	return ret;
}


static int setup_clock ()
{
	OMX_TIME_CONFIG_CLOCKSTATETYPE clock_state;
	int ret = 0;

	// set clock configuration
	memset (&clock_state, 0, sizeof (clock_state));
	clock_state.nSize             = sizeof (clock_state);
	clock_state.nVersion.nVersion = OMX_VERSION;
	clock_state.eState            = OMX_TIME_ClockStateWaitingForStartTime;
	clock_state.nWaitMask         = 0;

	if (video_stream_idx != AVERROR_STREAM_NOT_FOUND)
		clock_state.nWaitMask |= OMX_CLOCKPORT0;
	if (audio_stream_idx != AVERROR_STREAM_NOT_FOUND)
		clock_state.nWaitMask |= OMX_CLOCKPORT1;

	if (video_clock != NULL && OMX_SetParameter (ILC_GET_HANDLE (video_clock), OMX_IndexConfigTimeClockState, &clock_state) != OMX_ErrorNone)
	{
		fprintf (stderr, "Error settings parameters for video clock\n");
		ret = -13;
	}
	return ret;
}


static void cleanup ()
{
	destroy_packet_buffer (&video_packet_fifo);
	destroy_packet_buffer (&audio_packet_fifo);

	printf ("  closing streams\n");
	if (video_stream_idx != AVERROR_STREAM_NOT_FOUND)
	{
		close_video ();
		printf ("    video closed\n");
	}
	if (audio_stream_idx != AVERROR_STREAM_NOT_FOUND)
	{
		close_audio ();
		printf ("    audio closed\n");
	}

	printf ("  freeing ffmpeg structs\n");
	av_frame_free (&av_frame);
	avformat_close_input (&fmt_ctx);

	printf ("  cleaning up components\n");
	ilclient_state_transition   (list, OMX_StateIdle);
	ilclient_state_transition   (list, OMX_StateLoaded);
	ilclient_cleanup_components (list);

	flags = 0;
}


uint64_t rpi_mp_current_time ()
{
	OMX_TIME_CONFIG_TIMESTAMPTYPE timestamp;
	memset (&timestamp, 0x0, sizeof (timestamp));
	timestamp.nVersion.nVersion = OMX_VERSION;
	timestamp.nSize 			= sizeof (OMX_TIME_CONFIG_TIMESTAMPTYPE);
	timestamp.nPortIndex		= CLOCK_AUDIO_PORT;

	OMX_ERRORTYPE omx_error;
	if (( omx_error = OMX_GetParameter (ILC_GET_HANDLE (video_clock), OMX_IndexConfigTimeCurrentMediaTime, &timestamp)) != OMX_ErrorNone)
	{
		fprintf (stderr, "Could not get timestamp config from clock component. Error 0x%08x\n", omx_error);
		return 0;
	}
	uint64_t pts = (uint64_t) (timestamp.nTimestamp.nLowPart | (uint64_t) timestamp.nTimestamp.nHighPart << 32);
	return pts / AV_TIME_BASE;
}


// TODO implement correctly
int rpi_mp_seek (int64_t position)
{
	OMX_ERRORTYPE omx_error;
	// make sure we are paused first
	// if ( ~flags & PAUSED ) pause_playback ();
	lock();

	OMX_TIME_CONFIG_CLOCKSTATETYPE clock;
	memset ( & clock, 0x0, sizeof ( OMX_TIME_CONFIG_CLOCKSTATETYPE ) );
	clock.nVersion.nVersion = OMX_VERSION;
	clock.nSize 			= sizeof ( OMX_TIME_CONFIG_TIMESTAMPTYPE );
	clock.eState    		= OMX_TIME_ClockStateStopped;
	clock.nOffset			= pts__omx_timestamp (-1000LL * 200);
	// clock.nOffset     = ToOMXTime(-1000LL * OMX_PRE_ROLL);

	if (( omx_error = OMX_SetConfig ( ILC_GET_HANDLE ( video_clock ), OMX_IndexConfigTimeClockState, & clock )) != OMX_ErrorNone )
	{
		fprintf ( stderr, "Could not stop clock. Error 0x%08x\n", omx_error );
	 	return 0;
	}

	position *= AV_TIME_BASE;
	position += fmt_ctx->start_time;

	printf ( "trying to seek to position %llu\n", position );

	// clear fifo queues
	flush_buffer ( & video_packet_fifo );
	flush_buffer ( & audio_packet_fifo );

	// flush video buffer
	//*
	if ( ( omx_error = OMX_SendCommand ( ILC_GET_HANDLE ( video_decode ), OMX_CommandFlush, VIDEO_DECODE_INPUT_PORT, NULL ) != OMX_ErrorNone ) )
	{
		fprintf ( stderr, "Could not flush video decoder input (0x%08x)\n", omx_error );
		return 1;
	}
	if ( ( omx_error = OMX_SendCommand ( ILC_GET_HANDLE ( video_render ), OMX_CommandFlush, VIDEO_RENDER_INPUT_PORT, NULL ) != OMX_ErrorNone ) )
	{
		fprintf ( stderr, "Could not flush video render input (0x%08x)\n", omx_error );
		return 1;
	}
	ilclient_flush_tunnels ( video_tunnel, 0 );
	//*/

	// flush audio buffer
	//*
	if ( ( omx_error = OMX_SendCommand ( ILC_GET_HANDLE ( audio_render ), OMX_CommandFlush, AUDIO_RENDER_INPUT_PORT, NULL ) != OMX_ErrorNone ) )
	{
		fprintf ( stderr, "Could not flush video decoder input (0x%08x)\n", omx_error );
		return 1;
	}
	ilclient_flush_tunnels ( audio_tunnel, 0 );
	//*/

	// avcodec_flush_buffers ( video_codec_ctx );
	// avcodec_flush_buffers ( audio_codec_ctx );

	// seek to frame
	int ret = av_seek_frame ( fmt_ctx, -1, position, AVSEEK_FLAG_ANY );

	double t = (double) position * audio_stream->r_frame_rate.num / audio_stream->r_frame_rate.den;

	// reset hardware clock
	OMX_TIME_CONFIG_TIMESTAMPTYPE timestamp;
	memset ( & timestamp, 0x0, sizeof ( timestamp ) );
	timestamp.nVersion.nVersion 	= OMX_VERSION;
	timestamp.nSize 				= sizeof ( OMX_TIME_CONFIG_TIMESTAMPTYPE );
	timestamp.nPortIndex			= CLOCK_AUDIO_PORT;
	timestamp.nTimestamp 			= pts__omx_timestamp ( t );
	// timestamp.nTimestamp.nLowPart 	= 0;
	// timestamp.nTimestamp.nHighPart 	= 0;

	if (( omx_error = OMX_SetConfig ( ILC_GET_HANDLE ( video_clock ), OMX_IndexConfigTimeCurrentAudioReference, & timestamp )) != OMX_ErrorNone )
	{
		fprintf ( stderr, "Could not set timestamp for clock component. Error 0x%08x\n", omx_error );
	 	return 0;
	}
	// SET_FLAG ( FIRST_AUDIO );
	// SET_FLAG ( FIRST_VIDEO );


	// if (( omx_error = OMX_SetParameter ( ILC_GET_HANDLE ( video_clock ), OMX_IndexConfigTimeCurrentVideoReference, & timestamp )) != OMX_ErrorNone )
	// {
	// 	fprintf ( stderr, "Could not set timestamp for clock component. Error 0x%08x\n", omx_error );
	//  	return 0;
	// }

	// resume playback
	// pause_playback ();
	unlock();
	if (ret < 0)
		fprintf (stderr, "could not seek to position: %llu\n (%d)", position, AVERROR (ret));
	return ret;
}


int rpi_mp_init ()
{
	av_register_all ();
	avformat_network_init ();
	// init IL client lib
	if ((client = ilclient_init ()) == NULL)
	{
		fprintf (stderr, "Could not init ilclient\n");
		return -1;
	}
	// init OMX
	if (OMX_Init () != OMX_ErrorNone)
	{
		fprintf (stderr, "Could not init OMX, aborting\n");
		ilclient_destroy (client);
		return 1;
	}
	memset (list, 0, sizeof (list));
	return 0;
}


void rpi_mp_deinit ()
{
	OMX_Deinit();
	ilclient_destroy (client);
}


int rpi_mp_open (const char* source, int* image_width, int* image_height, int64_t* duration, int init_flags)
{
	int ret = 0;
	flags = FIRST_VIDEO |
			FIRST_AUDIO |
			(init_flags & RENDER_VIDEO_TO_TEXTURE ? RENDER_2_TEXTURE : 0) |
			(init_flags & ANALOG_AUDIO ? ANALOG_AUDIO_OUT : 0);

	// egl callback in case we are rendering to texture
	if (flags & RENDER_2_TEXTURE)
		ilclient_set_fill_buffer_done_callback (client, fill_egl_texture_buffer, 0);

    // open source
	if (avformat_open_input (&fmt_ctx, source, NULL, NULL) < 0)
	{
		fprintf (stderr, "Could not open source %s\n", source);
		return 1;
	}
    // search for streams
	if (avformat_find_stream_info (fmt_ctx, NULL) < 0)
	{
		fprintf (stderr, "Could not find stream information\n");
		return 1;
	}
	// create clock
	if (create_hw_clock() == 0)
	{
		// open video
		if (open_codec_context (&video_stream_idx, AVMEDIA_TYPE_VIDEO) == 0)
		{
			video_stream    = fmt_ctx->streams[video_stream_idx];
			video_codec_ctx = video_stream->codec;
			if (open_video() == 0)
			{
				*image_width  = video_codec_ctx->width;
				*image_height = video_codec_ctx->height;
			}
		}
		// open audio
		if (open_codec_context (&audio_stream_idx, AVMEDIA_TYPE_AUDIO) == 0)
		{
			audio_stream    = fmt_ctx->streams[audio_stream_idx];
			audio_codec_ctx = audio_stream->codec;
			open_audio ();
		}
		// check that we did get streams
		if (video_stream_idx == AVERROR_STREAM_NOT_FOUND && audio_stream_idx == AVERROR_STREAM_NOT_FOUND)
		{
			fprintf (stderr, "Could not find either audio or video in input, aborting\n");
			ret = 1;
			goto end;
		}

		*duration = fmt_ctx->duration / AV_TIME_BASE;

		if (setup_clock() != 0)
		{
			fprintf (stderr, "Could not setup HW clock\n");
			ret = 1;
			goto end;
		}
	}
	else
	{
		fprintf (stderr, "Could not create clock. exiting\n");
		return 1;
	}
	// dump input format
	av_dump_format (fmt_ctx, 0, source, 0);
	// allocate frame for decoding (audio here)
	if (!(av_frame = av_frame_alloc()))
	{
		fprintf (stderr, "Could not allocate frame\n");
		ret = AVERROR (ENOMEM);
		goto end;
	}
	// initialize packet
	av_init_packet (&av_packet);
	av_packet.data = NULL;
	av_packet.size = 0;
	// init buffers
	init_packet_buffer (&video_packet_fifo, 1024 * 1024 * 5);
	init_packet_buffer (&audio_packet_fifo, 1024 * 1024 * 5);
end:
	return ret;
}


void rpi_mp_setup_render_buffer (void* _egl_image, pthread_mutex_t** draw_mutex, pthread_cond_t** draw_cond)
{
	egl_image   = _egl_image;
	*draw_mutex = &buffer_filled_mut;
	*draw_cond  = &buffer_filled_cond;
}


int rpi_mp_start ()
{
	// start threads
	pthread_t video_decoding, audio_decoding;
	pthread_create (&video_decoding, NULL, (void*) &video_decoding_thread, NULL);
	pthread_create (&audio_decoding, NULL, (void*) &audio_decoding_thread, NULL);

	// start clock
	ilclient_change_component_state (video_clock, OMX_StateExecuting);

	// read packets from source
	while (~flags & STOPPED && (av_read_frame (fmt_ctx, &av_packet) >= 0))
	{
		if (process_packet() != 0)
			break;
	}
	SET_FLAG (DONE_READING);
	printf ("done reading\n");

	// wait for all threads to end
	pthread_join (video_decoding, NULL);
	pthread_join (audio_decoding, NULL);
	SET_FLAG (STOPPED);

	// cleanup
	printf ("cleaning up... \n");
	cleanup ();
	printf ("stopping reading thread\n");
	return 0;
}


void rpi_mp_stop ()
{
	SET_FLAG (STOPPED);
	// make sure to unpause otherwise threads won't exit
	if (flags & PAUSED)
        rpi_mp_pause();
	// flush video component
	if (video_stream_idx != AVERROR_STREAM_NOT_FOUND)
	{
		OMX_ERRORTYPE omx_error;
		if ((omx_error = OMX_SendCommand (ILC_GET_HANDLE (video_decode), OMX_CommandFlush, 130, NULL) != OMX_ErrorNone))
			fprintf (stderr, "Could not flush video decoder input (0x%08x)\n", omx_error);
	}
}


void rpi_mp_pause ()
{
	OMX_TIME_CONFIG_SCALETYPE scale;
	memset (&scale, 0x0, sizeof (OMX_TIME_CONFIG_SCALETYPE));
	scale.nSize 			= sizeof (OMX_TIME_CONFIG_SCALETYPE);
	scale.nVersion.nVersion = OMX_VERSION;
	scale.xScale 			= (flags & PAUSED ? 1 << 16 : 0);

	OMX_ERRORTYPE omx_error;
	if ((omx_error = OMX_SetParameter (ILC_GET_HANDLE (video_clock), OMX_IndexConfigTimeScale, &scale)) != OMX_ErrorNone)
	{
		fprintf (stderr, "Could not set scale parameter on video clock. Error 0x%08x\n", omx_error);
		return;
	}
	if (~flags & PAUSED)
	{
		SET_FLAG (PAUSED);
		/*
		//printf ( "waiting for threads to pause...\n" );
		// make sure threads are paused
		pthread_mutex_lock ( & video_mutex );
		while ( ~flags & VIDEO_PAUSED )
		{
			//printf ( "  waiting for video thread to set flag\n" );
			pthread_cond_wait ( & video_cond, & video_mutex );
			//printf ( "  signal recieved\n" );
		}
		pthread_mutex_unlock ( & video_mutex );
		//printf ( "  video is paused\n" );

		pthread_mutex_lock ( & audio_mutex );
		while ( ~flags & AUDIO_PAUSED )
			pthread_cond_wait ( & audio_cond, & audio_mutex );
		pthread_mutex_unlock ( & audio_mutex );
		//printf ( "  audio is paused\n" );
		//*/
	}
	else
	{
		UNSET_FLAG (PAUSED);
		pthread_cond_broadcast (&pause_condition);
	}
}

int rpi_mp_metadata (const char* key, char** title)
{
	AVDictionaryEntry* entry = NULL;
	entry = av_dict_get (fmt_ctx->metadata, key, 0, AV_DICT_IGNORE_SUFFIX);
	if (!entry)
		return 1;
	*title = entry->value;
	return 0;
}
