#
#  Makefile for the Squid Object Cache server
#
#  $Id: Makefile.am,v 1.3 2009/08/16 23:32:40 hno Exp $
#

libexec_PROGRAMS = squid_kerb_auth

SPNEGO = spnegohelp/derparse.c spnegohelp/derparse.h spnegohelp/Makefile spnegohelp/spnego.c spnegohelp/spnego.h spnegohelp/spnegohelp.c spnegohelp/spnegohelp.h spnegohelp/spnegoparse.c spnegohelp/spnegoparse.h
SOURCE = squid_kerb_auth.c base64.c base64.h
EXTRA_DIST = readme.txt do.sh

if HAVE_SPNEGO
squid_kerb_auth_SOURCES = $(SOURCE)
else
squid_kerb_auth_SOURCES = $(SOURCE) $(SPNEGO)
endif

AM_CPPFLAGS = $(KERBINCS) -I$(srcdir)/spnegohelp -I.  -I$(top_srcdir)/include
#-I$(top_srcdir)/src
LDADD = $(KERBLIBS)
#-L$(top_builddir)/lib -lmiscutil $(XTRA_LIBS)

# MIT
KERBINCS = @KRB5INCS@
KERBLIBS = @KRB5LIBS@
