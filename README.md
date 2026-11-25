# Wii U Server Selector

An Aroma plugin that lets you switch between Wii U server environments (for example Pretendo or Protarium) from the plugin config menu.

## Install

1. Copy `wiiuserverselector.wps` to `sd:/wiiu/environments/aroma/plugins/`.
2. Create `sd:/wiiu/servers/` on your SD card.
3. Create one folder inside `sd:/wiiu/servers/` for each server, and put that server's `.wms` and `.wps` files directly inside it.

Example layout:

```
sd:/wiiu/servers/
├── Pretendo/
│   ├── inkay-pretendo.wms
│   └── inkay-pretendo.wps
└── Protarium/
    ├── <server>.wms
    └── <server>.wps
```

## Usage

Open the plugin config menu and select a server. The plugin copies that server's `.wms` files into the Aroma modules folder and its `.wps` files into the Aroma plugins folder, then reboots the console.

The currently installed server is marked **Current**. Server folders are rescanned every time the menu opens, so you do not need to reboot after adding a new server folder.

## Troubleshooting

If a server file causes crashes, remove the old `.wms`/`.wps` files from these folders and reboot:

- `sd:/wiiu/environments/aroma/modules/`
- `sd:/wiiu/environments/aroma/plugins/`

## Build

Requires [Docker](https://www.docker.com/).

```sh
docker build -t wiiu_builder .
docker run --rm -v "${PWD}:/project" wiiu_builder make
```

The built plugin is written to `wiiuserverselector.wps` in the repository root.

## Credits

Based on the evWii plugin.
