/*
 * Line-editing routines
 *
 * Copyright 1992 Simmule Turner and Rich Salz.  All rights reserved.
 *
 *  
 *  This software is not subject to any license of the American Telephone
 *  and Telegraph Company or of the Regents of the University of California.
 *  
 *  Permission is granted to anyone to use this software for any purpose on
 *  any computer system, and to alter it and redistribute it freely, subject
 *  to the following restrictions:
 *  1. The authors are not responsible for the consequences of use of this
 *     software, no matter how awful, even if they arise from flaws in it.
 *  2. The origin of this software must not be misrepresented, either by
 *     explicit claim or by omission.  Since few users ever read sources,
 *     credits must appear in the documentation.
 *  3. Altered versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.  Since few users
 *     ever read sources, credits must appear in the documentation.
 *  4. This notice may not be removed or altered.
 *
 * The code was heavily simplified for inclusion in Wine. -- AJ
 */

#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "wintypes.h"

/*
**  Manifest constants.
*/
#define SCREEN_WIDTH	80
#define SCREEN_ROWS	24
#define NO_ARG		(-1)
#define DEL		127
#define CTL(x)		((x) & 0x1F)
#define ISCTL(x)	((x) && (x) < ' ')
#define UNCTL(x)	((x) + 64)
#define META(x)		((x) | 0x80)
#define ISMETA(x)	((x) & 0x80)
#define UNMETA(x)	((x) & 0x7F)
#if	!defined(HIST_SIZE)
#define HIST_SIZE	20
#endif	/* !defined(HIST_SIZE) */
#define CRLF   "\r\n"
#define MEM_INC		64
#define SCREEN_INC	256

#define DISPOSE(p)	free((char *)(p))
#define NEW(T, c)	\
	((T *)malloc((unsigned int)(sizeof (T) * (c))))
#define RENEW(p, T, c)	\
	(p = (T *)realloc((char *)(p), (unsigned int)(sizeof (T) * (c))))
#define COPYFROMTO(new, p, len)	\
	(void)memcpy((char *)(new), (char *)(p), (int)(len))

/*
**  Command status codes.
*/
typedef enum _STATUS {
    CSdone, CSeof, CSmove, CSdispatch, CSstay
} STATUS;

/*
**  The type of case-changing to perform.
*/
typedef enum _CASE {
    TOupper, TOlower
} CASE;

/*
**  Key to command mapping.
*/
typedef struct _KEYMAP {
    CHAR	Key;
    STATUS	(*Function)();
} KEYMAP;

/*
**  Command history structure.
*/
typedef struct _HISTORY {
    int		Size;
    int		Pos;
    CHAR	*Lines[HIST_SIZE];
} HISTORY;

/*
**  Globals.
*/
static int		rl_eof;
static int		rl_erase;
static int		rl_intr;
static int		rl_kill;

static CHAR		NIL[] = "";
static const CHAR	*Input = NIL;
static CHAR		*Line;
static const char	*Prompt;
static CHAR		*Yanked;
static char		*Screen;
static char		NEWLINE[]= CRLF;
static HISTORY		H;
static int		rl_quit;
static int		Repeat;
static int		End;
static int		Mark;
static int		OldPoint;
static int		Point;
static int		PushBack;
static int		Pushed;
static KEYMAP		Map[33];
static KEYMAP		MetaMap[16];
static size_t		Length;
static size_t		ScreenCount;
static size_t		ScreenSize;
static char		*backspace;
static int		TTYwidth;
static int		TTYrows;

/* Display print 8-bit chars as `M-x' or as the actual 8-bit char? */
int		rl_meta_chars = 1;

/*
**  Declarations.
*/
static CHAR	*editinput();
extern int	read();
extern int	write();
#if	defined(USE_TERMCAP)
extern char	*getenv();
extern char	*tgetstr();
extern int	tgetent();
#endif	/* defined(USE_TERMCAP) */

/*
**  TTY input/output functions.
*/

#ifdef HAVE_TCGETATTR
#include <termios.h>

static void
rl_ttyset(Reset)
    int				Reset;
{
    static struct termios	old;
    struct termios		new;

    if (Reset == 0) {
	(void)tcgetattr(0, &old);
	rl_erase = old.c_cc[VERASE];
	rl_kill = old.c_cc[VKILL];
	rl_eof = old.c_cc[VEOF];
	rl_intr = old.c_cc[VINTR];
	rl_quit = old.c_cc[VQUIT];

	new = old;
	new.c_cc[VINTR] = -1;
	new.c_cc[VQUIT] = -1;
	new.c_lflag &= ~(ECHO | ICANON);
	new.c_iflag &= ~(ISTRIP | INPCK);
	new.c_cc[VMIN] = 1;
	new.c_cc[VTIME] = 0;
	(void)tcsetattr(0, TCSANOW, &new);
    }
    else
	(void)tcsetattr(0, TCSANOW, &old);
}

#else  /* HAVE_TCGETATTR */

static void
rl_ttyset(Reset)
    int				Reset;
{
    static struct sgttyb	old_sgttyb;
    static struct tchars	old_tchars;
    struct sgttyb		new_sgttyb;
    struct tchars		new_tchars;

    if (Reset == 0) {
	(void)ioctl(0, TIOCGETP, &old_sgttyb);
	rl_erase = old_sgttyb.sg_erase;
	rl_kill = old_sgttyb.sg_kill;

	(void)ioctl(0, TIOCGETC, &old_tchars);
	rl_eof = old_tchars.t_eofc;
	rl_intr = old_tchars.t_intrc;
	rl_quit = old_tchars.t_quitc;

	new_sgttyb = old_sgttyb;
	new_sgttyb.sg_flags &= ~ECHO;
	new_sgttyb.sg_flags |= RAW;
#if	defined(PASS8)
	new_sgttyb.sg_flags |= PASS8;
#endif	/* defined(PASS8) */
	(void)ioctl(0, TIOCSETP, &new_sgttyb);

	new_tchars = old_tchars;
	new_tchars.t_intrc = -1;
	new_tchars.t_quitc = -1;
	(void)ioctl(0, TIOCSETC, &new_tchars);
    }
    else {
	(void)ioctl(0, TIOCSETP, &old_sgttyb);
	(void)ioctl(0, TIOCSETC, &old_tchars);
    }
}

#endif	/* HAVE_TCGETATTR */

static void
TTYflush()
{
    if (ScreenCount) {
	(void)write(1, Screen, ScreenCount);
	ScreenCount = 0;
    }
}

static void
TTYput(c)
    CHAR	c;
{
    Screen[ScreenCount] = c;
    if (++ScreenCount >= ScreenSize - 1) {
	ScreenSize += SCREEN_INC;
	RENEW(Screen, char, ScreenSize);
    }
}

static void
TTYputs(p)
    CHAR	*p;
{
    while (*p)
	TTYput(*p++);
}

static void
TTYshow(c)
    CHAR	c;
{
    if (c == DEL) {
	TTYput('^');
	TTYput('?');
    }
    else if (ISCTL(c)) {
	TTYput('^');
	TTYput(UNCTL(c));
    }
    else if (rl_meta_chars && ISMETA(c)) {
	TTYput('M');
	TTYput('-');
	TTYput(UNMETA(c));
    }
    else
	TTYput(c);
}

static void
TTYstring(p)
    CHAR	*p;
{
    while (*p)
	TTYshow(*p++);
}

static unsigned int
TTYget()
{
    CHAR	c;
    int retv;

    TTYflush();
    if (Pushed) {
	Pushed = 0;
	return PushBack;
    }
    if (*Input)
	return *Input++;

    while ( ( retv = read( 0, &c, (size_t)1 ) ) == -1 )
    {
        if ( errno != EINTR )
            perror( "read" );
    }

    return retv == 1 ? c : EOF;
}

#define TTYback()	(backspace ? TTYputs((CHAR *)backspace) : TTYput('\b'))

static void
TTYbackn(n)
    int		n;
{
    while (--n >= 0)
	TTYback();
}

static void
TTYinfo()
{
    static int		init;
#if	defined(USE_TERMCAP)
    char		*term;
    char		buff[2048];
    char		*bp;
#endif	/* defined(USE_TERMCAP) */
#if	defined(TIOCGWINSZ)
    struct winsize	W;
#endif	/* defined(TIOCGWINSZ) */

    if (init) {
#if	defined(TIOCGWINSZ)
	/* Perhaps we got resized. */
	if (ioctl(0, TIOCGWINSZ, &W) >= 0
	 && W.ws_col > 0 && W.ws_row > 0) {
	    TTYwidth = (int)W.ws_col;
	    TTYrows = (int)W.ws_row;
	}
#endif	/* defined(TIOCGWINSZ) */
	return;
    }
    init++;

    TTYwidth = TTYrows = 0;
#if	defined(USE_TERMCAP)
    bp = &buff[0];
    if ((term = getenv("TERM")) == NULL)
	term = "dumb";
    if (tgetent(buff, term) < 0) {
       TTYwidth = SCREEN_WIDTH;
       TTYrows = SCREEN_ROWS;
       return;
    }
    backspace = tgetstr("le", &bp);
    TTYwidth = tgetnum("co");
    TTYrows = tgetnum("li");
#endif	/* defined(USE_TERMCAP) */

#if	defined(TIOCGWINSZ)
    if (ioctl(0, TIOCGWINSZ, &W) >= 0) {
	TTYwidth = (int)W.ws_col;
	TTYrows = (int)W.ws_row;
    }
#endif	/* defined(TIOCGWINSZ) */

    if (TTYwidth <= 0 || TTYrows <= 0) {
	TTYwidth = SCREEN_WIDTH;
	TTYrows = SCREEN_ROWS;
    }
}



static void
reposition()
{
    int		i;
    CHAR	*p;

    TTYput('\r');
    TTYputs((CHAR *)Prompt);
    for (i = Point, p = Line; --i >= 0; p++)
	TTYshow(*p);
}

static void
left(Change)
    STATUS	Change;
{
    TTYback();
    if (Point) {
	if (ISCTL(Line[Point - 1]))
	    TTYback();
        else if (rl_meta_chars && ISMETA(Line[Point - 1])) {
	    TTYback();
	    TTYback();
	}
    }
    if (Change == CSmove)
	Point--;
}

static void
right(Change)
    STATUS	Change;
{
    TTYshow(Line[Point]);
    if (Change == CSmove)
	Point++;
}

static STATUS
ring_bell()
{
    TTYput('\07');
    TTYflush();
    return CSstay;
}

static STATUS
do_macro(c)
    unsigned int	c;
{
    CHAR		name[4];

    name[0] = '_';
    name[1] = c;
    name[2] = '_';
    name[3] = '\0';

    if ((Input = (CHAR *)getenv((char *)name)) == NULL) {
	Input = NIL;
	return ring_bell();
    }
    return CSstay;
}

static STATUS
do_forward(move)
    STATUS	move;
{
    int		i;
    CHAR	*p;

    i = 0;
    do {
	p = &Line[Point];
	for ( ; Point < End && (*p == ' ' || !isalnum(*p)); Point++, p++)
	    if (move == CSmove)
		right(CSstay);

	for (; Point < End && isalnum(*p); Point++, p++)
	    if (move == CSmove)
		right(CSstay);

	if (Point == End)
	    break;
    } while (++i < Repeat);

    return CSstay;
}

static STATUS
do_case(type)
    CASE	type;
{
    int		i;
    int		end;
    int		count;
    CHAR	*p;

    (void)do_forward(CSstay);
    if (OldPoint != Point) {
	if ((count = Point - OldPoint) < 0)
	    count = -count;
	Point = OldPoint;
	if ((end = Point + count) > End)
	    end = End;
	for (i = Point, p = &Line[i]; i < end; i++, p++) {
	    if (type == TOupper) {
		if (islower(*p))
		    *p = toupper(*p);
	    }
	    else if (isupper(*p))
		*p = tolower(*p);
	    right(CSmove);
	}
    }
    return CSstay;
}

static STATUS
case_down_word()
{
    return do_case(TOlower);
}

static STATUS
case_up_word()
{
    return do_case(TOupper);
}

static void
ceol()
{
    int		extras;
    int		i;
    CHAR	*p;

    for (extras = 0, i = Point, p = &Line[i]; i <= End; i++, p++) {
	TTYput(' ');
	if (ISCTL(*p)) {
	    TTYput(' ');
	    extras++;
	}
	else if (rl_meta_chars && ISMETA(*p)) {
	    TTYput(' ');
	    TTYput(' ');
	    extras += 2;
	}
    }

    for (i += extras; i > Point; i--)
	TTYback();
}

static void
clear_line()
{
    Point = -strlen(Prompt);
    TTYput('\r');
    ceol();
    Point = 0;
    End = 0;
    Line[0] = '\0';
}

static STATUS
insert_string(p)
    CHAR	*p;
{
    size_t	len;
    int		i;
    CHAR	*new;
    CHAR	*q;

    len = strlen((char *)p);
    if (End + len >= Length) {
	if ((new = NEW(CHAR, Length + len + MEM_INC)) == NULL)
	    return CSstay;
	if (Length) {
	    COPYFROMTO(new, Line, Length);
	    DISPOSE(Line);
	}
	Line = new;
	Length += len + MEM_INC;
    }

    for (q = &Line[Point], i = End - Point; --i >= 0; )
	q[len + i] = q[i];
    COPYFROMTO(&Line[Point], p, len);
    End += len;
    Line[End] = '\0';
    TTYstring(&Line[Point]);
    Point += len;

    return Point == End ? CSstay : CSmove;
}


static CHAR *
next_hist()
{
    return H.Pos >= H.Size - 1 ? NULL : H.Lines[++H.Pos];
}

static CHAR *
prev_hist()
{
    return H.Pos == 0 ? NULL : H.Lines[--H.Pos];
}

static STATUS
do_insert_hist(p)
    CHAR	*p;
{
    if (p == NULL)
	return ring_bell();
    Point = 0;
    reposition();
    ceol();
    End = 0;
    return insert_string(p);
}

static STATUS
do_hist(move)
    CHAR	*(*move)();
{
    CHAR	*p;
    int		i;

    i = 0;
    do {
	if ((p = (*move)()) == NULL)
	    return ring_bell();
    } while (++i < Repeat);
    return do_insert_hist(p);
}

static STATUS
h_next()
{
    return do_hist(next_hist);
}

static STATUS
h_prev()
{
    return do_hist(prev_hist);
}

static STATUS
h_first()
{
    return do_insert_hist(H.Lines[H.Pos = 0]);
}

static STATUS
h_last()
{
    return do_insert_hist(H.Lines[H.Pos = H.Size - 1]);
}

/*
**  Return zero if pat appears as a substring in text.
*/
static int
substrcmp(text, pat, len)
    char	*text;
    char	*pat;
    int		len;
{
    CHAR	c;

    if ((c = *pat) == '\0')
        return *text == '\0';
    for ( ; *text; text++)
        if ((CHAR)*text == c && strncmp(text, pat, len) == 0)
            return 0;
    return 1;
}

static CHAR *
search_hist(search, move)
    CHAR	*search;
    CHAR	*(*move)();
{
    static CHAR	*old_search;
    int		len;
    int		pos;
    int		(*match)();
    char	*pat;

    /* Save or get remembered search pattern. */
    if (search && *search) {
	if (old_search)
	    DISPOSE(old_search);
	old_search = (CHAR *)strdup((char *)search);
    }
    else {
	if (old_search == NULL || *old_search == '\0')
            return NULL;
	search = old_search;
    }

    /* Set up pattern-finder. */
    if (*search == '^') {
	match = strncmp;
	pat = (char *)(search + 1);
    }
    else {
	match = substrcmp;
	pat = (char *)search;
    }
    len = strlen(pat);

    for (pos = H.Pos; (*move)() != NULL; )
	if ((*match)((char *)H.Lines[H.Pos], pat, len) == 0)
            return H.Lines[H.Pos];
    H.Pos = pos;
    return NULL;
}

static STATUS
h_search()
{
    static int	Searching;
    const char	*old_prompt;
    CHAR	*(*move)();
    CHAR	*p;

    if (Searching)
	return ring_bell();
    Searching = 1;

    clear_line();
    old_prompt = Prompt;
    Prompt = "Search: ";
    TTYputs((CHAR *)Prompt);
    move = Repeat == NO_ARG ? prev_hist : next_hist;
    p = search_hist(editinput(), move);
    clear_line();
    Prompt = old_prompt;
    TTYputs((CHAR *)Prompt);

    Searching = 0;
    return do_insert_hist(p);
}

static STATUS
fd_char()
{
    int		i;

    i = 0;
    do {
	if (Point >= End)
	    break;
	right(CSmove);
    } while (++i < Repeat);
    return CSstay;
}

static void
save_yank(begin, i)
    int		begin;
    int		i;
{
    if (Yanked) {
	DISPOSE(Yanked);
	Yanked = NULL;
    }

    if (i < 1)
	return;

    if ((Yanked = NEW(CHAR, (size_t)i + 1)) != NULL) {
	COPYFROMTO(Yanked, &Line[begin], i);
	Yanked[i] = '\0';
    }
}

static STATUS
delete_string(count)
    int		count;
{
    int		i;
    CHAR	*p;

    if (count <= 0 || End == Point)
	return ring_bell();

    if (count == 1 && Point == End - 1) {
	/* Optimize common case of delete at end of line. */
	End--;
	p = &Line[Point];
	i = 1;
	TTYput(' ');
	if (ISCTL(*p)) {
	    i = 2;
	    TTYput(' ');
	}
	else if (rl_meta_chars && ISMETA(*p)) {
	    i = 3;
	    TTYput(' ');
	    TTYput(' ');
	}
	TTYbackn(i);
	*p = '\0';
	return CSmove;
    }
    if (Point + count > End && (count = End - Point) <= 0)
	return CSstay;

    if (count > 1)
	save_yank(Point, count);

    for (p = &Line[Point], i = End - (Point + count) + 1; --i >= 0; p++)
	p[0] = p[count];
    ceol();
    End -= count;
    TTYstring(&Line[Point]);
    return CSmove;
}

static STATUS
bk_char()
{
    int		i;

    i = 0;
    do {
	if (Point == 0)
	    break;
	left(CSmove);
    } while (++i < Repeat);

    return CSstay;
}

static STATUS
bk_del_char()
{
    int		i;

    i = 0;
    do {
	if (Point == 0)
	    break;
	left(CSmove);
    } while (++i < Repeat);

    return delete_string(i);
}

static STATUS
redisplay()
{
    TTYputs((CHAR *)NEWLINE);
    TTYputs((CHAR *)Prompt);
    TTYstring(Line);
    return CSmove;
}

static STATUS
kill_line()
{
    int		i;

    if (Repeat != NO_ARG) {
	if (Repeat < Point) {
	    i = Point;
	    Point = Repeat;
	    reposition();
	    (void)delete_string(i - Point);
	}
	else if (Repeat > Point) {
	    right(CSmove);
	    (void)delete_string(Repeat - Point - 1);
	}
	return CSmove;
    }

    save_yank(Point, End - Point);
    Line[Point] = '\0';
    ceol();
    End = Point;
    return CSstay;
}

static STATUS
insert_char(c)
    int		c;
{
    STATUS	s;
    CHAR	buff[2];
    CHAR	*p;
    CHAR	*q;
    int		i;

    if (Repeat == NO_ARG || Repeat < 2) {
	buff[0] = c;
	buff[1] = '\0';
	return insert_string(buff);
    }

    if ((p = NEW(CHAR, Repeat + 1)) == NULL)
	return CSstay;
    for (i = Repeat, q = p; --i >= 0; )
	*q++ = c;
    *q = '\0';
    Repeat = 0;
    s = insert_string(p);
    DISPOSE(p);
    return s;
}

static STATUS
meta()
{
    unsigned int	c;
    KEYMAP		*kp;

    if ((c = TTYget()) == EOF)
	return CSeof;
    /* Also include VT-100 arrows. */
    if (c == '[' || c == 'O')
	switch (c = TTYget()) {
	default:	return ring_bell();
	case EOF:	return CSeof;
	case 'A':	return h_prev();
	case 'B':	return h_next();
	case 'C':	return fd_char();
	case 'D':	return bk_char();
	}

    if (isdigit(c)) {
	for (Repeat = c - '0'; (c = TTYget()) != EOF && isdigit(c); )
	    Repeat = Repeat * 10 + c - '0';
	Pushed = 1;
	PushBack = c;
	return CSstay;
    }

    if (isupper(c))
	return do_macro(c);
    for (OldPoint = Point, kp = MetaMap; kp->Function; kp++)
	if (kp->Key == c)
	    return (*kp->Function)();

    return ring_bell();
}

static STATUS
emacs(c)
    unsigned int	c;
{
    STATUS		s;
    KEYMAP		*kp;

    if (ISMETA(c)) {
	Pushed = 1;
	PushBack = UNMETA(c);
	return meta();
    }
    for (kp = Map; kp->Function; kp++)
	if (kp->Key == c)
	    break;
    s = kp->Function ? (*kp->Function)() : insert_char((int)c);
    if (!Pushed)
	/* No pushback means no repeat count; hacky, but true. */
	Repeat = NO_ARG;
    return s;
}

static STATUS
TTYspecial(c)
    unsigned int	c;
{
    if (ISMETA(c))
	return CSdispatch;

    if (c == rl_erase || c == DEL)
	return bk_del_char();
    if (c == rl_kill) {
	if (Point != 0) {
	    Point = 0;
	    reposition();
	}
	Repeat = NO_ARG;
	return kill_line();
    }
    if (c == rl_intr || c == rl_quit) {
	Point = End = 0;
	Line[0] = '\0';
	return redisplay();
    }
    if (c == rl_eof && Point == 0 && End == 0)
	return CSeof;

    return CSdispatch;
}

static CHAR *
editinput()
{
    unsigned int	c;

    Repeat = NO_ARG;
    OldPoint = Point = Mark = End = 0;
    Line[0] = '\0';

    while ((c = TTYget()) != EOF)
	switch (TTYspecial(c)) {
	case CSdone:
	    return Line;
	case CSeof:
	    return NULL;
	case CSmove:
	    reposition();
	    break;
	case CSdispatch:
	    switch (emacs(c)) {
	    case CSdone:
		return Line;
	    case CSeof:
		return NULL;
	    case CSmove:
		reposition();
		break;
	    case CSdispatch:
	    case CSstay:
		break;
	    }
	    break;
	case CSstay:
	    break;
	}
    return NULL;
}

static void
hist_add(p)
    CHAR	*p;
{
    int		i;

    if ((p = (CHAR *)strdup((char *)p)) == NULL)
	return;
    if (H.Size < HIST_SIZE)
	H.Lines[H.Size++] = p;
    else {
	DISPOSE(H.Lines[0]);
	for (i = 0; i < HIST_SIZE - 1; i++)
	    H.Lines[i] = H.Lines[i + 1];
	H.Lines[i] = p;
    }
    H.Pos = H.Size - 1;
}

char *
readline(prompt)
    const char	*prompt;
{
    CHAR	*line;

    if (Line == NULL) {
	Length = MEM_INC;
	if ((Line = NEW(CHAR, Length)) == NULL)
	    return NULL;
    }

    TTYinfo();
    rl_ttyset(0);
    hist_add(NIL);
    ScreenSize = SCREEN_INC;
    Screen = NEW(char, ScreenSize);
    Prompt = prompt ? prompt : (char *)NIL;
    TTYputs((CHAR *)Prompt);
    if ((line = editinput()) != NULL) {
	line = (CHAR *)strdup((char *)line);
	TTYputs((CHAR *)NEWLINE);
	TTYflush();
    }
    rl_ttyset(1);
    DISPOSE(Screen);
    DISPOSE(H.Lines[--H.Size]);
    return (char *)line;
}

void
add_history(p)
    char	*p;
{
    if (p == NULL || *p == '\0')
	return;

#if	defined(UNIQUE_HISTORY)
    if (H.Pos && strcmp(p, H.Lines[H.Pos - 1]) == 0)
        return;
#endif	/* defined(UNIQUE_HISTORY) */
    hist_add((CHAR *)p);
}


static STATUS
beg_line()
{
    if (Point) {
	Point = 0;
	return CSmove;
    }
    return CSstay;
}

static STATUS
del_char()
{
    return delete_string(Repeat == NO_ARG ? 1 : Repeat);
}

static STATUS
end_line()
{
    if (Point != End) {
	Point = End;
	return CSmove;
    }
    return CSstay;
}

static STATUS
accept_line()
{
    Line[End] = '\0';
    return CSdone;
}

static STATUS
transpose()
{
    CHAR	c;

    if (Point) {
	if (Point == End)
	    left(CSmove);
	c = Line[Point - 1];
	left(CSstay);
	Line[Point - 1] = Line[Point];
	TTYshow(Line[Point - 1]);
	Line[Point++] = c;
	TTYshow(c);
    }
    return CSstay;
}

static STATUS
quote()
{
    unsigned int	c;

    return (c = TTYget()) == EOF ? CSeof : insert_char((int)c);
}

static STATUS
wipe()
{
    int		i;

    if (Mark > End)
	return ring_bell();

    if (Point > Mark) {
	i = Point;
	Point = Mark;
	Mark = i;
	reposition();
    }

    return delete_string(Mark - Point);
}

static STATUS
mk_set()
{
    Mark = Point;
    return CSstay;
}

static STATUS
exchange()
{
    unsigned int	c;

    if ((c = TTYget()) != CTL('X'))
	return c == EOF ? CSeof : ring_bell();

    if ((c = Mark) <= End) {
	Mark = Point;
	Point = c;
	return CSmove;
    }
    return CSstay;
}

static STATUS
yank()
{
    if (Yanked && *Yanked)
	return insert_string(Yanked);
    return CSstay;
}

static STATUS
copy_region()
{
    if (Mark > End)
	return ring_bell();

    if (Point > Mark)
	save_yank(Mark, Point - Mark);
    else
	save_yank(Point, Mark - Point);

    return CSstay;
}

static STATUS
move_to_char()
{
    unsigned int	c;
    int			i;
    CHAR		*p;

    if ((c = TTYget()) == EOF)
	return CSeof;
    for (i = Point + 1, p = &Line[i]; i < End; i++, p++)
	if (*p == c) {
	    Point = i;
	    return CSmove;
	}
    return CSstay;
}

static STATUS
fd_word()
{
    return do_forward(CSmove);
}

static STATUS
fd_kill_word()
{
    int		i;

    (void)do_forward(CSstay);
    if (OldPoint != Point) {
	i = Point - OldPoint;
	Point = OldPoint;
	return delete_string(i);
    }
    return CSstay;
}

static STATUS
bk_word()
{
    int		i;
    CHAR	*p;

    i = 0;
    do {
	for (p = &Line[Point]; p > Line && !isalnum(p[-1]); p--)
	    left(CSmove);

	for (; p > Line && p[-1] != ' ' && isalnum(p[-1]); p--)
	    left(CSmove);

	if (Point == 0)
	    break;
    } while (++i < Repeat);

    return CSstay;
}

static STATUS
bk_kill_word()
{
    (void)bk_word();
    if (OldPoint != Point)
	return delete_string(OldPoint - Point);
    return CSstay;
}

static int
argify(line, avp)
    CHAR	*line;
    CHAR	***avp;
{
    CHAR	*c;
    CHAR	**p;
    CHAR	**new;
    int		ac;
    int		i;

    i = MEM_INC;
    if ((*avp = p = NEW(CHAR*, i))== NULL)
	 return 0;

    for (c = line; isspace(*c); c++)
	continue;
    if (*c == '\n' || *c == '\0')
	return 0;

    for (ac = 0, p[ac++] = c; *c && *c != '\n'; ) {
	if (isspace(*c)) {
	    *c++ = '\0';
	    if (*c && *c != '\n') {
		if (ac + 1 == i) {
		    new = NEW(CHAR*, i + MEM_INC);
		    if (new == NULL) {
			p[ac] = NULL;
			return ac;
		    }
		    COPYFROMTO(new, p, i * sizeof (char **));
		    i += MEM_INC;
		    DISPOSE(p);
		    *avp = p = new;
		}
		p[ac++] = c;
	    }
	}
	else
	    c++;
    }
    *c = '\0';
    p[ac] = NULL;
    return ac;
}

static STATUS
last_argument()
{
    CHAR	**av;
    CHAR	*p;
    STATUS	s;
    int		ac;

    if (H.Size == 1 || (p = H.Lines[H.Size - 2]) == NULL)
	return ring_bell();

    if ((p = (CHAR *)strdup((char *)p)) == NULL)
	return CSstay;
    ac = argify(p, &av);

    if (Repeat != NO_ARG)
	s = Repeat < ac ? insert_string(av[Repeat]) : ring_bell();
    else
	s = ac ? insert_string(av[ac - 1]) : CSstay;

    if (ac)
	DISPOSE(av);
    DISPOSE(p);
    return s;
}

static KEYMAP	Map[33] = {
    {	CTL('@'),	ring_bell	},
    {	CTL('A'),	beg_line	},
    {	CTL('B'),	bk_char		},
    {	CTL('D'),	del_char	},
    {	CTL('E'),	end_line	},
    {	CTL('F'),	fd_char		},
    {	CTL('G'),	ring_bell	},
    {	CTL('H'),	bk_del_char	},
    {	CTL('I'),	ring_bell	},
    {	CTL('J'),	accept_line	},
    {	CTL('K'),	kill_line	},
    {	CTL('L'),	redisplay	},
    {	CTL('M'),	accept_line	},
    {	CTL('N'),	h_next		},
    {	CTL('O'),	ring_bell	},
    {	CTL('P'),	h_prev		},
    {	CTL('Q'),	ring_bell	},
    {	CTL('R'),	h_search	},
    {	CTL('S'),	ring_bell	},
    {	CTL('T'),	transpose	},
    {	CTL('U'),	ring_bell	},
    {	CTL('V'),	quote		},
    {	CTL('W'),	wipe		},
    {	CTL('X'),	exchange	},
    {	CTL('Y'),	yank		},
    {	CTL('Z'),	ring_bell	},
    {	CTL('['),	meta		},
    {	CTL(']'),	move_to_char	},
    {	CTL('^'),	ring_bell	},
    {	CTL('_'),	ring_bell	},
    {	0,		NULL		}
};

static KEYMAP	MetaMap[16]= {
    {	CTL('H'),	bk_kill_word	},
    {	DEL,		bk_kill_word	},
    {	' ',		mk_set	},
    {	'.',		last_argument	},
    {	'<',		h_first		},
    {	'>',		h_last		},
    {	'?',		ring_bell	},
    {	'b',		bk_word		},
    {	'd',		fd_kill_word	},
    {	'f',		fd_word		},
    {	'l',		case_down_word	},
    {	'u',		case_up_word	},
    {	'y',		yank		},
    {	'w',		copy_region	},
    {	0,		NULL		}
};
