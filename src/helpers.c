#include <math.h>
#include <string.h>
#include "rpi_mp_utils.h"

void flt_to_s16 (uint8_t *flt, uint8_t **s16, int size)
{
	int 		i = 0;
			 *s16 = (uint8_t *) malloc (size / 2);
	int16_t    *p = (int16_t *) *s16;
	float     *fp = (float   *) flt;

	for (; i < size / 4; i ++)
		p[i] = (int16_t) floor (fp[i] * 0x7FFF);
}
