# Writing Principles

## Purpose

Use this reference when writing or revising requirements, design, acceptance, failure behavior, implementation boundaries, or deferred choices.

A spec guides development of a concrete change. It is not a textbook, code tour, architecture essay, literary work, formal proof, or production documentation written in advance. Include background only when its absence could lead to a different design or result; cite existing sources for the rest.

## Write the Whole Before Polishing Parts

Judge the document first by whether an implementer can understand and execute it as a whole. A sentence can be true and still be harmful when it repeats another rule, interrupts the decision order, adds a distinction no one must act on, or narrows the design without reason.

Present information in the order needed for work:

1. the goal and acceptance;
2. how one run moves from entry or input to result;
3. the main responsibilities and work order, with reasons;
4. contracts that different parts or stages must share;
5. detail for the current work;
6. verification and any important open choice.

Use this as a reasoning order, not a mandatory set of headings. A small spec may cover it in a few paragraphs.

State what the design is before saying what it is not. Add a prohibition only when a plausible wrong assumption would change implementation or verification. Remove rationale, caveats, transitions, symbols, labels, summaries, and formatting that preserve no decision.

## State Contracts, Leave Code Choices Open

State a point explicitly when different readings could change any of these:

- observable behavior or acceptance;
- a public interface or what one part may assume about another;
- responsibility, identity, ownership, or lifetime;
- data meaning, failure behavior, or a condition that must remain true across components or steps;
- a development dependency needed to complete or verify the work.

Do not omit one of these merely because a familiar convention suggests a likely answer. Conversely, differences limited to private helpers, local control flow, internal data structures, ordinary library use, naming, or routine repo mechanics are implementation choices. They do not become spec requirements merely because two capable implementers might choose differently.

State an algorithm only when its semantics or constraints are part of the required result. Do not write a copyable private implementation body.

Do not copy explanatory terms or section structure into production types, APIs, identifiers, comments, or caller documentation unless the term is established domain language or exact wording is itself a public requirement. Derive production wording and structure from the requirements, repo conventions, and caller needs.

## Define Shared Rules Once

Give each shared rule or concept one defining section. Other sections point to it and describe only their local responsibility. A self-contained implementation brief may restate what the current step must do and point back to the defining section, but it must not redefine the common rule or make it appear specific to one component.

This is about one definition, not one mention. Do not make a brief unusable by replacing every local obligation with a link.

## Use Plain Language

Prefer the shortest ordinary term that accurately expresses the idea. Introduce a special term only when it names a distinction that recurs and changes behavior, design, or verification. Do not create a taxonomy simply to organize prose, borrow impressive terminology without a local need, or use metaphor and polished phrasing in place of a decision.

Write direct present-tense requirements. Add precision where the implementer needs it: behavior, interfaces, data meaning, integration, input guarantees, ownership and lifetime, failure, acceptance, verification, and edit boundaries. Precision does not require legalistic hedging, mathematical notation, repeated definitions, exhaustive negative lists, or a format for every possibility.

Use code-shaped text only for an existing declaration or an exact public requirement, and show only the necessary fragment. Otherwise describe responsibilities, flow, state changes, data shape, formulas, or short algorithm intent.

## Cover Only Useful Design Detail

For a behavior or component, consider the following and include only what applies:

- its role in the whole flow;
- what it owns, receives, and produces;
- behavior and state changes;
- assumptions shared with other parts;
- visible failures and where work stops;
- current input or operating limits;
- acceptance;
- what remains free for implementation.

Separately described responsibilities do not require separate production types or files. Separate code when responsibility, ownership, lifetime, dependency, integration, or independent use justifies the cost. Size and caller count are evidence, not automatic rules.

### Inputs and failure

For a multi-step flow, state guarantees already provided upstream only when doing so prevents duplicated or contradictory checking. Describe caller-visible failure when repo convention does not settle it. Require recovery, aggregation, fallback, or partial success only when the task needs it.

State a condition that must remain true when losing it could break correctness across reasonable implementations. Leave incidental private conditions to implementation. Assertions protect states the design says are impossible; invalid input follows the stated failure behavior.

### Current scale and later optimization

State current scale or operating limits when they affect the design. An unquantified future scale estimate supports simple high-value choices that avoid obvious waste or hard-to-change limits. It does not establish a current need for a large processing architecture. Specific optimization normally follows a working system and measurements in separate work.

### Acceptance and examples

State the general behavior and input domain before examples. Examples, tables, and illustrative fixtures are representative unless explicitly declared exhaustive. A fixture declared as acceptance, golden, or normative by the spec or repo carries that authority; the word `fixture` alone settles neither direction. Verification still covers relevant normal, boundary, and failure behavior implied by the rule.

Treat frozen fixtures as unchanged during implementation. If one contradicts an accepted requirement, use the process in `progress-and-decisions.md`; do not silently code to it or rewrite it.

### Deferred choices

Defer only an unsettled choice that affects the result and has a safe current default. Record the trigger, current default, who decides, and when it must be decided, using the smallest useful form. Without a safe current default, block the affected work. Private names, local cleanup, wording issues, and review chatter are not deferred design choices.

## Final Check

Read the draft in order and ask:

- Can the reader understand the goal and whole flow before local details?
- Does the work order follow real needs without pretending every task forms a perfect chain?
- Does every section change implementation, verification, review, or continuation?
- Are shared contracts clear and private code choices still free?
- Is each shared rule defined once?
- Are existing knowledge and code cited rather than retaught?
- Can any special term, symbol, format, caveat, or negative sentence be removed without losing a decision?
