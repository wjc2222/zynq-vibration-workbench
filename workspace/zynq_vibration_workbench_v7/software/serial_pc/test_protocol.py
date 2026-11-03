import base64
import hashlib
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from protocol import PacketDecoder, ProtocolError, RecordDownload, request_bytes


def packet(value, seq=1):
    raw=json.dumps(value,separators=(",",":")).encode()
    return json.dumps(dict(schema="vibration.serial.v1",type="packet",seq=seq,codec="qcompress-base64",
        payload=base64.b64encode(struct.pack(">I",len(raw))+zlib.compress(raw)).decode(),
        sha256=hashlib.sha256(raw).hexdigest()),separators=(",",":")).encode()+b"\n"


class ProtocolTests(unittest.TestCase):
    def test_fragmentation_and_multiple_packets(self):
        decoder=PacketDecoder(); stream=packet({"event":"a"})+packet({"event":"b"},2)
        result=[]
        for offset in range(0,len(stream),7):result+=decoder.feed(stream[offset:offset+7])
        self.assertEqual(result,[{"event":"a"},{"event":"b"}])

    def test_corrupt_hash(self):
        value=json.loads(packet({"event":"a"}));value["sha256"]="0"*64
        with self.assertRaises(ProtocolError):PacketDecoder().decode(json.dumps(value).encode())

    def test_sequence_replay(self):
        decoder=PacketDecoder();decoder.feed(packet({"event":"a"}))
        with self.assertRaises(ProtocolError):decoder.feed(packet({"event":"b"}))

    def test_decompression_size_guard(self):
        value=json.loads(packet({"event":"a"}));raw=struct.pack(">I",2097153)+zlib.compress(b"a")
        value["payload"]=base64.b64encode(raw).decode()
        with self.assertRaises(ProtocolError):PacketDecoder().decode(json.dumps(value).encode())

    def test_duplicate_keys(self):
        raw=packet({"event":"a"}).replace(b'"seq":1',b'"seq":1,"seq":2')
        with self.assertRaises(ProtocolError):PacketDecoder().feed(raw)

    def test_partial_packet_memory_bound(self):
        with self.assertRaises(ProtocolError):PacketDecoder().feed(b"x"*131073)

    def test_command_length_and_id(self):
        with self.assertRaises(ProtocolError):request_bytes("bad\nID","hello")
        with self.assertRaises(ProtocolError):request_bytes("a","hello",extra="x"*8192)

    def make_download(self,root):
        raw={"bin":b"CAPDDR2\0"+bytes(range(256))*30,"meta":b'{"synthetic":true}'}
        manifest=dict(record_id="abc/CAP1",files=[dict(kind=k,name="CAP1.BIN" if k=="bin" else "CAP1.meta.json",bytes=len(v),sha256=hashlib.sha256(v).hexdigest()) for k,v in raw.items()])
        return RecordDownload(root,manifest),raw,manifest

    def push(self,download,raw):
        while request:=download.next_chunk():
            data=raw[request["kind"]][request["offset"]:request["offset"]+request["length"]]
            download.accept(dict(request,total=len(raw[request["kind"]]),data=base64.b64encode(data).decode(),crc32=zlib.crc32(data)))

    def test_pair_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            download,raw,_=self.make_download(Path(tmp));self.push(download,raw);path=download.finish()
            self.assertEqual(path.read_bytes(),raw["bin"]);self.assertEqual(path.with_suffix(".meta.json").read_bytes(),raw["meta"])
            self.assertTrue((path.parent/"transfer-receipt.json").is_file());self.assertFalse(download.staging.exists())

    def test_partial_abort_preserves_existing_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);original=root/"CAP1.BIN";original.write_bytes(b"keep")
            download,_,_=self.make_download(root)
            with self.assertRaises(ProtocolError):download.finish()
            download.abort();self.assertEqual(original.read_bytes(),b"keep");self.assertEqual(list(root.iterdir()),[original])

    def test_bad_chunk_no_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            download,raw,_=self.make_download(Path(tmp));request=download.next_chunk();data=raw[request["kind"]]
            with self.assertRaises(ProtocolError):download.accept(dict(request,total=len(data),data=base64.b64encode(data).decode(),crc32=0))
            download.abort();self.assertFalse(download.final_dir.exists())

    def test_missing_or_traversal_metadata_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            download,_,manifest=self.make_download(Path(tmp));download.abort()
            manifest["files"][1]["name"]="../CAP1.meta.json"
            with self.assertRaises(ProtocolError):RecordDownload(Path(tmp),manifest)
            manifest["files"]=manifest["files"][:1]
            with self.assertRaises(ProtocolError):RecordDownload(Path(tmp),manifest)

    def test_source_metadata_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            download,raw,_=self.make_download(Path(tmp));raw["meta"]=b'{"synthetic":null}'
            item=download.entries["meta"];item["bytes"]=len(raw["meta"]);item["sha256"]=hashlib.sha256(raw["meta"]).hexdigest()
            self.push(download,raw)
            with self.assertRaises(ProtocolError):download.finish()
            download.abort();self.assertFalse(download.final_dir.exists())


if __name__=="__main__":unittest.main(verbosity=2)
