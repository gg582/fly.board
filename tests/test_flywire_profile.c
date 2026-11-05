#include "engine/flywire.h"
#include "utils/media_preview.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char path[PATH_MAX];
    assert(flywire_profile_local_path("/assets/profile/avatar.jpg", path, sizeof(path)));
    assert(strcmp(path, "public/profile/avatar.jpg") == 0);
    assert(!flywire_profile_local_path("/assets/uploads/avatar.jpg", path, sizeof(path)));
    assert(!flywire_profile_local_path("/assets/profile/../avatar.jpg", path, sizeof(path)));
    assert(!flywire_profile_local_path("/assets/profile/subdir/avatar.jpg", path, sizeof(path)));
    assert(!flywire_profile_local_path("/assets/profile/", path, sizeof(path)));
    char small[8];
    assert(!flywire_profile_local_path("/assets/profile/avatar.jpg", small, sizeof(small)));

    assert(!generate_static_asset_webp("public/profile/definitely-missing.jpg",
                                       "public/uploads/.thumbs/definitely-missing.webp", 512, 512));
    puts("flywire profile replication tests passed");
    return 0;
}
