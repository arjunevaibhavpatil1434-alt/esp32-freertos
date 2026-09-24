# SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize('target', ['esp32'], indirect=['target'])
def test_bt_spp_acceptor(dut: Dut) -> None:
    dut.expect_exact("SPP server started")
    dut.expect_exact("Discoverable as 'ESP32_SPP_ACCEPTOR'")
