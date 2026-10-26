# Visual Studio support for rin

Syntax highlighting for `.rin` files in Visual Studio 2022 and later, from the
TextMate grammar in [`../textmate`](../textmate).

## Install

```
powershell -ExecutionPolicy Bypass -File editors\visualstudio\install.ps1
```

This copies:

| from | to |
| --- | --- |
| `editors\textmate\rin.tmLanguage.json` | `%USERPROFILE%\.vs\Extensions\rin\Syntaxes\rin.tmLanguage.json` |
| `editors\textmate\language-configuration.json` | `%USERPROFILE%\.vs\Extensions\rin\language-configuration.json` |

**Restart Visual Studio afterwards**: it reads that folder at startup. The
grammar's `fileTypes` entry associates `*.rin` with it. Rerun the script
after changing the grammar. `-Uninstall` removes the folder, and `-DryRun`
shows what would happen.

Visual Studio maps TextMate scopes onto its own classifications by prefix:
`comment`, `string`, `constant.numeric`, `keyword`, `storage`,
`entity.name.function`, `entity.name.type`, `variable` and so on. A scope it
does not map shows as plain text, so some distinctions the grammar makes
(attributes, the `&` suffix, designated-initializer fields) may look like
their neighbours depending on the colour theme. `language-configuration.json`
(`//` and `/* */` comments, bracket pairs) is copied for completeness; Visual
Studio may not read it from a folder install, since it normally comes in
through a VSIX's pkgdef. The highlighting does not depend on it.

## VS Code

The same grammar works in VS Code. A minimal local extension is a folder in
`%USERPROFILE%\.vscode\extensions\rin` holding the two JSON files and this
`package.json`:

```json
{
  "name": "rin",
  "version": "0.0.1",
  "engines": { "vscode": "^1.60.0" },
  "contributes": {
    "languages": [{
      "id": "rin",
      "extensions": [".rin"],
      "configuration": "./language-configuration.json"
    }],
    "grammars": [{
      "language": "rin",
      "scopeName": "source.rin",
      "path": "./rin.tmLanguage.json"
    }]
  }
}
```

## Scopes

The grammar follows `nvim/syntax/rin.vim`.

| rin | scope |
| --- | --- |
| `// comment`, `/* comment */` | `comment.line.double-slash.rin`, `comment.block.rin` |
| `"text"`, `'c'`, `\n` | `string.quoted.double.rin`, `string.quoted.single.rin`, `constant.character.escape.rin` |
| `42`, `0x40`, `0b101`, `1.0`, `1024'u64` | `constant.numeric.{integer,hex,binary,float}.rin` |
| `import`, `cinclude`, `define` | `keyword.control.import.rin` |
| `if for while switch case return ...` | `keyword.control.rin` |
| `proc struct enum union alias label` | `storage.type.rin` |
| `const volatile static external external_emit` | `storage.modifier.rin` |
| `i32`, `f32`, `b32`, `void`, ... | `storage.type.primitive.rin` |
| `shl`, `shr` | `keyword.operator.word.rin` |
| `true false null` | `constant.language.rin` |
| `[external]`, `[must_use]`, `[callconv(CALLBACK)]` | `entity.other.attribute-name.rin` |
| `name: proc`, `vec<T>append: proc<T>`, calls | `entity.name.function.rin` |
| `name: struct`, a type after `:` or `->`, `vec<T>`, `Enemy<>` | `entity.name.type.rin` |
| `cast sizeof alignof offsetof` | `support.function.builtin.rin` |
| `.field =` in an initializer | `variable.other.member.rin` |
| `_ = f();` | `variable.language.discard.rin` |
| `name&` | `keyword.operator.address.rin` |
| `x: T = ?;` | `keyword.operator.uninitialized.rin` |
| `WM_DESTROY`, ALL_CAPS names | `variable.other.constant.rin` |
| `#define` and the other C directives / any other `#` line | `meta.preprocessor.rin` / `invalid.illegal.preprocessor.rin` |
