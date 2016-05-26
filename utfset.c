/*
 * See LICENSE file for copyright and licence details.
 *
 * This code implements a UTFSet, a set containing Unicode characters (`runes')
 * in a tree structure that mirrors the UTF-8 encoding format. The result is an
 * exceedingly simple data structure, implemented here in under 75 lines of C,
 * which nevertheless takes up less space when storing smaller (and generally
 * more common) runes, and more when storing larger ones. It may not be the most
 * efficient, but it is kind of neat.
 */
#include <stdint.h>
#include <stdlib.h>
#include <uchar.h>

/*
 * The structure underlying a UTFSet is a 64-ary tree (a tree whose nodes each
 * have 64 child nodes), with booleans for leaves. With the exception of the
 * root, which is special as described below, a node has homogenous children:
 * they are either all bool, all node<bool>, all node<node<bool>>, and so on.
 *
 * As an optimisation for nodes whose children are all bool, instead of storing
 * a 64-bit pointer to an array of 64 bools, we store a 64-bit bitmask in the
 * space that would have stored the pointer to the node. As a result, a node is
 * either a 64-bit pointer to an array of 64 child nodes or, in the case of a
 * leaf node, a 64-bit bitmask.
 */
struct block {
	union child {
		struct block *ptr;
		uint64_t bits;
	} blk[64];
};

/*
 * At the root of the tree is a node whose children are not homogenous; their
 * types depend on which index they have. The first 32 are node<bool> (and so
 * are all bitmasks), the next 16 are node<node<bool>>, the next 8 after that
 * are node<node<node<bool>>>, and so on.
 *
 * Note the correspondence here with the leading byte sequences in UTF-8. A
 * leading byte has the bit pattern 11xxxxxx, where the x bits may be 0 or 1.
 * There are 64 possible values for such a byte. The first 32, 11000000 up to
 * 11011111, have one continuation byte; the next 16, 11100000 up to 11101111,
 * have two continuation bytes; and so on.
 *
 * The tree is indexed in a similar way to a prefix tree, or `trie': the first
 * node is indexed by a 6-bit integer, comprising the x bits of the leading byte
 * 11xxxxxx. The second node is indexed by a subsequent 6-bit integer, which are
 * the x bits of the continuation byte 10xxxxxx. This continues until no more
 * continuation bytes are expected to follow the initial leading byte.
 *
 * There is no accommodation for ASCII bytes, i.e. 0xxxxxxx, which are instead
 * dealt with in a special way, described later.
 */
typedef struct block UTFSet;

/*
 * This table is used to look up the number of leading ones in a 6-bit integer.
 * Given some leading byte 11xxxxxx, if there are n leading ones in the 6-bit
 * value xxxxxx, then there are n+1 continuation bytes to follow.
 *
 * Note that the final four values, having 4 or more leading ones, are not valid
 * in UTF-8 according to RFC 3629.
 */
const char clo6[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00xxxx */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 01xxxx */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 10xxxx */
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 6, /* 11xxxx */
};

/*
 * addutf() adds the next rune in the UTF-8 string to the UTFSet, and returns a
 * pointer to the byte immediately after that rune.
 */
const char *
addutf(UTFSet *set, const char *s)
{
	/*
	 * For clarity of code, we will assume that the string is valid UTF-8.
	 * First, we read the first byte. This will tell us which of the root's
	 * children to start with, and also how many continuation bytes follow.
	 */
	unsigned char c = *s++;
	unsigned int n;

	/*
	 * We write UTF-8 bytes in octal, because their structure is clearer in
	 * this form. In octal 03xx is a leading byte, 02xx a continuation byte,
	 * and 00xx or 01xx an ASCII byte.
	 */
	if (c >= 0300) {
		/*
		 * This is a leading byte, 11xxxxxx. The number of leading ones
		 * in the lower 6 bits tells us how many continuation bytes are
		 * expected. We also want to use the lower 6-bits as the first
		 * index.
		 */
		c %= 64;     /* extract the lower bits */
		n = clo6[c]; /* there are n+1 bytes left */
	} else if (c < 0200) {
		/*
		 * This is an ASCII byte, 0xxxxxxx. One such byte like this is
		 * equivalent to two UTF-8 bytes of the form 1100000x 10xxxxxx.
		 * This means we can take the 7th bit as the first index, and
		 * then stay on the same byte so we can later read the lower 6
		 * bits as if they were a subsequent byte.
		 *
		 * Note that there is no wastage in treating an ASCII byte as
		 * two UTF-8 bytes, as the root's first 32 children are just
		 * bitmasks and so do not require any extra allocation.
		 */
		c /= 64; /* extract the higher bit */
		n = 0;   /* there is one byte left, */
		s--;     /* which is this one again */
	} else {
		/*
		 * This is a continuation byte, 10xxxxxx. This should not start
		 * a UTF-8 codepoint, so we return NULL to indicate an error.
		 */
		return NULL;
	}

	/*
	 * Given a byte 11xxxxxx (or ASCII equivalent - see above), we start our
	 * traversal of the tree at the xxxxxx-th child of the root node.
	 */
	union child *tp = &set->blk[c];

	/*
	 * There are n+1 bytes left. The first n will be handled as if a trie:
	 * with byte 10xxxxxx we will traverse to this node's xxxxxx-th child.
	 */
	for (; n > 0; n--) {
		/* If this child doesn't point anywhere yet, allocate a block. */
		if (tp->ptr == NULL) {
			tp->ptr = calloc(1, sizeof *tp->ptr);
			if (tp->ptr == NULL)
				return NULL; /* out of memory */
		}
		c = (unsigned char)*s++ % 64; /* extract the lower bits */
		tp = &tp->ptr->blk[c];        /* traverse to child node */
	}

	/*
	 * This is the final byte, so this node's children are boolean leaves.
	 * This means we need to set a bit in the bitmask to indicate that the
	 * rune corresponding to this byte sequence is an element of the set.
	 */
	c = (unsigned char)*s++ % 64; /* extract the lower bits */
	tp->bits |= UINT64_C(1) << c; /* set bit in the bitmask */

	return s;
}

static void foreach1(union child, unsigned int, char32_t, void (*)(char32_t));

/*
 * foreach() takes a pointer to a function that takes a rune, and calls that
 * function for each rune in the set, in ascending order.
 *
 * It may be worth noting that if an overlong UTF-8 sequence is entered into the
 * UTFSet then the runes will not actually be in true ascending order, as the
 * overlong rune will be reached after the `true' rune. However, since overlong
 * UTF-8 sequences are illegal, this is not a problem if the input is sanitised.
 */
void
foreach(const UTFSet *set, void (*fcn)(char32_t))
{
	/*
	 * For each possible leading byte (11xxxxxx), we determine the number of
	 * continuation bytes that will follow it, and unpack the bits that will
	 * contribute to the value of the rune. That is, all of the bits after
	 * the leading ones.
	 */
	for (unsigned char c = 0; c < 64; c++) {
		unsigned int n = clo6[c];   /* there are n+1 bytes left */
		char32_t r = c % (64 >> n); /* unpack bits for the rune */

		foreach1(set->blk[c], n, r, fcn);
	}
}

/*
 * foreach1() is the recursive workhorse used by foreach(). Whereas foreach()
 * applies to a UTFSet, foreach1() applies to an individual node in the tree.
 * As well as the node, which may be either a pointer or a bitmask, foreach1()
 * is passed: the number of bytes still to go, the value of the rune extracted
 * from bytes so far, and the function to be called for each rune in the set.
 */
void
foreach1(union child t, unsigned int n, char32_t r, void (*fcn)(char32_t))
{
	if (n > 0) {
		/*
		 * This is not the final byte, so this node's children (if it
		 * has any) are also nodes. Recurse over them for each possible
		 * continuation value (10xxxxxx). We append the byte's value
		 * onto that of the rune so far.
		 */
		if (t.ptr != NULL) {
			for (unsigned char c = 0; c < 64; c++) {
				foreach1(t.ptr->blk[c], n - 1, (r * 64) + c, fcn);
			}
		}
	} else {
		/*
		 * This is the final byte, so this node's children are booleans,
		 * which means this is a bitmask. Call the function for each set
		 * bit, again appending the value onto that of the rune so far.
		 */
		for (unsigned char c = 0; c < 64; c++) {
			if ((t.bits & (UINT64_C(1) << c))) {
				fcn((r * 64) + c);
			}
		}
	}
}

/*
 * The following are examples of how this data structure is to be used.
 */
#include <inttypes.h>
#include <stdio.h>

/*
 * prune() is an example function for passing to foreach(). It prints the rune
 * it is passed, in the usual U+0000 format, to stdout.
 */
void
prune(char32_t r)
{
	printf("U+%04" PRIX32 "\n", (uint32_t)r);
}

/*
 * prunes() is an example function which, given a UTF-8 string, will print the
 * runes it contains in ascending order. Returns 0 on success, -1 on failure.
 */
int
prunes(const char *s)
{
	UTFSet set = { 0 }; /* empty set */

	while (*s != '\0') {
		s = addutf(&set, s); /* add next rune to set */
		if (s == NULL)
			return -1; /* something went wrong */
	}

	foreach(&set, &prune); /* print all runes */
	return 0;
}
