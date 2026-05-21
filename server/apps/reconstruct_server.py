"""
DEPRECATED: Gunakan `python -m apps.reconstruct`.

File ini dipertahankan hanya untuk backward compatibility selama transisi.
Akan dihapus setelah semua workflow diupdate.
"""

import warnings
warnings.warn(
    "reconstruct_server.py deprecated. Gunakan: python -m apps.reconstruct",
    DeprecationWarning,
    stacklevel=2,
)

from apps.reconstruct.__main__ import main  # noqa: F401, E402

if __name__ == "__main__":
    main()
