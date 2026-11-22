"""测试控制台结果识别和进程身份检查。"""

import unittest
from unittest.mock import Mock, patch

from runtime_console import RuntimeConsole, result_line, validate_console_identity


class RuntimeConsoleTests(unittest.TestCase):
    def test_wrong_process_is_rejected(self):
        for component_id, pid in ((8002, 100), (8001, 101)):
            with self.subTest(component_id=component_id, pid=pid):
                with self.assertRaises(RuntimeError):
                    validate_console_identity(
                        b"AppID: 8001. UID: 10. PID: 100\r\n", component_id, pid
                    )

    def test_missing_identity_is_rejected(self):
        with self.assertRaises(RuntimeError):
            validate_console_identity(b"password:", 8001, 100)

    def test_command_echo_is_not_a_result(self):
        self.assertIsNone(result_line(b"print('\\nMARK:' + json.dumps([1]))\r\n", "MARK:"))

    def test_incomplete_result_is_not_accepted(self):
        self.assertIsNone(result_line(b"\r\nMARK:[1]", "MARK:"))

    def test_unrelated_response_is_not_accepted(self):
        self.assertIsNone(result_line(b"\r\nOLD:[1]\r\n", "MARK:"))

    def test_unicode_result(self):
        self.assertEqual('{"name": "角色"}', result_line('\r\nMARK:{"name": "角色"}\r\n'.encode(), "MARK:"))

    @patch("runtime_console.socket.create_connection")
    def test_login_uses_password_and_checks_owned_process(self, connect):
        connection = connect.return_value
        connection.recv.side_effect = [
            b"password:",
            b"AppID: 8001. UID: 10. PID: 100\r\n[cellapp@python ~] >>> ",
        ]
        console = RuntimeConsole("127.0.0.1", 50000, "test-console-pass", 8001, 100)
        connection.sendall.assert_called_once_with(b"test-console-pass\r\n")
        console.close()

    @patch("runtime_console.socket.create_connection")
    def test_wrong_process_closes_connection(self, connect):
        connection = connect.return_value
        connection.recv.return_value = b"AppID: 8002. PID: 101\r\n >>> "
        with self.assertRaises(RuntimeError):
            RuntimeConsole("127.0.0.1", 50000, "", 8001, 100)
        connection.close.assert_called_once()

    @patch("runtime_console.uuid.uuid4", return_value=Mock(hex="request"))
    @patch("runtime_console.socket.create_connection")
    def test_fragmented_result(self, connect, request_id):
        connection = connect.return_value
        connection.recv.side_effect = [
            b"AppID: 8001. PID: 100\r\n >>> ",
            b"print('\\nKBE_RUNTIME_request:' + json.dumps([1]))\r\n",
            b"\r\nKBE_RUNTIME_requ",
            b"est:[",
            b"1]\r\n",
        ]
        console = RuntimeConsole("127.0.0.1", 50000, "", 8001, 100)
        self.assertEqual([1], console.evaluate("[1]"))
        console.close()

    @patch("runtime_console.socket.create_connection")
    def test_remote_exception_fails(self, connect):
        connection = connect.return_value
        connection.recv.side_effect = [
            b"AppID: 8001. PID: 100\r\n >>> ",
            b"Traceback (most recent call last):\r\n",
        ]
        console = RuntimeConsole("127.0.0.1", 50000, "", 8001, 100)
        with self.assertRaises(RuntimeError):
            console.evaluate("missing_entity")
        console.close()

    @patch("runtime_console.socket.create_connection")
    def test_disconnect_is_not_an_empty_success(self, connect):
        connection = connect.return_value
        connection.recv.side_effect = [b"AppID: 8001. PID: 100\r\n >>> ", b""]
        console = RuntimeConsole("127.0.0.1", 50000, "", 8001, 100)
        with self.assertRaises(ConnectionError):
            console.evaluate("[]")
        console.close()


if __name__ == "__main__":
    unittest.main()
