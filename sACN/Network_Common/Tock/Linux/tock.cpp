/* tock.cpp

   Linux implementation of the platform-specific part of the tock library.

   The OSX build scales mach_absolute_time() ticks into milliseconds. Linux has
   a millisecond-capable monotonic clock directly, so this is just a read of
   CLOCK_MONOTONIC. A tock only ever has its difference taken and is documented
   to wrap, so truncating to uint4 is intentional.
*/

#include <time.h>

#include "deftypes.h"
#include "../tock.h"

static int tock_resolution = 1;

//Initializes the tock layer.  Only needs to be called once per application
bool Tock_StartLib()
{
	//CLOCK_MONOTONIC is nanosecond-resolution on any Linux we care about, but ask
	//rather than assume, and round any sub-millisecond resolution up to 1ms.
	struct timespec res;
	if(0 == clock_getres(CLOCK_MONOTONIC, &res))
	{
		long ms = (res.tv_sec * 1000L) + (res.tv_nsec / 1000000L);
		tock_resolution = (ms < 1) ? 1 : static_cast<int>(ms);
	}
	else
		tock_resolution = 1;

	return tock_resolution <= 10;
}

//Gets a tock representing the current time
tock Tock_GetTock()
{
	struct timespec ts;
	if(0 != clock_gettime(CLOCK_MONOTONIC, &ts))
		return tock(static_cast<uint4>(0));

	//Accumulate in 64 bits so the seconds term cannot overflow before we truncate.
	uint8 ms = (static_cast<uint8>(ts.tv_sec) * 1000ULL) + (static_cast<uint8>(ts.tv_nsec) / 1000000ULL);
	return tock(static_cast<uint4>(ms));
}

//Shuts down the tock layer.
void Tock_StopLib()
{
}

//Returns the number of ms between tocks
int Tock_GetRes()
{
	return tock_resolution;
}
