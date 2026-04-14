from common import (
    create_output_dir,
    download_release,
    finalize_bundle,
    get_args,
    override_binaries_and_assets,
    patch_mod_timestamp_and_version_info,
)

args = get_args("linux")
print(args)
create_output_dir(args, "linux")
download_release(args, "linux", is_zip=False)
override_binaries_and_assets(args, "linux")
patch_mod_timestamp_and_version_info(args, "linux")
finalize_bundle(args, "linux", is_zip=False)
