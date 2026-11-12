# Transcript — démo live Ambre sur QNX 8.0 (12/11/2026)

Commande : `scripts/qnx-demo.sh` (boot → crash → coupure d'alimentation → reboot → reprise).

```
=== BOOT 1 — la conduite, le choc, l'acquisition ===
==============================================================
== Ambre EDR — live on QNX 8.0
==============================================================
journal partition : /dev/hd0t177  (raw, no file system)
service path      : /dev/edr/journal
==============================================================
== 1. edr-journal starts: it opens the partition and runs the recovery (REP-01)
==============================================================
==============================================================
== 2. edrctl status — the service answers
==============================================================
mode            continuous
backend         qnx
health          ok
supervised      no
segment         0 (next block 50)
events          0 open, 0 acquired, 0 sealed
refused         0 (no space)
bytes written   512
flushes         1
uptime          18 ms
last freeze     0 us
worst freeze    0 us
==============================================================
== 3. first boot: the partition is empty, so we drive the crash
==============================================================
-- edr-capture starts (priority 61) and listens for the frames
-- edr-replay: 4 s of driving, frontal impact at 2.5 s
==============================================================
== 4. edrctl list — the event the recorder acquired
==============================================================
event 1    open   locked=yes complete_file=no  reason=restraint       age=10 ms (t+23589237 us) seg=0 blk=90 seq=91
==============================================================
== 5. edrctl status after the impact
==============================================================
mode            continuous
backend         qnx
health          ok
supervised      no
segment         0 (next block 93)
events          1 open, 1 acquired, 0 sealed
refused         0 (no space)
bytes written   22528
flushes         26
uptime          2592 ms
last freeze     10225 us
worst freeze    10225 us
==============================================================
== READY — the crash is on the media. The host now cuts the power (SIGKILL on QEMU).
==============================================================
-- the demo stays alive; cut the power whenever you want.
edr-replay: 400 frames, 9640 samples, 2 external signal(s) in 3991 ms
send round trip (PERF-05 / essay E4): 402 messages, mean 636350 ns, worst 68688000 ns
  p50 <= 25000 ns, p99 <= 18446744073709551615 ns, p99.9 <= 18446744073709551615 ns
  <=    5000 ns :       10  ( 2.49%)
  <=   10000 ns :        5  ( 1.24%)
  <=   25000 ns :      265  (65.92%)
  <=   50000 ns :       72  (17.91%)   <- PERF-05 threshold
  <=  100000 ns :       22  ( 5.47%)
  <=  250000 ns :        7  ( 1.74%)

=== COUPURE D'ALIMENTATION ===
kill -9 <qemu>  # la tension 12 V disparaît, le cache volatil avec

=== BOOT 2 — même disque, la reprise ===
==============================================================
== Ambre EDR — live on QNX 8.0
==============================================================
journal partition : /dev/hd0t177  (raw, no file system)
service path      : /dev/edr/journal
==============================================================
== 1. edr-journal starts: it opens the partition and runs the recovery (REP-01)
==============================================================
==============================================================
== 2. edrctl status — the service answers
==============================================================
mode            continuous
backend         qnx
health          ok
supervised      no
segment         0 (next block 139)
events          0 open, 0 acquired, 0 sealed
refused         0 (no space)
bytes written   512
flushes         1
uptime          21 ms
last freeze     0 us
worst freeze    0 us
==============================================================
== 3. AFTER THE POWER CUT — the journal recovered the partition at boot
==============================================================
-- what the black box still holds:
event 1    sealed locked=yes complete_file=yes reason=restraint       age=0 ms (t+23589237 us) seg=0 blk=90 seq=91
event 2    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23643333 us) seg=0 blk=99 seq=100
event 3    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23682080 us) seg=0 blk=103 seq=104
event 4    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23684080 us) seg=0 blk=105 seq=106
event 5    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23720541 us) seg=0 blk=113 seq=114
event 6    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23722541 us) seg=0 blk=115 seq=116
event 7    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23724541 us) seg=0 blk=117 seq=118
event 8    sealed locked=yes complete_file=yes reason=dv_long         age=0 ms (t+23726541 us) seg=0 blk=119 seq=120
==============================================================
== 4. edrctl status after the recovery
==============================================================
mode            continuous
backend         qnx
health          ok
supervised      no
segment         0 (next block 139)
events          0 open, 0 acquired, 0 sealed
refused         0 (no space)
```
