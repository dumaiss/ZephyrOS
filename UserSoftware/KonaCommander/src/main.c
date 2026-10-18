#include "kos.h"

#include "kc_remote.h"
#include "kc_serial.h"
#include "kc_ui.h"

int main(int argc, char **argv)
{
	int result;

	(void)argc;
	(void)argv;
	result = kc_link_open();
	if (result < 0) {
		printf("konacmd: %s\r\n", kc_remote_error(result));
		return 1;
	}
	result = kc_ui_run();
	kc_link_close();
	return result;
}
