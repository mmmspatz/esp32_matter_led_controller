# chip-patches

Patches applied to `modules/connectedhomeip` by `bootstrap.sh` after every
`west update` (which resets the module tree, discarding uncommitted edits).

Each patch should be small, carry a comment header explaining why it exists,
and reference an upstream issue/PR where one has been filed. The goal is for
this directory to trend toward empty.
