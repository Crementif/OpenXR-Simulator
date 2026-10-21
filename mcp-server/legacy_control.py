"""Compatibility tool names backed by acknowledged simulator transactions.

Only absent discovery selects the file adapter. A selected command endpoint
owns its source until explicit close; uncertain exchanges never fall back or
silently repeat a potentially admitted edit.
"""
import importlib
import threading


TOOLS = frozenset({'set_head_pose', 'set_controller_pose', 'hold_controller_inputs',
                   'set_ipd', 'set_fov'})


class SessionBinding:
    """Own the chosen endpoint and reject redirection across session lifetimes."""
    def __init__(self, directory):
        self._directory = directory
        self._sdk = None
        self._client = None
        self._session = None
        self._failure = None
        self._closed = False

    def close(self):
        self._closed = True
        client, self._client = self._client, None
        if client is not None:
            client.close()

    def invalidate(self, error):
        self._failure = str(error)
        self.close()

    def connect(self):
        if self._failure is not None:
            raise RuntimeError('Compatibility connection closed after an unfinished exchange: ' + self._failure)
        if self._closed:
            raise RuntimeError('Compatibility connection closed')
        if self._client is not None:
            return True
        try:
            sdk = importlib.import_module('simxr_control')
        except ModuleNotFoundError as error:
            if error.name == 'simxr_control':
                return False
            raise
        paths = sdk.discover(self._directory)
        if not paths:
            return False
        if len(paths) != 1:
            raise ValueError('Multiple runtimes found; set SIMXR_CONTROL_SOCKET to select one explicitly')
        client = sdk.Client(paths[0], required=sdk.Capability(0), name='MCP compatibility controls')
        try:
            if not client.hello['capabilities'] & sdk.Capability.SIMULATION_COMMANDS:
                raise ValueError('The selected runtime does not support simulation commands')
            sessions = client.sessions()
            if len(sessions) != 1:
                raise ValueError('Compatibility controls require exactly one live XR session')
        except BaseException:
            client.close()
            raise
        self._sdk = sdk
        self._session = sessions[0]
        self._client = client
        return True

    def observe(self):
        sessions = self._client.sessions()
        if sessions != [self._session]:
            # A previously accepted command belongs to the original generation.
            # Retiring this source is safer than redirecting the next edit or its
            # acknowledgement to a newly started session.
            self.close()
            raise RuntimeError('Compatibility session changed; create a new control binding explicitly')
        status = self._client.simulation_status(self._session)
        document = status.get('document')
        if not isinstance(document, dict):
            raise self._sdk.ProtocolError('Simulation status omitted its state document')
        return document

    @property
    def client(self):
        return self._client

    @property
    def session(self):
        return self._session

    def is_uncertain(self, error):
        if isinstance(error, OSError):
            return True
        return self._sdk is not None and isinstance(error, (self._sdk.ControlTimeout, self._sdk.ProtocolError))

    def error_code(self, error):
        if self._sdk is not None and isinstance(error, self._sdk.ControlError):
            return error.code.name.lower()
        return None


class TransactionAdmission:
    """Separate one accepted receipt from proof of application at a frame."""
    _PENDING = ('queued', 'rescheduled')
    _TERMINAL = ('applied', 'cancelled', 'late', 'conflict', 'invalid', 'capacity', 'closed', 'not_found')

    def __init__(self):
        self._pending = None

    def settle(self, binding):
        if self._pending is None:
            return None
        outcome = binding.client.simulation_status(binding.session, self._pending)
        phase = self._phase(outcome)
        if self._identity(outcome) != self._pending:
            from simxr_control import ProtocolError
            raise ProtocolError('Transaction acknowledgement does not match its accepted receipt')
        if phase in self._PENDING:
            return self.result(binding.session, outcome, ok=False,
                error='Previous compatibility transaction is still pending; advance the application frame before another update')
        self._pending = None
        if phase != 'applied':
            return self.result(binding.session, outcome, ok=False,
                error='Previous compatibility transaction was not applied; retry this update explicitly')
        return None

    def accepted(self, session, outcome):
        phase = self._phase(outcome)
        if phase in self._PENDING:
            transaction = self._identity(outcome)
            if type(transaction) is not int or not 0 < transaction < 2**64:
                from simxr_control import ProtocolError
                raise ProtocolError('Accepted simulation transaction omitted its identity')
            self._pending = transaction
        return self.result(session, outcome, ok=phase in (*self._PENDING, 'applied'))

    @staticmethod
    def _identity(outcome):
        return outcome.get('transaction_id', outcome.get('document', {}).get('transaction_id'))

    @classmethod
    def _phase(cls, outcome):
        phase = outcome.get('phase')
        if phase not in cls._PENDING + cls._TERMINAL:
            from simxr_control import ProtocolError
            raise ProtocolError(f'Unknown simulation transaction phase: {phase!r}')
        return phase

    @staticmethod
    def result(session, outcome, **extra):
        document = outcome.get('document', {})
        transaction = TransactionAdmission._identity(outcome)
        result = {'transport': 'simulation_commands', 'session': str(session.id),
                  'generation': str(session.generation), 'late_policy': 'reschedule_next',
                  'transaction_id': str(transaction), 'phase': outcome['phase'],
                  'applied': outcome['phase'] == 'applied', **extra}
        for key in ('target_frame', 'effective_frame', 'applied_frame'):
            if document.get(key) is not None:
                result[key] = str(document[key])
        return result


class LegacyControl:
    def __init__(self, directory):
        self.directory = directory
        self._lock = threading.Lock()
        self._binding = SessionBinding(directory)
        self._admission = TransactionAdmission()

    def close(self):
        with self._lock:
            self._binding.close()

    def call(self, name, arguments):
        """None selects the older file adapter only when no command endpoint exists."""
        if name not in TOOLS:
            return None
        with self._lock:
            try:
                if not self._binding.connect():
                    return None
                from simxr_control.compatibility import HeadPoseEdit, parse_edit
                edit = parse_edit(name, arguments)
                document = self._binding.observe()
                blocked = self._admission.settle(self._binding)
                if blocked is not None:
                    return blocked
                configuration = document.get('configuration')
                if configuration is None and not isinstance(edit, HeadPoseEdit):
                    raise ValueError('No applied configuration is available; allow the first application frame before this update')
                commands = edit.commands(configuration)
                frame = document.get('frame_id', 0)
                if type(frame) is not int or not 0 <= frame < 2**64 - 1:
                    from simxr_control import ProtocolError
                    raise ProtocolError('Simulation frame identity is missing, invalid, or exhausted')
                outcome = self._binding.client.schedule(self._binding.session, commands, frame=frame + 1, late_policy=1)
                return self._admission.accepted(self._binding.session, outcome)
            except Exception as error:
                if self._binding.is_uncertain(error):
                    self._binding.invalidate(error)
                result = {'ok': False, 'transport': 'simulation_commands', 'error': str(error)}
                code = self._binding.error_code(error)
                if code is not None:
                    result['code'] = code
                return result
