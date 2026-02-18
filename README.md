# Artic Base Server
<p align="center">
<img width="180" height="180" src="https://github.com/PabloMK7/ArticBaseServer/blob/main/images/logo.png?raw=true">
</p>

**NOTE: Artic Base Server was released very recently and may have unexpected bugs. Please back up the save file of the game you want to broadcast just in case!**

Artic Base is a tool for broadcasting games from your 3DS to a compatible emulator. This allows playing your physical or digital games on an emulator without having to dump them. You can find a demo [here](https://www.youtube.com/watch?v=noDz-FnOd2M).

## Description
Artic Base Server is a Luma3DS 3GX plugin that allows using the Artic Base protocol to broadcast your games to a compatible emulator (for example: [Azahar](https://github.com/azahar-emu/azahar)). This works by forwarding the filesystem requests from the emulator to the console.

### Features
- Play games from your console without having to dump them, with purchased updates and DLC.
- Sync the savedata/extdata of the broadcasted game during the play session.
- Use the console as the input device by enabling the Artic Controller feature on the emulator.
- Load shared ext data and NCCH archives from your console.
- Remove the need to dump AES keys, as the decryption is done by the console's OS.

### Advantages over playing on real console
- Play your games at higher resolutions.
- Use external controllers that may better fit your playstyle.
- Switch between playing on your PC at home and your console on the go.
- Reduce e-waste by reusing partially broken consoles to broadcast your games to your PC.
- Allow museums or non-profit game preservation organizations to have a centralised database of preserved games, while using an Artic Base Server + a compatible emulator to do research as needed.

### Disadvantages over game dumps
- Slower loading times, ranging from 2x to 5x depending on how much data is needed by the game. Thanks to the use of a cache in memory, loading times get better if game resources that were already loaded are requested again.
- Constant and stable connection is needed with the console running the server so that it's able to provide the game resources on demand.

## Usage instructions
1) Download the `.cia` or `.3dsx` file from the [releases page](https://github.com/PabloMK7/ArticBaseServer/releases) and install it.
2) Run the Artic Base Server application. Press A to confirm you want to start Artic Base.
3) Select the application you want to broadcast from the Home Menu.
4) On a compatible emulator, select the Artic Base option and enter the IP address displayed on your console.

NOTE: A recent version of Luma3DS (v13.1.1 or newer) is required to use Artic Base Server. You can get it [here](https://github.com/LumaTeam/Luma3DS/releases/latest).

## Future Plans
This section lists features that Artic Base Server cannot currently provide. Some of these features may be added in the future.

### Things that might be implemented
- Broadcasting homebrew applications

### Things that will never be implemented
- Dump games, and any of its assets to persistent storage. This includes caching data between play sessions.
- Dump any kind of system data, such as keys, archives or console unique files. This includes caching data between play sessions.
- Using the Home Menu of the console that is running Artic Base Server and switching between games.

## Credits
Artic Base uses icons from [flaticon](https://www.flaticon.com):
- <a href="https://www.flaticon.com/free-icons/igloo" title="igloo icons">Igloo icons created by smashingstocks - Flaticon</a>
- <a href="https://www.flaticon.com/free-icons/parabolic-dishes" title="parabolic dishes icons">Parabolic dishes icons created by Souayang - Flaticon</a>

Artic Base uses text renders from [cooltext](https://cooltext.com/).
