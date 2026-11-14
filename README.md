# DAHPadRouter: building it on GitHub (no installs needed)

The steps below take about 15 minutes of clicking, then a wait while GitHub builds.

## 1. Link Epic Games to GitHub
1. Sign in at https://www.epicgames.com, then click your name (top right) and choose **Account**.
2. Open **Apps and Accounts**, find **GitHub** and click **Connect**. Approve it on GitHub.
3. GitHub emails you an invite to the **EpicGames** organization. Accept it, or open
   https://github.com/orgs/EpicGames/invitation while signed in.
4. Check: https://github.com/EpicGames/UnrealEngine should open. A 404 means the invite isn't
   accepted yet. It can take a few minutes up to an hour to show up.

## 2. Create the repository
1. On GitHub, click **+** (top right), then **New repository**.
2. Name it `DAH-PadRouter`, choose **Private**, and click **Create repository**.

## 3. Upload the project
1. Unzip `DAH-PadRouter.zip` on your PC.
2. On the new repo page, click **uploading an existing file**.
3. Open the unzipped `DAH-PadRouter` folder and select **everything inside it**, including the
   `.github` folder. Windows hides folders starting with a dot: in File Explorer use
   View → Show → Hidden items. Drag it all into the browser.
4. Check that GitHub lists `.github/workflows/build.yml`, `xmake.lua`, `README.md` and the
   `DAHPadRouter/` files, then click **Commit changes**.

## 4. Give the build access to UE4SS's source
1. Click your profile picture, then **Settings** → **Developer settings** (bottom left) →
   **Personal access tokens** → **Tokens (classic)** → **Generate new token (classic)**.
2. Note: `DAH build`. Expiration: 90 days. Tick the **repo** box. Click **Generate token**.
3. Copy the token (starts with `ghp_`). You only see it once.
4. In your `DAH-PadRouter` repo, go to **Settings** → **Secrets and variables** → **Actions** →
   **New repository secret**. Name: `UE4SS_GIT_TOKEN`. Secret: paste the token. Click **Add secret**.

Never paste the token anywhere else, including into chats.

## 5. Build
1. In the repo, open the **Actions** tab. If asked, click to enable workflows.
2. Click **Build DAHPadRouter** → **Run workflow** → **Run workflow**.
3. Wait (20–40 minutes the first time). Green tick = done. Red X = click into it, open the failed
   step, and copy the last ~40 lines of its log.
4. Open the finished run and download the **DAHPadRouter** artifact at the bottom. It's a zip
   containing `DAHPadRouter\dlls\main.dll` and `config.txt`.

Uploading a changed file later starts a new build automatically.

## 6. Install
1. Copy the `DAHPadRouter` folder from the artifact into
   `...\Destroy All Humans!\DH\Binaries\Win64\ue4ss\Mods\`
2. Open `Mods\mods.txt` and add a line: `DAHPadRouter : 1`

## 7. First test (find the two numbers)
1. Start the game with both pads plugged in, load a save, and press Ctrl+Y (DAH2P) to spawn Player 2.
2. Pad 2 should now do **nothing**, so it no longer moves Player 1. That's expected for now.
3. Press **Ctrl+Shift+P**, then watch the UE4SS console (or `ue4ss\UE4SS.log`):
   - for 6 s, touch nothing
   - for 6 s, tap **A** repeatedly on **Player 1's** pad
   - for 6 s, spin **Player 1's left stick**
4. The log prints `PROBE suggestion: InputKeySlot=.. InputAxisSlot=..`. Put those two numbers into
   `config.txt`, then restart the game.
5. Spawn Player 2 again. Pad 2 should now control Player 2.

Ctrl+Shift+O switches the mod off/on at any time. If something goes wrong, send `UE4SS.log`.
