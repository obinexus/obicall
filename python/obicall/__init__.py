"""Python language adapter for Obicall.

This package is a real ctypes binding against libobicall's public C ABI
(see docs/ABI.md) - not a reimplementation of it. provider_worker.py is
the Python "worker" entry point the supervisor spawns when a provider
manifest declares "language": "python".
"""
