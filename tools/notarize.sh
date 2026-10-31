#!/usr/bin/env bash
#
# notarize.sh -- sign, notarize, staple and package Zynforge Recording.
#
# Works on a private copy of the built app, so the build tree keeps its ad-hoc
# seal and a failed run leaves nothing half-signed behind. Signing is
# inside-out: bundled dylibs, then the ZynforgeCapture helper, then the app
# bundle, because signing the bundle seals what is already inside it.
#
#   tools/notarize.sh --app PATH --identity NAME --profile NAME [options]
#
#   --app PATH        built "Zynforge Recording.app"          (or ZF_APP)
#   --identity NAME   "Developer ID Application: ..." or its
#                     SHA-1 hash, as listed by `security
#                     find-identity -v -p codesigning`        (or ZF_SIGN_IDENTITY)
#   --profile NAME    keychain profile created once with
#                     `xcrun notarytool store-credentials`    (or ZF_NOTARY_PROFILE)
#   --output DIR      where results go (default build/release) (or ZF_OUTPUT_DIR)
#   --label TEXT      DMG name part (default: short git hash)
#   --check           validate every input, change nothing, submit nothing
#   --sign-only       sign and verify only, no network. --identity - makes a
#                     local ad-hoc hardened-runtime signature for testing.
#
# Credentials stay in the keychain: this script takes a profile NAME only and
# has no option for an Apple ID, password or API key.
#
# Exit status: 0 success, 1 a step failed, 2 bad usage.

set -euo pipefail
set +x    # stays off under `bash -x`: nothing here should reach a build log unchosen

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ENTITLEMENTS_DIR="$REPO_ROOT/tools/entitlements"
APP_ENTITLEMENTS="$ENTITLEMENTS_DIR/ZynforgeRecording.entitlements"
HELPER_ENTITLEMENTS="$ENTITLEMENTS_DIR/ZynforgeCapture.entitlements"
HELPER_NAME="ZynforgeCapture"
HELPER_IDENTIFIER="com.zynforge.recording.capture"
VOLUME_NAME="Zynforge Recording"
NOTICE_FILES="LICENSE THIRD-PARTY-NOTICES.md"

APP="${ZF_APP:-}"
IDENTITY="${ZF_SIGN_IDENTITY:-}"
PROFILE="${ZF_NOTARY_PROFILE:-}"
OUTPUT="${ZF_OUTPUT_DIR:-$REPO_ROOT/build/release}"
LABEL=""
MODE="full"

WORK=""
MOUNT_POINT=""
PROBLEMS=0
CURRENT_STEP="startup"

usage()
{
    sed -n '3,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

fail()
{
    printf '\nnotarize: FAILED during "%s": %s\n' "$CURRENT_STEP" "$*" >&2
    exit 1
}

step()
{
    CURRENT_STEP="$1"
    printf '\n==> %s\n' "$1"
}

# --check reports everything wrong at once; a real run stops at the first.
problem()
{
    if [ "$MODE" = "check" ]; then
        PROBLEMS=$((PROBLEMS + 1))
        printf '  PROBLEM: %s\n' "$*" >&2
    else
        fail "$*"
    fi
}

# Show the command's output only when it fails or when the caller asks; a quiet
# success keeps the run readable and the first error easy to find.
run()
{
    local description="$1"
    shift
    local output
    if ! output=$("$@" 2>&1); then
        printf '%s\n' "$output" >&2
        fail "$description"
    fi
    printf '  ok: %s\n' "$description"
}

cleanup()
{
    if [ -n "$MOUNT_POINT" ] && [ -d "$MOUNT_POINT" ]; then
        hdiutil detach "$MOUNT_POINT" -quiet -force >/dev/null 2>&1 || true
    fi
    if [ -n "$WORK" ] && [ -d "$WORK" ]; then
        rm -rf "$WORK"
    fi
}

trap 'fail "unexpected failure at line $LINENO of notarize.sh"' ERR
trap cleanup EXIT

parse_arguments()
{
    while [ $# -gt 0 ]; do
        case "$1" in
            --app)       [ $# -ge 2 ] || usage; APP="$2"; shift 2 ;;
            --identity)  [ $# -ge 2 ] || usage; IDENTITY="$2"; shift 2 ;;
            --profile)   [ $# -ge 2 ] || usage; PROFILE="$2"; shift 2 ;;
            --output)    [ $# -ge 2 ] || usage; OUTPUT="$2"; shift 2 ;;
            --label)     [ $# -ge 2 ] || usage; LABEL="$2"; shift 2 ;;
            --check)     MODE="check"; shift ;;
            --sign-only) if [ "$MODE" = "check" ]; then CHECK_SIGN_ONLY=1; else MODE="sign-only"; fi; shift ;;
            -h|--help)   usage ;;
            *)
                # The name only: the value of a mistyped option may be a secret.
                printf 'notarize: unknown option %s (value not shown)\n' "${1%%=*}" >&2
                exit 2
                ;;
        esac
    done
}

CHECK_SIGN_ONLY=0

signing_only()
{
    [ "$MODE" = "sign-only" ] || [ "$CHECK_SIGN_ONLY" = "1" ]
}

check_tools()
{
    local tool
    for tool in codesign security spctl hdiutil ditto plutil lipo shasum file xattr xcrun; do
        command -v "$tool" >/dev/null 2>&1 || problem "required tool not found: $tool"
    done
    if ! signing_only; then
        xcrun --find notarytool >/dev/null 2>&1 || problem "notarytool not found (install Xcode 13 or newer)"
        xcrun --find stapler >/dev/null 2>&1 || problem "stapler not found (install the Xcode command line tools)"
    fi
}

plist_value()
{
    plutil -extract "$2" raw -o - "$1" 2>/dev/null
}

is_macho()
{
    file -b "$1" 2>/dev/null | grep -q 'Mach-O'
}

check_bundle()
{
    if [ -z "$APP" ]; then
        problem "no app given: pass --app or set ZF_APP"
        return 0
    fi
    if [ ! -d "$APP" ]; then
        problem "app not found: $APP"
        return 0
    fi
    local plist="$APP/Contents/Info.plist"
    if [ ! -f "$plist" ]; then
        problem "not an app bundle (no Contents/Info.plist): $APP"
        return 0
    fi

    local executable microphone
    executable=$(plist_value "$plist" CFBundleExecutable) || executable=""
    if [ -z "$executable" ] || [ ! -f "$APP/Contents/MacOS/$executable" ]; then
        problem "main executable missing from the bundle"
    elif ! is_macho "$APP/Contents/MacOS/$executable"; then
        problem "main executable is not a Mach-O binary"
    fi

    local helper="$APP/Contents/MacOS/$HELPER_NAME"
    if [ ! -f "$helper" ]; then
        problem "$HELPER_NAME is not embedded: daemon mode would fail with binary not found"
    elif ! is_macho "$helper"; then
        problem "$HELPER_NAME is not a Mach-O binary"
    elif [ -n "$executable" ] && [ -f "$APP/Contents/MacOS/$executable" ] \
        && [ "$(lipo -archs "$helper" 2>/dev/null)" != "$(lipo -archs "$APP/Contents/MacOS/$executable" 2>/dev/null)" ]; then
        problem "$HELPER_NAME and the app are built for different architectures"
    fi

    # A hardened app without this string is killed on its first input access.
    microphone=$(plist_value "$plist" NSMicrophoneUsageDescription) || microphone=""
    [ -n "$microphone" ] || problem "Info.plist has no NSMicrophoneUsageDescription"

    # Sealing these correctly needs per-bundle rules this script does not have;
    # refusing beats shipping a bundle whose nested code is signed wrongly.
    local nested
    nested=$(find "$APP/Contents" \( -name '*.framework' -o -name '*.app' -o -name '*.xpc' \
                                     -o -name '*.appex' -o -name '*.bundle' -o -name '*.plugin' \) -print -quit)
    [ -z "$nested" ] || problem "nested bundle needs explicit signing support: $nested"

    if ! plist_value "$plist" NSLocalNetworkUsageDescription >/dev/null; then
        printf '  warning: Info.plist has no NSLocalNetworkUsageDescription (macOS 15 shows its generic Local Network prompt)\n'
    fi
}

check_entitlements()
{
    local file
    for file in "$APP_ENTITLEMENTS" "$HELPER_ENTITLEMENTS"; do
        if [ ! -f "$file" ]; then
            problem "entitlements file missing: $file"
        elif ! plutil -lint -s "$file" >/dev/null 2>&1; then
            problem "entitlements file is not a valid plist: $file"
        elif grep -q 'get-task-allow' "$file"; then
            problem "get-task-allow is rejected by notarization: $file"
        fi
    done
}

check_identity()
{
    if [ -z "$IDENTITY" ]; then
        problem "no signing identity: pass --identity or set ZF_SIGN_IDENTITY"
        return 0
    fi
    if [ "$IDENTITY" = "-" ]; then
        signing_only || problem "the ad-hoc identity - cannot be notarized; use it with --sign-only"
        return 0
    fi
    local listed
    listed=$(security find-identity -v -p codesigning 2>/dev/null | grep -F -- "$IDENTITY" || true)
    if [ -z "$listed" ]; then
        problem "signing identity is not in the keychain: $IDENTITY"
    elif ! printf '%s\n' "$listed" | grep -q 'Developer ID Application'; then
        problem "identity is not a Developer ID Application certificate: $IDENTITY"
    fi
}

check_profile()
{
    signing_only && return 0
    if [ -z "$PROFILE" ]; then
        problem "no notary profile: pass --profile or set ZF_NOTARY_PROFILE"
        return 0
    fi
    # Read-only: lists past submissions, proving the stored credentials work.
    if ! xcrun notarytool history --keychain-profile "$PROFILE" --output-format json >/dev/null 2>&1; then
        problem "notary profile is missing or rejected: $PROFILE (see xcrun notarytool store-credentials)"
    fi
}

check_notices()
{
    signing_only && return 0
    local name
    for name in $NOTICE_FILES; do
        [ -f "$REPO_ROOT/$name" ] || problem "$name is missing: the DMG must carry it"
    done
}

resolve_names()
{
    if [ -z "$LABEL" ]; then
        LABEL=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null) || LABEL=""
        [ -n "$LABEL" ] || problem "no --label given and no git hash available"
    fi
    case "$LABEL" in
        *[!A-Za-z0-9._-]*) problem "--label may only use letters, digits, dot, dash, underscore" ;;
    esac
    local archs="" executable
    if [ -f "$APP/Contents/Info.plist" ]; then
        executable=$(plist_value "$APP/Contents/Info.plist" CFBundleExecutable) || executable=""
        archs=$(lipo -archs "$APP/Contents/MacOS/$executable" 2>/dev/null) || archs=""
    fi
    case "$archs" in
        *x86_64*arm64*|*arm64*x86_64*) ARCH_NAME="universal" ;;
        "")                            ARCH_NAME="unknown" ;;
        *)                             ARCH_NAME="${archs// /-}" ;;
    esac
    DMG_NAME="Zynforge-Recording-$LABEL-macOS-$ARCH_NAME.dmg"
    if ! signing_only && [ -e "$OUTPUT/$DMG_NAME" ]; then
        problem "refusing to overwrite an existing release: $OUTPUT/$DMG_NAME"
    fi
}

validate_inputs()
{
    step "Validating inputs"
    check_tools
    check_bundle
    check_entitlements
    check_identity
    check_profile
    check_notices
    resolve_names
}

prepare_stage()
{
    step "Copying the app to a private stage"
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/zf-notarize.XXXXXX")
    STAGED_APP="$WORK/$(basename "$APP")"
    # The stage lives on the system volume on purpose: an exFAT working copy
    # stores metadata as AppleDouble files, which codesign rejects as detritus.
    run "copy without extended attributes" ditto --norsrc --noextattr --noqtn "$APP" "$STAGED_APP"
    find "$STAGED_APP" -name '._*' -delete
    run "clear extended attributes" xattr -cr "$STAGED_APP"
    STAGED_EXECUTABLE="$STAGED_APP/Contents/MacOS/$(plist_value "$STAGED_APP/Contents/Info.plist" CFBundleExecutable)"
    STAGED_HELPER="$STAGED_APP/Contents/MacOS/$HELPER_NAME"
}

sign_code()
{
    local target="$1" entitlements="${2:-}" identifier="${3:-}"
    local arguments=(--force --sign "$IDENTITY" --options runtime)
    if [ "$IDENTITY" = "-" ]; then
        arguments+=(--timestamp=none)
    else
        arguments+=(--timestamp)
    fi
    if [ -n "$entitlements" ]; then arguments+=(--entitlements "$entitlements"); fi
    if [ -n "$identifier" ]; then arguments+=(--identifier "$identifier"); fi
    run "signed ${target#"$WORK"/}" codesign "${arguments[@]}" "$target"
}

# Every Mach-O in the bundle apart from the two signed by name, deepest first.
list_nested_code()
{
    local path
    while IFS= read -r path; do
        if [ "$path" != "$STAGED_EXECUTABLE" ] && [ "$path" != "$STAGED_HELPER" ] && is_macho "$path"; then
            printf '%s\n' "$path"
        fi
    done < <(find "$STAGED_APP/Contents" -type f | awk -F/ '{ print NF "\t" $0 }' | sort -rn | cut -f2-)
}

sign_bundle()
{
    step "Signing inside-out with the hardened runtime"
    local nested
    while IFS= read -r nested; do
        [ -n "$nested" ] || continue
        sign_code "$nested"
    done < <(list_nested_code)
    sign_code "$STAGED_HELPER" "$HELPER_ENTITLEMENTS" "$HELPER_IDENTIFIER"
    sign_code "$STAGED_APP" "$APP_ENTITLEMENTS"
}

verify_code()
{
    local target="$1" entitlements="${2:-}" information
    information=$(codesign -dv --verbose=4 "$target" 2>&1) \
        || { printf '%s\n' "$information" >&2; fail "cannot read the signature of $target"; }
    printf '%s\n' "$information" | grep -Eq '^CodeDirectory .*flags=0x[0-9a-f]+\([^)]*runtime' \
        || fail "hardened runtime is not set on $target"

    if [ "$IDENTITY" != "-" ]; then
        printf '%s\n' "$information" | grep -q '^Authority=Developer ID Application' \
            || fail "not signed with a Developer ID Application certificate: $target"
        printf '%s\n' "$information" | grep -q '^Timestamp=' \
            || fail "no secure timestamp on $target: notarization would reject it"
    fi

    if [ -n "$entitlements" ]; then
        local expected actual
        expected=$(plutil -convert xml1 -o - "$entitlements")
        actual=$(codesign -d --entitlements - --xml "$target" 2>/dev/null | plutil -convert xml1 -o - -) \
            || fail "cannot read the entitlements of $target"
        [ "$expected" = "$actual" ] || fail "entitlements of $target differ from $(basename "$entitlements")"
    fi
}

verify_signatures()
{
    step "Verifying signatures and entitlements"
    run "codesign deep strict verification" codesign --verify --deep --strict --verbose=2 "$STAGED_APP"
    local nested
    while IFS= read -r nested; do
        [ -n "$nested" ] || continue
        verify_code "$nested"
    done < <(list_nested_code)
    verify_code "$STAGED_HELPER" "$HELPER_ENTITLEMENTS"
    verify_code "$STAGED_APP" "$APP_ENTITLEMENTS"
    printf '  ok: hardened runtime and entitlements as expected\n'
}

# `notarytool submit --wait` can exit 0 for a rejected submission, so only the
# reported status counts.
notarize_file()
{
    local payload="$1" name="$2"
    local result="$WORK/$name.json" errors="$WORK/$name.err"
    local status="" identifier="" submitted=0
    printf '  submitting %s (this waits for Apple)\n' "$(basename "$payload")"
    if xcrun notarytool submit "$payload" --keychain-profile "$PROFILE" --wait --timeout 2h \
        --output-format json >"$result" 2>"$errors"; then
        submitted=1
    fi
    status=$(plist_value "$result" status) || status=""
    identifier=$(plist_value "$result" id) || identifier=""
    if [ "$status" != "Accepted" ]; then
        cat "$errors" >&2 || true
        if [ -n "$identifier" ]; then
            mkdir -p "$OUTPUT"
            local log="$OUTPUT/$name-notary-log.json"
            if xcrun notarytool log "$identifier" --keychain-profile "$PROFILE" "$log" >/dev/null 2>&1; then
                cat "$log" >&2
                printf '\nnotary log saved to %s\n' "$log" >&2
            fi
        fi
        fail "Apple did not accept $(basename "$payload"): status '${status:-none}', tool exit $((1 - submitted))"
    fi
    printf '  ok: accepted, submission %s\n' "$identifier"
}

notarize_app()
{
    step "Notarizing the app"
    local archive="$WORK/app-for-notarization.zip"
    run "zip the signed app" ditto -c -k --keepParent "$STAGED_APP" "$archive"
    notarize_file "$archive" "app"

    step "Stapling and assessing the app"
    run "staple the app ticket" xcrun stapler staple "$STAGED_APP"
    run "validate the stapled ticket" xcrun stapler validate "$STAGED_APP"
    assess_app "$STAGED_APP"
}

assess_app()
{
    local assessment
    assessment=$(spctl --assess --type execute --verbose=4 "$1" 2>&1) \
        || { printf '%s\n' "$assessment" >&2; fail "Gatekeeper rejected $1"; }
    printf '%s\n' "$assessment" | grep -q 'source=Notarized Developer ID' \
        || { printf '%s\n' "$assessment" >&2; fail "Gatekeeper accepted $1 but not as notarized"; }
    printf '  ok: Gatekeeper accepts it as Notarized Developer ID\n'
}

build_dmg()
{
    step "Building the DMG"
    local root="$WORK/dmg-root" name
    mkdir "$root"
    run "copy the stapled app" ditto --norsrc --noextattr --noqtn "$STAGED_APP" "$root/$(basename "$STAGED_APP")"
    ln -s /Applications "$root/Applications"
    # The FLAC, Ogg, libpng and font licences all require their notices to
    # travel with binaries, and a DMG is how those binaries travel.
    for name in $NOTICE_FILES; do
        cp "$REPO_ROOT/$name" "$root/$name"
    done
    STAGED_DMG="$WORK/$DMG_NAME"
    run "create the image" hdiutil create -volname "$VOLUME_NAME" -srcfolder "$root" \
        -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$STAGED_DMG"
    run "sign the DMG" codesign --force --sign "$IDENTITY" --timestamp "$STAGED_DMG"
    run "verify the DMG signature" codesign --verify --strict --verbose=2 "$STAGED_DMG"

    step "Notarizing the DMG"
    notarize_file "$STAGED_DMG" "dmg"

    step "Stapling and assessing the DMG"
    run "staple the DMG ticket" xcrun stapler staple "$STAGED_DMG"
    run "validate the DMG ticket" xcrun stapler validate "$STAGED_DMG"
    run "Gatekeeper accepts the DMG" spctl --assess --type open \
        --context context:primary-signature --verbose=4 "$STAGED_DMG"
    run "image checksum" hdiutil verify "$STAGED_DMG"

    MOUNT_POINT="$WORK/mounted"
    mkdir "$MOUNT_POINT"
    run "mount read-only" hdiutil attach "$STAGED_DMG" -readonly -nobrowse -noverify -mountpoint "$MOUNT_POINT"
    local mounted_app="$MOUNT_POINT/$(basename "$STAGED_APP")"
    run "app inside the DMG verifies" codesign --verify --deep --strict --verbose=2 "$mounted_app"
    assess_app "$mounted_app"
    for name in $NOTICE_FILES; do
        [ -f "$MOUNT_POINT/$name" ] || fail "$name did not reach the DMG"
    done
    run "unmount" hdiutil detach "$MOUNT_POINT" -quiet
    MOUNT_POINT=""
}

publish_dmg()
{
    step "Publishing"
    mkdir -p "$OUTPUT"
    cp "$STAGED_DMG" "$OUTPUT/$DMG_NAME"
    (cd "$OUTPUT" && shasum -a 256 "$DMG_NAME" > "$DMG_NAME.sha256")
    (cd "$OUTPUT" && shasum -a 256 -c "$DMG_NAME.sha256" >/dev/null) || fail "checksum of the published DMG does not match"
    printf '\nDone: %s/%s\n' "$OUTPUT" "$DMG_NAME"
    cat "$OUTPUT/$DMG_NAME.sha256"
}

publish_signed_app()
{
    step "Publishing the signed app"
    mkdir -p "$OUTPUT"
    local result="$OUTPUT/$(basename "$STAGED_APP")"
    rm -rf "$result"
    run "copy the signed app" ditto --norsrc --noextattr --noqtn "$STAGED_APP" "$result"
    run "the copy verifies" codesign --verify --deep --strict --verbose=2 "$result"
    printf '\nDone: %s\n' "$result"
}

main()
{
    parse_arguments "$@"
    validate_inputs

    if [ "$MODE" = "check" ]; then
        if [ "$PROBLEMS" -ne 0 ]; then
            printf '\nnotarize: --check found %d problem(s); nothing was signed or submitted\n' "$PROBLEMS" >&2
            exit 1
        fi
        printf '\nnotarize: --check passed; nothing was signed or submitted\n'
        exit 0
    fi

    prepare_stage
    sign_bundle
    verify_signatures

    if [ "$MODE" = "sign-only" ]; then
        publish_signed_app
        exit 0
    fi

    notarize_app
    build_dmg
    publish_dmg
}

main "$@"
