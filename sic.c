 /* See LICENSE file for license details. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int debug = 0;
static char *host = "irc.desertbus.org";
static char *port = "6667";
static char *start_chan = "#desertbus";
static char *password;
static char nick[32];
static char bufin[4096];
static char bufout[4096];
static char channel[256];
static time_t trespond;
static FILE *srv;

#include "util.c"

static void
pout(char *channel, char *fmt, ...) {
	static char timestr[18];
	time_t t;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(bufout, sizeof bufout, fmt, ap);
	va_end(ap);
	t = time(NULL);
	strftime(timestr, sizeof timestr, "%D %R", localtime(&t));
	fprintf(stdout, "%-12s: %s %s\n", channel, timestr, bufout);
}

static void
sout(char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(bufout, sizeof bufout, fmt, ap);
	va_end(ap);
	if (debug) fprintf(stderr, "SEND: %s\n", bufout);
	fprintf(srv, "%s\r\n", bufout);
}

static void
privmsg(char *channel, char *msg) {
	if(channel[0] == '\0') {
		pout("", "No channel to send to");
		return;
	}
	pout(channel, "<%s> %s", nick, msg);
	sout("PRIVMSG %s :%s", channel, msg);
}

static void
parsein(char *s) {
	char c, *p;

	if(s[0] == '\0')
		return;
	skip(s, '\n');
	if(s[0] != ':') {
		privmsg(channel, s);
		return;
	}
	c = *++s;
	if(c != '\0' && isspace(s[1])) {
		p = s + 2;
		switch(c) {
		case 'j':
			sout("JOIN %s", p);
			strlcpy(channel, p, sizeof channel);
			return;
		case 'l':
			s = eat(p, isspace, 1);
			p = eat(s, isspace, 0);
			if(!*s)
				s = channel;
			if(*p)
				*p++ = '\0';
			if(!*p)
				p = "sic - 250 LOC are too much!";
			sout("PART %s :%s", s, p);
			return;
		case 'm':
			s = eat(p, isspace, 1);
			p = eat(s, isspace, 0);
			if(*p)
				*p++ = '\0';
			privmsg(s, p);
			return;
		case 's':
			strlcpy(channel, p, sizeof channel);
			return;
		}
	}
	sout("%s", s);
}

static void
parsesrv(char *cmd) {
	if (debug) fprintf(stderr, "RECV: %s", cmd);

	char *usr, *par, *txt;

	usr = host;
	if(!cmd || !*cmd)
		return;
	if(cmd[0] == ':') {
		usr = cmd + 1;
		cmd = skip(usr, ' ');
		if(cmd[0] == '\0')
			return;
		skip(usr, '!');
	}
	skip(cmd, '\r');
	par = skip(cmd, ' ');
	txt = skip(par, ':');
	trim(par);
	if(!strcmp("NICK", cmd) && !strcmp(usr, nick)) {
		printf("Setting nick: %s, %s, %s", nick, par, txt);
		strlcpy(nick, txt, sizeof nick);
	}
	if(!strcmp("PONG", cmd))
		return;
	if(!strcmp("PRIVMSG", cmd))
		pout(par, "<%s> %s", usr, txt);
	else if(!strcmp("PING", cmd))
		sout("PONG %s", txt);
	else if (   (!strcmp("QUIT", cmd))
			|| (!strcmp("JOIN", cmd))
			|| (!strcmp("MODE", cmd))
			|| (!strcmp("NICK", cmd))
			)
		{}
	else {
		pout(usr, ">< %s (%s): %s", cmd, par, txt);
	}
}

int
main(int argc, char *argv[]) {
	int i, c;
	struct timeval tv;
	const char *user;
	(user = getenv("IRC_NICK")) || (user = getenv("USER")) || (user = "unknown");
	fd_set rd;

	strlcpy(nick, user, sizeof nick);
	for(i = 1; i < argc; i++) {
		c = argv[i][1];
		if(argv[i][0] != '-' || argv[i][2])
			c = -1;
		switch(c) {
		case 'h':
			if(++i < argc) host = argv[i];
			break;
		case 'p':
			if(++i < argc) port = argv[i];
			break;
		case 'n':
			if(++i < argc) strlcpy(nick, argv[i], sizeof nick);
			break;
		case 'k':
			if(++i < argc) password = argv[i];
			break;
		case 'd':
			debug = 1;
			break;
		case 'c':
			if(++i < argc)
				strlcpy(channel, argv[i], sizeof channel);
			break;
		case 'v':
			eprint("sic-"VERSION", © 2005-2012 Kris Maglione, Anselm R. Garbe, Nico Golde\n");
		default:
			eprint("usage: sic [-h host] [-p port] [-n nick] [-k keyword] [-c channel] [-d] [-v]\n");
		}
	}
	/* prompt for password */
	if(password && strcmp(password, "-") == 0) {
		password = getpass("Password: ");
		if(!password) perror("getpass() failed");
	}
	/* init */
	i = dial(host, port);
	srv = fdopen(i, "r+");
	/* login */
	if(password) {
		sout("PASS %s", password);
	}
	sout("USER %s localhost %s :%s", nick, host, nick);
	sout("NICK %s", nick);
	if (start_chan) {
		sout("JOIN %s", start_chan);
		strlcpy(channel, start_chan, sizeof channel);
	}
	if(channel[0] != '\0')
		sout("JOIN %s", channel);
	fflush(srv);
	setbuf(stdout, NULL);
	setbuf(srv, NULL);
	for(;;) { /* main loop */
		FD_ZERO(&rd);
		FD_SET(0, &rd);
		FD_SET(fileno(srv), &rd);
		tv.tv_sec = 120;
		tv.tv_usec = 0;
		i = select(fileno(srv) + 1, &rd, 0, 0, &tv);
		if(i < 0) {
			if(errno == EINTR)
				continue;
			eprint("sic: error on select():");
		}
		else if(i == 0) {
			if(time(NULL) - trespond >= 300)
				eprint("sic shutting down: parse timeout\n");
			sout("PING %s", host);
			continue;
		}
		if(FD_ISSET(fileno(srv), &rd)) {
			if(fgets(bufin, sizeof bufin, srv) == NULL)
				eprint("sic: remote host closed connection\n");
			parsesrv(bufin);
			trespond = time(NULL);
		}
		if(FD_ISSET(0, &rd)) {
			if(fgets(bufin, sizeof bufin, stdin) == NULL)
				eprint("sic: end of input\n");
			parsein(bufin);
		}
	}
	return 0;
}
