"""Move register-only conditional shuffles to a common workgroup rendezvous."""
from collections import Counter
import struct
import subprocess

from dm.android.tools.spv_module import decode, opcode

# Only register operations can be speculated in a previously inactive lane.
# In particular a ballot or vote observes the active mask, and a memory load
# can fault or see a different value. Neither may move with a shuffle.
SHUFFLE = 345
REGISTER_OPS = {81, 83, 124, 128, 130, 132, 164, 165, 166, 167, 168, 169,
                *range(170, 180), *range(194, 201), SHUFFLE}


def lift(data, optimizer):
    # The guest helper passes its source register and lane in function-local
    # memory. Inline it and promote those temporaries before inspecting arms.
    flags = ['--preserve-bindings', '--inline-entry-points-exhaustive', '--eliminate-local-single-block',
             '--ssa-rewrite', '--eliminate-dead-code-aggressive', '--simplify-instructions',
             '--eliminate-dead-code-aggressive', '--cfg-cleanup']
    run = subprocess.run([optimizer, *flags, '-', '-o', '-'], input=data, capture_output=True)
    if run.returncode:
        return None
    header, code = decode(run.stdout)
    labels = {}
    incoming = Counter()
    selections = []
    for pos, inst in enumerate(code):
        op = opcode(inst)
        if op == 248:
            labels[inst[1]] = pos
        elif op == 249:
            incoming[inst[1]] += 1
        elif op == 250:
            incoming.update(inst[2:4])
            if opcode(code[pos - 1]) == 247:
                selections.append((pos - 1, code[pos - 1][1], inst[2:4]))
        elif op == 251:  # uncounted switch edges could enter a candidate arm
            return None

    moved = set()
    insertions = {}
    for header_pos, merge, arms in selections:
        for arm in arms:
            if arm == merge or incoming[arm] != 1:
                continue
            start = labels[arm] + 1
            end = start
            while opcode(code[end]) in REGISTER_OPS:
                end += 1
            if opcode(code[end]) != 249 or code[end][1] != merge:
                continue
            if not any(opcode(inst) == SHUFFLE for inst in code[start:end]):
                continue
            # Every active donor supplies the same register before the branch.
            # An inactive donor was already undefined. The original branch and
            # its Phi still decide whether a recipient uses the shuffled value.
            insertions.setdefault(header_pos, []).extend(code[start:end])
            moved.update(range(start, end))
    if not moved:
        return None
    words = list(header)
    for pos, inst in enumerate(code):
        for lifted in insertions.get(pos, ()):
            words.extend(lifted)
        if pos not in moved:
            words.extend(inst)
    return struct.pack(f'<{len(words)}I', *words)
