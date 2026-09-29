import pytest


def test_ok():
    pass


def test_bad():
    assert 1 == 2


@pytest.mark.skip(reason="fixture")
def test_skipped():
    pass


@pytest.mark.xfail(reason="fixture")
def test_xfail():
    assert 0


@pytest.mark.xfail(reason="fixture", strict=False)
def test_xpass():
    pass
