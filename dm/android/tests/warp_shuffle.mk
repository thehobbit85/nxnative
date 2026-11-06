FIXTURE ?= dm/android/tests/warp_shuffle_fixture.py
FIXTURE_NAME = $(basename $(notdir $(FIXTURE)))
B ?= /tmp/nx-$(FIXTURE_NAME)
REMOTE ?= /data/local/tmp/nx-$(FIXTURE_NAME)
METAL_KEYS ?= $(B)/export/cases.list
METAL_RUNNER ?= dm/android/tests/warp_shuffle_metal.m
.PHONY: shaders apple android
shaders:
	python3 "$(FIXTURE)" "$(B)"
apple: shaders
	@while read -r name; do stem="$(B)/$${name%.amber}"; xcrun -sdk macosx metal -c -fpreserve-invariance -fmodules-cache-path="$(B)/modules" "$$stem.metal" -o "$$stem.air" && xcrun -sdk macosx metallib "$$stem.air" -o "$$stem.metallib" || exit; done < "$(METAL_KEYS)"
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc "$(METAL_RUNNER)" -framework Foundation -framework Metal -o "$(B)/check_metal"
	"$(B)/check_metal" "$(B)"
android: shaders
	@test -n "$(ADB)" && test -x "$(AMBER)"
	$(ADB) shell mkdir -p "$(REMOTE)"
	$(ADB) push "$(AMBER)" "$(REMOTE)/amber"
	$(ADB) push "$(B)/export" "$(REMOTE)/"
	$(ADB) shell 'cd $(REMOTE)/export && test -s cases.list && xargs ../amber -d < cases.list'
