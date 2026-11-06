# AMBER is a standalone Android build of github.com/google/amber (SPIRV-BIN needs no compiler).
# Select the test device explicitly, e.g. ADB='adb -s emulator-5554'.
B ?= /tmp/nx-surface-atomic
REMOTE ?= /data/local/tmp/nx-surface-atomic
.PHONY: shaders apple android
shaders:
	python3 dm/android/tests/surface_atomic_fixture.py "$(B)"
apple: shaders
	clang -fobjc-arc -O1 -Wall -Werror dm/gfx/tests/t_atomic_return.m -framework Foundation -framework Metal -o "$(B)/atomic_metal"
	python3 dm/android/tests/surface_atomic_fixture.py --metal-run "$(B)"
android: shaders
	@test -n "$(ADB)" && test -x "$(AMBER)"
	$(ADB) shell mkdir -p "$(REMOTE)"
	$(ADB) push "$(AMBER)" "$(REMOTE)/amber"
	$(ADB) push "$(B)/export" "$(REMOTE)/"
	$(ADB) shell 'cd $(REMOTE)/export && ../amber -d *.amber'
