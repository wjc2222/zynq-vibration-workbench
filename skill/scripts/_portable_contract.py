"""Shared strict JSON contract helpers; standard library only, no file writes."""
import argparse
import json
import math
import sys


class InputError(ValueError):
    pass


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise InputError(message)


def strict_load(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise InputError("duplicate JSON key: " + key)
            result[key] = value
        return result

    def constant(value):
        raise InputError("non-finite JSON constant: " + value)

    try:
        if path == "-":
            return json.load(sys.stdin, object_pairs_hook=pairs, parse_constant=constant)
        with open(path, encoding="utf-8-sig") as stream:
            return json.load(stream, object_pairs_hook=pairs, parse_constant=constant)
    except (OSError, UnicodeError):
        raise InputError("cannot read input as UTF-8 JSON") from None
    except json.JSONDecodeError as error:
        raise InputError("invalid JSON at line %d column %d" % (error.lineno, error.colno)) from None


def fields(value, required, optional=(), where="input"):
    if type(value) is not dict:
        raise InputError(where + " must be an object")
    missing = set(required) - value.keys()
    unknown = value.keys() - set(required) - set(optional)
    if missing:
        raise InputError(where + " missing keys: " + ", ".join(sorted(missing)))
    if unknown:
        raise InputError(where + " unknown keys: " + ", ".join(sorted(unknown)))


def number(value, name, zero=False, integer=False):
    if (type(value) not in (int, float) or (integer and type(value) is not int)
            or not math.isfinite(value) or value < 0 or (not zero and value == 0)):
        raise InputError(name + " must be a finite " + ("nonnegative " if zero else "positive ")
                         + ("integer" if integer else "number") + "; booleans are invalid")
    return value


def string(value, name):
    if type(value) is not str or not value.strip() or any(ord(c) < 32 for c in value):
        raise InputError(name + " must be a nonempty string without control characters")
    return value


def emit(value):
    print(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False))


def cli(main):
    try:
        code, result = main()
        emit(result)
        return code
    except (InputError, OverflowError):
        error = sys.exc_info()[1]
        emit({"status": "invalid", "errors": [str(error) if isinstance(error, InputError)
                                               else "numeric input exceeds calculation range"]})
        return 2
