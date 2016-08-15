/** ----------------------------------------------------------------------------------
 * File: video.h
 * Description: Public interface to mediaplayer.
 * ----------------------------------------------------------------------------------- */
#include <stdint.h>
#include <pthread.h>

/*  FLAGS */
enum _flags
{
	RENDER_VIDEO_TO_TEXTURE = 0x1,
	ANALOG_AUDIO 			= 0x2,
}
rpi_mp_open_flags;

/**
 *	Initialize the mediaplayer.
 * 	This function is required to be called before any operations on the media player
 *	Returns 0 on success, else non-zero for error
 */
int	rpi_mp_init () ;

/**
 *	Deinitialize the mediaplayer.
 * 	Called to turn off mediaplayer functionality. The initialize_mediaplayer function must be called again if any
 * 	operations on the player.
 */
void rpi_mp_deinit () ;

/**
 * 	Opens the mediaplayer with the set init flags. This needs to be called before starting playback.
 * 	Will set width, height and duration parameters for the media so they can be used before any playback is done.
 *	Returns 0 on success, else non-zero on error.
 */
int rpi_mp_open (const char* /* file */, int* /* width */, int* /* height */, int64_t* /* duration */, int /* flags */) ;

/**
 *  If rendering to a texture this function needs to be called to setup.
 *  Input parameters are a pointer to the EGL Render Buffer and pointers that are set
 *  to a mutex and condition for when texture is ready to be rendered to screen.
 */
void rpi_mp_setup_render_buffer (void*             /* egl_image */,
                                 pthread_mutex_t** /* draw_mutex */,
                                 pthread_cond_t**  /* draw_condition */) ;

/**
 *	Starts media playback. Takes a pointer to an EGLImage object for rendering to a texture.
 *	If the media was opened without the RENDER_VIDEO_TO_TEXTURE flag this parameter is ignored and can be set to NULL.
 *	Returns 0 on successfully playing the media, non-zero if there was an error during playback.
 */
int rpi_mp_start () ;

/**
 *	Stops the current playback.
 */
void rpi_mp_stop () ;

/**
 *	Pauses playback in play state, otherwise resumes a previously paused stream.
 */
void rpi_mp_pause () ;

/**
 *	Returns the current time in seconds for playback.
 */
uint64_t rpi_mp_current_time () ;

/**
 *	Seeks to the specified position (in seconds) in the media.
 */
int	rpi_mp_seek (int64_t /* position */) ;

/**
 *  Get title of stream.
 *  Returns non-zero if there is none.
 */
int rpi_mp_metadata (const char* /* key */, char** /* title */) ;
