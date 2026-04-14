from common import (
    create_output_dir,
    download_release,
    finalize_bundle,
    get_args,
    override_binaries_and_assets,
    patch_mod_timestamp_and_version_info,
)

args = get_args("macos-intel")
print(args)
create_output_dir(args, "macos-intel")
download_release(args, "macos-intel", is_zip=False)
override_binaries_and_assets(args, "macos-intel")
patch_mod_timestamp_and_version_info(args, "macos-intel")
finalize_bundle(args, "macos-intel", is_zip=False)
