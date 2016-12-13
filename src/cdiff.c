#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "nifty.h"


static int
cdiff(FILE *fp)
{
	char *line = NULL;
	size_t llen = 0UL;
	char *prev = NULL;
	size_t plen = 0UL;

	for (ssize_t nrd; (nrd = getline(&line, &llen, stdin)) > 0;) {
		size_t i;

		if (LIKELY(line[nrd - 1U] == '\n')) {
			nrd--;
		}
		for (i = 0U; i < plen && i < nrd && prev[i] == line[i]; i++) {
			putchar(' ');
		}
		if (LIKELY(i < nrd)) {
			fwrite(line + i, 1, nrd - i, stdout);
		}
		putchar('\n');

		/* save line for next time */
		if (UNLIKELY(plen < nrd)) {
			prev = realloc(prev, plen = nrd);
		}
		memcpy(prev, line, nrd);
	}
	free(line);
	return 0;
}


#include "cdiff.yucc"

int
main(int argc, char *argv[])
{
	yuck_t argi[1U];
	int rc;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	rc = cdiff(stdin) < 0;

out:
	yuck_free(argi);
	return rc;
}

/* cdiff.c ends here */
