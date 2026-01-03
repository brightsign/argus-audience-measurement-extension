# BrightSign Extension Naming Convention

This document describes the naming convention for BrightSign OS extensions, including how to incorporate version information into the extension name using git tags.

## Overview

BrightSign extensions have strict naming requirements enforced by the packaging scripts. This guide covers:

1. Base name requirements
2. Version string format
3. Combined naming scheme
4. Makefile implementation

## Base Name Requirements

The extension base name must follow these rules:

| Constraint | Requirement |
|------------|-------------|
| Length | 3-31 characters total (including version suffix) |
| First character | Lowercase letter (a-z) |
| Remaining characters | Lowercase letters (a-z), numbers (0-9), or underscores (_) |
| Not allowed | Uppercase letters, hyphens (-), dots (.), spaces, special characters |

### Valid Base Names

```
promgraf        # 8 characters
my_extension    # 12 characters
node_exporter   # 13 characters
app1            # 4 characters
```

### Invalid Base Names

```
PromGraf        # uppercase not allowed
my-extension    # hyphens not allowed
1app            # cannot start with number
_private        # cannot start with underscore
ab              # too short (minimum 3)
```

## Version String Format

Since dots (`.`) are not allowed in extension names, semantic versions must be transformed:

| Semver | Extension Format |
|--------|------------------|
| `1.0.0` | `1_0_0` |
| `2.1.3` | `2_1_3` |
| `10.20.30` | `10_20_30` |

### Transformation Rules

1. Remove any `v` prefix from the git tag
2. Replace dots (`.`) with underscores (`_`)

```
v1.0.0  ->  1_0_0
v2.1.3  ->  2_1_3
1.0.0   ->  1_0_0
```

## Combined Naming Scheme

The full extension name combines the base name and version with a single underscore separator:

```
{base_name}_{major}_{minor}_{patch}
```

### Examples

| Base Name | Version | Full Extension Name |
|-----------|---------|---------------------|
| `promgraf` | `1.0.0` | `promgraf_1_0_0` |
| `promgraf` | `2.1.3` | `promgraf_2_1_3` |
| `node_exp` | `1.8.2` | `node_exp_1_8_2` |

### Length Considerations

With the 31-character maximum, plan your base name accordingly:

```
promgraf_1_0_0      = 14 characters  (OK)
promgraf_10_20_30   = 17 characters  (OK)
my_long_extension_name_1_0_0 = 28 characters (OK)
very_long_extension_base_name_1_0_0 = 35 characters (TOO LONG)
```

**Recommendation:** Keep base names under 20 characters to allow for version growth.

## Makefile Implementation

### Basic Implementation

Add these lines near the top of your Makefile:

```makefile
#------------------------------------------------------------------------------
# Extension Naming
#------------------------------------------------------------------------------
# Base name for the extension (must follow BrightSign naming rules)
EXTENSION_BASE := myextension

# Get version from git tag: v1.0.0 -> 1_0_0
# Falls back to 0_0_0 if no tags exist
GIT_VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' | tr '.' '_')
ifeq ($(GIT_VERSION),)
    GIT_VERSION := 0_0_0
endif

# Allow override via environment variable
EXT_VERSION ?= $(GIT_VERSION)

# Final extension name: myextension_1_0_0
EXTENSION_NAME := $(EXTENSION_BASE)_$(EXT_VERSION)
```

### How It Works

1. **`git describe --tags --abbrev=0`** - Gets the most recent tag reachable from HEAD
2. **`sed 's/^v//'`** - Strips the `v` prefix if present
3. **`tr '.' '_'`** - Replaces dots with underscores
4. **Fallback** - Uses `0_0_0` if no git tags exist
5. **Override** - `EXT_VERSION` environment variable can override the git tag

### Usage Examples

```bash
# Use version from git tag (default)
make package

# Override version manually
make package EXT_VERSION=2_0_0

# Check what name will be used
make help
```

### Adding Help Output

Include the extension name in your help target:

```makefile
help:
	@echo "Extension Name: $(EXTENSION_NAME)"
	@echo "  Base: $(EXTENSION_BASE)"
	@echo "  Version: $(EXT_VERSION)"
```

## Git Tagging Workflow

### Creating Version Tags

```bash
# Create a new version tag
git tag v1.0.0
git push origin v1.0.0

# Build with the new version
make package
# Creates: myextension_1_0_0.squashfs
```

### Tag Naming Convention

Use semantic versioning with a `v` prefix:

```
v1.0.0    # Initial release
v1.0.1    # Patch release (bug fixes)
v1.1.0    # Minor release (new features, backward compatible)
v2.0.0    # Major release (breaking changes)
```

### Listing Tags

```bash
# List all tags
git tag -l

# List tags matching a pattern
git tag -l "v1.*"

# Show current version
git describe --tags --abbrev=0
```

## Advanced Patterns

### Development Builds

For development builds that include commit information:

```makefile
# Include commit count and hash: v1.0.0-5-g1a2b3c4 -> 1_0_0_5_g1a2b3c4
GIT_VERSION_DEV := $(shell git describe --tags 2>/dev/null | sed 's/^v//' | tr '.-' '_')
```

### Unversioned Builds

For builds without version in the name (useful during development):

```makefile
# Use versioned name only for tagged commits
GIT_TAG := $(shell git describe --tags --exact-match 2>/dev/null | sed 's/^v//' | tr '.' '_')

ifneq ($(GIT_TAG),)
    EXTENSION_NAME := $(EXTENSION_BASE)_$(GIT_TAG)
else
    EXTENSION_NAME := $(EXTENSION_BASE)
endif
```

### CI/CD Integration

For CI/CD pipelines, pass the version explicitly:

```yaml
# GitHub Actions example
- name: Build extension
  run: make package EXT_VERSION=${{ github.ref_name | sed 's/^v//' | tr '.' '_' }}
```

```bash
# Jenkins/shell script example
VERSION=$(echo "$GIT_TAG" | sed 's/^v//' | tr '.' '_')
make package EXT_VERSION="$VERSION"
```

## Validation

The packaging scripts (`make-extension-lvm`, `make-extension-ubi`) validate the final extension name using this regex:

```bash
^[a-z][a-z0-9_]{2,30}$
```

This ensures:
- Starts with lowercase letter
- Contains only lowercase letters, numbers, underscores
- Total length is 3-31 characters

If validation fails, you'll see:

```
Error: Invalid extension name 'My_Extension_1_0_0'
Name must be 3-31 lowercase letters, numbers, or underscores, starting with a letter
```

## File Artifacts

After packaging, you'll have these files:

```
ext_{name}.squashfs           # The extension filesystem image
ext_{name}_install-lvm.sh     # Install script for LVM volumes
{name}-{timestamp}.zip        # Distribution package
```

Example for `promgraf_1_0_0`:

```
ext_promgraf_1_0_0.squashfs
ext_promgraf_1_0_0_install-lvm.sh
promgraf_1_0_0-20240115-143022.zip
```

## Installation Path

Extensions are mounted at:

```
/var/volatile/bsext/ext_{name}/
```

Example:

```
/var/volatile/bsext/ext_promgraf_1_0_0/
```

## Summary

1. **Base name:** 3-20 chars, lowercase, letters/numbers/underscores, starts with letter
2. **Version format:** `{major}_{minor}_{patch}` (underscores replace dots)
3. **Full name:** `{base}_{major}_{minor}_{patch}` (single underscore separator)
4. **Git tags:** Use `v1.0.0` format, auto-transformed by Makefile
5. **Override:** Use `EXT_VERSION=x_y_z make package` when needed
