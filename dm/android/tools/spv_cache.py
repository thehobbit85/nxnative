"""Reuse compiled Metal bytes without reusing a Vulkan capture's translation."""
from contextlib import closing, contextmanager

from dm.tools.shader_cache import Store


def _copy_compiles(source, destination):
    # Only immutable, source/compiler-keyed results cross capture sessions.
    # In particular, neither shader/ nor index/ entries may skip the wrapper.
    with destination.batch():
        for name in source.names('metallib/'):
            if name.endswith('.metallib') and not destination.has(name):
                data = source.get(name)
                if data is not None:
                    destination.put(name, data)


@contextmanager
def capture_cache(private_path, compiled_path):
    """A fresh SQLite translation cache, with persistent compiled libraries."""
    with closing(Store(compiled_path)) as compiled, closing(Store(private_path)) as private:
        _copy_compiles(compiled, private)
        try:
            yield private_path
        finally:
            # A failed export can still leave valid content-addressed compiles.
            _copy_compiles(private, compiled)
