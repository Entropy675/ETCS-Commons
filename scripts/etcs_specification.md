# The .etcs Scripting Language

A `.etcs` file is a **sequential causal trace** — a record of named actions on named entities,
executed in order, fully deterministic and fully replayable. It is not a general-purpose
programming language. There are no branches, no loops, no mutable variables. What it has instead
is more precise: a structured way to declare which entities exist, what roles they occupy, and
what actions they perform on each other.

---

## Core Concepts

### The Execution Context

Every line in a `.etcs` script executes against a three-level address:

```
Module :: Tag . Action
```

- **Module** — the loaded provider library (e.g. `DatabaseProvider`)
- **Tag** — the entity type within that module (e.g. `LocalDatabase`)
- **Action** — the operation to dispatch (e.g. `Connect`, `QueryProduce`)

The interpreter maintains a current context. Once a module and tag are set, subsequent lines
can refer to actions by name alone, without repeating the full address.

---

## Setting Context

### Implicit (bare word)

```etcs
DatabaseProvider
LocalDatabase primary
```

The first uppercase word sets the module. The next uppercase word within that module sets the
tag. An optional name token (`primary`) declares the role this entity will occupy.

### Explicit (`context` keyword)

```etcs
context DatabaseProvider::LocalDatabase primary
context DatabaseProvider::LocalDatabase secondary
```

The `context` keyword switches the active entity by role name. If `primary` is already bound
to a RID, the context switches to that entity. If it is not yet bound, the name is held as a
pending role and satisfied when the next entity is created or selected.

---

## Role Names

Role names are the closest thing `.etcs` has to variables. They are not variables in the
conventional sense — they do not hold values that change through assignment. Instead they are
**named slots** that map a human-readable role to a specific entity instance (identified
internally by a RID).

```etcs
LocalDatabase primary     # declares the role "primary"
Connect primary.db        # actions go to whatever entity occupies "primary"
```

Role bindings can be **pre-populated by the caller** before a script runs (environment
injection). This means the same script can target entirely different entity instances without
modification — the script encodes the structure of the interaction, not which specific instances
participate.

### Spawn semantics

If a role name has not been bound and an action requires an entity, one is created
automatically and bound to the pending name:

```
[./long_database_test.etcs:8] No entity selected for 'DatabaseProvider::LocalDatabase' -- spawning one automatically.
```

An explicit `spawn` always creates a new entity and overwrites any prior binding for that name.
Selecting an existing entity by name never overwrites. The distinction is intentional and
enforced.

---

## Actions

Once a module, tag, and entity are in context, bare action names dispatch to that entity:

```etcs
Connect primary.db
ExecuteRaw PRAGMA journal_mode=WAL;
InitializeSchema CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT, role TEXT);
ExecuteTransaction INSERT INTO users (name, role) VALUES('Luke', 'admin'), ('Sibte', 'owner');
```

Actions fall into two categories determined by the module at load time.

### Stream 0 — Immediate actions

Request/response. The action executes and returns a result synchronously.

```etcs
Connect primary.db          # [Result] primary.db
ExecuteRaw PRAGMA ...;      # [Result] PRAGMA ...;
```

### Stream 1 — Produce/Consume pairs

Some actions open a data stream. A `QueryProduce` action suspends until the next line
provides a `RowConsume` action. The runtime holds the pending stream state between the two
declarations.

```etcs
QueryProduce SELECT * FROM users    # stream opens, waits
RowConsume                          # stream fires, rows arrive
```

The producer and consumer can be on **different entities**. A context switch between the two
lines is valid and correct — this is the mechanism for cross-entity data transfer:

```etcs
context DatabaseProvider::LocalDatabase primary
QueryProduce SELECT * FROM users        # primary produces

context DatabaseProvider::LocalDatabase secondary
RowConsume                              # secondary consumes
```

The runtime detects the mismatch, resolves both entity handles, and routes the pipe between
them. No explicit plumbing is required in the script.

If a script ends with an unmatched `QueryProduce` and no `RowConsume`, the runtime warns:

```
script ended with unmatched stream produce: 'QueryProduce' -- no consumer line followed.
```

---

## Comments

Lines beginning with `#` are ignored.

```etcs
# This is a comment
```

---

## Fully Qualified Actions

Actions can be written in fully qualified form without setting context first:

```etcs
DatabaseProvider::LocalDatabase.Connect primary.db
```

This is equivalent to switching context to `DatabaseProvider::LocalDatabase` and calling
`Connect`, but does not mutate the ambient context permanently.

---

## ABI Integrity

Every time a module is loaded, the runtime compares a hash of every participating header
between the host and the loaded library. A script that ran against version N of a module will
refuse to run against version N+1 if any participating type changed. This happens automatically,
without any action required in the script.

```
CORE:EventStream.h    03542ba5    03542ba5    [ OK ]
CORE:MirrorBuffer.h   3192e2a8    3192e2a8    [ OK ]
DatabaseProvider.h    N/A         7d4a4e93    [INFO]
```

`[INFO]` on the module's own header is expected — the host does not include the module's
internal header, only the ontology headers that form the shared ABI contract.

---

## Shutdown

Entities should be disconnected in reverse dependency order:

```etcs
context DatabaseProvider::LocalDatabase aggregate
Disconnect

context DatabaseProvider::LocalDatabase secondary
Disconnect

context DatabaseProvider::LocalDatabase primary
Disconnect
```

---

## Summary

| Concept | Mechanism |
|---|---|
| Module selection | Bare uppercase word or `context Module` |
| Tag selection | Bare uppercase word within module, or `context Module::Tag` |
| Entity selection | Role name or RID via `context Module::Tag name` |
| Role binding | Implicit on spawn, explicit via `spawn`, injectable from outside |
| Immediate action | Bare action name with optional payload |
| Stream pair | `QueryProduce` followed by `RowConsume`, optionally across entities |
| Comments | `#` prefix |
| ABI check | Automatic at module load time |

A `.etcs` script is most accurately read as a **protocol transcript**: a complete, ordered
description of which entities participated in a causal interaction, in what roles, and what
they did to each other. Its replayability is a structural property, not a feature bolted on
afterward.

