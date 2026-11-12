import doctest

import tradematch as tm


def test_the_docstring_examples_run():
    assert doctest.testmod(tm).failed == 0


def test_the_public_names_exist():
    assert all(hasattr(tm, name) for name in tm.__all__)
    assert tm.__version__ == "1.0.0"
