"""What follows a call that never returns, and what still reaches it.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_noreturn.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
sys.path.insert(0, HERE)
import translate as T  # noqa: E402
from test_translate_insns import NoImage, Opts  # noqa: E402

HALT, FN = 0x00401000, 0x00401100


def test_returning_thunk_chain_keeps_call_continuations():
    listings = [
        "00401000  JMP 0x00401200\n",
        "00401200  JMP 0x00401300\n",
        "00401300  RET\n",
        "00401100  CALL 0x00401000\n",
    ]
    functions = []
    for listing in listings:
        insns = T.parse_listing_text(listing)
        fn = T.Function(insns[0].addr, "synthetic", 0, insns)
        fn.measure(NoImage())
        functions.append(fn)
    inferred = T.noreturn_callees_from(functions, {})
    assert HALT not in inferred
    tr = T.Translator(NoImage(), {fn.addr for fn in functions}, Opts())
    tr.noreturn_callees = inferred
    assert not tr.never_returns(functions[-1].insns[0])


def test_exit_forwarding_thunk_stays_nonreturning():
    listings = [
        "00401000  JMP 0x00401200\n",
        "00401200  CALL dword ptr [0x405000]\n00401206  RET\n",
        "00401100  CALL 0x00401000\n",
    ]
    functions = []
    for listing in listings:
        insns = T.parse_listing_text(listing)
        fn = T.Function(insns[0].addr, "synthetic", 0, insns)
        fn.measure(NoImage())
        functions.append(fn)
    assert HALT in T.noreturn_callees_from(functions, {0x405000: "ExitProcess"})


def test_imported_exit_has_no_fallthrough_dispatch():
    image = NoImage()
    image.iat_names = {0x405000: "ExitProcess", 0x405004: "GetLastError"}
    tr = T.Translator(image, {FN}, Opts())
    fn = T.Function(FN, "exit", 0, T.parse_listing_text(
        "00401100  PUSH 0x3\n00401102  CALL dword ptr [0x405000]\n"))
    fn.measure(image)
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "recomp_unknown_call(c," in text
    assert "recomp_jump" not in text
    assert not tr.never_returns(T.parse_listing_text(
        "00401102  CALL dword ptr [0x405004]\n")[0])
    assert not tr.never_returns(T.parse_listing_text("00401102  CALL EAX\n")[0])


def translate(listing, entries=()):
    tr = T.Translator(NoImage(), {FN, HALT}, Opts())
    tr.noreturn_callees = {HALT}
    fn = T.Function(FN, "f", 0x40, T.parse_listing_text(listing))
    fn.measure(NoImage())
    tr.prepare(fn)
    return "\n".join(tr.translate(fn, entries))


# A literal behind the halt, as Ghidra lists it: the 16-bit addressing is
# what the emitter refuses.
TAIL = ("00401110  CALL 0x00401000\n"
        "00401115  ADD byte ptr [EAX],AL\n"
        "00401117  ADD byte ptr [BX + DI + 0x0],CH\n")


def test_the_bytes_behind_a_halt_are_not_emitted():
    text = translate("00401100  XOR EAX,EAX\n" + TAIL)
    assert "CALL_FN(00401000);" in text
    assert "00401117" not in text


def test_a_branch_into_the_tail_keeps_it():
    """Unreachable from the halt is not unreachable: a branch still gets there."""
    listing = ("00401100  TEST EAX,EAX\n"
               "00401102  JZ 0x00401115\n") + TAIL
    try:
        text = translate(listing)
    except T.TranslateError as e:
        assert "BX" in str(e)       # the tail was emitted, and refused
    else:
        raise AssertionError("the branch target was dropped:\n" + text)


def test_a_computed_jump_keeps_every_instruction():
    """JMP EAX not preceded by POP EAX can land anywhere in the body."""
    listing = ("00401100  MOV EAX,dword ptr [ESP + 0x4]\n"
               "00401104  JMP EAX\n") + TAIL
    try:
        translate(listing)
    except T.TranslateError as e:
        assert "BX" in str(e)
    else:
        raise AssertionError("an instruction a computed jump may reach was dropped")


def test_a_popped_jump_goes_only_to_pushed_continuations():
    """POP EAX; JMP EAX lands where a continuation was pushed, not everywhere."""
    listing = ("00401100  PUSH 0x40110e\n"
               "00401105  POP EAX\n"
               "00401106  JMP EAX\n"
               "0040110e  JMP 0x00401110\n") + TAIL
    text = translate(listing)
    assert "L_0040110e:" in text
    assert "00401117" not in text
