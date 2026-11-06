B ?= /tmp/nx-warp-shuffle
REMOTE ?= /data/local/tmp/nx-warp-shuffle
.PHONY: shaders apple android
shaders:
	python3 dm/android/tests/warp_shuffle_fixture.py "$(B)"
apple: shaders
	@for s in "$(B)"/*.metal; do xcrun -sdk macosx metal -c -fpreserve-invariance -fmodules-cache-path="$(B)/modules" "$$s" -o "$${s%.metal}.air" && xcrun -sdk macosx metallib "$${s%.metal}.air" -o "$${s%.metal}.metallib" || exit; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/warp_shuffle_metal.m -framework Foundation -framework Metal -o "$(B)/warp_shuffle_metal"
	"$(B)/warp_shuffle_metal" "$(B)"
android: shaders
	@test -n "$(ADB)" && test -x "$(AMBER)"
	$(ADB) shell mkdir -p "$(REMOTE)"
	$(ADB) push "$(AMBER)" "$(REMOTE)/amber"
	$(ADB) push "$(B)/export" "$(REMOTE)/"
	$(ADB) shell 'cd $(REMOTE)/export && ../amber -d *.amber'
