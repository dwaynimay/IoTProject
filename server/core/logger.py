"""Setup logging stdlib terpusat untuk seluruh server."""

import logging
import sys

from .config import LOG_LEVEL

_FORMAT  = "[%(asctime)s] [%(levelname)-8s] [%(name)s] %(message)s"
_DATEFMT = "%Y-%m-%d %H:%M:%S"


def setup_logging() -> None:
    """
    Panggil sekali di entry point (apps/*/___main__.py).
    Semua modul lain cukup: logger = logging.getLogger(__name__)
    """
    logging.basicConfig(
        level   = getattr(logging, LOG_LEVEL.upper(), logging.INFO),
        format  = _FORMAT,
        datefmt = _DATEFMT,
        stream  = sys.stdout,
    )
    # Kurangi noise dari library eksternal
    logging.getLogger("paho").setLevel(logging.WARNING)
    logging.getLogger("urllib3").setLevel(logging.WARNING)


def get_logger(name: str) -> logging.Logger:
    """Shortcut: logger = get_logger(__name__)"""
    return logging.getLogger(name)
