# 4x4 Matrix Wiring

This wiring matches the physical plate in the reference photo. Do not flash the
matrix firmware until the common-ground wiring has been removed and every key
has a diode.

## GPIO assignment

| Matrix line | ESP32-S3 GPIO | Direction |
| --- | ---: | --- |
| Row 0 | 8 | Output, scanned low |
| Row 1 | 9 | Output, scanned low |
| Row 2 | 10 | Output, scanned low |
| Row 3 | 11 | Output, scanned low |
| Column 0 | 4 | Input pull-up |
| Column 1 | 5 | Input pull-up |
| Column 2 | 6 | Input pull-up |
| Column 3 | 7 | Input pull-up |

Keep the encoder on GPIO 17/18/12, RGB data on GPIO 47, and GPIO 19/20 unused
for native USB.

## Physical map

The encoder is outside the switch matrix. Empty positions have no switch.

```text
                         C0       C1       C2       C3
                     +--------+--------+--------+--------+
R0 / GPIO 4          | empty  | Task 1 | Task 2 | empty  |
                     +--------+--------+--------+--------+
R1 / GPIO 5          | Task 3 | Task 4 | Task 5 | Task 6 |
                     +--------+--------+--------+--------+
R2 / GPIO 6          | Fast   | Approve| Reject |Continue|
                     +--------+--------+--------+--------+
R3 / GPIO 7          | empty  | Mic B  | Mic A  | Send   |
                     +--------+--------+--------+--------+
```

## One switch cell

Use one diode per switch. Use the same orientation everywhere:

```text
column wire ----|>|---- switch ---- row wire
                diode
```

The diode stripe/cathode faces the row wire. Do not connect any switch
terminal directly to ground. Each row is a continuous bus, and each column is a
continuous bus; a switch connects one row to one column through its diode.

## Wiring procedure

1. Remove the existing blue common-ground bus and individual GPIO signal wires.
2. Install one 1N4148 or 1N4148W diode on every switch.
3. Join the four row buses across the switch positions shown above.
4. Join the four column buses across the switch positions shown above.
5. Run eight wires from the matrix to GPIO 4-11.
6. Check every switch with a multimeter: open when released, and one diode
   direction only when pressed.
7. Flash the matrix firmware only after the continuity checks pass.

The two microphone keycaps can remain physically joined as one wide key, but
Mic A and Mic B remain separate electrical switches and can be assigned
independently in the mapper.

## Encoder wiring

The EC11 encoder is separate from the key matrix. With the encoder shaft facing
you, identify its pins using continuity mode because pin order varies between
manufacturers.

```text
EC11 pin/function       ESP32-S3
------------------      --------
Encoder A / CLK         GPIO 17
Encoder B / DT          GPIO 18
Encoder common          GND
Push switch pin 1       GPIO 12
Push switch pin 2       GND
```

The firmware uses `INPUT_PULLUP`, so do not add external pull-up resistors. The
encoder and push switch are active-low: closing either contact connects its
GPIO to ground.

For a typical five-pin EC11, the three encoder pins are the group of three
terminals and the two push-switch pins are the separate pair. Use a multimeter
to find the common encoder terminal: it alternates continuity with A and B as
the shaft turns. If clockwise and counter-clockwise are reversed after wiring,
swap A/B or enable the firmware's reverse-direction setting.