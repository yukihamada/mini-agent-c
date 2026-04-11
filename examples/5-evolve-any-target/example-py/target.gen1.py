"""
target.py - function under evolution.
The test suite in eval.sh has 10 test cases. Score = # of passing tests.
Goal: make all 10 pass.
"""

def word_count(text):
    """
    Return the number of words in text.
    A word is any sequence of non-whitespace characters.
    Returns 0 for empty or None input.
    """
    # Intentionally incomplete — evolution should fix the edge cases.
    if text is None or text == "":
        return 0
    return len(text.split())
