# line-diff

A command-line tool that prints the differences between two text files as a
unified diff.

## Usage

```
app [-U N] OLD NEW
```

- `-U N` (default 3): the number of context lines, a whole number ≥ 0. It
  comes before the file names.
- Exit code: 0 if the files are equal (nothing is printed), 1 if they differ,
  2 on any error.

## Lines

A file is split into lines after each LF. Every line includes its LF,
except possibly the last one; an empty file has no lines. Two lines are
equal only if they are byte-for-byte the same, including the LF (so `a`
at the end of a file without LF differs from `a` + LF, and a CR before the
LF is part of the line).

## Which lines change

Let A (n lines) and B (m lines) be the two files, and L(i, j) the length of
the longest common subsequence of A[i..] and B[j..] (0-based suffixes).
Start at i = 0, j = 0 and repeat until i = n and j = m:

- if i < n, j < m and A[i] = B[j]: the line is **kept**; i += 1, j += 1;
- else if j = m, or i < n and L(i+1, j) ≥ L(i, j+1): A[i] is **removed**;
  i += 1;
- else B[j] is **added**; j += 1.

## Output

If any line was removed or added, print:

```
--- OLD
+++ NEW
```

(OLD and NEW as given on the command line), then the hunks. A hunk covers a
run of changes plus up to N kept lines before and after it; two runs of
changes separated by at most 2·N kept lines are in the same hunk. Each hunk
starts with

```
@@ -S1,C1 +S2,C2 @@
```

where C1 and C2 are the numbers of OLD and NEW lines in the hunk (kept plus
removed, kept plus added) and S1, S2 the 1-based line numbers of its first
OLD and NEW line. A count of 1 is written without `,1` (`-3` rather than
`-3,1`). If a count is 0, the start is the number of the line just before
the hunk (0 at the start of the file): `-0,0`, `+4,0`.

Then the hunk's lines in the order the steps above produced them: a kept
line prefixed with a space, a removed line with `-`, an added line with
`+`. A line without a final LF is printed followed by LF and then the line
`\ No newline at end of file`.

## Errors

A file that can't be read: print `error: cannot read FILE` (FILE as given)
to standard error and exit with code 2. Not exactly two file names, an
unknown option, or a bad `-U` value: print a message starting with `usage:`
to standard error and exit with code 2. Nothing goes to standard output.

## Example

```
$ printf 'a\nb\nc\nd\ne\nf\n' > old.txt; printf 'a\nc\nd\ne\nf\ng\n' > new.txt
$ app -U 1 old.txt new.txt
--- old.txt
+++ new.txt
@@ -1,3 +1,2 @@
 a
-b
 c
@@ -6 +5,2 @@
 f
+g
```
