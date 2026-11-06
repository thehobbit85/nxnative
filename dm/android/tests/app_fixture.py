#!/usr/bin/env python3
"""Build synthetic NSOs for the actual CLI/APK; no test-only host hooks are linked."""
from pathlib import Path
import argparse
import os
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from dm.tools.tests.test_nso import make_nso
from dm.host.tests.gate.fixture import module


def build(output, ndk, program=None, web=False, display=False, watch=False, controllers=False, keyboard=False, motion=False, haptics=False):
    output.mkdir(parents=True, exist_ok=True)
    tc = ndk / 'toolchains/llvm/prebuilt' / ('darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64') / 'bin'
    asm = output / 'entry.S'
    imports = {
        'nvnBootstrapLoader': 'nvnBootstrapLoader',
        'hid_read': '_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj',
        'fs_mount': '_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE',
        'fs_open': '_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci',
        'fs_create': '_ZN2nn2fs10CreateFileEPKcl',
        'fs_read': '_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm',
        'fs_write': '_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE',
        'fs_close': '_ZN2nn2fs9CloseFileENS0_10FileHandleE',
        'fs_commit': '_ZN2nn2fs6CommitEPKc',
        'aoc_count': '_ZN2nn3aoc17CountAddOnContentEv',
        'font_state': '_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE',
        'font_address': '_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE',
    }
    if program is not None:
        imports.update(oe_execute='_ZN2nn2oe14ExecuteProgramEiPKvm',
                       oe_previous='_ZN2nn2oe23GetPreviousProgramIndexEv',
                       oe_parameter='_ZN2nn2oe21TryPopLaunchParameterEPmPvm',
                       fs_rom='_ZN2nn2fs8MountRomEPKcPvm')
    if web:
        imports.update(web_start='_ZN2nn3web17OfflineWebSession5StartEPPNS_2os15SystemEventTypeERKNS0_22ShowOfflineHtmlPageArgE',
                       web_wait='_ZN2nn3web17OfflineWebSession11WaitForExitEPNS0_26OfflineHtmlPageReturnValueE',
                       web_offline='_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE',
                       web_appear='_ZN2nn3web17OfflineWebSession6AppearEv', web_exit='_ZN2nn3web17OfflineWebSession11RequestExitEv',
                       web_send='_ZN2nn3web17OfflineWebSession21TrySendContentMessageEPKcm',
                       web_recv='_ZN2nn3web17OfflineWebSession24TryReceiveContentMessageEPmPcm')
    if haptics:
        imports.update(vib_handles='_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE',
                       vib_send='_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi',
                       vib_actual='_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE',
                       vib_mounted='_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE',
                       hid_ids='_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm')
    if motion:
        imports['six_handles'] = '_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE'
        imports['six_current'] = '_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE'
    if keyboard:
        imports['keyboard_show'] = '_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE'
    if controllers:
        imports['hid_full'] = '_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj'
        imports['hid_ids'] = '_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm'
    if display:
        imports['vi_resolution'] = '_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE'
    source = '.section .start,"ax"\n.global _start\n_start: b nnMain\n.word 0\n'
    for name in imports:
        slot = name + '_import'
        source += f'.text\n.global {name}\n.hidden {name}\n{name}:\nadrp x16, {slot}\nldr x16, [x16, :lo12:{slot}]\nbr x16\n.data\n.global {slot}\n.hidden {slot}\n{slot}: .quad 0\n'
    asm.write_text(source)
    script = output / 'guest.ld'
    script.write_text('SECTIONS { . = 0; .text : { *(.start) *(.text*) } . = ALIGN(0x4000); .rodata : { . += 0x2000; *(.rodata*) } . = ALIGN(0x4000); .data : { *(.data*) BYTE(0); *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }')
    elf = output / 'guest.elf'
    subprocess.run([tc / 'aarch64-linux-android30-clang', '-O1', '-fpie', '-fvisibility=hidden', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-nostdlib', '-static', '-Wl,--build-id=none', '-Wl,-T,' + str(script),
                    *(['-DAPP_PROGRAM=' + str(program)] if program is not None else []),
                    *(['-DAPP_WEB=1'] if web else []),
                    *(['-DAPP_DISPLAY=1'] if display else []),
                    *(['-DAPP_CONTROLLERS=1'] if controllers else []),
                    *(['-DAPP_KEYBOARD=1'] if keyboard else []),
                    *(['-DAPP_HAPTICS=1'] if haptics else []),
                    *(['-DAPP_MOTION=1'] if motion else []),
                    *(['-DAPP_WATCH=' + str({'heap': 1, 'alias': 2, 'stack': 3}.get(watch, 1))] if watch else []),
                    '-I' + str(ROOT / 'dm/gfx/nvn'), '-I' + str(ROOT / 'dm/gfx/nvn/include'), ROOT / 'dm/android/tests/app_guest.c', asm, '-o', elf], check=True)
    symbols = {}
    for line in subprocess.check_output([tc / 'llvm-nm', '-P', '--defined-only', elf], text=True).splitlines():
        name, kind, value, *_ = line.split()
        symbols[name] = (kind, int(value, 16))
    blob = elf.read_bytes()
    shoff = struct.unpack_from('<Q', blob, 40)[0]
    shsize, shnum, shnames = struct.unpack_from('<HHH', blob, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', blob, shoff + i * shsize) for i in range(shnum)]
    strings = blob[sections[shnames][4]:sections[shnames][4] + sections[shnames][5]]
    selected = {}
    for sec in sections:
        name = strings[sec[0]:].split(b'\0', 1)[0].decode()
        if name in ('.text', '.rodata', '.data'):
            selected[name] = (sec[3], bytearray(blob[sec[4]:sec[4] + sec[5]]))
    _, text = selected['.text']; ro_off, ro = selected['.rodata']; data_off, data = selected['.data']
    struct.pack_into('<I', text, 4, ro_off)
    ro[:4] = b'MOD0'; struct.pack_into('<i', ro, 4, 0x100); struct.pack_into('<i', ro, 16, 0x30)
    # Export all functions to give the rewriter real function boundaries as well as SDK startup names.
    exports = {n: v for n, (k, v) in symbols.items() if k.upper() == 'T' and n not in imports and v}
    starts = sorted(set(exports.values()))
    struct.pack_into('<4BII', ro, 0x30, 1, 3, 3, 3, 0, len(starts))
    for i, pc in enumerate(starts): struct.pack_into('<II', ro, 0x3c + i * 8, pc, 0)
    entries = [('', 0), *exports.items(), *((name, 0) for name in imports.values())]
    symtab = 0x200; strtab = symtab + 24 * len(entries); names = bytearray(b'\0')
    for i, (name, pc) in enumerate(entries[1:], 1):
        struct.pack_into('<IBBHQQ', ro, symtab + i * 24, len(names), 0x12, 0, bool(pc), pc, 0)
        names += name.encode() + b'\0'
    ro[strtab:strtab + len(names)] = names
    rela = (strtab + len(names) + 7) & ~7
    for i, name in enumerate(imports):
        struct.pack_into('<QQq', ro, rela + i * 24, symbols[name + '_import'][1], (1 + len(exports) + i) << 32 | 1026, 0)
    for i, (tag, val) in enumerate([(6, ro_off + symtab), (5, ro_off + strtab), (23, ro_off + rela), (2, 24 * len(imports))]):
        struct.pack_into('<QQ', ro, 0x100 + i * 16, tag, val)
    nso = make_nso((text, ro, data), compress=(0, 0, 0))
    struct.pack_into('<I', nso, 0x24, ro_off); struct.pack_into('<I', nso, 0x34, data_off)
    source = output / 'main.nso'; source.write_bytes(nso)
    dest = output / 'patched_android'; dest.mkdir(exist_ok=True)
    subprocess.run([sys.executable, ROOT / 'dm/tools/patch_nso.py', source, dest, 'main', '--thread-slots', 'android'], check=True,
                   env=dict(os.environ, DM_REWRITE_J='1'))
    # The production Hollow Knight profile loads these names; the fixture supplies synthetic SDK modules.
    for name in ('subsdk0', 'sdk'):
        module(dest, 'android', name, (0x14000002, 0, 0xd65f03c0), {}, 'nvnBootstrapLoader', 16384)
    for sub in ('extract/synthetic/RomFS', 'shader/vk', 'font'):
        (output / sub).mkdir(parents=True, exist_ok=True)
    (output / 'extract/synthetic/RomFS/fixture.txt').write_text('Synthetic APK integration fixture\n')
    if web:
        html = output / 'extract/synthetic/html/html0/html-document/menu'; html.mkdir(parents=True, exist_ok=True)
        (html / 'resource.txt').write_text('resource')
        (html / 'index.html').write_text('''<!doctype html><html><head><title>Offline HTML fixture</title>
<style>html{background:transparent;color:white;font:40px sans-serif}button{font-size:48px}</style>
<script>nx.addEventListener('message',function(e){if(e.data==='end'){nx.sendMessage('ENDING');nx.endApplet()}else nx.sendMessage('ECHO:'+e.data)});
fetch('resource.txt').then(r=>r.text()).then(s=>nx.sendMessage('READY:'+innerWidth+':'+s));
document.addEventListener('keydown',function(e){if(e.keyCode===32)nx.sendMessage('BUTTON_A')});</script>
</head><body><h1>Offline HTML on Android</h1><button autofocus>Press A, then B</button></body></html>''')
        (html / 'callback.html').write_text('<!doctype html><script>location.href="http://localhost/finished"</script>')
        (html / 'end.html').write_text('<!doctype html><script>nx.endApplet()</script>')
        (html / 'http_error.html').write_text('<!doctype html><script>location.replace("missing.html")</script>')
    if program is not None:
        (output / 'extract/synthetic/RomFS/program.txt').write_bytes(bytes([program]))
    (output / 'aoc/1').mkdir(parents=True, exist_ok=True)
    (output / 'aoc/1/fixture.txt').write_text('Synthetic DLC marker\n')
    # File routing, not font rasterization: the HLE maps these synthetic bytes.
    for name in ('standard.ttf', 'nintendo_ext.ttf'):
        (output / 'font' / name).write_bytes(b'\x00\x01\x00\x00synthetic font')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path); p.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME'))
    p.add_argument('--program-index', type=int, choices=(0, 1, 2), help='multi-program handoff fixture')
    p.add_argument('--web', action='store_true', help='offline HTML applet fixture')
    p.add_argument('--display', action='store_true', help='display resolution service fixture')
    p.add_argument('--watch', nargs='?', const='heap', choices=('heap', 'alias', 'stack'), help='managed write-watch diagnostic fixture')
    p.add_argument('--controllers', action='store_true', help='sample two guest controllers for the Android input test')
    p.add_argument('--keyboard', action='store_true', help='native text dialog and guest encoding checks')
    p.add_argument('--motion', action='store_true', help='read the live six-axis sensor through the guest API')
    p.add_argument('--haptics', action='store_true', help='drive the guest vibration handles from test buttons')
    a = p.parse_args()
    if not a.ndk: p.error('set ANDROID_NDK_HOME or --ndk')
    build(a.output.resolve(), Path(a.ndk), a.program_index, a.web, a.display, a.watch, a.controllers, a.keyboard, a.motion, a.haptics)
