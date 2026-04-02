# vibeinterpreter

A shebang-compatible interpreter that converts natural language scripts into bash using the Claude API, then executes them.

## Prerequisites

- GCC
- libcurl development headers
- A Google Gemini API key

Install dependencies on Debian/Ubuntu:

```
sudo apt-get install build-essential libcurl4-openssl-dev
```

## Build

```
make
```

## Install

```
sudo make install
```

This installs the binary to `/usr/bin/vibeinterpreter` and symlinks it to `/bin/vibeinterpreter` (required for shebang support). It also creates `/etc/vibeinterpreter/` if it doesn't exist.

## Configure

Place your Gemini API key in the config file:

```
echo "your-gemini-api-key" | sudo tee /etc/vibeinterpreter/api.secret
sudo chmod 600 /etc/vibeinterpreter/api.secret
```

## Usage

Create a `.vi` script:

```
#!/bin/vibeinterpreter

List all files in the current directory sorted by size, largest first,
and show their sizes in human-readable format.
```

Make it executable and run:

```
chmod +x script.vi
./script.vi
```

Or invoke directly:

```
vibeinterpreter script.vi
```

Extra arguments after the script name are passed through to the generated bash script.

## Uninstall

```
sudo make uninstall
```
