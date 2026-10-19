"""Readable log lines shared by the live_trading Python tools.

Same shape as the C++ services, always UTC:
    2026-10-10 18:20:44.028 [INFO ] main : Kraken read-only inspection complete
Never log secrets.
"""

import logging
import os
import sys
import time

FORMAT = '%(asctime)s.%(msecs)03d [%(levelname)-5s] %(funcName)s : %(message)s'
DATE_FORMAT = '%Y-%m-%d %H:%M:%S'


def setup_logging(debug: bool | None = None) -> None:
    """Configure the root logger once for a command-line entrypoint."""
    if debug is None:
        setting = os.environ.get('ALGOTRADING_LOG_DEBUG', os.environ.get('LOG_DEBUG', '0'))
        debug = setting.strip().lower() in ('1', 'true', 'yes', 'on')
    formatter = logging.Formatter(FORMAT, DATE_FORMAT)
    formatter.converter = time.gmtime
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(formatter)
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(logging.DEBUG if debug else logging.INFO)
    # Match the C++ label width for WARNING.
    logging.addLevelName(logging.WARNING, 'WARN')
