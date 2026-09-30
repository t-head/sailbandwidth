#!/usr/bin/env bash
#########################################################################
# SPDX-FileCopyrightText: Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# See LICENSE for license information
#########################################################################
set -e
#set -x

# Options and build steps; the argparse helper is defined at the end (main runs last).
main() {
ARGPARSE_DESCRIPTION="sailbandwidth build script"      # this is optional
argparse "$@" <<EOF || exit 1
parser.add_argument('--rebuild', default=False, action='store_true', help="clean and rebuild")
parser.add_argument('--build_type', default='rel_sym', type=str, help="Symbol control. rel_sym: release with some symbols. dbg: debug version. rel_no_sym: release and strip symbol.")
parser.add_argument('--verbose', default=False, action='store_true', help="show more compile message")
parser.add_argument('--ppu_archs', default='ppu_10,ppu_15', type=str, help="set ppu archs", choices=['ppu_10', 'ppu_15', 'ppu_10,ppu_15'])
parser.add_argument('--use_mpi', default=False, action='store_true', help="use mpi to build multinode version of sailbandwidth")
parser.add_argument('--use_vpipeid', default=False, action='store_true', help="enable use_vpipeid compile macro")
parser.add_argument('--cpu_num', default=0, type=int, help="specify the number of parallel jobs for make -j (default: use all CPUs)")
EOF

cmake_option="-DPPU_ARCHS=$PPU_ARCHS"

if [[ $BUILD_TYPE == "dbg" ]]; then
    cmake_option="${cmake_option} -DBUILD_DEBUG=ON"
elif [[ $BUILD_TYPE == "rel_no_sym" ]]; then
    cmake_option="${cmake_option} -DREL_NO_SYM=ON"
fi

if [[ $REBUILD ]]; then
    if [[ -d build ]]; then
        echo "Delete the build directory and rebuild"
        rm -rf build
    else
        echo "Build directory does not exist, nothing to delete"
    fi
fi

if [[ $VERBOSE ]]; then
    cmake_option="${cmake_option} -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON"
else
    cmake_option="${cmake_option} -DCMAKE_VERBOSE_MAKEFILE:BOOL=OFF"
fi

if [[ $USE_MPI ]]; then
    cmake_option="${cmake_option} -DMULTINODE=1"
fi

if [[ $USE_VPIPEID ]]; then
    cmake_option="${cmake_option} -DUSE_VPIPEID=1"
fi

cmake -S . -B build/ ${cmake_option}

cd build
if [ $CPU_NUM -gt 0 ]; then
    make -j${CPU_NUM}
else
    make -j
fi
}

# --- self-contained argparse (drop-in for the external helper; no deps) ------
# Reads parser.add_argument(...) from stdin, parses "$@" like Python argparse and exports
# each option as an upper-case variable (--build_type -> $BUILD_TYPE; store_true -> "yes"/"").
_argparse_default() {
    local line="$1" name fallback
    REPLY=""; REPLY_DISPLAY="None"; REPLY_KIND=none
    if [[ "$line" =~ default=os\.getenv\(\"([^\"]*)\"[[:space:]]*,[[:space:]]*\"([^\"]*)\"\) ]]; then
        name="${BASH_REMATCH[1]}"; fallback="${BASH_REMATCH[2]}"
        REPLY="${!name-$fallback}"; REPLY_DISPLAY="$REPLY"; REPLY_KIND=str
    elif [[ "$line" =~ default=os\.getenv\(\"([^\"]*)\"\) ]]; then
        name="${BASH_REMATCH[1]}"
        if [[ -n "${!name+set}" ]]; then REPLY="${!name}"; REPLY_DISPLAY="$REPLY"; REPLY_KIND=str; fi
    elif [[ "$line" =~ default=\'([^\']*)\' ]]; then
        REPLY="${BASH_REMATCH[1]}"; REPLY_DISPLAY="$REPLY"; REPLY_KIND=str
    elif [[ "$line" =~ default=\"([^\"]*)\" ]]; then
        REPLY="${BASH_REMATCH[1]}"; REPLY_DISPLAY="$REPLY"; REPLY_KIND=str
    elif [[ "$line" =~ default=(True|False) ]]; then
        REPLY="${BASH_REMATCH[1]}"; REPLY_DISPLAY="$REPLY"; REPLY_KIND=bool
    elif [[ "$line" =~ default=(-?[0-9]+) ]]; then
        REPLY="${BASH_REMATCH[1]}"; REPLY_DISPLAY="$REPLY"; REPLY_KIND=num
    fi
}

_argparse_choices() {
    local line="$1" blob item
    REPLY=""
    [[ "$line" =~ choices=\[([^]]*)\] ]] || return 0
    blob="${BASH_REMATCH[1]}"
    while [[ -n "$blob" ]]; do
        if [[ "$blob" =~ ^[[:space:],]*\'([^\']*)\'(.*)$ ]]; then
            item="q${BASH_REMATCH[1]}"; blob="${BASH_REMATCH[2]}"
        elif [[ "$blob" =~ ^[[:space:],]*\"([^\"]*)\"(.*)$ ]]; then
            item="q${BASH_REMATCH[1]}"; blob="${BASH_REMATCH[2]}"
        elif [[ "$blob" =~ ^[[:space:],]*([^,[:space:]]+)(.*)$ ]]; then
            item="u${BASH_REMATCH[1]}"; blob="${BASH_REMATCH[2]}"
        else
            break
        fi
        REPLY+="${item}"$'\n'
    done
}

_argparse_in_choices() {
    local val="$1" typ="$2" c
    while IFS= read -r c; do
        if [[ "$typ" == int ]]; then
            if [[ "${c:0:1}" == u ]] && _argparse_int "${c:1}" && [[ "$REPLY" == "$val" ]]; then return 0; fi
        elif [[ "${c:0:1}" == q && "${c:1}" == "$val" ]]; then
            return 0
        fi
    done < <(printf '%s' "$3")
    return 1
}

_argparse_int() {
    local re='^[[:space:]]*([+-]?)([0-9]+(_[0-9]+)*)[[:space:]]*$' sign digits
    [[ "$1" =~ $re ]] || return 1
    sign="${BASH_REMATCH[1]}"; digits="${BASH_REMATCH[2]//_/}"
    if [[ "$digits" =~ ^0*([0-9].*)$ ]]; then digits="${BASH_REMATCH[1]}"; fi
    if [[ "$sign" == "-" && "$digits" != 0 ]]; then REPLY="-$digits"; else REPLY="$digits"; fi
}

_argparse_classify() {
    local s="$1" pre="" exp="" has=0 i n=0 m_idx m_opt m_exp m_has names=""
    REPLY_KIND=A; REPLY_IDX=-1; REPLY_OPT=""; REPLY_EXP=""; REPLY_HASEXP=0
    if [[ -z "$s" || "${s:0:1}" != "-" ]]; then return 0; fi
    for i in "${!s_str[@]}"; do
        if [[ "${s_str[$i]}" == "$s" ]]; then REPLY_KIND=O; REPLY_IDX="${s_idx[$i]}"; REPLY_OPT="$s"; return 0; fi
    done
    if [[ ${#s} -eq 1 ]]; then return 0; fi
    if [[ "$s" == *=* ]]; then
        pre="${s%%=*}"; exp="${s#*=}"; has=1
        for i in "${!s_str[@]}"; do
            if [[ "${s_str[$i]}" == "$pre" ]]; then
                REPLY_KIND=O; REPLY_IDX="${s_idx[$i]}"; REPLY_OPT="$pre"; REPLY_EXP="$exp"; REPLY_HASEXP=1; return 0
            fi
        done
    else
        pre="$s"
    fi
    for i in "${!s_str[@]}"; do
        if [[ "${s:1:1}" != "-" && "${s_str[$i]}" == "${s:0:2}" ]]; then
            n=$((n + 1)); m_idx="${s_idx[$i]}"; m_opt="${s_str[$i]}"; m_exp="${s:2}"; m_has=1
        elif [[ "${s:1:1}" == "-" && "${s_str[$i]}" == "$pre"* ]] || [[ "${s:1:1}" != "-" && "${s_str[$i]}" == "$s"* ]]; then
            n=$((n + 1)); m_idx="${s_idx[$i]}"; m_opt="${s_str[$i]}"; m_exp="$exp"; m_has=$has
            if [[ "${s:1:1}" != "-" ]]; then m_exp=""; m_has=0; fi
        else
            continue
        fi
        names+="${names:+, }${s_str[$i]}"
    done
    if [[ $n -gt 1 ]]; then echo "argparse: ambiguous option: $s could match $names" >&2; return 1; fi
    if [[ $n -eq 1 ]]; then
        REPLY_KIND=O; REPLY_IDX="$m_idx"; REPLY_OPT="$m_opt"; REPLY_EXP="$m_exp"; REPLY_HASEXP="$m_has"; return 0
    fi
    if [[ "$s" =~ $negre ]]; then
        if [[ $has_neg == 0 ]]; then return 0; fi
    fi
    if [[ "$s" == *" "* ]]; then return 0; fi
    REPLY_KIND=O; REPLY_OPT="$s"
}

_argparse_help() {
    local line="$1"
    REPLY=""
    if [[ "$line" =~ help=\"([^\"]*)\" ]]; then
        REPLY="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ help=\'([^\']*)\' ]]; then
        REPLY="${BASH_REMATCH[1]}"
    fi
}

_argparse_usage() {
    local i label metavar choice choices_display separator
    echo "usage: $(basename "$0") [options]"
    echo "${ARGPARSE_DESCRIPTION:-usage}"
    echo
    echo "options:"
    echo "  -h, --help               show this help message and exit"
    for i in "${!o_name[@]}"; do
        label="${o_name[$i]}"
        if [[ -n "${o_short[$i]}" && "${o_short[$i]}" != "$label" ]]; then label="${o_short[$i]}, $label"; fi
        if [[ "${o_flag[$i]}" != 1 ]]; then
            metavar="${o_var[$i]}"
            if [[ -n "${o_choices[$i]}" ]]; then
                choices_display=""; separator=""
                while IFS= read -r choice; do
                    choice="${choice:1}"
                    [[ -n "$choice" ]] || choice="''"
                    choices_display+="$separator$choice"; separator=","
                done < <(printf '%s' "${o_choices[$i]}")
                metavar="{$choices_display}"
            fi
            label="$label $metavar"
        fi
        if [[ "${o_flag[$i]}" == 1 ]]; then
            printf '  %-30s %s (default: %s)\n' "$label" "${o_help[$i]}" "${o_def[$i]}"
        else
            printf '  %-30s %s (type: %s, default: %s)\n' \
                "$label" "${o_help[$i]}" "${o_typ[$i]}" "${o_def[$i]}"
        fi
    done
}

argparse() {
    local line name short var isflag def def_display kind typ choices help
    local i j n idx opt exp hasexp val sep found has_neg=0 negre='^-[0-9]+$|^-[0-9]*\.[0-9]+$'
    local -a o_name=() o_short=() o_var=() o_flag=() o_def=() o_typ=() o_choices=() o_help=()
    local -a o_defstr=() o_seen=() s_str=("-h" "--help") s_idx=(H H) argv=("$@")
    local -a a_kind=() a_idx=() a_opt=() a_exp=() a_has=() extras=() t_idx=() t_val=()

    while IFS= read -r line; do
        [[ "$line" == *add_argument* ]] || continue
        short=""
        if [[ "$line" =~ \'(-[A-Za-z0-9])\' ]]; then short="${BASH_REMATCH[1]}"; fi
        if [[ "$line" =~ \'(--[A-Za-z0-9_-]+)\' ]]; then
            name="${BASH_REMATCH[1]}"
        elif [[ -n "$short" ]]; then
            name="$short"
        else
            continue
        fi
        var="${name#--}"; var="${var#-}"; var="${var//-/_}"
        var="$(printf '%s' "$var" | tr '[:lower:]' '[:upper:]')"

        if [[ "$line" =~ action=[\'\"]store_true[\'\"] ]]; then isflag=1; else isflag=0; fi
        if [[ "$line" =~ type=int ]]; then typ=int; else typ=str; fi
        _argparse_default "$line"; def="$REPLY"; def_display="$REPLY_DISPLAY"; kind="$REPLY_KIND"
        if [[ "$kind" == bool ]]; then
            if [[ "$def" == True ]]; then def="yes"; else def=""; fi
        elif [[ "$kind" == num ]] && _argparse_int "$def"; then
            def="$REPLY"
        fi
        _argparse_choices "$line"; choices="$REPLY"
        _argparse_help "$line"; help="$REPLY"

        o_name+=("$name"); o_short+=("$short"); o_var+=("$var"); o_flag+=("$isflag")
        o_def+=("$def_display"); o_typ+=("$typ"); o_choices+=("$choices"); o_help+=("$help")
        if [[ "$kind" == str ]]; then o_defstr+=(1); else o_defstr+=(0); fi
        o_seen+=(0)
        s_str+=("$name"); s_idx+=("$((${#o_name[@]} - 1))")
        if [[ -n "$short" && "$short" != "$name" ]]; then s_str+=("$short"); s_idx+=("$((${#o_name[@]} - 1))"); fi
        printf -v "$var" '%s' "$def"
    done
    for i in "${!s_str[@]}"; do
        if [[ "${s_str[$i]}" =~ $negre ]]; then has_neg=1; fi
    done

    sep=0
    for i in "${!argv[@]}"; do
        REPLY_KIND=A; REPLY_IDX=-1; REPLY_OPT=""; REPLY_EXP=""; REPLY_HASEXP=0
        if [[ $sep == 1 ]]; then
            :
        elif [[ "${argv[$i]}" == "--" ]]; then
            REPLY_KIND=-; sep=1
        else
            _argparse_classify "${argv[$i]}" || return 1
        fi
        a_kind+=("$REPLY_KIND"); a_idx+=("$REPLY_IDX"); a_opt+=("$REPLY_OPT"); a_exp+=("$REPLY_EXP"); a_has+=("$REPLY_HASEXP")
    done

    n=${#argv[@]}; i=0
    while [[ $i -lt $n ]]; do
        if [[ "${a_kind[$i]}" != O || "${a_idx[$i]}" == -1 ]]; then
            extras+=("${argv[$i]}"); i=$((i + 1)); continue
        fi
        idx="${a_idx[$i]}"; opt="${a_opt[$i]}"; exp="${a_exp[$i]}"; hasexp="${a_has[$i]}"
        t_idx=(); t_val=()
        while true; do
            if [[ "$idx" != H && "${o_flag[$idx]}" != 1 ]]; then
                if [[ $hasexp == 1 ]]; then
                    t_val+=("$exp"); i=$((i + 1))
                elif [[ $((i + 1)) -lt $n && "${a_kind[$((i + 1))]}" == A ]]; then
                    t_val+=("${argv[$((i + 1))]}"); i=$((i + 2))
                else
                    echo "argparse: argument $opt: expected one argument" >&2; return 1
                fi
                t_idx+=("$idx"); break
            fi
            t_idx+=("$idx"); t_val+=("")
            if [[ $hasexp == 0 ]]; then i=$((i + 1)); break; fi
            found=-1
            if [[ "${opt:1:1}" != "-" && -n "$exp" ]]; then
                for j in "${!s_str[@]}"; do
                    if [[ "${s_str[$j]}" == "-${exp:0:1}" ]]; then found="${s_idx[$j]}"; break; fi
                done
            fi
            if [[ "$found" == -1 ]]; then
                echo "argparse: argument $opt: ignored explicit argument '$exp'" >&2; return 1
            fi
            opt="-${exp:0:1}"; exp="${exp:1}"; idx="$found"
            if [[ -n "$exp" ]]; then hasexp=1; else hasexp=0; fi
        done
        for j in "${!t_idx[@]}"; do
            idx="${t_idx[$j]}"
            if [[ "$idx" == H ]]; then _argparse_usage; return 1; fi
            o_seen[$idx]=1
            if [[ "${o_flag[$idx]}" == 1 ]]; then printf -v "${o_var[$idx]}" '%s' "yes"; continue; fi
            val="${t_val[$j]}"
            if [[ "$val" == "--" ]]; then unset "${o_var[$idx]}"; continue; fi
            if [[ "${o_typ[$idx]}" == int ]]; then
                if ! _argparse_int "$val"; then
                    echo "argparse: argument ${o_name[$idx]}: invalid int value: '$val'" >&2; return 1
                fi
                val="$REPLY"
            fi
            if [[ -n "${o_choices[$idx]}" ]] && ! _argparse_in_choices "$val" "${o_typ[$idx]}" "${o_choices[$idx]}"; then
                echo "argparse: argument ${o_name[$idx]}: invalid choice: '$val'" >&2; return 1
            fi
            printf -v "${o_var[$idx]}" '%s' "$val"
        done
    done

    for i in "${!o_name[@]}"; do
        if [[ "${o_typ[$i]}" == int && "${o_defstr[$i]}" == 1 && "${o_seen[$i]}" != 1 ]]; then
            var="${o_var[$i]}"
            if ! _argparse_int "${!var}"; then
                echo "argparse: argument ${o_name[$i]}: invalid int value: '${!var}'" >&2; return 1
            fi
            printf -v "$var" '%s' "$REPLY"
        fi
    done
    if [[ ${#extras[@]} -gt 0 ]]; then
        echo "argparse: unrecognized arguments: ${extras[*]}" >&2; return 1
    fi
    return 0
}

main "$@"
