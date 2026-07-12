# Script-graph node reference

GENERATED from the live node registry (AetherDocGen) - do not edit.
See Docs/script-graphs.md for the graph file schema. Node JSON shape:
`{ "id": "<unique>", "type": "<Type>", "p": "<param>", "n": <number>,
   "exec": ["<next-id>", ...], "in": [<input>, ...], "x": 0, "y": 0 }`
where each input is either a link `{ "from": "<node-id>", "out": <index> }`
or a literal (`{"t":"Float","f":1}`, `{"t":"Vec3","v":[x,y,z]}`,
`{"t":"Bool","b":true}`, `{"t":"String","s":"text"}`).
`exec` lists one target node id per exec-out pin ("" = end). Events have
no exec-in; pure nodes (no exec pins) evaluate on demand.

## Events

### OnStart

- event (no exec-in; fires on its own)
- exec out: `then`

### OnUpdate

- event (no exec-in; fires on its own)
- exec out: `then`
- out 0: `Dt` (Float)

### OnKey

- event (no exec-in; fires on its own)
- exec out: `pressed`
- param `p` (string): Key (single char)

### OnPlayerNear

- event (no exec-in; fires on its own)
- exec out: `then`
- out 0: `Player` (Entity)
- param `p` (string): Player entity (def MainCamera)
- param `n` (number): Radius

### OnFlag

- event (no exec-in; fires on its own)
- exec out: `then`
- param `p` (string): Flag
- param `n` (number): Equals value

### OnUIButton

- event (no exec-in; fires on its own)
- exec out: `clicked`
- param `p` (string): Button id

### OnAction

- event (no exec-in; fires on its own)
- exec out: `pressed`
- param `p` (string): Action name (input map)

## Flow

### If

- exec out: `true`, `false`
- in 0: `Cond` (Bool) default `0`

### Sequence

- exec out: `a`, `b`, `c`

### Delay

- exec out: `then`
- in 0: `Seconds` (Float) default `1`

### Once

- exec out: `then`

### ForLoop

- exec out: `body`, `done`
- in 0: `Count` (Float) default `3`
- out 0: `Index` (Float)

## Functions

### Function

- pure (no exec pins; evaluated when an input pulls it)
- exec out: `body`
- param `p` (string): Function name

### Call

- exec out: `then`
- param `p` (string): Function name

### Return


## Variables

### GetVar

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Value` (Float)
- param `p` (string): Variable name

### SetVar

- exec out: `then`
- in 0: `Value` (Float) default `0`
- param `p` (string): Variable name

## Entity

### Self

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Entity` (Entity)

### FindEntity

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Name` (String) default `0`
- out 0: `Entity` (Entity)
- param `p` (string): Name (if input empty)

### GetPosition

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Entity` (Entity) default `0`
- out 0: `Position` (Vec3)

### SetPosition

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Position` (Vec3) default `0`

### Translate

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Delta` (Vec3) default `0`

### RotateYaw

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Degrees` (Float) default `0`

### GetForward

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Entity` (Entity) default `0`
- out 0: `Forward` (Vec3)

### SetActive

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Active` (Bool) default `true`

### DestroyEntity

- exec out: `then`
- in 0: `Entity` (Entity) default `0`

## Physics

### GetVelocity

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Entity` (Entity) default `0`
- out 0: `Velocity` (Vec3)

### SetVelocity

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Velocity` (Vec3) default `0`

### AddImpulse

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Impulse` (Vec3) default `0`

### IsGrounded

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Entity` (Entity) default `0`
- out 0: `Grounded` (Bool)

### IsOverlapping

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Entity` (Entity) default `0`
- out 0: `Overlapping` (Bool)

### Raycast

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Origin` (Vec3) default `0`
- in 1: `Direction` (Vec3) default `(0, -1, 0)`
- in 2: `MaxDist` (Float) default `100`
- out 0: `Hit` (Bool)
- out 1: `Entity` (Entity)
- out 2: `Point` (Vec3)

## Math

### Add

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `0`
- out 0: `Out` (Float)

### Subtract

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `0`
- out 0: `Out` (Float)

### Multiply

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `1`
- out 0: `Out` (Float)

### Divide

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `1`
- out 0: `Out` (Float)

### Min

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `0`
- out 0: `Out` (Float)

### Max

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `0`
- out 0: `Out` (Float)

### Power

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `2`
- out 0: `Out` (Float)

### Sin

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- out 0: `Out` (Float)

### Cos

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- out 0: `Out` (Float)

### Abs

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- out 0: `Out` (Float)

### Clamp

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `Min` (Float) default `0`
- in 2: `Max` (Float) default `1`
- out 0: `Out` (Float)

### Lerp

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `1`
- in 2: `T` (Float) default `0`
- out 0: `Out` (Float)

### Random

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Min` (Float) default `0`
- in 1: `Max` (Float) default `1`
- out 0: `Out` (Float)

### Time

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Seconds` (Float)

### DeltaTime

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Dt` (Float)

### MakeVec3

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `X` (Float) default `0`
- in 1: `Y` (Float) default `0`
- in 2: `Z` (Float) default `0`
- out 0: `Vec` (Vec3)

### BreakVec3

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Vec` (Vec3) default `0`
- out 0: `X` (Float)
- out 1: `Y` (Float)
- out 2: `Z` (Float)

### AddVec3

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Vec3) default `0`
- in 1: `B` (Vec3) default `0`
- out 0: `Out` (Vec3)

### ScaleVec3

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Vec` (Vec3) default `0`
- in 1: `Scale` (Float) default `1`
- out 0: `Out` (Vec3)

### Distance

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Vec3) default `0`
- in 1: `B` (Vec3) default `0`
- out 0: `Out` (Float)

### Normalize

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Vec` (Vec3) default `0`
- out 0: `Out` (Vec3)

### Length

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Vec` (Vec3) default `0`
- out 0: `Out` (Float)

## Logic

### Compare

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Float) default `0`
- in 1: `B` (Float) default `0`
- out 0: `Out` (Bool)
- param `p` (string): Op: > < >= <= == !=

### And

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Bool) default `0`
- in 1: `B` (Bool) default `0`
- out 0: `Out` (Bool)

### Or

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Bool) default `0`
- in 1: `B` (Bool) default `0`
- out 0: `Out` (Bool)

### Not

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (Bool) default `0`
- out 0: `Out` (Bool)

### IsActionDown

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Down` (Bool)
- param `p` (string): Action name

### GetAxis

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Value` (Float)
- param `p` (string): Axis name (e.g. MoveX)

### IsKeyDown

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Down` (Bool)
- param `p` (string): Key (single char)

## Values

### Float

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Value` (Float)
- param `n` (number): Value

### String

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Value` (String)
- param `p` (string): Text

### Append

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `A` (String) default `0`
- in 1: `B` (String) default `0`
- out 0: `Out` (String)

## Game

### GetFlag

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Value` (Float)
- param `p` (string): Flag

### SetFlag

- exec out: `then`
- in 0: `Value` (Float) default `1`
- param `p` (string): Flag

### RequestCamera

- exec out: `then`
- in 0: `Blend` (Float) default `0`
- param `p` (string): Camera name (empty = release)

### PlaySound

- exec out: `then`
- in 0: `Volume` (Float) default `1`
- param `p` (string): Clip (.wav path)

### StartMission

- exec out: `then`
- param `p` (string): Mission id

### SaveGame

- exec out: `then`
- param `p` (string): Slot name

### LoadGame

- exec out: `then`
- param `p` (string): Slot name

### AIMoveTo

- exec out: `then`
- in 0: `Agent` (Entity) default `0`
- in 1: `Target` (Vec3) default `0`

### AIStop

- exec out: `then`
- in 0: `Agent` (Entity) default `0`

### AIArrived

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Agent` (Entity) default `0`
- out 0: `Arrived` (Bool)
- out 1: `Moving` (Bool)

### GetData

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Row` (String) default `0`
- in 1: `Field` (String) default `0`
- out 0: `Number` (Float)
- out 1: `Text` (String)
- out 2: `Found` (Bool)
- param `p` (string): Table (assets/data/*.json)

### DataRowCount

- pure (no exec pins; evaluated when an input pulls it)
- out 0: `Count` (Float)
- param `p` (string): Table (assets/data/*.json)

### DataRowName

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Index` (Float) default `0`
- out 0: `Name` (String)
- param `p` (string): Table (assets/data/*.json)

### TriggerDialogue

- exec out: `then`
- in 0: `Entity` (Entity) default `0`

## Entity

### SetAnimParam

- exec out: `then`
- in 0: `Entity` (Entity) default `0`
- in 1: `Value` (Float) default `0`
- param `p` (string): Anim parameter

### GetAnimParam

- pure (no exec pins; evaluated when an input pulls it)
- in 0: `Entity` (Entity) default `0`
- out 0: `Value` (Float)
- param `p` (string): Anim parameter

## Debug

### DrawLine

- exec out: `then`
- in 0: `From` (Vec3) default `0`
- in 1: `To` (Vec3) default `0`
- in 2: `Color` (Vec3) default `(0, 1, 0)`

### Log

- exec out: `then`
- in 0: `Message` (String) default `0`

