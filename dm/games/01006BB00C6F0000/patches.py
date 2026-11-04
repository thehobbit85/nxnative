# The Legend of Zelda: Link's Awakening (01006BB00C6F0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS, CODE_STUBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import B, NOP


def actor_priority(ctx):
    # The camera/system actor type 0xffff is a sentinel, not an entry in the
    # 88-byte actor metadata table. The original constructor reads beyond main
    # into whichever mapping follows it. Mac probes observed priority 3 there;
    # Android's separate mappings fault. Preserve that working priority only for
    # this sentinel, without changing valid actor metadata. This repairs guest
    # undefined behavior; the original Switch read has not been measured.
    ctx.site(0x1c17c, '0829099b08254139')
    start = ctx.here()
    for word in (0x52003d0b,  # eor w11,w8,#0xffff (x11 is dead through the epilogue)
                 0x3400008b,  # cbz w11,default (+16)
                 0x9b092908,  # madd x8,x8,x9,x10 (original index * 88 + table)
                 0x39412508): # ldrb w8,[x8,#0x49] (original priority)
        ctx.emit(word)
    ctx.emit(B(ctx.here(), 0x1c184))
    ctx.emit(0x52800068)      # default: mov w8,#3
    ctx.emit(B(ctx.here(), 0x1c184))
    ctx.put(0x1c17c, [B(0x1c17c, start), NOP], 'actor priority: no-type sentinel 0xffff uses working Mac priority 3 instead of reading beyond the actor table')


CODE_STUBS = {'909e904af78ac1b8deefe97ab2ccdb51': actor_priority}

CUTS = {
  # The Legend of Zelda: Link's Awakening 1.1.0 (v131072) main: the physics step (main+0x15730, main thread) rebuilds each world's PhysX completion task in place
  # (a PxLightCpuTask at world+0xc8) and passes it to simulate. A worker runs it (main+0x10ccf80: fetchResults, world+0x1d8 done = 1, notify, unlock) and then calls its
  # release (main+0x10cd070), which calls the task's own destructor: vtable back to the PxLightCpuTask base, mTm = 0 (main+0x10cced0). At 30 fps the game steps twice
  # per frame (step, wait, step, wait); the unlock hands the world's mutex to the woken main thread, and when the worker is descheduled there the main thread's
  # next step rebuilds the task before the late destructor runs, which then wipes the new task. The next simulation's NpSceneCompletion::release reads its mTm = 0:
  # crash at main+0x10ccf28 (lr main+0x13c236c; seen on the iPhone and in 2 of 5 Mac runs, in every Mac run under CPU load). The workers can run on other cores:
  # unlock does not protect their later destructor writes. Actual-instruction replay confirms the guest lifetime race. The release becomes a plain ret: the destructor only resets the vtable and mTm (no resources), the step rebuilds every field, and this
  # release is used by this one vtable (main+0x1c51308 slot 7) only
  '909e904af78ac1b8deefe97ab2ccdb51': [
    (0x10cd070, '080040f9', 'c0035fd6', 'physics completion task release (calls its own destructor): ldr x8,[x0] → ret (no longer destroyed; the step function rebuilds it in place every step)'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'Actor priority for the no-type sentinel', 'now': 'build', 'goal': 'game', 'purpose': 'runtime',
     'why': 'The game indexes its actor metadata with 0xffff for camera/system actors; the fallback preserves observed Mac behavior, with exact original bytes checked.',
     'blocked': 'Other builds need the constructor, sentinel and priority consumer checked before carrying the guard.',
     'checked': 'Native ARM64 replay: original sentinel faults, guarded sentinel returns 3, 40 valid priorities unchanged; Android opening movie reached.'},
    {'what': 'PhysX completion task release', 'now': 'build', 'goal': 'game', 'purpose': 'race',
     'why': "a lifetime race in the game's own completion task (its release slot serves this one vtable)"},
]
