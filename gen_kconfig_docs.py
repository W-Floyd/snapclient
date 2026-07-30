#!/usr/bin/env python3
"""Generate a Markdown document of all sdkconfig options with help text."""

import os
import sys
import tempfile

IDF_PATH  = os.environ.get('IDF_PATH', '/opt/esp/idf')
IDF_TARGET = os.environ.get('IDF_TARGET', 'esp32s3')
PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))

# Suppress spurious "not set" warning
os.environ.setdefault('IDF_ENV_FPGA', '0')
os.environ['IDF_PATH']   = IDF_PATH
os.environ['IDF_TARGET'] = IDF_TARGET

# ---- Build the two source-list files that IDF's top Kconfig needs ----

def find_kconfigs(search_dirs, filename):
    paths = []
    for base in search_dirs:
        if not os.path.isdir(base):
            continue
        for root, dirs, files in os.walk(base):
            # Skip test / example subtrees to keep output manageable
            dirs[:] = [d for d in dirs if d not in (
                'test', 'tests', 'test_apps', 'examples', 'example',
                'applications', '__pycache__', 'dist',
            )]
            if filename in files:
                paths.append(os.path.join(root, filename))
    return paths

search_dirs = [
    os.path.join(IDF_PATH, 'components'),
    os.path.join(PROJECT_DIR, 'components'),
    os.path.join(PROJECT_DIR, 'main'),
    os.path.join(PROJECT_DIR, 'managed_components'),
]

kconfigs          = find_kconfigs(search_dirs, 'Kconfig')
kconfigs_projbuild = find_kconfigs(search_dirs, 'Kconfig.projbuild')

def write_source_file(paths):
    f = tempfile.NamedTemporaryFile(mode='w', suffix='.in', delete=False)
    for p in paths:
        f.write(f'source "{p}"\n')
    f.flush()
    return f.name

os.environ['COMPONENT_KCONFIGS_SOURCE_FILE']          = write_source_file(kconfigs)
os.environ['COMPONENT_KCONFIGS_PROJBUILD_SOURCE_FILE'] = write_source_file(kconfigs_projbuild)

print(f'Found {len(kconfigs)} Kconfig + {len(kconfigs_projbuild)} Kconfig.projbuild files')

# ---- Load Kconfig ----

import kconfiglib

kconf = kconfiglib.Kconfig(os.path.join(IDF_PATH, 'Kconfig'), warn_to_stderr=False)

# Type constants — not exported in all kconfiglib versions; derive from a live symbol instead
_UNKNOWN  = 0
_BOOL     = 1
_TRISTATE = 2
_STRING   = 3
_INT      = 4
_HEX      = 5
# Override with module attrs if present (upstream kconfiglib)
for _attr, _val in (('UNKNOWN',0),('BOOL',1),('TRISTATE',2),('STRING',3),('INT',4),('HEX',5)):
    if hasattr(kconfiglib, _attr):
        globals()[f'_{_attr}'] = getattr(kconfiglib, _attr)

TYPE_NAMES = {
    _BOOL:     'bool',
    _INT:      'int',
    _HEX:      'hex',
    _STRING:   'string',
    _TRISTATE: 'tristate',
}

# ---- Generate Markdown ----

def menu_path(node):
    parts = []
    n = node.parent
    while n and n is not kconf.top_node:
        if hasattr(n, 'prompt') and n.prompt:
            parts.append(n.prompt[0])
        n = n.parent
    return ' > '.join(reversed(parts))

out = [
    '# ESP-IDF sdkconfig Options\n\n',
    f'Target: `{IDF_TARGET}` — IDF `{os.path.basename(IDF_PATH)}`\n\n',
    f'Total symbols: {len(kconf.unique_defined_syms)}\n\n---\n\n',
]

for sym in sorted(kconf.unique_defined_syms, key=lambda s: s.name):
    if sym.type == _UNKNOWN:
        continue

    name = f'CONFIG_{sym.name}'
    typ  = TYPE_NAMES.get(sym.type, 'unknown')

    prompt_str = location = path_str = help_text = ''
    for node in sym.nodes:
        if node.prompt and not prompt_str:
            prompt_str = node.prompt[0]
            path_str   = menu_path(node)
            location   = f'{node.filename}:{node.linenr}'
        if not help_text and getattr(node, 'help', None):
            help_text = node.help

    out.append(f'## {name}\n\n')
    if prompt_str:
        out.append(f'**{prompt_str}**\n\n')
    out.append(f'- **Type:** `{typ}`\n')
    out.append(f'- **Default:** `{sym.str_value}`\n')
    if path_str:
        out.append(f'- **Menu:** {path_str}\n')
    if location:
        out.append(f'- **Kconfig:** `{location}`\n')
    if help_text:
        out.append(f'\n{help_text.strip()}\n')
    out.append('\n---\n\n')

output_file = os.path.join(PROJECT_DIR, 'sdkconfig_docs.md')
with open(output_file, 'w') as f:
    f.writelines(out)

print(f'Written {len(kconf.unique_defined_syms)} options to {output_file}')
