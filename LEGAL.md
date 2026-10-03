← [Back to README](README.md)

# Legal basis for this project

*Not legal advice — this is a plain-English explanation of why this project is structured the way
it is, for readers' context, not a legal opinion for your specific situation. Consult a lawyer if
you need one, especially before building on this for something broader than personal use on
hardware you own.*

## Short version

This project adds a new capability to Focus V device firmware by changing a small number of
individual instructions — five, for the Carta 2 patch — each one replaced with another of the
identical length, everything else in the firmware untouched. Nobody involved has seen Focus V's
source code. Every address, every struct layout, and every function this patches was worked out
independently by disassembling firmware Focus V itself serves, in the clear, to any device that
asks for an update (see
[focusv-ble-research's LEGAL.md](https://github.com/hashking710/focusv-ble-research/blob/master/LEGAL.md)
for the fuller reasoning behind that research). This repo never contains Focus V's firmware — only
original code written for this project, plus a script describing where a few bytes change.

## The relevant statute

**[17 U.S.C. § 1201(f)](https://www.law.cornell.edu/uscode/text/17/1201)** — the DMCA's reverse-engineering
exemption — permits circumventing a technological protection measure "for the sole purpose of
identifying and analyzing those elements of the program that are necessary to achieve
interoperability of an independently created computer program with other programs," where that
information isn't otherwise available, and permits developing and sharing the resulting
interoperability information for that purpose. As with the research repo this builds on: nothing
here required circumventing an access control in the first place (the firmware this was studied
against is served over plain HTTPS with no auth), so §1201(a)'s prohibition on circumvention
doesn't even reach this case — §1201(f) is relevant mainly as a second, independent line of
support for why this kind of interoperability work is lawful, on top of there being no access
control to circumvent to begin with.

Reverse engineering for interoperability also sits on established precedent outside §1201 (*Sega v.
Accolade*, *Sony v. Connectix*, Ninth Circuit — fair-use treatment of intermediate copying during
reverse engineering to build an independently-created, interoperable program). A firmware patch
that adds a new scheduling capability on top of a device's existing, unmodified control logic — so
your own hardware does something new, for you, the owner — is squarely the kind of thing this line
of law is about.

## Why this repo ships a patch, not a firmware build

This is the specific question that distinguishes this repo from simply "here's a modified
firmware.bin, download it": **this repo never contains a complete, ready-to-flash image that
includes any of Focus V's own code.** What it contains instead:

- **Original source** (`common/`, and each device's `device.h` and display / LED / input code) —
  written from scratch for this project. No compiled code is published: you build it yourself with
  the TC32 toolchain via `tools/build.py`, which also verifies the result against your own file.
- **A small table of patch sites** — for each one, an address, the handful of bytes expected to
  already be there, and the bytes to replace them with. Nothing here reproduces any meaningful
  amount of Focus V's own code; a same-length instruction swap at a documented address is a fact
  about *where a change happens*, not a copy of the surrounding program.
- **A script** that applies that table to a firmware file *you* supply — obtained the same way the
  official app itself obtains it, from Focus V's own update infrastructure.

This is the same model the ROM-hacking community has used for decades for exactly this reason: an
IPS or BPS patch file describes a diff — offsets and replacement bytes — and is shared freely,
specifically because it lets a community build and distribute a modification *without*
redistributing the copyrighted base work the modification applies to. The person applying the
patch always supplies their own legitimately-obtained copy of the original. This project follows
that same structure deliberately, not as a technicality but because it's the actual meaningful
distinction between "sharing a modification" and "redistributing someone else's copyrighted
software" — the latter isn't something §1201(f) or the interoperability case law above speaks to
at all, and isn't something this project does.

**If you build on this repo**: that same reasoning only holds if your downstream work keeps the
same shape. A script, repo, or tool that bundles a complete pre-patched firmware image (rather than
a patch applied to a file the end user supplies themselves) is a materially different thing,
legally, than what's here — worth thinking through on its own terms rather than assuming this
document's reasoning just carries over.

## What this patch actually touches (and why that's relevant context)

Every patch in this repo is scoped to adding a new scheduling capability on top of existing,
unmodified logic — not to rewriting how the device measures or limits temperature, not to its
safety cutoffs, and (so far, for every patch here) not to its Bluetooth radio/stack code. That's a
deliberate design choice, documented per-device, and also happens to be relevant if you're thinking
about this in terms of risk, separate from the copyright question above:

- **Warranty.** Installing third-party firmware on a device you own is a DIY, enthusiast action
  that would likely affect any manufacturer warranty — not a copyright question, a straightforward
  consumer-electronics one. Go in expecting that, not as a surprise later.
- **Equipment certification (e.g. FCC in the US).** A device's regulatory certification covers it
  as shipped, including its as-certified firmware. This project's patches don't touch
  radio/BLE-stack code — every patch here only replaces calls inside the temperature-control tick
  loop and (for the Carta 2) the display-refresh routine — but "doesn't touch the radio code" is a
  fact about what this specific patch does, not a blanket legal clearance; modifying a certified
  device's firmware at all is worth being aware of in this context even when the RF path itself is
  untouched.
- **Safety.** Every device-specific README states plainly what's verified in software versus what
  still needs real-hardware confirmation. Don't treat "the bytes check out" as equivalent to
  "confirmed safe to run unattended" until a given patch's README says real hardware testing has
  happened.

None of the above changes the copyright analysis in the sections before it — they're separate
categories of consideration (consumer protection / regulatory / safety) worth being aware of
alongside it, not instead of it.
