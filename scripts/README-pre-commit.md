# Pre-commit Hook for clang-format

## Cross-Platform Support

✅ **Works on all platforms**: Linux, macOS, Windows (with Git Bash)

## Installation

### For Submodule (Current Setup)
The hook is already installed at:
```
../../.git/modules/libs/vtextedit/hooks/pre-commit
```

### For Standalone Repo
Copy the hook manually:
```bash
# Linux/macOS
cp scripts/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit

# Windows (Git Bash or WSL)
cp scripts/pre-commit .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
```

**Note**: On Linux/macOS, the `chmod +x` step is required. Git for Windows handles this automatically.

## What It Does

- Automatically formats C++ files (`.cpp`, `.h`, `.c`, `.cc`, `.cxx`, `.hpp`) before commit
- Uses `.clang-format` configuration in the repo root
- **Excludes third-party libraries**: `libs/hunspell`, `libs/peg-markdown-highlight`, `libs/sonnet`, `libs/syntax-highlighting`

## Requirements

- `clang-format` must be installed and in your PATH
- If not found, the hook will warn but allow the commit to proceed

## Manual Formatting

Format specific files manually:
```bash
clang-format -i src/textedit/vtextedit.cpp
```

Format all files (excluding third-party):
```bash
find src demo tests -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

## Bypass Hook (Not Recommended)

If needed, skip the hook with:
```bash
git commit --no-verify
```
