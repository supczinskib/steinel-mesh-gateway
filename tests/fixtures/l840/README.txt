Anonymized device reports supplied by mitch-thomas on 2026-10-18:
https://github.com/supczinskib/steinel-mesh-gateway/issues/2#issuecomment-6252519552

sc-near.json: L 840 SC, primary address 0x0018, near the gateway.
c-near.json:  L 840 C, primary address 0x000c, near the gateway.
sc-far.json:  L 840 SC, primary address 0x0012, farther from the gateway.

Each file is a separate collection. Observation timestamps are browser-relative;
packet timestamps are gateway-relative. No off-state reply was captured after
the final light_off marker. missed_events counts diagnostic-buffer gaps, not
radio packet loss. These reports are inputs for replay, not a radio simulator.
