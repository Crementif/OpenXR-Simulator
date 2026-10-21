"""Public structured remote errors and unfinished-exchange timeouts."""
from .protocol import ErrorCode


class ControlError(RuntimeError):
    def __init__(self, record):
        self.record = record
        self.code = ErrorCode(record['error'])
        super().__init__(record.get('detail', self.code.name))


class ControlTimeout(TimeoutError):
    """The exchange is unfinished; it may already have admitted an operation."""

    def __init__(self, request_id):
        self.request_id = request_id
        super().__init__(f'Control request {request_id} is unfinished; retry() or close() explicitly')


