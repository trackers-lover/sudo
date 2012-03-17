/*
 * Copyright (c) 1994-1996,1998-2011 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#ifdef _AIX
# include <sys/id.h>
#endif
#include <pwd.h>
#include <errno.h>
#include <grp.h>

#include "sudoers.h"

/*
 * Prototypes
 */
#if defined(HAVE_SETRESUID) || defined(HAVE_SETREUID) || defined(HAVE_SETEUID)
static struct group_list *runas_setgroups(void);
#endif

/*
 * We keep track of the current permisstions and use a stack to restore
 * the old permissions.  A depth of 16 is overkill.
 */
struct perm_state {
    uid_t ruid;
    uid_t euid;
#if defined(HAVE_SETRESUID) || defined(ID_SAVED)
    uid_t suid;
#endif
    gid_t rgid;
    gid_t egid;
#if defined(HAVE_SETRESUID) || defined(ID_SAVED)
    gid_t sgid;
#endif
    struct group_list *grlist;
};

#define PERM_STACK_MAX	16
static struct perm_state perm_stack[PERM_STACK_MAX];
static int perm_stack_depth = 0;

#undef ID
#define ID(x) (state->x == ostate->x ? -1 : state->x)
#undef OID
#define OID(x) (ostate->x == state->x ? -1 : ostate->x)

void
rewind_perms(void)
{
    debug_decl(rewind_perms, SUDO_DEBUG_PERMS)

    while (perm_stack_depth > 1)
	restore_perms();
    grlist_delref(perm_stack[0].grlist);

    debug_return;
}

#if defined(HAVE_SETRESUID)

#define UID_CHANGED (state->ruid != ostate->ruid || state->euid != ostate->euid || state->suid != ostate->suid)
#define GID_CHANGED (state->rgid != ostate->rgid || state->egid != ostate->egid || state->sgid != ostate->sgid)

/*
 * Set real and effective and saved uids and gids based on perm.
 * We always retain a saved uid of 0 unless we are headed for an exec().
 * We only flip the effective gid since it only changes for PERM_SUDOERS.
 * This version of set_perms() works fine with the "stay_setuid" option.
 */
int
set_perms(int perm)
{
    struct perm_state *state, *ostate = NULL;
    char errbuf[1024];
    int noexit;
    debug_decl(set_perms, SUDO_DEBUG_PERMS)

    noexit = ISSET(perm, PERM_NOEXIT);
    CLR(perm, PERM_MASK);

    if (perm_stack_depth == PERM_STACK_MAX) {
	strlcpy(errbuf, _("perm stack overflow"), sizeof(errbuf));
	errno = EINVAL;
	goto bad;
    }

    state = &perm_stack[perm_stack_depth];
    if (perm != PERM_INITIAL) {
	if (perm_stack_depth == 0) {
	    strlcpy(errbuf, _("perm stack underflow"), sizeof(errbuf));
	    errno = EINVAL;
	    goto bad;
	}
	ostate = &perm_stack[perm_stack_depth - 1];
    }

    switch (perm) {
    case PERM_INITIAL:
	/* Stash initial state */
#ifdef HAVE_GETRESUID
	if (getresuid(&state->ruid, &state->euid, &state->suid)) {
	    strlcpy(errbuf, "PERM_INITIAL: getresuid", sizeof(errbuf));
	    goto bad;

	}
	if (getresgid(&state->rgid, &state->egid, &state->sgid)) {
	    strlcpy(errbuf, "PERM_INITIAL: getresgid", sizeof(errbuf));
	    goto bad;
	}
#else
	state->ruid = getuid();
	state->euid = geteuid();
	state->suid = state->euid; /* in case we are setuid */

	state->rgid = getgid();
	state->egid = getegid();
	state->sgid = state->egid; /* in case we are setgid */
#endif
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_INITIAL: "
	    "ruid: %d, euid: %d, suid: %d, rgid: %d, egid: %d, sgid: %d",
	    __func__, (int)state->ruid, (int)state->euid, (int)state->suid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	break;

    case PERM_ROOT:
	state->ruid = ROOT_UID;
	state->euid = ROOT_UID;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setresuid(ID(ruid), ID(euid), ID(suid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_ROOT: setresuid(%d, %d, %d)",
		ID(ruid), ID(euid), ID(suid));
	    goto bad;
	}
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->sgid = ostate->sgid;
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	break;

    case PERM_USER:
	state->rgid = ostate->rgid;
	state->egid = user_gid;
	state->sgid = ostate->sgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setresgid(ID(rgid), ID(egid), ID(sgid))) {
	    snprintf(errbuf, sizeof(errbuf), "PERM_USER: setresgid(%d, %d, %d)",
		ID(rgid), ID(egid), ID(sgid));
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	state->euid = user_uid;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setresuid(ID(ruid), ID(euid), ID(suid))) {
	    snprintf(errbuf, sizeof(errbuf), "PERM_USER: setresuid(%d, %d, %d)",
		ID(ruid), ID(euid), ID(suid));
	    goto bad;
	}
	break;

    case PERM_FULL_USER:
	/* headed for exec() */
	state->rgid = user_gid;
	state->egid = user_gid;
	state->sgid = user_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setresgid(ID(rgid), ID(egid), ID(sgid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setresgid(%d, %d, %d)",
		ID(rgid), ID(egid), ID(sgid));
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_FULL_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	state->euid = user_uid;
	state->suid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setresuid(ID(ruid), ID(euid), ID(suid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setresuid(%d, %d, %d)",
		ID(ruid), ID(euid), ID(suid));
	    goto bad;
	}
	break;

    case PERM_RUNAS:
	state->rgid = ostate->rgid;
	state->egid = runas_gr ? runas_gr->gr_gid : runas_pw->pw_gid;
	state->sgid = ostate->sgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setresgid(ID(rgid), ID(egid), ID(sgid))) {
	    strlcpy(errbuf, _("unable to change to runas gid"), sizeof(errbuf));
	    goto bad;
	}
	state->grlist = runas_setgroups();
	state->ruid = ostate->ruid;
	state->euid = runas_pw ? runas_pw->pw_uid : user_uid;
	state->suid = ostate->suid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setresuid(ID(ruid), ID(euid), ID(suid))) {
	    strlcpy(errbuf, _("unable to change to runas uid"), sizeof(errbuf));
	    goto bad;
	}
	break;

    case PERM_SUDOERS:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);

	/* assumes euid == ROOT_UID, ruid == user */
	state->rgid = ostate->rgid;
	state->egid = sudoers_gid;
	state->sgid = ostate->sgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setresgid(ID(rgid), ID(egid), ID(sgid))) {
	    strlcpy(errbuf, _("unable to change to sudoers gid"), sizeof(errbuf));
	    goto bad;
	}

	state->ruid = ROOT_UID;
	/*
	 * If sudoers_uid == ROOT_UID and sudoers_mode is group readable
	 * we use a non-zero uid in order to avoid NFS lossage.
	 * Using uid 1 is a bit bogus but should work on all OS's.
	 */
	if (sudoers_uid == ROOT_UID && (sudoers_mode & 040))
	    state->euid = 1;
	else
	    state->euid = sudoers_uid;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setresuid(ID(ruid), ID(euid), ID(suid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_SUDOERS: setresuid(%d, %d, %d)",
		ID(ruid), ID(euid), ID(suid));
	    goto bad;
	}
	break;

    case PERM_TIMESTAMP:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->sgid = ostate->sgid;
	state->ruid = ROOT_UID;
	state->euid = timestamp_uid;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_TIMESTAMP: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setresuid(ID(ruid), ID(euid), ID(suid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_TIMESTAMP: setresuid(%d, %d, %d)",
		ID(ruid), ID(euid), ID(suid));
	    goto bad;
	}
	break;
    }

    perm_stack_depth++;
    debug_return_bool(1);
bad:
    warningx("%s: %s", errbuf,
	errno == EAGAIN ? _("too many processes") : strerror(errno));
    if (noexit)
	debug_return_bool(0);
    exit(1);
}

void
restore_perms(void)
{
    struct perm_state *state, *ostate;
    debug_decl(restore_perms, SUDO_DEBUG_PERMS)

    if (perm_stack_depth < 2)
	debug_return;

    state = &perm_stack[perm_stack_depth - 1];
    ostate = &perm_stack[perm_stack_depth - 2];
    perm_stack_depth--;

    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: uid: [%d, %d, %d] -> [%d, %d, %d]",
	__func__, (int)state->ruid, (int)state->euid, (int)state->suid,
	(int)ostate->ruid, (int)ostate->euid, (int)ostate->suid);
    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: gid: [%d, %d, %d] -> [%d, %d, %d]",
	__func__, (int)state->rgid, (int)state->egid, (int)state->sgid,
	(int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid);

    /* XXX - more cases here where euid != ruid */
    if (OID(euid) == ROOT_UID && state->euid != ROOT_UID) {
	if (setresuid(-1, ROOT_UID, -1)) {
	    warning("setresuid() [%d, %d, %d] -> [%d, %d, %d]",
		(int)state->ruid, (int)state->euid, (int)state->suid,
		-1, ROOT_UID, -1);
	    goto bad;
	}
    }
    if (setresuid(OID(ruid), OID(euid), OID(suid))) {
	warning("setresuid() [%d, %d, %d] -> [%d, %d, %d]",
	    (int)state->ruid, (int)state->euid, (int)state->suid,
	    (int)OID(ruid), (int)OID(euid), (int)OID(suid));
	goto bad;
    }
    if (setresgid(OID(rgid), OID(egid), OID(sgid))) {
	warning("setresgid() [%d, %d, %d] -> [%d, %d, %d]",
	    (int)state->rgid, (int)state->egid, (int)state->sgid,
	    (int)OID(rgid), (int)OID(egid), (int)OID(sgid));
	goto bad;
    }
    if (state->grlist != ostate->grlist) {
	if (sudo_setgroups(ostate->grlist->ngids, ostate->grlist->gids)) {
	    warning("setgroups()");
	    goto bad;
	}
    }
    grlist_delref(state->grlist);
    debug_return;

bad:
    exit(1);
}

#elif defined(_AIX) && defined(ID_SAVED)

#define UID_CHANGED (state->ruid != ostate->ruid || state->euid != ostate->euid || state->suid != ostate->suid)
#define GID_CHANGED (state->rgid != ostate->rgid || state->egid != ostate->egid || state->sgid != ostate->sgid)

/*
 * Set real and effective and saved uids and gids based on perm.
 * We always retain a saved uid of 0 unless we are headed for an exec().
 * We only flip the effective gid since it only changes for PERM_SUDOERS.
 * This version of set_perms() works fine with the "stay_setuid" option.
 */
int
set_perms(int perm)
{
    struct perm_state *state, *ostate = NULL;
    char errbuf[1024];
    int noexit;
    debug_decl(set_perms, SUDO_DEBUG_PERMS)

    noexit = ISSET(perm, PERM_NOEXIT);
    CLR(perm, PERM_MASK);

    if (perm_stack_depth == PERM_STACK_MAX) {
	strlcpy(errbuf, _("perm stack overflow"), sizeof(errbuf));
	errno = EINVAL;
	goto bad;
    }

    state = &perm_stack[perm_stack_depth];
    if (perm != PERM_INITIAL) {
	if (perm_stack_depth == 0) {
	    strlcpy(errbuf, _("perm stack underflow"), sizeof(errbuf));
	    errno = EINVAL;
	    goto bad;
	}
	ostate = &perm_stack[perm_stack_depth - 1];
    }

    switch (perm) {
    case PERM_INITIAL:
	/* Stash initial state */
	state->ruid = getuidx(ID_REAL);
	state->euid = getuidx(ID_EFFECTIVE);
	state->suid = getuidx(ID_SAVED);
	state->rgid = getgidx(ID_REAL);
	state->egid = getgidx(ID_EFFECTIVE);
	state->sgid = getgidx(ID_SAVED);
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_INITIAL: "
	    "ruid: %d, euid: %d, suid: %d, rgid: %d, egid: %d, sgid: %d",
	    __func__, (unsigned int)state->ruid, (unsigned int)state->euid,
	    (unsigned int)state->suid, (unsigned int)state->rgid,
	    (unsigned int)state->egid, (unsigned int)state->sgid);
	break;

    case PERM_ROOT:
	state->ruid = ROOT_UID;
	state->euid = ROOT_UID;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, ROOT_UID)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_ROOT: setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, %d)",
		ROOT_UID);
	    goto bad;
	}
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->sgid = ostate->sgid;
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	break;

    case PERM_USER:
	state->rgid = ostate->rgid;
	state->egid = user_gid;
	state->sgid = ostate->sgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setgidx(ID_EFFECTIVE, user_gid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_USER: setgidx(ID_EFFECTIVE, %d)", user_gid);
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	state->euid = user_uid;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (ostate->euid != ROOT_UID || ostate->suid != ROOT_UID) {
	    if (setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, ROOT_UID)) {
		snprintf(errbuf, sizeof(errbuf),
		    "PERM_USER: setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, %d)",
		    ROOT_UID);
		goto bad;
	    }
	}
	if (setuidx(ID_EFFECTIVE|ID_REAL, user_uid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_USER: setuidx(ID_EFFECTIVE|ID_REAL, %d)", user_uid);
	    goto bad;
	}
	break;

    case PERM_FULL_USER:
	/* headed for exec() */
	state->rgid = user_gid;
	state->egid = user_gid;
	state->sgid = user_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setgidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, user_gid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setgidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, %d)",
		user_gid);
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_FULL_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	state->euid = user_uid;
	state->suid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, user_uid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, %d)",
		user_uid);
	    goto bad;
	}
	break;

    case PERM_RUNAS:
	state->rgid = ostate->rgid;
	state->egid = runas_gr ? runas_gr->gr_gid : runas_pw->pw_gid;
	state->sgid = ostate->sgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setgidx(ID_EFFECTIVE, state->egid)) {
	    strlcpy(errbuf, _("unable to change to runas gid"), sizeof(errbuf));
	    goto bad;
	}
	state->grlist = runas_setgroups();
	state->ruid = ostate->ruid;
	state->euid = runas_pw ? runas_pw->pw_uid : user_uid;
	state->suid = ostate->suid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED && setuidx(ID_EFFECTIVE, state->euid)) {
	    strlcpy(errbuf, _("unable to change to runas uid"), sizeof(errbuf));
	    goto bad;
	}
	break;

    case PERM_SUDOERS:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);

	/* assume euid == ROOT_UID, ruid == user */
	state->rgid = ostate->rgid;
	state->egid = sudoers_gid;
	state->sgid = ostate->sgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: gid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid,
	    (int)state->rgid, (int)state->egid, (int)state->sgid);
	if (GID_CHANGED && setgidx(ID_EFFECTIVE, sudoers_gid)) {
	    strlcpy(errbuf, _("unable to change to sudoers gid"), sizeof(errbuf));
	    goto bad;
	}

	state->ruid = ROOT_UID;
	/*
	 * If sudoers_uid == ROOT_UID and sudoers_mode is group readable
	 * we use a non-zero uid in order to avoid NFS lossage.
	 * Using uid 1 is a bit bogus but should work on all OS's.
	 */
	if (sudoers_uid == ROOT_UID && (sudoers_mode & 040))
	    state->euid = 1;
	else
	    state->euid = sudoers_uid;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED) {
	    if (ostate->ruid != ROOT_UID || ostate->suid != ROOT_UID) {
		if (setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, ROOT_UID)) {
		    snprintf(errbuf, sizeof(errbuf),
			"PERM_SUDOERS: setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, %d)",
			ROOT_UID);
		    goto bad;
		}
	    }
	    if (setuidx(ID_EFFECTIVE, state->euid)) {
		snprintf(errbuf, sizeof(errbuf),
		    "PERM_SUDOERS: setuidx(ID_EFFECTIVE, %d)", sudoers_uid);
		goto bad;
	    }
	}
	break;

    case PERM_TIMESTAMP:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->sgid = ostate->sgid;
	state->ruid = ROOT_UID;
	state->euid = timestamp_uid;
	state->suid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_TIMESTAMP: uid: "
	    "[%d, %d, %d] -> [%d, %d, %d]", __func__,
	    (int)ostate->ruid, (int)ostate->euid, (int)ostate->suid,
	    (int)state->ruid, (int)state->euid, (int)state->suid);
	if (UID_CHANGED) {
	    if (ostate->ruid != ROOT_UID || ostate->suid != ROOT_UID) {
		if (setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, ROOT_UID)) {
		    snprintf(errbuf, sizeof(errbuf),
			"PERM_TIMESTAMP: setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, %d)",
			ROOT_UID);
		    goto bad;
		}
	    }
	    if (setuidx(ID_EFFECTIVE, timestamp_uid)) {
		snprintf(errbuf, sizeof(errbuf),
		    "PERM_TIMESTAMP: setuidx(ID_EFFECTIVE, %d)", timestamp_uid);
		goto bad;
	    }
	}
	break;
    }

    perm_stack_depth++;
    debug_return_bool(1);
bad:
    warningx("%s: %s", errbuf,
	errno == EAGAIN ? _("too many processes") : strerror(errno));
    if (noexit)
	debug_return_bool(0);
    exit(1);
}

void
restore_perms(void)
{
    struct perm_state *state, *ostate;
    debug_decl(restore_perms, SUDO_DEBUG_PERMS)

    if (perm_stack_depth < 2)
	debug_return;

    state = &perm_stack[perm_stack_depth - 1];
    ostate = &perm_stack[perm_stack_depth - 2];
    perm_stack_depth--;

    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: uid: [%d, %d, %d] -> [%d, %d, %d]",
	__func__, (int)state->ruid, (int)state->euid, (int)state->suid,
	(int)ostate->ruid, (int)ostate->euid, (int)ostate->suid);
    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: gid: [%d, %d, %d] -> [%d, %d, %d]",
	__func__, (int)state->rgid, (int)state->egid, (int)state->sgid,
	(int)ostate->rgid, (int)ostate->egid, (int)ostate->sgid);

    if (OID(ruid) != -1 && OID(euid) != -1 && OID(suid) != -1) {
	/* XXX - more cases here where euid != ruid */
	if (OID(euid) == ROOT_UID && state->euid != ROOT_UID) {
	    if (setuidx(ID_EFFECTIVE, ROOT_UID)) {
		warning("setuidx() [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->ruid, (int)state->euid, (int)state->suid,
		    -1, ROOT_UID, -1);
		goto bad;
	    }
	}
	if (OID(ruid) == OID(euid) && OID(euid) == OID(suid)) {
	    if (setuidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, OID(ruid))) {
		warning("setuidx() [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->ruid, (int)state->euid, (int)state->suid,
		    (int)OID(ruid), (int)OID(euid), (int)OID(suid));
		goto bad;
	    }
	} else if (OID(ruid) == -1 && OID(suid) == -1) {
	    if (setuidx(ID_EFFECTIVE, OID(euid))) {
		warning("setuidx(ID_EFFECTIVE) [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->ruid, (int)state->euid, (int)state->suid,
		    (int)OID(ruid), (int)OID(euid), (int)OID(suid));
		goto bad;
	    }
	} else if (OID(suid) == -1) {
	    if (setuidx(ID_REAL|ID_EFFECTIVE, OID(ruid))) {
		warning("setuidx(ID_REAL|ID_EFFECTIVE) [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->ruid, (int)state->euid, (int)state->suid,
		    (int)OID(ruid), (int)OID(euid), (int)OID(suid));
		goto bad;
	    }
	}
    }
    if (OID(rgid) != -1 && OID(egid) != -1 && OID(sgid) != -1) {
	if (OID(rgid) == OID(egid) && OID(egid) == OID(sgid)) {
	    if (setgidx(ID_EFFECTIVE|ID_REAL|ID_SAVED, OID(rgid))) {
		warning("setgidx() [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->rgid, (int)state->egid, (int)state->sgid,
		    (int)OID(rgid), (int)OID(egid), (int)OID(sgid));
		goto bad;
	    }
	} else if (OID(rgid) == -1 && OID(sgid) == -1) {
	    if (setgidx(ID_EFFECTIVE, OID(egid))) {
		warning("setgidx(ID_EFFECTIVE) [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->rgid, (int)state->egid, (int)state->sgid,
		    (int)OID(rgid), (int)OID(egid), (int)OID(sgid));
		goto bad;
	    }
	} else if (OID(sgid) == -1) {
	    if (setgidx(ID_REAL|ID_EFFECTIVE, OID(rgid))) {
		warning("setgidx(ID_REAL|ID_EFFECTIVE) [%d, %d, %d] -> [%d, %d, %d]",
		    (int)state->rgid, (int)state->egid, (int)state->sgid,
		    (int)OID(rgid), (int)OID(egid), (int)OID(sgid));
		goto bad;
	    }
	}
    }
    if (state->grlist != ostate->grlist) {
	if (sudo_setgroups(ostate->grlist->ngids, ostate->grlist->gids)) {
	    warning("setgroups()");
	    goto bad;
	}
    }
    grlist_delref(state->grlist);
    debug_return;

bad:
    exit(1);
}

#elif defined(HAVE_SETREUID)

#define UID_CHANGED (state->ruid != ostate->ruid || state->euid != ostate->euid)
#define GID_CHANGED (state->rgid != ostate->rgid || state->egid != ostate->egid)

/*
 * Set real and effective and saved uids and gids based on perm.
 * We always retain a saved uid of 0 unless we are headed for an exec().
 * We only flip the effective gid since it only changes for PERM_SUDOERS.
 * This version of set_perms() works fine with the "stay_setuid" option.
 */
int
set_perms(int perm)
{
    struct perm_state *state, *ostate = NULL;
    char errbuf[1024];
    int noexit;
    debug_decl(set_perms, SUDO_DEBUG_PERMS)

    noexit = ISSET(perm, PERM_NOEXIT);
    CLR(perm, PERM_MASK);

    if (perm_stack_depth == PERM_STACK_MAX) {
	strlcpy(errbuf, _("perm stack overflow"), sizeof(errbuf));
	errno = EINVAL;
	goto bad;
    }

    state = &perm_stack[perm_stack_depth];
    if (perm != PERM_INITIAL) {
	if (perm_stack_depth == 0) {
	    strlcpy(errbuf, _("perm stack underflow"), sizeof(errbuf));
	    errno = EINVAL;
	    goto bad;
	}
	ostate = &perm_stack[perm_stack_depth - 1];
    }

    switch (perm) {
    case PERM_INITIAL:
	/* Stash initial state */
	state->ruid = getuid();
	state->euid = geteuid();
	state->rgid = getgid();
	state->egid = getegid();
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_INITIAL: "
	    "ruid: %d, euid: %d, rgid: %d, egid: %d", __func__,
	    (int)state->ruid, (int)state->euid,
	    (int)state->rgid, (int)state->egid);
	break;

    case PERM_ROOT:
	state->ruid = ROOT_UID;
	state->euid = ROOT_UID;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	/*
	 * setreuid(0, 0) may fail on some systems if euid is not already 0.
	 */
	if (ostate->euid != ROOT_UID) {
	    if (setreuid(-1, ROOT_UID)) {
		snprintf(errbuf, sizeof(errbuf),
		    "PERM_ROOT: setreuid(-1, %d)", PERM_ROOT);
		goto bad;
	    }
	}
	if (ostate->ruid != ROOT_UID) {
	    if (setreuid(ROOT_UID, -1)) {
		snprintf(errbuf, sizeof(errbuf),
		    "PERM_ROOT: setreuid(%d, -1)", ROOT_UID);
		goto bad;
	    }
	}
	state->rgid = ostate->rgid;
	state->egid = ostate->rgid;
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	break;

    case PERM_USER:
	state->rgid = ostate->rgid;
	state->egid = user_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setregid(ID(rgid), ID(egid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_USER: setregid(%d, %d)", ID(rgid), ID(egid));
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = ROOT_UID;
	state->euid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (UID_CHANGED && setreuid(ID(ruid), ID(euid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_USER: setreuid(%d, %d)", ID(ruid), ID(euid));
	    goto bad;
	}
	break;

    case PERM_FULL_USER:
	/* headed for exec() */
	state->rgid = user_gid;
	state->egid = user_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setregid(ID(rgid), ID(egid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setregid(%d, %d)", ID(rgid), ID(egid));
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_FULL_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	state->euid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (UID_CHANGED && setreuid(ID(ruid), ID(euid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setreuid(%d, %d)", ID(ruid), ID(euid));
	    goto bad;
	}
	break;

    case PERM_RUNAS:
	state->rgid = ostate->rgid;
	state->egid = runas_gr ? runas_gr->gr_gid : runas_pw->pw_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setregid(ID(rgid), ID(egid))) {
	    strlcpy(errbuf, _("unable to change to runas gid"), sizeof(errbuf));
	    goto bad;
	}
	state->grlist = runas_setgroups();
	state->ruid = ROOT_UID;
	state->euid = runas_pw ? runas_pw->pw_uid : user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (UID_CHANGED && setreuid(ID(ruid), ID(euid))) {
	    strlcpy(errbuf, _("unable to change to runas uid"), sizeof(errbuf));
	    goto bad;
	}
	break;

    case PERM_SUDOERS:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);

	/* assume euid == ROOT_UID, ruid == user */
	state->rgid = ostate->rgid;
	state->egid = sudoers_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setregid(ID(rgid), ID(egid))) {
	    strlcpy(errbuf, _("unable to change to sudoers gid"), sizeof(errbuf));
	    goto bad;
	}

	state->ruid = ROOT_UID;
	/*
	 * If sudoers_uid == ROOT_UID and sudoers_mode is group readable
	 * we use a non-zero uid in order to avoid NFS lossage.
	 * Using uid 1 is a bit bogus but should work on all OS's.
	 */
	if (sudoers_uid == ROOT_UID && (sudoers_mode & 040))
	    state->euid = 1;
	else
	    state->euid = sudoers_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (UID_CHANGED && setreuid(ID(ruid), ID(euid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_SUDOERS: setreuid(%d, %d)", ID(ruid), ID(euid));
	    goto bad;
	}
	break;

    case PERM_TIMESTAMP:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->ruid = ROOT_UID;
	state->euid = timestamp_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_TIMESTAMP: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (UID_CHANGED && setreuid(ID(ruid), ID(euid))) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_TIMESTAMP: setreuid(%d, %d)", ID(ruid), ID(euid));
	    goto bad;
	}
	break;
    }

    perm_stack_depth++;
    debug_return_bool(1);
bad:
    warningx("%s: %s", errbuf,
	errno == EAGAIN ? _("too many processes") : strerror(errno));
    if (noexit)
	debug_return_bool(0);
    exit(1);
}

void
restore_perms(void)
{
    struct perm_state *state, *ostate;
    debug_decl(restore_perms, SUDO_DEBUG_PERMS)

    if (perm_stack_depth < 2)
	debug_return;

    state = &perm_stack[perm_stack_depth - 1];
    ostate = &perm_stack[perm_stack_depth - 2];
    perm_stack_depth--;

    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: uid: [%d, %d] -> [%d, %d]",
	__func__, (int)state->ruid, (int)state->euid,
	(int)ostate->ruid, (int)ostate->euid);
    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: gid: [%d, %d] -> [%d, %d]",
	__func__, (int)state->rgid, (int)state->egid,
	(int)ostate->rgid, (int)ostate->egid);

    /*
     * When changing euid to ROOT_UID, setreuid() may fail even if
     * the ruid is ROOT_UID so call setuid() first.
     */
    if (OID(euid) == ROOT_UID) {
	/* setuid() may not set the saved ID unless the euid is ROOT_UID */
	if (ID(euid) != ROOT_UID)
	    (void)setreuid(-1, ROOT_UID);
	if (setuid(ROOT_UID)) {
	    warning("setuid() [%d, %d] -> %d)", (int)state->ruid,
		(int)state->euid, ROOT_UID);
	    goto bad;
	}
    }
    if (setreuid(OID(ruid), OID(euid))) {
	warning("setreuid() [%d, %d] -> [%d, %d]", (int)state->ruid,
	    (int)state->euid, (int)OID(ruid), (int)OID(euid));
	goto bad;
    }
    if (setregid(OID(rgid), OID(egid))) {
	warning("setregid() [%d, %d] -> [%d, %d]", (int)state->rgid,
	    (int)state->egid, (int)OID(rgid), (int)OID(egid));
	goto bad;
    }
    if (state->grlist != ostate->grlist) {
	if (sudo_setgroups(ostate->grlist->ngids, ostate->grlist->gids)) {
	    warning("setgroups()");
	    goto bad;
	}
    }
    grlist_delref(state->grlist);
    debug_return;

bad:
    exit(1);
}

#elif defined(HAVE_SETEUID)

#define GID_CHANGED (state->rgid != ostate->rgid || state->egid != ostate->egid)

/*
 * Set real and effective uids and gids based on perm.
 * We always retain a real or effective uid of ROOT_UID unless
 * we are headed for an exec().
 * This version of set_perms() works fine with the "stay_setuid" option.
 */
int
set_perms(int perm)
{
    struct perm_state *state, *ostate = NULL;
    char errbuf[1024];
    int noexit;
    debug_decl(set_perms, SUDO_DEBUG_PERMS)

    noexit = ISSET(perm, PERM_NOEXIT);
    CLR(perm, PERM_MASK);

    if (perm_stack_depth == PERM_STACK_MAX) {
	strlcpy(errbuf, _("perm stack overflow"), sizeof(errbuf));
	errno = EINVAL;
	goto bad;
    }

    state = &perm_stack[perm_stack_depth];
    if (perm != PERM_INITIAL) {
	if (perm_stack_depth == 0) {
	    strlcpy(errbuf, _("perm stack underflow"), sizeof(errbuf));
	    errno = EINVAL;
	    goto bad;
	}
	ostate = &perm_stack[perm_stack_depth - 1];
    }

    /*
     * Since we only have setuid() and seteuid() and semantics
     * for these calls differ on various systems, we set
     * real and effective uids to ROOT_UID initially to be safe.
     */
    if (perm != PERM_INITIAL) {
	if (ostate->euid != ROOT_UID && seteuid(ROOT_UID)) {
	    snprintf(errbuf, sizeof(errbuf), "set_perms: seteuid(%d)", ROOT_UID);
	    goto bad;
	}
	if (ostate->ruid != ROOT_UID && setuid(ROOT_UID)) {
	    snprintf(errbuf, sizeof(errbuf), "set_perms: setuid(%d)", ROOT_UID);
	    goto bad;
	}
    }

    switch (perm) {
    case PERM_INITIAL:
	/* Stash initial state */
	state->ruid = getuid();
	state->euid = geteuid();
	state->rgid = getgid();
	state->egid = getegid();
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_INITIAL: "
	    "ruid: %d, euid: %d, rgid: %d, egid: %d", __func__,
	    (int)state->ruid, (int)state->euid,
	    (int)state->rgid, (int)state->egid);
	break;

    case PERM_ROOT:
	/* We already set ruid/euid above. */
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, ROOT_UID, ROOT_UID);
	state->ruid = ROOT_UID;
	state->euid = ROOT_UID;
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	break;

    case PERM_USER:
	state->egid = user_gid;
	state->rgid = ostate->rgid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setegid(user_gid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_USER: setegid(%d)", user_gid);
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = ROOT_UID;
	state->euid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_USER: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (seteuid(user_uid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_USER: seteuid(%d)", user_uid);
	    goto bad;
	}
	break;

    case PERM_FULL_USER:
	/* headed for exec() */
	state->rgid = user_gid;
	state->egid = user_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setgid(user_gid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setgid(%d)", user_gid);
	    goto bad;
	}
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_FULL_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	state->euid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_FULL_USER: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (setuid(user_uid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setuid(%d)", user_uid);
	    goto bad;
	}
	break;

    case PERM_RUNAS:
	state->rgid = ostate->rgid;
	state->egid = runas_gr ? runas_gr->gr_gid : runas_pw->pw_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setegid(state->egid)) {
	    strlcpy(errbuf, _("unable to change to runas gid"), sizeof(errbuf));
	    goto bad;
	}
	state->grlist = runas_setgroups();
	state->ruid = ostate->ruid;
	state->euid = runas_pw ? runas_pw->pw_uid : user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_RUNAS: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (seteuid(state->euid)) {
	    strlcpy(errbuf, _("unable to change to runas uid"), sizeof(errbuf));
	    goto bad;
	}
	break;

    case PERM_SUDOERS:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);

	/* assume euid == ROOT_UID, ruid == user */
	state->rgid = ostate->rgid;
	state->egid = sudoers_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: gid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->rgid,
	    (int)ostate->egid, (int)state->rgid, (int)state->egid);
	if (GID_CHANGED && setegid(sudoers_gid)) {
	    strlcpy(errbuf, _("unable to change to sudoers gid"), sizeof(errbuf));
	    goto bad;
	}

	state->ruid = ROOT_UID;
	/*
	 * If sudoers_uid == ROOT_UID and sudoers_mode is group readable
	 * we use a non-zero uid in order to avoid NFS lossage.
	 * Using uid 1 is a bit bogus but should work on all OS's.
	 */
	if (sudoers_uid == ROOT_UID && (sudoers_mode & 040))
	    state->euid = 1;
	else
	    state->euid = sudoers_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_SUDOERS: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (seteuid(state->euid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_SUDOERS: seteuid(%d)", state->euid);
	    goto bad;
	}
	break;

    case PERM_TIMESTAMP:
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	state->rgid = ostate->rgid;
	state->egid = ostate->egid;
	state->ruid = ROOT_UID;
	state->euid = timestamp_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_TIMESTAMP: uid: "
	    "[%d, %d] -> [%d, %d]", __func__, (int)ostate->ruid,
	    (int)ostate->euid, (int)state->ruid, (int)state->euid);
	if (seteuid(timestamp_uid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_TIMESTAMP: seteuid(%d)", timestamp_uid);
	    goto bad;
	}
	break;
    }

    perm_stack_depth++;
    debug_return_bool(1);
bad:
    warningx("%s: %s", errbuf,
	errno == EAGAIN ? _("too many processes") : strerror(errno));
    if (noexit)
	debug_return_bool(0);
    exit(1);
}

void
restore_perms(void)
{
    struct perm_state *state, *ostate;
    debug_decl(restore_perms, SUDO_DEBUG_PERMS)

    if (perm_stack_depth < 2)
	debug_return;

    state = &perm_stack[perm_stack_depth - 1];
    ostate = &perm_stack[perm_stack_depth - 2];
    perm_stack_depth--;

    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: uid: [%d, %d] -> [%d, %d]",
	__func__, (int)state->ruid, (int)state->euid,
	(int)ostate->ruid, (int)ostate->euid);
    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: gid: [%d, %d] -> [%d, %d]",
	__func__, (int)state->rgid, (int)state->egid,
	(int)ostate->rgid, (int)ostate->egid);

    /*
     * Since we only have setuid() and seteuid() and semantics
     * for these calls differ on various systems, we set
     * real and effective uids to ROOT_UID initially to be safe.
     */
    if (seteuid(ROOT_UID)) {
	warningx("seteuid() [%d] -> [%d]", (int)state->euid, ROOT_UID);
	goto bad;
    }
    if (setuid(ROOT_UID)) {
	warningx("setuid() [%d, %d] -> [%d, %d]", (int)state->ruid, ROOT_UID,
	    ROOT_UID, ROOT_UID);
	goto bad;
    }

    if (OID(egid) != -1 && setegid(ostate->egid)) {
	warning("setegid(%d)", (int)ostate->egid);
	goto bad;
    }
    if (state->grlist != ostate->grlist) {
	if (sudo_setgroups(ostate->grlist->ngids, ostate->grlist->gids)) {
	    warning("setgroups()");
	    goto bad;
	}
    }
    if (OID(euid) != -1 && seteuid(ostate->euid)) {
	warning("seteuid(%d)", ostate->euid);
	goto bad;
    }
    grlist_delref(state->grlist);
    debug_return;

bad:
    exit(1);
}

#else /* !HAVE_SETRESUID && !HAVE_SETREUID && !HAVE_SETEUID */

/*
 * Set uids and gids based on perm via setuid() and setgid().
 * NOTE: does not support the "stay_setuid" or timestampowner options.
 *       Also, sudoers_uid and sudoers_gid are not used.
 */
int
set_perms(int perm)
{
    struct perm_state *state, *ostate = NULL;
    char errbuf[1024];
    int noexit;
    debug_decl(set_perms, SUDO_DEBUG_PERMS)

    noexit = ISSET(perm, PERM_NOEXIT);
    CLR(perm, PERM_MASK);

    if (perm_stack_depth == PERM_STACK_MAX) {
	strlcpy(errbuf, _("perm stack overflow"), sizeof(errbuf));
	errno = EINVAL;
	goto bad;
    }

    state = &perm_stack[perm_stack_depth];
    if (perm != PERM_INITIAL) {
	if (perm_stack_depth == 0) {
	    strlcpy(errbuf, _("perm stack underflow"), sizeof(errbuf));
	    errno = EINVAL;
	    goto bad;
	}
	ostate = &perm_stack[perm_stack_depth - 1];
    }

    switch (perm) {
    case PERM_INITIAL:
	/* Stash initial state */
	state->ruid = geteuid() == ROOT_UID ? ROOT_UID : getuid();
	state->rgid = getgid();
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_INITIAL: "
	    "ruid: %d, rgid: %d", __func__, (int)state->ruid, (int)state->rgid);
	break;

    case PERM_ROOT:
	state->ruid = ROOT_UID;
	state->rgid = ostate->rgid;
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: uid: "
	    "[%d] -> [%d]", __func__, (int)ostate->ruid, (int)state->ruid);
	if (setuid(ROOT_UID)) {
	    snprintf(errbuf, sizeof(errbuf), "PERM_ROOT: setuid(%d)", ROOT_UID);
	    goto bad;
	}
	break;

    case PERM_FULL_USER:
	state->rgid = user_gid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: gid: "
	    "[%d] -> [%d]", __func__, (int)ostate->rgid, (int)state->rgid);
	(void) setgid(user_gid);
	state->grlist = user_group_list;
	grlist_addref(state->grlist);
	if (state->grlist != ostate->grlist) {
	    if (sudo_setgroups(state->grlist->ngids, state->grlist->gids)) {
		strlcpy(errbuf, "PERM_FULL_USER: setgroups", sizeof(errbuf));
		goto bad;
	    }
	}
	state->ruid = user_uid;
	sudo_debug_printf(SUDO_DEBUG_INFO, "%s: PERM_ROOT: uid: "
	    "[%d] -> [%d]", __func__, (int)ostate->ruid, (int)state->ruid);
	if (setuid(user_uid)) {
	    snprintf(errbuf, sizeof(errbuf),
		"PERM_FULL_USER: setuid(%d)", user_uid);
	    goto bad;
	}
	break;

    case PERM_USER:
    case PERM_SUDOERS:
    case PERM_RUNAS:
    case PERM_TIMESTAMP:
	/* Unsupported since we can't set euid. */
	state->ruid = ostate->ruid;
	state->rgid = ostate->rgid;
	state->grlist = ostate->grlist;
	grlist_addref(state->grlist);
	break;
    }

    perm_stack_depth++;
    debug_return_bool(1);
bad:
    warningx("%s: %s", errbuf,
	errno == EAGAIN ? _("too many processes") : strerror(errno));
    if (noexit)
	debug_return_bool(0);
    exit(1);
}

void
restore_perms(void)
{
    struct perm_state *state, *ostate;
    debug_decl(restore_perms, SUDO_DEBUG_PERMS)

    if (perm_stack_depth < 2)
	debug_return;

    state = &perm_stack[perm_stack_depth - 1];
    ostate = &perm_stack[perm_stack_depth - 2];
    perm_stack_depth--;

    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: uid: [%d] -> [%d]",
	__func__, (int)state->ruid, (int)ostate->ruid);
    sudo_debug_printf(SUDO_DEBUG_INFO, "%s: gid: [%d] -> [%d]",
	__func__, (int)state->rgid, (int)ostate->rgid);

    if (OID(rgid) != -1 && setgid(ostate->rgid)) {
	warning("setgid(%d)", (int)ostate->rgid);
	goto bad;
    }
    if (state->grlist != ostate->grlist) {
	if (sudo_setgroups(ostate->grlist->ngids, ostate->grlist->gids)) {
	    warning("setgroups()");
	    goto bad;
	}
    }
    grlist_delref(state->grlist);
    if (OID(ruid) != -1 && setuid(ostate->ruid)) {
	warning("setuid(%d)", (int)ostate->ruid);
	goto bad;
    }
    debug_return;

bad:
    exit(1);
}
#endif /* HAVE_SETRESUID || HAVE_SETREUID || HAVE_SETEUID */

#if defined(HAVE_SETRESUID) || defined(HAVE_SETREUID) || defined(HAVE_SETEUID)
static struct group_list *
runas_setgroups(void)
{
    struct passwd *pw;
    struct group_list *grlist;
    debug_decl(runas_setgroups, SUDO_DEBUG_PERMS)

    if (def_preserve_groups) {
	grlist_addref(user_group_list);
	debug_return_ptr(user_group_list);
    }

    pw = runas_pw ? runas_pw : sudo_user.pw;
#ifdef HAVE_SETAUTHDB
    aix_setauthdb(pw->pw_name);
#endif
    grlist = get_group_list(pw);
#ifdef HAVE_SETAUTHDB
    aix_restoreauthdb();
#endif
    if (sudo_setgroups(grlist->ngids, grlist->gids) < 0)
	log_error(USE_ERRNO|MSG_ONLY, _("unable to set runas group vector"));
    debug_return_ptr(grlist);
}
#endif /* HAVE_SETRESUID || HAVE_SETREUID || HAVE_SETEUID */
