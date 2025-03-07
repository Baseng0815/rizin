/*	$OpenBSD: engine.c,v 1.15 2005/08/05 13:03:00 espie Exp $	*/

/*-
 * Copyright (c) 1992, 1993, 1994 Henry Spencer.
 * Copyright (c) 1992, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Henry Spencer.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)engine.c	8.5 (Berkeley) 3/20/94
 */

/*
 * The matching engine and friends.  This file is #included by regexec.c
 * after suitable #defines of a variety of macros used herein, so that
 * different state representations can be used without duplicating masses
 * of code.
 */

#ifdef SNAMES
#define matcher smatcher
#define fast    sfast
#define slow    sslow
#define dissect sdissect
#define backref sbackref
#define step    sstep
#define print   sprint
#define at      sat
#define match   smat
#define nope    snope
#endif
#ifdef LNAMES
#define matcher lmatcher
#define fast    lfast
#define slow    lslow
#define dissect ldissect
#define backref lbackref
#define step    lstep
#define print   lprint
#define at      lat
#define match   lmat
#define nope    lnope
#endif

/* another structure passed up and down to avoid zillions of parameters */
struct match {
	struct re_guts *g;
	int eflags;
	RzRegexMatch *pmatch; /* [nsub+1] (0 element unused) */
	char *offp; /* offsets work from here */
	char *beginp; /* start of string -- virtual NUL precedes */
	char *endp; /* end of string -- virtual NUL here */
	char *coldp; /* can be no match starting before here */
	char **lastpos; /* [nplus+1] */
	STATEVARS;
	states st; /* current states */
	states fresh; /* states for a fresh start */
	states tmp; /* temporary */
	states empty; /* empty set of states */
};

static int matcher(struct re_guts *, char *, size_t, RzRegexMatch[], int);
static char *dissect(struct match *, char *, char *, sopno, sopno);
static char *backref(struct match *, char *, char *, sopno, sopno, sopno, int);
static char *fast(struct match *, char *, char *, sopno, sopno);
static char *slow(struct match *, char *, char *, sopno, sopno);
static states step(struct re_guts *, sopno, sopno, states, int, states);
#define MAX_RECURSION 100
#define BOL           (OUT + 1)
#define EOL           (BOL + 1)
#define BOLEOL        (BOL + 2)
#define NOTHING       (BOL + 3)
#define BOW           (BOL + 4)
#define EOW           (BOL + 5)
#define CODEMAX       (BOL + 5) /* highest code used */
#define NONCHAR(c)    ((c) > OUT)
#define NNONCHAR      (CODEMAX - OUT)
#ifdef REDEBUG
static void print(struct match *, char *, states, int, FILE *);
#endif
#ifdef REDEBUG
static void at(struct match *, char *, char *, char *, sopno, sopno);
#endif
#ifdef REDEBUG
static char *pchar(int);
#endif

#ifdef REDEBUG
#define SP(t, s, c)           print(m, t, s, c, stdout)
#define AT(t, p1, p2, s1, s2) at(m, t, p1, p2, s1, s2)
#define NOTE(str) \
	{ \
		if (m->eflags & RZ_REGEX_TRACE) \
			(void)printf("=%s\n", (str)); \
	}
static int nope = 0;
#else
#define SP(t, s, c)           /* nothing */
#define AT(t, p1, p2, s1, s2) /* nothing */
#define NOTE(s)               /* nothing */
#endif

/*
 - matcher - the actual matching engine
 */
static int /* 0 success, RZ_REGEX_NOMATCH failure */
matcher(struct re_guts *g, char *string, size_t nmatch, RzRegexMatch pmatch[],
	int eflags) {
	char *endp;
	int i;
	struct match mv;
	struct match *m = &mv;
	char *dp;
	const sopno gf = g->firststate + 1; /* +1 for OEND */
	const sopno gl = g->laststate;
	char *start;
	char *stop;

	/* simplify the situation where possible */
	if (g->cflags & RZ_REGEX_NOSUB)
		nmatch = 0;
	if (eflags & RZ_REGEX_STARTEND) {
		start = string + pmatch[0].rm_so;
		stop = string + pmatch[0].rm_eo;
	} else {
		start = string;
		stop = start + strlen(start);
	}
	if (stop < start)
		return (RZ_REGEX_INVARG);

	/* prescreening; this does wonders for this rather slow code */
	if (g->must != NULL) {
		for (dp = start; dp < stop; dp++)
			if (*dp == g->must[0] && stop - dp >= g->mlen &&
				memcmp(dp, g->must, (size_t)g->mlen) == 0)
				break;
		if (dp == stop) /* we didn't find g->must */
			return (RZ_REGEX_NOMATCH);
	}

	/* match struct setup */
	m->g = g;
	m->eflags = eflags;
	m->pmatch = NULL;
	m->lastpos = NULL;
	m->offp = string;
	m->beginp = start;
	m->endp = stop;

	if (m->g->nstates * 4 < m->g->nstates)
		return RZ_REGEX_NOMATCH;
	STATESETUP(m, 4);
	SETUP(m->st);
	SETUP(m->fresh);
	SETUP(m->tmp);
	SETUP(m->empty);
	CLEAR(m->empty);

	/* this loop does only one repetition except for backrefs */
	for (;;) {
		endp = fast(m, start, stop, gf, gl);
		if (!endp) { /* a miss */
			free(m->pmatch);
			free(m->lastpos);
			STATETEARDOWN(m);
			return (RZ_REGEX_NOMATCH);
		}
		if (nmatch == 0 && !g->backrefs)
			break; /* no further info needed */

		/* where? */
		if (!m->coldp) {
			break;
		}
		for (;;) {
			NOTE("finding start");
			endp = slow(m, m->coldp, stop, gf, gl);
			if (endp || m->coldp > m->endp) {
				break;
			}
			m->coldp++;
		}
		if (nmatch == 1 && !g->backrefs)
			break; /* no further info needed */

		/* oh my, he wants the subexpressions... */
		if (!m->pmatch) {
			if ((m->g->nsub + 1) * sizeof(RzRegexMatch) < m->g->nsub) {
				return RZ_REGEX_ESPACE;
			}
			m->pmatch = (RzRegexMatch *)malloc((m->g->nsub + 1) *
				sizeof(RzRegexMatch));
		}
		if (!m->pmatch) {
			STATETEARDOWN(m);
			return (RZ_REGEX_ESPACE);
		}
		for (i = 1; i <= m->g->nsub; i++)
			m->pmatch[i].rm_so = m->pmatch[i].rm_eo = -1;
		if (!g->backrefs && !(m->eflags & RZ_REGEX_BACKR)) {
			NOTE("dissecting");
			dp = dissect(m, m->coldp, endp, gf, gl);
		} else {
			if (g->nplus > 0 && !m->lastpos) {
				if ((g->nplus + 1) * sizeof(char *) < g->nplus) {
					free(m->pmatch);
					STATETEARDOWN(m);
					return RZ_REGEX_ESPACE;
				}
				m->lastpos = (char **)malloc((g->nplus + 1) *
					sizeof(char *));
			}
			if (g->nplus > 0 && !m->lastpos) {
				free(m->pmatch);
				STATETEARDOWN(m);
				return (RZ_REGEX_ESPACE);
			}
			NOTE("backref dissect");
			dp = backref(m, m->coldp, endp, gf, gl, (sopno)0, 0);
		}
		if (dp) {
			break;
		}
		/* uh-oh... we couldn't find a subexpression-level match */
		if (!g->backrefs) { /* must be back references doing it */
			break;
		}
		if (g->nplus || !m->lastpos) {
			break;
		}
		for (;;) {
			if (dp != NULL || endp <= m->coldp)
				break; /* defeat */
			NOTE("backoff");
			endp = slow(m, m->coldp, endp - 1, gf, gl);
			if (!endp)
				break; /* defeat */
				/* try it on a shorter possibility */
#ifndef NDEBUG
			for (i = 1; i <= m->g->nsub; i++) {
				if (m->pmatch[i].rm_so != -1) {
					break;
				}
				if (m->pmatch[i].rm_eo != -1) {
					break;
				}
			}
#endif
			NOTE("backoff dissect");
			dp = backref(m, m->coldp, endp, gf, gl, (sopno)0, 0);
		}
		if (dp != NULL || dp != endp) /* found a shorter one */
			break;

		/* despite initial appearances, there is no match here */
		NOTE("false alarm");
		if (m->coldp == stop)
			break;
		start = m->coldp + 1; /* recycle starting later */
	}

	/* fill in the details if requested */
	if (nmatch > 0) {
		pmatch[0].rm_so = m->coldp - m->offp;
		pmatch[0].rm_eo = endp - m->offp;
	}
	if (nmatch > 1) {
		if (m->pmatch) {
			for (i = 1; i < nmatch; i++) {
				if (i <= m->g->nsub) {
					pmatch[i] = m->pmatch[i];
				} else {
					pmatch[i].rm_so = -1;
					pmatch[i].rm_eo = -1;
				}
			}
		}
	}

	if (m->pmatch != NULL)
		free((char *)m->pmatch);
	if (m->lastpos != NULL)
		free((char *)m->lastpos);
	STATETEARDOWN(m);
	return (0);
}

/*
 - dissect - figure out what matched what, no back references
 */
static char * /* == stop (success) always */
dissect(struct match *m, char *start, char *stop, sopno startst, sopno stopst) {
	int i;
	sopno ss; /* start sop of current subRE */
	sopno es; /* end sop of current subRE */
	char *sp; /* start of string matched by it */
	char *stp; /* string matched by it cannot pass here */
	char *rest; /* start of rest of string */
	char *tail; /* string unmatched by rest of RE */
	sopno ssub; /* start sop of subsubRE */
	sopno esub; /* end sop of subsubRE */
	char *ssp; /* start of string matched by subsubRE */
	char *sep; /* end of string matched by subsubRE */
	char *oldssp; /* previous ssp */
	char *dp;

	AT("diss", start, stop, startst, stopst);
	sp = start;
	for (ss = startst; ss < stopst; ss = es) {
		/* identify end of subRE */
		es = ss;
		switch (OP(m->g->strip[es])) {
		case OPLUS_:
		case OQUEST_:
			es += OPND(m->g->strip[es]);
			break;
		case OCH_:
			while (OP(m->g->strip[es]) != O_CH)
				es += OPND(m->g->strip[es]);
			break;
		}
		es++;

		/* figure out what it matched */
		switch (OP(m->g->strip[ss])) {
		case OEND:
			break;
		case OCHAR:
			sp++;
			break;
		case OBOL:
		case OEOL:
		case OBOW:
		case OEOW:
			break;
		case OANY:
		case OANYOF:
			sp++;
			break;
		case OBACK_:
		case O_BACK:
			break;
		/* cases where length of match is hard to find */
		case OQUEST_:
			stp = stop;
			for (;;) {
				/* how long could this one be? */
				rest = slow(m, sp, stp, ss, es);
				if (rest) { /* it did match */
					/* could the rest match the rest? */
					tail = slow(m, rest, stop, es, stopst);
					if (tail == stop)
						break; /* yes! */
					/* no -- try a shorter match for this one */
					stp = rest - 1;
				}
			}
			ssub = ss + 1;
			esub = es - 1;
			/* did innards match? */
			if (slow(m, sp, rest, ssub, esub) != NULL) {
				dp = dissect(m, sp, rest, ssub, esub);
				if (dp != rest)
					return NULL;
			} else if (sp != rest)
				return NULL;
			sp = rest;
			break;
		case OPLUS_:
			stp = stop;
			for (;;) {
				/* how long could this one be? */
				rest = slow(m, sp, stp, ss, es);
				if (rest != NULL) { /* it did match */
					/* could the rest match the rest? */
					tail = slow(m, rest, stop, es, stopst);
					if (tail == stop)
						break; /* yes! */
					/* no -- try a shorter match for this one */
					stp = rest - 1;
				}
			}
			ssub = ss + 1;
			esub = es - 1;
			ssp = sp;
			oldssp = ssp;
			for (;;) { /* find last match of innards */
				sep = slow(m, ssp, rest, ssub, esub);
				if (!sep || sep == ssp)
					break; /* failed or matched null */
				oldssp = ssp; /* on to next try */
				ssp = sep;
			}
			if (!sep) {
				/* last successful match */
				sep = ssp;
				ssp = oldssp;
			}
			if (sep == rest) { /* must exhaust substring */
				if (slow(m, ssp, sep, ssub, esub) == rest) {
					dp = dissect(m, ssp, sep, ssub, esub);
					if (dp == sep) {
						sp = rest;
					}
				}
			}
			break;
		case OCH_:
			stp = stop;
			for (;;) {
				/* how long could this one be? */
				rest = slow(m, sp, stp, ss, es);
				if (rest) { /* it did match */
					/* could the rest match the rest? */
					tail = slow(m, rest, stop, es, stopst);
					if (tail == stop)
						break; /* yes! */
					/* no -- try a shorter match for this one */
					stp = rest - 1;
				}
			}
			ssub = ss + 1;
			esub = ss + OPND(m->g->strip[ss]) - 1;
			if (OP(m->g->strip[esub]) != OOR1) {
				break;
			}
			for (;;) { /* find first matching branch */
				if (slow(m, sp, rest, ssub, esub) == rest)
					break; /* it matched all of it */
				/* that one missed, try next one */
				if (OP(m->g->strip[esub]) == OOR1) {
					esub++;
					if (OP(m->g->strip[esub]) == OOR2) {
						ssub = esub + 1;
						esub += OPND(m->g->strip[esub]);
						if (OP(m->g->strip[esub]) == OOR2) {
							esub--;
						} else {
							if (OP(m->g->strip[esub]) != O_CH) {
								break;
							}
						}
					}
				}
			}
			dp = dissect(m, sp, rest, ssub, esub);
			if (dp == rest) {
				sp = rest;
			}
			break;
		case O_PLUS:
		case O_QUEST:
		case OOR1:
		case OOR2:
		case O_CH:
			break;
		case OLPAREN:
			i = OPND(m->g->strip[ss]);
			if (i > 0 && i <= m->g->nsub) {
				m->pmatch[i].rm_so = sp - m->offp;
			}
			break;
		case ORPAREN:
			i = OPND(m->g->strip[ss]);
			if (i > 0 && i <= m->g->nsub) {
				m->pmatch[i].rm_eo = sp - m->offp;
			}
			break;
		default: /* uh oh */
			break;
		}
	}

	if (sp == stop) {
		return sp;
	} else {
		return NULL;
	}
}

/*
 - backref - figure out what matched what, figuring in back references
 */
static char * /* == stop (success) or NULL (failure) */
backref(struct match *m, char *start, char *stop, sopno startst, sopno stopst,
	sopno lev, int rec) /* PLUS nesting level */
{
	int i;
	sopno ss; /* start sop of current subRE */
	char *sp; /* start of string matched by it */
	sopno ssub; /* start sop of subsubRE */
	sopno esub; /* end sop of subsubRE */
	char *ssp; /* start of string matched by subsubRE */
	char *dp;
	size_t len;
	int hard;
	sop s;
	ut64 offsave;
	cset *cs;

	AT("back", start, stop, startst, stopst);
	sp = start;

	/* get as far as we can with easy stuff */
	hard = 0;
	for (ss = startst; !hard && ss < stopst; ss++)
		switch (OP(s = m->g->strip[ss])) {
		case OCHAR:
			if (sp == stop || *sp++ != (char)OPND(s))
				return (NULL);
			break;
		case OANY:
			if (sp == stop)
				return (NULL);
			sp++;
			break;
		case OANYOF:
			cs = &m->g->sets[OPND(s)];
			if (sp == stop || !CHIN(cs, *sp++))
				return (NULL);
			break;
		case OBOL:
			if ((sp == m->beginp && !(m->eflags & RZ_REGEX_NOTBOL)) ||
				(sp < m->endp && *(sp - 1) == '\n' &&
					(m->g->cflags & RZ_REGEX_NEWLINE))) { /* yes */
			} else
				return (NULL);
			break;
		case OEOL:
			if ((sp == m->endp && !(m->eflags & RZ_REGEX_NOTEOL)) ||
				(sp < m->endp && *sp == '\n' &&
					(m->g->cflags & RZ_REGEX_NEWLINE))) { /* yes */
			} else
				return (NULL);
			break;
		case OBOW:
			if (((sp == m->beginp && !(m->eflags & RZ_REGEX_NOTBOL)) ||
				    (sp < m->endp && *(sp - 1) == '\n' &&
					    (m->g->cflags & RZ_REGEX_NEWLINE)) ||
				    (sp > m->beginp &&
					    !ISWORD((unsigned char)*(sp - 1)))) &&
				(sp < m->endp && ISWORD((unsigned char)*sp))) { /* yes */
			} else
				return (NULL);
			break;
		case OEOW:
			if (((sp == m->endp && !(m->eflags & RZ_REGEX_NOTEOL)) ||
				    (sp < m->endp && *sp == '\n' &&
					    (m->g->cflags & RZ_REGEX_NEWLINE)) ||
				    (sp < m->endp && !ISWORD((unsigned char)*sp))) &&
				(sp > m->beginp && ISWORD((unsigned char)*(sp - 1)))) { /* yes */
			} else
				return (NULL);
			break;
		case O_QUEST:
			break;
		case OOR1: /* matches null but needs to skip */
			ss++;
			s = m->g->strip[ss];
			do {
				if (OP(s) == OOR2) {
					ss += OPND(s);
				}
			} while (OP(s = m->g->strip[ss]) != O_CH);
			/* note that the ss++ gets us past the O_CH */
			break;
		default: /* have to make a choice */
			hard = 1;
			break;
		}
	if (!hard) { /* that was it! */
		if (sp != stop)
			return (NULL);
		return (sp);
	}
	ss--; /* adjust for the for's final increment */

	/* the hard stuff */
	AT("hard", sp, stop, ss, stopst);
	s = m->g->strip[ss];
	switch (OP(s)) {
	case OBACK_: /* the vilest depths */
		i = OPND(s);
		if (i > 0 && i <= m->g->nsub) {
			if (m->pmatch[i].rm_eo == -1) {
				return NULL;
			}
		}
		if (m->pmatch[i].rm_so != -1) {
			len = m->pmatch[i].rm_eo - m->pmatch[i].rm_so;
			if (len == 0 && rec++ > MAX_RECURSION)
				return (NULL);
			if (stop - m->beginp >= len) {
				if (sp > stop - len) {
					return (NULL); /* not enough left to match */
				}
			}
			ssp = m->offp + m->pmatch[i].rm_so;
			if (memcmp(sp, ssp, len) != 0)
				return (NULL);
			while (m->g->strip[ss] != SOP(O_BACK, i))
				ss++;
			return (backref(m, sp + len, stop, ss + 1, stopst, lev, rec));
		}
		break;
	case OQUEST_: /* to null or not */
		dp = backref(m, sp, stop, ss + 1, stopst, lev, rec);
		if (dp != NULL)
			return (dp); /* not */
		return (backref(m, sp, stop, ss + OPND(s) + 1, stopst, lev, rec));
		break;
	case OPLUS_:
		if (m->lastpos && (lev + 1 <= m->g->nplus)) {
			m->lastpos[lev + 1] = sp;
			return (backref(m, sp, stop, ss + 1, stopst, lev + 1, rec));
		}
		break;
	case O_PLUS:
		if (sp == m->lastpos[lev]) /* last pass matched null */
			return (backref(m, sp, stop, ss + 1, stopst, lev - 1, rec));
		/* try another pass */
		m->lastpos[lev] = sp;
		dp = backref(m, sp, stop, ss - OPND(s) + 1, stopst, lev, rec);
		if (!dp)
			return (backref(m, sp, stop, ss + 1, stopst, lev - 1, rec));
		else
			return (dp);
		break;
	case OCH_: /* find the right one, if any */
		ssub = ss + 1;
		esub = ss + OPND(s) - 1;
		if (OP(m->g->strip[esub]) != OOR1) {
			break;
		}
		for (;;) { /* find first matching branch */
			dp = backref(m, sp, stop, ssub, esub, lev, rec);
			if (dp != NULL)
				return (dp);
			/* that one missed, try next one */
			if (OP(m->g->strip[esub]) == O_CH)
				return (NULL); /* there is none */
			esub++;
			if (OP(m->g->strip[esub]) != OOR2) {
				break;
			}
			ssub = esub + 1;
			esub += OPND(m->g->strip[esub]);
			if (OP(m->g->strip[esub]) == OOR2)
				esub--;
			else if (OP(m->g->strip[esub]) != O_CH) {
				break;
			}
		}
		break;
	case OLPAREN: /* must undo assignment if rest fails */
		i = OPND(s);
		if (i > 0 && i <= m->g->nsub) {
			offsave = m->pmatch[i].rm_so;
			m->pmatch[i].rm_so = sp - m->offp;
			dp = backref(m, sp, stop, ss + 1, stopst, lev, rec);
			if (dp != NULL)
				return (dp);
			m->pmatch[i].rm_so = offsave;
			return (NULL);
		}
		break;
	case ORPAREN: /* must undo assignment if rest fails */
		i = OPND(s);
		if (i > 0 && i <= m->g->nsub) {
			offsave = m->pmatch[i].rm_eo;
			m->pmatch[i].rm_eo = sp - m->offp;
			dp = backref(m, sp, stop, ss + 1, stopst, lev, rec);
			if (dp != NULL)
				return (dp);
			m->pmatch[i].rm_eo = offsave;
			return (NULL);
		}
		break;
	default: /* uh oh */
		break;
	}

	/* NOTREACHED */
	return NULL;
}

/*
 - fast - step through the string at top speed
 */
static char * /* where tentative match ended, or NULL */
fast(struct match *m, char *start, char *stop, sopno startst, sopno stopst) {
	states st = m->st;
	states fresh = m->fresh;
	states tmp = m->tmp;
	char *p = start;
	int c = (start == m->beginp) ? OUT : *(start - 1);
	int lastc; /* previous c */
	int flagch;
	int i;
	char *coldp; /* last p after which no match was underway */

	CLEAR(st);
	SET1(st, startst);
	st = step(m->g, startst, stopst, st, NOTHING, st);
	ASSIGN(fresh, st);
	SP("start", st, *p);
	coldp = NULL;
	for (;;) {
		/* next character */
		lastc = c;
		c = (p == m->endp) ? OUT : *p;
		if (EQ(st, fresh)) {
			coldp = p;
		}

		/* is there an EOL and/or BOL between lastc and c? */
		flagch = '\0';
		i = 0;
		if ((lastc == '\n' && m->g->cflags & RZ_REGEX_NEWLINE) ||
			(lastc == OUT && !(m->eflags & RZ_REGEX_NOTBOL))) {
			flagch = BOL;
			i = m->g->nbol;
		}
		if ((c == '\n' && m->g->cflags & RZ_REGEX_NEWLINE) ||
			(c == OUT && !(m->eflags & RZ_REGEX_NOTEOL))) {
			flagch = (flagch == BOL) ? BOLEOL : EOL;
			i += m->g->neol;
		}
		if (i != 0) {
			for (; i > 0; i--)
				st = step(m->g, startst, stopst, st, flagch, st);
			SP("boleol", st, c);
		}

		/* how about a word boundary? */
		if ((flagch == BOL || (lastc != OUT && !ISWORD(lastc))) &&
			(c != OUT && ISWORD(c))) {
			flagch = BOW;
		}
		if ((lastc != OUT && ISWORD(lastc)) &&
			(flagch == EOL || (c != OUT && !ISWORD(c)))) {
			flagch = EOW;
		}
		if (flagch == BOW || flagch == EOW) {
			st = step(m->g, startst, stopst, st, flagch, st);
			SP("boweow", st, c);
		}

		/* are we done? */
		if (ISSET(st, stopst) || p == stop)
			break; /* NOTE BREAK OUT */

		/* no, we must deal with this character */
		ASSIGN(tmp, st);
		ASSIGN(st, fresh);
		if (c == OUT) {
			break;
		}
		st = step(m->g, startst, stopst, tmp, c, st);
		SP("aft", st, c);
		ASSIGN(tmp, st);
		if (!EQ(step(m->g, startst, stopst, tmp, NOTHING, tmp), st)) {
			break;
		}
		p++;
	}

	if (coldp) {
		m->coldp = coldp;
		if (ISSET(st, stopst))
			return (p + 1);
	}
	return NULL;
}

/*
 - slow - step through the string more deliberately
 */
static char * /* where it ended */
slow(struct match *m, char *start, char *stop, sopno startst, sopno stopst) {
	states st = m->st;
	states empty = m->empty;
	states tmp = m->tmp;
	char *p = start;
	int c = (start == m->beginp) ? OUT : *(start - 1);
	int lastc; /* previous c */
	int flagch;
	int i;
	char *matchp; /* last p at which a match ended */

	AT("slow", start, stop, startst, stopst);
	CLEAR(st);
	SET1(st, startst);
	SP("sstart", st, *p);
	st = step(m->g, startst, stopst, st, NOTHING, st);
	matchp = NULL;
	for (;;) {
		/* next character */
		lastc = c;
		c = (p == m->endp) ? OUT : *p;

		/* is there an EOL and/or BOL between lastc and c? */
		flagch = '\0';
		i = 0;
		if ((lastc == '\n' && m->g->cflags & RZ_REGEX_NEWLINE) ||
			(lastc == OUT && !(m->eflags & RZ_REGEX_NOTBOL))) {
			flagch = BOL;
			i = m->g->nbol;
		}
		if ((c == '\n' && m->g->cflags & RZ_REGEX_NEWLINE) ||
			(c == OUT && !(m->eflags & RZ_REGEX_NOTEOL))) {
			flagch = (flagch == BOL) ? BOLEOL : EOL;
			i += m->g->neol;
		}
		if (i != 0) {
			for (; i > 0; i--)
				st = step(m->g, startst, stopst, st, flagch, st);
			SP("sboleol", st, c);
		}

		/* how about a word boundary? */
		if ((flagch == BOL || (lastc != OUT && !ISWORD(lastc))) &&
			(c != OUT && ISWORD(c))) {
			flagch = BOW;
		}
		if ((lastc != OUT && ISWORD(lastc)) &&
			(flagch == EOL || (c != OUT && !ISWORD(c)))) {
			flagch = EOW;
		}
		if (flagch == BOW || flagch == EOW) {
			st = step(m->g, startst, stopst, st, flagch, st);
			SP("sboweow", st, c);
		}

		/* are we done? */
		if (ISSET(st, stopst))
			matchp = p;
		if (EQ(st, empty) || p == stop)
			break; /* NOTE BREAK OUT */

		/* no, we must deal with this character */
		ASSIGN(tmp, st);
		ASSIGN(st, empty);
		if (c == OUT) {
			break;
		}
		st = step(m->g, startst, stopst, tmp, c, st);
		SP("saft", st, c);
		if (!EQ(step(m->g, startst, stopst, st, NOTHING, st), st)) {
			break;
		}
		p++;
	}

	return (matchp);
}

/*
 - step - map set of states reachable before char to set reachable after
 */
static states
step(struct re_guts *g,
	sopno start, /* start state within strip */
	sopno stop, /* state after stop state within strip */
	states bef, /* states reachable before */
	int ch, /* character or NONCHAR code */
	states aft) /* states already known reachable after */
{
	cset *cs;
	sop s;
	sopno pc;
	onestate here; /* note, macros know this name */
	sopno look;
	int i;

	for (pc = start, INIT(here, pc); pc != stop; pc++, INC(here)) {
		s = g->strip[pc];
		switch (OP(s)) {
		case OEND:
			break;
		case OCHAR:
			/* only characters can match */
			if (!NONCHAR(ch) || ch != (char)OPND(s)) {
				if (ch == (char)OPND(s))
					FWD(aft, bef, 1);
			}
			break;
		case OBOL:
			if (ch == BOL || ch == BOLEOL)
				FWD(aft, bef, 1);
			break;
		case OEOL:
			if (ch == EOL || ch == BOLEOL)
				FWD(aft, bef, 1);
			break;
		case OBOW:
			if (ch == BOW)
				FWD(aft, bef, 1);
			break;
		case OEOW:
			if (ch == EOW)
				FWD(aft, bef, 1);
			break;
		case OANY:
			if (!NONCHAR(ch))
				FWD(aft, bef, 1);
			break;
		case OANYOF:
			cs = &g->sets[OPND(s)];
			if (!NONCHAR(ch) && CHIN(cs, ch))
				FWD(aft, bef, 1);
			break;
		case OBACK_: /* ignored here */
		case O_BACK:
			FWD(aft, aft, 1);
			break;
		case OPLUS_: /* forward, this is just an empty */
			FWD(aft, aft, 1);
			break;
		case O_PLUS: /* both forward and back */
			FWD(aft, aft, 1);
			i = ISSETBACK(aft, OPND(s));
			BACK(aft, aft, OPND(s));
			if (!i && ISSETBACK(aft, OPND(s))) {
				/* oho, must reconsider loop body */
				pc -= OPND(s) + 1;
				INIT(here, pc);
			}
			break;
		case OQUEST_: /* two branches, both forward */
			FWD(aft, aft, 1);
			FWD(aft, aft, OPND(s));
			break;
		case O_QUEST: /* just an empty */
			FWD(aft, aft, 1);
			break;
		case OLPAREN: /* not significant here */
		case ORPAREN:
			FWD(aft, aft, 1);
			break;
		case OCH_: /* mark the first two branches */
			FWD(aft, aft, 1);
			if ((OP(g->strip[pc + OPND(s)]) != OOR2)) {
				break;
			}
			FWD(aft, aft, OPND(s));
			break;
		case OOR1: /* done a branch, find the O_CH */
			if (ISSTATEIN(aft, here)) {
				for (look = 1;
					OP(s = g->strip[pc + look]) != O_CH;
					look += OPND(s)) {
					if (OP(s) != OOR2) {
						break;
					}
				}
				FWD(aft, aft, look);
			}
			break;
		case OOR2: /* propagate OCH_'s marking */
			FWD(aft, aft, 1);
			if (OP(g->strip[pc + OPND(s)]) != O_CH) {
				if (OP(g->strip[pc + OPND(s)]) == OOR2) {
					FWD(aft, aft, OPND(s));
				}
			}
			break;
		case O_CH: /* just empty */
			FWD(aft, aft, 1);
			break;
		default: /* ooooops... */
			eprintf("ops in regex.c\n");
			break;
		}
	}

	return (aft);
}

#ifdef REDEBUG
/*
 - print - print a set of states
 */
static void
print(struct match *m, char *caption, states st, int ch, FILE *d) {
	struct re_guts *g = m->g;
	int i;
	int first = 1;

	if (!(m->eflags & RZ_REGEX_TRACE))
		return;

	(void)fprintf(d, "%s", caption);
	if (ch != '\0')
		(void)fprintf(d, " %s", pchar(ch));
	for (i = 0; i < g->nstates; i++)
		if (ISSET(st, i)) {
			(void)fprintf(d, "%s%d", (first) ? "\t" : ", ", i);
			first = 0;
		}
	(void)fprintf(d, "\n");
}

/*
 - at - print current situation
 */
static void
at(struct match *m, char *title, char *start, char *stop, sopno startst,
	sopno stopst) {
	if (!(m->eflags & RZ_REGEX_TRACE))
		return;

	(void)printf("%s %s-", title, pchar(*start));
	(void)printf("%s ", pchar(*stop));
	(void)printf("%ld-%ld\n", (long)startst, (long)stopst);
}

#ifndef PCHARDONE
#define PCHARDONE /* never again */
/*
 - pchar - make a character printable
 *
 * Is this identical to regchar() over in debug.c?  Well, yes.  But a
 * duplicate here avoids having a debugging-capable regexec.o tied to
 * a matching debug.o, and this is convenient.  It all disappears in
 * the non-debug compilation anyway, so it doesn't matter much.
 */
static char * /* -> representation */
pchar(int ch) {
	static char pbuf[10];

	if (isprint((ut8)ch) || ch == ' ')
		(void)snprintf(pbuf, sizeof pbuf, "%c", ch);
	else
		(void)snprintf(pbuf, sizeof pbuf, "\\%o", ch);
	return (pbuf);
}
#endif
#endif

#undef matcher
#undef fast
#undef slow
#undef dissect
#undef backref
#undef step
#undef print
#undef at
#undef match
#undef nope
