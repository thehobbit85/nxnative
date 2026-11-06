FIXTURE ?= dm/android/tests/storage_surface_fixture.py
B ?= /tmp/nx-storage-surface
REMOTE ?= /data/local/tmp/nx-storage-surface
ANDROID_CHECK ?= amber-android
.PHONY: shaders apple android amber-android
shaders:
	python3 "$(FIXTURE)" "$(B)"
apple: shaders
	@for s in "$(B)"/*.metal; do xcrun -sdk macosx metal -c -fpreserve-invariance -fmodules-cache-path="$(B)/modules" "$$s" -o "$${s%.metal}.air" && xcrun -sdk macosx metallib "$${s%.metal}.air" -o "$${s%.metal}.metallib" || exit; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/storage_surface_metal.m -framework Foundation -framework Metal -o "$(B)/storage_surface_metal"
	"$(B)/storage_surface_metal" "$(B)" > "$(B)/metal.bin"
android: $(ANDROID_CHECK)
amber-android: shaders
	@test -n "$(ADB)" && test -x "$(AMBER)"
	$(ADB) shell mkdir -p "$(REMOTE)"
	$(ADB) push "$(AMBER)" "$(REMOTE)/amber"
	$(ADB) push "$(B)/export" "$(REMOTE)/"
	$(ADB) shell 'cd $(REMOTE)/export && ../amber -d *.amber'
