# -*- shell-script -*-
#
# Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AC_DEFUN([MCA_prte_prteif_bsdx_ipv6_COMPILE_MODE], [
    AC_MSG_CHECKING([for MCA component $1:$2 compile mode])
    $3="static"
    AC_MSG_RESULT([$$3])
])

# MCA_prteif_config_CONFIG(action-if-can-compile,
#                        [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_prte_prteif_bsdx_ipv6_CONFIG], [
    AC_CONFIG_FILES([src/mca/prteif/bsdx_ipv6/Makefile])

    AC_REQUIRE([PRTE_CHECK_OS_FLAVORS])

    # If we found struct sockaddr and we're on any of the BSDs, we're
    # happy.  I.e., this:
    #if defined( __NetBSD__) || defined(__OpenBSD__) || defined(__FreeBSD__) || \
    #             defined(__386BSD__) || defined(__bsdi__) || defined(__APPLE__)
    AC_MSG_CHECKING([struct sockaddr])
    AS_IF([test "$prte_found_sockaddr" = "yes"],
          [AC_MSG_RESULT([yes (cached)])
           AC_MSG_CHECKING([some flavor of BSD])
           AS_IF([test "$prte_found_netbsd" = "yes" || \
                  test "$prte_found_freebsd" = "yes" || \
                  test "$prte_found_openbsd" = "yes" || \
                  test "$prte_found_386bsd" = "yes" || \
                  test "$prte_found_bsdi" = "yes" ||
                  test "$prte_found_apple" = "yes"],
                 [AC_MSG_RESULT([yes])
                  $1],
                 [AC_MSG_RESULT([no])
                  $2])],
          [AC_MSG_RESULT([no (cached)])
           $2])
])dnl
