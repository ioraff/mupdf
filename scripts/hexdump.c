/* hexdump.c -- an "xxd -i" workalike for dumping binary files as source code */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int zero, string;

static int
hexdump(FILE *fo, FILE *fi)
{
	int c, n;

	if (string)
		fprintf(fo, "\"");

	n = 0;
	c = fgetc(fi);
	while (c != -1)
	{
		n += fprintf(fo, string ? "\\x%02x" : "%d,", c);
		if (n > 72) {
			fprintf(fo, string ? "\"\n\"" : "\n");
			n = 0;
		}
		c = fgetc(fi);
	}

	if (string)
		fprintf(fo, "\"\n");

	return n;
}

static int
usage(void)
{
	fprintf(stderr, "usage: hexdump [-0] [-s] [-p prefix] output.c input.dat\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	FILE *fo;
	FILE *fi;
	char filenamebuf[256], *filename = filenamebuf;
	char *basename;
	char *p;
	char *prefix = "";
	int c, i, size;

	if (argc < 3)
		usage();

	while ((c = getopt(argc, argv, "0sp:")) != -1)
	{
		switch (c)
		{
		case '0':
			zero = 1;
			break;
		case 's':
			string = 1;
			break;
		case 'p':
			prefix = optarg;
			break;
		case '?':
			usage();
		}
	}

	fo = fopen(argv[optind], "wb");
	if (!fo)
	{
		fprintf(stderr, "hexdump: could not open output file '%s'\n", argv[optind]);
		return 1;
	}

	for (i = optind+1; i < argc; i++)
	{
		fi = fopen(argv[i], "rb");
		if (!fi)
		{
			fclose(fo);
			fprintf(stderr, "hexdump: could not open input file '%s'\n", argv[i]);
			return 1;
		}

		basename = strrchr(argv[i], '/');
		if (!basename)
			basename = strrchr(argv[i], '\\');
		if (basename)
			basename++;
		else
			basename = argv[i];

		if (strlen(basename) >= sizeof(filenamebuf))
		{
			fclose(fi);
			fclose(fo);
			fprintf(stderr, "hexdump: filename '%s' too long\n", basename);
			return 1;
		}

		strcpy(filename, argv[i]);
		while (*prefix && *prefix == *filename)
		{
			++prefix;
			++filename;
		}
		for (p = filename; *p; ++p)
		{
			if (*p == '/' || *p == '.' || *p == '\\' || *p == '-')
				*p = '_';
		}

		fseek(fi, 0, SEEK_END);
		size = ftell(fi);
		fseek(fi, 0, SEEK_SET);

		fprintf(fo, "const int fz_%s_size = %d;\n", filename, size + zero);
		fprintf(fo, "const unsigned char fz_%s[] =", filename);
		fprintf(fo, string ? "\n" : " {\n");
		hexdump(fo, fi);
		if (!zero)
		{
			fprintf(fo, string ? ";\n" : "};\n");
		}
		else
		{
			/* zero-terminate so we can hexdump text files into C strings */
			fprintf(fo, string ? ";\n" : "0};\n");
		}

		fclose(fi);
	}

	if (fclose(fo))
	{
		fprintf(stderr, "hexdump: could not close output file '%s'\n", argv[1]);
		return 1;
	}

	return 0;
}
