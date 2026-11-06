FIXTURE := dm/android/tests/storage_array_atomic_fixture.py
B ?= /tmp/nx-storage-array-atomic
REMOTE ?= /data/local/tmp/nx-storage-array-atomic
ANDROID_CHECK := array-android
include dm/android/tests/storage_surface.mk
.PHONY: array-android
array-android: shaders
	@test -n "$(ADB)" && test -x "$(VK_CC)"
	"$(VK_CC)" -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/storage_array_atomic.c -lvulkan -o "$(B)/atomic_array"
	$(ADB) shell mkdir -p "$(REMOTE)"
	$(ADB) push "$(B)/atomic_array" "$(B)/export" "$(REMOTE)/"
	$(ADB) shell 'cd $(REMOTE)/export && test -s cases.list && xargs -n 2 ../atomic_array < cases.list && wc -l < cases.list'
