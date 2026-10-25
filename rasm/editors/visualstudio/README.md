# Visual Studio 2022 support for rasm

Syntax highlighting for `.rasm` files, from the TextMate grammar in
[`../textmate`](../textmate).

## Install

```
powershell -ExecutionPolicy Bypass -File editors\visualstudio\install.ps1
```

This copies:

| from | to |
| --- | --- |
| `editors\textmate\rasm.tmLanguage.json` | `%USERPROFILE%\.vs\Extensions\rasm\Syntaxes\rasm.tmLanguage.json` |
| `editors\textmate\language-configuration.json` | `%USERPROFILE%\.vs\Extensions\rasm\language-configuration.json` |

**Restart Visual Studio afterwards**: it reads that folder at startup. The
grammar's `fileTypes` entry associates `*.rasm` with it. Rerun the script
after changing the grammar. `-Uninstall` removes the folder, and `-DryRun`
shows what would happen.

Visual Studio maps TextMate scopes onto its own classifications by prefix:
`comment`, `string`, `constant.numeric`, `keyword`, `storage`,
`entity.name.function`, `entity.name.type`, `variable` and so on. A scope it
does not map shows as plain text, so some distinctions the grammar makes
(labels, attributes, the `&` suffix) may look the same as their neighbours
depending on the colour theme. `language-configuration.json` (`;` comments,
bracket pairs) is copied for completeness. Visual Studio may not read it
from a folder install, since it normally comes in through a VSIX's pkgdef.
The highlighting does not depend on it.

## VS Code

The same grammar works in VS Code. A minimal local extension is a folder in
`%USERPROFILE%\.vscode\extensions\rasm` holding the two JSON files and this
`package.json`:

```json
{
  "name": "rasm",
  "version": "0.0.1",
  "engines": { "vscode": "^1.60.0" },
  "contributes": {
    "languages": [{
      "id": "rasm",
      "extensions": [".rasm"],
      "configuration": "./language-configuration.json"
    }],
    "grammars": [{
      "language": "rasm",
      "scopeName": "source.rasm",
      "path": "./rasm.tmLanguage.json"
    }]
  }
}
```

## Scopes

| rasm | scope |
| --- | --- |
| `; comment` | `comment.line.semicolon.rasm` |
| `"text"`, `'c'` | `string.quoted.double.rasm`, `string.quoted.single.rasm` |
| `42`, `0x40`, `0b101`, `1.0` | `constant.numeric.{integer,hex,binary,float}.rasm` |
| `import`, `cinclude` | `keyword.control.import.rasm` |
| `#define NAME` | `keyword.control.directive.define.rasm`, `entity.name.function.preprocessor.rasm` |
| `struct union enum alias proc macro` | `storage.type.rasm` |
| `const` | `storage.modifier.rasm` |
| `u32`, `f32`, `void`, ... | `storage.type.primitive.rasm` |
| `byte dword ... ptr` | `storage.type.size.rasm` |
| `[external]`, `[align(16)]` | `entity.other.attribute-name.rasm` |
| `mov`, `addps`, ... / `rep`, `lock` | `keyword.control.mnemonic.rasm` / `keyword.control.prefix.rasm` |
| `rax`, `xmm0`, `ymm15` | `variable.language.register.rasm` |
| `name: proc` / `name: macro` | `entity.name.function.rasm` / `entity.name.function.macro.rasm` |
| `name: struct`, a type in a type position, `[rbx].T` | `entity.name.type.rasm` |
| `label:` | `entity.name.label.rasm` |
| `invoke`, `vcall` / other macro calls | `support.function.macro.rasm` / `entity.name.function.macro.rasm` |
| `sizeof offsetof alignof cast` | `support.function.builtin.rasm` |
| `name&` | `keyword.operator.address.rasm` |
| ALL_CAPS names | `variable.other.constant.rasm` |
| other `#` lines, `;` inside `#define` | `invalid.illegal.*.rasm` |
