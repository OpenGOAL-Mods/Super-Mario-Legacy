from common import (
    create_output_dir,
    download_release,
    finalize_bundle,
    get_args,
    override_binaries_and_assets,
    patch_mod_timestamp_and_version_info,
)

args = get_args("windows")
print(args)
create_output_dir(args, "windows")
download_release(args, "windows", is_zip=True)
override_binaries_and_assets(args, "windows")
patch_mod_timestamp_and_version_info(args, "windows")
finalize_bundle(args, "windows", is_zip=True)
