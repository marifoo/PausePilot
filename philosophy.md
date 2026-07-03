Core Philosophy:

The greatest enemy of software design is Complexity. Complexity is caused by
dependencies and obscurity. It manifests in three ways:

    Change amplification: A simple enhancement requires modifying many
    different places.

    Cognitive load: A developer must hold too much context in their head to
    understand or modify the code.

    Unknown unknowns: It is not obvious which pieces of code must be modified
    to complete a task.

When reviewing code, prefer changes that reduce future complexity, not just
changes that satisfy the immediate requirement.

Evaluation Criteria (The Ousterhout Checklist):
When reviewing the code, aggressively look for violations of the following
principles and suggest concrete refactoring steps to fix them:

1. Deep vs. Shallow Modules:

    Identify Shallow Modules: Look for classes, functions, or modules that have
    large or complex interfaces but do very little actual work. The interface
    should not be almost as complicated as the implementation.

    Enforce Deep Modules: Recommend ways to make modules deeper, where the
    interface is simple but the module hides substantial internal complexity.

2. Information Hiding:

    Detect Leakage: Check if internal design decisions, policies, or data
    structures are leaking through the interface.

    Consolidate Design Decisions: Ensure that each important design decision is
    owned by a single module. If multiple modules must know the same internal
    detail, the abstraction is leaking.

3. Define Errors Out of Existence:

    Critique Error-Prone APIs: Flag interfaces that force every caller to
    handle routine edge cases, defensive checks, or awkward invalid states.

    Prefer Safer Designs: Suggest ways to redesign the API or logic so that
    invalid states are impossible, harmless, or handled internally.

    Use Exceptions Sparingly: Exceptions are acceptable when truly exceptional,
    but avoid designs where exceptions become the normal way callers manage
    routine control flow.

4. Pull Complexity Downwards:

    Check if modules are forcing their callers to deal with complexity.

    Check whether callers must know too much to use the module correctly.

    Suggest ways for the module to absorb validation, defaults, edge cases, and
    workflow details internally so the caller does not have to.

5. Better Naming:

    Flag generic or vague names such as Manager, Data, Process, or Result.

    Suggest precise names that accurately reflect the variable, function, or
    module's exact purpose without requiring the reader to inspect the
    implementation.

6. Consistency and Obviousness:

    Check whether similar concepts are named, structured, and handled the same
    way across the codebase.

    Flag surprising behavior, hidden preconditions, unusual control flow, or
    clever implementations that require deep context to understand.

    Prefer code whose behavior is easy to infer from its name, shape, and
    surrounding conventions.

7. Eliminate Special Cases:

    Look for one-off branches, flags, sentinel values, and custom handling that
    exist only for awkward edge cases.

    Suggest ways to fold edge cases into the normal path so the common behavior
    handles more situations uniformly.

8. Layers Must Add Value:

    Flag wrapper classes, pass-through functions, or intermediate layers that
    do not provide a distinct abstraction or hide meaningful complexity.

    Ensure that each layer simplifies something for its callers rather than
    merely forwarding work elsewhere.

9. Code vs. Comments:

    Flag Redundant Comments: Identify comments that merely restate what the code
    does.

    Require Informative Comments: Ensure comments capture information that is
    not obvious from code, such as interface contracts, invariants, side
    effects, intent, and rationale.

    A developer should be able to understand how to use a module from its
    interface comments without reading the implementation.

10. Togetherness vs. Separation:

    Check for Conjoined Twins: If two separate files, classes, or methods must
    always be modified together, suggest combining them.

    Check for False Togetherness: If unrelated concerns are crammed together,
    suggest splitting them.
