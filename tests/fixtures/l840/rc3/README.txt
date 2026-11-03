Public captures supplied by mitch-thomas on 2026-10-20:
https://github.com/supczinskib/steinel-mesh-gateway/issues/2#issuecomment-6300914157

sc-0018-motion.json:  L 840 SC 0x0018, autonomous motion cycle, full recording.
sc-0018-command.json: L 840 SC 0x0018, HA command cycle, stopped at 88476 ms.
c-000c-command.json:  L 840 C  0x000c, another HA command cycle, full recording.

These are original report bytes, copied without changing events or observations.
They are not synchronized captures. Duplicate light_on markers have no per-lamp
label. Browser markers and gateway events use different clocks. Ordinary command
sends and queue arrival times are absent. missed_events counts browser export
gaps in the diagnostic ring, not lost Mesh packets. Transition target/remaining
fields are absent from exported output records.

The pasted HA action sequence is Output Off for all three lamps, followed by
Automatic control On. It is retained literally as a separate model input, not
silently replaced with Output On. The comment does not establish whether this
is the intended turn-on branch or the restoration branch of the automation.
