SessionOpenMP ping test -- proves the network link WITHOUT the game.

1. Unzip anywhere. Run omp_ping.exe  (add --relay to test Epic relay routing)
2. It prints YOUR ID. Send it to the other player, paste theirs in, press enter.
3. Let it run ~2 minutes, then send back omp_ping.log (created next to the exe).

The [ping] line each second shows both directions separately:
  THEIR->ME N/s   = how much of their traffic reaches you (should be ~60)
  ME->THEM rtt    = round trip; pongs/s ~60 means your traffic reaches them too
  link=DIRECT/RELAY = how EOS routed the connection
If the plain run shows one bad direction, run BOTH sides again with --relay.
