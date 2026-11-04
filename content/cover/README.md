# Cover recipes

The original `ivy-*` and `moss-*` recipes retain their defaults and geometry. The
[surface-colonization gallery](../test-scenes/surface-colonization/README.md) uses these opt-in
ecological inputs from the [cover solver](../../docs/subsystems/cover.md):

| Recipe | Host and habitat | Source budget |
|---|---|---:|
| `crevice-moss.cover.json` | Granite boulder, strong preference for measured shelter | 12,000 |
| `exposed-lichen.cover.json` | Granite boulder, siliceous crustose guild | 12,000 |
| `nurse-log.cover.json` | Decaying wood, shallow penetrable seedbed, fungi and juvenile plants | 12,000 |
| `nurse-log.deadwood.json` | The gallery's static host, independent visual decay and hollow | 20,000 |

The host and companion use identical scene transforms. Site inputs and habitat are authored,
not inferred from a material or a file suffix. Plants are representative small rosettes, not a
new species. Their optional pack role is `cover/plant`, a whole-leaf cutout; without a pack they
draw the kit's flat geometric cards. All built containers and captures belong outside the repo.
