#!/usr/bin/env nu

def main [
    --scene: string
    --game
    --smoke
    --asset-root: path
] {
    let assets = if $asset_root != null { $asset_root | path expand } else { null }
    if $game and $assets != null {
        error make {msg: "Use --asset-root with the editor."}
    }
    mut args = if $game {
        [run --target windows --scene ($scene | default room)]
    } else {
        [editor --scene ($scene | default entities)]
    }
    if $smoke { $args = ($args | append '--smoke') }
    if $assets != null { $args = ($args | append ['--asset-root' $assets]) }
    cd $env.FILE_PWD
    ^cargo xtask ...$args
    exit $env.LAST_EXIT_CODE
}
