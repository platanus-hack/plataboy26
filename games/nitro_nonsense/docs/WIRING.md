# Parallel cable topology

These are signal diagrams, not connector orientation drawings. Identify the
GND and SWIO labels on each actual board before making the cable. Each console
uses its own normal power source. Leave VCC unconnected. No PCB modification is
proposed. SWIO/PD1 is shared as an open-drain data line; GND is its common reference.

## Two consoles

```mermaid
flowchart LR
  subgraph P1[Console 1]
    G1[GND]
    S1[SWIO / PD1]
  end
  subgraph P2[Console 2]
    G2[GND]
    S2[SWIO / PD1]
  end
  G1 --- GND[Common GND bus]
  GND --- G2
  S1 --- DATA[Common SWIO bus]
  DATA --- S2
```

## Three consoles

```mermaid
flowchart LR
  subgraph P1[Console 1]
    G1[GND]
    S1[SWIO / PD1]
  end
  subgraph P2[Console 2]
    G2[GND]
    S2[SWIO / PD1]
  end
  subgraph P3[Console 3]
    G3[GND]
    S3[SWIO / PD1]
  end
  G1 --- GND[Common GND bus]
  G2 --- GND
  G3 --- GND
  S1 --- DATA[Common SWIO bus]
  S2 --- DATA
  S3 --- DATA
```

## Four consoles

```mermaid
flowchart LR
  subgraph P1[Console 1]
    G1[GND]
    S1[SWIO / PD1]
  end
  subgraph P2[Console 2]
    G2[GND]
    S2[SWIO / PD1]
  end
  subgraph P3[Console 3]
    G3[GND]
    S3[SWIO / PD1]
  end
  subgraph P4[Console 4]
    G4[GND]
    S4[SWIO / PD1]
  end
  G1 --- GND[Common GND bus]
  G2 --- GND
  G3 --- GND
  G4 --- GND
  S1 --- DATA[Common SWIO bus]
  S2 --- DATA
  S3 --- DATA
  S4 --- DATA
```

All GND terminals share one conductor. All SWIO terminals share a different
conductor. GND and SWIO are never joined. This is one parallel bus, not a ring
and not separate point-to-point links. Player numbers come from UID arbitration,
not connector position: physical Console 2 can become P3.

## Physical acceptance remains pending

The owner has no consoles yet. The emulator cannot establish electrical operation,
connector orientation, pull-up strength, cable capacitance/length tolerance,
clock drift or programming recovery on real boards. Measure idle/high and low
levels, rise times and bit timing with two, three and four powered consoles;
verify no output drives high against a peer pulling low. Test mixed power states,
power cycles and unplug/replug separately. Hold B at boot to retain the debug pin
and test programming recovery on each physical console before release.
