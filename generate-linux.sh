#!/bin/sh

usage="Usage: $0 [-h|--help] [<options>] [<config>]"

# External toolchain defaults.
mn_ver="11.0.0"
mn_url="https://dl.winehq.org/wine/wine-mono"
mw_url="https://github.com/mstorsjo/msvc-wine.git"

# Cache and installation directories.
ca_dir="/tmp/msvc"
mi_dir="$HOME/.msvc"
wn_dir="$HOME/.wine"

# Build configuration (Debug or Release).
cfg="Debug"

owd="$(pwd)"
prog="$0"

fail ()
{
  cd "$owd" || true
  exit 1
}

diag ()
{
  if test $# -eq 0; then
    echo 1>&2
  else
    echo "$*" 1>&2
  fi
}

error ()
{
  diag "error: $*"
  fail
}

run ()
{
  diag "+ $*"
  "$@"
  if test "$?" -ne 0; then
    fail
  fi
}

check_cmd ()
{
  if ! command -v "$1" >/dev/null 2>&1; then
    diag "error: unable to execute $1: command not found"
    if test -n "$2"; then
      diag "  info: $2"
    fi
    fail
  fi
}

yes=
clean=
deps=true
submods=true
verb=
force=
to=300
cto=30

bcfg=

while test $# -ne 0; do
  case "$1" in
    -h|--help)
      diag
      diag "$usage"
      diag "Options:"
      diag "  --yes               Don't ask for confirmation before starting."
      diag "  --clean             Remove existing wine and msvc installations before starting."
      diag "  --no-deps           Don't install system dependencies."
      diag "  --no-submodules     Don't update git submodules."
      diag "  --force             Perform a full bootstrap even if the environment is ready."
      diag "  --verbose           Print commands before they are executed."
      diag "  --cache-dir <dir>   Directory to cache downloaded msvc components (/tmp/msvc by default)."
      diag "  --msvc-dir <dir>    Directory to install the msvc toolchain (~/.msvc by default)."
      diag "  --wine-dir <dir>    Directory to use for the wine prefix (~/.wine by default)."
      diag "  --timeout <sec>     Network operations timeout in seconds (300 by default)."
      diag
      diag "By default this script builds the Debug configuration."
      diag
      diag "If the package manager requires root permissions to install system"
      diag "dependencies, sudo(1) will be used by default."
      diag
      diag "Options must come before the <config> argument."
      diag
      exit 0
      ;;
    --yes)
      yes=true
      shift
      ;;
    --clean)
      clean=true
      shift
      ;;
    --no-deps)
      deps=
      shift
      ;;
    --no-submodules)
      submods=
      shift
      ;;
    --force)
      force=true
      shift
      ;;
    --verbose)
      verb=true
      shift
      ;;
    --cache-dir)
      shift
      if test $# -eq 0; then
        error "directory expected after --cache-dir; run $prog -h for details"
      fi
      ca_dir="$1"
      shift
      ;;
    --msvc-dir)
      shift
      if test $# -eq 0; then
        error "directory expected after --msvc-dir; run $prog -h for details"
      fi
      mi_dir="$1"
      shift
      ;;
    --wine-dir)
      shift
      if test $# -eq 0; then
        error "directory expected after --wine-dir; run $prog -h for details"
      fi
      wn_dir="$1"
      shift
      ;;
    --timeout)
      shift
      if test $# -eq 0; then
        error "value in seconds expected after --timeout; run $prog -h for details"
      fi
      to="$1"
      shift
      ;;
    -*)
      diag "error: unknown option '$1'"
      diag "  info: run 'sh $prog -h' for usage"
      fail
      ;;
    *)
      bcfg="$1"
      shift
      if test $# -ne 0; then
        diag "error: unexpected argument '$1'"
        diag "  info: options must come before the <config> argument"
        fail
      fi
      break
      ;;
  esac
done

if test -n "$bcfg"; then
  case "$bcfg" in
    debug|Debug)     cfg="Debug"   ;;
    release|Release) cfg="Release" ;;
    *)
      error "invalid build configuration '$bcfg' (expected debug or release)"
      ;;
  esac
fi

if test "$verb" = true; then
  set -x
fi

prompt ()
{
  while test -z "$yes"; do
    printf "Continue? [Y/n] " 1>&2
    read -r yes
    case "$yes" in
      y|Y|"") yes=true ;;
      n|N)    fail     ;;
      *)      yes=     ;;
    esac
  done
}

download ()
{
  if test -n "$2"; then
    run curl -fL --connect-timeout "$cto" --max-time "$to" --progress-bar -o "$2" "$1"
  else
    run curl -fL --connect-timeout "$cto" --max-time "$to" --progress-bar -O "$1"
  fi
}

check_env ()
{
  if test "$force" = true; then
    return 1
  fi

  if ! test -d "$wn_dir" || ! test -d "$mi_dir"; then
    return 1
  fi

  if ! test -f "$mi_dir/bin/x86/msbuild.exe"; then
    return 1
  fi

  return 0
}

sys_deps ()
{
  if test -z "$deps"; then
    return
  fi

  diag "info: detecting package manager"

  pm=
  if   command -v dnf >/dev/null 2>&1; then pm=dnf
  elif command -v yum >/dev/null 2>&1; then pm=yum
  elif command -v apt >/dev/null 2>&1; then pm=apt
  elif command -v pacman >/dev/null 2>&1; then pm=pacman
  fi

  if test -z "$pm"; then
    error "unable to detect package manager (dnf, yum, apt, pacman)"
  fi

  diag "info: using package manager $pm"

  case "$pm" in
    dnf|yum)
      cmd="sudo $pm install -y"
      pkgs="wine python3 msitools ca-certificates samba-winbind git curl gcc gcc-c++ make"
      ;;
    apt)
      cmd="sudo apt install -y"
      pkgs="wine python3 msitools ca-certificates winbind git curl build-essential"
      ;;
    pacman)
      cmd="sudo pacman -S --noconfirm"
      pkgs="wine python msitools ca-certificates samba git curl base-devel"
      ;;
  esac

  diag "info: installing system dependencies"

  # shellcheck disable=SC2086
  run $cmd $pkgs
}

wine_setup ()
{
  diag "info: setting up wine environment"

  export WINEPREFIX="$wn_dir"
  export WINEDLLOVERRIDES="mscoree,mshtml="

  if ! test -d "$wn_dir"; then
    diag "info: initializing wine prefix at $wn_dir/"
    run wineboot --init
  fi

  mmsi="wine-mono-$mn_ver-x86.msi"
  murl="$mn_url/$mn_ver/$mmsi"

  if ! test -f "/tmp/$mmsi"; then
    diag "info: downloading wine-mono $mn_ver"
    download "$murl" "/tmp/$mmsi"
  else
    diag "info: using cached wine-mono installer"
  fi

  diag "info: installing wine-mono"
  run msiexec /i "/tmp/$mmsi" /quiet
}

msvc_setup ()
{
  diag "info: setting up msvc toolchain"

  if ! test -d "msvc-wine"; then
    run git clone "$mw_url" msvc-wine
  else
    # shellcheck disable=SC2164
    run cd msvc-wine
    run git pull
    # shellcheck disable=SC2164
    run cd "$owd"
  fi

  # shellcheck disable=SC2164
  run cd msvc-wine

  if ! test -d "$mi_dir"; then
    diag "info: downloading msvc toolchain to $ca_dir/"
    run mkdir -p "$ca_dir"
    run python3 ./vsdownload.py --accept-license --cache "$ca_dir" --dest "$mi_dir"
  else
    diag "info: msvc toolchain already present at $mi_dir/"
  fi

  diag "info: installing msvc into wine prefix"
  run ./install.sh "$mi_dir"

  # shellcheck disable=SC2164
  run cd "$owd"
}

gen_vscode ()
{
  diag "info: generating vscode configuration"

  run mkdir -p .vscode

  mv="14.44.35207"
  sv="10.0.26100.0"

  if test -d "$mi_dir/vc/tools/msvc"; then
    # shellcheck disable=SC2012
    v="$(ls "$mi_dir/vc/tools/msvc" | sort -V | tail -n 1)"
    if test -n "$v"; then mv="$v"; fi
  fi

  if test -d "$mi_dir/kits/10/Include"; then
    # shellcheck disable=SC2010,SC2012
    v="$(ls "$mi_dir/kits/10/Include" | grep -E '^10\.' | sort -V | tail -n 1)"
    if test -n "$v"; then sv="$v"; fi
  fi

  cat > .vscode/c_cpp_properties.json << EOF
{
  "configurations": [
    {
      "name": "Linux",
      "includePath": [
        "\${default}",
        "$mi_dir/vc/tools/msvc/$mv/atlmfc/include",
        "$mi_dir/vc/tools/msvc/$mv/include",
        "$mi_dir/kits/10/Include/$sv/shared",
        "$mi_dir/kits/10/Include/$sv/ucrt",
        "$mi_dir/kits/10/Include/$sv/um",
        "$mi_dir/kits/10/Include/$sv/winrt",
        "\${workspaceFolder}/**"
      ],
      "defines": [],
      "cStandard": "c17",
      "cppStandard": "c++20",
      "intelliSenseMode": "windows-msvc-x86",
      "forcedInclude": [
        "$owd/src/std_include.hpp"
      ]
    }
  ],
  "version": 4
}
EOF
}

gen_bbuild ()
{
  diag "info: generating build files"

  if ! test -f "tools/premake5.exe"; then
    error "premake5 executable not found in tools/ directory"
  fi

  run wine tools/premake5.exe generate-buildinfo

  sed '/^[[:space:]]*prebuildcommands/,/^[[:space:]]*}/d; /^[[:space:]]*postbuildcommands/,/^[[:space:]]*}/d' premake5.lua > premake5-linux.lua

  run wine tools/premake5.exe --file=premake5-linux.lua "--target-dir=%{wks.location}/bin/%{cfg.buildcfg}" vs2022
  run rm -f premake5-linux.lua
}

compile ()
{
  diag "info: building consolation-client ($cfg)"

  sln="build/consolation-client.sln"
  if ! test -f "$sln"; then
    error "solution file $sln not found"
  fi

  args="/p:Configuration=$cfg /p:Platform=Win32"
  if test "$verb" = true; then
    args="$args /verbosity:detailed"
  fi

  # shellcheck disable=SC2086
  run "$mi_dir/bin/x86/msbuild.exe" "$sln" $args
}

sync_submodules ()
{
  if test -z "$submods"; then
    return
  fi

  diag "info: updating git submodules"
  run git submodule sync --recursive
  run git submodule update --init --recursive
}

print_summary ()
{
  diag
  diag "-------------------------------------------------------------------------"
  diag
  diag "Build configuration: $cfg"
  diag "Solution:            $owd/build/consolation-client.sln"
  diag "Output directory:    $owd/build/bin/$cfg/"
  diag
}

build ()
{
  diag
  diag "-------------------------------------------------------------------------"
  diag
  diag "About to perform fast path build of Consolation Client."
  diag
  diag "Build configuration: $cfg"
  diag "Wine prefix:         $wn_dir/"
  diag "MSVC install dir:    $mi_dir/"
  diag
  diag "To perform a from-scratch bootstrap, specify the --force option."
  diag
  prompt

  sync_submodules
  gen_vscode
  gen_bbuild
  compile
  print_summary
}

bootstrap ()
{
  diag
  diag "-------------------------------------------------------------------------"
  diag
  diag "About to bootstrap Consolation Client build environment and compile."
  diag
  diag "Build configuration: $cfg"
  diag "Wine prefix:         $wn_dir/"
  diag "MSVC install dir:    $mi_dir/"
  diag
  if test "$clean" = true; then
    diag "WARNING: --clean option will remove existing wine and msvc installations."
    diag
  fi
  prompt

  if test "$clean" = true; then
    diag "info: cleaning build environment"
    run rm -rf build/ msvc-wine/ "$wn_dir" "$mi_dir" /tmp/wine-mono-*.msi
  fi

  sys_deps

  check_cmd wine "install wine package for your distribution"
  check_cmd git "install git package for your distribution"
  check_cmd curl "install curl package for your distribution"
  check_cmd python3 "install python3 package for your distribution"
  check_cmd msiexec "wine installation may be incomplete"

  wine_setup
  msvc_setup

  sync_submodules
  gen_vscode
  gen_bbuild
  compile
  print_summary

  diag "To rebuild, change to $owd/ and run:"
  diag
  diag "  sh $prog $(echo "$cfg" | tr '[:upper:]' '[:lower:]')"
  diag
}

main ()
{
  if check_env; then
    build
  else
    bootstrap
  fi
}

main
