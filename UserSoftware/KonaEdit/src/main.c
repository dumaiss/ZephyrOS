#include "kos.h"

#include "ke_ui.h"

int main(int argc, char **argv)
{
	const char *path = (const char *)0;

	/* KonaOS argv contains command-tail words only; there is no argv[0]
	 * program-name convention. */
	if (argc > 0) path = argv[0];
	return ke_ui_run(path);
}
