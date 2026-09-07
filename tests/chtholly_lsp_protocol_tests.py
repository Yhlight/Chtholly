#!/usr/bin/env python3
import argparse
import atexit
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time


def frame(message: dict) -> bytes:
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode(
        "utf-8"
    )
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def read_frame(stream) -> dict:
    length = None
    while True:
        line = stream.readline()
        if not line:
            raise EOFError("language server closed stdout")
        if line in (b"\r\n", b"\n"):
            break
        name, separator, value = line.partition(b":")
        if separator and name.lower() == b"content-length":
            length = int(value.strip())
    if length is None:
        raise AssertionError("language server response omitted Content-Length")
    body = stream.read(length)
    if len(body) != length:
        raise EOFError("language server truncated a response")
    return json.loads(body.decode("utf-8"))


class Client:
    def __init__(self, executable: str):
        self.process = subprocess.Popen(
            [executable],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.messages = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self._cleanup = self.abort
        atexit.register(self._cleanup)

    def _read(self):
        try:
            while True:
                self.messages.put(read_frame(self.process.stdout))
        except EOFError:
            return
        except BaseException as error:
            self.messages.put(error)

    def send(self, message: dict, fragmented: bool = False):
        payload = frame(message)
        if fragmented:
            self.process.stdin.write(payload[:7])
            self.process.stdin.flush()
            self.process.stdin.write(payload[7:])
        else:
            self.process.stdin.write(payload)
        self.process.stdin.flush()

    def response(self, request_id, timeout: float = 20.0) -> dict:
        deadline = time.monotonic() + timeout
        deferred = []
        while time.monotonic() < deadline:
            try:
                message = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if isinstance(message, BaseException):
                raise message
            if message.get("id") == request_id:
                for item in deferred:
                    self.messages.put(item)
                return message
            deferred.append(message)
        raise AssertionError(f"timed out waiting for response {request_id!r}")

    def notification(self, method: str, predicate=None,
                     timeout: float = 20.0) -> dict:
        deadline = time.monotonic() + timeout
        deferred = []
        while time.monotonic() < deadline:
            try:
                message = self.messages.get(
                    timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if isinstance(message, BaseException):
                raise message
            if message.get("method") == method and (
                    predicate is None or predicate(message)):
                for item in deferred:
                    self.messages.put(item)
                return message
            deferred.append(message)
        for item in deferred:
            self.messages.put(item)
        raise AssertionError(f"timed out waiting for notification {method!r}")

    def close(self):
        if self.process.stdin:
            self.process.stdin.close()
        return_code = self.process.wait(timeout=20)
        stderr = self.process.stderr.read().decode("utf-8", errors="strict")
        if return_code != 0:
            raise AssertionError(
                f"language server exited with {return_code}: {stderr!r}"
            )
        if stderr:
            raise AssertionError(f"language server wrote stderr: {stderr!r}")
        atexit.unregister(self._cleanup)

    def abort(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=20)

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lsp", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-next-lsp-") as directory:
        root = pathlib.Path(directory)
        source = root / "src" / "main.cns"
        source.parent.mkdir()
        (root / "chtholly.toml").write_text(
            '[package]\nname = "lsp-test"\nlanguage = "1.0"\n\n[build]\n'
            'entry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8",
        )
        contents = (
            "module main;\n"
            "// 😀\n"
            "fn helper(value: i32): i32 { return value; }\n"
            "fn main(): i32 { return helper(1); }\n"
        )
        source.write_text(contents, encoding="utf-8")
        uri = source.resolve().as_uri()
        utility_source = root / "src" / "utility.cns"
        utility_contents = (
            "module utility;\n"
            "pub fn shared(value: i32): i32 { return value; }\n"
        )
        utility_source.write_text(utility_contents, encoding="utf-8")
        utility_uri = utility_source.resolve().as_uri()
        cfdl_source = root / "src" / "binding.cfdl"
        cfdl_contents = (
            "module binding;\n"
            "foreign type Session: void* invalid null;\n"
            "foreign fn os_open(url: view void*) -> owned Session\n"
            "where result obliges close;\n"
            "foreign fn os_close(session: ref_mut Session) -> void\n"
            "where session discharges close;\n"
        )
        cfdl_source.write_text(cfdl_contents, encoding="utf-8")
        cfdl_uri = cfdl_source.resolve().as_uri()

        client = Client(args.lsp)
        client.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"rootUri": root.resolve().as_uri()},
            },
            fragmented=True,
        )
        initialized = client.response(1)
        capabilities = initialized["result"]["capabilities"]
        assert capabilities["positionEncoding"] == "utf-16"
        assert capabilities["textDocumentSync"]["change"] == 2
        assert capabilities["completionProvider"]["triggerCharacters"] == [".", ":"]
        assert capabilities["documentSymbolProvider"] is True
        assert capabilities["renameProvider"]["prepareProvider"] is True
        assert capabilities["codeActionProvider"]["codeActionKinds"] == [
            "quickfix"
        ]
        client.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "chtholly",
                        "version": 1,
                        "text": contents,
                    }
                },
            }
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "id": "hover-1",
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                },
            }
        )
        hover = client.response("hover-1")
        assert "fn helper" in hover["result"]["contents"]["value"]

        client.send(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                },
            }
        )
        definition = client.response(3)["result"]
        assert len(definition) == 1
        assert definition[0]["range"]["start"]["line"] == 2

        client.send(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                    "context": {"includeDeclaration": True},
                },
            }
        )
        assert len(client.response(4)["result"]) == 2

        client.send(
            {
                "jsonrpc": "2.0",
                "id": "symbols-1",
                "method": "textDocument/documentSymbol",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        symbols = client.response("symbols-1")["result"]
        symbol_names = [symbol["name"] for symbol in symbols]
        assert "helper" in symbol_names and "main" in symbol_names, symbols
        assert all(symbol["kind"] == 12 for symbol in symbols), symbols

        client.send(
            {
                "jsonrpc": "2.0",
                "id": "prepare-rename-1",
                "method": "textDocument/prepareRename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                },
            }
        )
        prepared = client.response("prepare-rename-1")["result"]
        assert prepared["placeholder"] == "helper", prepared

        client.send(
            {
                "jsonrpc": "2.0",
                "id": "rename-1",
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                    "newName": "renamed_helper",
                },
            }
        )
        rename_response = client.response("rename-1")
        assert "result" in rename_response, rename_response
        rename = rename_response["result"]
        edits = rename["changes"][uri]
        assert len(edits) == 2, rename
        assert all(edit["newText"] == "renamed_helper" for edit in edits)

        client.send(
            {
                "jsonrpc": "2.0",
                "id": "rename-invalid",
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                    "newName": "fn",
                },
            }
        )
        assert "error" in client.response("rename-invalid")

        missing_import_diagnostic = {
            "range": {
                "start": {"line": 3, "character": 20},
                "end": {"line": 3, "character": 21},
            },
            "severity": 1,
            "code": "chtholly.next.sem.operator.missing-import",
            "source": "chtholly-next",
            "message": "overloaded operators require an explicit import",
        }
        client.send(
            {
                "jsonrpc": "2.0",
                "id": "code-action-import",
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": missing_import_diagnostic["range"],
                    "context": {"diagnostics": [missing_import_diagnostic]},
                },
            }
        )
        actions = client.response("code-action-import")["result"]
        assert len(actions) == 1, actions
        assert actions[0]["title"] == "Import std::ops", actions
        import_edit = actions[0]["edit"]["changes"][uri][0]
        assert import_edit["range"]["start"] == {"line": 1, "character": 0}
        assert import_edit["newText"] == "import std::ops;\n"

        result_import_diagnostic = dict(
            missing_import_diagnostic,
            code="chtholly.next.sem.async.missing-result-import",
            message="async Result operations require an explicit import",
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "id": "code-action-result-import",
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": result_import_diagnostic["range"],
                    "context": {"diagnostics": [result_import_diagnostic]},
                },
            }
        )
        result_actions = client.response("code-action-result-import")["result"]
        assert len(result_actions) == 1, result_actions
        assert result_actions[0]["title"] == "Import std::result", result_actions
        assert result_actions[0]["edit"]["changes"][uri][0]["newText"] == (
            "import std::result;\n"
        )

        move_diagnostic = dict(
            missing_import_diagnostic,
            code="chtholly.next.sem.method.explicit-move-required",
            message="a by-value receiver requires an explicit move",
            range={
                "start": {"line": 3, "character": 19},
                "end": {"line": 3, "character": 22},
            },
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "id": "code-action-move",
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": move_diagnostic["range"],
                    "context": {"diagnostics": [move_diagnostic]},
                },
            }
        )
        move_actions = client.response("code-action-move")["result"]
        assert len(move_actions) == 1, move_actions
        assert move_actions[0]["title"] == "Insert explicit move", move_actions
        move_edit = move_actions[0]["edit"]["changes"][uri][0]
        assert move_edit["range"]["start"] == move_diagnostic["range"]["start"]
        assert move_edit["range"]["end"] == move_diagnostic["range"]["start"]
        assert move_edit["newText"] == "move "

        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [
                        {
                            "range": {
                                "start": {"line": 1, "character": 3},
                                "end": {"line": 1, "character": 5},
                            },
                            "rangeLength": 2,
                            "text": "note",
                        }
                    ],
                },
            }
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/hover",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 26},
                },
            }
        )
        assert "fn helper" in client.response(5)["result"]["contents"]["value"]

        completion_contents = (
            "module main;\n"
            "struct Box { value: i32; }\n"
            "impl Box { pub fn read(self: const Self&): i32 { return self.value; } "
            "pub fn zero(): i32 { return 0; } }\n"
            "fn inspect(box: const Box&): i32 { return box.re; }\n"
            "fn main(): i32 { return Box::zero(); }\n"
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": completion_contents}],
                },
            }
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "id": "completion-1",
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 3, "character": 48},
                },
            }
        )
        completion = client.response("completion-1")["result"]
        assert completion["isIncomplete"] is False
        assert [item["label"] for item in completion["items"]] == [
            "read"
        ], completion
        assert completion["items"][0]["kind"] == 2
        assert "fn Box::read" in completion["items"][0]["detail"]

        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 4},
                    "contentChanges": [
                        {
                            "text": "module main;\nimport binding;\n"
                            "fn main(): i32 { return 0; }\n"
                        }
                    ],
                },
            }
        )

        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": cfdl_uri,
                        "languageId": "cfdl",
                        "version": 1,
                        "text": cfdl_contents,
                    }
                },
            }
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "id": "completion-cfdl",
                "method": "textDocument/completion",
                "params": {
                    "textDocument": {"uri": cfdl_uri},
                    "position": {"line": 2, "character": 10},
                },
            }
        )
        cfdl_completion = client.response("completion-cfdl")["result"]
        cfdl_labels = [item["label"] for item in cfdl_completion["items"]]
        assert "fn" in cfdl_labels, cfdl_completion
        assert "operation" not in cfdl_labels, cfdl_completion

        invalid_contents = (
            "module main;\n"
            "fn main(): i32 { return missing_value; }\n"
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 5},
                    "contentChanges": [{"text": invalid_contents}],
                },
            }
        )
        invalid_diagnostics = client.notification(
            "textDocument/publishDiagnostics",
            lambda message: message["params"].get("uri") == uri
            and message["params"].get("version") == 5,
        )["params"]["diagnostics"]
        assert any(
            diagnostic["code"] == "chtholly.next.sem.unknown-name"
            and diagnostic["severity"] == 1
            and diagnostic["range"]["start"]["line"] == 1
            for diagnostic in invalid_diagnostics
        ), invalid_diagnostics

        repaired_contents = "module main;\nfn main(): i32 { return 0; }\n"
        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 6},
                    "contentChanges": [{"text": repaired_contents}],
                },
            }
        )
        repaired = client.notification(
            "textDocument/publishDiagnostics",
            lambda message: message["params"].get("uri") == uri
            and message["params"].get("version") == 6,
        )
        assert repaired["params"]["diagnostics"] == [], repaired

        cross_module_contents = (
            "module main;\n"
            "import utility;\n"
            "fn main(): i32 { return utility::shared(1); }\n"
        )
        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 7},
                    "contentChanges": [{"text": cross_module_contents}],
                },
            }
        )
        cross_diagnostics = client.notification(
            "textDocument/publishDiagnostics",
            lambda message: message["params"].get("uri") == uri
            and message["params"].get("version") == 7,
        )
        assert cross_diagnostics["params"]["diagnostics"] == [], cross_diagnostics

        client.send(
            {
                "jsonrpc": "2.0",
                "id": "rename-cross-module",
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": uri},
                    "position": {"line": 2, "character": 33},
                    "newName": "renamed_shared",
                },
            }
        )
        cross_rename_response = client.response("rename-cross-module")
        assert "result" in cross_rename_response, cross_rename_response
        cross_changes = cross_rename_response["result"]["changes"]
        assert set(cross_changes) == {uri, utility_uri}, cross_changes
        assert len(cross_changes[uri]) == 1, cross_changes
        assert len(cross_changes[utility_uri]) == 1, cross_changes

        client.send(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didClose",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        closed = client.notification(
            "textDocument/publishDiagnostics",
            lambda message: message["params"].get("uri") == uri
            and "version" not in message["params"],
        )
        assert closed["params"]["diagnostics"] == [], closed

        client.send(
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWatchedFiles",
                "params": {
                    "changes": [
                        {
                            "uri": (root / "chtholly.toml").resolve().as_uri(),
                            "type": 2,
                        }
                    ]
                },
            }
        )

        client.send(
            {"jsonrpc": "2.0", "id": 6, "method": "unknown/method", "params": {}}
        )
        assert client.response(6)["error"]["code"] == -32601
        client.send({"jsonrpc": "2.0", "id": 7, "method": "shutdown"})
        assert client.response(7)["result"] is None
        client.send({"jsonrpc": "2.0", "id": 8, "method": "shutdown"})
        assert client.response(8)["error"]["code"] == -32600
        client.send({"jsonrpc": "2.0", "method": "exit"})
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
