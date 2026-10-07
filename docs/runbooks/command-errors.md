# Runbook: elevated command errors

**Symptoms:** `bitkv_command_errors_total` rising faster than 1% of commands.

## Diagnose

1. Which commands? Compare `rate(bitkv_commands_total[5m])` by `command` with
   the error rate. A rise in `command="unknown"` means clients are sending
   unsupported commands (a client library or version mismatch).
2. `INCR` errors usually mean a non-integer value stored under a counter key.
3. Errors containing `IOError` mean the disk failed a write. After a write
   failure bitkv refuses all further writes until restarted (so it cannot
   append after a damaged record). Check disk space and `dmesg`.

## Fix

- Unsupported commands: fix the client, or add the command.
- Disk write failure: free or expand disk, then restart. Recovery will
  truncate any partial record.

## Verify

Error ratio back under 1% for 15 minutes.
