"""Combined foreground, audio, held-input and process-death check for app_smoke.py."""
import subprocess
import time


def run(adb, package, start_guest, shell, log, wait_for, output, cycles):
    # Only the dedicated synthetic package on the selected AOSP test emulator.
    assert 'uid=0' in shell('su 0 id')
    menu = package + '/com.nxnative.MenuActivity'
    private = '/data/user/0/' + package + '/files/save/integration.bin'
    generations = []
    for generation in range(3):
        if generation:
            shell('am force-stop ' + package)
            wait_for(lambda: not shell('pidof ' + package + ':game || true').strip(), 'previous game process exit')
            shell('am start -W -n ' + menu)
            wait_for(lambda: 'MenuActivity' in shell('dumpsys window | grep mCurrentFocus'), 'restart menu')
        start_guest()
        wait_for(lambda: 'APP_FIXTURE_AUDIO_PROGRESS' in log(), 'guest buffers returned by AAudio')
        assert 'channels 2 real output' in log()
        pid = int(shell('pidof ' + package + ':game').strip())
        saved = shell('su 0 od -An -tu4 ' + private).split()
        assert len(saved) == 2 and int(saved[0]) == 0x4e585350
        if generations:
            assert int(saved[1]) == generations[-1]['save'] + 1 and 'APP_FIXTURE_SAVE_RETAINED' in log()
        samples = []
        for cycle in range(cycles):
            wait_for(lambda: 'GameActivity' in shell('dumpsys window | grep mCurrentFocus') and
                     log().rfind('oe focus state → 1') > log().rfind('oe focus state → 2'), 'game input focus')
            print(f'process {generation+1}/3, cycle {cycle+1}/{cycles}', flush=True)
            before = log()
            # Release is delivered while the launcher has focus. The app must
            # clear its held guest button even when it never receives that key-up.
            with subprocess.Popen([*adb, 'shell', 'input', 'keyevent', '--duration', '1800',
                                   'KEYCODE_BUTTON_A'], stdout=subprocess.DEVNULL) as key:
                wait_for(lambda: 'APP_FIXTURE_BUTTON_DOWN' in log()[len(before):], 'guest received held button')
                shell('input keyevent KEYCODE_HOME')
                wait_for(lambda: 'front end: paused' in log()[len(before):], 'game paused')
                assert key.poll() is None, 'key released before the game was backgrounded'
                key.wait(timeout=8)
                assert key.returncode == 0
            paused = log()
            time.sleep(.3)
            assert 'APP_FIXTURE_FRAME' not in log()[len(paused):], 'guest presented while paused'
            shell('am start -W -n ' + menu)
            def recovered():
                segment = log()[len(paused):]
                return all(s in segment for s in ('front end: resumed','APP_FIXTURE_AUDIO_PROGRESS','APP_FIXTURE_FRAME'))
            wait_for(recovered, 'resume, audio callback and presented frame')
            current = log()
            segment = current[len(before):]
            assert 'APP_FIXTURE_BUTTON_UP' in segment and 'swapchain ' in segment
            assert current.count('APP_FIXTURE_MAIN') == 1 and 'APP_FIXTURE_FAILED' not in current
            assert int(shell('pidof ' + package + ':game')) == pid
            counts = shell(f"su 0 sh -c 'ls /proc/{pid}/fd | wc -l; ls /proc/{pid}/task | wc -l'").split()
            assert len(counts) == 2
            samples.append(tuple(map(int, counts)))
        # Allow transient framework descriptors and helper threads to vary;
        # a leaked surface/stream on every resume exceeds either bound.
        warm = samples[1:]
        assert max(f for f,t in warm) - min(f for f,t in warm) <= 4, samples
        assert max(t for f,t in warm) - min(t for f,t in warm) <= 3, samples
        (output / f'process-{generation}.log').write_text(log())
        generations.append(dict(pid=pid, save=int(saved[1]), fd_threads=samples))
    return dict(cycles_per_process=cycles, processes=generations)
