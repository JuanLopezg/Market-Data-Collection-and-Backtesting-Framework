"""Readable log lines shared by the live_trading Python tools.

Same shape as the C++ services, always UTC:
    2026-10-10 18:20:44.028 [INFO ] main : Kraken read-only inspection complete
Never log secrets.
"""

import logging
import sys
import time

FORMAT = '%(asctime)s.%(msecs)03d [%(levelname)-5s] %(funcName)s : %(message)s'
DATE_FORMAT = '%Y-%m-%d %H:%M:%S'


def setup_logging(debug: bool = False) -> None:
    """Configure the root logger once for a command-line entrypoint."""
    formatter = logging.Formatter(FORMAT, DATE_FORMAT)
    formatter.converter = time.gmtime
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(formatter)
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(logging.DEBUG if debug else logging.INFO)
    # Match the C++ label width for WARNING.
    logging.addLevelName(logging.WARNING, 'WARN')
