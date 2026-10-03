"""Check every managed class, method and field used by the native mod against a dump.

The dump is a dump.cs-style listing of the original Granny 1.8.12 IL2CPP metadata
(class/struct headers, fields with offsets and methods with parameter types), for example
produced by Il2CppDumper or a LibCpp2IL script. Unity's managed code stripping removes
unused overloads, so a method missing here would make the mod silently lose a feature.
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / 'native/main.cpp', ROOT / 'native/unity_ui.inc']
OPTIONAL = {('UnityEngine.UI.Shadow', 'set_effectDistance')}


def load_dump(path):
    classes, current = {}, None
    for line in path.read_text(errors='replace').splitlines():
        header = re.match(r'^(?:class|struct) (\S+)(?: : (\S+))? \{', line)
        if header:
            current = {'base': header.group(2), 'methods': [], 'fields': set()}
            classes[header.group(1)] = current
            continue
        if current is None or line.startswith('}'):
            continue
        method = re.match(r'^  (?:static )?.+? (\S+)\((.*)\) // RVA', line)
        if method:
            params = [p.strip().rsplit(' ', 1)[0] for p in method.group(2).split(', ')] if method.group(2) else []
            current['methods'].append((method.group(1), params))
            continue
        field = re.match(r'^  (?:static )?.+ (\S+); // 0x', line)
        if field:
            current['fields'].add(field.group(1))
    return classes


def has_method(classes, name, method, params):
    while name in classes:
        # IL2CPP reports by-ref parameters with a trailing '&'; dumps print the plain type.
        if any(m == method and p == [x.rstrip('&') for x in params] for m, p in classes[name]['methods']):
            return True
        name = classes[name]['base']
    return False


def has_field(classes, name, field):
    while name in classes:
        if field in classes[name]['fields']:
            return True
        name = classes[name]['base']
    return False


def check(dump):
    classes = load_dump(dump)
    text = '\n'.join(p.read_text() for p in SOURCES)
    variables = {}
    for var, ns, name in re.findall(r'CLASS\((\w+),"([\w.]*)","(\w+)"\)', text):
        variables['U.' + var] = variables[var] = (ns + '.' + name).lstrip('.')
    for var, ns, name in re.findall(r'(\w+)=R\.klass\("([\w.]*)","(\w+)"\)', text):
        variables.setdefault(var, (ns + '.' + name).lstrip('.'))
        variables.setdefault('U.' + var, (ns + '.' + name).lstrip('.'))
    errors, checked = [], 0
    calls = re.findall(r'M\(\w+,(\w+),"([^"]+)"((?:,"[^"]+")*)\)', text)
    calls += re.findall(r'R\.method\(([\w.]+),"([^"]+)",\{((?:"[^"]+",?)*)\}\)', text)
    for var, method, params in calls:
        cls = variables.get(var)
        if not cls:
            errors.append(f'unknown class variable {var} for {method}')
            continue
        signature = re.findall(r'"([^"]+)"', params)
        checked += 1
        if not has_method(classes, cls, method, signature) and (cls, method) not in OPTIONAL:
            errors.append(f'missing method {cls}::{method}({", ".join(signature)})')
    for obj_class, field in re.findall(r'R\.(?:field<[^>]+>|set)\(\w+,([\w.]+),"([^"]+)"', text):
        cls = variables.get(obj_class)
        checked += 1
        if not cls or not has_field(classes, cls, field):
            errors.append(f'missing field {cls or obj_class}.{field}')
    for match in re.findall(r'R\.(?:field<[^>]+>|set)\(\w+,U\.fps,(\w+)\?"(\w+)":"(\w+)"', text):
        for field in match[1:]:
            checked += 1
            if not has_field(classes, 'FPSControllerNEW', field):
                errors.append(f'missing field FPSControllerNEW.{field}')
    for field in ['forwardSpeed', 'backwardSpeed', 'sidestepSpeed', 'timeInAir', 'joystick', 'cameraPivot', 'character', 'granny', 'playerCaught', 'playerCrouch']:
        checked += 1
        if not has_field(classes, 'FPSControllerNEW', field):
            errors.append(f'missing field FPSControllerNEW.{field}')
    for cls in set(variables.values()):
        if cls not in classes:
            errors.append(f'missing class {cls}')
    if errors:
        raise SystemExit('IL2CPP API check failed:\n  ' + '\n  '.join(errors))
    print(f'IL2CPP API check passed: {len(set(variables.values()))} classes, {checked} methods/fields present in the build')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--dump', type=Path, required=True)
    check(parser.parse_args().dump)
