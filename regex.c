/* Extended regular expression matching and search library.
   Copyright (C) 1985, 1989-90 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 1, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */
/* Multi-byte extension added May, 1993 by t^2 (Takahiro Tanimoto)
   Last change: May 21, 1993 by t^2  */


/* To test, compile with -Dtest.  This Dtestable feature turns this into
   a self-contained program which reads a pattern, describes how it
   compiles, then reads a string and searches for it.

   On the other hand, if you compile with both -Dtest and -Dcanned you
   can run some tests we've already thought of.  */

/* We write fatal error messages on standard error.  */
#include <stdio.h>

/* isalpha(3) etc. are used for the character classes.  */
#include <ctype.h>
#include <sys/types.h>

#include "config.h"
#include "defines.h"

#ifdef __STDC__
#define P(s)    s
#define MALLOC_ARG_T size_t
#else
#define P(s)    ()
#define MALLOC_ARG_T unsigned
#define volatile
#define const
#endif

/* #define	NO_ALLOCA	/* try it out for now */
#ifndef NO_ALLOCA
/* Make alloca work the best possible way.  */
#ifdef __GNUC__
#ifndef atarist
#ifndef alloca
#define alloca __builtin_alloca
#endif
#endif /* atarist */
#else
#if defined(HAVE_ALLOCA_H) && !defined(__GNUC__)
#include <alloca.h>
#else
char *alloca();
#endif
#endif /* __GNUC__ */

#ifdef _AIX
#pragma alloca
#endif

#define RE_ALLOCATE alloca
#define FREE_VARIABLES() alloca(0)

#define FREE_AND_RETURN_VOID(stackb)	return
#define FREE_AND_RETURN(stackb,val)	return(val)
#define DOUBLE_STACK(stackx,stackb,len) \
        (stackx = (unsigned char **) alloca(2 * len			\
                                            * sizeof(unsigned char *)),\
	/* Only copy what is in use.  */				\
        (unsigned char **) memcpy(stackx, stackb, len * sizeof (char *)))
#else  /* NO_ALLOCA defined */

#define RE_ALLOCATE malloc

#define FREE_VAR(var) if (var) free(var); var = NULL
#define FREE_VARIABLES()						\
  do {									\
    FREE_VAR(regstart);							\
    FREE_VAR(regend);							\
    FREE_VAR(best_regstart);						\
    FREE_VAR(best_regend);						\
    FREE_VAR(reg_info);							\
  } while (0)

#define FREE_AND_RETURN_VOID(stackb)   free(stackb);return
#define FREE_AND_RETURN(stackb,val)    free(stackb);return(val)
#define DOUBLE_STACK(stackx,stackb,len) \
        (unsigned char **)xrealloc(stackb, 2 * len * sizeof(unsigned char *))
#endif /* NO_ALLOCA */

#define RE_TALLOC(n,t)  ((t*)RE_ALLOCATE((n)*sizeof(t)))
#define TMALLOC(n,t)    ((t*)xmalloc((n)*sizeof(t)))
#define TREALLOC(s,n,t) (s=((t*)xrealloc(s,(n)*sizeof(t))))

/* Get the interface, including the syntax bits.  */
#include "regex.h"

static void store_jump P((char *, int, char *));
static void insert_jump P((int, char *, char *, char *));
static void store_jump_n P((char *, int, char *, unsigned));
static void insert_jump_n P((int, char *, char *, char *, unsigned));
static void insert_op_2 P((int, char *, char *, int, int ));
static int memcmp_translate P((unsigned char *, unsigned char *,
			       int, unsigned char *));

/* Define the syntax stuff, so we can do the \<, \>, etc.  */

/* This must be nonzero for the wordchar and notwordchar pattern
   commands in re_match_2.  */
#ifndef Sword 
#define Sword 1
#endif

#define SYNTAX(c) re_syntax_table[c]

static char re_syntax_table[256];
static void init_syntax_once P((void));

#undef P

#include "util.h"

static void
init_syntax_once()
{
   register int c;
   static int done = 0;

   if (done)
     return;

   memset(re_syntax_table, 0, sizeof re_syntax_table);

   for (c = 'a'; c <= 'z'; c++)
     re_syntax_table[c] = Sword;

   for (c = 'A'; c <= 'Z'; c++)
     re_syntax_table[c] = Sword;

   for (c = '0'; c <= '9'; c++)
     re_syntax_table[c] = Sword;

   re_syntax_table['_'] = Sword;

   /* Add specific syntax for ISO Latin-1.  */
   for (c = 0300; c <= 0377; c++)
     re_syntax_table[c] = Sword;
   re_syntax_table[0327] = 0;
   re_syntax_table[0367] = 0;

   done = 1;
}

/* Sequents are missing isgraph.  */
#ifndef isgraph
#define isgraph(c) (isprint((c)) && !isspace((c)))
#endif

/* These are the command codes that appear in compiled regular
   expressions, one per byte.  Some command codes are followed by
   argument bytes.  A command code can specify any interpretation
   whatsoever for its arguments.  Zero-bytes may appear in the compiled
   regular expression.

   The value of `exactn' is needed in search.c (search_buffer) in emacs.
   So regex.h defines a symbol `RE_EXACTN_VALUE' to be 1; the value of
   `exactn' we use here must also be 1.  */

enum regexpcode
  {
    unused=0,
    exactn=1, /* Followed by one byte giving n, then by n literal bytes.  */
    begline,  /* Fail unless at beginning of line.  */
    endline,  /* Fail unless at end of line.  */
    jump,     /* Followed by two bytes giving relative address to jump to.  */
    on_failure_jump,	 /* Followed by two bytes giving relative address of 
			    place to resume at in case of failure.  */
    finalize_jump,	 /* Throw away latest failure point and then jump to 
			    address.  */
    maybe_finalize_jump, /* Like jump but finalize if safe to do so.
			    This is used to jump back to the beginning
			    of a repeat.  If the command that follows
			    this jump is clearly incompatible with the
			    one at the beginning of the repeat, such that
			    we can be sure that there is no use backtracking
			    out of repetitions already completed,
			    then we finalize.  */
    dummy_failure_jump,  /* Jump, and push a dummy failure point. This 
			    failure point will be thrown away if an attempt 
                            is made to use it for a failure. A + construct 
                            makes this before the first repeat.  Also
                            use it as an intermediary kind of jump when
                            compiling an or construct.  */
    succeed_n,	 /* Used like on_failure_jump except has to succeed n times;
		    then gets turned into an on_failure_jump. The relative
                    address following it is useless until then.  The
                    address is followed by two bytes containing n.  */
    jump_n,	 /* Similar to jump, but jump n times only; also the relative
		    address following is in turn followed by yet two more bytes
                    containing n.  */
    set_number_at,	/* Set the following relative location to the
			   subsequent number.  */
    anychar,	 /* Matches any (more or less) one character.  */
    charset,     /* Matches any one char belonging to specified set.
		    First following byte is number of bitmap bytes.
		    Then come bytes for a bitmap saying which chars are in.
		    Bits in each byte are ordered low-bit-first.
		    A character is in the set if its bit is 1.
		    A character too large to have a bit in the map
		    is automatically not in the set.  */
    charset_not, /* Same parameters as charset, but match any character
                    that is not one of those specified.  */
    start_memory, /* Start remembering the text that is matched, for
		    storing in a memory register.  Followed by one
                    byte containing the register number.  Register numbers
                    must be in the range 0 through RE_NREGS.  */
    stop_memory, /* Stop remembering the text that is matched
		    and store it in a memory register.  Followed by
                    one byte containing the register number. Register
                    numbers must be in the range 0 through RE_NREGS.  */
    duplicate,   /* Match a duplicate of something remembered.
		    Followed by one byte containing the index of the memory 
                    register.  */
    wordchar,    /* Matches any word-constituent character.  */
    notwordchar, /* Matches any char that is not a word-constituent.  */
    wordbound,   /* Succeeds if at a word boundary.  */
    notwordbound,/* Succeeds if not at a word boundary.  */
  };


/* Number of failure points to allocate space for initially,
   when matching.  If this number is exceeded, more space is allocated,
   so it is not a hard limit.  */

#ifndef NFAILURES
#define NFAILURES 80
#endif

#if defined(CHAR_UNSIGNED) || defined(__CHAR_UNSIGNED__)
#define SIGN_EXTEND_CHAR(c) ((c)>(char)127?(c)-256:(c)) /* for IBM RT */
#endif
#ifndef SIGN_EXTEND_CHAR
#define SIGN_EXTEND_CHAR(x) (x)
#endif


/* Store NUMBER in two contiguous bytes starting at DESTINATION.  */
#define STORE_NUMBER(destination, number)				\
  { (destination)[0] = (number) & 0377;					\
    (destination)[1] = (number) >> 8; }

/* Same as STORE_NUMBER, except increment the destination pointer to
   the byte after where the number is stored.  Watch out that values for
   DESTINATION such as p + 1 won't work, whereas p will.  */
#define STORE_NUMBER_AND_INCR(destination, number)			\
  { STORE_NUMBER(destination, number);					\
    (destination) += 2; }


/* Put into DESTINATION a number stored in two contingous bytes starting
   at SOURCE.  */
#define EXTRACT_NUMBER(destination, source)				\
  { (destination) = *(source) & 0377;					\
    (destination) += SIGN_EXTEND_CHAR (*(char *)((source) + 1)) << 8; }

/* Same as EXTRACT_NUMBER, except increment the pointer for source to
   point to second byte of SOURCE.  Note that SOURCE has to be a value
   such as p, not, e.g., p + 1. */
#define EXTRACT_NUMBER_AND_INCR(destination, source)			\
  { EXTRACT_NUMBER(destination, source);				\
    (source) += 2; }


/* Specify the precise syntax of regexps for compilation.  This provides
   for compatibility for various utilities which historically have
   different, incompatible syntaxes.

   The argument SYNTAX is a bit-mask comprised of the various bits
   defined in regex.h.  */

long
re_set_syntax(syntax)
  long syntax;
{
  long ret;

  ret = re_syntax_options;
  re_syntax_options = syntax;
  return ret;
}

/* Set by re_set_syntax to the current regexp syntax to recognize.  */
long re_syntax_options = DEFAULT_MBCTYPE;


/* Macros for re_compile_pattern, which is found below these definitions.  */

/* Fetch the next character in the uncompiled pattern---translating it 
   if necessary.  Also cast from a signed character in the constant
   string passed to us by the user to an unsigned char that we can use
   as an array index (in, e.g., `translate').  */
#define PATFETCH(c)							\
  do {if (p == pend) goto end_of_pattern;				\
    c = (unsigned char) *p++; 						\
    if (translate) c = (unsigned char)translate[c];			\
  } while (0)

/* Fetch the next character in the uncompiled pattern, with no
   translation.  */
#define PATFETCH_RAW(c)							\
  do {if (p == pend) goto end_of_pattern;				\
    c = (unsigned char) *p++; 						\
  } while (0)

/* Go backwards one character in the pattern.  */
#define PATUNFETCH p--


/* If the buffer isn't allocated when it comes in, use this.  */
#define INIT_BUF_SIZE  28

/* Make sure we have at least N more bytes of space in buffer.  */
#define GET_BUFFER_SPACE(n)						\
  {								        \
    while (b - bufp->buffer + (n) >= bufp->allocated)			\
      EXTEND_BUFFER;							\
  }

/* Make sure we have one more byte of buffer space and then add CH to it.  */
#define BUFPUSH(ch)							\
  {									\
    GET_BUFFER_SPACE(1);						\
    *b++ = (char)(ch);							\
  }

/* Extend the buffer by twice its current size via reallociation and
   reset the pointers that pointed into the old allocation to point to
   the correct places in the new allocation.  If extending the buffer
   results in it being larger than 1 << 16, then flag memory exhausted.  */
#define EXTEND_BUFFER							\
  { char *old_buffer = bufp->buffer;					\
    if (bufp->allocated == (1L<<16)) goto too_big;			\
    bufp->allocated *= 2;						\
    if (bufp->allocated > (1L<<16)) bufp->allocated = (1L<<16);		\
    bufp->buffer = (char *) xrealloc (bufp->buffer, bufp->allocated);	\
    if (bufp->buffer == 0)						\
      goto memory_exhausted;						\
    b = (b - old_buffer) + bufp->buffer;				\
    if (fixup_jump)							\
      fixup_jump = (fixup_jump - old_buffer) + bufp->buffer;		\
    if (laststart)							\
      laststart = (laststart - old_buffer) + bufp->buffer;		\
    begalt = (begalt - old_buffer) + bufp->buffer;			\
    if (pending_exact)							\
      pending_exact = (pending_exact - old_buffer) + bufp->buffer;	\
  }


/* Set the bit for character C in a character set list.  */
#define SET_LIST_BIT(c)							\
  (b[(unsigned char)(c) / BYTEWIDTH]					\
   |= 1 << ((unsigned char)(c) % BYTEWIDTH))

/* Get the next unsigned number in the uncompiled pattern.  */
#define GET_UNSIGNED_NUMBER(num) 					\
  { if (p != pend) 							\
      { 								\
        PATFETCH(c); 							\
	while (isdigit(c)) 						\
	  { 								\
	    if (num < 0) 						\
	       num = 0; 						\
            num = num * 10 + c - '0'; 					\
	    if (p == pend) 						\
	       break; 							\
	    PATFETCH(c); 						\
	  } 								\
        } 								\
  }

/* Subroutines for re_compile_pattern.  */
/* static void store_jump(), insert_jump(), store_jump_n(),
	    insert_jump_n(), insert_op_2(); */

#define STORE_MBC(p, c) \
  ((p)[0] = (unsigned char)(c >> 8), (p)[1] = (unsigned char)(c))
#define STORE_MBC_AND_INCR(p, c) \
  (*(p)++ = (unsigned char)(c >> 8), *(p)++ = (unsigned char)(c))

#define EXTRACT_MBC(p) \
  ((unsigned short)((unsigned char)(p)[0] << 8 | (unsigned char)(p)[1]))
#define EXTRACT_MBC_AND_INCR(p) \
  ((unsigned short)((p) += 2, (unsigned char)(p)[-2] << 8 | (unsigned char)(p)[-1]))

#define EXTRACT_UNSIGNED(p) \
  ((unsigned char)(p)[0] | (unsigned char)(p)[1] << 8)
#define EXTRACT_UNSIGNED_AND_INCR(p) \
  ((p) += 2, (unsigned char)(p)[-2] | (unsigned char)(p)[-1] << 8)

/* Handle (mb)?charset(_not)?.

   Structure of mbcharset(_not)? in compiled pattern.

     struct {
       unsinged char id;		mbcharset(_not)?
       unsigned char sbc_size;
       unsigned char sbc_map[sbc_size];	same as charset(_not)? up to here.
       unsigned short mbc_size;		number of intervals.
       struct {
	 unsigned short beg;		beginning of interval.
	 unsigned short end;		end of interval.
       } intervals[mbc_size];
     }; */

static void
set_list_bits(c1, c2, b)
    unsigned short c1, c2;
    unsigned char *b;
{
  unsigned char sbc_size = b[-1];
  unsigned short mbc_size = EXTRACT_UNSIGNED(&b[sbc_size]);
  unsigned short beg, end, upb;

  if (c1 > c2)
    return;
  if ((int)c1 < 1 << BYTEWIDTH) {
    upb = c2;
    if (1 << BYTEWIDTH <= (int)upb)
      upb = (1 << BYTEWIDTH) - 1;	/* The last single-byte char */
    if (sbc_size <= (unsigned short)(upb / BYTEWIDTH)) {
      /* Allocate maximum size so it never happens again.  */
      /* NOTE: memcpy() would not work here.  */
      memmove(&b[(1 << BYTEWIDTH) / BYTEWIDTH], &b[sbc_size], 2 + mbc_size*4);
      memset(&b[sbc_size], 0, (1 << BYTEWIDTH) / BYTEWIDTH - sbc_size);
      b[-1] = sbc_size = (1 << BYTEWIDTH) / BYTEWIDTH;
    }
    for (; c1 <= upb; c1++)
	if (!ismbchar(c1))
	    SET_LIST_BIT(c1);
    if ((int)c2 < 1 << BYTEWIDTH)
      return;
    c1 = 0x8000;			/* The first wide char */
  }
  b = &b[sbc_size + 2];

  for (beg = 0, upb = mbc_size; beg < upb; ) {
    unsigned short mid = (unsigned short)(beg + upb) >> 1;

    if ((int)c1 - 1 > (int)EXTRACT_MBC(&b[mid*4 + 2]))
      beg = mid + 1;
    else
      upb = mid;
  }

  for (end = beg, upb = mbc_size; end < upb; ) {
    unsigned short mid = (unsigned short)(end + upb) >> 1;

    if ((int)c2 >= (int)EXTRACT_MBC(&b[mid*4]) - 1)
      end = mid + 1;
    else
      upb = mid;
  }

  if (beg != end) {
    if (c1 > EXTRACT_MBC(&b[beg*4]))
      c1 = EXTRACT_MBC(&b[beg*4]);
    if (c2 < EXTRACT_MBC(&b[(end - 1)*4]))
      c2 = EXTRACT_MBC(&b[(end - 1)*4]);
  }
  if (end < mbc_size && end != beg + 1)
    /* NOTE: memcpy() would not work here.  */
    memmove(&b[(beg + 1)*4], &b[end*4], (mbc_size - end)*4);
  STORE_MBC(&b[beg*4 + 0], c1);
  STORE_MBC(&b[beg*4 + 2], c2);
  mbc_size += beg - end + 1;
  STORE_NUMBER(&b[-2], mbc_size);
}

static int
is_in_list(c, b)
    unsigned short c;
    const unsigned char *b;
{
    unsigned short size;
    unsigned short i, j;
    int result = 0;

    size = *b++;
    if ((int)c < 1<<BYTEWIDTH) {
	if ((int)c / BYTEWIDTH < (int)size && b[c / BYTEWIDTH] & 1 << c % BYTEWIDTH) {
	    return 1;
	}
    }
    b += size + 2;
    size = EXTRACT_UNSIGNED(&b[-2]);
    if (size == 0) return 0;

    if (b[(size-1)*4] == 0xff) {
	i = c;
	if ((int)c >= 1<<BYTEWIDTH) {
	    i = i>>BYTEWIDTH;
	}
	while (size>0 && b[size*4-2] == 0xff) {
	    size--;
	    if (b[size*4+1] <= i && i <= b[size*4+3]) {
		result = 2;
		break;
	    }
	}
    }
    for (i = 0, j = size; i < j; ) {
	unsigned short k = (unsigned short)(i + j) >> 1;

	if (c > EXTRACT_MBC(&b[k*4+2]))
	    i = k + 1;
	else
	    j = k;
    }
    if (i < size && EXTRACT_MBC(&b[i*4]) <= c
	&& ((unsigned char)c != '\n' && (unsigned char)c != '\0'))
	return 1;
    return result;
}

/* re_compile_pattern takes a regular-expression string
   and converts it into a buffer full of byte commands for matching.

   PATTERN   is the address of the pattern string
   SIZE      is the length of it.
   BUFP	    is a  struct re_pattern_buffer *  which points to the info
	     on where to store the byte commands.
	     This structure contains a  char *  which points to the
	     actual space, which should have been obtained with malloc.
	     re_compile_pattern may use realloc to grow the buffer space.

   The number of bytes of commands can be found out by looking in
   the `struct re_pattern_buffer' that bufp pointed to, after
   re_compile_pattern returns. */

char *
re_compile_pattern(pattern, size, bufp)
     char *pattern;
     size_t size;
     struct re_pattern_buffer *bufp;
{
    register char *b = bufp->buffer;
    register char *p = pattern;
    char *pend = pattern + size;
    register unsigned c, c1;
    char *p0;
    int numlen;

    /* Address of the count-byte of the most recently inserted `exactn'
       command.  This makes it possible to tell whether a new exact-match
       character can be added to that command or requires a new `exactn'
       command.  */

    char *pending_exact = 0;

    /* Address of the place where a forward-jump should go to the end of
       the containing expression.  Each alternative of an `or', except the
       last, ends with a forward-jump of this sort.  */

    char *fixup_jump = 0;

    /* Address of start of the most recently finished expression.
       This tells postfix * where to find the start of its operand.  */

    char *laststart = 0;

    /* In processing a repeat, 1 means zero matches is allowed.  */

    char zero_times_ok;

    /* In processing a repeat, 1 means many matches is allowed.  */

    char many_times_ok;

    /* Address of beginning of regexp, or inside of last \(.  */

    char *begalt = b;

    /* In processing an interval, at least this many matches must be made.  */
    int lower_bound;

    /* In processing an interval, at most this many matches can be made.  */
    int upper_bound;

    /* Place in pattern (i.e., the {) to which to go back if the interval
       is invalid.  */
    char *beg_interval = 0;

    /* Stack of information saved by \( and restored by \).
       Four stack elements are pushed by each \(:
       First, the value of b.
       Second, the value of fixup_jump.
       Third, the value of regnum.
       Fourth, the value of begalt.  */

    int stackb[40];
    int *stackp = stackb;
    int *stacke = stackb + 40;
    int *stackt;

    /* Counts \('s as they are encountered.  Remembered for the matching \),
       where it becomes the register number to put in the stop_memory
       command.  */

    int regnum = 1;
    int range = 0;

    /* How to translate the characters in the pattern.  */
    char *translate = bufp->translate;

    bufp->fastmap_accurate = 0;

    /* Initialize the syntax table.  */
    init_syntax_once();

    if (bufp->allocated == 0)
	{
	    bufp->allocated = INIT_BUF_SIZE;
	    if (bufp->buffer)
		/* EXTEND_BUFFER loses when bufp->allocated is 0.  */
		bufp->buffer = (char *) xrealloc (bufp->buffer, INIT_BUF_SIZE);
	    else
		/* Caller did not allocate a buffer.  Do it for them.  */
	bufp->buffer = (char *) xmalloc(INIT_BUF_SIZE);
      if (!bufp->buffer) goto memory_exhausted;
      begalt = b = bufp->buffer;
    }

  while (p != pend)
    {
      PATFETCH(c);

      switch (c)
	{
	case '$':
	  {
	    char *p1 = p;
	    /* When testing what follows the $,
	       look past the \-constructs that don't consume anything.  */
	    if (! (re_syntax_options & RE_CONTEXT_INDEP_OPS))
	      while (p1 != pend)
		{
		  if (*p1 == '\\' && p1 + 1 != pend
		      && (p1[1] == 'b' || p1[1] == 'B'))
		    p1 += 2;
		  else
		    break;
		}
            if (re_syntax_options & RE_TIGHT_VBAR)
	      {
		if (! (re_syntax_options & RE_CONTEXT_INDEP_OPS) && p1 != pend)
		  goto normal_char;
		/* Make operand of last vbar end before this `$'.  */
		if (fixup_jump)
		  store_jump(fixup_jump, jump, b);
		fixup_jump = 0;
		BUFPUSH(endline);
		break;
	      }
	    /* $ means succeed if at end of line, but only in special contexts.
	      If validly in the middle of a pattern, it is a normal character. */

#if 0
	    /* not needed for perl4 compatible */
            if ((re_syntax_options & RE_CONTEXTUAL_INVALID_OPS) && p1 != pend)
	      goto invalid_pattern;
#endif
	    if (p1 == pend || *p1 == '\n'
		|| (re_syntax_options & RE_CONTEXT_INDEP_OPS)
		|| (re_syntax_options & RE_NO_BK_PARENS
		    ? *p1 == ')'
		    : *p1 == '\\' && p1[1] == ')')
		|| (re_syntax_options & RE_NO_BK_VBAR
		    ? *p1 == '|'
		    : *p1 == '\\' && p1[1] == '|'))
	      {
		BUFPUSH(endline);
		break;
	      }
	    goto normal_char;
          }
	case '^':
	  /* ^ means succeed if at beg of line, but only if no preceding 
             pattern.  */

          if ((re_syntax_options & RE_CONTEXTUAL_INVALID_OPS) && laststart)
            goto invalid_pattern;
          if (laststart && p - 2 >= pattern && p[-2] != '\n'
	       && !(re_syntax_options & RE_CONTEXT_INDEP_OPS))
	    goto normal_char;
	  if (re_syntax_options & RE_TIGHT_VBAR)
	    {
	      if (p != pattern + 1
		  && ! (re_syntax_options & RE_CONTEXT_INDEP_OPS))
		goto normal_char;
	      BUFPUSH(begline);
	      begalt = b;
	    }
	  else
	    {
	      BUFPUSH(begline);
	    }
	  break;

	case '+':
	case '?':
	  if ((re_syntax_options & RE_BK_PLUS_QM)
	      || (re_syntax_options & RE_LIMITED_OPS))
	    goto normal_char;
	handle_plus:
	case '*':
	  /* If there is no previous pattern, char not special. */
	  if (!laststart)
            {
              if (re_syntax_options & RE_CONTEXTUAL_INVALID_OPS)
                goto invalid_pattern;
              else if (! (re_syntax_options & RE_CONTEXT_INDEP_OPS))
		goto normal_char;
            }
	  /* If there is a sequence of repetition chars,
	     collapse it down to just one.  */
	  zero_times_ok = 0;
	  many_times_ok = 0;
	  while (1)
	    {
	      zero_times_ok |= c != '+';
	      many_times_ok |= c != '?';
	      if (p == pend)
		break;
	      PATFETCH(c);
	      if (c == '*')
		;
	      else if (!(re_syntax_options & RE_BK_PLUS_QM)
		       && (c == '+' || c == '?'))
		;
	      else if ((re_syntax_options & RE_BK_PLUS_QM)
		       && c == '\\')
		{
		  /* int c1; */
		  PATFETCH(c1);
		  if (!(c1 == '+' || c1 == '?'))
		    {
		      PATUNFETCH;
		      PATUNFETCH;
		      break;
		    }
		  c = c1;
		}
	      else
		{
		  PATUNFETCH;
		  break;
		}
	    }

	  /* Star, etc. applied to an empty pattern is equivalent
	     to an empty pattern.  */
	  if (!laststart)  
	    break;

	  /* Now we know whether or not zero matches is allowed
	     and also whether or not two or more matches is allowed.  */
	  if (many_times_ok)
	    {
	      /* If more than one repetition is allowed, put in at the
                 end a backward relative jump from b to before the next
                 jump we're going to put in below (which jumps from
                 laststart to after this jump).  */
              GET_BUFFER_SPACE(3);
	      store_jump(b, maybe_finalize_jump, laststart - 3);
	      b += 3;  	/* Because store_jump put stuff here.  */
	    }
          /* On failure, jump from laststart to b + 3, which will be the
             end of the buffer after this jump is inserted.  */
          GET_BUFFER_SPACE(3);
	  insert_jump(on_failure_jump, laststart, b + 3, b);
	  pending_exact = 0;
	  b += 3;
	  if (!zero_times_ok)
	    {
	      /* At least one repetition is required, so insert a
                 dummy-failure before the initial on-failure-jump
                 instruction of the loop. This effects a skip over that
                 instruction the first time we hit that loop.  */
              GET_BUFFER_SPACE(6);
              insert_jump(dummy_failure_jump, laststart, laststart + 6, b);
	      b += 3;
	    }
	  break;

	case '.':
	  laststart = b;
	  BUFPUSH(anychar);
	  break;

        case '[':
          if (p == pend)
            goto invalid_pattern;
	  while (b - bufp->buffer
		 > bufp->allocated - 9 - (1 << BYTEWIDTH) / BYTEWIDTH)
	    EXTEND_BUFFER;

	  laststart = b;
	  if (*p == '^')
	    {
              BUFPUSH(charset_not); 
              p++;
            }
	  else
	    BUFPUSH(charset);
	  p0 = p;

	  BUFPUSH((1 << BYTEWIDTH) / BYTEWIDTH);
	  /* Clear the whole map */
	  memset(b, 0, (1 << BYTEWIDTH) / BYTEWIDTH + 2);

	  if ((re_syntax_options & RE_HAT_NOT_NEWLINE) && b[-2] == charset_not)
            SET_LIST_BIT('\n');


	  /* Read in characters and ranges, setting map bits.  */
	  while (1)
	    {
	      int size;
	      unsigned last = (unsigned)-1;

	      if ((size = EXTRACT_UNSIGNED(&b[(1 << BYTEWIDTH) / BYTEWIDTH]))) {
		/* Ensure the space is enough to hold another interval
		   of multi-byte chars in charset(_not)?.  */
		size = (1 << BYTEWIDTH) / BYTEWIDTH + 2 + size*4 + 4;
		while (b + size + 1 > bufp->buffer + bufp->allocated)
		  EXTEND_BUFFER;
	      }
	    range_retry:
	      PATFETCH(c);

              if (c == ']') {
                  if (p == p0 + 1) {
		      /* If this is an empty bracket expression.  */
                      if ((re_syntax_options & RE_NO_EMPTY_BRACKETS) 
                          && p == pend)
			  goto invalid_pattern;
		  }
                  else 
		    /* Stop if this isn't merely a ] inside a bracket
                       expression, but rather the end of a bracket
                       expression.  */
		      break;
	      }
	      if (ismbchar(c)) {
		PATFETCH(c1);
		c = c << BYTEWIDTH | c1;
	      }

	      /* \ escapes characters when inside [...].  */
	      if (c == '\\') {
	          PATFETCH(c);
		  switch (c) {
		    case 'w':
		      for (c = 0; c < (1 << BYTEWIDTH); c++)
		          if (SYNTAX(c) == Sword)
			      SET_LIST_BIT(c);
		      last = -1;
		      continue;

		    case 'W':
		      for (c = 0; c < (1 << BYTEWIDTH); c++)
		          if (SYNTAX(c) != Sword)
			      SET_LIST_BIT(c);
		      if (re_syntax_options & RE_MBCTYPE_MASK) {
			  set_list_bits(0x8000, 0xffff, (unsigned char*)b);
		      }
		      last = -1;
		      continue;

		    case 's':
		      for (c = 0; c < 256; c++)
			  if (isspace(c))
			      SET_LIST_BIT(c);
		      last = -1;
		      continue;

		    case 'S':
		      for (c = 0; c < 256; c++)
			  if (!isspace(c))
			      SET_LIST_BIT(c);
		      if (re_syntax_options & RE_MBCTYPE_MASK) {
			  set_list_bits(0x8000, 0xffff, (unsigned char*)b);
		      }
		      last = -1;
		      continue;

		    case 'd':
		      for (c = '0'; c <= '9'; c++)
			  SET_LIST_BIT(c);
		      last = -1;
		      continue;

		    case 'D':
		      for (c = 0; c < 256; c++)
			  if (!isdigit(c))
			      SET_LIST_BIT(c);
		      if (re_syntax_options & RE_MBCTYPE_MASK) {
			  set_list_bits(0x8000, 0xffff, (unsigned char*)b);
		      }
		      last = -1;
		      continue;

		    case 'x':
		      c = scan_hex(p, 2, &numlen);
		      if ((re_syntax_options & RE_MBCTYPE_MASK) && c > 0x7f)
			  c = 0xff00 | c;
		      p += numlen;
		      break;

		    case '0': case '1': case '2': case '3': case '4':
		    case '5': case '6': case '7': case '8': case '9':
		      PATUNFETCH;
		      c = scan_oct(p, 3, &numlen);
		      if ((re_syntax_options & RE_MBCTYPE_MASK) && ismbchar(c))
			  c = 0xff00 | c;
		      p += numlen;
		      break;

		    default:
		      if (ismbchar(c)) {
			  PATFETCH(c1);
			  c = c << 8 | c1;
		      }
		      break;
		  }
	      }

              /* Get a range.  */
	      if (range) {
		  if (last > c)
                    goto invalid_pattern;

		  if ((re_syntax_options & RE_NO_HYPHEN_RANGE_END) 
                      && c == '-' && *p != ']')
                    goto invalid_pattern;

		  range = 0;
		  if (last < 1 << BYTEWIDTH && c < 1 << BYTEWIDTH) {
		      for (;last<=c;last++)
			  SET_LIST_BIT(last);
		  }
		  else {
		      set_list_bits(last, c, (unsigned char*)b);
		  }
	      }
              else if (p[0] == '-' && p[1] != ']') {
		  last = c;
		  PATFETCH(c1);
		  range = 1;
		  goto range_retry;
	      }
              else if (c < 1 << BYTEWIDTH)
		SET_LIST_BIT(c);
	      else
		set_list_bits(c, c, (unsigned char*)b);
	    }

          /* Discard any character set/class bitmap bytes that are all
             0 at the end of the map. Decrement the map-length byte too.  */
          while ((int) b[-1] > 0 && b[b[-1] - 1] == 0) 
            b[-1]--; 
	  if (b[-1] != (1 << BYTEWIDTH) / BYTEWIDTH)
	    memmove(&b[b[-1]], &b[(1 << BYTEWIDTH) / BYTEWIDTH],
		    2 + EXTRACT_UNSIGNED (&b[(1 << BYTEWIDTH) / BYTEWIDTH])*4);
	  b += b[-1] + 2 + EXTRACT_UNSIGNED (&b[b[-1]])*4;
          break;

	case '(':
	  if (! (re_syntax_options & RE_NO_BK_PARENS))
	    goto normal_char;
	  else
	    goto handle_open;

	case ')':
	  if (! (re_syntax_options & RE_NO_BK_PARENS))
	    goto normal_char;
	  else
	    goto handle_close;

        case '\n':
	  if (! (re_syntax_options & RE_NEWLINE_OR))
	    goto normal_char;
	  else
	    goto handle_bar;

	case '|':
#if 0
	  /* not needed for perl4 compatible */
	  if ((re_syntax_options & RE_CONTEXTUAL_INVALID_OPS)
              && (! laststart  ||  p == pend))
	    goto invalid_pattern;
	  else 
          if (! (re_syntax_options & RE_NO_BK_VBAR))
	    goto normal_char;
	  else
#endif
	  goto handle_bar;

	case '{':
           if (! ((re_syntax_options & RE_NO_BK_CURLY_BRACES)
                  && (re_syntax_options & RE_INTERVALS)))
             goto normal_char;
           else
             goto handle_interval;

        case '\\':
	  if (p == pend) goto invalid_pattern;
          /* Do not translate the character after the \, so that we can
             distinguish, e.g., \B from \b, even if we normally would
             translate, e.g., B to b.  */
	  PATFETCH_RAW(c);
	  switch (c)
	    {
	    case '(':
	      if (re_syntax_options & RE_NO_BK_PARENS)
		goto normal_backsl;
	    handle_open:
	      if (stackp == stacke) goto nesting_too_deep;

              /* Laststart should point to the start_memory that we are about
                 to push (unless the pattern has RE_NREGS or more ('s).  */
	      /* obsolete: now RE_NREGS is just a default register size. */
              *stackp++ = b - bufp->buffer;    
	      BUFPUSH(start_memory);
	      BUFPUSH(regnum);
	      *stackp++ = fixup_jump ? fixup_jump - bufp->buffer + 1 : 0;
	      *stackp++ = regnum++;
	      *stackp++ = begalt - bufp->buffer;
	      fixup_jump = 0;
	      laststart = 0;
	      begalt = b;
	      /* too many ()'s to fit in a byte.  */
	      if (regnum >= (1<<BYTEWIDTH)) goto too_big;
	      break;

	    case ')':
	      if (re_syntax_options & RE_NO_BK_PARENS)
		goto normal_backsl;
	    handle_close:
	      if (stackp == stackb) goto unmatched_close;
	      begalt = *--stackp + bufp->buffer;
	      if (fixup_jump)
		store_jump(fixup_jump, jump, b);
	      BUFPUSH(stop_memory);
	      BUFPUSH(stackp[-1]);
	      stackp -= 2;
              fixup_jump = *stackp ? *stackp + bufp->buffer - 1 : 0;
              laststart = *--stackp + bufp->buffer;
	      break;

	    case '|':
              if ((re_syntax_options & RE_LIMITED_OPS)
	          || (re_syntax_options & RE_NO_BK_VBAR))
		goto normal_backsl;
	    handle_bar:
              if (re_syntax_options & RE_LIMITED_OPS)
                goto normal_char;
	      /* Insert before the previous alternative a jump which
                 jumps to this alternative if the former fails.  */
              GET_BUFFER_SPACE(6);
              insert_jump(on_failure_jump, begalt, b + 6, b);
	      pending_exact = 0;
	      b += 3;
	      /* The alternative before the previous alternative has a
                 jump after it which gets executed if it gets matched.
                 Adjust that jump so it will jump to the previous
                 alternative's analogous jump (put in below, which in
                 turn will jump to the next (if any) alternative's such
                 jump, etc.).  The last such jump jumps to the correct
                 final destination.  */
              if (fixup_jump)
		store_jump(fixup_jump, jump, b);

	      /* Leave space for a jump after previous alternative---to be 
                 filled in later.  */
              fixup_jump = b;
              b += 3;

              laststart = 0;
	      begalt = b;
	      break;

            case '{': 
              if (! (re_syntax_options & RE_INTERVALS)
		  /* Let \{ be a literal.  */
                  || ((re_syntax_options & RE_INTERVALS)
                      && (re_syntax_options & RE_NO_BK_CURLY_BRACES))
		  /* If it's the string "\{".  */
		  || (p - 2 == pattern  &&  p == pend))
                goto normal_backsl;
            handle_interval:
	      beg_interval = p - 1;		/* The {.  */
              /* If there is no previous pattern, this isn't an interval.  */
	      if (!laststart)
	        {
                  if (re_syntax_options & RE_CONTEXTUAL_INVALID_OPS)
		    goto invalid_pattern;
                  else
                    goto normal_backsl;
                }
              /* It also isn't an interval if not preceded by an re
                 matching a single character or subexpression, or if
                 the current type of intervals can't handle back
                 references and the previous thing is a back reference.  */
              if (! (*laststart == anychar
		     || *laststart == charset
		     || *laststart == charset_not
		     || *laststart == wordchar
		     || *laststart == notwordchar
		     || *laststart == start_memory
		     || (*laststart == exactn
			 && (laststart[1] == 1
			     || laststart[1] == 2 && ismbchar(laststart[2])))
		     || (! (re_syntax_options & RE_NO_BK_REFS)
                         && *laststart == duplicate)))
                {
                  if (re_syntax_options & RE_NO_BK_CURLY_BRACES)
                    goto normal_char;

		  /* Posix extended syntax is handled in previous
                     statement; this is for Posix basic syntax.  */
                  if (re_syntax_options & RE_INTERVALS)
                    goto invalid_pattern;

                  goto normal_backsl;
		}
              lower_bound = -1;			/* So can see if are set.  */
	      upper_bound = -1;
              GET_UNSIGNED_NUMBER(lower_bound);
	      if (c == ',')
		{
		  GET_UNSIGNED_NUMBER(upper_bound);
		  if (upper_bound < 0)
		    upper_bound = RE_DUP_MAX;
		}
	      if (upper_bound < 0)
		upper_bound = lower_bound;
              if (! (re_syntax_options & RE_NO_BK_CURLY_BRACES)) 
                {
                  if (c != '\\')
                    goto invalid_pattern;
                  PATFETCH(c);
                }
	      if (c != '}' || lower_bound < 0 || upper_bound > RE_DUP_MAX
		  || lower_bound > upper_bound 
                  || ((re_syntax_options & RE_NO_BK_CURLY_BRACES) 
		      && p != pend  && *p == '{')) 
	        {
		  if (re_syntax_options & RE_NO_BK_CURLY_BRACES)
                    goto unfetch_interval;
                  else
                    goto invalid_pattern;
		}

	      /* If upper_bound is zero, don't want to succeed at all; 
 		 jump from laststart to b + 3, which will be the end of
                 the buffer after this jump is inserted.  */

               if (upper_bound == 0)
                 {
                   GET_BUFFER_SPACE(3);
                   insert_jump(jump, laststart, b + 3, b);
                   b += 3;
                 }

               /* Otherwise, after lower_bound number of succeeds, jump
                  to after the jump_n which will be inserted at the end
                  of the buffer, and insert that jump_n.  */
               else 
		 { /* Set to 5 if only one repetition is allowed and
	              hence no jump_n is inserted at the current end of
                      the buffer; then only space for the succeed_n is
                      needed.  Otherwise, need space for both the
                      succeed_n and the jump_n.  */

                   unsigned slots_needed = upper_bound == 1 ? 5 : 10;

                   GET_BUFFER_SPACE(slots_needed);
                   /* Initialize the succeed_n to n, even though it will
                      be set by its attendant set_number_at, because
                      re_compile_fastmap will need to know it.  Jump to
                      what the end of buffer will be after inserting
                      this succeed_n and possibly appending a jump_n.  */
                   insert_jump_n(succeed_n, laststart, b + slots_needed, 
		                  b, lower_bound);
                   b += 5; 	/* Just increment for the succeed_n here.  */

		   /* When hit this when matching, set the succeed_n's n.  */
                   GET_BUFFER_SPACE(5);
		   insert_op_2(set_number_at, laststart, b, 5, lower_bound);
                   b += 5;

		  /* More than one repetition is allowed, so put in at
		     the end of the buffer a backward jump from b to the
                     succeed_n we put in above.  By the time we've gotten
                     to this jump when matching, we'll have matched once
                     already, so jump back only upper_bound - 1 times.  */

                   if (upper_bound > 1)
                     {
		       GET_BUFFER_SPACE(15);
                       store_jump_n(b, jump_n, laststart+5, upper_bound - 1);
                       b += 5;
                       /* When hit this when matching, reset the
                          preceding jump_n's n to upper_bound - 1.  */
		       insert_op_2(set_number_at, laststart, b, b - laststart, upper_bound - 1);
		       b += 5;

                       BUFPUSH(set_number_at);
                       STORE_NUMBER_AND_INCR(b, -5);
                       STORE_NUMBER_AND_INCR(b, upper_bound - 1);
                     }
                 }
              pending_exact = 0;
	      beg_interval = 0;
              break;


            unfetch_interval:
	      /* If an invalid interval, match the characters as literals.  */
	       if (beg_interval)
                 p = beg_interval;
  	       else
                 {
                   fprintf(stderr, 
		      "regex: no interval beginning to which to backtrack.\n");
		   exit (1);
                 }

               beg_interval = 0;
               PATFETCH(c);		/* normal_char expects char in `c'.  */
	       goto normal_char;
	       break;

	    case 's':
	    case 'S':
	    case 'd':
	    case 'D':
	      while (b - bufp->buffer
		     > bufp->allocated - 9 - (1 << BYTEWIDTH) / BYTEWIDTH)
		  EXTEND_BUFFER;

	      laststart = b;
	      if (c == 's' || c == 'd') {
		  BUFPUSH(charset);
	      }
	      else {
		  BUFPUSH(charset_not);
	      }

	      BUFPUSH((1 << BYTEWIDTH) / BYTEWIDTH);
	      memset(b, 0, (1 << BYTEWIDTH) / BYTEWIDTH + 2);
	      if (c == 's' || c == 'S') {
		  SET_LIST_BIT(' ');
		  SET_LIST_BIT('\t');
		  SET_LIST_BIT('\n');
		  SET_LIST_BIT('\r');
		  SET_LIST_BIT('\f');
	      }
	      else {
		  char cc;

		  for (cc = '0'; cc <= '9'; cc++) {
		      SET_LIST_BIT(cc);
		  }
	      }

	      while ((int) b[-1] > 0 && b[b[-1] - 1] == 0) 
		  b[-1]--; 
	      if (b[-1] != (1 << BYTEWIDTH) / BYTEWIDTH)
		  memmove(&b[b[-1]], &b[(1 << BYTEWIDTH) / BYTEWIDTH],
		    2 + EXTRACT_UNSIGNED(&b[(1 << BYTEWIDTH) / BYTEWIDTH])*4);
	      b += b[-1] + 2 + EXTRACT_UNSIGNED(&b[b[-1]])*4;
	      break;

	    case 'w':
	      laststart = b;
	      BUFPUSH(wordchar);
	      break;

	    case 'W':
	      laststart = b;
	      BUFPUSH(notwordchar);
	      break;

	    case 'b':
	      BUFPUSH(wordbound);
	      break;

	    case 'B':
	      BUFPUSH(notwordbound);
	      break;

	      /* hex */
	    case 'x':
	      c1 = 0;
	      c = scan_hex(p, 2, &numlen);
	      p += numlen;
	      if ((re_syntax_options & RE_MBCTYPE_MASK) && c > 0x7f)
		  c1 = 0xff;
	      goto numeric_char;

	      /* octal */
	    case '0':
	      c1 = 0;
	      c = scan_oct(p, 3, &numlen);
	      p += numlen;
	      if ((re_syntax_options & RE_MBCTYPE_MASK) && c > 0x7f)
		  c1 = 0xff;
	      goto numeric_char;

	      /* back-ref or octal */
	    case '1': case '2': case '3':
	    case '4': case '5': case '6':
	    case '7': case '8': case '9':
	      {
		  char *p_save;

		  PATUNFETCH;
		  p_save = p;

		  c1 = 0;
		  GET_UNSIGNED_NUMBER(c1);
		  if (p < pend) PATUNFETCH;

		  if (c1 >= regnum) {
		      /* need to get octal */
		      p = p_save;
		      c = scan_oct(p_save, 3, &numlen);
		      p = p_save + numlen;
		      c1 = 0;
		      if ((re_syntax_options & RE_MBCTYPE_MASK) && c > 0x7f)
			  c1 = 0xff;
		      goto numeric_char;
		  }
	      }

              /* Can't back reference to a subexpression if inside of it.  */
              for (stackt = stackp - 2;  stackt > stackb;  stackt -= 4)
 		if (*stackt == c1)
		  goto normal_char;
	      laststart = b;
	      BUFPUSH(duplicate);
	      BUFPUSH(c1);
	      break;

	    case '+':
	    case '?':
	      if (re_syntax_options & RE_BK_PLUS_QM)
		goto handle_plus;
	      else
                goto normal_backsl;
              break;

            default:
	    normal_backsl:
	      goto normal_char;
	    }
	  break;

	default:
	normal_char:		/* Expects the character in `c'.  */
	  c1 = 0;
	  if (ismbchar(c)) {
	    c1 = c;
	    PATFETCH(c);
	  }
	  else if (c > 0x7f) {
	      c1 = 0xff;
	  }
	numeric_char:
	  if (!pending_exact || pending_exact + *pending_exact + 1 != b
	      || *pending_exact >= (c1 ? 0176 : 0177)
	      || *p == '*' || *p == '^'
	      || ((re_syntax_options & RE_BK_PLUS_QM)
		  ? *p == '\\' && (p[1] == '+' || p[1] == '?')
		  : (*p == '+' || *p == '?'))
	      || ((re_syntax_options & RE_INTERVALS) 
                  && ((re_syntax_options & RE_NO_BK_CURLY_BRACES)
		      ? *p == '{'
                      : (p[0] == '\\' && p[1] == '{'))))
	    {
	      laststart = b;
	      BUFPUSH(exactn);
	      pending_exact = b;
	      BUFPUSH(0);
	    }
	  if (c1) {
	    BUFPUSH(c1);
	    (*pending_exact)++;
	  }
	  BUFPUSH(c);
	  (*pending_exact)++;
	}
    }

  if (fixup_jump)
    store_jump(fixup_jump, jump, b);

  if (stackp != stackb) goto unmatched_open;

  bufp->used = b - bufp->buffer;
  bufp->re_nsub = regnum;
  return 0;

 invalid_char:
  return "Invalid character in regular expression";

 invalid_pattern:
  return "Invalid regular expression";

 unmatched_open:
  return "Unmatched (";

 unmatched_close:
  return "Unmatched )";

 end_of_pattern:
  return "Premature end of regular expression";

 nesting_too_deep:
  return "Nesting too deep";

 too_big:
  return "Regular expression too big";

 memory_exhausted:
  return "Memory exhausted";
}


/* Store a jump of the form <OPCODE> <relative address>.
   Store in the location FROM a jump operation to jump to relative
   address FROM - TO.  OPCODE is the opcode to store.  */

static void
store_jump(from, opcode, to)
     char *from, *to;
     int opcode;
{
  from[0] = (char)opcode;
  STORE_NUMBER(from + 1, to - (from + 3));
}


/* Open up space before char FROM, and insert there a jump to TO.
   CURRENT_END gives the end of the storage not in use, so we know 
   how much data to copy up. OP is the opcode of the jump to insert.

   If you call this function, you must zero out pending_exact.  */

static void
insert_jump(op, from, to, current_end)
     int op;
     char *from, *to, *current_end;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 3;		/* ...to here.  */

  while (pfrom != from)			       
    *--pto = *--pfrom;
  store_jump(from, op, to);
}


/* Store a jump of the form <opcode> <relative address> <n> .

   Store in the location FROM a jump operation to jump to relative
   address FROM - TO.  OPCODE is the opcode to store, N is a number the
   jump uses, say, to decide how many times to jump.

   If you call this function, you must zero out pending_exact.  */

static void
store_jump_n(from, opcode, to, n)
     char *from, *to;
     int opcode;
     unsigned n;
{
  from[0] = (char)opcode;
  STORE_NUMBER(from + 1, to - (from + 3));
  STORE_NUMBER(from + 3, n);
}


/* Similar to insert_jump, but handles a jump which needs an extra
   number to handle minimum and maximum cases.  Open up space at
   location FROM, and insert there a jump to TO.  CURRENT_END gives the
   end of the storage in use, so we know how much data to copy up. OP is
   the opcode of the jump to insert.

   If you call this function, you must zero out pending_exact.  */

static void
insert_jump_n(op, from, to, current_end, n)
     int op;
     char *from, *to, *current_end;
     unsigned n;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 5;		/* ...to here.  */

  while (pfrom != from)			       
    *--pto = *--pfrom;
  store_jump_n(from, op, to, n);
}


/* Open up space at location THERE, and insert operation OP followed by
   NUM_1 and NUM_2.  CURRENT_END gives the end of the storage in use, so
   we know how much data to copy up.

   If you call this function, you must zero out pending_exact.  */

static void
insert_op_2(op, there, current_end, num_1, num_2)
     int op;
     char *there, *current_end;
     int num_1, num_2;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 5;		/* ...to here.  */

  while (pfrom != there)			       
    *--pto = *--pfrom;

  there[0] = (char)op;
  STORE_NUMBER(there + 1, num_1);
  STORE_NUMBER(there + 3, num_2);
}



/* Given a pattern, compute a fastmap from it.  The fastmap records
   which of the (1 << BYTEWIDTH) possible characters can start a string
   that matches the pattern.  This fastmap is used by re_search to skip
   quickly over totally implausible text.

   The caller must supply the address of a (1 << BYTEWIDTH)-byte data 
   area as bufp->fastmap.
   The other components of bufp describe the pattern to be used.  */

void
re_compile_fastmap(bufp)
     struct re_pattern_buffer *bufp;
{
  unsigned char *pattern = (unsigned char *) bufp->buffer;
  int size = bufp->used;
  register char *fastmap = bufp->fastmap;
  register unsigned char *p = pattern;
  register unsigned char *pend = pattern + size;
  register int j, k;
  unsigned char *translate = (unsigned char *)bufp->translate;
  unsigned is_a_succeed_n;

  unsigned char **stackb;
  unsigned char **stackp;
  stackb = RE_TALLOC(NFAILURES, unsigned char*);
  stackp = stackb;

  memset(fastmap, 0, (1 << BYTEWIDTH));
  bufp->fastmap_accurate = 1;
  bufp->can_be_null = 0;

  while (p)
    {
      is_a_succeed_n = 0;
      if (p == pend)
	{
	  bufp->can_be_null = 1;
	  break;
	}
#ifdef SWITCH_ENUM_BUG
      switch ((int) ((enum regexpcode)*p++))
#else
      switch ((enum regexpcode)*p++)
#endif
	{
	case exactn:
	  if (p[1] == 0xff) {
	      if (translate)
		fastmap[translate[p[2]]] = 2;
	      else
		fastmap[p[2]] = 2;
	  }
	  else if (translate)
	    fastmap[translate[p[1]]] = 1;
	  else
	    fastmap[p[1]] = 1;
	  break;

        case begline:
	case wordbound:
	case notwordbound:
          continue;

	case endline:
	  if (translate)
	    fastmap[translate['\n']] = 1;
	  else
	    fastmap['\n'] = 1;

	  if (bufp->can_be_null == 0)
	    bufp->can_be_null = 2;
	  break;

	case jump_n:
        case finalize_jump:
	case maybe_finalize_jump:
	case jump:
	case dummy_failure_jump:
          EXTRACT_NUMBER_AND_INCR(j, p);
	  p += j;	
	  if (j > 0)
	    continue;
          /* Jump backward reached implies we just went through
	     the body of a loop and matched nothing.
	     Opcode jumped to should be an on_failure_jump.
	     Just treat it like an ordinary jump.
	     For a * loop, it has pushed its failure point already;
	     If so, discard that as redundant.  */

          if ((enum regexpcode) *p != on_failure_jump
	      && (enum regexpcode) *p != succeed_n)
	    continue;
          p++;
          EXTRACT_NUMBER_AND_INCR(j, p);
          p += j;	
          if (stackp != stackb && *stackp == p)
            stackp--;
          continue;

        case on_failure_jump:
	handle_on_failure_jump:
          EXTRACT_NUMBER_AND_INCR(j, p);
          *++stackp = p + j;
	  if (is_a_succeed_n)
            EXTRACT_NUMBER_AND_INCR(k, p);	/* Skip the n.  */
	  continue;

	case succeed_n:
	  is_a_succeed_n = 1;
          /* Get to the number of times to succeed.  */
          p += 2;		
	  /* Increment p past the n for when k != 0.  */
          EXTRACT_NUMBER_AND_INCR(k, p);
          if (k == 0)
	    {
              p -= 4;
              goto handle_on_failure_jump;
            }
          continue;

	case set_number_at:
          p += 4;
          continue;

        case start_memory:
	case stop_memory:
	  p++;
	  continue;

	case duplicate:
	  bufp->can_be_null = 1;
	  fastmap['\n'] = 1;
	case anychar:
	  for (j = 0; j < (1 << BYTEWIDTH); j++)
	    if (j != '\n')
	      fastmap[j] = 1;
	  if (bufp->can_be_null)
	    {
	      FREE_AND_RETURN_VOID(stackb);
	    }
	  /* Don't return; check the alternative paths
	     so we can set can_be_null if appropriate.  */
	  break;

	case wordchar:
	  for (j = 0; j < (1 << BYTEWIDTH); j++)
	    if (SYNTAX(j) == Sword)
	      fastmap[j] = 1;
	  break;

	case notwordchar:
	  for (j = 0; j < 0x80; j++)
	    if (SYNTAX(j) != Sword)
	      fastmap[j] = 1;
	  for (j = 0x80; j < (1 << BYTEWIDTH); j++)
	      fastmap[j] = 1;
	  break;

	case charset:
	  /* NOTE: Charset for single-byte chars never contain
		   multi-byte char.  See set_list_bits().  */
	  for (j = *p++ * BYTEWIDTH - 1; j >= 0; j--)
	    if (p[j / BYTEWIDTH] & (1 << (j % BYTEWIDTH)))
	      {
		if (translate)
		  fastmap[translate[j]] = 1;
		else
		  fastmap[j] = 1;
	      }
	  {
	    unsigned short size;
	    unsigned c, end;

	    p += p[-1] + 2;
	    size = EXTRACT_UNSIGNED(&p[-2]);
	    for (j = 0; j < (int)size; j++) {
	      if ((unsigned char)p[j*4] == 0xff) {
		for (c = (unsigned char)p[j*4+1],
		    end = (unsigned char)p[j*4+3];
		     c <= end; c++) {
		  fastmap[c] = 2;
		}
	      }
	      else {
		/* set bits for 1st bytes of multi-byte chars.  */
		for (c = (unsigned char)p[j*4],
		     end = (unsigned char)p[j*4 + 2];
		     c <= end; c++) {
		  /* NOTE: Charset for multi-byte chars might contain
		     single-byte chars.  We must reject them. */
		  if (ismbchar(c))
		    fastmap[c] = 1;
		}
	      }
	    }
	  }
	  break;

	case charset_not:
	  /* S: set of all single-byte chars.
	     M: set of all first bytes that can start multi-byte chars.
	     s: any set of single-byte chars.
	     m: any set of first bytes that can start multi-byte chars.

	     We assume S+M = U.
	       ___      _   _
	       s+m = (S*s+M*m).  */
	  /* Chars beyond end of map must be allowed */
	  /* NOTE: Charset_not for single-byte chars might contain
		   multi-byte chars.  See set_list_bits(). */
	  for (j = *p * BYTEWIDTH; j < (1 << BYTEWIDTH); j++)
	    if (!ismbchar(j))
	      fastmap[j] = 1;

	  for (j = *p++ * BYTEWIDTH - 1; j >= 0; j--)
	    if (!(p[j / BYTEWIDTH] & (1 << (j % BYTEWIDTH))))
	      {
		if (!ismbchar(j))
		  fastmap[j] = 1;
	      }
	  {
	    unsigned short size;
	    unsigned char c, beg;

	    p += p[-1] + 2;
	    size = EXTRACT_UNSIGNED(&p[-2]);
	    if (size == 0) {
		for (j = 0x80; j < (1 << BYTEWIDTH); j++)
		    if (ismbchar(j))
			fastmap[j] = 1;
	    }
	    for (j = 0,c = 0x80;j < (int)size; j++) {
	      if ((unsigned char)p[j*4] == 0xff) {
	        for (beg = (unsigned char)p[j*4+1]; c < beg; c++)
		  fastmap[c] = 2;
	        c = (unsigned char)p[j*4+3] + 1;
	      }
	      else {
	        for (beg = (unsigned char)p[j*4 + 0]; c < beg; c++)
		  if (ismbchar(c))
		    fastmap[c] = 1;
	        c = (unsigned char)p[j*4 + 2] + 1;
	      }
	    }
	  }
	  break;

	case unused:	/* pacify gcc -Wall */
	  break;
	}

      /* Get here means we have successfully found the possible starting
         characters of one path of the pattern.  We need not follow this
         path any farther.  Instead, look at the next alternative
         remembered in the stack.  */
      if (stackp != stackb)
	p = *stackp--;
      else
	break;
    }
   FREE_AND_RETURN_VOID(stackb);
}




/* Using the compiled pattern in BUFP->buffer, first tries to match
   STRING, starting first at index STARTPOS, then at STARTPOS + 1, and
   so on.  RANGE is the number of places to try before giving up.  If
   RANGE is negative, it searches backwards, i.e., the starting
   positions tried are STARTPOS, STARTPOS - 1, etc.  STRING is of SIZE.
   In REGS, return the indices of STRING that matched the entire
   BUFP->buffer and its contained subexpressions.

   The value returned is the position in the strings at which the match
   was found, or -1 if no match was found, or -2 if error (such as
   failure stack overflow).  */

int
re_search(bufp, string, size, startpos, range, regs)
     struct re_pattern_buffer *bufp;
     char *string;
     int size, startpos, range;
     struct re_registers *regs;
{
  register char *fastmap = bufp->fastmap;
  register unsigned char *translate = (unsigned char *) bufp->translate;
  int val, anchor = 0;

  /* Check for out-of-range starting position.  */
  if (startpos < 0  ||  startpos > size)
    return -1;

  /* Update the fastmap now if not correct already.  */
  if (fastmap && !bufp->fastmap_accurate) {
      re_compile_fastmap (bufp);
  }

  if (bufp->used > 0 && (enum regexpcode)bufp->buffer[0] == begline)
      anchor = 1;

  for (;;)
    {
      /* If a fastmap is supplied, skip quickly over characters that
         cannot possibly be the start of a match.  Note, however, that
         if the pattern can possibly match the null string, we must
         test it at each starting point so that we take the first null
         string we get.  */

      if (fastmap && startpos < size
	  && bufp->can_be_null != 1 && !(anchor && startpos == 0))
	{
	  if (range > 0)	/* Searching forwards.  */
	    {
	      register int lim = 0;
	      register unsigned char *p, c;
	      int irange = range;

	      lim = range - (size - startpos);
	      p = (unsigned char *)&(string[startpos]);

	      while (range > lim) {
		c = *p++;
		if (ismbchar(c)) {
		  if (fastmap[c])
		    break;
		  c = *p++;
		  range--;
		  if (fastmap[c] == 2)
		    break;
		}
		else 
		  if (fastmap[translate ? translate[c] : c])
		    break;
		range--;
	      }
	      startpos += irange - range;
	    }
	  else			/* Searching backwards.  */
	    {
	      register unsigned char c;

	      c = string[startpos];
              c &= 0xff;
	      if (translate ? !fastmap[translate[c]] : !fastmap[c])
		goto advance;
	    }
	}

      if (anchor && startpos > 0 && startpos < size
	  && string[startpos-1] != '\n') goto advance;

      if (fastmap && startpos == size && range >= 0
	  && (bufp->can_be_null == 0 ||
	      (bufp->can_be_null == 2 && size > 0
	       && string[startpos-1] == '\n')))
	return -1;

      val = re_match(bufp, string, size, startpos, regs);
      if (val >= 0)
	return startpos;
      if (val == -2)
	return -2;

#ifndef NO_ALLOCA
#ifdef cALLOCA
      alloca(0);
#endif /* cALLOCA */

#endif /* NO_ALLOCA */
    advance:
      if (!range) 
        break;
      else if (range > 0) {
	const char *d = string + startpos;

	if (ismbchar(*d)) {
	  range--, startpos++;
	  if (!range)
	    break;
	}
	range--, startpos++;
      }
      else {
	range++, startpos--;
	{
	  const char *s, *d, *p;

	  s = string; d = string + startpos;
	  for (p = d; p-- > s && ismbchar(*p); )
	    /* --p >= s would not work on 80[12]?86. 
	      (when the offset of s equals 0 other than huge model.)  */
	    ;
	  if (!((d - p) & 1)) {
	    if (!range)
	      break;
	    range++, startpos--;
	  }
	}
      }
    }
  return -1;
}




/* The following are used for re_match, defined below:  */

/* Roughly the maximum number of failure points on the stack.  Would be
   exactly that if always pushed MAX_NUM_FAILURE_ITEMS each time we failed.  */

int re_max_failures = 2000;

/* Routine used by re_match.  */
/* static int memcmp_translate(); *//* already declared */


/* Structure and accessing macros used in re_match:  */

struct register_info
{
  unsigned is_active : 1;
  unsigned matched_something : 1;
};

#define IS_ACTIVE(R)  ((R).is_active)
#define MATCHED_SOMETHING(R)  ((R).matched_something)


/* Macros used by re_match:  */

/* I.e., regstart, regend, and reg_info.  */

#define NUM_REG_ITEMS  3

/* We push at most this many things on the stack whenever we
   fail.  The `+ 2' refers to PATTERN_PLACE and STRING_PLACE, which are
   arguments to the PUSH_FAILURE_POINT macro.  */

#define MAX_NUM_FAILURE_ITEMS   (num_regs * NUM_REG_ITEMS + 2)


/* We push this many things on the stack whenever we fail.  */

#define NUM_FAILURE_ITEMS  (last_used_reg * NUM_REG_ITEMS + 2)


/* This pushes most of the information about the current state we will want
   if we ever fail back to it.  */

#define PUSH_FAILURE_POINT(pattern_place, string_place)			\
  {									\
    long last_used_reg, this_reg;					\
									\
    /* Find out how many registers are active or have been matched.	\
       (Aside from register zero, which is only set at the end.) */	\
    for (last_used_reg = num_regs - 1; last_used_reg > 0; last_used_reg--)\
      if (regstart[last_used_reg] != (unsigned char *)(-1L))		\
        break;								\
									\
    if (stacke - stackp <= NUM_FAILURE_ITEMS)				\
      {									\
	unsigned char **stackx;						\
	unsigned int len = stacke - stackb;				\
	if (len > re_max_failures * MAX_NUM_FAILURE_ITEMS)		\
	  {								\
	    FREE_VARIABLES();						\
	    FREE_AND_RETURN(stackb,(-2));				\
	  }								\
									\
        /* Roughly double the size of the stack.  */			\
        stackx = DOUBLE_STACK(stackx,stackb,len);			\
	/* Rearrange the pointers. */					\
	stackp = stackx + (stackp - stackb);				\
	stackb = stackx;						\
	stacke = stackb + 2 * len;					\
      }									\
									\
    /* Now push the info for each of those registers.  */		\
    for (this_reg = 1; this_reg <= last_used_reg; this_reg++)		\
      {									\
        *stackp++ = regstart[this_reg];					\
        *stackp++ = regend[this_reg];					\
        *stackp++ = (unsigned char *)&reg_info[this_reg];		\
      }									\
									\
    /* Push how many registers we saved.  */				\
    *stackp++ = (unsigned char *)last_used_reg;				\
									\
    *stackp++ = pattern_place;                                          \
    *stackp++ = string_place;                                           \
  }


/* This pops what PUSH_FAILURE_POINT pushes.  */

#define POP_FAILURE_POINT()						\
  {									\
    int temp;								\
    stackp -= 2;		/* Remove failure points.  */		\
    temp = (int) *--stackp;	/* How many regs pushed.  */	        \
    temp *= NUM_REG_ITEMS;	/* How much to take off the stack.  */	\
    stackp -= temp; 		/* Remove the register info.  */	\
  }

#define PREFETCH if (d == dend) goto fail

/* Call this when have matched something; it sets `matched' flags for the
   registers corresponding to the subexpressions of which we currently
   are inside.  */
#define SET_REGS_MATCHED 						\
  { unsigned this_reg;							\
    for (this_reg = 0; this_reg < num_regs; this_reg++) 		\
      { 								\
        if (IS_ACTIVE(reg_info[this_reg]))				\
          MATCHED_SOMETHING(reg_info[this_reg]) = 1;			\
        else								\
          MATCHED_SOMETHING(reg_info[this_reg]) = 0;			\
      } 								\
  }

#define AT_STRINGS_BEG  (d == string)
#define AT_STRINGS_END  (d == dend)	

#define AT_WORD_BOUNDARY						\
  (AT_STRINGS_BEG || AT_STRINGS_END || IS_A_LETTER (d - 1) != IS_A_LETTER (d))

/* We have two special cases to check for: 
     1) if we're past the end of string1, we have to look at the first
        character in string2;
     2) if we're before the beginning of string2, we have to look at the
        last character in string1; we assume there is a string1, so use
        this in conjunction with AT_STRINGS_BEG.  */
#define IS_A_LETTER(d) (SYNTAX(*(d)) == Sword)

static void
init_regs(regs, num_regs)
    struct re_registers *regs;
    unsigned num_regs;
{
    int i;

    regs->num_regs = num_regs;
    if (num_regs < RE_NREGS)
	num_regs = RE_NREGS;

    if (regs->allocated == 0) {
	regs->beg = TMALLOC(num_regs, int);
	regs->end = TMALLOC(num_regs, int);
	regs->allocated = num_regs;
    }
    else if (regs->allocated < num_regs) {
	TREALLOC(regs->beg, num_regs, int);
	TREALLOC(regs->end, num_regs, int);
    }
    for (i=0; i<num_regs; i++) {
	regs->beg[i] = regs->end[i] = -1;
    }
}

/* Match the pattern described by BUFP against STRING, which is of
   SIZE.  Start the match at index POS in STRING.  In REGS, return the
   indices of STRING that matched the entire BUFP->buffer and its
   contained subexpressions.

   If bufp->fastmap is nonzero, then it had better be up to date.

   The reason that the data to match are specified as two components
   which are to be regarded as concatenated is so this function can be
   used directly on the contents of an Emacs buffer.

   -1 is returned if there is no match.  -2 is returned if there is an
   error (such as match stack overflow).  Otherwise the value is the
   length of the substring which was matched.  */

int
re_match(bufp, string_arg, size, pos, regs)
     struct re_pattern_buffer *bufp;
     char *string_arg;
     int size, pos;
     struct re_registers *regs;
{
  register unsigned char *p = (unsigned char *) bufp->buffer;

  /* Pointer to beyond end of buffer.  */
  register unsigned char *pend = p + bufp->used;

  unsigned num_regs = bufp->re_nsub;

  unsigned char *string = (unsigned char *) string_arg;

  register unsigned char *d, *dend;
  register int mcnt;			/* Multipurpose.  */
  unsigned char *translate = (unsigned char *) bufp->translate;
  unsigned is_a_jump_n = 0;

 /* Failure point stack.  Each place that can handle a failure further
    down the line pushes a failure point on this stack.  It consists of
    restart, regend, and reg_info for all registers corresponding to the
    subexpressions we're currently inside, plus the number of such
    registers, and, finally, two char *'s.  The first char * is where to
    resume scanning the pattern; the second one is where to resume
    scanning the strings.  If the latter is zero, the failure point is a
    ``dummy''; if a failure happens and the failure point is a dummy, it
    gets discarded and the next next one is tried.  */

  unsigned char **stackb;
  unsigned char **stackp;
  unsigned char **stacke;


  /* Information on the contents of registers. These are pointers into
     the input strings; they record just what was matched (on this
     attempt) by a subexpression part of the pattern, that is, the
     regnum-th regstart pointer points to where in the pattern we began
     matching and the regnum-th regend points to right after where we
     stopped matching the regnum-th subexpression.  (The zeroth register
     keeps track of what the whole pattern matches.)  */

  unsigned char **regstart = RE_TALLOC(num_regs, unsigned char*);
  unsigned char **regend = RE_TALLOC(num_regs, unsigned char*);

  /* The is_active field of reg_info helps us keep track of which (possibly
     nested) subexpressions we are currently in. The matched_something
     field of reg_info[reg_num] helps us tell whether or not we have
     matched any of the pattern so far this time through the reg_num-th
     subexpression.  These two fields get reset each time through any
     loop their register is in.  */

  struct register_info *reg_info = RE_TALLOC(num_regs, struct register_info);

  /* The following record the register info as found in the above
     variables when we find a match better than any we've seen before. 
     This happens as we backtrack through the failure points, which in
     turn happens only if we have not yet matched the entire string.  */

  unsigned best_regs_set = 0;
  unsigned char **best_regstart = RE_TALLOC(num_regs, unsigned char*);
  unsigned char **best_regend = RE_TALLOC(num_regs, unsigned char*);

  if (regs) {
      init_regs(regs, num_regs);
  }


  /* Initialize the stack. */
  stackb = RE_TALLOC(MAX_NUM_FAILURE_ITEMS * NFAILURES, unsigned char*);
  stackp = stackb;
  stacke = &stackb[MAX_NUM_FAILURE_ITEMS * NFAILURES];

#ifdef DEBUG_REGEX
  fprintf (stderr, "Entering re_match(%s%s)\n", string1_arg, string2_arg);
#endif

  /* Initialize subexpression text positions to -1 to mark ones that no
     \( or ( and \) or ) has been seen for. Also set all registers to
     inactive and mark them as not having matched anything or ever
     failed. */
  for (mcnt = 0; mcnt < num_regs; mcnt++) {
      regstart[mcnt] = regend[mcnt] = (unsigned char *) (-1L);
      IS_ACTIVE(reg_info[mcnt]) = 0;
      MATCHED_SOMETHING(reg_info[mcnt]) = 0;
  }

  /* Set up pointers to ends of strings.
     Don't allow the second string to be empty unless both are empty.  */


  /* `p' scans through the pattern as `d' scans through the data. `dend'
     is the end of the input string that `d' points within. `d' is
     advanced into the following input string whenever necessary, but
     this happens before fetching; therefore, at the beginning of the
     loop, `d' can be pointing at the end of a string, but it cannot
     equal string2.  */

  d = string + pos, dend = string + size;


  /* This loops over pattern commands.  It exits by returning from the
     function if match is complete, or it drops through if match fails
     at this starting point in the input data.  */

  while (1)
    {
#ifdef DEBUG_REGEX
      fprintf(stderr,
	      "regex loop(%d):  matching 0x%02d\n",
	      p - (unsigned char *) bufp->buffer,
	      *p);
#endif
      is_a_jump_n = 0;
      /* End of pattern means we might have succeeded.  */
      if (p == pend)
	{
	  /* If not end of string, try backtracking.  Otherwise done.  */
          if (d != dend)
	    {
              if (stackp != stackb)
                {
                  /* More failure points to try.  */

                  /* If exceeds best match so far, save it.  */
                  if (! best_regs_set || (d > best_regend[0]))
                    {
                      best_regs_set = 1;
                      best_regend[0] = d;	/* Never use regstart[0].  */

                      for (mcnt = 1; mcnt < num_regs; mcnt++)
                        {
                          best_regstart[mcnt] = regstart[mcnt];
                          best_regend[mcnt] = regend[mcnt];
                        }
                    }
                  goto fail;	       
                }
              /* If no failure points, don't restore garbage.  */
              else if (best_regs_set)   
                {
	      restore_best_regs:
                  /* Restore best match.  */
                  d = best_regend[0];

		  for (mcnt = 0; mcnt < num_regs; mcnt++)
		    {
		      regstart[mcnt] = best_regstart[mcnt];
		      regend[mcnt] = best_regend[mcnt];
		    }
                }
            }

	  /* If caller wants register contents data back, convert it 
	     to indices.  */
	  if (regs)
	    {
	      regs->beg[0] = pos;
	      regs->end[0] = d - string;
	      for (mcnt = 1; mcnt < num_regs; mcnt++)
		{
		  if (regend[mcnt] == (unsigned char *)(-1L))
		    {
		      regs->beg[mcnt] = -1;
		      regs->end[mcnt] = -1;
		      continue;
		    }
		  regs->beg[mcnt] = regstart[mcnt] - string;
		  regs->end[mcnt] = regend[mcnt] - string;
		}
	    }
	  FREE_VARIABLES();
	  FREE_AND_RETURN(stackb, (d - pos - string));
        }

      /* Otherwise match next pattern command.  */
#ifdef SWITCH_ENUM_BUG
      switch ((int)((enum regexpcode)*p++))
#else
      switch ((enum regexpcode)*p++)
#endif
	{

	/* \( [or `(', as appropriate] is represented by start_memory,
           \) by stop_memory.  Both of those commands are followed by
           a register number in the next byte.  The text matched
           within the \( and \) is recorded under that number.  */
	case start_memory:
          regstart[*p] = d;
          IS_ACTIVE(reg_info[*p]) = 1;
          MATCHED_SOMETHING(reg_info[*p]) = 0;
          p++;
          break;

	case stop_memory:
          regend[*p] = d;
          IS_ACTIVE(reg_info[*p]) = 0;

          /* If just failed to match something this time around with a sub-
	     expression that's in a loop, try to force exit from the loop.  */
          if ((! MATCHED_SOMETHING(reg_info[*p])
	       || (enum regexpcode) p[-3] == start_memory)
	      && (p + 1) != pend)              
            {
	      register unsigned char *p2 = p + 1;
              mcnt = 0;
              switch (*p2++)
                {
                  case jump_n:
		    is_a_jump_n = 1;
                  case finalize_jump:
		  case maybe_finalize_jump:
		  case jump:
		  case dummy_failure_jump:
                    EXTRACT_NUMBER_AND_INCR(mcnt, p2);
		    if (is_a_jump_n)
		      p2 += 2;
                    break;
                }
	      p2 += mcnt;

              /* If the next operation is a jump backwards in the pattern
	         to an on_failure_jump, exit from the loop by forcing a
                 failure after pushing on the stack the on_failure_jump's 
                 jump in the pattern, and d.  */
	      if (mcnt < 0 && (enum regexpcode) *p2++ == on_failure_jump)
		{
                  EXTRACT_NUMBER_AND_INCR(mcnt, p2);
                  PUSH_FAILURE_POINT(p2 + mcnt, d);
                  goto fail;
                }
            }
          p++;
          break;

	/* \<digit> has been turned into a `duplicate' command which is
           followed by the numeric value of <digit> as the register number.  */
        case duplicate:
	  {
	    int regno = *p++;   /* Get which register to match against */
	    register unsigned char *d2, *dend2;

	    /* Where in input to try to start matching.  */
            d2 = regstart[regno];

            /* Where to stop matching; if both the place to start and
               the place to stop matching are in the same string, then
               set to the place to stop, otherwise, for now have to use
               the end of the first string.  */

            dend2 = regend[regno];
	    while (1)
	      {
		/* At end of register contents => success */
		if (d2 == dend2) break;

		/* If necessary, advance to next segment in data.  */
		PREFETCH;

		/* How many characters left in this segment to match.  */
		mcnt = dend - d;

		/* Want how many consecutive characters we can match in
                   one shot, so, if necessary, adjust the count.  */
                if (mcnt > dend2 - d2)
		  mcnt = dend2 - d2;

		/* Compare that many; failure if mismatch, else move
                   past them.  */
		if (translate 
                    ? memcmp_translate(d, d2, mcnt, translate) 
                    : memcmp((char *)d, (char *)d2, mcnt))
		  goto fail;
		d += mcnt, d2 += mcnt;
	      }
	  }
	  break;

	case anychar:
	  PREFETCH;
	  /* Match anything but a newline, maybe even a null.  */
	  if (ismbchar(*d)) {
	    if (d + 1 == dend || d[1] == '\n' || d[1] == '\0')
	      goto fail;
	    SET_REGS_MATCHED;
	    d += 2;
	    break;
	  }
	  if ((translate ? translate[*d] : *d) == '\n'
              || ((re_syntax_options & RE_DOT_NOT_NULL) 
                  && (translate ? translate[*d] : *d) == '\000'))
	    goto fail;
	  SET_REGS_MATCHED;
          d++;
	  break;

	case charset:
	case charset_not:
	  {
	    int not;	    /* Nonzero for charset_not.  */
	    int half;	    /* 2 if need to match latter half of mbc */
	    int c;

	    PREFETCH;
	    c = (unsigned char)*d;
	    if (ismbchar(c)) {
	      if (d + 1 != dend) {
	        c <<= 8;
		c |= (unsigned char)d[1];
	      }
	    }
	    else if (translate)
	      c = (unsigned char)translate[c];

	    half = not = is_in_list(c, p);
	    if (*(p - 1) == (unsigned char)charset_not) {
		not = !not;
	    }

	    p += 1 + *p + 2 + EXTRACT_UNSIGNED(&p[1 + *p])*4;

	    if (!not) goto fail;
	    SET_REGS_MATCHED;

            d++;
	    if (half != 2 && d != dend && c >= 1 << BYTEWIDTH)
		d++;
	    break;
	  }

	case begline:
          if (size == 0
	      || d == string
              || (d && d[-1] == '\n'))
            break;
          else
            goto fail;

	case endline:
	  if (d == dend || *d == '\n')
	    break;
	  goto fail;

	/* `or' constructs are handled by starting each alternative with
           an on_failure_jump that points to the start of the next
           alternative.  Each alternative except the last ends with a
           jump to the joining point.  (Actually, each jump except for
           the last one really jumps to the following jump, because
           tensioning the jumps is a hassle.)  */

	/* The start of a stupid repeat has an on_failure_jump that points
	   past the end of the repeat text. This makes a failure point so 
           that on failure to match a repetition, matching restarts past
           as many repetitions have been found with no way to fail and
           look for another one.  */

	/* A smart repeat is similar but loops back to the on_failure_jump
	   so that each repetition makes another failure point.  */

	case on_failure_jump:
        on_failure:
          EXTRACT_NUMBER_AND_INCR(mcnt, p);
          PUSH_FAILURE_POINT(p + mcnt, d);
          break;

	/* The end of a smart repeat has a maybe_finalize_jump back.
	   Change it either to a finalize_jump or an ordinary jump.  */
	case maybe_finalize_jump:
          EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  {
	    register unsigned char *p2 = p;
	    /* Compare what follows with the beginning of the repeat.
	       If we can establish that there is nothing that they would
	       both match, we can change to finalize_jump.  */
	    while (p2 + 1 != pend
		   && (*p2 == (unsigned char)stop_memory
		       || *p2 == (unsigned char)start_memory))
	      p2 += 2;				/* Skip over reg number.  */
	    if (p2 == pend)
	      p[-3] = (unsigned char)finalize_jump;
	    else if (*p2 == (unsigned char)exactn
		     || *p2 == (unsigned char)endline)
	      {
		register int c = *p2 == (unsigned char)endline ? '\n' : p2[2];
		register unsigned char *p1 = p + mcnt;
		/* p1[0] ... p1[2] are an on_failure_jump.
		   Examine what follows that.  */
		if (p1[3] == (unsigned char)exactn && p1[5] != c)
		  p[-3] = (unsigned char)finalize_jump;
		else if (p1[3] == (unsigned char)charset
			 || p1[3] == (unsigned char)charset_not) {
		    int not;
		    if (ismbchar(c))
		      c = c << 8 | p2[3];
		    /* `is_in_list()' is TRUE if c would match */
		    /* That means it is not safe to finalize.  */
		    not = is_in_list(c, p1 + 4);
		    if (p1[3] == (unsigned char)charset_not)
			not = !not;
		    if (!not)
			p[-3] = (unsigned char)finalize_jump;
		  }
	      }
	  }
	  p -= 2;		/* Point at relative address again.  */
	  if (p[-1] != (unsigned char)finalize_jump)
	    {
	      p[-1] = (unsigned char)jump;	
	      goto nofinalize;
	    }
        /* Note fall through.  */

	/* The end of a stupid repeat has a finalize_jump back to the
           start, where another failure point will be made which will
           point to after all the repetitions found so far.  */

        /* Take off failure points put on by matching on_failure_jump 
           because didn't fail.  Also remove the register information
           put on by the on_failure_jump.  */
        case finalize_jump:
          POP_FAILURE_POINT();
        /* Note fall through.  */

	/* Jump without taking off any failure points.  */
        case jump:
	nofinalize:
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  p += mcnt;
	  break;

        case dummy_failure_jump:
          /* Normally, the on_failure_jump pushes a failure point, which
             then gets popped at finalize_jump.  We will end up at
             finalize_jump, also, and with a pattern of, say, `a+', we
             are skipping over the on_failure_jump, so we have to push
             something meaningless for finalize_jump to pop.  */
          PUSH_FAILURE_POINT(0, 0);
          goto nofinalize;


        /* Have to succeed matching what follows at least n times.  Then
          just handle like an on_failure_jump.  */
        case succeed_n: 
          EXTRACT_NUMBER(mcnt, p + 2);
          /* Originally, this is how many times we HAVE to succeed.  */
          if (mcnt)
            {
               mcnt--;
	       p += 2;
               STORE_NUMBER_AND_INCR(p, mcnt);
            }
	  else if (mcnt == 0)
            {
	      p[2] = unused;
              p[3] = unused;
              goto on_failure;
            }
          else
	    { 
              fprintf(stderr, "regex: the succeed_n's n is not set.\n");
              exit(1);
	    }
          break;

        case jump_n: 
          EXTRACT_NUMBER(mcnt, p + 2);
          /* Originally, this is how many times we CAN jump.  */
          if (mcnt)
            {
               mcnt--;
               STORE_NUMBER(p + 2, mcnt);
	       goto nofinalize;	     /* Do the jump without taking off
			                any failure points.  */
            }
          /* If don't have to jump any more, skip over the rest of command.  */
	  else      
	    p += 4;		     
          break;

	case set_number_at:
	  {
  	    register unsigned char *p1;

            EXTRACT_NUMBER_AND_INCR(mcnt, p);
            p1 = p + mcnt;
            EXTRACT_NUMBER_AND_INCR(mcnt, p);
	    STORE_NUMBER(p1, mcnt);
            break;
          }

        /* Ignore these.  Used to ignore the n of succeed_n's which
           currently have n == 0.  */
        case unused:
          break;

        case wordbound:
	  if (AT_WORD_BOUNDARY)
	    break;
	  goto fail;

	case notwordbound:
	  if (AT_WORD_BOUNDARY)
	    goto fail;
	  break;

	case wordchar:
	  PREFETCH;
          if (!IS_A_LETTER(d))
            goto fail;
	  d++;
	  SET_REGS_MATCHED;
	  break;

	case notwordchar:
	  PREFETCH;
	  if (IS_A_LETTER(d))
            goto fail;
	  if (ismbchar(*d) && d + 1 != dend)
	    d++;
	  d++;
          SET_REGS_MATCHED;
	  break;

	case exactn:
	  /* Match the next few pattern characters exactly.
	     mcnt is how many characters to match.  */
	  mcnt = *p++;
	  /* This is written out as an if-else so we don't waste time
             testing `translate' inside the loop.  */
          if (translate)
	    {
	      do
		{
		  unsigned char c;

		  PREFETCH;
		  c = *d++;
		  if (*p == 0xff) {
		    p++;  
		    if (!--mcnt
			|| d == dend
			|| (unsigned char)*d++ != (unsigned char)*p++)
		      goto fail;
		    continue;
		  }
		  else if (ismbchar(c)) {
		    if (c != (unsigned char)*p++
			|| !--mcnt	/* redundant check if pattern was
					   compiled properly. */
			|| d == dend
			|| (unsigned char)*d++ != (unsigned char)*p++)
		      goto fail;
		    continue;
		  }
		  /* compiled code translation needed for ruby */
		  if ((unsigned char)translate[c]
		      != (unsigned char)translate[*p++])
		    goto fail;
		}
	      while (--mcnt);
	    }
	  else
	    {
	      do
		{
		  PREFETCH;
		  if (*p == 0xff) {p++; mcnt--;}
		  if (*d++ != *p++) goto fail;
		}
	      while (--mcnt);
	    }
	  SET_REGS_MATCHED;
          break;
	}
      continue;  /* Successfully executed one pattern command; keep going.  */

    /* Jump here if any matching operation fails. */
    fail:
      if (stackp != stackb)
	/* A restart point is known.  Restart there and pop it. */
	{
          short last_used_reg, this_reg;

          /* If this failure point is from a dummy_failure_point, just
             skip it.  */
	  if (!stackp[-2])
            {
              POP_FAILURE_POINT();
              goto fail;
            }

          d = *--stackp;
	  p = *--stackp;
          /* Restore register info.  */
          last_used_reg = (long) *--stackp;

          /* Make the ones that weren't saved -1 or 0 again. */
          for (this_reg = num_regs - 1; this_reg > last_used_reg; this_reg--)
            {
              regend[this_reg] = (unsigned char *)(-1L);
              regstart[this_reg] = (unsigned char *)(-1L);
              IS_ACTIVE(reg_info[this_reg]) = 0;
              MATCHED_SOMETHING(reg_info[this_reg]) = 0;
            }

          /* And restore the rest from the stack.  */
          for ( ; this_reg > 0; this_reg--)
            {
              reg_info[this_reg] = *(struct register_info *) *--stackp;
              regend[this_reg] = *--stackp;
              regstart[this_reg] = *--stackp;
            }
	}
      else
        break;   /* Matching at this starting point really fails.  */
    }

  if (best_regs_set)
    goto restore_best_regs;

  FREE_AND_RETURN(stackb,(-1)); 	/* Failure to match.  */
}


static int
memcmp_translate(s1, s2, len, translate)
     unsigned char *s1, *s2;
     register int len;
     unsigned char *translate;
{
  register unsigned char *p1 = s1, *p2 = s2, c;
  while (len)
    {
      c = *p1++;
      if (ismbchar(c)) {
	if (c != *p2++ || !--len || *p1++ != *p2++)
	  return 1;
      }
      else
	if (translate[c] != translate[*p2++])
	  return 1;
      len--;
    }
  return 0;
}

void
re_copy_registers(regs1, regs2)
     struct re_registers *regs1, *regs2;
{
    int i;

    if (regs1 == regs2) return;
    if (regs1->allocated == 0) {
	regs1->beg = TMALLOC(regs2->num_regs, int);
	regs1->end = TMALLOC(regs2->num_regs, int);
	regs1->allocated = regs2->num_regs;
    }
    else if (regs1->allocated < regs2->num_regs) {
	TREALLOC(regs1->beg, regs2->num_regs, int);
	TREALLOC(regs1->end, regs2->num_regs, int);
	regs1->allocated = regs2->num_regs;
    }
    for (i=0; i<regs2->num_regs; i++) {
	regs1->beg[i] = regs2->beg[i];
	regs1->end[i] = regs2->end[i];
    }
    regs1->num_regs = regs2->num_regs;
}

void
re_free_registers(regs)
     struct re_registers *regs;
{
    if (regs->allocated == 0) return;
    if (regs->beg) free(regs->beg);
    if (regs->end) free(regs->end);
}
