/* The placement probe: main runs in the home zone and checks one
 * crossing per line; `placetest PASS` when every one held. */

#include "kos.h"
#include "kos/event.h"
#include "kos/timer.h"
#include "place.h"

static uint8_t failed;

static void check(const char *name, uint32_t got, uint32_t want)
{
	console_puts("place: ");
	console_puts(name);
	if (got == want) {
		console_puts(" ok\r\n");
	} else {
		console_puts(" FAIL\r\n");
		failed = 1u;
	}
}

uint16_t home_square(uint16_t x)
{
	return (uint16_t)(x * x);
}

int main(int argc, char **argv)
{
	int timer;
	(void)argc; (void)argv;
	check("home to far1", f1_add(1200u, 34u), 1234u);
	check("home to far1, stack arguments",
	      f1_mix(0x12u, 0x3456u, 0x00010000ul, 0x78u, 0x9Au),
	      0x12000000ul + 0x00010000ul + 0x3456u + 0x78u + 0x9A00u);
	check("home to far2, 8 bits", f2_twice(21u), 42u);
	check("far1 to far2 and home", f1_nested(7u), 14u + 49u);
	check("far2 through a resident pointer", f2_apply(res_inc, 40u), 42u);
	idle();
	timer = timer_open();
	if (timer < 0 || notify(timer, res_on_tick) < 0 ||
	    timer_event(timer, 782u) < 0) {
		check("timer", 0u, 1u);
	} else {
		check("handler while far1 runs", f1_spin(3u) >= 3u, 1u);
		(void)timer_event(timer, 0u);
		(void)notify(timer, (kos_event_handler_t)0);
		(void)unbind((unsigned char)timer);
	}
	console_puts(failed ? "placetest FAIL\r\n" : "placetest PASS\r\n");
	return failed;
}
