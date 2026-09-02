"""Regresión offline del contrato de ACK USB/WebSocket de seis bytes."""

from __future__ import annotations

import unittest

import ws_cmd_test


class AckContractTests(unittest.TestCase):
    def test_ack_value_is_single_b1_byte_and_b0_is_reserved(self) -> None:
        # SET_RECLEN=600 se refleja como low8(600)=88. El paquete no contiene
        # el uint16 completo: [56, FF, ACK, AE, 58, 00].
        packet = bytes((0x56, 0xFF, 0x07, 0xAE, 0x58, 0x00))
        description = ws_cmd_test.describe(packet)
        self.assertIn("ack cmd=0xAE val=88", description)
        self.assertNotIn("val=22528", description)


if __name__ == "__main__":
    unittest.main()

