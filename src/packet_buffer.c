#include "rpi_mp_packet_buffer.h"

#define FIFO_ALLOC_SIZE 1000


int init_packet_buffer (packet_buffer* buffer, uint size)
{
	buffer->n_packets 	= 0;
	buffer->size  		= size;
	buffer->capacity	= FIFO_ALLOC_SIZE;
	buffer->packets 	= (AVPacket*) malloc (FIFO_ALLOC_SIZE * sizeof (AVPacket));
	pthread_mutex_init (&buffer->mutex, NULL);

	// error
	if (!buffer->packets)
		return 1;

	memset (buffer->packets, 0x0, FIFO_ALLOC_SIZE * sizeof (AVPacket));
	buffer->_front = buffer->_back = buffer->packets;
	return 0;
}


void destroy_packet_buffer (packet_buffer* buffer)
{
	flush_buffer (buffer);
	free (buffer->packets);
	buffer->n_packets = 0;
	buffer->size 	  = 0;
	pthread_mutex_destroy (&buffer->mutex);
}


int push_packet (packet_buffer* buffer, AVPacket p)
{
	pthread_mutex_lock (&buffer->mutex);
	int ret = 0;
	// check if size would be too large
	if (buffer->size_packets + p.size > buffer->size)
	{
		ret = FULL_BUFFER;
		goto end;
	}
	// we might need to increment the size of the allocated buffer
	if (buffer->n_packets == buffer->capacity - 1)
	{
		// allocate new larger buffer
		AVPacket* tmp = (AVPacket*) malloc (sizeof (AVPacket) * (buffer->capacity + FIFO_ALLOC_SIZE));
		memset (tmp, 0x0, sizeof (AVPacket) * (buffer->capacity + FIFO_ALLOC_SIZE));
		// copy packets
		if (buffer->_front < buffer->_back)
			memcpy (tmp, buffer->_front, sizeof (AVPacket) * (buffer->n_packets - 1));
		else
		{
			int n = buffer->capacity - (buffer->_front - buffer->packets);
			memcpy (tmp,     buffer->_front,  sizeof (AVPacket) * n);
			memcpy (tmp + n, buffer->packets, sizeof (AVPacket) * (buffer->n_packets - n));
		}
		// free old buffer and set pointers
		free (buffer->packets);
		buffer->capacity += FIFO_ALLOC_SIZE;
		buffer->packets = tmp;
		buffer->_front = buffer->packets;
		buffer->_back  = buffer->packets + buffer->n_packets;
	}
	*buffer->_back = p;
	buffer->n_packets ++;
	buffer->_back ++;
	buffer->size_packets += p.size;
	// loop the back pointer back to the beginning of the buffer
	if (buffer->_back - buffer->packets == buffer->capacity)
		buffer->_back = buffer->packets;
end:
	pthread_mutex_unlock (&buffer->mutex);
	return ret;
}


int pop_packet (packet_buffer* buffer, AVPacket* p)
{
	pthread_mutex_lock (&buffer->mutex);
	int ret = 0;
	// empty buffer
	if (buffer->n_packets == 0)
	{
		ret = EMPTY_BUFFER;
		goto end;
	}
	*p = *buffer->_front;

	buffer->_front ++;
	buffer->n_packets --;
	buffer->size_packets -= p->size;
	// loop front pointer back to beginning of the buffer
	if (buffer->_front - buffer->packets == buffer->capacity)
		buffer->_front = buffer->packets;
end:
	pthread_mutex_unlock (&buffer->mutex);
	return ret;
}


void flush_buffer (packet_buffer* buffer)
{
	pthread_mutex_lock (&buffer->mutex);
	while (buffer->_front != buffer->_back)
	{
		av_packet_unref (buffer->_front);
		buffer->_front ++;
		if (buffer->_front - buffer->packets == buffer->capacity)
			buffer->_front = buffer->packets;
	}
	// reset
	buffer->size_packets = 0;
	buffer->n_packets    = 0;
	buffer->_front = buffer->_back = buffer->packets;
	pthread_mutex_unlock (&buffer->mutex);
}
