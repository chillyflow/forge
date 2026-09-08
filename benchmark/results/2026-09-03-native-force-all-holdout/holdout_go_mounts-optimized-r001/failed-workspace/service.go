package holdout

import (
	"path"
	"strings"
)

func ResolveMount(request string, mounts []string) (string, string, bool) {
	request = path.Clean("/" + request)
	best := ""
	for _, raw := range mounts {
		mount := path.Clean("/" + raw)
		if mount == "/" {
			// Root mount matches everything
			if len(mount) > len(best) {
				best = mount
			}
		} else if strings.HasPrefix(request, mount+"/") {
			// Mount matches as a prefix with a trailing slash
			if len(mount) > len(best) {
				best = mount
			}
		} else if strings.HasPrefix(request, mount) && len(request) > len(mount) {
			// Mount matches as a prefix but not with trailing slash, and request is longer
			if len(mount) > len(best) {
				best = mount
			}
		}
	}
	if best == "" {
		return "", "", false
	}
	return best, strings.TrimPrefix(strings.TrimPrefix(request, best), "/"), true
}
