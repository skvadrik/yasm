/*
 * Program entry point, command line parsing
 *
 *  Copyright (C) 2001  Peter Johnson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <util.h>
/*@unused@*/ RCSID("$IdPath$");

#include <libyasm/compat-queue.h>
#include <libyasm/bitvect.h>
#include <libyasm.h>

#ifndef WIN32
#include "ltdl.h"
#endif
#include "yasm-module.h"
#include "yasm-options.h"


/* Extra path to search for our modules. */
#ifndef YASM_MODULE_PATH_ENV
# define YASM_MODULE_PATH_ENV	"YASM_MODULE_PATH"
#endif

#ifndef WIN32
extern const lt_dlsymlist lt_preloaded_symbols[];
#endif

/* Preprocess-only buffer size */
#define PREPROC_BUF_SIZE    16384

/* Check the module version */
#define check_module_version(d, TYPE, desc)	\
do { \
    if (d && d->version != YASM_##TYPE##_VERSION) { \
	print_error( \
	    _("%s: module version mismatch: %s `%s' (need %d, module %d)"), \
	    _("FATAL"), _(desc), d->keyword, YASM_##TYPE##_VERSION, \
	    d->version); \
	exit(EXIT_FAILURE); \
    } \
} while (0)

/*@null@*/ /*@only@*/ static char *obj_filename = NULL, *in_filename = NULL;
/*@null@*/ /*@only@*/ static char *machine_name = NULL;
static int special_options = 0;
/*@null@*/ /*@dependent@*/ static yasm_arch *cur_arch = NULL;
/*@null@*/ /*@dependent@*/ static yasm_parser *cur_parser = NULL;
/*@null@*/ /*@dependent@*/ static yasm_preproc *cur_preproc = NULL;
/*@null@*/ /*@dependent@*/ static yasm_objfmt *cur_objfmt = NULL;
/*@null@*/ /*@dependent@*/ static yasm_optimizer *cur_optimizer = NULL;
/*@null@*/ /*@dependent@*/ static yasm_dbgfmt *cur_dbgfmt = NULL;
static int preproc_only = 0;
static int warning_error = 0;	/* warnings being treated as errors */

/*@null@*/ /*@dependent@*/ static FILE *open_obj(const char *mode);
static void cleanup(/*@null@*/ yasm_sectionhead *sections);

/* Forward declarations: cmd line parser handlers */
static int opt_special_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_arch_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_parser_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_preproc_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_objfmt_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_dbgfmt_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_objfile_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_machine_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_warning_handler(char *cmd, /*@null@*/ char *param, int extra);
static int preproc_only_handler(char *cmd, /*@null@*/ char *param, int extra);
static int opt_preproc_include_path(char *cmd, /*@null@*/ char *param,
				    int extra);
static int opt_preproc_include_file(char *cmd, /*@null@*/ char *param,
				    int extra);

static /*@only@*/ char *replace_extension(const char *orig, /*@null@*/
					  const char *ext, const char *def);
static void print_error(const char *fmt, ...);

static /*@exits@*/ void handle_yasm_int_error(const char *file,
					      unsigned int line,
					      const char *message);
static /*@exits@*/ void handle_yasm_fatal(const char *message, ...);
static const char *handle_yasm_gettext(const char *msgid);
static void print_yasm_error(const char *filename, unsigned long line,
			     const char *msg);
static void print_yasm_warning(const char *filename, unsigned long line,
			       const char *msg);

static void apply_preproc_saved_options(void);
static void print_list_keyword_desc(const char *name, const char *keyword);

/* values for special_options */
#define SPECIAL_SHOW_HELP 0x01
#define SPECIAL_SHOW_VERSION 0x02
#define SPECIAL_LISTED 0x04

/* command line options */
static opt_option options[] =
{
    { 0, "version", 0, opt_special_handler, SPECIAL_SHOW_VERSION,
      N_("show version text"), NULL },
    { 'h', "help", 0, opt_special_handler, SPECIAL_SHOW_HELP,
      N_("show help text"), NULL },
    { 'a', "arch", 1, opt_arch_handler, 0,
      N_("select architecture (list with -a help)"), N_("arch") },
    { 'p', "parser", 1, opt_parser_handler, 0,
      N_("select parser (list with -p help)"), N_("parser") },
    { 'r', "preproc", 1, opt_preproc_handler, 0,
      N_("select preprocessor (list with -r help)"), N_("preproc") },
    { 'f', "oformat", 1, opt_objfmt_handler, 0,
      N_("select object format (list with -f help)"), N_("format") },
    { 'g', "dformat", 1, opt_dbgfmt_handler, 0,
      N_("select debugging format (list with -g help)"), N_("debug") },
    { 'o', "objfile", 1, opt_objfile_handler, 0,
      N_("name of object-file output"), N_("filename") },
    { 'm', "machine", 1, opt_machine_handler, 0,
      N_("select machine (list with -m help)"), N_("machine") },
    { 'w', NULL, 0, opt_warning_handler, 1,
      N_("inhibits warning messages"), NULL },
    { 'W', NULL, 0, opt_warning_handler, 0,
      N_("enables/disables warning"), NULL },
    { 'e', "preproc-only", 0, preproc_only_handler, 0,
      N_("preprocess only (writes output to stdout by default)"), NULL },
    { 'I', NULL, 1, opt_preproc_include_path, 0,
      N_("add include path"), N_("path") },
    { 'P', NULL, 1, opt_preproc_include_file, 0,
      N_("pre-include file"), N_("filename") },
};

/* version message */
/*@observer@*/ static const char *version_msg[] = {
    PACKAGE " " VERSION "\n",
    N_("Copyright (c) 2001-2003 Peter Johnson and other"), " " PACKAGE " ",
    N_("developers.\n"),
    N_("**Licensing summary**\n"),
    N_("Note: This summary does not provide legal advice nor is it the\n"),
    N_(" actual license.  See the individual licenses for complete\n"),
    N_(" details.  Consult a laywer for legal advice.\n"),
    N_("The primary license is the 2-clause BSD license.  Please use this\n"),
    N_(" license if you plan on submitting code to the project.\n"),
    N_("Libyasm:\n"),
    N_(" Libyasm is 2-clause or 3-clause BSD licensed, with the exception\n"),
    N_(" of bitvect, which is triple-licensed under the Artistic license,\n"),
    N_(" GPL, and LGPL.  Libyasm is thus GPL and LGPL compatible.  In\n"),
    N_(" addition, this also means that libyasm is free for binary-only\n"),
    N_(" distribution as long as the terms of the 3-clause BSD license and\n"),
    N_(" Artistic license (as it applies to bitvect) are fulfilled.\n"),
    N_("Modules:\n"),
    N_(" Most of the modules are 2-clause BSD licensed, except:\n"),
    N_("  preprocs/nasm - LGPL licensed\n"),
    N_("Frontends:\n"),
    N_(" The frontends are 2-clause BSD licensed.\n"),
    N_("License Texts:\n"),
    N_(" The full text of all licenses are provided in separate files in\n"),
    N_(" this program's source distribution.  Each file may include the\n"),
    N_(" entire license (in the case of the BSD and Artistic licenses), or\n"),
    N_(" may reference the GPL or LGPL license file.\n"),
    N_("This program has absolutely no warranty; not even for\n"),
    N_("merchantibility or fitness for a particular purpose.\n"),
    N_("Compiled on"), " " __DATE__ ".\n",
};

/* help messages */
/*@observer@*/ static const char help_head[] = N_(
    "usage: yasm [option]* file\n"
    "Options:\n");
/*@observer@*/ static const char help_tail[] = N_(
    "\n"
    "Files are asm sources to be assembled.\n"
    "\n"
    "Sample invocation:\n"
    "   yasm -f elf -o object.o source.asm\n"
    "\n"
    "Report bugs to bug-yasm@tortall.net\n");

/* parsed command line storage until appropriate modules have been loaded */
typedef STAILQ_HEAD(constcharparam_head, constcharparam) constcharparam_head;

typedef struct constcharparam {
    STAILQ_ENTRY(constcharparam) link;
    const char *param;
} constcharparam;

static constcharparam_head includepaths;
static constcharparam_head includefiles;

/* main function */
/*@-globstate -unrecog@*/
int
main(int argc, char *argv[])
{
    /*@null@*/ FILE *in = NULL, *obj = NULL;
    yasm_sectionhead *sections;
    size_t i;
#ifndef WIN32
    int errors;
#endif

#if defined(HAVE_SETLOCALE) && defined(HAVE_LC_MESSAGES)
    setlocale(LC_MESSAGES, "");
#endif
#if defined(LOCALEDIR)
    bindtextdomain(PACKAGE, LOCALEDIR);
#endif
    textdomain(PACKAGE);

    /* Initialize errwarn handling */
    yasm_internal_error_ = handle_yasm_int_error;
    yasm_fatal = handle_yasm_fatal;
    yasm_gettext_hook = handle_yasm_gettext;
    yasm_errwarn_initialize();

#ifndef WIN32
    /* Set libltdl malloc/free functions. */
#ifdef WITH_DMALLOC
    lt_dlmalloc = malloc;
    lt_dlfree = free;
#else
    lt_dlmalloc = yasm_xmalloc;
    lt_dlfree = yasm_xfree;
#endif

    /* Initialize preloaded symbol lookup table. */
    lt_dlpreload_default(lt_preloaded_symbols);

    /* Initialize libltdl. */
    errors = lt_dlinit();

    /* Set up extra module search directories. */
    if (errors == 0) {
	const char *path = getenv(YASM_MODULE_PATH_ENV);
	if (path)
	    errors = lt_dladdsearchdir(path);
#if defined(YASM_MODULEDIR)
	if (errors == 0)
	    errors = lt_dladdsearchdir(YASM_MODULEDIR);
#endif
    }
    if (errors != 0) {
	print_error(_("%s: module loader initialization failed"), _("FATAL"));
	return EXIT_FAILURE;
    }
#endif

    /* Initialize parameter storage */
    STAILQ_INIT(&includepaths);
    STAILQ_INIT(&includefiles);

    if (parse_cmdline(argc, argv, options, NELEMS(options), print_error))
	return EXIT_FAILURE;

    switch (special_options) {
	case SPECIAL_SHOW_HELP:
	    /* Does gettext calls internally */
	    help_msg(help_head, help_tail, options, NELEMS(options));
	    return EXIT_SUCCESS;
	case SPECIAL_SHOW_VERSION:
	    for (i=0; i<sizeof(version_msg)/sizeof(char *); i++)
		printf("%s", gettext(version_msg[i]));
	    return EXIT_SUCCESS;
	case SPECIAL_LISTED:
	    /* Printed out earlier */
	    return EXIT_SUCCESS;
    }

    /* Initialize BitVector (needed for floating point). */
    if (BitVector_Boot() != ErrCode_Ok) {
	print_error(_("%s: could not initialize BitVector"), _("FATAL"));
	return EXIT_FAILURE;
    }

    if (in_filename && strcmp(in_filename, "-") != 0) {
	/* Open the input file (if not standard input) */
	in = fopen(in_filename, "rt");
	if (!in) {
	    print_error(_("%s: could not open file `%s'"), _("FATAL"),
			in_filename);
	    yasm_xfree(in_filename);
	    if (obj_filename)
		yasm_xfree(obj_filename);
	    return EXIT_FAILURE;
	}
    } else {
	/* If no files were specified or filename was "-", read stdin */
	in = stdin;
	if (!in_filename)
	    in_filename = yasm__xstrdup("-");
    }

    /* Initialize line manager */
    yasm_std_linemgr.initialize();
    yasm_std_linemgr.set(in_filename, 1, 1);

    /* Initialize intnum and floatnum */
    yasm_intnum_initialize();
    yasm_floatnum_initialize();

    /* Initialize symbol table */
    yasm_symrec_initialize();

    /* handle preproc-only case here */
    if (preproc_only) {
	char *preproc_buf = yasm_xmalloc(PREPROC_BUF_SIZE);
	size_t got;

	/* Default output to stdout if not specified */
	if (!obj_filename)
	    obj = stdout;
	else {
	    /* Open output (object) file */
	    obj = open_obj("wt");
	    if (!obj) {
		yasm_xfree(preproc_buf);
		return EXIT_FAILURE;
	    }
	}

	/* If not already specified, default to nasm preproc. */
	if (!cur_preproc)
	    cur_preproc = load_preproc("nasm");

	if (!cur_preproc) {
	    print_error(_("%s: could not load default %s"), _("FATAL"),
			_("preprocessor"));
	    cleanup(NULL);
	    return EXIT_FAILURE;
	}
	check_module_version(cur_preproc, PREPROC, "preproc");

	apply_preproc_saved_options();

	/* Pre-process until done */
	cur_preproc->initialize(in, in_filename, &yasm_std_linemgr);
	while ((got = cur_preproc->input(preproc_buf, PREPROC_BUF_SIZE)) != 0)
	    fwrite(preproc_buf, got, 1, obj);

	if (in != stdin)
	    fclose(in);

	if (obj != stdout)
	    fclose(obj);

	if (yasm_get_num_errors(warning_error) > 0) {
	    yasm_errwarn_output_all(&yasm_std_linemgr, warning_error,
				    print_yasm_error, print_yasm_warning);
	    if (obj != stdout)
		remove(obj_filename);
	    yasm_xfree(preproc_buf);
	    cleanup(NULL);
	    return EXIT_FAILURE;
	}
	yasm_xfree(preproc_buf);
	cleanup(NULL);
	return EXIT_SUCCESS;
    }

    /* Default to x86 as the architecture */
    if (!cur_arch) {
	cur_arch = load_arch("x86");
	if (!cur_arch) {
	    print_error(_("%s: could not load default %s"), _("FATAL"),
			_("architecture"));
	    return EXIT_FAILURE;
	}
    }
    check_module_version(cur_arch, ARCH, "arch");

    /* Set up architecture using the selected (or default) machine */
    if (!machine_name)
	machine_name = yasm__xstrdup(cur_arch->default_machine_keyword);
    
    if (cur_arch->initialize(machine_name)) {
	if (strcmp(machine_name, "help") == 0) {
	    yasm_arch_machine *m = cur_arch->machines;
	    printf(_("Available %s for %s `%s':\n"), _("machines"),
		   _("architecture"), cur_arch->keyword);
	    while (m->keyword && m->name) {
		print_list_keyword_desc(m->name, m->keyword);
		m++;
	    }
	    return EXIT_SUCCESS;
	}

	print_error(_("%s: `%s' is not a valid %s for %s `%s'"),
		    _("FATAL"), machine_name, _("machine"),
		    _("architecture"), cur_arch->keyword);
	return EXIT_FAILURE;
    }

    /* Set basic as the optimizer (TODO: user choice) */
    cur_optimizer = load_optimizer("basic");

    if (!cur_optimizer) {
	print_error(_("%s: could not load default %s"), _("FATAL"),
		    _("optimizer"));
	return EXIT_FAILURE;
    }
    check_module_version(cur_optimizer, OPTIMIZER, "optimizer");

    yasm_arch_common_initialize(cur_arch);
    yasm_expr_initialize(cur_arch);
    yasm_bc_initialize(cur_arch);

    /* If not already specified, default to bin as the object format. */
    if (!cur_objfmt)
	cur_objfmt = load_objfmt("bin");

    if (!cur_objfmt) {
	print_error(_("%s: could not load default %s"), _("FATAL"),
		    _("object format"));
	return EXIT_FAILURE;
    }
    check_module_version(cur_objfmt, OBJFMT, "objfmt");

    /* If not already specified, default to null as the debug format. */
    if (!cur_dbgfmt)
	cur_dbgfmt = load_dbgfmt("null");
    else {
	int matched_dbgfmt = 0;
	/* Check to see if the requested debug format is in the allowed list
	 * for the active object format.
	 */
	for (i=0; cur_objfmt->dbgfmt_keywords[i]; i++)
	    if (yasm__strcasecmp(cur_objfmt->dbgfmt_keywords[i],
				 cur_dbgfmt->keyword) == 0)
		matched_dbgfmt = 1;
	if (!matched_dbgfmt) {
	    print_error(_("%s: `%s' is not a valid %s for %s `%s'"),
		_("FATAL"), cur_dbgfmt->keyword, _("debug format"),
		_("object format"), cur_objfmt->keyword);
	    if (in != stdin)
		fclose(in);
	    /*cleanup(NULL);*/
	    return EXIT_FAILURE;
	}
    }

    if (!cur_dbgfmt) {
	print_error(_("%s: could not load default %s"), _("FATAL"),
		    _("debug format"));
	return EXIT_FAILURE;
    }
    check_module_version(cur_dbgfmt, DBGFMT, "dbgfmt");

    /* determine the object filename if not specified */
    if (!obj_filename) {
	if (in == stdin)
	    /* Default to yasm.out if no obj filename specified */
	    obj_filename = yasm__xstrdup("yasm.out");
	else
	    /* replace (or add) extension */
	    obj_filename = replace_extension(in_filename,
					     cur_objfmt->extension,
					     "yasm.out");
    }

    /* Initialize the object format */
    if (cur_objfmt->initialize) {
	if (cur_objfmt->initialize(in_filename, obj_filename, cur_dbgfmt,
				   cur_arch, machine_name)) {
	    print_error(
		_("%s: object format `%s' does not support architecture `%s' machine `%s'"),
		_("FATAL"), cur_objfmt->keyword, cur_arch->keyword,
		machine_name);
	    if (in != stdin)
		fclose(in);
	    return EXIT_FAILURE;
	}
    }

    /* Default to NASM as the parser */
    if (!cur_parser) {
	cur_parser = load_parser("nasm");
	if (!cur_parser) {
	    print_error(_("%s: could not load default %s"), _("FATAL"),
			_("parser"));
	    cleanup(NULL);
	    return EXIT_FAILURE;
	}
    }
    check_module_version(cur_parser, PARSER, "parser");

    /* If not already specified, default to the parser's default preproc. */
    if (!cur_preproc)
	cur_preproc = load_preproc(cur_parser->default_preproc_keyword);
    else {
	int matched_preproc = 0;
	/* Check to see if the requested preprocessor is in the allowed list
	 * for the active parser.
	 */
	for (i=0; cur_parser->preproc_keywords[i]; i++)
	    if (yasm__strcasecmp(cur_parser->preproc_keywords[i],
			   cur_preproc->keyword) == 0)
		matched_preproc = 1;
	if (!matched_preproc) {
	    print_error(_("%s: `%s' is not a valid %s for %s `%s'"),
			_("FATAL"), cur_preproc->keyword, _("preprocessor"),
			_("parser"), cur_parser->keyword);
	    if (in != stdin)
		fclose(in);
	    cleanup(NULL);
	    return EXIT_FAILURE;
	}
    }

    if (!cur_preproc) {
	print_error(_("%s: could not load default %s"), _("FATAL"),
		    _("preprocessor"));
	cleanup(NULL);
	return EXIT_FAILURE;
    }
    check_module_version(cur_preproc, PREPROC, "preproc");

    apply_preproc_saved_options();

    /* Get initial x86 BITS setting from object format */
    if (strcmp(cur_arch->keyword, "x86") == 0) {
	unsigned char *x86_mode_bits;
	x86_mode_bits = (unsigned char *)get_module_data(MODULE_ARCH, "x86",
							 "mode_bits");
	if (x86_mode_bits)
	    *x86_mode_bits = cur_objfmt->default_x86_mode_bits;
    }

    /* Parse! */
    sections = cur_parser->do_parse(cur_preproc, cur_arch, cur_objfmt,
				    &yasm_std_linemgr, in, in_filename, 0);

    /* Close input file */
    if (in != stdin)
	fclose(in);

    if (yasm_get_num_errors(warning_error) > 0) {
	yasm_errwarn_output_all(&yasm_std_linemgr, warning_error,
				print_yasm_error, print_yasm_warning);
	cleanup(sections);
	return EXIT_FAILURE;
    }

    yasm_symrec_parser_finalize();
    cur_optimizer->optimize(sections);

    if (yasm_get_num_errors(warning_error) > 0) {
	yasm_errwarn_output_all(&yasm_std_linemgr, warning_error,
				print_yasm_error, print_yasm_warning);
	cleanup(sections);
	return EXIT_FAILURE;
    }

    /* open the object file for output (if not already opened by dbg objfmt) */
    if (!obj && strcmp(cur_objfmt->keyword, "dbg") != 0) {
	obj = open_obj("wb");
	if (!obj) {
	    cleanup(sections);
	    return EXIT_FAILURE;
	}
    }

    /* Write the object file */
    cur_objfmt->output(obj?obj:stderr, sections,
		       strcmp(cur_dbgfmt->keyword, "null"));

    /* Close object file */
    if (obj)
	fclose(obj);

    /* If we had an error at this point, we also need to delete the output
     * object file (to make sure it's not left newer than the source).
     */
    if (yasm_get_num_errors(warning_error) > 0) {
	yasm_errwarn_output_all(&yasm_std_linemgr, warning_error,
				print_yasm_error, print_yasm_warning);
	remove(obj_filename);
	cleanup(sections);
	return EXIT_FAILURE;
    }

    yasm_errwarn_output_all(&yasm_std_linemgr, warning_error,
			    print_yasm_error, print_yasm_warning);

    cleanup(sections);
    return EXIT_SUCCESS;
}
/*@=globstate =unrecog@*/

/* Open the object file.  Returns 0 on failure. */
static FILE *
open_obj(const char *mode)
{
    FILE *obj;

    assert(obj_filename != NULL);

    obj = fopen(obj_filename, mode);
    if (!obj)
	print_error(_("could not open file `%s'"), obj_filename);
    return obj;
}

/* Define DO_FREE to 1 to enable deallocation of all data structures.
 * Useful for detecting memory leaks, but slows down execution unnecessarily
 * (as the OS will free everything we miss here).
 */
#define DO_FREE		1

/* Cleans up all allocated structures. */
static void
cleanup(yasm_sectionhead *sections)
{
    if (DO_FREE) {
	if (cur_objfmt && cur_objfmt->cleanup)
	    cur_objfmt->cleanup();
	if (cur_dbgfmt && cur_dbgfmt->cleanup)
	    cur_dbgfmt->cleanup();
	if (cur_preproc)
	    cur_preproc->cleanup();
	if (sections)
	    yasm_sections_delete(sections);
	yasm_symrec_cleanup();
	if (cur_arch)
	    cur_arch->cleanup();

	yasm_floatnum_cleanup();
	yasm_intnum_cleanup();

	yasm_errwarn_cleanup();
	yasm_std_linemgr.cleanup();

	BitVector_Shutdown();
    }

    unload_modules();

#ifndef WIN32
    /* Finish with libltdl. */
    lt_dlexit();
#endif

    if (DO_FREE) {
	if (in_filename)
	    yasm_xfree(in_filename);
	if (obj_filename)
	    yasm_xfree(obj_filename);
	if (machine_name)
	    yasm_xfree(machine_name);
    }
}

/*
 *  Command line options handlers
 */
int
not_an_option_handler(char *param)
{
    if (in_filename) {
	print_error(
	    _("warning: can open only one input file, only the last file will be processed"));
	yasm_xfree(in_filename);
    }

    in_filename = yasm__xstrdup(param);

    return 0;
}

static int
opt_special_handler(/*@unused@*/ char *cmd, /*@unused@*/ char *param, int extra)
{
    if (special_options == 0)
	special_options = extra;
    return 0;
}

static int
opt_arch_handler(/*@unused@*/ char *cmd, char *param, /*@unused@*/ int extra)
{
    assert(param != NULL);
    cur_arch = load_arch(param);
    if (!cur_arch) {
	if (!strcmp("help", param)) {
	    printf(_("Available yasm %s:\n"), _("architectures"));
	    list_archs(print_list_keyword_desc);
	    special_options = SPECIAL_LISTED;
	    return 0;
	}
	print_error(_("%s: unrecognized %s `%s'"), _("FATAL"),
		    _("architecture"), param);
	exit(EXIT_FAILURE);
    }
    return 0;
}

static int
opt_parser_handler(/*@unused@*/ char *cmd, char *param, /*@unused@*/ int extra)
{
    assert(param != NULL);
    cur_parser = load_parser(param);
    if (!cur_parser) {
	if (!strcmp("help", param)) {
	    printf(_("Available yasm %s:\n"), _("parsers"));
	    list_parsers(print_list_keyword_desc);
	    special_options = SPECIAL_LISTED;
	    return 0;
	}
	print_error(_("%s: unrecognized %s `%s'"), _("FATAL"), _("parser"),
		    param);
	exit(EXIT_FAILURE);
    }
    return 0;
}

static int
opt_preproc_handler(/*@unused@*/ char *cmd, char *param, /*@unused@*/ int extra)
{
    assert(param != NULL);
    cur_preproc = load_preproc(param);
    if (!cur_preproc) {
	if (!strcmp("help", param)) {
	    printf(_("Available yasm %s:\n"), _("preprocessors"));
	    list_preprocs(print_list_keyword_desc);
	    special_options = SPECIAL_LISTED;
	    return 0;
	}
	print_error(_("%s: unrecognized %s `%s'"), _("FATAL"),
		    _("preprocessor"), param);
	exit(EXIT_FAILURE);
    }
    return 0;
}

static int
opt_objfmt_handler(/*@unused@*/ char *cmd, char *param, /*@unused@*/ int extra)
{
    assert(param != NULL);
    cur_objfmt = load_objfmt(param);
    if (!cur_objfmt) {
	if (!strcmp("help", param)) {
	    printf(_("Available yasm %s:\n"), _("object formats"));
	    list_objfmts(print_list_keyword_desc);
	    special_options = SPECIAL_LISTED;
	    return 0;
	}
	print_error(_("%s: unrecognized %s `%s'"), _("FATAL"),
		    _("object format"), param);
	exit(EXIT_FAILURE);
    }
    return 0;
}

static int
opt_dbgfmt_handler(/*@unused@*/ char *cmd, char *param, /*@unused@*/ int extra)
{
    assert(param != NULL);
    cur_dbgfmt = load_dbgfmt(param);
    if (!cur_dbgfmt) {
	if (!strcmp("help", param)) {
	    printf(_("Available yasm %s:\n"), _("debug formats"));
	    list_dbgfmts(print_list_keyword_desc);
	    special_options = SPECIAL_LISTED;
	    return 0;
	}
	print_error(_("%s: unrecognized %s `%s'"), _("FATAL"),
		    _("debug format"), param);
	exit(EXIT_FAILURE);
    }
    return 0;
}

static int
opt_objfile_handler(/*@unused@*/ char *cmd, char *param,
		    /*@unused@*/ int extra)
{
    if (obj_filename) {
	print_error(
	    _("warning: can output to only one object file, last specified used"));
	yasm_xfree(obj_filename);
    }

    assert(param != NULL);
    obj_filename = yasm__xstrdup(param);

    return 0;
}

static int
opt_machine_handler(/*@unused@*/ char *cmd, char *param,
		    /*@unused@*/ int extra)
{
    if (machine_name)
	yasm_xfree(machine_name);

    assert(param != NULL);
    machine_name = yasm__xstrdup(param);

    return 0;
}

static int
opt_warning_handler(char *cmd, /*@unused@*/ char *param, int extra)
{
    int enable = 1;	/* is it disabling the warning instead of enabling? */

    if (extra == 1) {
	/* -w, disable warnings */
	yasm_warn_disable_all();
	return 0;
    }

    /* skip past 'W' */
    cmd++;

    /* detect no- prefix to disable the warning */
    if (cmd[0] == 'n' && cmd[1] == 'o' && cmd[2] == '-') {
	enable = 0;
	cmd += 3;   /* skip past it to get to the warning name */
    }

    if (cmd[0] == '\0')
	/* just -W or -Wno-, so definitely not valid */
	return 1;
    else if (strcmp(cmd, "error") == 0) {
	warning_error = enable;
    } else if (strcmp(cmd, "unrecognized-char") == 0) {
	if (enable)
	    yasm_warn_enable(YASM_WARN_UNREC_CHAR);
	else
	    yasm_warn_disable(YASM_WARN_UNREC_CHAR);
    } else
	return 1;

    return 0;
}

static int
preproc_only_handler(/*@unused@*/ char *cmd, /*@unused@*/ char *param,
		     /*@unused@*/ int extra)
{
    preproc_only = 1;
    return 0;
}

static int
opt_preproc_include_path(/*@unused@*/ char *cmd, char *param,
			 /*@unused@*/ int extra)
{
    constcharparam *cp;
    cp = yasm_xmalloc(sizeof(constcharparam));
    cp->param = param;
    STAILQ_INSERT_TAIL(&includepaths, cp, link);
    return 0;
}

static void
apply_preproc_saved_options()
{
    constcharparam *cp;
    constcharparam *cpnext;

    if (cur_preproc->add_include_path != NULL) {
	STAILQ_FOREACH(cp, &includepaths, link) {
	    cur_preproc->add_include_path(cp->param);
	}
    }

    cp = STAILQ_FIRST(&includepaths);
    while (cp != NULL) {
	cpnext = STAILQ_NEXT(cp, link);
	yasm_xfree(cp);
	cp = cpnext;
    }
    STAILQ_INIT(&includepaths);

    if (cur_preproc->add_include_file != NULL) {
	STAILQ_FOREACH(cp, &includefiles, link) {
	    cur_preproc->add_include_file(cp->param);
	}
    }

    cp = STAILQ_FIRST(&includepaths);
    while (cp != NULL) {
	cpnext = STAILQ_NEXT(cp, link);
	yasm_xfree(cp);
	cp = cpnext;
    }
    STAILQ_INIT(&includepaths);
}

static int
opt_preproc_include_file(/*@unused@*/ char *cmd, char *param,
			 /*@unused@*/ int extra)
{
    constcharparam *cp;
    cp = yasm_xmalloc(sizeof(constcharparam));
    cp->param = param;
    STAILQ_INSERT_TAIL(&includefiles, cp, link);
    return 0;
}

/* Replace extension on a filename (or append one if none is present).
 * If output filename would be identical to input (same extension out as in),
 * returns (copy of) def.
 * A NULL ext means the trailing '.' should NOT be included, whereas a "" ext
 * means the trailing '.' should be included.
 */
static char *
replace_extension(const char *orig, /*@null@*/ const char *ext,
		  const char *def)
{
    char *out, *outext;

    /* allocate enough space for full existing name + extension */
    out = yasm_xmalloc(strlen(orig)+(ext ? (strlen(ext)+2) : 1));
    strcpy(out, orig);
    outext = strrchr(out, '.');
    if (outext) {
	/* Existing extension: make sure it's not the same as the replacement
	 * (as we don't want to overwrite the source file).
	 */
	outext++;   /* advance past '.' */
	if (ext && strcmp(outext, ext) == 0) {
	    outext = NULL;  /* indicate default should be used */
	    print_error(
		_("file name already ends in `.%s': output will be in `%s'"),
		ext, def);
	}
    } else {
	/* No extension: make sure the output extension is not empty
	 * (again, we don't want to overwrite the source file).
	 */
	if (!ext)
	    print_error(
		_("file name already has no extension: output will be in `%s'"),
		def);
	else {
	    outext = strrchr(out, '\0');    /* point to end of the string */
	    *outext++ = '.';		    /* append '.' */
	}
    }

    /* replace extension or use default name */
    if (outext) {
	if (!ext) {
	    /* Back up and replace '.' with string terminator */
	    outext--;
	    *outext = '\0';
	} else
	    strcpy(outext, ext);
    } else
	strcpy(out, def);

    return out;
}

void
print_list_keyword_desc(const char *name, const char *keyword)
{
    printf("%4s%-12s%s\n", "", keyword, name);
}

static void
print_error(const char *fmt, ...)
{
    va_list va;
    fprintf(stderr, "yasm: ");
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    fputc('\n', stderr);
}

static /*@exits@*/ void
handle_yasm_int_error(const char *file, unsigned int line, const char *message)
{
    fprintf(stderr, _("INTERNAL ERROR at %s, line %u: %s\n"), file, line,
	    gettext(message));
#ifdef HAVE_ABORT
    abort();
#else
    exit(EXIT_FAILURE);
#endif
}

static /*@exits@*/ void
handle_yasm_fatal(const char *fmt, ...)
{
    va_list va;
    fprintf(stderr, "yasm: %s: ", _("FATAL"));
    va_start(va, fmt);
    vfprintf(stderr, gettext(fmt), va);
    va_end(va);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static const char *
handle_yasm_gettext(const char *msgid)
{
    return gettext(msgid);
}

static void
print_yasm_error(const char *filename, unsigned long line, const char *msg)
{
    fprintf(stderr, "%s:%lu: %s\n", filename, line, msg);
}

static void
print_yasm_warning(const char *filename, unsigned long line, const char *msg)
{
    fprintf(stderr, "%s:%lu: %s %s\n", filename, line, _("warning:"), msg);
}