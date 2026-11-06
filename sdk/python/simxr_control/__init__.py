"""Shared OpenXR Simulator local control API."""
from .client import Client, ControlError, ControlTimeout, Lease, Session, ReceivedHandles, discover
from .discovery import endpoint
from .errors import TransactionError
from .protocol import Capability, CommandKind, ErrorCode, ProtocolError, TargetKind

__all__ = ['Client', 'ControlError', 'ControlTimeout', 'TransactionError', 'Lease', 'Session', 'ReceivedHandles',
           'discover', 'endpoint',
           'Capability', 'CommandKind', 'ErrorCode', 'ProtocolError', 'TargetKind']

from .commands import SourceKind, LatePolicy, ControlChannel, Scenario, encode_command, decode_command
__all__ += ['SourceKind', 'LatePolicy', 'ControlChannel', 'Scenario', 'encode_command', 'decode_command']

from .recording import collect_recording, to_luau
__all__ += ['collect_recording', 'to_luau']

from .frame_recording import record_frames
__all__ += ['record_frames']
