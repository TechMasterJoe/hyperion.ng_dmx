# Custom DMX Fixture Support

This fork adds custom DMX fixture definitions for both **Art-Net** (`udpartnet`) and **USB DMX** (`dmx`) devices.

## Features

- Define one or more fixture types
- Set the number of DMX channels per fixture
- Assign roles to channels: `R`, `G`, `B`, `W`, `H` (always 255), `L` (always 0), `F` (fixed value)
- Provide default/fixed values for non-color channels
- Optionally map individual Hyperion LEDs to specific fixture types via `ledFixtureMap`
- Fully backward compatible — if `customFixtures` is absent, the original behavior is used

## Configuration (expert mode)

Add these keys to the device configuration (JSON):

```json
"customFixtures": [
  {
    "name": "RGBD Par",
    "channelConfig": "RGBH",
    "fixedValues": [0, 0, 0, 255]
  },
  {
    "name": "RGBW",
    "channelRoles": ["R", "G", "B", "W"],
    "channelCount": 4
  }
],
"ledFixtureMap": [0, 0, 1, 1, 0]
```

### Channel roles

| Role | Meaning |
|------|---------|
| R    | Red from Hyperion LED |
| G    | Green from Hyperion LED |
| B    | Blue from Hyperion LED |
| W    | White (calculated via whiteAlgorithm if set) |
| H    | Always 255 (useful for master dimmer) |
| L    | Always 0 |
| F    | Use the corresponding value from `fixedValues` |

You can use either `channelRoles` (array) or the compact `channelConfig` string (e.g. `"RGBH"`).

### LED → Fixture mapping

`ledFixtureMap` is an array of 0-based indices into `customFixtures`.  
If shorter than the LED count, the last entry is repeated.  
If omitted, all LEDs use fixture index 0.

## USB DMX

Select device type **custom** (or leave as raw/McCrypt and still supply `customFixtures` — custom fixtures take priority when present).

## Art-Net

Works with the existing `channelsPerFixture` / `whiteAlgorithm` settings when custom fixtures are not defined. When custom fixtures are present they override the per-LED channel packing.

## Notes

- Universe rollover on Art-Net is preserved.
- White channel calculation reuses Hyperion’s existing `RGBW::Rgb_to_Rgbw` algorithms.
- Tested conceptually against the open request in hyperion-project/hyperion.ng#1667.
