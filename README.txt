TAR - POSIX tar for 16-bit MS-DOS
==================================

Build (Linux host, Open Watcom in ~/opt/watcom):

    make                    # produces tar.exe (8086, large model)
    make test               # runs tests/run.sh under DOSBox-X

The version is the one line in VERSION.  When a PR that changes it is
merged to main, the release workflow tags the merge commit v<VERSION>
and publishes a GitHub release with tar.exe attached.

The tests need dosbox-x and GNU tar on the host (override with DOSBOX=
and HOSTTAR=); set KEEP=1 to keep the work directory.

Usage (POSIX/SUS tar utility; the leading '-' on the key is optional):

    tar -c[vwbfmo] [blocksize] [archive] file...    create
    tar -r[vwbf]   [blocksize] [archive] file...    append
    tar -t[vbf]    [blocksize] [archive] [file...]  list
    tar -u[vwbf]   [blocksize] [archive] file...    update (append if newer)
    tar -x[vwbflmo][blocksize] [archive] [file...]  extract
    tar --version                                   print the version

    v  verbose                  w  confirm each file (reads from CON)
    f  archive name, - = stdin/stdout (the default)
    b  blocking factor, 1-20 (default 20, i.e. 10240-byte records)
    m  do not restore modification times on extract
    l, o  accepted; no-ops (DOS has no links or file ownership)

Arguments for b and f follow the key in the order the letters appear,
e.g.  tar cbf 1 a.tar dir   or   tar cfb a.tar 1 dir.

Wildcards:
* c, r, u: operands containing * or ? are expanded by DOS (COMMAND.COM
  does not do it), using DOS rules, in the last path component only:
  tar cf src.tar *.c   or   tar cf src.tar src\*.c
* t, x: operands containing *, ? or [...] are POSIX patterns matched
  against member names, ignoring case.  As in pax, * and ? also match
  '/', so  tar xf src.tar *.c  extracts every .c file at any depth.  A
  pattern that matches a directory selects everything under it.

Output format is POSIX.1 ustar.  On input, ustar, V7, and pax extended
headers (path, linkpath, size, mtime) are understood.

DOS specifics
-------------
* Set TZ (e.g. SET TZ=CET-1CEST) so timestamps convert correctly to and
  from UTC; without it the Watcom runtime assumes EST5EDT.
* Stored names: drive letters removed, '\' becomes '/', lower-cased.
* Stored modes: 644 for files and 755 for directories, or 444 for
  read-only files.  uid/gid are 0 and no user or group name is stored.
* On extract: a member with no owner write permission is made read-only.
  Hard links are created as copies of the linked file.  Symbolic links
  and device/FIFO members are skipped with a diagnostic.  Names are
  passed to DOS as-is: long components are truncated to 8.3 by DOS
  itself, and names DOS rejects are reported as errors.
* The archive file itself is never added to the archive.

Exit status: 0 success, 1 if some files could not be processed,
2 for a fatal error (usage, unreadable or corrupt archive).
