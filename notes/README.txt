MAMEUI Netplay                                          Conventions


                   NOTES: FORMAT AND EVIDENCE


Abstract

   How documents in notes/ are written, tagged and checked. The rules
   themselves are in CLAUDE.md; this is the working reference for
   applying them. Read rules 4 through 7 of CLAUDE.md first.

   notes/ is ours. docs/ is MAME's Sphinx tree and is not ours to
   write to.

Table of Contents

   1. What lives here
   2. Format
      2.1. The skeleton
      2.2. Tables
      2.3. What is never in a .txt file
   3. Evidence tags
   4. Checks
   5. Open questions sections

1.  What lives here

   One subject per file. A note describes what something IS and why it
   is that way; it does not narrate how it was found.

   +-------------------------+------------------------------------------+
   | File                    | Holds                                    |
   +=========================+==========================================+
   | README.txt              | this file                                |
   +-------------------------+------------------------------------------+
   | upstream-edits.txt      | every upstream MAME file we changed, and |
   |                         | why. The rebase checklist.               |
   +-------------------------+------------------------------------------+
   | determinism.txt         | what perturbs emulated state, with the   |
   |                         | evidence for each claim                  |
   +-------------------------+------------------------------------------+
   | netplay-hooks.txt       | where we hook MAME and what upstream     |
   |                         | guarantees at each point                 |
   +-------------------------+------------------------------------------+
   | protocol.txt            | the wire format                          |
   +-------------------------+------------------------------------------+
   | build-config.txt        | toolchain, flags, MSVC 2017 quirks       |
   +-------------------------+------------------------------------------+
   | coinop-subtarget.txt    | how the trimmed driver set is derived    |
   +-------------------------+------------------------------------------+

   A fact has exactly one home (CLAUDE.md rule 7.1). Everything else
   points at it - "see determinism.txt 3.4" - and does not restate it.

2.  Format

2.1.  The skeleton

   Right-hand label on line 1 says what kind of document it is:
   Design Note, Analysis Note, Conventions, Protocol.

       MAMEUI Netplay                                     Design Note


                            FRAME SEQUENCING


       Abstract

          One paragraph on what this document covers. Body text is
          indented three spaces and wrapped at 72 columns.

       Table of Contents

          1. First Section
             1.1. A Subsection

       1.  First Section

          Two spaces after the section number at top level.

   Blocks indented four or more spaces are preformatted - address
   maps, listings, diagrams, code - and are never rewrapped.

2.2.  Tables

   Drawn with +---+ borders and space-aligned columns, so they line up
   in any editor with no renderer. +===+ under the header row.

       +--------+-------+----------------------------------------+
       | Field  | Bytes | Notes                                  |
       +========+=======+========================================+
       | magic  |   4   | 'MNP1'                                 |
       +--------+-------+----------------------------------------+

   Every | row must be exactly as wide as the +---+ border above it.
   Editing a cell without re-padding the row is the easiest mistake in
   this format to make and the hardest to see in a diff, which is why
   asciify.py checks it. --fix does not repair these; widen or rewrap
   the cell by hand.

   Body text wraps at 72 columns. Tables may run to 120 including
   their indent.

2.3.  What is never in a .txt file

   No markdown. No # headings, no **bold**, no backticks, no
   *emphasis*, no [text](link), no |-delimited markdown tables. If it
   needs a renderer to look right, it is wrong here.

   ASCII only, every byte <= 0x7F. Write -> not an arrow, - not an em
   dash, ... not an ellipsis, x not a multiplication sign. Diagrams
   use + - | and not box-drawing characters.

3.  Evidence tags

   Every claim carries one.

   +--------------+-------------------------------------------------+
   | Tag          | Means                                           |
   +==============+=================================================+
   | [verified]   | read out of this source tree, or observed at    |
   |              | runtime in our own build. Cite file and symbol. |
   +--------------+-------------------------------------------------+
   | [inferred]   | reasoned from verified evidence. State the      |
   |              | evidence.                                       |
   +--------------+-------------------------------------------------+
   | [upstream]   | documented by MAME or MAMEdev. True of MAME in  |
   |              | general; check it still holds at 0.210.         |
   +--------------+-------------------------------------------------+
   | [guess]      | a hypothesis worth testing. Never build on one  |
   |              | without saying so.                              |
   +--------------+-------------------------------------------------+

   Cite the function, not the line number. Line numbers drift when we
   move to another MAME tag; ioport_manager::frame_update stays
   findable.

   A number produced by a script is [verified] only if the script is
   in tools/ and can be re-run. A number that exists only in a chat
   transcript is not evidence.

4.  Checks

       python tools/asciify.py --check      ASCII, width, table alignment
       python tools/asciify.py --fix        transliterate and rewrap
       python tools/rfcfmt.py  --fix PATH   draft -> RFC form, one way

   With no PATH, asciify walks our paths only: CLAUDE.md, notes/,
   tools/, server/, src/emu/netplay/. Upstream MAME contains
   legitimate non-ASCII in language/, hash/ and driver comments; it is
   not ours to rewrite.

   Run --check before treating any document as finished. If it reports
   an UNMAPPED character, add a deliberate entry to XLAT rather than
   letting a '?' through.

   rfcfmt.py is a one-way converter, not a round-trip formatter. It
   refuses a file already in RFC form, because re-rendering its own
   output flattens the layout back into paragraphs. Author new
   documents in RFC form directly.

5.  Open questions sections

   Every document keeps one, at the end. An honest gap is more useful
   than a confident guess, and it is the only place in a note where
   the state of the work is allowed to appear.

       6.  Open questions

          o Does ram_state round-trip on CPS1 with the Q-Sound board
            present? Untested.
          o Frame pacing under a relayed connection is unmeasured.

   Progress does not go anywhere else. "Done", "next", "in progress"
   are not facts about the emulator and do not belong in a document
   (CLAUDE.md rule 7.1).
