#include <libavformat/avformat.h>
#include <pthread.h>

enum FIFO_STATUS
{
	EMPTY_BUFFER,
	FULL_BUFFER
};

/**
 *	Represents a FIFO of AVPackets
 */
typedef struct
{
	uint 	 		size;
	uint 			capacity;
	uint 	 		n_packets;
	uint 			size_packets;
	AVPacket      * packets;
	AVPacket      * _front;
	AVPacket      * _back;
	pthread_mutex_t mutex;
} packet_buffer ;


/**
 *	Initialize the FIFO buffer.
 *  Allocates necessary buffers. Don't forget to call destroy_packet_buffer!
 *
 *  @param packet_buffer
 *		pointer to a struct to perform initialization on
 *	@param size
 *		maximum size of the fifo in bytes.
 *	@return int ret
 *		0 on success, or non-zero on failure
 */
int init_packet_buffer ( packet_buffer * buffer, uint size ) ;

/**
 *	Destroys a FIFO buffer.
 *	Performs necessary deallocation of buffers.
 *
 *	@param packet_buffer * buffer
 *		pointer to fifo that will be destroyed
 */
void destroy_packet_buffer ( packet_buffer * buffer ) ;

/**
 *	Pushes AVPacket into the FIFO buffer.
 *	If the FIFO has already reached maximum size, or it will go over by pushing the packet,
 *	an error is returned.
 *
 *	@param packet_buffer * buffer
 *		pointer to fifo queue
 *	@param AVPacket p
 *	@return int ret
 *		0 on success, non-zero on failure.
 */
int push_packet ( packet_buffer * buffer, AVPacket   p ) ;

/**
 *	Pops the first packet from the fifo queue.
 * 	Returns error on empty buffer.
 *
 *	@param packet_buffer * buffer
 *		pointer to buffer from which to perform pop
 *	@param AVPacket * p
 *		pointer that will be set to the poped packet
 *	@return int ret
 *		zero on success, non-zero on failure
 */
int pop_packet ( packet_buffer * buffer, AVPacket * p ) ;

/**
 *	Pops any packets that are left in the buffer and thereby reseting it
 */
void flush_buffer ( packet_buffer * buffer ) ;
