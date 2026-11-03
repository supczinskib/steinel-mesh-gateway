Reports supplied by mitch-thomas after installing 2.0.1-rc.1:
https://github.com/supczinskib/steinel-mesh-gateway/issues/2#issuecomment-6273601201

sc-near.json: L 840 SC, 0x0018, near the gateway.
c-near.json:  L 840 C,  0x000c, near the gateway.
sc-far.json:  L 840 SC, 0x0012, farther from the gateway.

These are separate motion-triggered cycles, not simultaneous recordings.
Browser observation markers and gateway packet timestamps use different clocks.
All reports cover 180 seconds. sc-near has no physical switch-off marker.
Regular request sends/timeouts are not included; diagnostic sends/timeouts are.
The records cannot identify whether a gap between regular replies was caused
by a delayed request, a lost request or a lost reply.

test_l840_network.py replays the captured packets through production callbacks,
then runs a closed-loop model of the three-device group. Composition and packet
payloads come from these reports. Modelled physical transitions use the two
complete on/off observation pairs. Diagnostic success/failure order and measured
round-trip times form response profiles, with explicit variants for the unknown
prehistory, ordinary-request losses and an unreachable device. The model does
not treat old reply arrival times as fixed device response opportunities.
