# Style guide

How a page in `docs/` is written. The [README](README.md) is a reader's map,
so the rules for writers live here.

## Two layers

* **Physical** (`machine/`): what you see on the machine and what you should
  expect it to do. Stable across rewrites of the code underneath, though not
  frozen.
* **Logical** (`system/`): what happens underneath: controller states, bus
  traffic, who owns which value, the bridges between host, controller and
  nodes. Changes as the code does.

Physical pages link down to logical ones; logical pages need not link up.
`reference/` holds the exact strings both kinds of page use.

## All pages

* **Notation:** strings are written as the README's
  [notation](README.md#notation) says. A page links it rather than keeping
  its own copy.
* No marks inside tables: the column header says what a cell is.
* No superscripts or bracketed marks; plain Markdown renders neither well.
* Write for the merged state, in the present tense. No branch or date header.
* No test status on a page that describes behaviour. What has been tried on
  the machine goes in a plan's Outcome.
* Codes, replies and command forms are checked against the source before they
  are written down.
* **Write for a reader new to the subject,** not as a terse, densely packed
  summary for one who already knows it.
* **Write a page for the pages that exist.** A page doesn't take on material
  because the page planned to hold it isn't written yet; it links or leaves it
  out.

## Physical pages

* **Subject:** an activity: something you start, watch and end (homing,
  positioning, tools, running a job). Parts (gantry, head, holder, bed) are
  the words every page uses, not pages. Things that happen to any activity
  (e-stop, pause, faults, limits) are rows in each page's refusals or
  recovery, not pages of their own.
* **Nodes, controller and host are named by their role**, as you can tell them
  from what they are wired to: "the axis's node", "the controller". Never by
  config instance: no "head 0", "node 7".
* **Machine words:** the gantry, a head, a holder, a switch, a magnet.
* **Known gaps are behaviour:** what the machine does not protect you from,
  stated as plainly as what it does.
* **Comparisons go in tables** with one column per kind (linear and rotary
  side by side), not in parallel prose.
* **Failures as a table:** what you see, the code, the cause, and which kinds
  of axis it applies to.
* **Recovery says where it doesn't help.**
* **Ends with "Underneath":** links to the `system/` and `reference/` pages.

Section order:

1. **What it is.**
2. **The concepts**, each with the commands, replies and status keys that
   belong to it, and when those commands run. These sections are the page's
   own: cycles, legs and kinds for homing; coordinates, selection, zeroing,
   moves and limits for positioning.
3. **Commands:** an index table only (command, what it does, section), one
   row per form.
4. **Failures or refusals**, as a table.
5. **Recovery and stops**, with where it doesn't help.
6. **Underneath.**

A linear-and-rotary comparison sits with the concepts, wherever the page's
kinds differ.

## Reference pages

* **Subject:** one kind of exact string (commands, status keys, packets,
  config fields), every one of them, grouped by use.
* **Checked against a commit.** The page names the commit whose code it was
  checked against, and the `git diff` that shows what changed since. Updating
  the page means re-checking the diff and moving the commit. No test.
* **Entries, as man pages:** an index up front, then one entry per command: a
  synopsis block with one line per form, a description, a Replies list, a
  Runs line. A command whose second word picks a form (`home`, `leg`, `goto`,
  `place`) adds a small table of its forms; each reply is listed once, with
  the forms it applies to.
* **Shared answers once:** what many entries share (reply shapes, node
  results, refusal reasons, when a kind is admitted) is a table at the top;
  an entry explains a reason again only where it means something narrower.
* **Links to the physical page** for an activity, not a copy of it.

### The host API

The pages in `reference/host-api/`, one per subpath, follow the reference
rules with MDN's anatomy:

* A class has a description, then Constructor, Properties (a table) and
  Methods (a table linking each method).
* A method or function with something to say has its own entry under bold
  labels: **Syntax**, **Parameters**, **Return value**, **Exceptions**,
  **Example**. A one-liner stays a row in its class's Methods table.
* Constants, enums, name helpers and encoders are tables.
* Three heading levels per page, all of them used: `#` the page intro and
  each class or function, `##` its Constructor, Properties or Methods, `###`
  a member's entry. Deeper levels render too small to tell apart, and a page
  title at `#` over everything would waste one. The intro's title must not
  share a class's anchor (`Transports`, not `Transport`).

## Logical pages

* **Subject:** one layer: what it owns, who writes each value, and what
  crosses its edges, up and down.
* **Checked against a commit**, as reference pages are, with a `git diff`
  over the layer's files.
* **Code named by file**, never by line.
* **Decisions:** each one with the alternatives it rejected.
* **Decisions read as a chain:** each follows from the one before, in threads
  with no headings; a new thread starts where the chain breaks. The order
  carries the reasoning, with no "so" linking them.
* **Reads in order:** no section leans on a term a later one introduces. A
  short overview names the parts before the sections that detail them.
* **One subject per section;** a section that holds several splits.
* **Code names by notation:** a value or function is named in words with its
  code name at first use in each section, by its code name after. Who writes
  what lists every name, so it doubles as the glossary.
* **Plain headings:** I/O, not Edges. Each I/O boundary is named by its ends:
  "Host to Core 0", not "Up, to the host".

Common sections, in no fixed order; reads-in-order sets it per page:

* **Intro**, under the title: what the layer is, in a few sentences.
* **Hardware:** what the layer runs on and touches.
* **I/O:** what crosses each boundary, and over which channel.
* **Lifecycle:** boot, the overview of the parts, failures and how the layer
  recovers, teardown. Failures sit with the faults that report them, not in a
  section of their own.
* **The layer's own sections**, each a part the overview named.
* **Glossary**, when the page has more terms than it can introduce inline.
* **Who writes what:** a table of value, writer, readers.
* **Decisions.**
* **Code:** the files.

A page uses the sections that apply to its layer. Some layers read best
opening with Hardware, I/O and Lifecycle; others follow a path through the
layer first, after which I/O and Lifecycle can be read.

## Plans

* **A plan stays in `plans/`** whatever its state; moving it breaks its
  links. `archive/` holds docs from eras the project has left, not plans.
* **Open or closed:** a plan is open while any branch is neither merged nor
  dropped, closed once every branch is. Its first lines say which.
* **Replaced by a page:** a plan whose decisions a `system/` page now
  describes opens with a line naming the page: "Current behaviour:
  `system/<page>.md`; this plan records how it was built."

How plans are used while working is in [AGENTS.md](../AGENTS.md), Plan Docs.
