/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007-2012 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2008-2009 Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2011-2019 IBM Corporation.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <time.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <fcntl.h>
#include <signal.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"
#include "src/event/event-internal.h"
#include "src/util/argv.h"
#include "src/util/prte_environ.h"
#include "src/util/basename.h"
#include "src/util/path.h"
#include "src/class/prte_pointer_array.h"

#include "src/util/show_help.h"
#include "src/runtime/prte_wait.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/fd.h"
#include "src/threads/threads.h"

#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/routed/routed.h"
#include "src/mca/rml/base/rml_contact.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/state/state.h"

#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/rsh/plm_rsh.h"

static int rsh_init(void);
static int rsh_launch(prte_job_t *jdata);
static int remote_spawn(void);
static int rsh_terminate_prteds(void);
static int rsh_finalize(void);

prte_plm_base_module_t prte_plm_rsh_module = {
    .init = rsh_init,
    .set_hnp_name = prte_plm_base_set_hnp_name,
    .spawn = rsh_launch,
    .remote_spawn = remote_spawn,
    .terminate_job = prte_plm_base_prted_terminate_job,
    .terminate_orteds = rsh_terminate_prteds,
    .terminate_procs = prte_plm_base_prted_kill_local_procs,
    .signal_job = prte_plm_base_prted_signal_local_procs,
    .finalize = rsh_finalize
};

typedef struct {
    prte_list_item_t super;
    int argc;
    char **argv;
    prte_proc_t *daemon;
} prte_plm_rsh_caddy_t;
static void caddy_const(prte_plm_rsh_caddy_t *ptr)
{
    ptr->argv = NULL;
    ptr->daemon = NULL;
}
static void caddy_dest(prte_plm_rsh_caddy_t *ptr)
{
    if (NULL != ptr->argv) {
        prte_argv_free(ptr->argv);
    }
    if (NULL != ptr->daemon) {
        PRTE_RELEASE(ptr->daemon);
    }
}
PRTE_CLASS_INSTANCE(prte_plm_rsh_caddy_t,
                   prte_list_item_t,
                   caddy_const, caddy_dest);

typedef enum {
    PRTE_PLM_RSH_SHELL_BASH = 0,
    PRTE_PLM_RSH_SHELL_ZSH,
    PRTE_PLM_RSH_SHELL_TCSH,
    PRTE_PLM_RSH_SHELL_CSH,
    PRTE_PLM_RSH_SHELL_KSH,
    PRTE_PLM_RSH_SHELL_SH,
    PRTE_PLM_RSH_SHELL_UNKNOWN
} prte_plm_rsh_shell_t;

/* These strings *must* follow the same order as the enum PRTE_PLM_RSH_SHELL_* */
static const char *prte_plm_rsh_shell_name[7] = {
    "bash",
    "zsh",
    "tcsh",       /* tcsh has to be first otherwise strstr finds csh */
    "csh",
    "ksh",
    "sh",
    "unknown"
};

/*
 * Local functions
 */
static void set_handler_default(int sig);
static prte_plm_rsh_shell_t find_shell(char *shell);
static int launch_agent_setup(const char *agent, char *path);
static void ssh_child(int argc, char **argv) __prte_attribute_noreturn__;
static int rsh_probe(char *nodename,
                     prte_plm_rsh_shell_t *shell);
static int setup_shell(prte_plm_rsh_shell_t *rshell,
                       prte_plm_rsh_shell_t *lshell,
                       char *nodename, int *argc, char ***argv);
static void launch_daemons(int fd, short args, void *cbdata);
static void process_launch_list(int fd, short args, void *cbdata);

/* local global storage */
static int num_in_progress=0;
static prte_list_t launch_list;
static prte_event_t launch_event;
static char *rsh_agent_path=NULL;
static char **rsh_agent_argv=NULL;

/**
 * Init the module
 */
static int rsh_init(void)
{
    char *tmp;
    int rc;

    /* we were selected, so setup the launch agent */
    if (prte_plm_rsh_component.using_qrsh) {
        /* perform base setup for qrsh */
        prte_asprintf(&tmp, "%s/bin/%s", getenv("SGE_ROOT"), getenv("ARC"));
        if (PRTE_SUCCESS != (rc = launch_agent_setup("qrsh", tmp))) {
            PRTE_ERROR_LOG(rc);
            free(tmp);
            return rc;
        }
        free(tmp);
        /* automatically add -inherit and grid engine PE related flags */
        prte_argv_append_nosize(&rsh_agent_argv, "-inherit");
        /* Don't use the "-noshell" flag as qrsh would have a problem
         * swallowing a long command */
        prte_argv_append_nosize(&rsh_agent_argv, "-nostdin");
        prte_argv_append_nosize(&rsh_agent_argv, "-V");
        if (0 < prte_output_get_verbosity(prte_plm_base_framework.framework_output)) {
            prte_argv_append_nosize(&rsh_agent_argv, "-verbose");
            tmp = prte_argv_join(rsh_agent_argv, ' ');
            prte_output_verbose(1, prte_plm_base_framework.framework_output,
                                "%s plm:rsh: using \"%s\" for launching\n",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), tmp);
            free(tmp);
        }
    } else if(prte_plm_rsh_component.using_llspawn) {
        /* perform base setup for llspawn */
        if (PRTE_SUCCESS != (rc = launch_agent_setup("llspawn", NULL))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        prte_output_verbose(1, prte_plm_base_framework.framework_output,
                            "%s plm:rsh: using \"%s\" for launching\n",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            rsh_agent_path);
    } else {
        /* not using qrsh or llspawn - use MCA-specified agent */
        if (PRTE_SUCCESS != (rc = launch_agent_setup(prte_plm_rsh_component.agent, NULL))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    /* point to our launch command */
    if (PRTE_SUCCESS != (rc = prte_state.add_job_state(PRTE_JOB_STATE_LAUNCH_DAEMONS,
                                                       launch_daemons, PRTE_SYS_PRI))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* setup the event for metering the launch */
    PRTE_CONSTRUCT(&launch_list, prte_list_t);
    prte_event_set(prte_event_base, &launch_event, -1, 0, process_launch_list, NULL);
    prte_event_set_priority(&launch_event, PRTE_SYS_PRI);

    /* start the recvs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_start())) {
        PRTE_ERROR_LOG(rc);
    }

    /* we assign daemon nodes at launch */
    prte_plm_globals.daemon_nodes_assigned_at_launch = true;

    return rc;
}

/**
 * Callback on daemon exit.
 */
static void rsh_wait_daemon(int sd, short flags, void *cbdata)
{
    prte_job_t *jdata;
    prte_wait_tracker_t *t2 = (prte_wait_tracker_t*)cbdata;
    prte_plm_rsh_caddy_t *caddy=(prte_plm_rsh_caddy_t*)t2->cbdata;
    prte_proc_t *daemon = caddy->daemon;

    if (prte_prteds_term_ordered || prte_abnormal_term_ordered) {
        /* ignore any such report - it will occur if we left the
         * session attached, e.g., while debugging
         */
        PRTE_RELEASE(caddy);
        PRTE_RELEASE(t2);
        return;
    }

    if (!WIFEXITED(daemon->exit_code) ||
        WEXITSTATUS(daemon->exit_code) != 0) { /* if abnormal exit */
        /* if we are not the HNP, send a message to the HNP alerting it
         * to the failure
         */
        if (!PRTE_PROC_IS_MASTER) {
            prte_buffer_t *buf;
            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s daemon %d failed with status %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (int)daemon->name.vpid, WEXITSTATUS(daemon->exit_code)));
            buf = PRTE_NEW(prte_buffer_t);
            prte_dss.pack(buf, &(daemon->name.vpid), 1, PRTE_VPID);
            prte_dss.pack(buf, &daemon->exit_code, 1, PRTE_INT);
            prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf,
                                    PRTE_RML_TAG_REPORT_REMOTE_LAUNCH,
                                    prte_rml_send_callback, NULL);
            /* note that this daemon failed */
            daemon->state = PRTE_PROC_STATE_FAILED_TO_START;
        } else {
            jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->jobid);

            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s daemon %d failed with status %d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 (int)daemon->name.vpid, WEXITSTATUS(daemon->exit_code)));
            /* set the exit status */
            PRTE_UPDATE_EXIT_STATUS(WEXITSTATUS(daemon->exit_code));
            /* note that this daemon failed */
            daemon->state = PRTE_PROC_STATE_FAILED_TO_START;
            /* increment the #daemons terminated so we will exit properly */
            jdata->num_terminated++;
            /* remove it from the routing table to ensure num_routes
             * returns the correct value
             */
            prte_routed.route_lost(&daemon->name);
            /* report that the daemon has failed so we can exit */
            PRTE_ACTIVATE_PROC_STATE(&daemon->name, PRTE_PROC_STATE_FAILED_TO_START);
        }
    }

    /* release any delay */
    --num_in_progress;
    if (num_in_progress < prte_plm_rsh_component.num_concurrent) {
        /* trigger continuation of the launch */
        prte_event_active(&launch_event, EV_WRITE, 1);
    }
    /* cleanup */
    PRTE_RELEASE(t2);
}

static int setup_launch(int *argcptr, char ***argvptr,
                        char *nodename,
                        int *node_name_index1,
                        int *proc_vpid_index, char *prefix_dir)
{
    int argc;
    char **argv;
    char *param, *value;
    prte_plm_rsh_shell_t remote_shell, local_shell;
    int orted_argc;
    char **orted_argv;
    char *orted_cmd, *orted_prefix, *final_cmd;
    int orted_index;
    int rc;
    int i;
    char *lib_base=NULL, *bin_base=NULL;
    char *prte_prefix = getenv("PRTE_PREFIX");
    char* full_orted_cmd = NULL;

    /* Figure out the basenames for the libdir and bindir.  This
       requires some explanation:

       - Use prte_install_dirs.libdir and prte_install_dirs.bindir.

       - After a discussion on the devel-core mailing list, the
       developers decided that we should use the local directory
       basenames as the basis for the prefix on the remote note.
       This does not handle a few notable cases (e.g., if the
       libdir/bindir is not simply a subdir under the prefix, if the
       libdir/bindir basename is not the same on the remote node as
       it is here on the local node, etc.), but we decided that
       --prefix was meant to handle "the common case".  If you need
       something more complex than this, a) edit your shell startup
       files to set PATH/LD_LIBRARY_PATH properly on the remove
       node, or b) use some new/to-be-defined options that
       explicitly allow setting the bindir/libdir on the remote
       node.  We decided to implement these options (e.g.,
       --remote-bindir and --remote-libdir) to prun when it
       actually becomes a problem for someone (vs. a hypothetical
       situation).

       Hence, for now, we simply take the basename of this install's
       libdir and bindir and use it to append this install's prefix
       and use that on the remote node.
    */

    /*
     * Build argv array
     */
    argv = prte_argv_copy(rsh_agent_argv);
    argc = prte_argv_count(argv);
    /* if any ssh args were provided, now is the time to add them */
    if (NULL != prte_plm_rsh_component.ssh_args) {
        char **ssh_argv;
        ssh_argv = prte_argv_split(prte_plm_rsh_component.ssh_args, ' ');
        for (i=0; NULL != ssh_argv[i]; i++) {
            prte_argv_append(&argc, &argv, ssh_argv[i]);
        }
        prte_argv_free(ssh_argv);
    }
    *node_name_index1 = argc;
    prte_argv_append(&argc, &argv, "<template>");

    /* setup the correct shell info */
    if (PRTE_SUCCESS != (rc = setup_shell(&remote_shell, &local_shell,
                                          nodename, &argc, &argv))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* now get the orted cmd - as specified by user - into our tmp array.
     * The function returns the location where the actual orted command is
     * located - usually in the final spot, but someone could
     * have added options. For example, it should be legal for them to use
     * "orted --debug-devel" so they get debug output from the orteds, but
     * not from mpirun. Also, they may have a customized version of orted
     * that takes arguments in addition to the std ones we already support
     */
    orted_argc = 0;
    orted_argv = NULL;
    orted_index = prte_plm_base_setup_prted_cmd(&orted_argc, &orted_argv);

    /* look at the returned orted cmd argv to check several cases:
     *
     * - only "orted" was given. This is the default and thus most common
     *   case. In this situation, there is nothing we need to do
     *
     * - something was given that doesn't include "orted" - i.e., someone
     *   has substituted their own daemon. There isn't anything we can
     *   do here, so we want to avoid adding prefixes to the cmd
     *
     * - something was given that precedes "orted". For example, someone
     *   may have specified "valgrind [options] orted". In this case, we
     *   need to separate out that "orted_prefix" section so it can be
     *   treated separately below
     *
     * - something was given that follows "orted". An example was given above.
     *   In this case, we need to construct the effective "orted_cmd" so it
     *   can be treated properly below
     *
     * Obviously, the latter two cases can be combined - just to make it
     * even more interesting! Gotta love rsh/ssh...
     */
    if (0 == orted_index) {
        /* single word cmd - this is the default scenario, but there could
         * be options specified so we need to account for that possibility.
         * However, we don't need/want a prefix as nothing precedes the orted
         * cmd itself
         */
        orted_cmd = prte_argv_join(orted_argv, ' ');
        orted_prefix = NULL;
    } else {
        /* okay, so the "orted" cmd is somewhere in this array, with
         * something preceding it and perhaps things following it.
         */
        orted_prefix = prte_argv_join_range(orted_argv, 0, orted_index, ' ');
        orted_cmd = prte_argv_join_range(orted_argv, orted_index, prte_argv_count(orted_argv), ' ');
    }
    prte_argv_free(orted_argv);  /* done with this */

    /* if the user specified a library path to pass, set it up now */
    param = prte_basename(prte_install_dirs.libdir);
    if (NULL != prte_plm_rsh_component.pass_libpath) {
        if (NULL != prefix_dir) {
            prte_asprintf(&lib_base, "%s:%s/%s", prte_plm_rsh_component.pass_libpath, prefix_dir, param);
        } else {
            prte_asprintf(&lib_base, "%s:%s", prte_plm_rsh_component.pass_libpath, param);
        }
    } else if (NULL != prefix_dir) {
        prte_asprintf(&lib_base, "%s/%s", prefix_dir, param);
    }
    free(param);

    /* we now need to assemble the actual cmd that will be executed - this depends
     * upon whether or not a prefix directory is being used
     */
    if (NULL != prefix_dir) {
        /* if we have a prefix directory, we need to set the PATH and
         * LD_LIBRARY_PATH on the remote node, and prepend just the orted_cmd
         * with the prefix directory
         */

        value = prte_basename(prte_install_dirs.bindir);
        prte_asprintf(&bin_base, "%s/%s", prefix_dir, value);
        free(value);

        if (NULL != orted_cmd) {
            if (0 == strcmp(orted_cmd, "prted")) {
                /* if the cmd is our standard one, then add the prefix */
                prte_asprintf(&full_orted_cmd, "%s/%s", bin_base, orted_cmd);
            } else {
                /* someone specified something different, so don't prefix it */
                full_orted_cmd = strdup(orted_cmd);
            }
            free(orted_cmd);
        }
    } else {
        full_orted_cmd = orted_cmd;
    }

    if (NULL != lib_base || NULL != bin_base) {
        if (PRTE_PLM_RSH_SHELL_SH == remote_shell ||
            PRTE_PLM_RSH_SHELL_KSH == remote_shell ||
            PRTE_PLM_RSH_SHELL_ZSH == remote_shell ||
            PRTE_PLM_RSH_SHELL_BASH == remote_shell) {
            /* if there is nothing preceding orted, then we can just
             * assemble the cmd with the orted_cmd at the end. Otherwise,
             * we have to insert the orted_prefix in the right place
             */
            prte_asprintf (&final_cmd,
                            "%s%s%s%s%s%s PATH=%s%s$PATH ; export PATH ; "
                            "LD_LIBRARY_PATH=%s%s$LD_LIBRARY_PATH ; export LD_LIBRARY_PATH ; "
                            "DYLD_LIBRARY_PATH=%s%s$DYLD_LIBRARY_PATH ; export DYLD_LIBRARY_PATH ; "
                            "%s %s",
                            (NULL != prte_plm_rsh_component.chdir ? "cd " : " "),
                            (NULL != prte_plm_rsh_component.chdir ? prte_plm_rsh_component.chdir : " "),
                            (NULL != prte_plm_rsh_component.chdir ? " ; " : " "),
                            (prte_prefix != NULL ? "PRTE_PREFIX=" : " "),
                            (prte_prefix != NULL ? prte_prefix : " "),
                            (prte_prefix != NULL ? " ; export PRTE_PREFIX;" : " "),
                            (NULL != bin_base ? bin_base : " "),
                            (NULL != bin_base ? ":" : " "),
                            (NULL != lib_base ? lib_base : " "),
                            (NULL != lib_base ? ":" : " "),
                            (NULL != lib_base ? lib_base : " "),
                            (NULL != lib_base ? ":" : " "),
                            (orted_prefix != NULL ? orted_prefix : " "),
                            (full_orted_cmd != NULL ? full_orted_cmd : " "));
        } else if (PRTE_PLM_RSH_SHELL_TCSH == remote_shell ||
                   PRTE_PLM_RSH_SHELL_CSH == remote_shell) {
            /* [t]csh is a bit more challenging -- we
               have to check whether LD_LIBRARY_PATH
               is already set before we try to set it.
               Must be very careful about obeying
               [t]csh's order of evaluation and not
               using a variable before it is defined.
               See this thread for more details:
               https://www.open-mpi.org/community/lists/users/2006/01/0517.php. */
            /* if there is nothing preceding orted, then we can just
             * assemble the cmd with the orted_cmd at the end. Otherwise,
             * we have to insert the orted_prefix in the right place
             */
            prte_asprintf (&final_cmd,
                            "%s%s%s%s%s%s set path = ( %s $path ) ; "
                            "if ( $?LD_LIBRARY_PATH == 1 ) "
                            "set OMPI_have_llp ; "
                            "if ( $?LD_LIBRARY_PATH == 0 ) "
                            "setenv LD_LIBRARY_PATH %s ; "
                            "if ( $?OMPI_have_llp == 1 ) "
                            "setenv LD_LIBRARY_PATH %s%s$LD_LIBRARY_PATH ; "
                            "if ( $?DYLD_LIBRARY_PATH == 1 ) "
                            "set OMPI_have_dllp ; "
                            "if ( $?DYLD_LIBRARY_PATH == 0 ) "
                            "setenv DYLD_LIBRARY_PATH %s ; "
                            "if ( $?OMPI_have_dllp == 1 ) "
                            "setenv DYLD_LIBRARY_PATH %s%s$DYLD_LIBRARY_PATH ; "
                            "%s %s",
                            (NULL != prte_plm_rsh_component.chdir ? "cd " : " "),
                            (NULL != prte_plm_rsh_component.chdir ? prte_plm_rsh_component.chdir : " "),
                            (NULL != prte_plm_rsh_component.chdir ? " ; " : " "),
                            (prte_prefix != NULL ? "setenv PRTE_PREFIX " : " "),
                            (prte_prefix != NULL ? prte_prefix : " "),
                            (prte_prefix != NULL ? " ;" : " "),
                            (NULL != bin_base ? bin_base : " "),
                            (NULL != lib_base ? lib_base : " "),
                            (NULL != lib_base ? lib_base : " "),
                            (NULL != lib_base ? ":" : " "),
                            (NULL != lib_base ? lib_base : " "),
                            (NULL != lib_base ? lib_base : " "),
                            (NULL != lib_base ? ":" : " "),
                            (orted_prefix != NULL ? orted_prefix : " "),
                            (full_orted_cmd != NULL ? full_orted_cmd : " "));
        } else {
            prte_show_help("help-plm-rsh.txt", "cannot-resolve-shell-with-prefix", true,
                           (NULL == prte_prefix) ? "NULL" : prte_prefix,
                           prefix_dir);
            if (NULL != bin_base) {
                free(bin_base);
            }
            if (NULL != lib_base) {
                free(lib_base);
            }
            if (NULL != orted_prefix) free(orted_prefix);
            if (NULL != full_orted_cmd) free(full_orted_cmd);
            return PRTE_ERR_SILENT;
        }
        if (NULL != bin_base) {
            free(bin_base);
        }
        if (NULL != lib_base) {
            free(lib_base);
        }
        if( NULL != full_orted_cmd ) {
            free(full_orted_cmd);
        }
    } else {
        /* no prefix directory, so just aggregate the result */
        prte_asprintf(&final_cmd, "%s %s",
                       (orted_prefix != NULL ? orted_prefix : ""),
                       (full_orted_cmd != NULL ? full_orted_cmd : ""));
        if (NULL != full_orted_cmd) {
            free(full_orted_cmd);
        }
    }
    /* now add the final cmd to the argv array */
    prte_argv_append(&argc, &argv, final_cmd);
    free(final_cmd);  /* done with this */
    if (NULL != orted_prefix) free(orted_prefix);

    /* if we are not tree launching or debugging, tell the daemon
     * to daemonize so we can launch the next group
     */
    if (prte_plm_rsh_component.no_tree_spawn &&
        !prte_debug_flag &&
        !prte_debug_daemons_flag &&
        !prte_debug_daemons_file_flag &&
        !prte_leave_session_attached &&
        /* Daemonize when not using qrsh.  Or, if using qrsh, only
         * daemonize if told to by user with daemonize_qrsh flag. */
        ((!prte_plm_rsh_component.using_qrsh) ||
         (prte_plm_rsh_component.using_qrsh && prte_plm_rsh_component.daemonize_qrsh)) &&
        ((!prte_plm_rsh_component.using_llspawn) ||
         (prte_plm_rsh_component.using_llspawn && prte_plm_rsh_component.daemonize_llspawn))) {
        prte_argv_append(&argc, &argv, "--daemonize");
    }

    /*
     * Add the basic arguments to the orted command line, including
     * all debug options
     */
    prte_plm_base_prted_append_basic_args(&argc, &argv,
                                           "env",
                                           proc_vpid_index);

    /* ensure that only the ssh plm is selected on the remote daemon */
    prte_argv_append(&argc, &argv, "--prtemca");
    prte_argv_append(&argc, &argv, "plm");
    prte_argv_append(&argc, &argv, "rsh");

    /* if we are tree-spawning, tell our child daemons the
     * uri of their parent (me) */
    if (!prte_plm_rsh_component.no_tree_spawn) {
        prte_argv_append(&argc, &argv, "--tree-spawn");
        prte_oob_base_get_addr(&param);
        prte_argv_append(&argc, &argv, "--prtemca");
        prte_argv_append(&argc, &argv, "prte_parent_uri");
        prte_argv_append(&argc, &argv, param);
        free(param);
    }

    /* unless told otherwise... */
    if (prte_plm_rsh_component.pass_environ_mca_params) {
        /* now check our local environment for MCA params - add them
         * only if they aren't already present
         */
        if (PRTE_SUCCESS != (rc = prte_schizo.parse_env(NULL, environ, &argv, true))) {
            prte_argv_free(argv);
            return rc;
        }
        argc = prte_argv_count(argv);
    }

    /* protect the params */
    prte_schizo.wrap_args(argv);

    value = prte_argv_join(argv, ' ');
    if (sysconf(_SC_ARG_MAX) < (int)strlen(value)) {
        prte_show_help("help-plm-rsh.txt", "cmd-line-too-long",
                       true, strlen(value), sysconf(_SC_ARG_MAX));
        free(value);
        return PRTE_ERR_SILENT;
    }
    free(value);

    if (PRTE_PLM_RSH_SHELL_SH == remote_shell ||
        PRTE_PLM_RSH_SHELL_KSH == remote_shell) {
        prte_argv_append(&argc, &argv, ")");
    }

    if (0 < prte_output_get_verbosity(prte_plm_base_framework.framework_output)) {
        param = prte_argv_join(argv, ' ');
        prte_output(prte_plm_base_framework.framework_output,
                    "%s plm:rsh: final template argv:\n\t%s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                    (NULL == param) ? "NULL" : param);
        if (NULL != param) free(param);
    }

    /* all done */
    *argcptr = argc;
    *argvptr = argv;
    return PRTE_SUCCESS;
}

/* actually ssh the child */
static void ssh_child(int argc, char **argv)
{
    char** env;
    char* var;
    char *exec_path;
    char **exec_argv;
    int fdin;
    sigset_t sigs;

    /* setup environment */
    env = prte_argv_copy(prte_launch_environ);

    /* We don't need to sense an oversubscribed condition and set the sched_yield
     * for the node as we are only launching the daemons at this time. The daemons
     * are now smart enough to set the oversubscribed condition themselves when
     * they launch the local procs.
     */

    /* We cannot launch locally as this would cause multiple daemons to
     * exist on a node (HNP counts as a daemon). This is taken care of
     * by the earlier check for daemon_preexists, so we only have to worry
     * about remote launches here
     */
    exec_argv = argv;
    exec_path = strdup(rsh_agent_path);

    /* Don't let ssh slurp all of our stdin! */
    fdin = open("/dev/null", O_RDWR);
    dup2(fdin, 0);
    close(fdin);

    /* close all file descriptors w/ exception of stdin/stdout/stderr */
    prte_close_open_file_descriptors(-1);

    /* Set signal handlers back to the default.  Do this close
     to the execve() because the event library may (and likely
     will) reset them.  If we don't do this, the event
     library may have left some set that, at least on some
     OS's, don't get reset via fork() or exec().  Hence, the
     orted could be unkillable (for example). */

    set_handler_default(SIGTERM);
    set_handler_default(SIGINT);
    set_handler_default(SIGHUP);
    set_handler_default(SIGPIPE);
    set_handler_default(SIGCHLD);

    /* Unblock all signals, for many of the same reasons that
     we set the default handlers, above.  This is noticable
     on Linux where the event library blocks SIGTERM, but we
     don't want that blocked by the orted (or, more
     specifically, we don't want it to be blocked by the
     orted and then inherited by the PRTE processes that it
     forks, making them unkillable by SIGTERM). */
    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);

    /* exec the daemon */
    var = prte_argv_join(argv, ' ');
    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: executing: (%s) [%s]",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         exec_path, (NULL == var) ? "NULL" : var));
    if (NULL != var) free(var);

    execve(exec_path, exec_argv, env);
    prte_output(0, "plm:rsh: execv of %s failed with errno=%s(%d)\n",
                exec_path, strerror(errno), errno);
    exit(-1);
}

/*
 * launch a set of daemons from a remote daemon
 */
static int remote_spawn(void)
{
    int node_name_index1;
    int proc_vpid_index;
    char **argv = NULL;
    char *prefix, *hostname, *var;
    int argc;
    int rc=PRTE_SUCCESS;
    bool failed_launch = true;
    prte_process_name_t target;
    prte_plm_rsh_caddy_t *caddy;
    prte_job_t *daemons;
    prte_list_t coll;
    prte_namelist_t *child;

    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: remote spawn called",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* if we hit any errors, tell the HNP it was us */
    target.vpid = PRTE_PROC_MY_NAME->vpid;

    /* check to see if enable-prun-prefix-by-default was given - if
     * this is being done by a singleton, then prun will not be there
     * to put the prefix in the app. So make sure we check to find it */
    if ((bool)PRTE_WANT_PRTE_PREFIX_BY_DEFAULT) {
        prefix = strdup(prte_install_dirs.prefix);
    } else {
        prefix = NULL;
    }

    /* get the updated routing list */
    PRTE_CONSTRUCT(&coll, prte_list_t);
    prte_routed.get_routing_list(&coll);

    /* if I have no children, just return */
    if (0 == prte_list_get_size(&coll)) {
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:rsh: remote spawn - have no children!",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        failed_launch = false;
        rc = PRTE_SUCCESS;
        PRTE_DESTRUCT(&coll);
        goto cleanup;
    }

    /* setup the launch */
    if (PRTE_SUCCESS != (rc = setup_launch(&argc, &argv,
                                           prte_process_info.nodename, &node_name_index1,
                                           &proc_vpid_index, prefix))) {
        PRTE_ERROR_LOG(rc);
        PRTE_DESTRUCT(&coll);
        goto cleanup;
    }

    /* get the daemon job object */
    if (NULL == (daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->jobid))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        PRTE_DESTRUCT(&coll);
        goto cleanup;
    }

    target.jobid = PRTE_PROC_MY_NAME->jobid;
    PRTE_LIST_FOREACH(child, &coll, prte_namelist_t) {
        target.vpid = child->name.vpid;

        /* get the host where this daemon resides */
        if (NULL == (hostname = prte_get_proc_hostname(&target))) {
            prte_output(0, "%s unable to get hostname for daemon %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_VPID_PRINT(child->name.vpid));
            rc = PRTE_ERR_NOT_FOUND;
            PRTE_DESTRUCT(&coll);
            goto cleanup;
        }

        free(argv[node_name_index1]);
        argv[node_name_index1] = strdup(hostname);

        /* pass the vpid */
        rc = prte_util_convert_vpid_to_string(&var, target.vpid);
        if (PRTE_SUCCESS != rc) {
            prte_output(0, "prte_plm_rsh: unable to get daemon vpid as string");
            exit(-1);
        }
        free(argv[proc_vpid_index]);
        argv[proc_vpid_index] = strdup(var);
        free(var);

        /* we are in an event, so no need to protect the list */
        caddy = PRTE_NEW(prte_plm_rsh_caddy_t);
        caddy->argc = argc;
        caddy->argv = prte_argv_copy(argv);
        /* fake a proc structure for the new daemon - will be released
         * upon startup
         */
        caddy->daemon = PRTE_NEW(prte_proc_t);
        caddy->daemon->name.jobid = PRTE_PROC_MY_NAME->jobid;
        caddy->daemon->name.vpid = target.vpid;
        prte_list_append(&launch_list, &caddy->super);
    }
    PRTE_LIST_DESTRUCT(&coll);
    /* we NEVER use tree-spawn for secondary launches - e.g.,
     * due to a dynamic launch requesting add_hosts - so be
     * sure to turn it off here */
    prte_plm_rsh_component.no_tree_spawn = true;

    /* trigger the event to start processing the launch list */
    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: activating launch event",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    prte_event_active(&launch_event, EV_WRITE, 1);

    /* declare the launch a success */
    failed_launch = false;

cleanup:
    if (NULL != argv) {
        prte_argv_free(argv);
    }

    /* check for failed launch */
    if (failed_launch) {
        /* report cannot launch this daemon to HNP */
        prte_buffer_t *buf;
        buf = PRTE_NEW(prte_buffer_t);
        prte_dss.pack(buf, &target.vpid, 1, PRTE_VPID);
        prte_dss.pack(buf, &rc, 1, PRTE_INT);
        prte_rml.send_buffer_nb(PRTE_PROC_MY_HNP, buf,
                                PRTE_RML_TAG_REPORT_REMOTE_LAUNCH,
                                prte_rml_send_callback, NULL);
    }

    return rc;
}

/*
 * Launch a daemon (bootproxy) on each node. The daemon will be responsible
 * for launching the application.
 */

static int rsh_launch(prte_job_t *jdata)
{
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_RESTART)) {
        /* this is a restart situation - skip to the mapping stage */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP);
    } else {
        /* new job - set it up */
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_INIT);
    }
    return PRTE_SUCCESS;
}

static void process_launch_list(int fd, short args, void *cbdata)
{
    prte_list_item_t *item;
    pid_t pid;
    prte_plm_rsh_caddy_t *caddy;

    PRTE_ACQUIRE_OBJECT(caddy);

    while (num_in_progress < prte_plm_rsh_component.num_concurrent) {
        item = prte_list_remove_first(&launch_list);
        if (NULL == item) {
            /* we are done */
            break;
        }
        caddy = (prte_plm_rsh_caddy_t*)item;
        /* register the sigchild callback */
        PRTE_FLAG_SET(caddy->daemon, PRTE_PROC_FLAG_ALIVE);
        prte_wait_cb(caddy->daemon, rsh_wait_daemon, prte_event_base, (void*)caddy);

        /* fork a child to exec the rsh/ssh session */
        pid = fork();
        if (pid < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_CHILDREN);
            prte_wait_cb_cancel(caddy->daemon);
            continue;
        }

        /* child */
        if (pid == 0) {
            /*
             * When the user presses CTRL-C, SIGINT is sent to the whole process
             * group which terminates the rsh/ssh command. This can cause the
             * remote daemon to crash with a SIGPIPE when it tried to print out
             * status information. This has two concequences:
             * 1) The remote node is not cleaned up as it should. The local
             *    processes will notice that the orted failed and cleanup their
             *    part of the session directory, but the job level part will
             *    remain littered.
             * 2) Any debugging information we expected to see from the orted
             *    during shutdown is lost.
             *
             * The solution here is to put the child processes in a separate
             * process group from the HNP. So when the user presses CTRL-C
             * then only the HNP receives the signal, and not the rsh/ssh
             * child processes.
             */
#if HAVE_SETPGID
            if( 0 != setpgid(0, 0) ) {
                prte_output(0, "plm:rsh: Error: setpgid(0,0) failed in child with errno=%s(%d)\n",
                            strerror(errno), errno);
                exit(-1);
            }
#endif

            /* do the ssh launch - this will exit if it fails */
            ssh_child(caddy->argc, caddy->argv);
        } else { /* father */
            // Put the child in a separate progress group
            // - see comment in child section.
#if HAVE_SETPGID
            if( 0 != setpgid(pid, pid) ) {
                prte_output(0, "plm:rsh: Warning: setpgid(%ld,%ld) failed in parent with errno=%s(%d)\n",
                            (long)pid, (long)pid, strerror(errno), errno);
                // Ignore this error since the child is off and running.
                // We still need to track it.
            }
#endif

            /* indicate this daemon has been launched */
            caddy->daemon->state = PRTE_PROC_STATE_RUNNING;
            /* record the pid of the ssh fork */
            caddy->daemon->pid = pid;

            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:rsh: recording launch of daemon %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&(caddy->daemon->name))));
            num_in_progress++;
        }
    }
}

static void launch_daemons(int fd, short args, void *cbdata)
{
    prte_job_map_t *map = NULL;
    int node_name_index1;
    int proc_vpid_index;
    char **argv = NULL;
    char *prefix_dir=NULL, *var;
    int argc;
    int rc;
    prte_app_context_t *app;
    prte_node_t *node, *nd;
    int32_t nnode;
    prte_job_t *daemons;
    prte_state_caddy_t *state = (prte_state_caddy_t*)cbdata;
    prte_plm_rsh_caddy_t *caddy;
    prte_list_t coll;
    char *username;
    int port, *portptr;
    prte_namelist_t *child;

    PRTE_ACQUIRE_OBJECT(state);

    /* if we are launching debugger daemons, then just go
     * do it - no new daemons will be launched
     */
    if (PRTE_FLAG_TEST(state->jdata, PRTE_JOB_FLAG_DEBUGGER_DAEMON)) {
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PRTE_RELEASE(state);
        return;
    }

    /* setup the virtual machine */
    daemons = prte_get_job_data_object(PRTE_PROC_MY_NAME->jobid);
    if (PRTE_SUCCESS != (rc = prte_plm_base_setup_virtual_machine(state->jdata))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if we don't want to launch, then don't attempt to
     * launch the daemons - the user really wants to just
     * look at the proposed process map
     */
    if (prte_do_not_launch) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PRTE_RELEASE(state);
        return;
    }

    /* Get the map for this job */
    if (NULL == (map = daemons->map)) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (0 == map->num_new_daemons) {
        /* set the state to indicate the daemons reported - this
         * will trigger the daemons_reported event and cause the
         * job to move to the following step
         */
        state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;
        PRTE_ACTIVATE_JOB_STATE(state->jdata, PRTE_JOB_STATE_DAEMONS_REPORTED);
        PRTE_RELEASE(state);
        return;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: launching vm",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if ((0 < prte_output_get_verbosity(prte_plm_base_framework.framework_output) ||
         prte_leave_session_attached) &&
        prte_plm_rsh_component.num_concurrent < map->num_new_daemons) {
        /**
         * If we are in '--debug-daemons' we keep the ssh connection
         * alive for the span of the run. If we use this option
         * AND we launch on more than "num_concurrent" machines
         * then we will deadlock. No connections are terminated
         * until the job is complete, no job is started
         * since all the orteds are waiting for all the others
         * to come online, and the others ore not launched because
         * we are waiting on those that have started to terminate
         * their ssh tunnels. :(
         * As we cannot run in this situation, pretty print the error
         * and return an error code.
         */
        prte_show_help("help-plm-rsh.txt", "deadlock-params",
                       true, prte_plm_rsh_component.num_concurrent, map->num_new_daemons);
        PRTE_ERROR_LOG(PRTE_ERR_FATAL);
        rc = PRTE_ERR_SILENT;
        goto cleanup;
    }

    /*
     * After a discussion between Ralph & Jeff, we concluded that we
     * really are handling the prefix dir option incorrectly. It currently
     * is associated with an app_context, yet it really refers to the
     * location where PRTE is installed on a NODE. Fixing
     * this right now would involve significant change to prun as well
     * as elsewhere, so we will intentionally leave this incorrect at this
     * point. The error, however, is identical to that seen in all prior
     * releases of PRTE, so our behavior is no worse than before.
     *
     * A note to fix this, along with ideas on how to do so, has been filed
     * on the project's Trac system under "feature enhancement".
     *
     * For now, default to the prefix_dir provided in the first app_context.
     * Since there always MUST be at least one app_context, we are safe in
     * doing this.
     */
    app = (prte_app_context_t*)prte_pointer_array_get_item(state->jdata->apps, 0);
    if (NULL == app) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (!prte_get_attribute(&app->attributes, PRTE_APP_PREFIX_DIR, (void**)&prefix_dir, PRTE_STRING)) {
        /* check to see if enable-prun-prefix-by-default was given - if
         * this is being done by a singleton, then prun will not be there
         * to put the prefix in the app. So make sure we check to find it */
        if ((bool)PRTE_WANT_PRTE_PREFIX_BY_DEFAULT) {
            prefix_dir = strdup(prte_install_dirs.prefix);
        }
    }
    /* we also need at least one node name so we can check what shell is
     * being used, if we have to
     */
    node = NULL;
    for (nnode = 0; nnode < map->nodes->size; nnode++) {
        if (NULL != (nd = (prte_node_t*)prte_pointer_array_get_item(map->nodes, nnode))) {
            node = nd;
            /* if the node is me, then we continue - we would
             * prefer to find some other node so we can tell what the remote
             * shell is, if necessary
             */
            if (0 != strcmp(node->name, prte_process_info.nodename)) {
                break;
            }
        }
    }
    if (NULL == node) {
        /* this should be impossible, but adding the check will
         * silence code checkers that don't know better */
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        rc = PRTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* if we are tree launching, find our children and create the launch cmd */
    if (!prte_plm_rsh_component.no_tree_spawn) {
        prte_job_t *jdatorted;

        /* get the orted job data object */
        if (NULL == (jdatorted = prte_get_job_data_object(PRTE_PROC_MY_NAME->jobid))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            goto cleanup;
        }

        /* get the updated routing list */
        PRTE_CONSTRUCT(&coll, prte_list_t);
        prte_routed.get_routing_list(&coll);
    }

    /* setup the launch */
    if (PRTE_SUCCESS != (rc = setup_launch(&argc, &argv, node->name, &node_name_index1,
                                           &proc_vpid_index, prefix_dir))) {
        PRTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /*
     * Iterate through each of the nodes
     */
    for (nnode=0; nnode < map->nodes->size; nnode++) {
        if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(map->nodes, nnode))) {
            continue;
        }

        /* if we are tree launching, only launch our own children */
        if (!prte_plm_rsh_component.no_tree_spawn) {
            PRTE_LIST_FOREACH(child, &coll, prte_namelist_t) {
                if (child->name.vpid == node->daemon->name.vpid) {
                    goto launch;
                }
            }
            /* didn't find it - ignore this node */
            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:rsh:launch daemon %s not a child of mine",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_VPID_PRINT(node->daemon->name.vpid)));
            continue;
        }

    launch:
        /* if this daemon already exists, don't launch it! */
        if (PRTE_FLAG_TEST(node, PRTE_NODE_FLAG_DAEMON_LAUNCHED)) {
            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:rsh:launch daemon already exists on node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 node->name));
            continue;
        }

        /* if the node's daemon has not been defined, then we
         * have an error!
         */
        if (NULL == node->daemon) {
            PRTE_ERROR_LOG(PRTE_ERR_FATAL);
            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:rsh:launch daemon failed to be defined on node %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 node->name));
            continue;
        }

        /* setup node name */
        free(argv[node_name_index1]);
        username = NULL;
        if (prte_get_attribute(&node->attributes, PRTE_NODE_USERNAME, (void**)&username, PRTE_STRING)) {
            prte_asprintf (&argv[node_name_index1], "%s@%s",
                            username, node->name);
            free(username);
        } else {
            argv[node_name_index1] = strdup(node->name);
        }

        /* pass the vpid */
        rc = prte_util_convert_vpid_to_string(&var, node->daemon->name.vpid);
        if (PRTE_SUCCESS != rc) {
            prte_output(0, "prte_plm_rsh: unable to get daemon vpid as string");
            exit(-1);
        }
        free(argv[proc_vpid_index]);
        argv[proc_vpid_index] = strdup(var);
        free(var);

        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:rsh: adding node %s to launch list",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             node->name));

        /* we are in an event, so no need to protect the list */
        caddy = PRTE_NEW(prte_plm_rsh_caddy_t);
        caddy->argc = argc;
        caddy->argv = prte_argv_copy(argv);
        /* insert the alternate port if any */
        portptr = &port;
        if (prte_get_attribute(&node->attributes, PRTE_NODE_PORT, (void**)&portptr, PRTE_INT)) {
            char portname[16];
            /* for the sake of simplicity, insert "-p" <port> in the duplicated argv */
            prte_argv_insert_element(&caddy->argv, node_name_index1+1, "-p");
            snprintf (portname, 15, "%d", port);
            prte_argv_insert_element(&caddy->argv, node_name_index1+2, portname);
        }
        caddy->daemon = node->daemon;
        PRTE_RETAIN(caddy->daemon);
        prte_list_append(&launch_list, &caddy->super);
    }
    /* we NEVER use tree-spawn for secondary launches - e.g.,
     * due to a dynamic launch requesting add_hosts - so be
     * sure to turn it off here */
    prte_plm_rsh_component.no_tree_spawn = true;

    /* set the job state to indicate the daemons are launched */
    state->jdata->state = PRTE_JOB_STATE_DAEMONS_LAUNCHED;

    /* trigger the event to start processing the launch list */
    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: activating launch event",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    PRTE_POST_OBJECT(state);
    prte_event_active(&launch_event, EV_WRITE, 1);

    /* now that we've launched the daemons, let the daemon callback
     * function determine they are all alive and trigger the next stage
     */
    PRTE_RELEASE(state);
    prte_argv_free(argv);
    return;

 cleanup:
    PRTE_RELEASE(state);
    PRTE_FORCED_TERMINATE(PRTE_ERROR_DEFAULT_EXIT_CODE);
}

/**
 * Terminate the orteds for a given job
 */
static int rsh_terminate_prteds(void)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_plm_base_prted_exit(PRTE_DAEMON_EXIT_CMD))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

static int rsh_finalize(void)
{
    int rc, i;
    prte_job_t *jdata;
    prte_proc_t *proc;
    pid_t ret;

    /* remove launch event */
    prte_event_del(&launch_event);
    PRTE_LIST_DESTRUCT(&launch_list);

    /* cleanup any pending recvs */
    if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
        PRTE_ERROR_LOG(rc);
    }

    if ((PRTE_PROC_IS_DAEMON || PRTE_PROC_IS_MASTER) && prte_abnormal_term_ordered) {
        /* ensure that any lingering ssh's are gone */
        if (NULL == (jdata = prte_get_job_data_object(PRTE_PROC_MY_NAME->jobid))) {
            return rc;
        }
        for (i=0; i < jdata->procs->size; i++) {
            if (NULL == (proc = prte_pointer_array_get_item(jdata->procs, i))) {
                continue;
            }
            if (0 < proc->pid) {
                /* this is a daemon we started - see if the ssh process still exists */
                ret = waitpid(proc->pid, &proc->exit_code, WNOHANG);
                if (-1 == ret && ECHILD == errno) {
                    /* The pid no longer exists, so we'll call this "good
                       enough for government work" */
                    continue;
                }
                if (ret == proc->pid) {
                    /* already died */
                    continue;
                }
                /* ssh session must still be alive, so kill it */
                kill(proc->pid, SIGKILL);
            }
        }
    }
    free(prte_plm_rsh_component.agent_path);
    free(rsh_agent_path);
    prte_argv_free(prte_plm_rsh_component.agent_argv);
    prte_argv_free(rsh_agent_argv);

    return rc;
}


static void set_handler_default(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(sig, &act, (struct sigaction *)0);
}


static prte_plm_rsh_shell_t find_shell(char *shell)
{
    int i         = 0;
    char *sh_name = NULL;

    if( (NULL == shell) || (strlen(shell) == 1) ) {
        /* Malformed shell */
        return PRTE_PLM_RSH_SHELL_UNKNOWN;
    }

    sh_name = rindex(shell, '/');
    if( NULL == sh_name ) {
        /* Malformed shell */
        return PRTE_PLM_RSH_SHELL_UNKNOWN;
    }

    /* skip the '/' */
    ++sh_name;
    for (i = 0; i < (int)(sizeof (prte_plm_rsh_shell_name) /
                          sizeof(prte_plm_rsh_shell_name[0])); ++i) {
        if (NULL != strstr(sh_name, prte_plm_rsh_shell_name[i])) {
            return (prte_plm_rsh_shell_t)i;
        }
    }

    /* We didn't find it */
    return PRTE_PLM_RSH_SHELL_UNKNOWN;
}

static int launch_agent_setup(const char *agent, char *path)
{
    char *bname;
    int i;

    /* if no agent was provided, then report not found */
    if (NULL == prte_plm_rsh_component.agent && NULL == agent) {
        return PRTE_ERR_NOT_FOUND;
    }

    /* search for the argv */
    PRTE_OUTPUT_VERBOSE((5, prte_plm_base_framework.framework_output,
                         "%s plm:rsh_setup on agent %s path %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == agent) ? prte_plm_rsh_component.agent : agent,
                         (NULL == path) ? "NULL" : path));
    rsh_agent_argv = prte_plm_rsh_search(agent, path);

    if (0 == prte_argv_count(rsh_agent_argv)) {
        /* nothing was found */
        return PRTE_ERR_NOT_FOUND;
    }

    /* see if we can find the agent in the path */
    rsh_agent_path = prte_path_findv(rsh_agent_argv[0], X_OK, environ, path);

    if (NULL == rsh_agent_path) {
        /* not an error - just report not found */
        prte_argv_free(rsh_agent_argv);
        return PRTE_ERR_NOT_FOUND;
    }

    bname = prte_basename(rsh_agent_argv[0]);
    if (NULL != bname && 0 == strcmp(bname, "ssh")) {
        /* if xterm option was given, add '-X', ensuring we don't do it twice */
        if (NULL != prte_xterm) {
            prte_argv_append_unique_nosize(&rsh_agent_argv, "-X");
        } else if (0 >= prte_output_get_verbosity(prte_plm_base_framework.framework_output)) {
            /* if debug was not specified, and the user didn't explicitly
             * specify X11 forwarding/non-forwarding, add "-x" if it
             * isn't already there (check either case)
             */
            for (i = 1; NULL != rsh_agent_argv[i]; ++i) {
                if (0 == strcasecmp("-x", rsh_agent_argv[i])) {
                    break;
                }
            }
            if (NULL == rsh_agent_argv[i]) {
                prte_argv_append_nosize(&rsh_agent_argv, "-x");
            }
        }
    }
    if (NULL != bname) {
        free(bname);
    }

    /* the caller can append any additional argv's they desire */
    return PRTE_SUCCESS;
}

/**
 * Check the Shell variable and system type on the specified node
 */
static int rsh_probe(char *nodename,
                     prte_plm_rsh_shell_t *shell)
{
    char ** argv;
    int argc, rc = PRTE_SUCCESS, i;
    int fd[2];
    pid_t pid;
    char outbuf[4096];

    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: going to check SHELL variable on node %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         nodename));

    *shell = PRTE_PLM_RSH_SHELL_UNKNOWN;
    if (pipe(fd)) {
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:rsh: pipe failed with errno=%d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             errno));
        return PRTE_ERR_IN_ERRNO;
    }
    if ((pid = fork()) < 0) {
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:rsh: fork failed with errno=%d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             errno));
        return PRTE_ERR_IN_ERRNO;
    }
    else if (pid == 0) {          /* child */
        if (dup2(fd[1], 1) < 0) {
            PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                 "%s plm:rsh: dup2 failed with errno=%d",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 errno));
            exit(01);
        }
        /* Build argv array */
        argv = prte_argv_copy(prte_plm_rsh_component.agent_argv);
        argc = prte_argv_count(prte_plm_rsh_component.agent_argv);
        prte_argv_append(&argc, &argv, nodename);
        prte_argv_append(&argc, &argv, "echo $SHELL");

        execvp(argv[0], argv);
        exit(errno);
    }
    if (close(fd[1])) {
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:rsh: close failed with errno=%d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             errno));
        return PRTE_ERR_IN_ERRNO;
    }

    {
        ssize_t ret = 1;
        char* ptr = outbuf;
        size_t outbufsize = sizeof(outbuf);

        do {
            ret = read (fd[0], ptr, outbufsize-1);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                                     "%s plm:rsh: Unable to detect the remote shell (error %s)",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                     strerror(errno)));
                rc = PRTE_ERR_IN_ERRNO;
                break;
            }
            if( outbufsize > 1 ) {
                outbufsize -= ret;
                ptr += ret;
            }
        } while( 0 != ret );
        *ptr = '\0';
    }
    close(fd[0]);

    if( outbuf[0] != '\0' ) {
        char *sh_name = rindex(outbuf, '/');
        if( NULL != sh_name ) {
            sh_name++; /* skip '/' */
            /* Search for the substring of known shell-names */
            for (i = 0; i < (int)(sizeof (prte_plm_rsh_shell_name)/
                                  sizeof(prte_plm_rsh_shell_name[0])); i++) {
                if ( NULL != strstr(sh_name, prte_plm_rsh_shell_name[i]) ) {
                    *shell = (prte_plm_rsh_shell_t)i;
                    break;
                }
            }
        }
    }

    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: node %s has SHELL: %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         nodename,
                         (PRTE_PLM_RSH_SHELL_UNKNOWN == *shell) ? "UNHANDLED" : (char*)prte_plm_rsh_shell_name[*shell]));

    return rc;
}

static int setup_shell(prte_plm_rsh_shell_t *rshell,
                       prte_plm_rsh_shell_t *lshell,
                       char *nodename, int *argc, char ***argv)
{
    prte_plm_rsh_shell_t remote_shell, local_shell;
    char *param;
    int rc;

    /* What is our local shell? */
    local_shell = PRTE_PLM_RSH_SHELL_UNKNOWN;

#if PRTE_ENABLE_GETPWUID
    {
        struct passwd *p;

        p = getpwuid(getuid());
        if( NULL != p ) {
            param = p->pw_shell;
            local_shell = find_shell(p->pw_shell);
        }
    }
#endif

    /* If we didn't find it in getpwuid(), try looking at the $SHELL
       environment variable (see https://svn.open-mpi.org/trac/ompi/ticket/1060)
    */
    if (PRTE_PLM_RSH_SHELL_UNKNOWN == local_shell &&
        NULL != (param = getenv("SHELL"))) {
        local_shell = find_shell(param);
    }

    if (PRTE_PLM_RSH_SHELL_UNKNOWN == local_shell) {
        prte_output(0, "WARNING: local probe returned unhandled shell:%s assuming bash\n",
                    (NULL != param) ? param : "unknown");
        local_shell = PRTE_PLM_RSH_SHELL_BASH;
    }

    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: local shell: %d (%s)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         local_shell, prte_plm_rsh_shell_name[local_shell]));

    /* What is our remote shell? */
    if (prte_plm_rsh_component.assume_same_shell) {
        remote_shell = local_shell;
        PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:rsh: assuming same remote shell as local shell",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    } else {
        rc = rsh_probe(nodename, &remote_shell);

        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        if (PRTE_PLM_RSH_SHELL_UNKNOWN == remote_shell) {
            prte_output(0, "WARNING: rsh probe returned unhandled shell; assuming bash\n");
            remote_shell = PRTE_PLM_RSH_SHELL_BASH;
        }
    }

    PRTE_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:rsh: remote shell: %d (%s)",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         remote_shell, prte_plm_rsh_shell_name[remote_shell]));

    /* Do we need to source .profile on the remote side?
       - sh: yes (see bash(1))
       - ksh: yes (see ksh(1))
       - bash: no (see bash(1))
       - [t]csh: no (see csh(1) and tcsh(1))
       - zsh: no (see http://zsh.sourceforge.net/FAQ/zshfaq03.html#l19)
    */

    if (PRTE_PLM_RSH_SHELL_SH == remote_shell ||
        PRTE_PLM_RSH_SHELL_KSH == remote_shell) {
        int i;
        char **tmp;
        tmp = prte_argv_split("( test ! -r ./.profile || . ./.profile;", ' ');
        if (NULL == tmp) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        for (i = 0; NULL != tmp[i]; ++i) {
            prte_argv_append(argc, argv, tmp[i]);
        }
        prte_argv_free(tmp);
    }

    /* pass results back */
    *rshell = remote_shell;
    *lshell = local_shell;

    return PRTE_SUCCESS;
}
