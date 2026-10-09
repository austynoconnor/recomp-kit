"""Spin-wait loops hand the baton over; ordinary loops do not.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_spin.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
sys.path.insert(0, HERE)
import translate as T  # noqa: E402
from test_translate_insns import NoImage, Opts  # noqa: E402

FN = 0x00401100


def translate(listing):
    tr = T.Translator(NoImage(), {FN}, Opts())
    fn = T.Function(FN, "f", 0x40, T.parse_listing_text(listing))
    fn.measure(NoImage())
    tr.prepare(fn)
    return "\n".join(tr.translate(fn, ()))


def test_a_loop_polling_one_byte_yields_on_its_back_edge():
    # Crazy Taxi 0040861a: waits for another thread to set [ECX + 0x20].
    text = translate("00401100  MOV DL,byte ptr [ECX + 0x20]\n"
                     "00401103  TEST DL,DL\n"
                     "00401105  JZ 0x00401100\n"
                     "00401107  RET\n")
    assert "recomp_spin_wait(c); goto L_00401100;" in text


def test_a_list_walk_moves_its_address_and_does_not_yield():
    text = translate("00401100  MOV ESI,EAX\n"
                     "00401102  MOV EAX,dword ptr [ESI]\n"
                     "00401104  CMP EAX,ECX\n"
                     "00401106  JNZ 0x00401100\n"
                     "00401108  RET\n")
    assert "recomp_spin_wait" not in text


def test_a_loop_that_stores_is_work_not_a_wait():
    text = translate("00401100  MOV byte ptr [ECX],0x1\n"
                     "00401103  CMP byte ptr [ECX + 0x1],0x0\n"
                     "00401107  JZ 0x00401100\n"
                     "00401109  RET\n")
    assert "recomp_spin_wait" not in text


def test_pause_is_a_checkpoint():
    text = translate("00401100  PAUSE\n"
                     "00401102  RET\n")
    assert "recomp_spin_wait(c);" in text
