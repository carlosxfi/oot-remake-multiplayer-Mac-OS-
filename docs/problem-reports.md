# Problem reports

The kit can put the co-op problems it finds in a player's game logs into a small zip, masked as collect-logs masks its
zip, for the player to send with the `/report` bot command. Nothing is sent by the kit: no webhook, no network. It is
opt-in: the start asks once, and the answer is kept.

Windows only for now: the Linux kit's `start.sh` does not ask, and has no `problem-report` script.

collect-logs (`collect-logs.cmd`) stays the way to send whole logs for one report. Problem reports are the small, regular
side: a few lines a session, and only problems not reported before.

## How a player meets it

- **Asked once.** The first start after the update (`start.ps1`, step 5b) asks: `Make problem reports? [y/N]`. No is the
  default. The answer is kept in `logs\problem-reports.json`.
- **With yes:** each start looks at the game's logs of the sessions since the last report. When they hold a problem never
  reported before, it writes `logs\coop-report-<date>.zip` and says: send it with `/report` in the Discord. Nothing is
  written when there is nothing new; the newest 10 zips are kept. Nothing here can stop the start (any error skips this
  step).
- **At any time:** `scripts\problem-report.cmd` makes a report at once, whatever the answer.
- **Changing the answer:** `scripts\problem-report.cmd on` or `scripts\problem-report.cmd off` (the kit's README says so
  too).

## What is in the zip

`problem-report.txt`: the kit's version, then one line per new problem with how many times it came
(`2x [hos Presentation Thread] coop: boss: could not move into the boss room ... giving up`).

The problems are the module's own warnings and errors, lines saying something failed, could not be done, was refused,
gave up, timed out or was lost, faults the guard caught, and crashes. The words are matched as whole words and as the
module writes them (lower case), so a scene's name (`Lost Woods`) or a note that only mentions a crash is not a problem.
Routine status lines that only contain such words (the pose and actor counters, `camera lost, markers off`, the invite
note) are left out.

Every line is masked with collect-logs' own `Protect-LogText` (`scripts\log-mask.ps1`): IP addresses become their kind
(`<lan-1>`, `<public-1>`, ...), MAC addresses `<mac>`, the Windows user name `<user>`, the PC name `<pc>`, invite codes and
passwords `(removed)`. Players' names become `<name>`: the ones the module writes where a name goes (`player 2 joined: X`,
`player 2 left: X (...)`, `player 2 'X' joined`, `Mods: player 2 (X)`, our own `name 'X'`), and those names anywhere
later in the same log. After that, a last gate drops any line in which anything is still an address by the masking's
own rules (`Get-MaskKind4`, `Get-MaskKind6`), a host name (`myhost.duckdns.org`; file names such as `hookshot_coop.dll`
pass) or a host with a port (`myhost:27510`): nothing that may be one goes into the zip.

## A problem in one report only

Each masked line becomes a fingerprint: its thread tag, the masks' numbers (`<lan-1>`, `<lan-2>`), addresses and numbers
blanked, and names already `<name>`, so the same problem at another address, frame, count or with another player is one
fingerprint. `logs\problem-reports.json` keeps the sessions read (each log's `run_id` line, else its size and first
64 KB) and the fingerprints reported, oldest first (the newest 5000 are kept): a session is read once, and a problem
reported once is never in a later report, from any session. The log of a game still running is left for the next start.

## Reading the logs

A session's key comes from the start of its log only, so a session read before is not read again (a start with nothing
new reads 64 KB a log). A new session is read a line at a time. Logs are opened as collect-logs opens them (shared for
reading, writing and deleting), so a log another program still has open is read; one that cannot be opened at all is
skipped, and tried again at the next start, while the others are still read.

Tests: `tools/release/tests/problem_report_test.ps1` (made-up log lines only).
