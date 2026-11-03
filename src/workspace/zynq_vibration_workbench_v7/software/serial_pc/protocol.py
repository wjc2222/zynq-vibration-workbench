"""Bounded serial framing and transactional CAPDDR2+metadata download.

The capture service protocol and original raw format are intentionally untouched.
This module owns only the PC transport wrapper.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import struct
import uuid
import zlib
from pathlib import Path

SCHEMA = "vibration.serial.v1"
MAX_WIRE = 131072
MAX_JSON = 2097152


class ProtocolError(ValueError):
    pass


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(data: bytes):
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                      parse_constant=lambda s: (_ for _ in ()).throw(ProtocolError(s)))


def request_bytes(request_id: str, command: str, **fields) -> bytes:
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", request_id):
        raise ProtocolError("Invalid request ID")
    value = dict(fields)
    value.update(schema=SCHEMA, id=request_id, command=command)
    data = json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode()
    if len(data) > 8192:
        raise ProtocolError("Request exceeds limit")
    return data + b"\n"


class PacketDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.sequence = 0

    def feed(self, data: bytes):
        return list(self.iter_feed(data))

    def iter_feed(self, data: bytes):
        """Yield each validated packet before looking at the following line.

        A session can stop after a validated, matched shutdown
        response, when the same read also contains the restored shell prompt.
        Other consumers retain the original strict batch API through feed().
        """
        self.buffer.extend(data)
        while b"\n" in self.buffer:
            end = self.buffer.index(10)
            if end > MAX_WIRE:
                raise ProtocolError("Serial packet exceeds limit")
            line = bytes(self.buffer[:end]).rstrip(b"\r")
            del self.buffer[:end + 1]
            if not line:
                continue
            yield self.decode(line)
        if len(self.buffer) > MAX_WIRE:
            raise ProtocolError("Unterminated serial packet exceeds limit")

    def decode(self, line: bytes):
        if len(line) > MAX_WIRE:
            raise ProtocolError("Packet too large")
        packet = parse_json(line)
        if not isinstance(packet, dict) or packet.get("schema") != SCHEMA or packet.get("type") != "packet" or packet.get("codec") != "qcompress-base64":
            raise ProtocolError("Unexpected serial protocol")
        seq = packet.get("seq")
        if type(seq) is not int or seq <= self.sequence:
            raise ProtocolError("Old or duplicate serial packet")
        payload = packet.get("payload")
        if not isinstance(payload, str):
            raise ProtocolError("Missing payload")
        packed = base64.b64decode(payload, validate=True)
        if base64.b64encode(packed).decode() != payload or len(packed) < 5:
            raise ProtocolError("Noncanonical packet payload")
        expected = struct.unpack(">I", packed[:4])[0]
        if not 1 <= expected <= MAX_JSON:
            raise ProtocolError("Uncompressed packet exceeds limit")
        inflater = zlib.decompressobj()
        raw = inflater.decompress(packed[4:], expected + 1)
        if len(raw) != expected or not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
            raise ProtocolError("Compressed packet length mismatch")
        if hashlib.sha256(raw).hexdigest() != packet.get("sha256"):
            raise ProtocolError("Serial packet SHA256 mismatch")
        decoded = parse_json(raw)
        if not isinstance(decoded, dict):
            raise ProtocolError("Packet body must be an object")
        self.sequence = seq
        return decoded


class RecordDownload:
    """Publish a new, uniquely named directory only after both files verify."""
    def __init__(self, destination: Path, manifest: dict):
        self.destination = Path(destination).resolve(strict=True)
        if not self.destination.is_dir():
            raise ProtocolError("Save directory is not a directory")
        self.record_id = manifest.get("record_id")
        self.entries = {}
        for item in manifest.get("files", []):
            kind, name, length, sha = (item.get(k) for k in ("kind", "name", "bytes", "sha256"))
            if kind not in ("bin", "meta") or kind in self.entries:
                raise ProtocolError("Record must contain exactly one BIN and metadata")
            if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,180}", name) or name.startswith("."):
                raise ProtocolError("Unsafe record filename")
            if type(length) is not int or not 0 < length <= (67108864 if kind == "bin" else 1048576):
                raise ProtocolError("Invalid record length")
            if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
                raise ProtocolError("Invalid record hash")
            self.entries[kind] = dict(item, received=0, hasher=hashlib.sha256())
        if set(self.entries) != {"bin", "meta"}:
            raise ProtocolError("Metadata and BIN must travel together")
        bin_name = self.entries["bin"]["name"]
        if not bin_name.lower().endswith(".bin") or self.entries["meta"]["name"] != bin_name[:-4] + ".meta.json":
            raise ProtocolError("BIN and metadata names do not match")
        token = uuid.uuid4().hex[:12]
        self.final_dir = self.destination / (Path(bin_name).stem + "_" + token)
        self.staging = self.destination / (".receiving_" + token)
        self.staging.mkdir()
        self.closed = False
        self.handles = {}
        try:
            for kind, item in self.entries.items():
                self.handles[kind] = (self.staging / item["name"]).open("xb")
        except Exception:
            self.abort()
            raise

    @property
    def total(self):
        return sum(e["bytes"] for e in self.entries.values())

    @property
    def received(self):
        return sum(e["received"] for e in self.entries.values())

    def next_chunk(self):
        for kind in ("meta", "bin"):
            item = self.entries[kind]
            if item["received"] < item["bytes"]:
                return dict(record_id=self.record_id, kind=kind, offset=item["received"],
                            length=min(3072, item["bytes"] - item["received"]))
        return None

    def accept(self, chunk):
        expected = self.next_chunk()
        if expected is None or self.closed:
            raise ProtocolError("Unexpected file chunk")
        for key in ("record_id", "kind", "offset"):
            if chunk.get(key) != expected[key]:
                raise ProtocolError(f"File chunk identity mismatch: {key}")
        item = self.entries[expected["kind"]]
        if chunk.get("total") != item["bytes"] or chunk.get("length") != expected["length"]:
            raise ProtocolError("File chunk length mismatch")
        encoded = chunk.get("data", "")
        raw = base64.b64decode(encoded, validate=True)
        if base64.b64encode(raw).decode() != encoded or len(raw) != expected["length"] or zlib.crc32(raw) != chunk.get("crc32"):
            raise ProtocolError("File chunk CRC mismatch")
        self.handles[expected["kind"]].write(raw)
        item["hasher"].update(raw)
        item["received"] += len(raw)

    def finish(self):
        if self.closed or self.next_chunk() is not None:
            raise ProtocolError("Record transfer is incomplete")
        for item in self.entries.values():
            if item["hasher"].hexdigest() != item["sha256"]:
                raise ProtocolError("Complete file SHA256 mismatch")
        for handle in self.handles.values():
            handle.flush()
            os.fsync(handle.fileno())
            handle.close()
        self.handles.clear()
        metadata = parse_json((self.staging / self.entries["meta"]["name"]).read_bytes())
        if not isinstance(metadata, dict) or type(metadata.get("synthetic")) is not bool:
            raise ProtocolError("Missing source declaration in metadata")
        receipt = {"schema": "vibration.serial.download.v1", "record_id": self.record_id,
                   "transfer_integrity": "passed", "files": [
                       {k: v for k, v in e.items() if k not in ("hasher", "received")} for e in self.entries.values()]}
        (self.staging / "transfer-receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding="utf-8")
        self.staging.rename(self.final_dir)
        self.closed = True
        return self.final_dir / self.entries["bin"]["name"]

    def abort(self):
        if self.closed:
            return
        for handle in self.handles.values():
            handle.close()
        self.handles.clear()
        # Only exact files this object created, never recursive deletion.
        for item in self.entries.values():
            path = self.staging / item["name"]
            if path.is_file() and not path.is_symlink():
                path.unlink()
        receipt = self.staging / "transfer-receipt.json"
        if receipt.is_file() and not receipt.is_symlink():
            receipt.unlink()
        self.staging.rmdir()
        self.closed = True
