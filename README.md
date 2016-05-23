# UTFSet

This code implements a UTFSet, a set containing Unicode characters (‘runes’)
in a tree structure that mirrors the UTF-8 encoding format. The result is an
exceedingly simple data structure, implemented here in under 75 lines of C,
which nevertheless takes up less space when storing smaller (and generally
more common) runes, and more when storing larger ones. It may not be the most
efficient, but it is kind of neat.

The code is very well commented; read `utfset.c` in order to understand it.
